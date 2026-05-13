#pragma once
//======================================================================================================
// SHARDED SNAPSHOT PERSISTENCE
//======================================================================================================
// Phase 4 of the sharded completion plan. Sharded engine had no persistent
// state — every restart lost: per-core regime hysteresis, pnl_feeder,
// realized P&L counters, kill switch peak/trip state, position bitmap.
//
// The legacy PortfolioController had its own snapshot (CONTROLLER_SNAPSHOT_VERSION
// 11) but sharded never adopted it. Different magic so we can refuse legacy
// files cleanly: 0x53484430 ("SHD0" little-endian).
//
// No backward compat with legacy: refuse PORTFOLIO_SNAPSHOT_MAGIC files.
// Cfg starts fresh on first sharded run; subsequent runs round-trip through
// this module.
//
// Save model:
//   - Atomic rename: write to .tmp, fsync, rename. Mid-save crash leaves
//     the previous good file intact.
//   - Save on shutdown signal (forced final).
//   - Periodic save every N slow-path cycles (configurable via cfg).
//
// On load:
//   - Refuse if magic doesn't match (legacy v11, corruption, wrong file).
//   - Refuse if version != current (no migration logic — Phase 4 is v1).
//   - Refuse if num_cores stored != num_cores configured (cfg changed
//     post-snapshot; safer to start fresh than guess at slot mapping).
//   - On any error, log + start fresh — never crash the engine.
//======================================================================================================
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cerrno>       // errno
#include <chrono>
#include <unistd.h>     // fsync
#include "ControllerEventLoop.hpp"
#include "OrderManager.hpp"
#include "Portfolio.hpp"
#include "../FixedPoint/FixedPointN.hpp"
#include "../MemHeaders/OmsPersistFieldRegistry.hpp"  // v5.15.5.C.2 (S3a-W) — FOREACH_OMS_PERSIST_FIELD

