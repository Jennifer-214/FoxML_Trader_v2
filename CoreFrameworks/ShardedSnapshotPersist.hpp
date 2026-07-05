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
//   - Refuse if num_nodes stored != num_nodes configured (cfg changed
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
// v5.15.5.C.3 Phase 3b — canonical FOREACH_OMS_FIELD + OMS_PROJECT_PERSIST_*
// dispatch is included transitively via OrderManager.hpp:71. The legacy
// OmsPersistFieldRegistry.hpp was deleted in this phase; consumers now walk
// the canonical 8-tuple registry via the PERSIST projection macros.

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
// v4 (v5.4.0 Phase 1.1): NodeContext gained `void* strategy_state`
//     pointer + `uint8_t strategy_state_kind`. Persistence policy:
//     kind-only is serialized (4 bytes/core); the void* is NOT
//     serialized (pointers don't survive across processes). On load,
//     Strategy_InitPerCore reallocates state from scratch matching the
//     persisted kind. Treated as session-only — strategy adapted
//     parameters reconverge within a few slow-path cadences. Full
//     state persistence deferred to v5.5.0.
//     v3 files rejected on load with a version-mismatch error.
// v5: adds node_gross_wins / node_gross_losses (added in v4.7.25 but
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
//
// v8 (v5.15.5.C.3): OMS gains uint64_t paper_session_start_us field
// (wall-clock microseconds at OrderManager_Init; reset on paper-reset).
// Used by paper-reset archive flow (Phase 6) to format the
// {start_iso}_to_{end_iso}.paper archive directory name. v7 files
// rejected on load with version-mismatch — paper-mode data only;
// operator restarts a fresh paper session. No live data is lost
// (live mode never persists via this path; reconciles from exchange).
#define SHARDED_SNAPSHOT_VERSION  10u  // Ship-B DECIMAL epoch: per-core money re-encoded; v9 (16B binary, H21 tombstone) rejected. Was: // Ship-A 16B FPN_Binary: embedded Position/FPN_Binary-struct byte layouts changed; v8 version-rejected (H21/D-144)

