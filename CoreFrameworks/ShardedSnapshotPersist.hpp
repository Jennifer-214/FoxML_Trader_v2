#pragma once
//======================================================================================================
// [FILE]_[CoreFrameworks/ShardedSnapshotPersist.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[sharded snapshot save/load — atomic-rename writes, refuse-don't-migrate loads, H21 append-only version]
// [CONTAINS]
//   - [MACRO]_[SHARDED_SNAPSHOT_VERSION]
//   - [FUNCTION]_[ShardedSnapshot_Save]
//   - [FUNCTION]_[ShardedSnapshot_Load]
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
//   - Refuse if magic doesn't match (legacy PortfolioController-format "TICK" file, corruption, wrong file).
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
// E.1.2 D-305 — the ordered per-node persist wire spec (29 rows; SAVE/READ/COMMIT
// projections). MUST come after NodeContext + the delegate walkers are visible.
#include "../MemHeaders/NodeCtxPersistRegistry.hpp"
// v5.15.5.C.3 Phase 3b — canonical FOREACH_OMS_FIELD + OMS_PROJECT_PERSIST_*
// dispatch is included transitively via OrderManager.hpp:71. The legacy
// OmsPersistFieldRegistry.hpp was deleted in this phase; consumers now walk
// the canonical 8-tuple registry via the PERSIST projection macros.