namespace tt {

// 0x53484430 = "SHD0" little-endian. Distinct from PORTFOLIO_SNAPSHOT_MAGIC
// (0x4B434954 "TICK") so a legacy v11 file produces a clean refuse-load
// rather than parsing as garbage.
#define SHARDED_SNAPSHOT_MAGIC    0x53484430u
// v1: initial layout (Phase 4)
// v2: + per-core RollingIC + RollingRMSE buffer contents (Phase 4.1).
//     Without this, ML cores cold-start to ~zero confidence on every
//     restart and stay armed-but-inactive until live fills repopulate.
// v3: + partial_exit_enabled byte in header + per-core leg-B
//     ExecutionCore mirrors (active_b, entry_price_b, live_tp_b,
//     live_sl_b). Closes the bug where toggling partials between
//     sessions reinterpreted single-position snapshot slots under the
//     paired-leg geometry. v2 files refused on load (no migration —
//     paired-leg state can't be reconstructed from single-leg).
// v4 (v5.4.0 Phase 1.1): CoreContext gained `void* strategy_state`
//     pointer + `uint8_t strategy_state_kind`. Persistence policy:
//     kind-only is serialized (4 bytes/core); the void* is NOT
//     serialized (pointers don't survive across processes). On load,
//     Strategy_InitPerCore reallocates state from scratch matching the
//     persisted kind. Treated as session-only — strategy adapted
//     parameters reconverge within a few slow-path cadences. Full
//     state persistence deferred to v5.5.0.
//     v3 files rejected on load with a version-mismatch error.
// v5: adds core_gross_wins / core_gross_losses (added in v4.7.25 but
// silently skipped from persistence — Stats panel avg_win / avg_loss /
// profit_factor read zero after restart). Plus idle_cycles for
// death-spiral state continuity.
// v6 (recurring-bugs Class 6): adds OMS counters total_fees,
// total_maker_fees, total_taker_fees, maker_fills_count,
// taker_fills_count — were never persisted, so session forensics
// reset on every restart even though balance + realized_pnl
// continued. v5 files rejected on load with version-mismatch.
//
// v7 (v5.11.65): Position gains uint64_t entry_timestamp_us field
// (wall-clock microseconds since epoch). Survives restart so trade
// history can compute accurate hold time across engine restarts.
// Replaces the tick-derived hold (which got reset by snapshot-drift
// guard at restart). Also feeds future ML-training "optimal exit
// timing" features. v6 files rejected on load with version-mismatch.
#define SHARDED_SNAPSHOT_VERSION  7u

//======================================================================================================
// [SAVE]
//======================================================================================================
// Returns 1 on success, 0 on any I/O failure. On failure the previous good
// file (if any) is unchanged — atomic rename never moves the .tmp into
// place if writes failed.
//======================================================================================================
// `partial_exit_enabled` is the engine's current cfg.partial_exit_enabled
// (1 if partials are on, 0 otherwise). Persisted in the header so the
// loader can refuse a snapshot taken under a different toggle state —
// position slot indices have different meaning between the two
// geometries (1:1 vs paired legs A+B).
template <unsigned F>
inline int ShardedSnapshot_Save(const EventLoopState<F>* state,
                                  const char* filepath,
                                  int partial_exit_enabled) {
    if (!state || !state->oms || !filepath) return 0;

    // Write to .tmp first; rename only on full success.
    char tmppath[512];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp", filepath);
    FILE* f = fopen(tmppath, "wb");
    if (!f) {
        fprintf(stderr, "[snapshot] open %s for write failed\n", tmppath);
        return 0;
    }

    uint32_t magic   = SHARDED_SNAPSHOT_MAGIC;
    uint32_t version = SHARDED_SNAPSHOT_VERSION;
    uint32_t num_cores = (uint32_t)state->registered_count;
    uint64_t timestamp_us = (uint64_t)
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    // ---- HEADER ----
    if (fwrite(&magic, 4, 1, f) != 1)         goto fail;
    if (fwrite(&version, 4, 1, f) != 1)       goto fail;
    if (fwrite(&num_cores, 4, 1, f) != 1)     goto fail;
    if (fwrite(&timestamp_us, 8, 1, f) != 1)  goto fail;
    // v3: partials toggle byte + 3 bytes pad (keeps subsequent block
    // 4-byte aligned). Loader refuses a file whose toggle state differs
    // from current cfg, since position slot indices have different
    // meaning between the two geometries.
    {
        uint8_t partials_byte = partial_exit_enabled ? 1 : 0;
        if (fwrite(&partials_byte, 1, 1, f) != 1) goto fail;
        uint8_t pad[3] = {0, 0, 0};
        if (fwrite(pad, 3, 1, f) != 1) goto fail;
    }

    // ---- GLOBAL OMS BLOCK ----
    // v5.15.5.C.2 (S3a-W) — registry-driven save. Class 18 mirror close
    // (CLAUDE.md item 19); adding a new persisted OMS field = 1 row in
    // FOREACH_OMS_PERSIST_FIELD. Wire format byte-preserved (CLAUDE.md
    // item 15). v6 OMS counters covered automatically by registry order.
    // kill_switch_tripped bit-extracted from oms_state_flags (S3a) at save.
    FOREACH_OMS_PERSIST_FIELD(OMS_PERSIST_DO_SAVE)

    // Portfolio bitmap + 16 positions (full Portfolio struct snapshot).
    if (fwrite(&state->oms->portfolio.active_bitmap, 2, 1, f) != 1) goto fail;
    {
        uint16_t pad = 0;
        if (fwrite(&pad, 2, 1, f) != 1) goto fail;
    }
    if (fwrite(state->oms->portfolio.positions, sizeof(Position<F>), 16, f) != 16) goto fail;

    // ---- PER-CORE BLOCKS ----
    for (uint32_t i = 0; i < num_cores; ++i) {
        const CoreContext<F>& ctx = state->cores[i];

        // Identity / sizing
        if (fwrite(&ctx.strategy_id,         1, 1, f) != 1) goto fail;
        if (fwrite(&ctx.resolved_strategy_id,1, 1, f) != 1) goto fail;
        // v5.4.0 (snapshot v4): persist strategy_state_kind so the load
        // path can call Strategy_InitPerCore with the right kind to
        // reallocate state. The void* strategy_state pointer itself is
        // NOT persisted (pointers don't survive restart).
        if (fwrite(&ctx.strategy_state_kind, 1, 1, f) != 1) goto fail;
        {
            uint8_t pad8 = 0;  // was uint16_t pad16 in v3; one byte stolen for state_kind
            if (fwrite(&pad8, 1, 1, f) != 1) goto fail;
        }
        if (fwrite(&ctx.allocated_balance,   sizeof(FPN<F>), 1, f) != 1) goto fail;

        // Counters
        if (fwrite(&ctx.entries_processed,   8, 1, f) != 1) goto fail;
        if (fwrite(&ctx.exits_processed,     8, 1, f) != 1) goto fail;
        if (fwrite(&ctx.core_realized,       sizeof(FPN<F>), 1, f) != 1) goto fail;
        if (fwrite(&ctx.core_fees,           sizeof(FPN<F>), 1, f) != 1) goto fail;
        if (fwrite(&ctx.core_open_notional,  sizeof(FPN<F>), 1, f) != 1) goto fail;
        if (fwrite(&ctx.core_wins,           4, 1, f) != 1) goto fail;
        if (fwrite(&ctx.core_losses,         4, 1, f) != 1) goto fail;
        // v5.4.3 (recurring-bugs Class 4): core_gross_wins/losses were
        // added in v4.7.25 but never persisted, so Stats panel avg_win,
        // avg_loss, profit_factor, expectancy all read $0.00 after
        // restart until the next post-restart trade. idle_cycles is
        // the death-spiral counter — also persisted for continuity.
        if (fwrite(&ctx.core_gross_wins,     sizeof(FPN<F>), 1, f) != 1) goto fail;
        if (fwrite(&ctx.core_gross_losses,   sizeof(FPN<F>), 1, f) != 1) goto fail;
        if (fwrite(&ctx.idle_cycles,         4, 1, f) != 1) goto fail;

        // Spacing state
        if (fwrite(&ctx.last_entry_price,    sizeof(FPN<F>), 1, f) != 1) goto fail;
        if (fwrite(&ctx.last_entry_tick,     8, 1, f) != 1) goto fail;
        if (fwrite(&ctx.sl_cooldown_remaining, 4, 1, f) != 1) goto fail;

        // Kill switch
        if (fwrite(&ctx.core_peak_balance,   sizeof(FPN<F>), 1, f) != 1) goto fail;
        if (fwrite(&ctx.core_dd_pct,         sizeof(FPN<F>), 1, f) != 1) goto fail;
        // v5.15.5.B.3 — core_kill_tripped migrated to core_state_flags bitmap
        // bit. Format preservation: write as 1-byte 0/1 the same way pre-.B.3
        // saved the uint8_t field. Bytewise-identical wire format (no
        // SHARDED_SNAPSHOT_VERSION bump needed).
        {
            uint8_t kill_byte =
                CORE_STATE_FLAG_IS_SET(ctx, KILL_TRIPPED) ? (uint8_t)1 : (uint8_t)0;
            if (fwrite(&kill_byte, 1, 1, f) != 1) goto fail;
        }
        {
            uint8_t pad8[3] = {0,0,0};
            if (fwrite(pad8, 3, 1, f) != 1) goto fail;
        }
        if (fwrite(&ctx.core_ks_trips_total, 4, 1, f) != 1) goto fail;

        // Regime hysteresis (RegimeState — 4 ints + 1 uint64 + 1 time_t)
        if (fwrite(&ctx.regime_state.current_regime,        sizeof(int), 1, f) != 1) goto fail;
        if (fwrite(&ctx.regime_state.proposed_regime,       sizeof(int), 1, f) != 1) goto fail;
        if (fwrite(&ctx.regime_state.hysteresis_count,      sizeof(int), 1, f) != 1) goto fail;
        if (fwrite(&ctx.regime_state.hysteresis_threshold,  sizeof(int), 1, f) != 1) goto fail;
        if (fwrite(&ctx.regime_state.last_strategy_id,      sizeof(int), 1, f) != 1) goto fail;
        if (fwrite(&ctx.regime_state.regime_start_tick,     sizeof(uint64_t), 1, f) != 1) goto fail;
        if (fwrite(&ctx.regime_state.regime_start_time,     sizeof(time_t), 1, f) != 1) goto fail;

        // pnl_feeder ring buffer (size MAX_WINDOW = 8 FPN entries + 2 ints)
        if (fwrite(ctx.pnl_feeder.price_samples, sizeof(FPN<F>), MAX_WINDOW, f) != MAX_WINDOW) goto fail;
        if (fwrite(&ctx.pnl_feeder.head,  sizeof(int), 1, f) != 1) goto fail;
        if (fwrite(&ctx.pnl_feeder.count, sizeof(int), 1, f) != 1) goto fail;

        // Confidence scorer prediction snapshots (doubles)
        if (fwrite(&ctx.staged_prediction, 8, 1, f) != 1) goto fail;
        if (fwrite(&ctx.active_prediction, 8, 1, f) != 1) goto fail;
        if (fwrite(&ctx.last_confidence,   8, 1, f) != 1) goto fail;

        // v2 (Phase 4.1) — RollingIC + RollingRMSE buffer contents. The
        // `window` field is intentionally NOT saved: it comes from cfg at
        // Init, so persisting it would carry stale config across restarts
        // if the user changed confidence_window. predictions/actuals/
        // squared_errors are the actual learning signal we don't want to
        // lose on restart.
        if (fwrite(ctx.confidence.ic.predictions,
                   sizeof(double), ROLLING_IC_MAX_WINDOW, f) != ROLLING_IC_MAX_WINDOW) goto fail;
        if (fwrite(ctx.confidence.ic.actuals,
                   sizeof(double), ROLLING_IC_MAX_WINDOW, f) != ROLLING_IC_MAX_WINDOW) goto fail;
        if (fwrite(&ctx.confidence.ic.count, sizeof(int), 1, f) != 1) goto fail;
        if (fwrite(&ctx.confidence.ic.head,  sizeof(int), 1, f) != 1) goto fail;
        if (fwrite(ctx.confidence.rmse.squared_errors,
                   sizeof(double), ROLLING_IC_MAX_WINDOW, f) != ROLLING_IC_MAX_WINDOW) goto fail;
        if (fwrite(&ctx.confidence.rmse.count, sizeof(int), 1, f) != 1) goto fail;
        if (fwrite(&ctx.confidence.rmse.head,  sizeof(int), 1, f) != 1) goto fail;
    }
    // v3 NOTE: ExecutionCore leg-B mirrors (active_b, entry_price_b,
    // live_tp_b, live_sl_b) are NOT persisted as separate fields —
    // they're derived at load time from portfolio.positions[2c+1],
    // which is already persisted in the global OMS block. Same source
    // of truth as leg A (positions[2c]). One less field set to keep
    // in sync.

    // Flush + fsync so the rename is meaningful.
    if (fflush(f) != 0) goto fail;
    if (fsync(fileno(f)) != 0) {
        // fsync failure isn't catastrophic — log and continue. Some
        // filesystems (tmpfs) don't support it.
        fprintf(stderr, "[snapshot] fsync warning (continuing): %s\n", strerror(errno));
    }
    fclose(f);

    // Atomic rename: previous good file replaced only if write succeeded.
    if (rename(tmppath, filepath) != 0) {
        fprintf(stderr, "[snapshot] rename %s → %s failed: %s\n",
                tmppath, filepath, strerror(errno));
        unlink(tmppath);  // best effort cleanup
        return 0;
    }
    return 1;

fail:
    fprintf(stderr, "[snapshot] write to %s failed mid-block — aborting save\n", tmppath);
    fclose(f);
    unlink(tmppath);  // remove partial file
    return 0;
}

//======================================================================================================
// [LOAD]
//======================================================================================================
// Returns 1 on successful load (state populated from file), 0 on any
// validation failure (bad magic, version, core count). On failure the
// state is left untouched — caller continues with fresh init.
//
// Safety: explicit checks on magic + version + core count BEFORE any
// state mutation. The first per-core read failure aborts the whole load
// and rolls back to the in-memory snapshot taken at function entry.
//======================================================================================================
// `partial_exit_enabled` is the engine's CURRENT cfg.partial_exit_enabled
// (1 if partials are on, 0 otherwise). Loader refuses files written under
// a different toggle state — slot indices have different meaning between
// the two geometries (1:1 vs paired legs A+B), so reusing positions
// across the toggle creates "zombie" entries that look real in the GUI
// panel but can't be exited by the hot path.
template <unsigned F>
inline int ShardedSnapshot_Load(EventLoopState<F>* state, const char* filepath,
                                  int partial_exit_enabled,
                                  const ControllerConfig<F>* cfg = nullptr) {
    if (!state || !state->oms || !filepath) return 0;
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        // Missing file is not an error — it's the first run.
        return 0;
    }