// Ship-B P2 epoch guard (S-4): this file raw-fwrites per-core money (allocated_balance /
// node_realized / fees / notional / pnl_feeder / 16x Position). Encoding-keyed (the 16B->16B
// decimal flip is layout-invisible): red-builds until the version rides the SAME commit.
static_assert(MONEY_ENCODING_EPOCH == 0u || SHARDED_SNAPSHOT_VERSION >= 9u + MONEY_ENCODING_EPOCH,
              "Ship-B epoch: the engine money type flipped to decimal — bump "
              "SHARDED_SNAPSHOT_VERSION to 10u (H21 tombstone 9u) in THIS commit.");

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
    uint32_t num_nodes = (uint32_t)state->registered_count;
    uint64_t timestamp_us = (uint64_t)
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    // ---- HEADER ----
    if (fwrite(&magic, 4, 1, f) != 1)         goto fail;
    if (fwrite(&version, 4, 1, f) != 1)       goto fail;
    if (fwrite(&num_nodes, 4, 1, f) != 1)     goto fail;
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
    // v5.15.5.C.3 Phase 3b — canonical FOREACH_OMS_FIELD via PERSIST projection.
    // Replaces FOREACH_OMS_PERSIST_FIELD(OMS_PERSIST_DO_SAVE) — same wire bytes,
    // single canonical source of truth. PERSIST view filters by PERSIST_KIND
    // column (SKIP_PERSIST rows emit no-op); STORAGE_KIND dispatch handles
    // DIRECT (fwrite field) vs BIT (extract bit from oms_state_flags →
    // fwrite as type bytes). Adding a new persisted OMS field = 1 row in
    // FOREACH_OMS_FIELD. Wire format byte-preserved (CLAUDE.md item 15).
    FOREACH_OMS_FIELD(OMS_PROJECT_PERSIST_SAVE)

    // Portfolio bitmap + 16 positions (full Portfolio struct snapshot).
    if (fwrite(&state->oms->portfolio.active_bitmap, 2, 1, f) != 1) goto fail;
    {
        uint16_t pad = 0;
        if (fwrite(&pad, 2, 1, f) != 1) goto fail;
    }
    if (fwrite(state->oms->portfolio.positions, sizeof(Position<F>), 16, f) != 16) goto fail;

    // ---- PER-CORE BLOCKS ----
    for (uint32_t i = 0; i < num_nodes; ++i) {
        const NodeContext<F>& ctx = state->nodes[i];

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
        if (fwrite(&ctx.allocated_balance,   sizeof(Money), 1, f) != 1) goto fail;

        // Counters
        if (fwrite(&ctx.entries_processed,   8, 1, f) != 1) goto fail;
        if (fwrite(&ctx.exits_processed,     8, 1, f) != 1) goto fail;
        if (fwrite(&ctx.node_realized,       sizeof(Money), 1, f) != 1) goto fail;
        if (fwrite(&ctx.node_fees,           sizeof(Money), 1, f) != 1) goto fail;
        if (fwrite(&ctx.node_open_notional,  sizeof(Money), 1, f) != 1) goto fail;
        if (fwrite(&ctx.node_wins,           4, 1, f) != 1) goto fail;
        if (fwrite(&ctx.node_losses,         4, 1, f) != 1) goto fail;
        // v5.4.3 (recurring-bugs Class 4): node_gross_wins/losses were
        // added in v4.7.25 but never persisted, so Stats panel avg_win,
        // avg_loss, profit_factor, expectancy all read $0.00 after
        // restart until the next post-restart trade. idle_cycles is
        // the death-spiral counter — also persisted for continuity.
        if (fwrite(&ctx.node_gross_wins,     sizeof(Money), 1, f) != 1) goto fail;
        if (fwrite(&ctx.node_gross_losses,   sizeof(Money), 1, f) != 1) goto fail;
        if (fwrite(&ctx.idle_cycles,         4, 1, f) != 1) goto fail;

        // Spacing state
        if (fwrite(&ctx.last_entry_price,    sizeof(Money), 1, f) != 1) goto fail;
        if (fwrite(&ctx.last_entry_tick,     8, 1, f) != 1) goto fail;
        if (fwrite(&ctx.sl_cooldown_remaining, 4, 1, f) != 1) goto fail;

        // Kill switch
        if (fwrite(&ctx.node_peak_balance,   sizeof(Money), 1, f) != 1) goto fail;
        if (fwrite(&ctx.node_dd_pct,         sizeof(Money), 1, f) != 1) goto fail;
        // v5.15.5.B.3 — node_kill_tripped migrated to node_state_flags bitmap
        // bit. Format preservation: write as 1-byte 0/1 the same way pre-.B.3
        // saved the uint8_t field. Bytewise-identical wire format (no
        // SHARDED_SNAPSHOT_VERSION bump needed).
        {
            uint8_t kill_byte =
                NODE_STATE_FLAG_IS_SET(ctx, KILL_TRIPPED) ? (uint8_t)1 : (uint8_t)0;
            if (fwrite(&kill_byte, 1, 1, f) != 1) goto fail;
        }
        {
            uint8_t pad8[3] = {0,0,0};
            if (fwrite(pad8, 3, 1, f) != 1) goto fail;
        }
        if (fwrite(&ctx.node_ks_trips_total, 4, 1, f) != 1) goto fail;

        // Regime hysteresis — registry-driven delegate (E.1.2 Step-2): 5 ints + uint64 + time_t,
        // byte-identical to the pre-registry hand-loop. FOREACH_REGIME_PERSIST_FIELD (RegimeDetector.hpp).
        if (RegimeState_FieldwiseWrite(&ctx.regime_state, f) != 0) goto fail;

        // pnl_feeder ring buffer — registry-driven delegate (E.1.2 Step-2): price_samples
        // [FPN_Binary<F> x MAX_WINDOW] + head + count. FOREACH_FEEDER_PERSIST_FIELD. The retype
        // Money->FPN_Binary<F> is byte-identical (both 16B) + type-honest (R1).
        if (RegressionFeederX_FieldwiseWrite(&ctx.pnl_feeder, f) != 0) goto fail;

        // Confidence scorer prediction snapshots (doubles)
        if (fwrite(&ctx.staged_prediction, 8, 1, f) != 1) goto fail;
        if (fwrite(&ctx.active_prediction, 8, 1, f) != 1) goto fail;
        if (fwrite(&ctx.last_confidence,   8, 1, f) != 1) goto fail;

        // v5.15.5.E.0 — ConfidenceScorer fieldwise persistence via shared
        // FOREACH_CONFIDENCE_PERSIST_FIELD registry. Byte-identical to pre-.E
        // sharded wire format (predictions + actuals + count + head + rmse
        // arrays + count + head). Closes Class-18 mirror with
        // PortfolioController.hpp:2117 — both persistence sites now call the
        // same helper. Adding a new persisted field = 1 row in the registry.
        // Pattern: DESIGN_SPECS/registry-tuple-as-single-source-of-truth.md +
        // DESIGN_SPECS/autopopulate-pattern-for-production-caller-class.md.
        //
        // `window` field intentionally NOT in the registry: comes from cfg at
        // Init; persisting would carry stale config across restarts if the
        // user changed confidence_window between sessions.
        if (ConfidenceScorer_FieldwiseWrite(&ctx.confidence, f) != 0) goto fail;
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

    uint32_t magic = 0, version = 0, file_num_nodes = 0;
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
    if (fread(&file_num_nodes, 4, 1, f) != 1) {
        fclose(f); return 0;
    }
    if ((int)file_num_nodes != state->registered_count) {
        fprintf(stderr, "[snapshot] %s has %u nodes, current cfg has %d — "
                        "refusing load, starting fresh (cfg likely changed)\n",
                filepath, file_num_nodes, state->registered_count);
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
    // v5.15.5.C.3 Phase 3b — canonical PERSIST projection (DECLARE_TMP + READ).
    FOREACH_OMS_FIELD(OMS_PROJECT_PERSIST_DECLARE)
    FOREACH_OMS_FIELD(OMS_PROJECT_PERSIST_READ)
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
    struct NodeSnap {
        uint8_t  strategy_id;
        uint8_t  resolved_strategy_id;
        uint8_t  strategy_state_kind;  // v5.4.0 — used by load to dispatch Strategy_InitPerCore
        Money   allocated_balance;
        uint64_t entries_processed;
        uint64_t exits_processed;
        Money   node_realized, node_fees, node_open_notional;
        uint32_t node_wins, node_losses;
        // v5.4.3 (snapshot v5): gross accumulators + idle counter
        Money   node_gross_wins, node_gross_losses;
        uint32_t idle_cycles;
        Money   last_entry_price;
        uint64_t last_entry_tick;
        uint32_t sl_cooldown_remaining;
        Money   node_peak_balance, node_dd_pct;
        uint8_t  node_kill_tripped;
        uint32_t node_ks_trips_total;
        // regime — nested staging (E.1.2 Step-2, D-305): FieldwiseRead populates the 7
        // persisted fields; the 2 unpersisted score ints stay default-init + uncommitted.
        RegimeState<F> staging_regime;
        // feeder — nested staging (E.1.2 Step-2, D-305): FieldwiseRead populates all 3 fields.
        RegressionFeederX<F> staging_feeder;
        // confidence (v1)
        double   staged, active, last_confidence;
        // v5.15.5.E.0 — staging ConfidenceScorer instance replaces the prior
        // flat staging fields (ic_predictions / ic_actuals / ic_count / etc).
        // FieldwiseRead populates only the persisted subset (ic + rmse
        // history); composite-mode fields default-init via this struct's
        // default-initialization (they're re-init from cfg via
        // ConfidenceScorer_BindCompositeCfg at boot, NOT restored from snapshot).
        // Pattern: DESIGN_SPECS/registry-tuple-as-single-source-of-truth.md.
        ConfidenceScorer staging_confidence;
    };
    NodeSnap snaps[MAX_EXECUTION_NODES];

    for (uint32_t i = 0; i < file_num_nodes; ++i) {
        NodeSnap& s = snaps[i];
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
        if (fread(&s.allocated_balance, sizeof(Money), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.entries_processed, 8, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.exits_processed,   8, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.node_realized,     sizeof(Money), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.node_fees,         sizeof(Money), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.node_open_notional,sizeof(Money), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.node_wins,         4, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.node_losses,       4, 1, f) != 1) { fclose(f); return 0; }
        // v5.4.3 (snapshot v5): gross accumulators + idle_cycles.
        if (fread(&s.node_gross_wins,   sizeof(Money), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.node_gross_losses, sizeof(Money), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.idle_cycles,       4, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.last_entry_price,  sizeof(Money), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.last_entry_tick,   8, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.sl_cooldown_remaining, 4, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.node_peak_balance, sizeof(Money), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.node_dd_pct,       sizeof(Money), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.node_kill_tripped, 1, 1, f) != 1) { fclose(f); return 0; }
        if (fread(pad8,                 3, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.node_ks_trips_total, 4, 1, f) != 1) { fclose(f); return 0; }
        if (RegimeState_FieldwiseRead(&s.staging_regime, f) != 0) { fclose(f); return 0; }
        if (RegressionFeederX_FieldwiseRead(&s.staging_feeder, f) != 0) { fclose(f); return 0; }
        if (fread(&s.staged,         8, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.active,         8, 1, f) != 1) { fclose(f); return 0; }
        if (fread(&s.last_confidence,8, 1, f) != 1) { fclose(f); return 0; }
        // v5.15.5.E.0 — ConfidenceScorer fieldwise read into staging instance.
        // FOREACH_CONFIDENCE_PERSIST_FIELD drives the read; same wire format
        // as ShardedSnapshotPersist save. Atomicity preserved by reading into
        // staging (commit happens later after all per-core reads validate).
        if (ConfidenceScorer_FieldwiseRead(&s.staging_confidence, f) != 0) { fclose(f); return 0; }
    }
    fclose(f);

    // ---- COMMIT (after all reads validated) ----
    // v5.15.5.C.2 (S3a-W) — registry-driven commit. tmp_<name> → state->oms->name
    // for DIRECT-kind fields; bit set/clear for BIT-kind kill_switch_tripped.
    // v5.15.5.C.3 Phase 3b — canonical PERSIST projection (COMMIT).
    FOREACH_OMS_FIELD(OMS_PROJECT_PERSIST_COMMIT)
    state->oms->portfolio.active_bitmap = bitmap;
    memcpy(state->oms->portfolio.positions, positions, sizeof(positions));

    for (uint32_t i = 0; i < file_num_nodes; ++i) {
        NodeSnap& s = snaps[i];
        NodeContext<F>& ctx = state->nodes[i];
        // Strategy id intentionally NOT restored — it comes from cfg.
        // resolved_strategy_id is per-tick output; restore it for display
        // continuity but the next rebuild will overwrite anyway.
        ctx.resolved_strategy_id = s.resolved_strategy_id;
        // v5.4.0: persisted `strategy_state_kind` was originally intended
        // to drive Strategy_InitPerCore at boot (the comment said "engine's
        // init path checks strategy_state_kind and dispatches"). In
        // practice, EngineSharded.hpp:1183 dispatches on
        // `state.nodes[i].strategy_id` (cfg-derived), NOT the loaded kind.
        // The persisted field is therefore dead weight — and worse,
        // restoring it here corrupts the invariant that `strategy_state_kind`
        // describes the C++ type of the allocated `strategy_state` pointer.
        //
        // Concrete repro of the kind/state mismatch the restore introduced:
        //
        //   Run 1 cfg: node_0_strategy=auto         → state=nullptr, kind=AUTO
        //              snapshot saves kind=AUTO.
        //   Run 2 cfg: node_0_strategy=mean_reversion
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
        ctx.node_realized        = s.node_realized;
        ctx.node_fees            = s.node_fees;
        ctx.node_open_notional   = s.node_open_notional;
        ctx.node_wins            = s.node_wins;
        ctx.node_losses          = s.node_losses;
        // v5.4.3 (snapshot v5): apply gross accumulators + idle counter.
        ctx.node_gross_wins      = s.node_gross_wins;
        ctx.node_gross_losses    = s.node_gross_losses;
        ctx.idle_cycles          = s.idle_cycles;
        ctx.last_entry_price     = s.last_entry_price;
        ctx.last_entry_tick      = s.last_entry_tick;
        ctx.sl_cooldown_remaining= s.sl_cooldown_remaining;
        ctx.node_peak_balance    = s.node_peak_balance;
        ctx.node_dd_pct          = s.node_dd_pct;
        // v5.15.5.B.3 — kill bit packed in node_state_flags; load 1 byte
        // into a temp + set/clear bit accordingly.
        if (s.node_kill_tripped) {
            NODE_STATE_FLAG_SET(ctx, KILL_TRIPPED);
        } else {
            NODE_STATE_FLAG_CLR(ctx, KILL_TRIPPED);
        }
        ctx.node_ks_trips_total  = s.node_ks_trips_total;
        RegimeState_CommitPersistedFields(&ctx.regime_state, &s.staging_regime);
        RegressionFeederX_CommitPersistedFields(&ctx.pnl_feeder, &s.staging_feeder);
        ctx.staged_prediction = s.staged;
        ctx.active_prediction = s.active;
        ctx.last_confidence   = s.last_confidence;
        // v5.15.5.E.0 — Commit ConfidenceScorer persisted subset via shared
        // helper. FOREACH_CONFIDENCE_PERSIST_FIELD drives both the read into
        // staging AND this commit copy → adding a new persisted field is ONE
        // registry row; both paths auto-update. The `window` field is NOT in
        // the registry: it stays at the value set by ConfidenceScorer_Init
        // (current cfg), so the runtime composite-mode + window stay valid
        // even when restoring from an older config's snapshot.
        ConfidenceScorer_CommitPersistedFields(&ctx.confidence, &s.staging_confidence);
        // v5.15.5.E.D — Recompute running sum_squared_errors after commit
        // (not in wire format; cheap O(N=32) recompute keeps wire minimal).
        ConfidenceScorer_RecomputeRunningSums(&ctx.confidence);
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
        int node_id = Sharded_SlotNode(slot, partial_exit_enabled);
        int leg     = partial_exit_enabled ? (slot & 1)  : 0;
        if (node_id < 0 || node_id >= (int)state->registered_count) continue;
        ExecutionCore<F>* node_ptr = state->nodes[node_id].core;
        if (!node_ptr) continue;
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
        Money entry = pos.entry_price;
        // When cfg is provided (production caller), recompute live_tp/sl
        // from entry × (1 ± pct) so it matches the fresh-entry hot path.
        // When cfg is nullptr (legacy callers, tests), fall back to the
        // pre-fix pos.* path.
        Money live_tp_a = pos.take_profit_price;
        Money live_sl_a = pos.stop_loss_price;
        Money live_tp_b_val = pos.take_profit_price;
        if (cfg) {
            ControllerConfig<F> resolved = ControllerConfig_ResolveForCore(*cfg, node_id);
            // .E.0.10 A1 (H22): resolve the per-NODE per-strategy override, NOT the GLOBAL pct —
            // single-sourced with the fresh-entry dispatcher (ResolvePerFillTpPct/SlPct) so a
            // restored SimpleDip/MR/EmaCross position exits at the SAME TP/SL it had while live.
            const uint8_t a1_sid = state->nodes[node_id].resolved_strategy_id;
            Money tp_pct_a = ResolvePerFillTpPct(a1_sid, resolved);
            Money sl_pct_a = ResolvePerFillSlPct(a1_sid, resolved);
            if (!Money_IsZero(tp_pct_a))
                live_tp_a = Money_Add(entry, Money_Mul(entry, tp_pct_a));
            if (!Money_IsZero(sl_pct_a))
                live_sl_a = Money_Sub(entry, Money_Mul(entry, sl_pct_a));
            Money tp_pct_b = !Money_IsZero(resolved.tp2_mult) && !Money_IsZero(tp_pct_a)
                ? Money_Mul(tp_pct_a, resolved.tp2_mult)
                : tp_pct_a;
            if (!Money_IsZero(tp_pct_b))
                live_tp_b_val = Money_Add(entry, Money_Mul(entry, tp_pct_b));
        }
        if (leg == 0) {
            node_ptr->entry_price = entry;
            node_ptr->live_tp     = live_tp_a;
            node_ptr->live_sl     = live_sl_a;
            node_ptr->active      = 1;
        } else {
            node_ptr->entry_price_b = entry;
            node_ptr->live_tp_b     = live_tp_b_val;
            node_ptr->live_sl_b     = live_sl_a;  // shared SL
            node_ptr->active_b      = 1;
        }
        restored_count++;
    }
    if (restored_count > 0) {
        fprintf(stderr,
            "[snapshot] re-activated %d ExecutionCore(s) from restored "
            "positions — hot-path SG/TP/SL gates armed\n",
            restored_count);
    }

    fprintf(stderr, "[snapshot] loaded sharded snapshot from %s (%u nodes, ts=%llu)\n",
            filepath, file_num_nodes, (unsigned long long)timestamp_us);
    return 1;
}

}  // namespace tt
