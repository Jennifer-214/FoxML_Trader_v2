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

namespace tt {

// 0x53484430 = "SHD0" little-endian. Distinct from PORTFOLIO_SNAPSHOT_MAGIC
// (0x4B434954 "TICK") so a legacy v11 file produces a clean refuse-load
// rather than parsing as garbage.
#define SHARDED_SNAPSHOT_MAGIC    0x53484430u
// v1: initial layout (Phase 4)
// v2: + per-core RollingIC + RollingRMSE buffer contents (Phase 4.1).
//     Without this, ML cores cold-start to ~zero confidence on every
//     restart and stay armed-but-inactive until live fills repopulate.
#define SHARDED_SNAPSHOT_VERSION  2u

//======================================================================================================
// [SAVE]
//======================================================================================================
// Returns 1 on success, 0 on any I/O failure. On failure the previous good
// file (if any) is unchanged — atomic rename never moves the .tmp into
// place if writes failed.
//======================================================================================================
template <unsigned F>
inline int ShardedSnapshot_Save(const EventLoopState<F>* state,
                                  const char* filepath) {
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

    // ---- GLOBAL OMS BLOCK ----
    if (fwrite(&state->oms->balance,             sizeof(FPN<F>), 1, f) != 1) goto fail;
    if (fwrite(&state->oms->realized_pnl,        sizeof(FPN<F>), 1, f) != 1) goto fail;
    if (fwrite(&state->oms->ks_peak_balance,     sizeof(FPN<F>), 1, f) != 1) goto fail;
    if (fwrite(&state->oms->kill_switch_tripped, sizeof(int),    1, f) != 1) goto fail;

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
        {
            uint16_t pad16 = 0;
            if (fwrite(&pad16, 2, 1, f) != 1) goto fail;
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

        // Spacing state
        if (fwrite(&ctx.last_entry_price,    sizeof(FPN<F>), 1, f) != 1) goto fail;
        if (fwrite(&ctx.last_entry_tick,     8, 1, f) != 1) goto fail;
        if (fwrite(&ctx.sl_cooldown_remaining, 4, 1, f) != 1) goto fail;

        // Kill switch
        if (fwrite(&ctx.core_peak_balance,   sizeof(FPN<F>), 1, f) != 1) goto fail;
        if (fwrite(&ctx.core_dd_pct,         sizeof(FPN<F>), 1, f) != 1) goto fail;
        if (fwrite(&ctx.core_kill_tripped,   1, 1, f) != 1) goto fail;
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
template <unsigned F>
inline int ShardedSnapshot_Load(EventLoopState<F>* state, const char* filepath) {
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

    // ---- GLOBAL OMS BLOCK ----
    FPN<F> tmp_balance, tmp_realized, tmp_peak;
    int tmp_kill_tripped;
    if (fread(&tmp_balance,      sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(&tmp_realized,     sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(&tmp_peak,         sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(&tmp_kill_tripped, sizeof(int),    1, f) != 1) { fclose(f); return 0; }

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
        FPN<F>   allocated_balance;
        uint64_t entries_processed;
        uint64_t exits_processed;
        FPN<F>   core_realized, core_fees, core_open_notional;
        uint32_t core_wins, core_losses;
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
        uint16_t pad16_2 = 0;
        uint8_t  pad8[3];
        if (fread(&s.strategy_id, 1, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.resolved_strategy_id, 1, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&pad16_2, 2, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.allocated_balance, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.entries_processed, 8, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.exits_processed,   8, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_realized,     sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_fees,         sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_open_notional,sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_wins,         4, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.core_losses,       4, 1, f) != 1) { fclose(f); return 0; }
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
    state->oms->balance             = tmp_balance;
    state->oms->realized_pnl        = tmp_realized;
    state->oms->ks_peak_balance     = tmp_peak;
    state->oms->kill_switch_tripped = tmp_kill_tripped;
    state->oms->portfolio.active_bitmap = bitmap;
    memcpy(state->oms->portfolio.positions, positions, sizeof(positions));

    for (uint32_t i = 0; i < file_num_cores; ++i) {
        CoreSnap& s = snaps[i];
        CoreContext<F>& ctx = state->cores[i];
        // Strategy id intentionally NOT restored — it comes from cfg.
        // resolved_strategy_id is per-tick output; restore it for display
        // continuity but the next rebuild will overwrite anyway.
        ctx.resolved_strategy_id = s.resolved_strategy_id;
        ctx.allocated_balance    = s.allocated_balance;
        ctx.entries_processed    = s.entries_processed;
        ctx.exits_processed      = s.exits_processed;
        ctx.core_realized        = s.core_realized;
        ctx.core_fees            = s.core_fees;
        ctx.core_open_notional   = s.core_open_notional;
        ctx.core_wins            = s.core_wins;
        ctx.core_losses          = s.core_losses;
        ctx.last_entry_price     = s.last_entry_price;
        ctx.last_entry_tick      = s.last_entry_tick;
        ctx.sl_cooldown_remaining= s.sl_cooldown_remaining;
        ctx.core_peak_balance    = s.core_peak_balance;
        ctx.core_dd_pct          = s.core_dd_pct;
        ctx.core_kill_tripped    = s.core_kill_tripped;
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

    fprintf(stderr, "[snapshot] loaded sharded snapshot from %s (%u cores, ts=%llu)\n",
            filepath, file_num_cores, (unsigned long long)timestamp_us);
    return 1;
}

}  // namespace tt