    uint32_t magic = 0, version = 0, file_num_cores = 0;
    uint64_t timestamp_us = 0;
    if (fread(&magic, 4, 1, f) != 1) {
        fprintf(stderr, "[snapshot] %s truncated at magic — refusing load\n", filepath);
        fclose(f); return 0;
    }
    // Refuse legacy v11 PortfolioController snapshots cleanly.
    if (magic == 0x4B434954u) {  // PORTFOLIO_SNAPSHOT_MAGIC
        fprintf(stderr, "[snapshot] %s is a LEGACY snapshot (PortfolioController v11). "
                        "Sharded does not migrate legacy snapshots — starting fresh.\n",
                filepath);
        fclose(f); return 0;
    }
    if (magic != SHARDED_SNAPSHOT_MAGIC) {
        fprintf(stderr, "[snapshot] %s magic mismatch (got 0x%08x, expected 0x%08x) — "
                        "refusing load, starting fresh\n",
                filepath, magic, SHARDED_SNAPSHOT_MAGIC);
        fclose(f); return 0;
    }
    if (fread(&version, 4, 1, f) != 1 || version != SHARDED_SNAPSHOT_VERSION) {
        fprintf(stderr, "[snapshot] %s version %u != current %u — refusing load\n",
                filepath, version, SHARDED_SNAPSHOT_VERSION);
        fclose(f); return 0;
    }
    if (fread(&file_num_cores, 4, 1, f) != 1) {
        fclose(f); return 0;
    }
    if ((int)file_num_cores != state->registered_count) {
        fprintf(stderr, "[snapshot] %s has %u cores, current cfg has %d — "
                        "refusing load, starting fresh (cfg likely changed)\n",
                filepath, file_num_cores, state->registered_count);
        fclose(f); return 0;
    }
    if (fread(&timestamp_us, 8, 1, f) != 1) { fclose(f); return 0; }