namespace tt {

//----------------------------------------------------------------------
// [MACRO]_[SHARDED_SNAPSHOT_VERSION]
// [TAG]_[[ENGINE] [PERSISTENCE] [DETERMINISM]]
// [REFERENCE]_[INVARIANT]_[[H21] [H9]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[wire magic + APPEND-ONLY snapshot version (v1->v10 history inline) + Ship-B money-epoch compile guard]
//----------------------------------------------------------------------
// 0x53484430 = "SHD0" little-endian. Distinct from PORTFOLIO_SNAPSHOT_MAGIC
// (0x4B434954 "TICK") so a legacy PortfolioController-format file produces a
// clean refuse-load rather than parsing as garbage. ("legacy v11" here meant
// the CONTROLLER format's version — an active name-collision once SHARDED
// itself reached 11; reworded at the E.1.2 bump.)
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
#define SHARDED_SNAPSHOT_VERSION  11u  // E.1.2 v11: per-node wire row swap — node_dd_pct DROPPED (recompute) + partner_pending_pnl ADDED (AM-4/TD-227/D-420); net-0 bytes, name-listing golden is the catcher; v10 (Ship-B DECIMAL epoch, H21 tombstone) rejected. Was: // Ship-B DECIMAL: money re-encoded; v9 (16B binary) rejected

// Ship-B P2 epoch guard (S-4): this file raw-fwrites per-core money (allocated_balance /
// node_realized / fees / notional / pnl_feeder / 16x Position). Encoding-keyed (the 16B->16B
// decimal flip is layout-invisible): red-builds until the version rides the SAME commit.
// [ASSERT]_[EPOCH_TRIPWIRE]_[SHARDED_SNAPSHOT_VERSION >= 9 + MONEY_ENCODING_EPOCH — encoding flip forces a version bump]
static_assert(MONEY_ENCODING_EPOCH == 0u || SHARDED_SNAPSHOT_VERSION >= 9u + MONEY_ENCODING_EPOCH,
              "Ship-B epoch: the engine money type flipped to decimal — bump "
              "SHARDED_SNAPSHOT_VERSION past the epoch floor (H21 tombstone the "
              "old version) in THIS commit.");

//======================================================================
// [FUNCTION]_[ShardedSnapshot_Save]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [SLOW_PATH]]
// [REFERENCE]_[INVARIANT]_[[H21] [H9]]
// [REFERENCE]_[DESIGN_SPEC]_[[registry-tuple-as-single-source-of-truth] [autopopulate-pattern-for-production-caller-class.md]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[atomic-rename snapshot write (.tmp + fsync + rename) — header, registry-driven OMS block, 16 positions, per-node blocks]
// [REFERENCE]_[CLASS]_[4]
//======================================================================
// [CODE]
//======================================================================
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
    // E.1.2 D-305 — registry-driven wire walk. The ordered FOREACH_NODE_PERSIST_FIELD
    // (MemHeaders/NodeCtxPersistRegistry.hpp) IS the per-node wire spec: 29 rows,
    // 1944B/node, row order == emission order, byte-identical to the retired
    // hand-loop (frozen golden: tests/sharded_snapshot_v11_golden.hpp). Per-field
    // history (v5.4.3 Class-4 gross-drop, the v5.11.15 kind-leak NO_COMMITs, the
    // .B.3 kill-bit byte format, the Step-2c delegates + the D-110 interleave)
    // rides the registry rows. Dropping a wire op ⟺ dropping a row ⟺ caught by
    // the ==29 count-lock. `confidence.window` stays deliberately OFF the wire
    // (cfg-owned at Init; persisting would carry stale cfg across restarts).
    for (uint32_t i = 0; i < num_nodes; ++i) {
        const NodeContext<F>& ctx = state->nodes[tt::NodeIdx{(int16_t)i}];
        FOREACH_NODE_PERSIST_FIELD(NPF_PROJECT_SAVE)
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Returns 1 on success, 0 on any I/O failure. On failure the previous good
// file (if any) is unchanged — atomic rename never moves the .tmp into
// place if writes failed.
//
// `partial_exit_enabled` is the engine's current cfg.partial_exit_enabled
// (1 if partials are on, 0 otherwise). Persisted in the header so the
// loader can refuse a snapshot taken under a different toggle state —
// position slot indices have different meaning between the two
// geometries (1:1 vs paired legs A+B).
//======================================================================
// [END_FUNCTION]_[ShardedSnapshot_Save]
//======================================================================

//======================================================================
// [FUNCTION]_[ShardedSnapshot_Load]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [BOOT_TIME]]
// [REFERENCE]_[INVARIANT]_[[H21] [H22]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[refuse-don't-migrate load — magic/version/node-count/partials-geometry gates, staged reads, all-or-nothing commit, ExecutionCore re-activation]
// [REFERENCE]_[DECISION]_[D-305]
// [REFERENCE]_[DESIGN_SPEC]_[registry-tuple-as-single-source-of-truth.md]
//======================================================================
// [CODE]
//======================================================================
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
    // Refuse legacy PortfolioController-format snapshots cleanly (any TICK-era
    // file — controller v4-v14 AND the old Portfolio_Save v<=7 both stamped it).
    if (magic == 0x4B434954u) {  // PORTFOLIO_SNAPSHOT_MAGIC
        fprintf(stderr, "[snapshot] %s is a LEGACY snapshot (PortfolioController-format, TICK magic). "
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
    // E.1.2 D-305: NodeSnap IS the registry's DECLARE view, hand-kept (nested staging
    // landed at Step 2c) — field names MATCH the FOREACH_NODE_PERSIST_FIELD row NAMEs
    // so the READ/COMMIT projections address `s.<NAME>` and `ctx.<NAME>` uniformly.
    // PAD rows use walk-local scratch (no staging field).
    struct NodeSnap {
        uint8_t  strategy_id;
        uint8_t  resolved_strategy_id;
        uint8_t  strategy_state_kind;  // v5.4.0 — read off the wire; NO_COMMIT (v5.11.15 leak — see commit site)
        Money   allocated_balance;
        uint64_t entries_processed;
        uint64_t exits_processed;
        Money   node_realized, node_fees, node_open_notional;
        uint32_t node_wins, node_losses;
        // v5.4.3 (snapshot v5): gross accumulators + idle counter
        Money   node_gross_wins, node_gross_losses;
        // E.1.2 v11 (D-420/AM-4): the W/L pairing park joins the wire; its
        // bitmap is EventLoopState-level and RE-DERIVED post-walk, never staged.
        Money   partner_pending_pnl;
        uint32_t idle_cycles;
        Money   last_entry_price;
        uint64_t last_entry_tick;
        uint32_t sl_cooldown_remaining;
        // node_dd_pct DROPPED at v11 (D-420): eval-transient. NARROWED at D-421 —
        // "recomputed before EVERY read" was the wording here and in the registry,
        // and it covers 2 of the 4 reads. Both CAPITAL reads (the kill-trip eval +
        // its log) ARE dominated by the recompute from the persisted
        // node_peak_balance, both branches assigning, so no capital decision ever
        // sees a stale drawdown — that is the property the drop rests on. The TUI
        // publish and the paper-reset summary emit read it OUT of pass and show 0
        // for one slow-path cycle after a warm restart: display-only, accepted.
        // (SECOND copy of the claim — the D-421 sweep corrected the registry one
        // and missed this one; found by the close's independent review, not a guard.)
        Money   node_peak_balance;
        uint8_t  node_kill_tripped;
        uint32_t node_ks_trips_total;
        // regime — nested staging (E.1.2 Step-2, D-305): FieldwiseRead populates the 7
        // persisted fields; the 2 unpersisted score ints stay default-init + uncommitted.
        RegimeState<F> regime_state;
        // feeder — nested staging (E.1.2 Step-2, D-305): FieldwiseRead populates all 3 fields.
        RegressionFeederX<F> pnl_feeder;
        // confidence prediction snapshots (v1; the D-110 interleave doubles)
        double   staged_prediction, active_prediction, last_confidence;
        // v5.15.5.E.0 — staging ConfidenceScorer instance replaces the prior
        // flat staging fields (ic_predictions / ic_actuals / ic_count / etc).
        // FieldwiseRead populates only the persisted subset (ic + rmse
        // history); composite-mode fields default-init via this struct's
        // default-initialization (they're re-init from cfg via
        // ConfidenceScorer_BindCompositeCfg at boot, NOT restored from snapshot).
        // Pattern: DESIGN_SPECS/registry-tuple-as-single-source-of-truth.md.
        ConfidenceScorer confidence;
    };
    tt::NodeArray<NodeSnap, MAX_EXECUTION_NODES> snaps;   // E.1.3 P0/TD-299: typed per-NODE

    // E.1.2 D-305 — registry-driven read walk (same 29 ordered rows as the save
    // walk; STORAGE_KIND dispatch ONLY, so NO_COMMIT rows still consume their
    // wire bytes and every later offset stays correct — the A2 invariant).
    for (uint32_t i = 0; i < file_num_nodes; ++i) {
        NodeSnap& s = snaps[tt::NodeIdx{(int16_t)i}];
        FOREACH_NODE_PERSIST_FIELD(NPF_PROJECT_READ)
    }
    fclose(f);

    // ---- COMMIT (after all reads validated) ----
    // v5.15.5.C.2 (S3a-W) — registry-driven commit. tmp_<name> → state->oms->name
    // for DIRECT-kind fields; bit set/clear for BIT-kind kill_switch_tripped.
    // v5.15.5.C.3 Phase 3b — canonical PERSIST projection (COMMIT).
    FOREACH_OMS_FIELD(OMS_PROJECT_PERSIST_COMMIT)
    state->oms->portfolio.active_bitmap = bitmap;
    memcpy(state->oms->portfolio.positions, positions, sizeof(positions));

    // E.1.2 D-305 — registry-driven commit walk (COMMIT_KIND × STORAGE_KIND
    // dispatch). The two NO_COMMIT rows are DELIBERATE — history below stays
    // load-bearing (the registry rows cross-reference it):
    //
    // `strategy_id` intentionally NOT restored — it comes from cfg.
    // `resolved_strategy_id` IS restored (per-tick output; display continuity —
    // the next rebuild overwrites anyway).
    //
    // v5.4.0: persisted `strategy_state_kind` was originally intended
    // to drive Strategy_InitPerCore at boot (the comment said "engine's
    // init path checks strategy_state_kind and dispatches"). In
    // practice, the boot path (EngineCommon_BootPerCore, called from
    // EngineSharded/Run.hpp) dispatches Strategy_InitPerCore on
    // `state..strategy_id` (cfg-derived), NOT the loaded kind.
    // The persisted field is therefore dead weight — and worse,
    // restoring it corrupts the invariant that `strategy_state_kind`
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
    // back-compat — the read walk above still READS it into
    // s.strategy_state_kind per the A2 invariant, the NO_COMMIT kind just
    // never APPLIES it). Future snapshot versions can drop the field
    // entirely without breaking older saves.
    //
    // The kind invariant is now: set ONLY by Strategy_InitPerCore at
    // boot and Strategy_FreePerCore on teardown (both in
    // Strategies/StrategyLifecycle.hpp). No other site mutates it.
    //
    // The `window` field is NOT in the confidence sub-registry: it stays at
    // the value set by ConfidenceScorer_Init (current cfg), so the runtime
    // composite-mode + window stay valid even when restoring from an older
    // config's snapshot.
    for (uint32_t i = 0; i < file_num_nodes; ++i) {
        NodeSnap& s = snaps[tt::NodeIdx{(int16_t)i}];
        NodeContext<F>& ctx = state->nodes[tt::NodeIdx{(int16_t)i}];
        FOREACH_NODE_PERSIST_FIELD(NPF_PROJECT_COMMIT)
        // E.1.2 REC-A: the derived rmse.sum_squared_errors recompute is now
        // EMBEDDED in ConfidenceScorer_CommitPersistedFields' tail (the
        // confidence DELEGATE row's commit) — no caller-side call to forget.
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
        int node_id = (int)Sharded_SlotNode(tt::SlotIdx{(int16_t)slot}, partial_exit_enabled);
        int leg     = partial_exit_enabled ? (slot & 1)  : 0;
        if (node_id < 0 || node_id >= (int)state->registered_count) continue;
        ExecutionCore<F>* node_ptr = state->nodes[tt::NodeIdx{(int16_t)node_id}].core;
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
            const uint8_t a1_sid = state->nodes[tt::NodeIdx{(int16_t)node_id}].resolved_strategy_id;
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

    // E.1.2 v11 (D-420/AM-4) — re-derive partner_pending_bitmap from slot parity.
    // The bitmap is EventLoopState-level, NOT a per-node wire row: under partials,
    // exactly-one-leg-active ⟺ a first-leg exit parked its net in the node's
    // partner_pending_pnl (committed by the registry walk above) OR a rare orphan
    // leg (ring-full leg-B push, pnl==0 — its exit then merges-with-zero into ONE
    // correct W/L stat; the D-420 state table shows re-derive ties-or-wins every
    // reachable state, so the bitmap never rides the wire). Whole-value ASSIGN,
    // never OR. Gated on the partials geometry: with partials OFF every slot is
    // its own node and a lone active slot MUST NOT set a partner bit; the
    // partials-mismatch refuse-load above guarantees file⟷cfg geometry agreement,
    // so this gate is total. Boot-time-only (H20 boot exception).
    state->partner_pending_bitmap = 0;
    if (partial_exit_enabled) {
        const uint16_t am4_bm = state->oms->portfolio.active_bitmap;
        for (int n = 0; n < (int)state->registered_count; ++n) {
            const uint16_t leg_parity =
                (uint16_t)(((am4_bm >> (2 * n)) ^ (am4_bm >> (2 * n + 1))) & 1u);
            state->partner_pending_bitmap |= (uint16_t)(leg_parity << n);
        }
    }

    fprintf(stderr, "[snapshot] loaded sharded snapshot from %s (%u nodes, ts=%llu)\n",
            filepath, file_num_nodes, (unsigned long long)timestamp_us);
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Returns 1 on successful load (state populated from file), 0 on any
// validation failure (bad magic, version, core count). On failure the
// state is left untouched — caller continues with fresh init.
//
// Safety: explicit checks on magic + version + core count BEFORE any
// state mutation. The first per-core read failure aborts the whole load
// and rolls back to the in-memory snapshot taken at function entry.
//
// `partial_exit_enabled` is the engine's CURRENT cfg.partial_exit_enabled
// (1 if partials are on, 0 otherwise). Loader refuses files written under
// a different toggle state — slot indices have different meaning between
// the two geometries (1:1 vs paired legs A+B), so reusing positions
// across the toggle creates "zombie" entries that look real in the GUI
// panel but can't be exited by the hot path.
//======================================================================
// [END_FUNCTION]_[ShardedSnapshot_Load]
//======================================================================

}  // namespace tt