    // v3: partials toggle byte + 3 bytes pad. Refuse if the saved toggle
    // state differs from the current cfg — slot-index geometry changed,
    // restored positions would be zombies. (User toggled partial_exit_enabled
    // since the file was written.)
    {
        uint8_t file_partials_byte = 0;
        uint8_t pad[3] = {0, 0, 0};
        if (fread(&file_partials_byte, 1, 1, f) != 1) { fclose(f); return 0; }
        if (fread(pad, 3, 1, f) != 1) { fclose(f); return 0; }
        int file_partials = file_partials_byte ? 1 : 0;
        int cfg_partials  = partial_exit_enabled ? 1 : 0;
        if (file_partials != cfg_partials) {
            fprintf(stderr,
                "[snapshot] %s written with partial_exit_enabled=%d, current cfg=%d — "
                "refusing load (slot geometry differs; restoring would create zombie "
                "positions). Starting fresh.\n",
                filepath, file_partials, cfg_partials);
            fclose(f); return 0;
        }
    }

    // ---- GLOBAL OMS BLOCK ----
    // v5.15.5.C.2 (S3a-W) — registry-driven load. tmp_<name> declared +
    // populated via FOREACH expansion; commit happens in the post-read
    // commit block below.
    FOREACH_OMS_PERSIST_FIELD(OMS_PERSIST_DECLARE_TMP)
    FOREACH_OMS_PERSIST_FIELD(OMS_PERSIST_DO_READ)
    // v6 OMS counters: read into tmps, apply at commit point below.
    // v5.15.5.C.2 (S3a-W) — v6 OMS counters (total_fees, total_maker_fees,
    // total_taker_fees, maker_fills_count, taker_fills_count) are now
    // declared + read by the FOREACH_OMS_PERSIST_FIELD expansion above.

    uint16_t bitmap = 0, pad16 = 0;
    Position<F> positions[16];
    if (fread(&bitmap, 2, 1, f) != 1) { fclose(f); return 0; }
    if (fread(&pad16,  2, 1, f) != 1) { fclose(f); return 0; }
    if (fread(positions, sizeof(Position<F>), 16, f) != 16) { fclose(f); return 0; }

    // ---- PER-CORE BLOCKS ----
    // Read all cores into temporary storage first; only commit if all reads succeed.
    struct CoreSnap {
        uint8_t  strategy_id;
        uint8_t  resolved_strategy_id;
        uint8_t  strategy_state_kind;  // v5.4.0 — used by load to dispatch Strategy_InitPerCore
        FPN<F>   allocated_balance;
        uint64_t entries_processed;
        uint64_t exits_processed;
        FPN<F>   core_realized, core_fees, core_open_notional;
        uint32_t core_wins, core_losses;
        // v5.4.3 (snapshot v5): gross accumulators + idle counter
        FPN<F>   core_gross_wins, core_gross_losses;
        uint32_t idle_cycles;
        FPN<F>   last_entry_price;
        uint64_t last_entry_tick;
        uint32_t sl_cooldown_remaining;
        FPN<F>   core_peak_balance, core_dd_pct;
        uint8_t  core_kill_tripped;
        uint32_t core_ks_trips_total;
        // regime
        int      rs_current, rs_proposed, rs_count, rs_threshold, rs_last_strat;
        uint64_t rs_start_tick;
        time_t   rs_start_time;
        // feeder
        FPN<F>   feeder_samples[MAX_WINDOW];
        int      feeder_head, feeder_count;
        // confidence (v1)
        double   staged, active, last_confidence;
        // v2: rolling buffer contents
        double   ic_predictions[ROLLING_IC_MAX_WINDOW];
        double   ic_actuals[ROLLING_IC_MAX_WINDOW];
        int      ic_count, ic_head;
        double   rmse_squared_errors[ROLLING_IC_MAX_WINDOW];
        int      rmse_count, rmse_head;
    };
    CoreSnap snaps[MAX_EXECUTION_CORES];

    for (uint32_t i = 0; i < file_num_cores; ++i) {
        CoreSnap& s = snaps[i];
        // v5.4.0: pad16_2 (2-byte pad after resolved_strategy_id) was
        // replaced by strategy_state_kind (1 byte) + 1-byte pad.
        uint8_t  pad8[3];
        if (fread(&s.strategy_id, 1, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.resolved_strategy_id, 1, 1, f) != 1) { fclose(f); return 0; }
        // v5.4.0 (snapshot v4): read strategy_state_kind. Used by load to
        // call Strategy_InitPerCore(kind) — see end of load function.
        if (fread(&s.strategy_state_kind, 1, 1, f) != 1) { fclose(f); return 0; }
        {
            uint8_t pad8_kind = 0;
            if (fread(&pad8_kind, 1, 1, f) != 1) { fclose(f); return 0; }
        }
        if (fread(&s.allocated_balance, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.entries_processed, 8, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.exits_processed,   8, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_realized,     sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_fees,         sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_open_notional,sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_wins,         4, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_losses,       4, 1, f) != 1) { fclose(f); return 0; }
        // v5.4.3 (snapshot v5): gross accumulators + idle_cycles.
        if (fread(&s.core_gross_wins,   sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_gross_losses, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.idle_cycles,       4, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.last_entry_price,  sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.last_entry_tick,   8, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.sl_cooldown_remaining, 4, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_peak_balance, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_dd_pct,       sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_kill_tripped, 1, 1, f) != 1) { fclose(f); return 0; }
        if (fread(pad8,                 3, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_ks_trips_total, 4, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.rs_current,    sizeof(int), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.rs_proposed,   sizeof(int), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.rs_count,      sizeof(int), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.rs_threshold,  sizeof(int), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.rs_last_strat, sizeof(int), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.rs_start_tick, sizeof(uint64_t), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.rs_start_time, sizeof(time_t), 1, f) != 1) { fclose(f); return 0; }
        if (fread(s.feeder_samples, sizeof(FPN<F>), MAX_WINDOW, f) != MAX_WINDOW) { fclose(f); return 0; }
        if (fread(&s.feeder_head,   sizeof(int), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.feeder_count,  sizeof(int), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.staged,         8, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.active,         8, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.last_confidence,8, 1, f) != 1) { fclose(f); return 0; }
        // v2 — RollingIC + RollingRMSE buffers
        if (fread(s.ic_predictions, sizeof(double), ROLLING_IC_MAX_WINDOW, f) != ROLLING_IC_MAX_WINDOW) { fclose(f); return 0; }
        if (fread(s.ic_actuals,     sizeof(double), ROLLING_IC_MAX_WINDOW, f) != ROLLING_IC_MAX_WINDOW) { fclose(f); return 0; }
        if (fread(&s.ic_count, sizeof(int), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.ic_head,  sizeof(int), 1, f) != 1) { fclose(f); return 0; }
        if (fread(s.rmse_squared_errors, sizeof(double), ROLLING_IC_MAX_WINDOW, f) != ROLLING_IC_MAX_WINDOW) { fclose(f); return 0; }
        if (fread(&s.rmse_count, sizeof(int), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.rmse_head,  sizeof(int), 1, f) != 1) { fclose(f); return 0; }
    }
    fclose(f);

    // ---- COMMIT (after all reads validated) ----
    // v5.15.5.C.2 (S3a-W) — registry-driven commit. tmp_<name> → state->oms->name
    // for DIRECT-kind fields; bit set/clear for BIT-kind kill_switch_tripped.
    FOREACH_OMS_PERSIST_FIELD(OMS_PERSIST_DO_COMMIT)
    state->oms->portfolio.active_bitmap = bitmap;
    memcpy(state->oms->portfolio.positions, positions, sizeof(positions));

    for (uint32_t i = 0; i < file_num_cores; ++i) {
        CoreSnap& s = snaps[i];
        CoreContext<F>& ctx = state->cores[i];
        // Strategy id intentionally NOT restored — it comes from cfg.
        // resolved_strategy_id is per-tick output; restore it for display
        // continuity but the next rebuild will overwrite anyway.
        ctx.resolved_strategy_id = s.resolved_strategy_id;
        // v5.4.0: persisted `strategy_state_kind` was originally intended
        // to drive Strategy_InitPerCore at boot (the comment said "engine's
        // init path checks strategy_state_kind and dispatches"). In
        // practice, EngineSharded.hpp:1183 dispatches on
        // `state.cores[i].strategy_id` (cfg-derived), NOT the loaded kind.
        // The persisted field is therefore dead weight — and worse,
        // restoring it here corrupts the invariant that `strategy_state_kind`
        // describes the C++ type of the allocated `strategy_state` pointer.
        //
        // Concrete repro of the kind/state mismatch the restore introduced:
        //
        //   Run 1 cfg: core_0_strategy=auto         → state=nullptr, kind=AUTO
        //              snapshot saves kind=AUTO.
        //   Run 2 cfg: core_0_strategy=mean_reversion
        //              Strategy_InitPerCore(MR) → state=non-null MR, kind=MR.
        //              Snapshot Load (this site) → kind = AUTO (from snap).
        //              Net: state=non-null + kind=AUTO. Mismatch.
        //
        // At shutdown Strategy_FreePerCore can't dispatch the type-correct
        // `delete` — pre-v5.11.11 it hit the `default:` branch and WARN'd
        // ("unknown kind ..."), post-v5.11.11 it quietly took the AUTO/NONE
        // branch and leaked the state pointer (~1-4 KB; arena cleanup at
        // shutdown reclaims either way).
        //
        // v5.11.15 (2026-05-07) — root-cause fix. Stop restoring kind from
        // snapshot. The persisted byte stays in the format (snapshot v4+
        // back-compat — we still READ it at line 420 above, just don't
        // APPLY it here). Future snapshot versions can drop the field
        // entirely without breaking older saves.
        //
        // The kind invariant is now: set ONLY by Strategy_InitPerCore at
        // boot (StrategyLifecycle.hpp:160) and Strategy_FreePerCore on
        // teardown (StrategyLifecycle.hpp:432). No other site mutates it.
        (void)s.strategy_state_kind;  // intentionally unused; see comment.
        ctx.allocated_balance    = s.allocated_balance;
        ctx.entries_processed    = s.entries_processed;
        ctx.exits_processed      = s.exits_processed;
        ctx.core_realized        = s.core_realized;
        ctx.core_fees            = s.core_fees;
        ctx.core_open_notional   = s.core_open_notional;
        ctx.core_wins            = s.core_wins;
        ctx.core_losses          = s.core_losses;
        // v5.4.3 (snapshot v5): apply gross accumulators + idle counter.
        ctx.core_gross_wins      = s.core_gross_wins;
        ctx.core_gross_losses    = s.core_gross_losses;
        ctx.idle_cycles          = s.idle_cycles;
        ctx.last_entry_price     = s.last_entry_price;
        ctx.last_entry_tick      = s.last_entry_tick;
        ctx.sl_cooldown_remaining= s.sl_cooldown_remaining;
        ctx.core_peak_balance    = s.core_peak_balance;
        ctx.core_dd_pct          = s.core_dd_pct;
        // v5.15.5.B.3 — kill bit packed in core_state_flags; load 1 byte
        // into a temp + set/clear bit accordingly.
        if (s.core_kill_tripped) {
            CORE_STATE_FLAG_SET(ctx, KILL_TRIPPED);
        } else {
            CORE_STATE_FLAG_CLR(ctx, KILL_TRIPPED);
        }
        ctx.core_ks_trips_total  = s.core_ks_trips_total;
        ctx.regime_state.current_regime       = s.rs_current;
        ctx.regime_state.proposed_regime      = s.rs_proposed;
        ctx.regime_state.hysteresis_count     = s.rs_count;
        ctx.regime_state.hysteresis_threshold = s.rs_threshold;
        ctx.regime_state.last_strategy_id     = s.rs_last_strat;
        ctx.regime_state.regime_start_tick    = s.rs_start_tick;
        ctx.regime_state.regime_start_time    = s.rs_start_time;
        memcpy(ctx.pnl_feeder.price_samples, s.feeder_samples, sizeof(s.feeder_samples));
        ctx.pnl_feeder.head  = s.feeder_head;
        ctx.pnl_feeder.count = s.feeder_count;
        ctx.staged_prediction = s.staged;
        ctx.active_prediction = s.active;
        ctx.last_confidence   = s.last_confidence;
        // v2 — restore rolling IC + RMSE buffer contents. Don't overwrite
        // the `window` field on either struct: it was set from current
        // cfg at ConfidenceScorer_Init and persisting it would carry
        // stale config across restarts.
        memcpy(ctx.confidence.ic.predictions, s.ic_predictions, sizeof(s.ic_predictions));
        memcpy(ctx.confidence.ic.actuals,     s.ic_actuals,     sizeof(s.ic_actuals));
        ctx.confidence.ic.count = s.ic_count;
        ctx.confidence.ic.head  = s.ic_head;
        memcpy(ctx.confidence.rmse.squared_errors, s.rmse_squared_errors, sizeof(s.rmse_squared_errors));
        ctx.confidence.rmse.count = s.rmse_count;
        ctx.confidence.rmse.head  = s.rmse_head;
    }

    // Bug fix (2026-04-27): re-activate ExecutionCore<F> hot-path state from
    // each restored Position. Snapshot persists portfolio.positions[slot]
    // (entry_price, take_profit_price, stop_loss_price, quantity) but NOT the
    // ExecutionCore's active flag / live_tp / live_sl / entry_price — those
    // are core-owned and ephemeral, written on entry by the hot path.
    //
    // Without this re-activation, hot path's exit gate evaluates:
    //   can_exit = active & sg_fires    // active=0 (init default)
    // → can_exit always 0 → SG never fires for restored positions →
    // positions become "zombie" (visible in portfolio.active_bitmap and
    // GUI Positions panel, but cannot be exited by TP/SL on the hot path).
    //
    // Symptom: after engine restart with active snapshot positions, even
    // when price drops below a position's stop_loss_price, the hot path's
    // SG_Evaluate doesn't fire because active=0. Position stays open until
    // (a) user manually exits via GUI, (b) max_hold_ticks force-closes via
    // slow path, or (c) day-boundary close.
    //
    // Fix: walk the restored active_bitmap, copy Position fields into the
    // matching ExecutionCore<F>'s hot-path mirrors, set active flag.
    //
    // v3 — partials-aware mapping. With partial_exit_enabled=1, slot 2c
    // is core c's leg A and slot 2c+1 is core c's leg B. Each leg has
    // its own mirror set on the same ExecutionCore (active vs active_b,
    // live_tp vs live_tp_b, etc.). Without this branch, leg-B slots
    // (2c+1 >= registered_count when N==MAX/2) silently fail the bound
    // check and never re-activate.
    uint16_t restored_bm = state->oms->portfolio.active_bitmap;
    int restored_count = 0;
    while (restored_bm) {
        int slot = __builtin_ctz(restored_bm);
        restored_bm &= (uint16_t)(restored_bm - 1);
        int core_id = partial_exit_enabled ? (slot >> 1) : slot;
        int leg     = partial_exit_enabled ? (slot & 1)  : 0;
        if (core_id < 0 || core_id >= (int)state->registered_count) continue;
        ExecutionCore<F>* core_ptr = state->cores[core_id].core;
        if (!core_ptr) continue;
        const Position<F>& pos = state->oms->portfolio.positions[slot];
        // Active flag last (no atomic needed — core hot-path thread
        // isn't running yet at snapshot-load time).
        // v5.5.5: recompute live_tp / live_sl from entry × (1 ± pct).
        // pre-fix copied pos.take_profit_price / pos.stop_loss_price into
        // live_tp / live_sl. Those are populated at entry from the
        // strategy's intended_tp = sg_take_profit_price. For Momentum,
        // sg_take_profit_price is stddev-based (entry + stddev × momentum_tp_mult)
        // and can be just $10-30 above entry in low-vol regimes — nothing
        // like the fresh-entry hot path's `live_tp = fill × (1 + tp_pct)`
        // which uses cfg's pct (e.g. 0.8% for core 0). User reported on
        // 2026-04-30 that restored Momentum positions sold at near-entry
        // prices for tiny gains that lost to fees. Now compute live_tp
        // the same way ExecutionCore_Tick does on a fresh entry, using
        // per-core resolved tp_pct/sl_pct from cfg.
        FPN<F> entry = pos.entry_price;
        // When cfg is provided (production caller), recompute live_tp/sl
        // from entry × (1 ± pct) so it matches the fresh-entry hot path.
        // When cfg is nullptr (legacy callers, tests), fall back to the
        // pre-fix pos.* path.
        FPN<F> live_tp_a = pos.take_profit_price;
        FPN<F> live_sl_a = pos.stop_loss_price;
        FPN<F> live_tp_b_val = pos.take_profit_price;
        if (cfg) {
            ControllerConfig<F> resolved = ControllerConfig_ResolveForCore(*cfg, core_id);
            FPN<F> tp_pct_a = resolved.take_profit_pct;
            FPN<F> sl_pct_a = resolved.stop_loss_pct;
            if (!FPN_IsZero(tp_pct_a))
                live_tp_a = FPN_Add(entry, FPN_Mul(entry, tp_pct_a));
            if (!FPN_IsZero(sl_pct_a))
                live_sl_a = FPN_Sub(entry, FPN_Mul(entry, sl_pct_a));
            FPN<F> tp_pct_b = !FPN_IsZero(resolved.tp2_mult) && !FPN_IsZero(tp_pct_a)
                ? FPN_Mul(tp_pct_a, resolved.tp2_mult)
                : tp_pct_a;
            if (!FPN_IsZero(tp_pct_b))
                live_tp_b_val = FPN_Add(entry, FPN_Mul(entry, tp_pct_b));
        }
        if (leg == 0) {
            core_ptr->entry_price = entry;
            core_ptr->live_tp     = live_tp_a;
            core_ptr->live_sl     = live_sl_a;
            core_ptr->active      = 1;
        } else {
            core_ptr->entry_price_b = entry;
            core_ptr->live_tp_b     = live_tp_b_val;
            core_ptr->live_sl_b     = live_sl_a;  // shared SL
            core_ptr->active_b      = 1;
        }
        restored_count++;
    }
    if (restored_count > 0) {
        fprintf(stderr,
            "[snapshot] re-activated %d ExecutionCore(s) from restored "
            "positions — hot-path SG/TP/SL gates armed\n",
            restored_count);
    }

    fprintf(stderr, "[snapshot] loaded sharded snapshot from %s (%u cores, ts=%llu)\n",
            filepath, file_num_cores, (unsigned long long)timestamp_us);
    return 1;
}

}  // namespace tt
