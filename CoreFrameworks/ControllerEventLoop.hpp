// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/ControllerEventLoop.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [OMS_DRAINER] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the controller side of the sharded engine — NodeContext/EventLoopState state model + event drain + the per-node slow-path rebuild family + kill switch + exits]
// [CONTAINS]
//   - [STRUCT]_[NodeSlowState]          (+ NodeSlowState_Init)
//   - [STRUCT]_[SlowPathTelemetry]
//   - [STRUCT]_[NodeContext]
//   - [STRUCT]_[WsHeartbeatTelemetry]
//   - [STRUCT]_[NodeContextDisplayMeta] (+ NodeContextDisplayMeta_Init)
//   - [STRUCT]_[EventLoopState]
//   - [FUNCTION]_[EventLoopState_ReconstructPerCoreFromEventLog]
//   - [FUNCTION]_[EventLoopState_Init]  (+ InitLegacy / Free)
//   - [FUNCTION]_[EventLoopState_RegisterCore]
//   - [FUNCTION]_[Sharded_SlotNode]     (+ LegSlot / NodeSlotMask / ValidatePartialExitCfg geometry family)
//   - [FUNCTION]_[EventLoopState_SetCoreStrategy]
//   - [FUNCTION]_[EventLoop_DrainPostFillOneCore] (+ DrainPostFill fan)
//   - [FUNCTION]_[EventLoop_OnEvent]
//   - [FUNCTION]_[EventLoop_DrainEvents]
//   - [FUNCTION]_[EventLoop_RebuildAllParameters]
//   - [FUNCTION]_[EventLoop_UpdateRollingStateOneCore] (+ UpdateEmaPriceAllCores)
//   - [FUNCTION]_[EventLoop_RebuildOneCore]
//   - [FUNCTION]_[EventLoop_PushParameters]
//   - [FUNCTION]_[EventLoop_KillSwitchEvaluate] (+ ConfigureKillSwitch / ClearAllPermissions / KillSwitchTrip)
//   - [FUNCTION]_[EventLoop_TimeExitOneCore]
//   - [FUNCTION]_[EventLoop_FlattenAll] (+ CheckWsStaleness / TryClearRecovery)
//   - [FUNCTION]_[EventLoop_TrailingSLRatchetOneCore]
//   - [FUNCTION]_[EventLoop_BreakevenOnProfitOneCore]
//   - [FUNCTION]_[EventLoop_Unpause] (+ SlowPath / RunController)
//======================================================================================================
// Per-core sharded engine controller-side. Reads TradeEvents from each
// registered execution core's event ring, processes entries and exits into
// the canonical Portfolio state, updates balance and realized P&L.
//
// Architecture:
//   - controller core owns this loop, runs on its own pinned CPU
//   - each execution core registered via EventLoopState_RegisterCore
//   - node_id maps directly to portfolio slot (no indirection)
//   - drain happens round-robin with a per-core cap so one chatty core
//     can't starve the others (pitfall P4.1)
//   - events are processed in arrival-per-core order; cross-core ordering
//     is NOT preserved by drain (pitfall P4.2 — sort by event.timestamp
//     downstream if you need market-time ordering)
//
// What this phase IMPLEMENTS:
//   - EventLoopState struct + init
//   - core registration (allocates slot, stores intended trade params)
//   - OnEvent (single event → portfolio + balance update)
//   - DrainEvents (round-robin drain across registered cores)
//   - RunController (the controller core's main thread loop)
//
// What this phase does NOT do:
//   - Parameter push from controller to cores (phase 05 — for now register
//     time fixes the intended TP/SL/qty for the next entry)
//   - TradeLog CSV writing (phase 08 — OnEvent has hooks but no impl)
//   - Kill switch evaluation (phase 09)
//   - Snapshot building (phase 07/10)
//
// Critical invariant from pitfall P4.7:
//   NEVER write to core->gate_params from inside _OnEvent. The execution core
//   is reading those fields concurrently. Parameter writes are deferred to
//   the parameter-push step (phase 05), which uses an atomic ParameterSlot.
//   For phase 04 the intended TP/SL/qty are stored on the controller side
//   (in NodeContext) and applied to the portfolio at entry-event time.
//======================================================================================================

#pragma once

#include "../Limits.hpp"
#include "../ML_Headers/ConfidenceScore.hpp"
#include "SlowPathGateRegistry.hpp"  // v5.14.9.B.0 — FOREACH_SLOW_PATH_GATE + AUTOPOPULATE
#include "../ML_Headers/LinearRegression3X.hpp"  // v4.0.3 D10 RegressionFeederX
#include "../ML_Headers/RollingStats.hpp"
#include "../ML_Headers/RollingWindowRegistry.hpp"  // v5.15.5.B.6 — FOREACH_ROLLING_WINDOW(name, W) cohort
#include "../ML_Headers/RollingTurnover.hpp"  // v5.14.1.G — portfolio turnover
#include "../ML_Headers/ROR_regressor.hpp"        // v5.1.0 — RORRegressor on NodeContext::slow_state
#include "../ML_Headers/FlowFeatures.hpp"         // v5.1.0 — FlowState etc on NodeContext::slow_state
#include "../MemHeaders/HealthLog.hpp"
#include "../MemHeaders/InitArena.hpp"  // v5.11.6.A — unified mmap arena for init allocations
#include "../ML_Headers/FeatureRegistry.hpp"  // v5.9.0b: FEATURE_REGISTRY_HASH() in entry log           // v5.4.0 Phase 0.1 — structured JSONL diagnostic log
#include "../Strategies/StrategyParameters.hpp"
// Strategies/StrategyLifecycle.hpp included LATER (post-EventLoopState
// definition) to avoid the include cycle: SL needs NodeContext +
// EventLoopState, and ControllerEventLoop.hpp defines them below.
#include "NodeLatencyStats.hpp"  // v4.7.42 — slow_path_latency on NodeContext
#include "../MemHeaders/DisplayMetaRegistry.hpp"  // v5.15.5.B.2 — FOREACH_GATE_DIAG_PAIR + FOREACH_DISPLAY_META_FIELD
#include "../MemHeaders/NodeStateFlagRegistry.hpp"  // v5.15.5.B.3 — FOREACH_NODE_STATE_FLAG bitmap for 5 booleans
#include "SpSectionRegistry.hpp"  // v5.15.5.B.5 — FOREACH_SP_SECTION enum + SP_SECTION_NAME/DOC helpers
#include "ExecutionCore.hpp"
#include "Notify.hpp"
#include "OrderManager.hpp"
#include "../MemHeaders/OmsPushExitHelper.hpp"  // v5.15.5.C.4 Phase D5 — OMS_PushExitForSlot helper (Class-18 close)
#include "Portfolio.hpp"
#include "ShardedTradeLog.hpp"
#include "TradeEvent.hpp"

#include <cstdint>
#include <type_traits>  // E.1.2.C leg 0 — std::common_type non-deduced wrapper below
#include <ctime>
#include <chrono>      // v5.12.1.A.2 — system_clock for WS staleness math

namespace tt {

//======================================================================
// [STRUCT]_[NodeSlowState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [DATA_ORIENTED_DESIGN]]
// [THREAD]_[[PRODUCER_WRITER] [SLOW_READER]]
// [REFERENCE]_[INVARIANT]_[H6]
// [REFERENCE]_[DESIGN_SPEC]_[decision-first-cluster-layout-pattern]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-node data plane — lazy-rebuild gate trio at offset 0 (locked by static_assert), then registry-generated rolling windows + regime/flow state; ema_price is the one producer-written field]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct NodeSlowState {
    // ---- v5.15.5.B.1 — HOT cluster at struct head (lazy-rebuild gate + ema_price) ----
    //
    // EventLoop_RebuildOneCore reads these THREE fields FIRST every cycle to
    // run the lazy-rebuild gate (CLAUDE.md item 18 Pattern 8a). Placing them
    // at offset 0 means skip-eligible cycles touch ONE cache line and bail;
    // pre-v5.15.5.B.1 they sat at struct TAIL (offset ~278384), guaranteeing
    // a cold-cache miss every cycle (~100 ns each, ~30-50% of cycles fire
    // the bail per CLAUDE.md item 18 lazy-rebuild stat). See
    // decision-first-cluster-layout-pattern.md Step 4 (forward-sequential
    // subsequent fields).
    //
    // ema_price (per-tick: producer fan_out updates EVERY tick; slow-path
    // consumers read at slow-path cadence; in per_node_slow this is a
    // producer→slow-path cross-thread read — eventual consistency,
    // x86-acceptable on aligned word).
    FPN_Binary<F>                  ema_price;

    // v5.12.2.B — lazy slow-path rebuild bookkeeping. Updated at the
    // END of every full RebuildOneCore execution. The next RebuildOneCore
    // call uses these to decide whether to skip the rebuild body when:
    //   (a) BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_LAZY_REBUILD_ENABLED) = 1 AND
    //   (b) (now_us - us_at_last_rebuild) < cfg.lazy_rebuild_force_period_us AND
    //   (c) |price_avg - price_at_last_rebuild| / price_at_last_rebuild
    //       < cfg.lazy_rebuild_price_threshold_pct
    // When all three hold, RebuildOneCore returns early after marking
    // pending_params for republish (so v5.12.1.B's publish_tick stays
    // fresh). Single-writer (this core's slow-path); no atomics.
    uint64_t                us_at_last_rebuild;
    FPN_Binary<F>                  price_at_last_rebuild;

    // ---- WARM/COLD cluster: per-cadence rolling/regime/flow state ----
    //
    // Per-cadence rolling stats (RegimeDetector inputs). alignas(64) on
    // RollingStats::head propagates the discipline to all W instantiations.
    // v5.15.5.B.6 — registry-driven field declarations via FOREACH_ROLLING_WINDOW.
    // Adding a 5th window = ONE row in ML_Headers/RollingWindowRegistry.hpp.
#define X(name, W) RollingStats<F, W> rolling_##name;
    FOREACH_ROLLING_WINDOW(X)
#undef X

    // Per-cadence regime / flow state.
    RORRegressor<F>         regime_ror;
    CumDeltaState<F>        cumdelta_state;
    TickRateState           tick_rate_state;
    BookImbalanceHistory<F, 1024> book_imb_history;
    FlowState               flow_state;
    LargeTradeState<F, 1024> large_trade_state;
    SpreadState<F, 1024>    spread_state;
};

// v5.15.5.B.1 — Layout invariant. ema_price + lazy_rebuild gate fields MUST
// sit at offset 0 of NodeSlowState (decision-first-cluster-layout-pattern.md
// Step 2). Hoisted from struct TAIL — the per-cycle lazy-rebuild gate at
// EventLoop_RebuildOneCore reads them FIRST every cycle; offset-0 placement
// means skip-eligible cycles touch one cache line and bail.
static_assert(offsetof(NodeSlowState<64>, ema_price) == 0,
              "ema_price MUST sit at NodeSlowState offset 0 — per-cycle lazy-"
              "rebuild gate reads it first; see decision-first-cluster-layout-"
              "pattern.md Step 4 (forward-sequential subsequent fields).");

template <unsigned F>
inline void NodeSlowState_Init(NodeSlowState<F>* s) {
    // v5.15.5.B.6 — registry-driven init via FOREACH_ROLLING_WINDOW.
#define X(name, W) s->rolling_##name = RollingStats_Init<F, W>();
    FOREACH_ROLLING_WINDOW(X)
#undef X
    s->regime_ror       = RORRegressor_Init<F>();
    CumDelta_Init(&s->cumdelta_state);
    TickRate_Init(&s->tick_rate_state);
    BookImbHistory_Init(&s->book_imb_history);
    FlowState_Init(&s->flow_state);
    LargeTradeState_Init(&s->large_trade_state);
    SpreadState_Init(&s->spread_state);
    s->ema_price = FPN_Zero<F>();
    // v5.12.2.B — initial values force a full rebuild on the first cycle
    // (us_at_last_rebuild=0 → time-bound predicate fires; price=0 →
    // delta predicate fires).
    s->us_at_last_rebuild = 0;
    s->price_at_last_rebuild = FPN_Zero<F>();
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.1.0 per-core data plane — each engine owns its rolling/regime/flow state instead of reading
// producer-owned shared state. See plans/2026-04-28-v5.1-data-plane-
// decouple.md for the full design.
//
// SINGLE-WRITER RULE per engine:
//   - per_node_slow arch:  Producer writes per-tick (ema_price) to all N
//                          cores in fan_out; per-cadence updates done by
//                          per-core slow-path c on its OWN slow_state
//   - backtest:            Single thread; loops c=0..N
//
// READ PATTERN: per-core slow-path body (per-core thread) reads
// state.nodes[c].slow_state. ema_price is producer-written; per-cadence
// fields are written by the per-core thread itself (no cross-thread for
// those). ema_price has eventual-consistency cross-thread reads (relaxed
// loads — same as pre-v5.1.0 shared design but now per-core targets).
//
// Window sizes are template-fixed (RollingStats<F, 128> etc.). Per-core
// override of window size is a future enhancement requiring runtime-
// sized buffers — currently every engine uses identical windows.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-24]
// [SIZE]_[191744B]
// [ALIGN]_[64]
// [CACHE_LINES]_[2996]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[NodeSlowState]
//======================================================================

//======================================================================
// [STRUCT]_[SlowPathTelemetry]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CONCURRENCY] [MONITORING_PLANE]]
// [THREAD]_[[SLOW_WRITER] [PRODUCER_READER]]
// [SYNC]_[ATOMIC]
// [REFERENCE]_[INVARIANT]_[H6]
// [REFERENCE]_[DESIGN_SPEC]_[cross-thread-snapshot-publish-cluster-isolation]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-node slow-path liveness cluster — exactly one cache line (both locked by static_assert); publisher reads can't invalidate slow-path neighbors]
//======================================================================
// [CODE]
//======================================================================
struct alignas(64) SlowPathTelemetry {
    std::atomic<uint64_t> last_tick_us{0};
    std::atomic<uint64_t> cycles_total{0};
    std::atomic<uint64_t> yield_count{0};
    std::atomic<uint8_t>  state{0};
};
static_assert(alignof(SlowPathTelemetry) == 64,
              "SlowPathTelemetry MUST be cache-line aligned for cross-thread "
              "isolation. See cross-thread-snapshot-publish-cluster-isolation.md.");
static_assert(sizeof(SlowPathTelemetry) == 64,
              "SlowPathTelemetry MUST occupy exactly one cache line.");
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.B.2 — v5.0.3-era thread observability fields, wrapped in alignas(64) cluster for
// cross-thread cache-line isolation. Single-writer is the slow-path thread
// that owns this core (or producer in centralized mode); GUI publish reads
// relaxed and copies into TUISnapshot::PerNodeSnap. Snapshot-publisher reads
// happen on a different thread → without alignas isolation, the read pulls
// the cache line + invalidates neighbor slow-path-written fields, causing
// cycle stalls at ~30Hz × 16 cores = ~480 invalidations/sec.
//
// Fields:
//   last_tick_us  — wall-clock us at end of last cycle (cadence-drift display)
//   cycles_total  — monotonic count of completed slow-path cycles
//   yield_count   — monotonic count of cadence yields + parks
//   state         — coarse thread state: 0=running, 1=parked (reset_in_progress),
//                   2=cadence-yield, 3=paused (user via paused_engines_mask)
//
// Pattern: cross-thread-snapshot-publish-cluster-isolation.md (ND1 first ref).
// ~25 B used in 64 B cache line — generous padding kept intentional for
// future flag additions + crosstalk prevention.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[64B]
// [ALIGN]_[64]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[SlowPathTelemetry]
//======================================================================

//======================================================================
// [STRUCT]_[NodeContext]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [DATA_ORIENTED_DESIGN]]
// [THREAD]_[[SLOW_PATH_WRITER] [TUI_READER]]
// [REGION]_[HOT cluster — every slow-path cycle; decision-first, gate_state at offset 0]
// [REGION]_[WARM cluster — per-event/per-fill accounting; entries_processed anchors (64B-locked)]
// [REGION]_[COLD cluster — display/cross-thread/lifetime; sp_telemetry anchors (64B-locked)]
// [REFERENCE]_[INVARIANT]_[[H6] [H14]]
// [REFERENCE]_[DESIGN_SPEC]_[[decision-first-cluster-layout-pattern] [cache-layout-discipline-for-hot-side-structs] [cross-thread-snapshot-publish-cluster-isolation.md]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-node controller-side state — HOT/WARM/COLD clustered by access cadence; explicit alignas(64) + static_asserts lock the inter-slot false-sharing invariant locally]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct alignas(64) NodeContext {
    // ════════════════════════════════════════════════════════════════════
    // HOT CLUSTER — touched every slow-path cycle.
    // Decision-first ordering: dispatch metadata at offset 0 (gate_state
    // bitmap → handles → strategy enums) so skip-eligible cycles touch
    // ONE cache line + the NodeSlowState head fields and bail.
    // ════════════════════════════════════════════════════════════════════

    // v5.14.9.B.0 — per-core slow-path gate cache (FOREACH_SLOW_PATH_GATE
    // PER_NODE entries). Populated by SLOW_PATH_GATE_AUTOPOPULATE_PER_NODE
    // at slow-path entry per core (after ControllerConfig_ResolveForCore).
    // Read via BITMAP_IS_SET(gate_state.flags, MASK_<NAME>) at use sites
    // in ML_BuildParameters body (mctx->gate_state pointer). At offset 0 of
    // HOT cluster so decision-first bail-out per ND3.
    //
    // ⚠ CORRECTED at E.1.2/D-421 — this comment used to say "single-threaded
    // per-core access; no atomics needed". That is FALSE, and its falseness is
    // very likely why this field went uninitialized for so long: it told every
    // reader there was nothing to ask about. The snapshot publisher reads
    // gate_state.flags from the PRODUCER thread (ShardedSnapshot.hpp) to build
    // the PerNodeSnap ladder bit. Single WRITER (the owning slow thread), NOT
    // single reader; the cross-thread read is a deliberately accepted
    // benign-race for display, not an absence of one. Full discussion at the
    // struct definition in SlowPathGateRegistry.hpp — and note the initializer
    // there is load-bearing, not style.
    SlowPathGateState gate_state;
    ExecutionCore<F>* core;             // registered execution core pointer
    // v5.1.0 (per-core data-plane decoupling): each engine OWNS its
    // rolling/regime/flow state. POINTER (not inline) — NodeSlowState is
    // ~272KB per engine; 16 inline copies would overflow the 8MB default
    // stack on tests. Heap-allocated in EventLoopState_Init; freed in
    // EventLoopState_Free. Slow-path-only access; indirection cost
    // negligible at slow-path cadence.
    NodeSlowState<F>* slow_state;
    void*    model_handle;              // NodeModelZoo<F>* for STRATEGY_ML nodes (nullptr for others)
    void*    ensemble_handle;           // v5.10.0a.G.5 — EnsembleModelZoo<F>* when multi-horizon active; nullptr = single-zoo path
    // v5.4.0 Phase 1.1 — per-strategy state (lifecycle stage 1 + 2:
    // Init/Adapt). Heap-allocated by Strategy_InitPerCore at engine boot;
    // freed by Strategy_FreePerCore on shutdown / strategy hot-swap.
    // Concrete type depends on strategy_state_kind:
    //   STRATEGY_MOMENTUM       → MomentumState<F>*
    //   STRATEGY_MEAN_REVERSION → MeanReversionState<F>*
    //   STRATEGY_SIMPLE_DIP     → SimpleDipState<F>*
    //   STRATEGY_EMA_CROSS      → EmaCrossState<F>*
    //   STRATEGY_ML             → MLStrategyState<F>*
    //   STRATEGY_AUTO/NONE      → nullptr (no state needed)
    // void* used to avoid pulling all strategy headers into
    // ControllerEventLoop.hpp. Single-writer per core (per-core slow-path
    // thread), single-reader (same thread). Snapshot persistence: only
    // strategy_state_kind is persisted; on load, Strategy_InitPerCore is
    // called to reallocate state from scratch matching the persisted kind.
    void*    strategy_state;
    uint8_t  strategy_id;               // STRATEGY_* constant; STRATEGY_NONE means "do not trade"
    uint8_t  resolved_strategy_id;      // v4.0.4 — after AUTO regime resolution; equals strategy_id for non-AUTO cores
    // v5.15.5.B.3 — 5 boolean flags bit-packed into uint8_t node_state_flags.
    // Replaces: dirty, node_kill_tripped, model_load_failed (was in DisplayMeta
    // v5.15.5.B.2), cfg_drift_strict_refused (same), warmup_log_emitted (same).
    // Per CLAUDE.md item 20 (BITMAP_* universalization) + item 1 (Portfolio
    // bitmap precedent). Single-writer per core; readers via NODE_STATE_FLAG_
    // IS_SET / BITMAP_ANY for branchless multi-flag check.
    uint8_t  node_state_flags;
    uint8_t  strategy_state_kind;       // matches strategy_id at allocation; 0xFF = uninitialized

    // v4.0.3 B: per-core regime state for STRATEGY_AUTO. Tracks current
    // regime + hysteresis so the auto-mode core's strategy choice doesn't
    // flap on noise. Each AUTO core has its own state — different cores
    // can detect different regimes if their cfg differs.
    // v5.15.5.F.4d.1.E.1.2 — alignas(64): was straddling L0→L1 @off 56 on a
    // [THREAD]-tagged struct (H6 false-sharing finding, D-414 register). The
    // anchor also keeps HOT line 0 pure decision-first dispatch (gate_state
    // → handles → strategy ids), with the 48B regime block whole on line 1.
    alignas(64) RegimeState<F> regime_state;

    // Phase 6prep (sharded c12-c14): per-core ML confidence loop. The scorer
    // is fed (prediction, realized_return) pairs at exit fill time and read
    // on the slow path during ML_BuildParameters to damp the entry threshold.
    // last_confidence is the most recently computed conf for snapshot reads
    // (don't recompute on hot/snapshot path — it's an O(W²) Spearman ranking).
    //
    // Two prediction fields, intentional:
    //   staged_prediction  — output of ML_BuildParameters; overwritten every
    //                        slow-path rebuild with the freshest prediction
    //   active_prediction  — snapshot of staged at entry-submit time; persists
    //                        across the entry→exit window so the IC update at
    //                        exit can correlate against the prediction that
    //                        actually triggered the trade (not the latest one)
    // Single-position-per-core invariant means active_prediction is plenty;
    // multi-position would need a per-position ring.
    ConfidenceScorer confidence;

    GateParameters<F> pending_params;   // staged params, pushed on next _PushParameters

    Money intended_tp;                 // TP to apply when this node's next entry fires
    Money intended_sl;                 // SL to apply when this node's next entry fires
    Money intended_qty;                // quantity to size the next entry to
    Money allocated_balance;           // capital share for this node (set by Regime_AllocateCores in phase 06+)

    // v4.0.3 D8 halt reason: most recent reason the gate was zero-gated.
    // 0 = ok / armed; 1 = spacing; 2 = vwap; 3 = long-slope; 4 = vol-delta;
    // 5 = min-stddev; 6 = sl-cooldown; 7 = warmup; 8 = core-budget (Phase 2.2);
    // 9 = core-kill (Phase 3); 10 = imbalance (Track E.3, surfaced v5.6.0).
    // Displayed in GUI per core.
    uint8_t  halt_reason;
    // v5.6.2: strategy-internal halt reason. Distinct from halt_reason
    // (controller-level). Each strategy's _BuildParameters sets this
    // to a SHALT_* code (see StrategyInterface.hpp) before zero-gating
    // or setting BUY_BLOCKED. SHALT_OK = no strategy-level veto.
    // GUI display order: halt_reason > 0 > strategy_halt_reason > 0.
    uint8_t  strategy_halt_reason;

    double staged_prediction;           // prediction from last ML rebuild
    double active_prediction;           // prediction at last entry submit (0 = no open pos)
    double last_confidence;             // most recent ConfidenceScorer_Compute result
    // v5.14.9.B — soft risk degradation ladder factor (composite confidence
    // × FOREACH_DEGRADATION_CURVE compute fn). 1.0 when ladder inactive
    // (default cfg) preserves pre-v5.14.9 behavior; (0, 1) when active +
    // composite ∈ [min, full]; 0.0 when ladder bottom hit (entry blocked
    // + SHALT_LOW_CONFIDENCE). Written by ML_BuildParameters via
    // mctx.out_confidence_factor; copied to PerNodeSnap.ml_confidence_factor
    // by ShardedSnapshot for ML Status panel display.
    double last_confidence_factor;
    // v5.13.0.B — sell-side ML prediction. Written by ML_BuildParameters
    // when BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_USE_EXIT_MODEL) && exit_predictor_count > 0. Slow-path body
    // post-RebuildOneCore checks against cfg.exit_threshold and fires
    // OMS_PushSubmit for any open positions on this core's slot(s).
    // Default 0.0 = no exit prediction this cycle. Reset each cycle.
    double last_exit_prediction;
    int    last_exit_dominant_horizon;  // -1 = none; otherwise [0..exit_predictor_count)
    // v5.15.5.A.6 — buy-side per-horizon barrier dispatch observability.
    // Mirrors exit-side fields above; written by ML_BuildParameters when
    // the per-horizon barrier feature is active. Surfaced via PerNodeSnap
    // to MLStatusPanel for the `tp: 0.050% (h2)` display pattern.
    int     last_buy_dominant_horizon;  // -1 = no buy-side dispatch this cycle
    uint8_t last_barrier_mode_used;     // active mode enum (FOREACH_BARRIER_BLEND_MODE)

    // ════════════════════════════════════════════════════════════════════
    // WARM CLUSTER — per-event/per-fill accounting; NOT per-cycle.
    // alignas(64) on entries_processed marks the cluster boundary.
    // ════════════════════════════════════════════════════════════════════
    alignas(64) uint64_t entries_processed;  // bumped on entry event
    uint64_t exits_processed;           // bumped on exit event

    // v4.0.3 spacing: last entry price for this core, set by drainer on
    // entry submit. Strategy _BuildParameters checks
    // |new_entry - last_entry_price| < stddev × spacing_multiplier and
    // zero-gates if too close, preventing entry clustering at similar
    // prices. Mirrors legacy PortfolioController spacing logic.
    Money   last_entry_price;
    uint64_t last_entry_tick;           // for time-based exit (A3)
    // v4.7.6: wall-clock microseconds at the leg-A entry stamp site so
    // GUI can show "hold time" for open positions. Independent of
    // last_entry_tick (which is a producer count, not seconds).
    uint64_t last_entry_wall_us;
    // v4.0.3 D7 SL cooldown: after a stop-loss exit, pause entries on this
    // core for N slow-path cycles. Decremented each rebuild; entries
    // zero-gated while > 0. Optionally adaptive — scales by trend confidence
    // at SL time (BITMAP_IS_SET(cfg.risk_cfg_flags, MASK_RISK_CFG_SL_COOLDOWN_ADAPTIVE_ENABLED) per Cx-U H14 migration).
    uint32_t sl_cooldown_remaining;
    // v4.2.1 — slow-path cycles since last fill on this core. Resets on
    // entry. Used as a "death-spiral" detector: if a core hasn't fired
    // in many cycles, the pnl_feeder is full of stale regression data
    // that's no longer informative — clear it so adaptive feedback
    // (D10) doesn't keep applying shifts based on ancient outcomes.
    // Mirrors legacy PortfolioController_Tick's idle-cycles mechanism but
    // doesn't need the filter-decay step (sharded recomputes
    // resolved_cfg fresh each rebuild, so there's no live-filter state
    // to drift back toward defaults).
    uint32_t idle_cycles;

    // v4.0.4: per-core P&L tracking. The OMS keeps a single global
    // realized_pnl across all cores (since portfolio is shared); these
    // counters split it out by source core for the Account panel and any
    // future per-core kill switch / risk re-allocation logic. Updated in
    // EventLoop_OnEvent exit branch, alongside oms->realized_pnl.
    Money   node_realized;             // sum of net P&L from this node's exits
    Money   node_fees;                 // sum of fees paid by this node's fills
    uint32_t node_wins;                 // exits with net > 0
    uint32_t node_losses;               // exits with net <= 0
    // v4.7.21: per-trade W/L pairing under partial exits. When partials are
    // enabled, leg A and leg B close as separate fills, but they belong to
    // ONE trade idea. Counting each leg independently overstates trade count
    // and loses the "did this idea make money?" signal — e.g. leg A=TP1
    // (+small) plus leg B=SL (-larger) is net negative but per-leg counts
    // 1W + 1L. We pair them by stashing the first leg's net P&L; when the
    // partner closes we compute total net and bump node_wins/node_losses by 1.
    // partials disabled → bypass pairing, per-leg-A logic is correct
    // (single-leg trades, no partner exists).
    Money   partner_pending_pnl;
    // partner_pending_active migrated to EventLoopState.partner_pending_bitmap
    // (v5.14.9.G; 1 bit per core in single uint16_t bitmap)
    // v4.7.25: per-core gross win/loss accumulators, mirroring the legacy
    // single_core's ctrl->gross_wins / ctrl->gross_losses. Sum of net P&L
    // for winning trades (gross_wins) and absolute net P&L for losing
    // trades (gross_losses, stored unsigned). Updated alongside node_wins
    // / node_losses — same per-trade semantics under partials (sum the
    // pair, classify by sign, accumulate into the matching gross bucket).
    // Pre-v4.7.25 sharded snapshot left snap->avg_win / avg_loss /
    // profit_factor / expectancy at zero — these accumulators feed those
    // fields in TUI_CopySnapshotSharded.
    Money   node_gross_wins;
    Money   node_gross_losses;
    // Phase 2.1: per-core open notional. Sum of (entry_price × qty) across
    // currently-open positions for this core. Updated branchlessly in
    // EventLoop_OnEvent — entry adds notional, exit subtracts the SAME
    // notional snapshot (NOT exit_price × qty — asymmetric subtraction
    // would leak positive residue per winning trade and accumulate
    // unboundedly). FPN_SubSat guards against rare underflow.
    //
    // Used by: Account panel "Budget Used %" display today (instrumentation
    // only); Phase 2.2 sizing clamp (max_qty = (allocated - open_notional)
    // / entry_price); Phase 3 MTM kill switch (current_value = allocated +
    // realized + unrealized, where unrealized is computed from open
    // positions). Single-position-per-core invariant means this is the
    // entry notional of one position today, but the sum-of-positions
    // model survives future multi-position-per-core.
    Money   node_open_notional;

    // Phase 3: per-core kill switch state. Realized + MTM unrealized P&L
    // tracked against a peak-to-trough drawdown. When dd exceeds threshold
    // (and absolute drop exceeds min_kill_loss floor), the core is "killed"
    // — buying_halted equivalent, future entries zero-gate with reason 9.
    // Open positions ride to TP/SL; kill blocks NEW trades, doesn't force-close.
    //
    // current_value = allocated + realized + unrealized
    // peak          = FPN_Max(peak, current_value)  (slow path, branchless)
    // dd_pct        = (peak - current_value) / peak
    //
    // Manual reset via TUISharedState::kill_reset_per_node[N] resets the
    // trip flag and refreshes peak to current. Aggregate OMS-level breaker
    // remains as backstop for whole-account drawdown.
    Money   node_peak_balance;         // peak of current_value over core's lifetime
    Money   node_dd_pct;               // current drawdown % (display field, recomputed each rebuild)
    // v5.15.5.B.3 — node_kill_tripped migrated to node_state_flags bitmap on
    // NodeContext HOT cluster (see node_state_flags above). _pad_kill[3]
    // eliminated as natural pad-collapse consequence — the kill trip is now
    // 1 bit in the bitmap word, not a byte with 3-byte alignment padding.
    uint32_t node_ks_trips_total;       // lifetime trip count (for forensics)

    // v4.0.3 D10: per-core P&L regression feeder for adaptive feedback.
    // Drainer pushes realized return on each exit; slow path reads slope
    // to shift resolved_cfg.entry_offset_pct + volume_multiplier within
    // [offset_min/max] + [vol_mult_min/max] bounds. Mirrors legacy
    // MeanReversion_Adapt / Momentum_Adapt regression-driven adaptation.
    RegressionFeederX<F> pnl_feeder;
    // v5.14.1.G — portfolio turnover diagnostic. Per-core; ephemeral
    // (NOT in the legacy PortfolioController snapshot fwrite path; sharded-only
    // state). Populated per slow-path cycle from weights_buf top-K.
    // Surfaced via PerNodeSnap.ml_portfolio_turnover. State + math
    // in ML_Headers/RollingTurnover.hpp.
    RollingTurnover turnover;
    // v5.10.0e — drift detection. Sampled post-fill (when
    // ConfidenceScorer_Update fires for ML cores). Engine emits CRITICAL
    // log on sustained-breach + optionally trips per-core kill_switch
    // when cfg.auto_kill_on_drift=1. See ConfidenceScore.hpp DriftHistory.
    DriftHistory drift_history;

    // ════════════════════════════════════════════════════════════════════
    // COLD CLUSTER — display-only / cross-thread / lifetime / boot.
    // (.B.2 will extract display-only fields → NodeContextDisplayMeta sibling
    // struct; .B.2 will also wrap sp_* atomics in alignas(64) sp_telemetry.)
    // ════════════════════════════════════════════════════════════════════

    // v5.1.1 + v5.1.3 (slow-path work breakdown): per-section profiling.
    // Same single-writer rule as slow_path_latency.
    //
    // Sections (label rework in v5.1.3 — earlier "Other" lumped the heavy
    // rolling-state pushes with trivial setup; now split honestly):
    //   0=ROLLING   — EventLoop_UpdateRollingStateOneCore + cadence setup
    //                 (depth read, swap pickup, mtm_price). DOMINATES the
    //                 cycle: 4 RollingStats pushes × O(W) FPN_Binary math, full-
    //                 window recompute. Expect ~100-300µs in steady state
    //                 with W=1024 baseline.
    //   1=REBUILD   — EventLoop_RebuildOneCore: regime classify + strategy
    //                 dispatch + gate compute. Expect 5-30µs.
    //   2=PUSH      — seqlock push of pending_params to ExecutionCore.
    //                 Expect ~100-500ns (single FPN_Binary copy + atomic).
    //   3=TIME_EXIT — EventLoop_TimeExitOneCore. Expect ~100-300ns.
    //   4=TRAIL_SL  — EventLoop_TrailingSLRatchetOneCore. Expect ~100-300ns.
    //
    // Sum-of-sections ≈ slow_path_latency total (within rdtsc bracket noise).
    // ~10ns _Sample × 5 = ~50ns/cycle overhead, < 0.1% of typical cycle.
    // v5.15.5.B.5 — SP_SECTION_* constants migrated to FOREACH_SP_SECTION
    // registry (CoreFrameworks/SpSectionRegistry.hpp). Use tt::SP_SECTION_<NAME>
    // directly at consumer sites. Back-compat aliases SP_SECTION_OTHER and
    // SP_SECTION_PUSH_PARAMS removed — callers now use the canonical names.
    // SP_SECTION_COUNT is now the registry-derived sentinel.

    // v5.0.3 (Engine Topology) live thread observability — wrapped in alignas(64)
    // SlowPathTelemetry struct (defined above) per cross-thread-snapshot-publish-
    // cluster-isolation.md. Single-writer = this core's slow-path thread (or
    // producer in centralized mode); reader = snapshot publisher (different
    // thread; relaxed atomics). alignas(64) on the struct itself isolates it
    // to its own cache line so publisher reads can't invalidate slow-path-
    // written neighbor fields. Field accessors:
    //   sp_telemetry.last_tick_us / cycles_total / yield_count / state
    SlowPathTelemetry sp_telemetry;

    // v5.15.5.B.2 — DisplayMeta fields (12 diag_*, observability counters,
    // cfg-drift state, model_load_failed/warmup_log_emitted booleans,
    // slow_path_latency + breakdown[]) EXTRACTED to NodeContextDisplayMeta<F>
    // sibling struct on EventLoopState. Per-cycle slow-path body no longer
    // pulls those ~9-10 KB of display-only data into HOT cluster L1 working
    // set. Access via state->display_meta[node_id].<field>; registry-driven
    // additions per MemHeaders/DisplayMetaRegistry.hpp.
    //
    // Booleans flagged "(.B.3 → node_state_flags bit)" in the registry will
    // migrate BACK to NodeContext as bitmap bits in v5.15.5.B.3 (uint8_t
    // node_state_flags); this temporary residence in DisplayMeta is the
    // .B.2 staging step. Per CLAUDE.md item 19 + Caramel 2026-05-13.
};

// v5.15.5.B.1 — NodeContext layout invariants. Lock the brittleness class:
// pre-v5.15.5.B.1 the alignas(64) was TRANSITIVE via embedded NodeLatencyStats
// (the `struct alignas(64) NodeLatencyStats` decl). A future refactor removing that alignas would
// silently break inter-slot false-sharing on the nodes[16] array. Explicit
// alignas + these static_asserts make the invariant LOCAL and compile-time-
// enforced. See cache-layout-discipline-for-hot-side-structs.md Rule 3+4 +
// decision-first-cluster-layout-pattern.md Step 5.
static_assert(sizeof(NodeContext<64>) % 64 == 0,
              "NodeContext size MUST be a multiple of 64B for inter-slot "
              "false-sharing prevention across nodes[MAX_EXECUTION_NODES]. "
              "Future field-insertion that violates this is a regression.");
static_assert(alignof(NodeContext<64>) >= 64,
              "NodeContext MUST be 64-byte aligned (now explicit via the "
              "`struct alignas(64)` declaration, NOT transitive via embedded "
              "NodeLatencyStats's alignas).");
// WARM cluster boundary anchor — entries_processed marks the per-event-cadence
// cluster start. Hot-cluster footprint is everything before this offset; warm-
// cluster everything between this and sp_last_tick_us; cold-cluster from there.
static_assert(offsetof(NodeContext<64>, entries_processed) % 64 == 0,
              "WARM cluster anchor MUST be 64-byte aligned. "
              "See decision-first-cluster-layout-pattern.md Step 5.");
// COLD cluster atomics boundary — sp_telemetry (SlowPathTelemetry struct)
// starts the cross-thread atomics block. The struct itself is alignas(64)
// so its placement is naturally aligned, but locking offsetof%64==0 protects
// against future field-insertion before sp_telemetry that might violate the
// alignment assumption (alignas only enforces that the struct ITSELF is
// 64-aligned; static_assert(offsetof%64==0) ensures cluster anchor remains
// at a cache line boundary within the enclosing struct).
static_assert(offsetof(NodeContext<64>, sp_telemetry) % 64 == 0,
              "Cross-thread atomics cluster (sp_telemetry) MUST start at a "
              "cache line boundary. See "
              "cross-thread-snapshot-publish-cluster-isolation.md.");
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// per-core controller-side state. one slot per registered execution core.
// the node_id field is implicit: nodes[i].core->node_id == i. holds the
// "intended" trade parameters that the controller wants applied at the next
// entry — phase 05 will replace this with a real parameter-slot push protocol,
// for phase 04 it's just a hot-update field the controller writes between
// entries and reads at entry time.
//
// statistics (entries_processed / exits_processed) are bumped from the drain
// loop, useful for monitoring per-core throughput in the TUI later.
//
// v5.15.5.B.1 — Fields grouped into HOT/WARM/COLD clusters by access frequency
// (cache-layout-discipline-for-hot-side-structs.md Rule 4); intra-cluster
// ordering is decision-first (decision-first-cluster-layout-pattern.md):
// gate_state bitmap at offset 0 for cycle-entry bail-out + forward-sequential
// subsequent fields for Intel/AMD stride-prefetcher friendliness.
//
// Explicit `struct alignas(64)` makes inter-slot false-sharing prevention
// LOCAL — previously TRANSITIVE via embedded NodeLatencyStats's alignas(64);
// a future refactor removing that would silently break the nodes[16] inter-
// slot guarantee. Static_asserts after the struct close lock the invariant
// at compile time.
//
// Safe to reorder: ShardedSnapshotPersist is field-by-field fwrite (no memcpy
// of struct bytes); no HMAC / SHA-256 / wire-format path uses raw NodeContext
// bytes; safety greps cleared 2026-05-12. CLAUDE.md item 27 (struct-padding
// determinism) explicitly NOT in scope — NodeContext is not in byte-
// equivalence path.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-14]
// [SIZE]_[7168B]
// [ALIGN]_[64]
// [CACHE_LINES]_[112]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[NodeContext]
//======================================================================

//======================================================================
// [STRUCT]_[WsHeartbeatTelemetry]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CONCURRENCY] [MONITORING_PLANE]]
// [THREAD]_[[PRODUCER_WRITER] [SLOW_READER]]
// [SYNC]_[ATOMIC]
// [REFERENCE]_[INVARIANT]_[H6]
// [REFERENCE]_[DESIGN_SPEC]_[cross-thread-snapshot-publish-cluster-isolation]
// [STRADDLE_EXEMPT]_[bucket_count]_[benign-by-design: single producer-writer on both lines; alignas(64) cluster isolation intact; 2-line footprint documented intentional — D-413/A-class C(a) 2026-08-10]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[WS heartbeat cluster on EventLoopState — producer-written tick liveness read by slow-path staleness gates + GUI; alignas(64) isolated]
//======================================================================
// [CODE]
//======================================================================
struct alignas(64) WsHeartbeatTelemetry {
    std::atomic<uint64_t> last_tick_us{0};
    std::atomic<uint64_t> ticks_per_5s{0};
    uint64_t              bucket_last_sec[5]{0, 0, 0, 0, 0};  // producer-only writer; not atomic
    uint32_t              bucket_count[5]{0, 0, 0, 0, 0};     // producer-only writer; not atomic
};
static_assert(alignof(WsHeartbeatTelemetry) == 64,
              "WsHeartbeatTelemetry MUST be cache-line aligned for cross-thread "
              "isolation. See cross-thread-snapshot-publish-cluster-isolation.md.");
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.B.2 — v5.12.1.A + v5.12.1.C WS heartbeat fields, wrapped in alignas(64) cluster
// for cross-thread cache-line isolation on EventLoopState. Single-writer is
// the producer thread (live) or backtest driver (offline). Readers: per-core
// slow paths (WS-staleness gate at slow-path entry) + snapshot publisher
// (GUI heartbeat display). alignas(64) prevents publisher reads from
// invalidating cache lines holding producer-written neighbor fields.
//
// Pattern: cross-thread-snapshot-publish-cluster-isolation.md (ND1 — sister
// application to SlowPathTelemetry on NodeContext). ~76 B used in 128 B
// (two cache lines); padding intentional for future heartbeat additions.
//
// Fields:
//   last_tick_us       — wall-clock us of last WS tick received
//   ticks_per_5s       — rolling tick count over last 5 seconds
//   bucket_last_sec[5] — second-tag of each rotating bucket (stale detection)
//   bucket_count[5]    — tick count in each bucket
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-10]
// [SIZE]_[128B]
// [ALIGN]_[64]
// [CACHE_LINES]_[2]
// [STRADDLE]_[bucket_count@56]
//======================================================================
// [END_STRUCT]_[WsHeartbeatTelemetry]
//======================================================================

//======================================================================
// [STRUCT]_[NodeContextDisplayMeta]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [DATA_ORIENTED_DESIGN]]
// [THREAD]_[[SLOW_WRITER] [PRODUCER_READER]]
// [STRADDLE_EXEMPT]_[slow_path_breakdown]_[element-uniform NodeLatencyStats record array (display/diag plane; per-element layout governed by its own block); name-sugar unresolvable only — D-414 leaf-3 2026-08-10]
// [REFERENCE]_[DESIGN_SPEC]_[[display-execution-invariant-registry-pattern] [cache-layout-discipline-for-hot-side-structs.md]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[registry-generated display-only sibling of NodeContext (parallel array by index) — keeps ~9-10KB of cold display data out of the HOT working set]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct NodeContextDisplayMeta {
    // ------------------------------------------------------------------
    // Gate-diagnostic actual/threshold pairs — auto-generated.
    // Adding a 7th pair = one row in FOREACH_GATE_DIAG_PAIR.
    // ------------------------------------------------------------------
#define X(FAMILY, ACTUAL_FIELD, OTHER_FIELD, _DOC) \
    FPN_Binary<F> diag_##ACTUAL_FIELD; \
    FPN_Binary<F> diag_##OTHER_FIELD;
    FOREACH_GATE_DIAG_PAIR(X)
#undef X

    // ------------------------------------------------------------------
    // Heterogeneous observability counters + edge-trigger state +
    // cfg-drift counters + boot booleans. Auto-generated; default-init
    // value per registry tuple.
    // ------------------------------------------------------------------
#define X(TYPE, NAME, INIT, _DOC) TYPE NAME = INIT;
    FOREACH_DISPLAY_META_FIELD(X)
#undef X

    // ------------------------------------------------------------------
    // v4.7.42 Phase E — per-core slow-path latency profiling. Mirrors
    // ExecutionCore::latency_stats (hot-path) for the slow-path.
    // Single-writer (this core's slow-path thread); single-reader
    // (snapshot publisher). Each NodeLatencyStats is alignas(64) so
    // arrays of them are 64-aligned by ABI.
    // ------------------------------------------------------------------
    NodeLatencyStats slow_path_latency;

    // v5.1.1 + v5.1.3 — per-section breakdown. Sections defined by
    // NodeContext<F>::SP_SECTION_* constants. (.B.5 — FOREACH_SP_SECTION
    // close removes the back-compat alias indirection.)
    NodeLatencyStats slow_path_breakdown[tt::SP_SECTION_COUNT];
};

// Init helper — zero-init all registry fields + Init the latency stats.
// Called once per core in EventLoopState_Init. Future field additions
// flow through the registry expansion automatically — no manual init
// per new field needed.
template <unsigned F>
inline void NodeContextDisplayMeta_Init(NodeContextDisplayMeta<F>* m) {
    // Gate-diagnostic FPN_Binary<F> pairs zeroed.
#define X(FAMILY, ACTUAL_FIELD, OTHER_FIELD, _DOC) \
    m->diag_##ACTUAL_FIELD = FPN_Zero<F>(); \
    m->diag_##OTHER_FIELD  = FPN_Zero<F>();
    FOREACH_GATE_DIAG_PAIR(X)
#undef X
    // Heterogeneous fields — reset to registry-defined init value.
    // (Default member-init covers boot-time; explicit reset here for
    // re-init paths + clarity.)
#define X(TYPE, NAME, INIT, _DOC) m->NAME = (TYPE)(INIT);
    FOREACH_DISPLAY_META_FIELD(X)
#undef X
    // Latency profiling — init zeros + disabled until engine explicitly enables.
    NodeLatencyStats_Init(&m->slow_path_latency);
    for (int s = 0; s < tt::SP_SECTION_COUNT; ++s) {
        NodeLatencyStats_Init(&m->slow_path_breakdown[s]);
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.B.2 — per-core display-only state, extracted from NodeContext per
// cache-layout-discipline-for-hot-side-structs.md Rule 1 (extract
// display-only fields off the HOT cluster of slow-path-cycled structs).
//
// These fields are WRITTEN by slow-path-body (gate-diag capture +
// observability counter increments + cfg-drift counters at boot) but
// READ ONLY by ShardedSnapshot publisher (ShardedSnapshot.hpp + entry
// log emission). They DO NOT influence per-cycle decision code.
//
// Parallel-array layout: EventLoopState has display_meta[MAX_EXECUTION_NODES]
// paired by index with nodes[] — meta for core i lives at display_meta[i].
// Separate storage = slow-path cycle's HOT cluster access doesn't pull
// display-meta cache lines into L1 unnecessarily; ~9-10 KB of cold data
// stays out of the HOT/WARM working set.
//
// Single-writer per core (slow-path thread for this core); single-reader
// per snapshot (publisher thread at ~30 Hz). Cross-thread accesses do
// not happen on per-cycle cadence so no alignas isolation is needed
// within DisplayMeta (homogeneous low-cadence access). The aggregate
// `EventLoopState::display_meta[MAX_EXECUTION_NODES]` array IS sized to
// be a multiple of 64 bytes via static_assert below.
//
// FIELDS ARE REGISTRY-GENERATED. See
// `MemHeaders/DisplayMetaRegistry.hpp` for the two FOREACH registries
// that drive every aspect of this struct:
//   FOREACH_GATE_DIAG_PAIR(X)   — 12 FPN_Binary<F> gate-diag fields (paired)
//   FOREACH_DISPLAY_META_FIELD(X) — 12 heterogeneous counters + flags
// To add a new field: append ONE row to the appropriate registry; the
// struct decl, init helper, snapshot publisher reads, etc. auto-flow.
// See DESIGN_SPECS/display-execution-invariant-registry-pattern.md (ND2).
//
// `slow_path_latency` + `slow_path_breakdown[]` are KEPT as direct
// fields because NodeLatencyStats has its own Init/Enable/Sample
// helpers + alignas(64) discipline; shoehorning them into a uniform
// registry shape would lose the type safety.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-24]
//----------------------------------------------------------------------
// [SIZE]_[13056B]
// [ALIGN]_[64]
// [CACHE_LINES]_[204]
// [STRADDLE]_[unverified: slow_path_breakdown]
//======================================================================
// [END_STRUCT]_[NodeContextDisplayMeta]
//======================================================================

//======================================================================
// [STRUCT]_[EventLoopState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CONCURRENCY]]
// [REFERENCE]_[INVARIANT]_[[H6] [H22]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the controller root — nodes[] + display_meta[] parallel arrays, event counters, the OMS back-pointer (ALL financial state routes through it), WS telemetry + engine-wide gate cache]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-13]
// [REFERENCE]_[DESIGN_SPEC]_[[bitmap-flag-api.md] [cache-layout-discipline-for-hot-side-structs.md] [cross-thread-snapshot-publish-cluster-isolation.md]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct alignas(64) EventLoopState {
    NodeContext<F> nodes[MAX_EXECUTION_NODES];
    // v5.15.5.B.2 — Per-core display-only state (extracted from NodeContext per
    // cache-layout-discipline-for-hot-side-structs.md Rule 1). Parallel array;
    // display_meta[i] is paired by index with nodes[i]. Fields are registry-
    // generated; see MemHeaders/DisplayMetaRegistry.hpp.
    NodeContextDisplayMeta<F> display_meta[MAX_EXECUTION_NODES];
    int registered_count;
    uint64_t total_events_processed;
    uint64_t total_entries;
    uint64_t total_exits;
    // v5.14.9.G — per-core partner-pending bitmap (TECH_DEBT-013 candidate 6).
    // Migrated from `uint8_t partner_pending_active` on each NodeContext (16 × 1 byte
    // = 16 bytes; plus the 16 × 7 bytes _pad_partner alignment padding = 128 bytes).
    // Now 1 bit per core in a single uint16_t = 2 bytes. Memory saved: ~126 bytes per
    // EventLoopState; better cache locality (single load to query any core's state).
    //
    // Bit N = core N's partner_pending_active. Set via BITMAP_SET / BITMAP_BIT_U16(N);
    // tested via BITMAP_IS_SET. See bitmap-flag-api.md for primitives.
    uint16_t partner_pending_bitmap;
    // Phase 03 chunk 1B: OMS back-pointer. MUST be non-null after Init —
    // all financial state reads go through oms->. EventLoopState_Init takes
    // the OMS pointer as its second argument and stores it here. callers that
    // pass nullptr will get crashes in any accessor or OnEvent call.
    OrderManagerState<F>* oms;
    // v5.12.1.A + v5.12.1.C — WS heartbeat telemetry. Wrapped in alignas(64)
    // WsHeartbeatTelemetry cluster (defined below) per
    // cross-thread-snapshot-publish-cluster-isolation.md (.B.2 v5.15.5).
    //
    // last_tick_us  — wall-clock us of last WS tick received. Single-writer
    //                 (producer thread in live, backtest driver in offline);
    //                 multiple-reader (per-core slow paths for the v5.12.1.A
    //                 WS-staleness emergency-flatten gate; GUI/TUI heartbeat
    //                 indicator). Initialized to 0; rises monotonically.
    //                 The slow-path check:
    //                   gap_us = now_us - last_tick_us;
    //                   if (gap_us >= cfg.ws_dead_time_flatten_threshold_secs * 1e6 &&
    //                       BITMAP_IS_SET(cfg.risk_cfg_flags, MASK_RISK_CFG_WS_DEAD_TIME_FLATTEN_ENABLED))
    //                       → OMS_FlattenAll(...)
    //                 Pre-warmup (last_tick_us == 0) is treated as "no flatten".
    // ticks_per_5s  — rolling tick count over last 5 seconds; producer fan_out
    //                 increments bucket_count for the current second; slow-path
    //                 / GUI sums all buckets within the last 5 seconds.
    // bucket_last_sec — second-tag of each bucket (so stale buckets don't
    //                 contribute). Single-writer (producer); single-reader.
    // bucket_count  — counts per bucket; producer-only writer.
    WsHeartbeatTelemetry ws_telemetry;
    // v5.14.9.B.0 — engine-wide slow-path gate cache (FOREACH_SLOW_PATH_GATE
    // ENGINE_WIDE entries: lazy_rebuild + ws_flatten today). Populated by
    // SLOW_PATH_GATE_AUTOPOPULATE_ENGINE_WIDE once per slow-path entry with
    // global cfg (no per-core resolution). Read via BITMAP_IS_SET at the
    // engine-wide use sites (function-entry of EventLoop_RebuildOneCore +
    // engine-wide outer ws-staleness check).
    GlobalGateState global_gate_state;
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// holds everything the controller core needs to process events from N
// execution cores. cores array is indexed by registration order; once a core
// is registered its slot index is its node_id forever (no compaction on
// unregister, just mark inactive).
//
// Phase 03 chunk 1B: all financial state (portfolio, balance, realized_pnl,
// fee_rate, kill switch, trade log) now lives in OrderManagerState. the OMS
// pointer is the ONLY path to reach them. EventLoopState is a thin dispatcher
// that owns per-core contexts, event counters, and the OMS back-pointer.
//
// total_events_processed is a heartbeat / liveness counter useful for
// detecting a stalled controller (TUI shows it growing each frame).
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-24]
// [SIZE]_[323840B]
// [ALIGN]_[64]
// [CACHE_LINES]_[5060]
// [STRADDLE]_[unverified: nodes display_meta]
//======================================================================
// [END_STRUCT]_[EventLoopState]
//======================================================================

}  // namespace tt
// v5.4.0 Phase 2.1 — Strategy_AdaptPerCore / Strategy_InitPerCore /
// Strategy_FreePerCore live in StrategyLifecycle.hpp. Included here
// (post-EventLoopState definition) so the dispatcher can refer to
// NodeContext / EventLoopState without an include cycle.
#include "../Strategies/StrategyLifecycle.hpp"
namespace tt {

}  // namespace tt
// v5.15.5.B.7 — NodeCtx init/reset registry + AUTOPOPULATE macros.
// Included AFTER NodeContext + EventLoopState + NodeContextDisplayMeta +
// all helper-Init declarations are visible so the templated helpers in
// the registry header can resolve every type + function they invoke.
#include "../MemHeaders/NodeCtxInitRegistry.hpp"
namespace tt {

//======================================================================
// [FUNCTION]_[EventLoopState_ReconstructPerCoreFromEventLog]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [BOOT_TIME] [CAPITAL_BEARING]]
// [REFERENCE]_[CLASS]_[18]
// [REFERENCE]_[INVARIANT]_[H20]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[replay the OrderEventLog into per-node attribution (entries/exits/realized/fees/notional/W-L) — idempotent; closes the snapshot-vs-replay Class-18 mirror (.F.2 Budget -100% bug)]
// [REFERENCE]_[DECISION]_[[D-190] [D-294]]
// [REFERENCE]_[DESIGN_SPEC]_[[branchless-dispatch-discipline.md] [structural-fix-preferred-decision-framework.md]]
//======================================================================
// [CODE]
//======================================================================
// Fwd-decl: Sharded_SlotNode is defined with its geometry family below; this
// early consumer precedes the definition (same tt namespace). (D-294)
static inline int Sharded_SlotNode(int slot, int partial_exit_enabled);

template <unsigned F>
inline void EventLoopState_ReconstructPerCoreFromEventLog(EventLoopState<F>* state,
                                                          const PerNodeCfg<F>* nodes = nullptr) {
    if (!state || !state->oms) return;
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — branchless cores-select: ONE cmov at entry; loop reads pure ALU.
    static const PerNodeCfg<F> NULL_PER_NODE_CFG_STUB_ARRAY[MAX_EXECUTION_NODES] = {};
    const PerNodeCfg<F>* effective_nodes = nodes ? nodes : NULL_PER_NODE_CFG_STUB_ARRAY;
    const tt::OrderEventLog<F>& log = state->oms->event_log;
    if (log.count == 0) return;  // no events → nothing to reconstruct (first boot)

    // Per-slot outstanding-entry tracker; transient (replay walk only).
    struct SlotEntry { Money entry_price; Money qty; Money entry_fee; int valid; };
    SlotEntry slot_entries[MAX_PORTFOLIO_POSITIONS] = {};
    // partial-exit flag (hoisted; drives slot→node via Sharded_SlotNode). (D-294)
    const int partial_on = BITMAP_IS_SET(state->oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);

    for (size_t i = 0; i < log.count; ++i) {
        const tt::OrderEvent<F>& e = log.entries[i];
        if (e.type != tt::OEVT_FULL_FILL) continue;
        int slot = (int)e.node_id;
        if (slot < 0 || slot >= MAX_PORTFOLIO_POSITIONS) continue;
        // slot → owning node: legs A/B at 2c/2c+1 → core c under partials; slot==core
        // single-mode. (was: ungated `slot>>1` — HALVED the node in single mode; D-294.)
        int node_id = Sharded_SlotNode(slot, partial_on);
        if (node_id < 0 || node_id >= MAX_EXECUTION_NODES) continue;

        // v5.15.5.F.4c.3 WIP2d-1.B.1 — branchless read via effective_nodes (hoisted above loop).
        const Money fee_rate_taker_for_core = effective_nodes[node_id].fee_rate_taker;
        if (e.order_type == tt::ORDER_MARKET_BUY) {
            Money notional  = Money_Mul(e.price, e.qty);
            Money entry_fee = Money_Mul(notional, fee_rate_taker_for_core);
            slot_entries[slot].entry_price = e.price;
            slot_entries[slot].qty         = e.qty;
            slot_entries[slot].entry_fee   = entry_fee;
            slot_entries[slot].valid       = 1;
            state->nodes[node_id].entries_processed++;
            state->nodes[node_id].node_open_notional =
                Money_Add(state->nodes[node_id].node_open_notional, notional);
        } else if (e.order_type == tt::ORDER_MARKET_SELL) {
            if (!slot_entries[slot].valid) continue;  // unmatched SELL — skip
            Money entry_price  = slot_entries[slot].entry_price;
            Money qty          = slot_entries[slot].qty;
            Money entry_fee    = slot_entries[slot].entry_fee;
            Money exit_notional= Money_Mul(e.price, qty);
            Money exit_fee     = Money_Mul(exit_notional, fee_rate_taker_for_core);  // per-node via cores param
            Money total_fee    = Money_Add(entry_fee, exit_fee);
            Money gross        = Money_FillGross(entry_price, e.price, qty);  // D-190 single-source
            Money net          = Money_Sub(gross, total_fee);
            state->nodes[node_id].exits_processed++;
            state->nodes[node_id].node_realized =
                Money_Add(state->nodes[node_id].node_realized, net);
            state->nodes[node_id].node_fees     =
                Money_Add(state->nodes[node_id].node_fees, total_fee);
            Money entry_notional = Money_Mul(entry_price, qty);
            state->nodes[node_id].node_open_notional =
                Money_Sub(state->nodes[node_id].node_open_notional, entry_notional);
            uint32_t is_win = (uint32_t)Money_Gt(net, Money_Zero());
            state->nodes[node_id].node_wins   += is_win;
            state->nodes[node_id].node_losses += (1u - is_win);
            slot_entries[slot].valid = 0;  // slot freed for next match
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.F.2 — walks state->oms->event_log + reconstructs per-core attribution state
// (entries_processed, exits_processed, node_realized, node_fees, node_open_
// notional, node_wins, node_losses). Idempotent + safe to call multiple
// times on the same log content; expects per-core fields to be zero-init'd
// before call (NODE_CTX_INIT_AUTOPOPULATE handles that).
//
// Closes the Class-18 mirror between:
//   - ShardedSnapshot_Load (restores per-core fields field-by-field)
//   - Portfolio_FromEventLog (restores ONLY OMS-level state; per-core fields
//     left at Init defaults — caused .F.2 Budget -100% display bug + drainer
//     risk gate misbehavior).
//
// Per the structural-fix-preferred gradient (structural fix preferred when a bug class can recur)
// + DESIGN_SPECS/structural-fix-preferred-decision-framework.md.
//
// Algorithm:
//   1. Walk events in order; track per-slot "outstanding entry" record.
//   2. BUY event → record (price, qty, entry_fee) for slot + bump entries_-
//      processed + add notional to node_open_notional.
//   3. SELL event → match against slot's outstanding entry → compute gross +
//      net + fees → bump exits_processed + accumulate node_realized + fees +
//      wins/losses + subtract entry_notional from node_open_notional.
//
// v5.15.5.F.4c.3 WIP2d-1.B.1 — `cores` param added (nullptr-tolerant) for per-core fee_rate
// during replay. Per cfg-scope-discipline § "consumer over per-core array"; caller passes
// `cfg.nodes`. Recovery-path nullable semantic per Decision 2 — nullptr → FPN_Zero fees in
// reconstructed accounting (acceptable for legacy replay; primary recovery is HandleFill).
//
// Branchless nullptr handling per H20 + branchless-dispatch-discipline.md Pattern 3:
// hoist a SINGLE cmov at fn entry to select effective_nodes; loop body reads
// effective_nodes[idx].field with zero per-iteration branches.
//======================================================================
// [END_FUNCTION]_[EventLoopState_ReconstructPerCoreFromEventLog]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoopState_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[zero the dispatcher, install the OMS back-pointer, NODE_CTX_INIT_AUTOPOPULATE every slot, then reconstruct per-node attribution from any replayed event log; InitLegacy (tests) + Free share the section]
// [REFERENCE]_[DESIGN_SPEC]_[structural-fix-preferred-decision-framework.md]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EventLoopState_Init(EventLoopState<F>* state,
                                OrderManagerState<F>* oms) {
    state->registered_count = 0;
    state->total_events_processed = 0;
    state->total_entries = 0;
    state->total_exits = 0;
    state->oms = oms;
    // v5.14.9.G — partner_pending bitmap (1 bit per core; 0 = no partner pending)
    state->partner_pending_bitmap = 0;
    // v5.12.1.A — pre-warmup sentinel; first tick from producer/backtest
    // sets it to a monotonic wall-clock us value.
    // v5.15.5.B.2 — wrapped in WsHeartbeatTelemetry alignas(64) cluster.
    state->ws_telemetry.last_tick_us.store(0, std::memory_order_relaxed);
    // v5.12.1.C — heartbeat throughput tracking init.
    state->ws_telemetry.ticks_per_5s.store(0, std::memory_order_relaxed);
    for (int i = 0; i < 5; ++i) {
        state->ws_telemetry.bucket_last_sec[i] = 0;
        state->ws_telemetry.bucket_count[i] = 0;
    }
    // v5.15.5.B.7 — Per-slot init via NODE_CTX_INIT_AUTOPOPULATE companion
    // macro. ~50 lines of per-field init / helper-Init calls / sp_telemetry
    // atomic stores / slow_state arena allocation / display_meta sibling init
    // are now covered by one macro call. Adding a new NodeContext field that
    // needs boot-init = ONE row in FOREACH_NODE_CTX_FIELD; macro picks
    // it up at next compile. See MemHeaders/NodeCtxInitRegistry.hpp.
    for (int i = 0; i < MAX_EXECUTION_NODES; i++) {
        NODE_CTX_INIT_AUTOPOPULATE(state, i);
    }

    // v5.15.5.F.2 — reconstruct per-core attribution from any events that were
    // replayed during OMS AUTOPOPULATE. Closes the Class-18 mirror between
    // snapshot-restore (ShardedSnapshot restores per-core fields directly) and
    // OrderEventLog-replay (which previously only restored OMS-level state +
    // Portfolio, leaving per-core fields at Init defaults). Without this:
    //   - node_open_notional starts at 0 after replay
    //   - First live exit's FPN_SubSat(0, entry_notional) yields NEGATIVE
    //     (FPN_SubSat is magnitude-saturating, NOT zero-floored; an earlier
    //      notional-subtract comment in this file was incorrect about zero-saturation)
    //   - Display shows "Budget -100%" + drainer risk gates (Phase 2.2 when
    //     enforced) misbehave on stale per-core notional state.
    //   - entries_processed/exits_processed unbalanced → node_open_positions
    //     wraparound (uint64 underflow → huge uint32_t).
    // Per the structural-fix-preferred gradient (structural fix preferred when
    // a bug class can recur) + DESIGN_SPECS/structural-fix-preferred-decision-framework.md.
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — no cfg available at boot Init context; pass nullptr.
    // Replayed fees default to zero; caller can re-run with cores via the public sig if needed.
    EventLoopState_ReconstructPerCoreFromEventLog(state);
}

//------------------------------------------------------------------------------
// [SECTION]_[INIT LEGACY — convenience for tests]
//------------------------------------------------------------------------------
// test helper that creates an OMS + wires it into the EventLoopState in one
// call. the caller provides the OMS on the stack alongside the state. uses a
// default-constructed (zeroed) ExchangeAdapter and live_trading=0 (paper mode).
//------------------------------------------------------------------------------
template <unsigned F>
inline void EventLoopState_InitLegacy(EventLoopState<F>* state,
                                       OrderManagerState<F>* oms,
                                       Money starting_balance) {
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — `fee_rate` param DELETED. OMS no longer holds scalar fee_rate;
    // per-Order pre_resolved.fee_rate set at submit via Order_BindPreResolved with cfg.nodes[c].
    // Test fixtures that need non-zero fee accounting must populate cfg.nodes[c].fee_rate_*.
    ExchangeAdapter<F> empty{};
    OrderManager_Init(oms, empty, 0, /*partial_exit_enabled=*/0, starting_balance);
    EventLoopState_Init(state, oms);
}

//------------------------------------------------------------------------------
// [SECTION]_[FREE — v5.1.0]
//------------------------------------------------------------------------------
// Free heap-allocated NodeSlowState pointers. Idempotent: safe to call
// twice (NULLs the pointer). Tests + engine that init via
// EventLoopState_Init / EventLoopState_InitLegacy must call this on
// teardown to avoid LeakSanitizer noise.
//
// v5.4.0 IMPORTANT: caller MUST also call `Strategy_FreePerCore` (from
// Strategies/StrategyLifecycle.hpp) for each slot BEFORE this function,
// to release per-strategy state. We don't dispatch to Strategy_FreePerCore
// here because doing so would create a circular include
// (ControllerEventLoop.hpp ↔ StrategyLifecycle.hpp). The contract is:
//
//   for (int c = 0; c < state.registered_count; ++c)
//       Strategy_FreePerCore(&state, c);
//   EventLoopState_Free(&state);
//
// Forgetting Strategy_FreePerCore = leak of strategy state structs
// (~few KB per core). LeakSanitizer flags it.
//======================================================================================================
template <unsigned F>
inline void EventLoopState_Free(EventLoopState<F>* state) {
    // v5.11.26 NOTE: writer thread cleanup happens at OrderManagerState's
    // destructor (RAII at scope exit, ~OrderManagerState in OrderManager.hpp). Don't stop
    // the writer here — the test might still use `oms` after this Free
    // returns; premature stop would race with subsequent OMS work.
    for (int i = 0; i < MAX_EXECUTION_NODES; ++i) {
        if (state->nodes[i].slow_state) {
            // v5.11.6.A — if the arena owns this allocation, it's freed
            // by InitArena_Destroy at engine shutdown — skip delete here.
            // Otherwise (test path / no arena), `new` allocated it and
            // we delete normally.
            //
            // Placement-new'd objects need explicit destructor call before
            // the arena reclaims their memory, but NodeSlowState is
            // trivially destructible (no pointers it owns; all FPN_Binary +
            // POD). For non-trivial types, add a manual ->~NodeSlowState<F>()
            // here when the arena is in use.
            if (!tt::InitArena_Owns(tt::InitArena_Global(),
                                     state->nodes[i].slow_state)) {
                delete state->nodes[i].slow_state;
            }
            state->nodes[i].slow_state = nullptr;
        }
        // strategy_state is freed by caller via Strategy_FreePerCore
        // before this function (see contract above). If it's still
        // non-null here, it's a leak — log to help catch the missing
        // call. Continue rather than dispatch (we don't know the kind
        // here without the strategy headers).
        if (state->nodes[i].strategy_state) {
            fprintf(stderr,
                "[EventLoopState_Free] WARN: slot %d has non-null strategy_state "
                "kind=%u — caller forgot to call Strategy_FreePerCore. Leaking.\n",
                i, state->nodes[i].strategy_state_kind);
            state->nodes[i].strategy_state = nullptr;  // prevent dangling
            state->nodes[i].strategy_state_kind = 0xFF;
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// zero the dispatcher state, install the OMS back-pointer. all financial
// state (portfolio, balance, fee_rate, kill switch, trade log) lives in the
// OMS — EventLoopState just holds per-core contexts and event counters.
// the OMS must be initialized via OrderManager_Init BEFORE calling this.
//======================================================================
// [END_FUNCTION]_[EventLoopState_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoopState_RegisterCore]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[claim the next slot for an execution core — slot index IS node_id forever; -1 when full]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int EventLoopState_RegisterCore(EventLoopState<F>* state,
                                       ExecutionCore<F>* core,
                                       Money intended_tp,
                                       Money intended_sl,
                                       Money intended_qty) {
    if (state->registered_count >= MAX_EXECUTION_NODES) return -1;
    int slot = state->registered_count++;
    state->nodes[slot].core         = core;
    state->nodes[slot].intended_tp  = intended_tp;
    state->nodes[slot].intended_sl  = intended_sl;
    state->nodes[slot].intended_qty = intended_qty;
    state->nodes[slot].entries_processed = 0;
    state->nodes[slot].exits_processed   = 0;
    core->node_id = (uint16_t)slot;
    return slot;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// claim a slot for this execution core. node_id is set to the slot index so
// future events from this core route to the right portfolio slot.
//
// returns the assigned slot (== node_id), or -1 if MAX_EXECUTION_NODES is full.
//
// the intended_tp / intended_sl / intended_qty parameters are the trade
// parameters the controller wants to apply on the next entry. phase 05 will
// allow the controller to update them between entries via a separate push
// protocol; for phase 04 they're set once at registration and reused.
//======================================================================
// [END_FUNCTION]_[EventLoopState_RegisterCore]
//======================================================================

//======================================================================
// [FUNCTION]_[Sharded_SlotNode]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [DATA_ORIENTED_DESIGN]]
// [REFERENCE]_[INVARIANT]_[H20]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the slot<->node geometry family — Sharded_LegSlot / NodeSlotMask / SlotNode (THE canonical slot->node accessor, pre-commit Check O enforced) / ValidatePartialExitCfg share this section]
// [REFERENCE]_[DECISION]_[[D-294] [D-295] [D-296]]
// [REFERENCE]_[PLAN]_[partial-exits-sharded.md]
//======================================================================
// [CODE]
//======================================================================
// Per `plans/partial-exits-sharded.md` (P.1, 2026-04-27): when
// `cfg.partial_exit_enabled=1`, each core owns TWO portfolio slots — one
// for leg A (first half exits at TP1), one for leg B (second half rides
// to TP2 or shared SL). When disabled, each core owns ONE slot at index
// == node_id (legacy single-position behavior).
//
// Slot layout with partials:
//   core 0 → slots {0, 1}  (leg A=0, leg B=1)
//   core 1 → slots {2, 3}
//   core 2 → slots {4, 5}
//   ...
//
// Without partials:
//   core c → slot {c}      (leg index ignored)
//
// CAPACITY: with partials enabled, max cores caps at MAX_PORTFOLIO_POSITIONS
// / 2 = 8 (assuming MAX_PORTFOLIO_POSITIONS=16). Boot-time validation
// refuses to start if num_execution_nodes × 2 > MAX_PORTFOLIO_POSITIONS.
//
// LEG INDICES — the constants live in CoreFrameworks/TradeEvent.hpp so
// ExecutionCore_Tick (which doesn't include this header) can use the same
// names. Re-stated here as a comment for readability:
//   PARTIAL_LEG_A = 0
//   PARTIAL_LEG_B = 1

// Returns the portfolio slot index for (node_id, leg) given the cfg.
// leg=0 always returns a valid slot; leg=1 returns -1 when partial_exit_-
// enabled=0. Caller-side: ignore leg=1 result when partials disabled.
//
// All slow-path / boot-time. Trivially inlined.
static inline int Sharded_LegSlot(int node_id, int leg, int partial_exit_enabled) {
    if (node_id < 0) return -1;
    if (!partial_exit_enabled) {
        // Single-slot mode: leg index ignored, slot == node_id
        return (leg == PARTIAL_LEG_A) ? node_id : -1;
    }
    // Pair mode: leg A = 2c, leg B = 2c+1
    if (leg != PARTIAL_LEG_A && leg != PARTIAL_LEG_B) return -1;
    int slot = node_id * 2 + leg;
    if (slot >= MAX_PORTFOLIO_POSITIONS) return -1;
    return slot;
}

// Build the bitmap mask for core c's portfolio slot(s). Under partials,
// core c owns slots 2c and 2c+1 (legs A and B). Without partials,
// core c owns just slot c. Used by slow-path checks that ask "is core
// c currently in any position?" or "what portfolio slots does this
// core occupy?".
static inline uint16_t Sharded_NodeSlotMask(int node_id, int partial_exit_enabled) {
    // 2026-08-22 — the mask SHAPE now delegates to BITMAP_NODE_SLOT_MASK
    // (MemHeaders/BitmapMacros.hpp), the raw-shape SSoT extracted from 12
    // open-coded copies; this fn stays the BOUNDS-CHECKED wrapper (the raw
    // macro is deliberately unchecked — its historical sites are loop-bounded).
    if (node_id < 0 || node_id >= MAX_PORTFOLIO_POSITIONS) return 0;
    if (partial_exit_enabled && (node_id * 2 + 1) >= MAX_PORTFOLIO_POSITIONS) return 0;
    return BITMAP_NODE_SLOT_MASK(node_id, partial_exit_enabled);
}

// Inverse of Sharded_LegSlot: the logical node that owns portfolio slot `slot`.
// Branchless (H20): partial_on ∈ {0,1} IS the shift count — slot>>1 under partials (legs
// A/B at 2c/2c+1 → core c), slot>>0 = slot single-slot. No cmov (matches the tuned
// TrailingSLRatchet site below).
// Caller contract: slot ≥ 0 (a valid portfolio slot); callers that may hold -1 guard first
// (e.g. the ReconstructPerCoreFromEventLog walk above). THE single source for slot→node —
// replaces the open-coded shifts that lived in EngineSharded/SlowPath.hpp,
// ShardedSnapshotPersist.hpp (one was UNGATED = the bug), the TrailingSLRatchet site below,
// and ShardedSnapshot.hpp. GUI sites grandfathered for the E-series decouple. (D-294/D-295)
static inline int Sharded_SlotNode(int slot, int partial_exit_enabled) {
    // s5-1b (2026-08-23): shape delegated to the raw macro (BitmapMacros.hpp) so
    // non-CEL consumers (ShardedTradeLog's attribution derive) share ONE impl —
    // the same two-tier raw-macro/checked-wrapper split as Sharded_NodeSlotMask.
    return BITMAP_SLOT_NODE(slot, partial_exit_enabled);  // SLOT_DERIVE_OK: THE canonical accessor (D-296)
}

// Boot-time validation. Returns 1 if cfg + capacity are consistent, 0
// otherwise (and prints the reason to stderr). Call from engine startup
// AFTER cfg load, BEFORE core registration.
//
// Failure modes:
//   - partial_exit_enabled=1 AND num_execution_nodes * 2 > MAX_PORTFOLIO_POSITIONS
//   - num_execution_nodes < 1 (caller should already validate)
//   - partial_exit_pct outside (0.0, 1.0) when partials enabled
template <unsigned F>
static inline int Sharded_ValidatePartialExitCfg(const ControllerConfig<F>* cfg) {
    if (!BITMAP_IS_SET(cfg->lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED)) return 1;  // disabled = always valid
    int n_nodes = (int)cfg->num_execution_nodes;
    if (n_nodes < 1) {
        std::fprintf(stderr,
            "[partial-exits] num_execution_nodes=%d invalid; needs >= 1\n",
            n_nodes);
        return 0;
    }
    int max_pair_nodes = MAX_PORTFOLIO_POSITIONS / 2;
    if (n_nodes > max_pair_nodes) {
        std::fprintf(stderr,
            "[partial-exits] partial_exit_enabled=1 caps num_execution_nodes "
            "at %d (got %d). Each node uses TWO portfolio slots in pair mode "
            "(leg A + leg B); MAX_PORTFOLIO_POSITIONS=%d → %d nodes max.\n"
            "  Either: (a) reduce num_execution_nodes to %d or fewer, or\n"
            "          (b) set partial_exit_enabled=0 in your cfg.\n",
            max_pair_nodes, n_nodes, MAX_PORTFOLIO_POSITIONS, max_pair_nodes,
            max_pair_nodes);
        return 0;
    }
    double pct = Money_ToDouble(cfg->partial_exit_pct);
    if (pct <= 0.0 || pct >= 1.0) {
        std::fprintf(stderr,
            "[partial-exits] partial_exit_pct=%.4f invalid; needs (0, 1) "
            "exclusive. 0.5 = exit half at TP1, half rides to TP2.\n",
            pct);
        return 0;
    }
    std::fprintf(stderr,
        "[partial-exits] enabled: %d nodes using %d slots (legs A+B), "
        "TP1 exits %.0f%% of qty\n",
        n_nodes, n_nodes * 2, pct * 100.0);
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Sharded_SlotNode]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoopState_SetCoreStrategy]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[assign strategy + risk budget to a registered node; STRATEGY_NONE = safe-disabled (P6.5)]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-160]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EventLoopState_SetCoreStrategy(EventLoopState<F>* state, int slot,
                                            uint8_t strategy_id,
                                            Money allocated_balance) {
    // The MAX_EXECUTION_NODES clause is compiler-provable bound hygiene (TECH_DEBT-160): the
    // registered_count invariant (<= MAX_EXECUTION_NODES, enforced at registration) already bounds
    // slot at runtime, but value-range analysis can't see it -> gui-lane -Wstringop-overflow FP.
    // Never fires alone; boot/setup cadence (not hot path).
    if (slot < 0 || slot >= state->registered_count || slot >= MAX_EXECUTION_NODES) return;
    state->nodes[slot].strategy_id       = strategy_id;
    // s5-1b (2026-08-23): seed the resolved id at registration. It is re-stashed
    // every rebuild (the v4.0.4 site), but a warm-restart exit can fill BEFORE the
    // first rebuild — without this seed such a fill would attribute as id 0 (= MR).
    // AUTO nodes honestly show the AUTO sentinel until first regime resolution.
    state->nodes[slot].resolved_strategy_id = strategy_id;
    state->nodes[slot].allocated_balance = allocated_balance;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// assign a strategy to a registered core. the strategy_id is used by
// Strategy_BuildParameters (phase 06+) to dispatch to the right strategy's
// _BuildParameters function during slow-path parameter rebuilds.
//
// STRATEGY_NONE disables the core (parameter pushes are skipped, the
// execution core's permission stays at 0). this is the safe default — see
// pitfall P6.5 (cores running undefined strategies).
//
// allocated_balance is the share of total balance this core is permitted to
// risk on a single trade. set by Regime_AllocateCores (phase 06+). units are
// FPN_Binary<F> currency, not percentage.
//======================================================================
// [END_FUNCTION]_[EventLoopState_SetCoreStrategy]
//======================================================================

//------------------------------------------------------------------------------
// [SECTION]_[wiring + bank accessors — attach/read-through helpers (trivial, unblocked)]
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// [SECTION]_[ATTACH TRADE LOG — phase 08]
//------------------------------------------------------------------------------
// install an optional CSV trade log. pass nullptr to disable. ownership stays
// with the caller (the engine main usually); EventLoop never opens or closes
// the log. attach is idempotent — calling twice replaces the previous pointer.
//
// pitfall P8.2: only the controller core writes to this log. don't share the
// pointer with GUI or other threads. if the GUI wants to display trades it
// must read the CSV file from disk, never call the recorder directly.
//------------------------------------------------------------------------------
template <unsigned F>
inline void EventLoopState_AttachTradeLog(EventLoopState<F>* state,
                                          ShardedTradeLog* log) {
    state->oms->trade_log = log;
    // v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer wire-to-real.
    state->oms->on_entry_fill_emit = &tt::real_on_entry_fill_emit<F>;
    state->oms->on_exit_fill_emit  = &tt::real_on_exit_fill_emit<F>;
}

//------------------------------------------------------------------------------
// [SECTION]_[ATTACH OMS — phase 03 chunk 1B]
//------------------------------------------------------------------------------
// Replace the OMS pointer. Mainly useful for re-wiring after Init if the
// OMS was constructed separately (e.g. EngineSharded.hpp pre-1B callers).
// EventLoopState_Init already wires the OMS pointer, so most call sites
// don't need this anymore.
//------------------------------------------------------------------------------
template <unsigned F>
inline void EventLoopState_AttachOms(EventLoopState<F>* state,
                                      OrderManagerState<F>* oms) {
    state->oms = oms;
}

//------------------------------------------------------------------------------
// [SECTION]_[BANK ACCESSORS — phase 03 chunk 1B]
//------------------------------------------------------------------------------
// Forwarding accessors for the financial fields. All forward to state->oms->
// since the bank state now lives in OrderManagerState.
//
// Convention: each accessor is named EventLoopState_<Field> and takes a
// const-pointer for read accessors, a non-const-pointer for mutators.
// Mutators are intentionally limited to the small set of operations
// EventLoop code actually performs (Init/AttachTradeLog/Configure/Trip/etc.) —
// callers should not freely write to balance/portfolio from the outside.
//======================================================================================================
template <unsigned F>
inline FPN_Binary<F> EventLoopState_Balance(const EventLoopState<F>* state) {
    return state->oms->balance;
}

template <unsigned F>
inline FPN_Binary<F> EventLoopState_RealizedPnl(const EventLoopState<F>* state) {
    return state->oms->realized_pnl;
}

// v5.15.5.F.4c.3 WIP2d-1.B.1 — EventLoopState_FeeRate getter DELETED.
// Was a proxy for state->oms->fee_rate which is also being deleted at r-5. Zero production
// callers (verified via codebase grep at r-2 audit). Per-core fee rates now live on
// cfg.nodes[c].fee_rate_taker / fee_rate_maker; HandleFill reads Order::pre_resolved.fee_rate
// (pre-resolved at submit via Order_BindPreResolved). No getter needed at this scope.

template <unsigned F>
inline const Portfolio<F>* EventLoopState_Portfolio(const EventLoopState<F>* state) {
    return &state->oms->portfolio;
}

template <unsigned F>
inline Portfolio<F>* EventLoopState_PortfolioMut(EventLoopState<F>* state) {
    return &state->oms->portfolio;
}

template <unsigned F>
inline FPN_Binary<F> EventLoopState_KsMinBalance(const EventLoopState<F>* state) {
    return state->oms->ks_min_balance;
}

template <unsigned F>
inline FPN_Binary<F> EventLoopState_KsMaxDrawdownPct(const EventLoopState<F>* state) {
    return state->oms->ks_max_drawdown_pct;
}

template <unsigned F>
inline FPN_Binary<F> EventLoopState_KsPeakBalance(const EventLoopState<F>* state) {
    return state->oms->ks_peak_balance;
}

template <unsigned F>
inline uint8_t EventLoopState_KillSwitchTripped(const EventLoopState<F>* state) {
    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    return (uint8_t)BITMAP_IS_SET(state->oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED);
}

template <unsigned F>
inline uint64_t EventLoopState_KsTripsTotal(const EventLoopState<F>* state) {
    return state->oms->ks_trips_total;
}

template <unsigned F>
inline ShardedTradeLog* EventLoopState_TradeLog(const EventLoopState<F>* state) {
    return state->oms->trade_log;
}

//------------------------------------------------------------------------------
// [SECTION]_[SET INTENDED PARAMS]
//------------------------------------------------------------------------------
// for phase 04, the controller updates the intended TP/SL/qty for a core
// directly through this helper. phase 05 replaces it with an atomic
// ParameterSlot push that races safely against the execution core. for now
// it's just a write to the controller-side struct, no synchronization needed
// because the execution core never reads from NodeContext (only from its own
// gate_params, which we DO NOT touch from this function — see P4.7).
//------------------------------------------------------------------------------
template <unsigned F>
inline void EventLoopState_SetIntendedParams(EventLoopState<F>* state, int slot,
                                              FPN_Binary<F> tp, FPN_Binary<F> sl, Money qty) {
    if (slot < 0 || slot >= state->registered_count) return;
    state->nodes[slot].intended_tp  = tp;
    state->nodes[slot].intended_sl  = sl;
    state->nodes[slot].intended_qty = qty;
}

//======================================================================
// [FUNCTION]_[EventLoop_DrainPostFillOneCore]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[mode-1 per-node stats consumer — FillRecords -> node accounting + ConfidenceScorer/pnl_feeder/SL-cooldown; PER-LEG vs PER-TRADE split is load-bearing under partials (v4.7.4 IC-contamination fix)]
// [REFERENCE]_[CLASS]_[[24] [25]]
// [REFERENCE]_[DECISION]_[D-190]
// [REFERENCE]_[PARITY]_[PARITY-2]
// [REFERENCE]_[DESIGN_SPEC]_[[bitmap-flag-api.md] [decision-time-data-binding-pattern.md] [phase-separated-drainer-for-safe-cross-temporal-derives.md] [slot-state-foreach-registry-with-storage-routing.md]]
//======================================================================
// [CODE]
//======================================================================
// E.1.2.C leg 0 (2026-08-20) — signature SHRUNK + FULLY DE-DEFAULTED.
// The v5.14.1.F mid-signature insert of confidence_ic_variant silently
// re-mapped every later positional binding at the fan (the cfg exit flag
// landed in the IC-variant slot; the 0.001 fee literal truncated into the
// int enable flag = permanently 0) — the exit-bandit reward update was
// unreachable on every production path from f973b5c until this fix, and
// enabling exit_bandit_enabled=1 in live would have poisoned the drift-IC
// channel instead. Closure is structural: NO defaulted parameters remain,
// so any future arity change is a compile error at every caller; and the
// two per-node facts (exit-bandit enable, counterfactual fee) are no
// longer parameters at all — they derive from node_cfg AT POINT OF USE
// (decision-time binding; completes the WIP2d-1.B.1 fee-param deletion
// this file's SlowPath binder comment recorded but never executed).
template <unsigned F>
inline void EventLoop_DrainPostFillOneCore(EventLoopState<F>* state,
                                             OrderManagerState<F>* oms,
                                             uint32_t sl_cooldown_cycles,
                                             int node_id,
                                             double ensemble_trade_reward_mult,
                                             // v5.10.0e — runtime IC drift detection.
                                             // floor=0 → DriftHistory_CheckBreach skip
                                             // (avg < 0 floor never fires).
                                             double drift_floor,
                                             uint32_t drift_window_seconds,
                                             int      drift_auto_kill,
                                             // v5.14.1.F — IC variant selector for
                                             // drift detection (0 = Spearman).
                                             int      confidence_ic_variant,
                                             // v5.15.5.F.4c.3 WIP2d-1.B.1 — per-core cfg slice.
                                             // Per cfg-scope-discipline § "consumer function signatures over per-core slices"
                                             // — single-slice form (this fn is single-core-scoped via the `node_id` param).
                                             // nullptr = per-node ML extras OFF (test callers): exit-bandit
                                             // attribution derives from node_cfg below, so nullptr disables it.
                                             // common_type wrapper = NON-DEDUCED context so a bare `nullptr`
                                             // argument compiles (F deduces from state/oms alone).
                                             const typename std::common_type<PerNodeCfg<F>>::type* node_cfg) {
    // E.1.2.C A-2 — belt behind the registry clamp: an out-of-range variant
    // would make ConfidenceScorer_ComputeICVariant return constant 0.0 and
    // poison the drift-breach channel (spurious auto-kill). Cfg parse already
    // WARN_ON_CLAMPs to the registry max; this line keeps raw callers safe
    // without a log (boot already surfaced any clamp).
    if ((unsigned)confidence_ic_variant >= (unsigned)(FOREACH_IC_VARIANT_COUNT))
        confidence_ic_variant = 0;
    // E.1.2.C leg 0 — derived at point of use, not a parameter (see header
    // comment). node_cfg==nullptr ⇒ disabled, which also guarantees node_cfg
    // is NON-NULL anywhere below that is gated on exit_bandit_enabled.
    const int exit_bandit_enabled =
        (node_cfg != nullptr) &&
        BITMAP_IS_SET(node_cfg->ml_cfg_flags, MASK_ML_CFG_EXIT_BANDIT_ENABLED);
    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    const int partial_on = BITMAP_IS_SET(oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
    uint16_t my_mask = BITMAP_NODE_SLOT_MASK(node_id, partial_on);
    const int max_slot = partial_on ? state->registered_count * 2
                                    : state->registered_count;

    NodeContext<F>& ctx = state->nodes[node_id];

    // v5.4.1 Bug B diagnostic: log when this core has bits to process.
    // Cheap (cfg-gated, no-op when disabled). Helps narrow whether the
    // mask plumbing or the increment math is the cause of stuck-zero
    // per-core counters.
    {
        uint16_t hit_open  = (uint16_t)(oms->last_opened_mask & my_mask);
        uint16_t hit_close = (uint16_t)(oms->last_closed_mask & my_mask);
        if ((hit_open || hit_close) && tt::Health_LogEnabled(tt::HEALTH_INFO)) {
            tt::Health_Log(tt::HEALTH_INFO, "drain", node_id,
                "my_mask=0x%x last_open=0x%x last_close=0x%x partial=%d realized_pre=%g fees_pre=%g wins=%u losses=%u",
                (unsigned)my_mask,
                (unsigned)oms->last_opened_mask,
                (unsigned)oms->last_closed_mask,
                partial_on,
                Money_ToDouble(ctx.node_realized),
                Money_ToDouble(ctx.node_fees),
                ctx.node_wins, ctx.node_losses);
        }
    }

    // ---- Entries: open_notional / fees + heartbeat counters ----
    uint16_t open_mask = (uint16_t)(oms->last_opened_mask & my_mask);
    while (open_mask) {
        int slot = __builtin_ctz(open_mask);
        open_mask &= (uint16_t)(open_mask - 1);
        if (slot < 0 || slot >= max_slot) continue;
        // v5.15.5.C.4 Phase H — derive entry-side fields from Position state.
        // Phase F's invariant guarantees Position is in OPEN form here
        // (Portfolio_OpenSlot just wrote entry_price + quantity + entry_fee
        // in Phase B; DrainPostFill open-mask iter runs after).
        const auto& pos_entry = oms->portfolio.positions[slot];
        const Money entry_notional_derived = Money_Mul(pos_entry.entry_price, pos_entry.quantity);
        const Money entry_fee_derived      = pos_entry.entry_fee;
        ctx.node_open_notional = Money_Add(ctx.node_open_notional, entry_notional_derived);
        // v5.3.1 (Phase D fee accounting fix): do NOT add entry_fee here.
        // The exit pass below adds rec.exit_total_fees which already equals
        // entry_fee + exit_fee (set in OMS_HandleFill). Adding entry_fee
        // here was double-counting it — visible in the GUI as per-core
        // "Fees" being ~1.5× the sum of Trade History fees (entry+exit
        // round-trip × N legs vs entry × N + entry+exit × N).
        ctx.entries_processed++;
        state->total_entries++;
        state->total_events_processed++;

        // v5.7.1: entry-quality health log. Captures the state of the
        // engine at the moment a fill landed — strategy + resolved
        // strategy + regime + scores + slow-path diag values + gate
        // threshold + fee-floor margin. Operators can post-hoc grep:
        //   jq 'select(.cat=="entry" and .core==N)' health.jsonl
        // and classify trades by regime/strategy/score combination.
        // Entry price + qty come from the portfolio slot (FillRecord
        // only stores notional, not the components).
        if (tt::Health_LogEnabled(tt::HEALTH_INFO) && slot < 16) {
            const auto& pos = oms->portfolio.positions[slot];
            // v5.9.0b — extended with ML decision context (V5_9_AUDIT-#3).
            // Post-mortem analysis: "why did ML enter that bad trade?" is
            // now answerable from the log alone (prediction, threshold,
            // confidence, registry hash).
            int is_ml = (ctx.resolved_strategy_id == STRATEGY_ML);
            tt::Health_Log(tt::HEALTH_INFO, "entry", node_id,
                "slot=%d strat=%u resolved=%u regime=%d "
                "trend_score=%d vol_score=%d hyst=%d/%d "
                "entry_px=%g qty=%g entry_notional=%g entry_fee=%g "
                "tp_pct=%g tp_floor=%g "
                "stddev_pct=%g long_slope=%g vol_delta=%g "
                "ml_pred=%g ml_thr=%g ml_eff_thr=%g ml_conf=%g registry=%016lx",
                slot, (unsigned)ctx.strategy_id,
                (unsigned)ctx.resolved_strategy_id,
                ctx.regime_state.current_regime,
                ctx.regime_state.last_trending_score,
                ctx.regime_state.last_volatile_score,
                ctx.regime_state.hysteresis_count,
                ctx.regime_state.hysteresis_threshold,
                Money_ToDouble(pos.entry_price),
                Money_ToDouble(pos.quantity),
                Money_ToDouble(entry_notional_derived),  // v5.15.5.C.4 Phase H — derived
                Money_ToDouble(entry_fee_derived),       // v5.15.5.C.4 Phase H — derived
                // v5.15.5.B.2 — diag_* + last_ml_* fields extracted to
                // display_meta. Use the per-core meta alias for readability.
                FPN_ToDouble(state->display_meta[node_id].diag_tp_pct_actual),
                FPN_ToDouble(state->display_meta[node_id].diag_tp_pct_floor),
                FPN_ToDouble(state->display_meta[node_id].diag_stddev_pct),
                FPN_ToDouble(state->display_meta[node_id].diag_long_slope),
                FPN_ToDouble(state->display_meta[node_id].diag_volume_delta),
                // v5.9.0b — ML decision context. Zero for non-ML cores.
                is_ml ? ctx.active_prediction : 0.0,
                is_ml ? state->display_meta[node_id].last_ml_threshold : 0.0,
                is_ml ? state->display_meta[node_id].last_ml_effective_threshold : 0.0,
                is_ml ? ctx.last_confidence : 0.0,
                (unsigned long)FEATURE_REGISTRY_HASH());
        }
    }
    oms->last_opened_mask &= (uint16_t)~my_mask;  // clear only my bits

    // ---- Exits ----
    uint16_t close_mask = (uint16_t)(oms->last_closed_mask & my_mask);
    while (close_mask) {
        int slot = __builtin_ctz(close_mask);
        close_mask &= (uint16_t)(close_mask - 1);
        if (slot < 0 || slot >= max_slot) continue;
        // Leg A is the even slot under partials; the only slot when
        // partials disabled. Per-trade signals (W/L, ConfidenceScorer,
        // pnl_feeder, cooldown) fire only on leg A.
        bool is_leg_a = !partial_on || ((slot & 1) == 0);
        // v5.15.5.C.4 Phase K — `const auto& rec = oms->last_fill[slot]`
        // declaration REMOVED. FillRecord struct is now extinct; all reads
        // (entry-side + exit-side) derive from Position state at this iter.

        // v5.15.5.C.4 Phase G — derive exit-side fields from Position state.
        // v5.15.5.C.5 — exit_fill_price + is_maker reverted to OMS sibling state
        // (slot-state-foreach-registry-with-storage-routing.md decision tree).
        // Phase F's invariant guarantees Position is in CLOSE form here
        // (DrainPostFill runs between Phase A SELL processing and Phase B BUY
        // processing within each drainer cycle; Portfolio_CloseSlot only clears
        // the active_bitmap bit, NOT the values; Portfolio_OpenSlot's overwrite
        // is gated to Phase B). See
        // DESIGN_SPECS/phase-separated-drainer-for-safe-cross-temporal-derives.md.
        const auto& pos = oms->portfolio.positions[slot];
        const bool slot_is_maker = BITMAP_IS_SET(oms->last_is_maker_bitmap, BITMAP_BIT_U16(slot));
        const Money exit_entry_notional = Money_Mul(pos.entry_price, pos.quantity);
        // v5.15.5.F.4c.3 WIP2d-1.B.1 — read authoritative exit_fee from OMS sibling array (set by HandleFill
        // SELL from o->pre_resolved.fee_rate). Replaces the prior cfg-recompute which lost the Order's
        // captured fee_rate. Per decision-time-data-binding-pattern.md: Order pre_resolved is canonical;
        // DrainPostFill is a CONSUMER, not a re-deriver. Eliminates the node_cfg param dependency at this site.
        const Money exit_fee            = oms->last_exit_fee[slot];
        const Money exit_total_fees     = Money_Add(pos.entry_fee, exit_fee);
        // D-190: gross via the SINGLE-SOURCE Money_FillGross (was 2-mul Sub(exit_notional, exit_entry_notional)
        // — the lone site that diverged from the 1-mul OMS books by 1 ULP under decimal). exit_entry_notional
        // is kept for the node_open_notional decrement below; the standalone exit_notional is no longer needed.
        const Money gross               = Money_FillGross(pos.entry_price, oms->last_exit_fill_price[slot], pos.quantity);
        const Money exit_net_pnl        = Money_Sub(gross, exit_total_fees);

        // Per-leg accounting: every exit fill contributes.
        ctx.node_realized      = Money_Add(ctx.node_realized, exit_net_pnl);
        ctx.node_open_notional = Money_Sub(ctx.node_open_notional, exit_entry_notional);
        ctx.node_fees          = Money_Add(ctx.node_fees, exit_total_fees);
        ctx.exits_processed++;
        state->total_exits++;
        state->total_events_processed++;

        // v5.7.1: exit-quality health log. Pairs with entry-log lines
        // to classify (strategy, regime_at_entry, exit_kind, net_bps)
        // for the strategy quality dashboard. exit_kind is inferred
        // from oms->last_realized_return + cooldown state — TP / SL
        // distinction is captured in `was_win` for now; finer
        // exit-kind taxonomy (time-exit, ratchet, manual) is deferred
        // to the dashboard panel computing it from order_history CSV.
        // v5.15.5.C.4 Phase J — was_win now in cross-slot bitmap; hoisted
        // single read per slot iter used at multiple sites below.
        const bool slot_was_win = BITMAP_IS_SET(oms->last_was_win_bitmap, BITMAP_BIT_U16(slot));
        if (tt::Health_LogEnabled(tt::HEALTH_INFO)) {
            double realized = oms->last_realized_return[slot];
            tt::Health_Log(tt::HEALTH_INFO, "exit", node_id,
                "slot=%d strat=%u resolved=%u was_win=%d realized_ret=%g "
                "net_pnl=%g entry_notional=%g total_fees=%g leg_a=%d",
                slot, (unsigned)ctx.strategy_id,
                (unsigned)ctx.resolved_strategy_id,
                (int)slot_was_win, realized,
                Money_ToDouble(exit_net_pnl),         // v5.15.5.C.4 Phase G — derived
                Money_ToDouble(exit_entry_notional),  // v5.15.5.C.4 Phase G — derived
                Money_ToDouble(exit_total_fees),       // v5.15.5.C.4 Phase G — derived
                is_leg_a ? 1 : 0);
        }

        // v4.7.21 W/L pairing under partials.
        // v5.14.9.G — partner_pending_active is now BITMAP_IS_SET(state->partner_pending_bitmap, bit)
        if (partial_on) {
            if (BITMAP_IS_SET(state->partner_pending_bitmap, BITMAP_BIT_U16(node_id))) {
                // v5.15.5.C.4 Phase G — exit_net_pnl is derived (see top of slot iter).
                Money total_net = Money_Add(ctx.partner_pending_pnl, exit_net_pnl);
                if (Money_Gt(total_net, Money_Zero())) {
                    ctx.node_wins++;
                    ctx.node_gross_wins = Money_Add(ctx.node_gross_wins, total_net);
                } else {
                    ctx.node_losses++;
                    ctx.node_gross_losses = Money_Add(ctx.node_gross_losses,
                                                    Money_Negate(total_net));
                }
                ctx.partner_pending_pnl = Money_Zero();
                BITMAP_CLR(state->partner_pending_bitmap, BITMAP_BIT_U16(node_id));
            } else {
                ctx.partner_pending_pnl = exit_net_pnl;  // v5.15.5.C.4 Phase G — derived
                BITMAP_SET(state->partner_pending_bitmap, BITMAP_BIT_U16(node_id));
            }
        }

        // Per-trade signals: leg A only.
        if (is_leg_a) {
            if (!partial_on) {
                // v5.15.5.C.4 Phase J — uses hoisted slot_was_win from above.
                ctx.node_wins   += (slot_was_win ? 1u : 0u);
                ctx.node_losses += (slot_was_win ? 0u : 1u);
                if (slot_was_win) {
                    ctx.node_gross_wins = Money_Add(ctx.node_gross_wins, exit_net_pnl);  // v5.15.5.C.4 Phase G — derived
                } else {
                    ctx.node_gross_losses = Money_Add(ctx.node_gross_losses,
                                                    Money_Negate(exit_net_pnl));
                }
            }
            double realized = oms->last_realized_return[slot];
            if (ctx.strategy_id == STRATEGY_ML) {
                // v5.14.1.B.1 (PARITY-002 + CLAUDE.md item 16 merge-scan):
                // hoist clock_gettime once; serves both UpdateAndMark
                // (composite freshness) + drift detection below. Saves
                // ~50-100ns vs the prior pattern of computing now_us only
                // inside the drift_floor branch.
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
                uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL +
                                  (uint64_t)ts.tv_nsec / 1000ULL;
                ConfidenceScorer_UpdateAndMark(&ctx.confidence,
                                               ctx.active_prediction, realized,
                                               now_us);
                ctx.active_prediction = 0.0;

                // v5.10.0e — runtime IC drift detection. Sample current IC
                // post-update; push to drift history; check sustained breach.
                // Only meaningful when operator has set drift_floor > 0.
                if (drift_floor > 0.0) {
                    // v5.14.1.F — variant-aware IC dispatcher honors
                    // cfg.confidence_ic_variant (default 0 = Spearman).
                    // Future Pearson/Kendall variants slot in via
                    // FOREACH_IC_VARIANT registry; existing semantics
                    // preserved bytewise (single-case switch inlines to
                    // direct RollingIC_Compute call).
                    double ic_now = ConfidenceScorer_ComputeICVariant(
                        &ctx.confidence, confidence_ic_variant);
                    DriftHistory_Push(&ctx.drift_history, ic_now, now_us);
                    double avg_ic = 0.0;
                    int    n_samples = 0;
                    int    breach = DriftHistory_CheckBreach(
                        &ctx.drift_history, now_us,
                        (uint64_t)drift_window_seconds * 1000000ULL,
                        drift_floor, &avg_ic, &n_samples);
                    // v5.15.5.E.B — breached + kill_tripped migrated to
                    // drift_state_flags bitmap (uint8_t; 2 bits used, 6 free).
                    // breach_first_us extracted to display_meta (write-only;
                    // preserved for future GUI). Per bitmap-flag-api.md +
                    // cache-layout-discipline Rule 1.
                    if (breach && !BITMAP_IS_SET(ctx.drift_history.drift_state_flags, MASK_DRIFT_BREACHED)) {
                        // First breach — log CRITICAL + record onset.
                        // Rate-limit at 60s per core to avoid log spam if
                        // breach toggles around the threshold.
                        BITMAP_SET(ctx.drift_history.drift_state_flags, MASK_DRIFT_BREACHED);
                        state->display_meta[node_id].drift_breach_first_us = now_us;
                        static uint64_t s_drift_log_us[16] = {0};
                        Health_LogCriticalRateLimited(
                            &s_drift_log_us[node_id & 15], 60000000ULL,
                            node_id, "drift",
                            "IC=%.4f below floor=%.4f over %us window (%d samples)",
                            avg_ic, drift_floor,
                            (unsigned)drift_window_seconds, n_samples);
                        if (drift_auto_kill && !BITMAP_IS_SET(ctx.drift_history.drift_state_flags, MASK_DRIFT_KILL_TRIPPED)) {
                            NODE_STATE_FLAG_SET(state->nodes[node_id], KILL_TRIPPED);
                            state->nodes[node_id].node_ks_trips_total++;
                            BITMAP_SET(ctx.drift_history.drift_state_flags, MASK_DRIFT_KILL_TRIPPED);
                            static uint64_t s_drift_kill_log_us[16] = {0};
                            Health_LogCriticalRateLimited(
                                &s_drift_kill_log_us[node_id & 15], 60000000ULL,
                                node_id, "drift",
                                "AUTO-KILL: per-node kill_switch tripped due to "
                                "sustained IC drift");
                        }
                    } else if (!breach && BITMAP_IS_SET(ctx.drift_history.drift_state_flags, MASK_DRIFT_BREACHED)) {
                        // Recovery — clear breach state, log info
                        BITMAP_CLR(ctx.drift_history.drift_state_flags, MASK_DRIFT_BREACHED);
                        fprintf(stderr,
                            "[drift] node %d RECOVERED: IC=%.4f above floor=%.4f\n",
                            node_id, avg_ic, drift_floor);
                    }
                }
            }
            if (realized < 0.0 && sl_cooldown_cycles > 0) {
                ctx.sl_cooldown_remaining = sl_cooldown_cycles;
            }
            RegressionFeederX_Push(&ctx.pnl_feeder, FPN_FromDouble<F>(realized));

            // v5.10.0a.G.8 — trade-close reward hook for ensemble bandit.
            // Real-money signal (incl. fees + slippage) carries higher
            // weight than slow-path lookback (default ×4). Cast through
            // void* since NodeContext can't depend on EnsembleModelZoo<F>
            // directly (would force ML_Headers visibility). Bandit feed
            // is rare (~1 per closed trade) — cost negligible.
            if (ctx.ensemble_handle) {
                auto* ezoo = static_cast<EnsembleModelZoo<F>*>(ctx.ensemble_handle);
                if (BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE) && BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY)) {
                    double bal_d = Money_ToDouble(oms->balance);
                    if (bal_d > 0.0) {
                        double pnl_d = Money_ToDouble(exit_net_pnl);  // v5.15.5.C.4 Phase G — derived
                        double pnl_bps = (pnl_d / bal_d) * 10000.0;
                        // v5.15.5.F.4d — pass node_cfg for per-core bandit_algorithm dispatch
                        // (Step 3 + § H Class 25 sweep). Inside _TradeCloseReward, reward attribution
                        // routes through g_buy_reward_dispatch[algo] — for THOMPSON / ghost / BLENDED
                        // modes, Thompson_Update fires too (was silently never called pre-.F.4d).
                        EnsembleModelZoo_TradeCloseReward(ezoo, pnl_bps,
                                                            ensemble_trade_reward_mult,
                                                            node_cfg);
                    }
                }
            }
        }

        // v5.13.4 — sell-side bandit reward attribution. Per-LEG (not
        // leg-A only) since each slot's exit decision is independent
        // under partials. Conditions for crediting exit_bandit:
        //   1. BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_EXIT_BANDIT_ENABLED)
        //   2. per-slot last_exit_predicted_bitmap (bit set at submit)
        //   3. NOT a flatten-induced safety event (don't pollute bandit
        //      with non-strategic exits)
        //   4. exit_bandits actually initialized (exit models loaded)
        //   5. captured arm + regime are valid
        //
        // Counterfactual reward (basis points relative to entry notional):
        //   actual_pnl_bps = exit_net_pnl / entry_notional * 10000
        //   hypothetical_pnl_bps = (tp_pct - 2*fee_rate) * 10000
        //     where tp_pct = (original_tp - entry_price) / entry_price
        //   reward = actual - hypothetical
        // Optimistic assumption: TP would have hit before SL.
        // Biases bandit AGAINST firing exits (since hypothetical is
        // usually positive) — operator scales via cfg.exit_bandit_lr.
        // v5.15.5.C.2 (S3b) — bit-packed in last_exit_predicted_bitmap.
        if (exit_bandit_enabled
            && slot < (int)MAX_PORTFOLIO_POSITIONS
            && BITMAP_IS_SET(oms->last_exit_predicted_bitmap, BITMAP_BIT_U16(slot))
            && oms->flatten_pending.load(std::memory_order_acquire) == 0
            && ctx.ensemble_handle) {
            auto* ezoo = static_cast<EnsembleModelZoo<F>*>(ctx.ensemble_handle);
            // v5.15.5.C.2.1 (LOW-2) — parallel decode of arm + regime from
            // packed meta byte. Both extracts have no data dependency on
            // each other; modern compilers fuse into ~1-2 cycles via ILP.
            // OMS_META_IS_VALID replaces the prior `>= 0` -1-sentinel check.
            uint8_t meta_byte = oms->last_exit_predicted_meta[slot];
            int chosen_arm = (int)OMS_META_GET_ARM(meta_byte);
            int regime     = (int)OMS_META_GET_REGIME(meta_byte);
            if (BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_EXIT_BANDITS_READY)
                && OMS_META_IS_VALID(meta_byte)
                && chosen_arm < ezoo->exit_predictor_count
                && regime < NUM_REGIMES) {
                // original_tp = the trail anchor locked at entry, FILL-priced
                // post-A25/D-205 (fill×(1+tp_pct), NOT the expected-entry TP) —
                // stable against later ratchet writes (those modify
                // take_profit_price, not original_tp). The exit-bandit
                // counterfactual recovers the resolved tp_pct from it.
                Money entry_p   = oms->portfolio.positions[slot].entry_price;
                Money orig_tp   = oms->portfolio.positions[slot].original_tp;
                double entry_d   = Money_ToDouble(entry_p);
                double orig_tp_d = Money_ToDouble(orig_tp);
                if (entry_d > 0.0 && orig_tp_d > entry_d) {
                    double tp_pct = (orig_tp_d - entry_d) / entry_d;
                    // E.1.2.C leg 0 — counterfactual fee reads the NODE's taker
                    // rate at point of use (fraction-scaled Money, e.g. 0.00100 =
                    // 0.1%; default equals the retired 0.001 literal bytewise).
                    // node_cfg is non-null here by construction: this block is
                    // gated on exit_bandit_enabled, which derives from node_cfg.
                    double fee_taker_cf = Money_ToDouble(node_cfg->fee_rate_taker);
                    double hypothetical_pnl_bps =
                        (tp_pct - 2.0 * fee_taker_cf) * 10000.0;
                    double notional_d = Money_ToDouble(exit_entry_notional);  // v5.15.5.C.4 Phase G — derived
                    double actual_pnl_bps = (notional_d > 0.0)
                        ? Money_ToDouble(exit_net_pnl) / notional_d * 10000.0  // v5.15.5.C.4 Phase G — derived
                        : 0.0;
                    double reward_bps = actual_pnl_bps - hypothetical_pnl_bps;
                    // v5.15.5.F.4d — exit-side bandit reward attribution via g_exit_reward_dispatch
                    // (Step 3 + § A.2 + § H of merged plan body). Pre-.F.4d this was direct Bandit_Update
                    // on exit_bandits only — Thompson posterior on exit-side was NEVER updated
                    // (Class 24 sister bug at exit attribution surface). Now: dispatch table auto-selects
                    // exp3_only_reward / thompson_only_reward / both_reward per algo metadata. For
                    // algo=EXP3 (cfg=0) → bytewise identical to pre-.F.4d (only Bandit_Update on
                    // exit_bandits[]). For THOMPSON / ghost / BLENDED → also exit_thompson_update_fn
                    // sink (noop_thompson_update if exit-side Thompson subsystem not initialized,
                    // real_thompson_update if _InitExitThompsonBandits boot-wired). Closes exit-side
                    // Class 24 sister attribution gap structurally + closes asymmetry where buy-side
                    // had Thompson reward but exit-side was Exp3-only.
                    int exit_algo = node_cfg ? node_cfg->bandit_algorithm : (int)BANDIT_ALGO_EXP3;
                    if (exit_algo < 0 || exit_algo >= FOREACH_BANDIT_ALGORITHM_COUNT) exit_algo = (int)BANDIT_ALGO_EXP3;
                    g_exit_reward_dispatch<F>[exit_algo](ezoo, regime, chosen_arm, reward_bps);
                }
            }
        }
        // v5.13.0.B + v5.13.4 — clear per-slot exit-prediction state
        // post-attribution (single-use per trade).
        // v5.15.5.C.3 Phase 8 — Class-18 mirror close. Shared macro
        // OMS_RESET_PER_SLOT_EXIT_PREDICTOR (defined in
        // MemHeaders/OmsExitPredictorMetaRegistry.hpp) clears all 3
        // components atomically: bitmap bit + last_exit_predicted_p[slot] +
        // last_exit_predicted_meta[slot]. Same macro is referenced by
        // future call sites (e.g., manual close paths if added); adding a
        // 4th per-slot exit-predictor state field expands ONE macro instead
        // of N parallel sites.
        if (slot < (int)MAX_PORTFOLIO_POSITIONS) {
            OMS_RESET_PER_SLOT_EXIT_PREDICTOR(oms, slot);
        }
    }
    oms->last_closed_mask &= (uint16_t)~my_mask;  // clear only my bits
}

// Wrapper: iterate registered cores. ALL production paths reach OneCore
// through this fan (live drainer via EngineCommon_DrainPostFill; backtest
// driver likewise). E.1.2.C leg 0 — FULLY DE-DEFAULTED (arity is the class
// guard; see OneCore header comment for the v5.14.1.F positional-shift bug
// this closes) and the two per-node facts are no longer parameters: the fan
// passes the per-node cfg slice and OneCore derives them at point of use.
// cfg == nullptr (test callers) ⇒ node_cfg == nullptr per core ⇒ per-node
// ML extras off — identical to the retired defaults.
template <unsigned F>
inline void EventLoop_DrainPostFill(EventLoopState<F>* state,
                                     OrderManagerState<F>* oms,
                                     uint32_t sl_cooldown_cycles,
                                     double ensemble_trade_reward_mult,
                                     double drift_floor,
                                     uint32_t drift_window_seconds,
                                     int      drift_auto_kill,
                                     int      confidence_ic_variant,
                                     // common_type wrapper = NON-DEDUCED context (bare nullptr OK).
                                     const typename std::common_type<ControllerConfig<F>>::type* cfg) {
    for (int c = 0; c < state->registered_count; ++c) {
        EventLoop_DrainPostFillOneCore(state, oms, sl_cooldown_cycles, c,
                                         ensemble_trade_reward_mult,
                                         drift_floor, drift_window_seconds,
                                         drift_auto_kill,
                                         confidence_ic_variant,
                                         cfg ? &cfg->nodes[c]
                                             : (const PerNodeCfg<F>*)nullptr);
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Run by the drainer thread after every OrderManager_Tick. Consumes the
// FillRecords + masks populated by OrderManager_HandleFill and applies
// per-core NodeContext updates that the legacy mode-0 path used to do
// inside EventLoop_OnEvent: node_open_notional, node_realized, node_fees,
// node_wins/node_losses, ConfidenceScorer feedback, SL cooldown,
// pnl_feeder push.
//
// Slot → node_id mapping is partials-aware via oms_state_flags
// PARTIAL_EXIT_ENABLED bit (v5.15.5.C.2 / S3a).
// Both legs of a paired trade route their stats to the SAME NodeContext
// (one per core, not one per leg) — partials only split the exit
// schedule, not the allocation.
//
// Per-leg vs per-trade fields (load-bearing under partials):
//   - PER-LEG (every exit fill contributes): node_realized,
//     node_open_notional, node_fees. Each leg has its own qty + entry
//     notional + fee, so all aggregate.
//   - PER-TRADE (leg-A only fires the signal): node_wins/node_losses,
//     ConfidenceScorer_Update, active_prediction reset, pnl_feeder push,
//     sl_cooldown_remaining. One trade = one outcome signal. Mirrors the
//     entry-side rule that last_entry_tick + last_entry_price +
//     active_prediction are stamped only on leg-A entries.
//
// v4.7.4: prior behavior fired the per-trade hooks on EVERY leg bit,
// causing IC contamination on the ConfidenceScorer (1 valid (X, ret_a)
// pair + 1 garbage (0, ret_b) pair after active_prediction was wiped on
// leg-A iter), 2× pnl_feeder pushes per trade, doubled W/L counters. The
// double-fire was actually present since v4.7.0 partials but only
// became visible once cores were swapped to STRATEGY_ML.
//
// All slow-path. Single-threaded (drainer is sole reader, OMS_Tick on
// the same thread is sole writer). FPN_Binary-pure on the per-core math.
//
// v4.7.38 (Phase C.1): per-core helper. Walks ONLY this core's slots in
// the open/close masks. Clears only its own bits (`mask &= ~my_mask`).
// Wrapper EventLoop_DrainPostFill calls this for each registered core.
//
// Atomicity note: in the centralized path (drainer thread is sole caller,
// today's mode), plain uint16_t writes to last_opened_mask / last_closed_mask
// are safe. When Phase C.2 spawns per-core slow-path threads, EACH thread's
// OneCore call clears its own bits — multiple writers to the same uint16
// becomes a race. Phase C.2 will convert these to std::atomic<uint16_t>
// and use fetch_and(~my_mask). For Phase C.1 (this commit), single-writer
// drainer keeps plain access safe.
//======================================================================
// [END_FUNCTION]_[EventLoop_DrainPostFillOneCore]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoop_OnEvent]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING]]
// [REFERENCE]_[INVARIANT]_[H20]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one TradeEvent -> portfolio/balance/stats; combined-mask single-guard dispatch; mode-1 (production) routes through OMS_PushSubmit and returns early — the mode-0 body is legacy/test bookkeeping]
// [REFERENCE]_[DECISION]_[D-202]
// [REFERENCE]_[DESIGN_SPEC]_[adversarial-pessimistic-simulation-discipline.md]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EventLoop_OnEvent(EventLoopState<F>* state, const TradeEvent<F>& event_in,
                              // v5.15.5.F.4c.3 WIP2d-1.B.1 — per-core cfg array (nullptr fallback).
                              // OnEvent reads event.node_id then indexes nodes[event.node_id] for
                              // per-core fee_rate / slippage_pct. Mode-1 path returns early (sharded
                              // production); mode-0 legacy body uses these. Per cfg-scope-discipline
                              // § "consumer over per-core array."
                              const PerNodeCfg<F>* nodes = nullptr) {
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — branchless cores-select: ONE cmov at entry; subsequent reads pure ALU.
    static const PerNodeCfg<F> NULL_PER_NODE_CFG_STUB_ARRAY[MAX_EXECUTION_NODES] = {};
    const PerNodeCfg<F>* effective_nodes = nodes ? nodes : NULL_PER_NODE_CFG_STUB_ARRAY;
    // v5.15.5.F.4d.1.E.0.10 A9 — paper/backtest SLIPPAGE moved to the OrderManager_Submit synthetic-fill
    // chokepoint: the SINGLE production slip SSoT (D-202 + adversarial-pessimistic-simulation-discipline.md).
    // This OnEvent slip was DEAD on the production (mode-1) path — the should_apply gate below returns BEFORE
    // the mode-0 body reads event.price, and Async books the raw price via Submit (the BookFill site in EngineSharded/Async.hpp). It is now
    // removed; the mode-0 / test-only bookkeeping path below books the raw trigger price (slip lives at Submit).
    // Mutate a local copy so the caller's event is untouched.
    TradeEvent<F> event = event_in;
    // event.node_id on THIS path is the NODE (ExecutionCore writes core->node_id), while the
    // portfolio calls below need the SLOT. Under partial_exit_enabled a node owns slots 2N+0/2N+1,
    // so the two diverge and one variable cannot serve both — this body used `slot` for BOTH
    // (nodes[]/effective_nodes[] AND positions[]/OpenSlot/CloseSlot), which mapped node N's fill
    // onto portfolio slot N. Latent only because mode-0 is legacy/test-only (E.1.2.F Class-61).
    const int node = (int)event.node_id;
    const int partial_on_ev = BITMAP_IS_SET(state->oms->oms_state_flags,
                                            tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
    const int pslot = Sharded_LegSlot(node, (int)event.leg, partial_on_ev);
    int slot = node;   // node-space alias; every portfolio call below uses `pslot`
    // v5.15.5.F.4c.3 WIP2d-1.B.1 option C — combined-mask collapse: 3 separate predicate branches
    // (bounds + mutex + mode-1 fast-path) collapsed into 1 combined-mask + single guard branch.
    // Reduces predictor entries 3 → 1; bounds variance to a single source. Per Caramel's
    // determinism principle: even bounded predictor variance is variance worth eliminating.
    //
    // Branchless mask compute (pure ALU; ~5ns):
    bool is_entry = (event.type & TRADE_EVENT_ENTRY) != 0;
    bool is_exit  = (event.type & TRADE_EVENT_EXIT)  != 0;
    const bool valid_slot   = slot < state->registered_count && pslot >= 0
                              && pslot < MAX_PORTFOLIO_POSITIONS;   // node bound AND a derivable slot
    const bool valid_mutex  = !(is_entry && is_exit);              // same-tick entry+exit impossible by ExecutionCore_Tick construction
    const bool mode_0_body  = !MBS_EQ_U8(state->oms->oms_state_flags, tt::MASK_OMS_STATE_EVENT_LOG_MODE,
                                          tt::SHIFT_OMS_STATE_EVENT_LOG_MODE, 1);  // mode-1 (production sharded) → false → skip body
    const bool should_apply = valid_slot && valid_mutex && mode_0_body;
    // Single guard branch — predicted-taken in production (mode-1 means !should_apply).
    // TECH_DEBT option B: full branchless via Portfolio_OpenSlot/CloseSlot + TradeLog_RecordEntry
    // mask-param refactor scheduled for future ship (per ship-close TECH_DEBT entry).
    if (!should_apply) return;

    // === MODE 0: legacy OnEvent path (reached only when should_apply mask is true) ===
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — mode-1 body deleted (was pure-noop return). Combined-mask
    // above filters out mode-1 + invalid input in single branch; mode-0 body only runs when
    // valid + mode-0. Per v4.7.19 doctrine: production counter bumps happen via DrainPostFill,
    // not here. Mode-0 path is legacy / test-only (sharded production is mode-1).
    NodeContext<F>* ctx = &state->nodes[slot];
    if (is_entry) {
        // Compute entry fee = entry_price * qty * fee_rate
        // Phase 8: synchronous market BUY = taker by exchange definition.
        // OMS HandleFill (mode 1) will book the actual maker/taker fee from
        // the WS executionReport. This sync accounting is optimistic.
        Money notional = Money_Mul(event.price, ctx->intended_qty);
        // v5.15.5.F.4c.3 WIP2d-1.B.1 — branchless read via effective_nodes (slot already validated above).
        const Money entry_fee_rate = effective_nodes[slot].fee_rate_taker;
        Money entry_fee = Money_Mul(notional, entry_fee_rate);
        Portfolio_OpenSlot(&state->oms->portfolio, pslot,
                           event.price,
                           ctx->intended_qty,
                           ctx->intended_tp,
                           ctx->intended_sl,
                           entry_fee);
        ctx->entries_processed++;
        state->total_entries++;
        state->total_events_processed++;
        // v4.2.1: reset idle-cycle counter on every fill
        ctx->idle_cycles = 0;
        // Phase 2.1: per-core open notional. Add the entry notional. The
        // exit branch subtracts the SAME (entry_price × qty) snapshot so
        // round-trips return to exactly zero — never use exit_price × qty
        // here (asymmetric subtraction would leak residue per trade).
        ctx->node_open_notional = Money_Add(ctx->node_open_notional, notional);
        // CSV: record AFTER portfolio mutation so the slot is consistent if the
        // log call inspects it (currently it doesn't, but kept defensive).
        if (state->oms->trade_log) {
            // s5-1b: resolved id (post-AUTO), matching the mode-1 fill-emit path.
            ShardedTradeLog_RecordEntry(state->oms->trade_log, event,
                                        ctx->resolved_strategy_id,
                                        event.price,
                                        ctx->intended_qty,
                                        entry_fee,
                                        state->oms->balance,
                                        /*regime=*/(int)ctx->regime_state.current_regime);
        }
        return;
    }

    if (is_exit) {
        // close slot returns gross. apply both entry fee (already paid at fill
        // time, recorded in position) and exit fee (computed from exit notional).
        // Snapshot the position fields BEFORE CloseSlot clears the bit, so the
        // CSV row sees the entry_price + qty even though the slot is "closed".
        Money entry_price_snap = state->oms->portfolio.positions[pslot].entry_price;
        Money qty_snap = state->oms->portfolio.positions[pslot].quantity;
        Money entry_fee = state->oms->portfolio.positions[pslot].entry_fee;
        Money gross = Portfolio_CloseSlot(&state->oms->portfolio, pslot, event.price);
        Money exit_notional = Money_Mul(event.price, qty_snap);
        // Phase 8: TP/SL exit = market sell = always taker by exchange def.
        // v5.15.5.F.4c.3 WIP2d-1.B.1 — branchless read via effective_nodes (slot already validated above).
        const Money exit_fee_rate = effective_nodes[slot].fee_rate_taker;
        Money exit_fee = Money_Mul(exit_notional, exit_fee_rate);
        Money total_fee = Money_Add(entry_fee, exit_fee);
        Money net = Money_Sub(gross, total_fee);
        state->oms->balance = Money_Add(state->oms->balance, net);
        state->oms->realized_pnl = Money_Add(state->oms->realized_pnl, net);
        // v4.0.4: per-core P&L bookkeeping. The OMS keeps a single global
        // accumulator (one portfolio); we split it back out by source core
        // for the Account panel so users can see which core is making/losing
        // money. node_fees adds the entry+exit fee for this fill.
        // Branchless win/loss: Money_Gt returns 1/0, used as integer
        // mask. Slow path so cost is irrelevant — kept branchless for
        // consistency with the rest of the engine.
        // (.E.0.10: comment de-rotted FPN_GreaterThan→Money_Gt — the line below
        //  is decimal Money_Gt; the stale name caused a false register finding.)
        ctx->node_realized = Money_Add(ctx->node_realized, net);
        ctx->node_fees = Money_Add(ctx->node_fees, total_fee);
        uint32_t is_win = (uint32_t)Money_Gt(net, Money_Zero());
        ctx->node_wins   += is_win;
        ctx->node_losses += (1u - is_win);
        // Phase 2.1: subtract the SAME entry notional we added at entry time.
        // Use entry_price_snap × qty_snap, NOT exit_price × qty_snap — the
        // latter would leak residue per round trip (positive when winning,
        // negative when losing) and drift the budget tracker unboundedly.
        // FPN_SubSat saturates at zero if state ever becomes inconsistent
        // (defensive against future bugs; should never trigger in practice).
        Money entry_notional_snap = Money_Mul(entry_price_snap, qty_snap);
        ctx->node_open_notional = Money_Sub(ctx->node_open_notional, entry_notional_snap);
        // Phase 09: track peak balance for drawdown-based kill switch.
        // Cheap on the slow path; the comparison is one FPN_Binary compare per exit.
        if (Money_Gt(state->oms->balance, state->oms->ks_peak_balance)) {
            state->oms->ks_peak_balance = state->oms->balance;
        }
        ctx->exits_processed++;
        state->total_exits++;
        state->total_events_processed++;
        // CSV: pitfall P8.7 — log AFTER net/total_fee/balance are computed.
        if (state->oms->trade_log) {
            // s5-1b: resolved id (post-AUTO), matching the mode-1 fill-emit path.
            ShardedTradeLog_RecordExit(state->oms->trade_log, event,
                                       ctx->resolved_strategy_id,
                                       entry_price_snap,
                                       event.price,
                                       qty_snap,
                                       net,
                                       total_fee,
                                       state->oms->balance,
                                       /*regime=*/(int)ctx->regime_state.current_regime);
        }
        return;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// process one TradeEvent. dispatches to entry or exit handling based on
// event.type bits. updates portfolio + balance + statistics. does NOT call
// kill switch eval, regime update, or any code that might mutate gate_params
// (those go through deferred dirty-flag mechanisms in later phases).
//
// entry handling:
//   - look up NodeContext for event.node_id (the source core)
//   - read intended TP/SL/qty from the context (set by controller earlier)
//   - call Portfolio_OpenSlot to write the position fields and set the bit
//   - bump per-core entries_processed counter
//
// exit handling:
//   - call Portfolio_CloseSlot which clears the bit and returns gross P&L
//   - apply fees: net = gross - (gross * fee_rate)  (matches existing
//     PortfolioController fee model)
//   - balance += net, realized_pnl += net
//   - bump per-core exits_processed counter
//
// invalid event types (type == 0 or type == TRADE_EVENT_ENTRY|TRADE_EVENT_EXIT)
// are silently ignored — the branchless ExecutionCore_Tick can never produce
// them, but defensive logic in case of replay or fuzz testing.
//======================================================================
// [END_FUNCTION]_[EventLoop_OnEvent]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoop_DrainEvents]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[round-robin drain with per-node cap (P4.1 anti-starvation) -> OnEvent each; cross-node ordering NOT preserved (P4.2)]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int EventLoop_DrainEvents(EventLoopState<F>* state) {
    int total_drained = 0;
    for (int slot = 0; slot < state->registered_count; ++slot) {
        ExecutionCore<F>* core = state->nodes[slot].core;
        if (core == nullptr) continue;

        for (int i = 0; i < MAX_EVENTS_PER_DRAIN_PER_NODE; ++i) {
            TradeEvent<F> event;
            if (!SPSCRing_TryPop(&core->event_ring, &event)) break;
            EventLoop_OnEvent(state, event);
            ++total_drained;
        }
    }
    return total_drained;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// round-robin across all registered cores. for each core, pop up to
// MAX_EVENTS_PER_DRAIN_PER_NODE events from its event ring and process each
// via _OnEvent. returns total events processed in this pass.
//
// the per-core cap prevents one chatty core from monopolizing a drain pass
// and starving the others (pitfall P4.1). under sustained burst load, the
// loop iterates fast enough that 16 events × 16 cores = 256 events per pass
// is plenty for realistic event rates (a busy strategy fires entries at
// most a few times per second).
//======================================================================
// [END_FUNCTION]_[EventLoop_DrainEvents]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoop_QueueParameters]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[stage pending_params + set the DIRTY flag; coalesced into one seqlock push per cycle (P5.6 two-step)]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EventLoop_QueueParameters(EventLoopState<F>* state, int slot,
                                       const GateParameters<F>& new_params) {
    if (slot < 0 || slot >= state->registered_count) return;
    state->nodes[slot].pending_params = new_params;
    NODE_STATE_FLAG_SET(state->nodes[slot], DIRTY);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// stage a new GateParameters pack for the given core and mark it dirty. the
// next call to _PushParameters will atomically push it through the core's
// triple-buffered ParameterSlot.
//
// rationale (pitfall P5.6): parameter rebuilds must come from a slow-path
// context where RollingStats and regime state are fresh. the controller's
// strategy code calls _QueueParameters from the slow path. the actual push
// happens in the run loop's parameter-push step, which is also slow-path.
// this two-step (queue then push) lets multiple subsystems mark a core dirty
// in one slow-path cycle and have the changes coalesced into a single push.
//
// pitfall P5.3: the dirty bit is single-writer because the controller is
// single-threaded. no atomic needed.
//======================================================================
// [END_FUNCTION]_[EventLoop_QueueParameters]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoop_RebuildAllParameters]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[centralized/backtest fan — one global book-imbalance veto, then RebuildOneCore per registered node (STRATEGY_NONE skipped)]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W = 128, unsigned WL = 512>
inline int EventLoop_RebuildAllParameters(
    EventLoopState<F>* state,
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    const RollingStats<F, WL>* rolling_long = nullptr,
    const void* ror_regressor = nullptr,    // const RORRegressor<F>*
    const void* ema_price     = nullptr,    // const FPN_Binary<F>*
    const void* current_price = nullptr,    // const FPN_Binary<F>* — Phase 3 MTM
    // v4.3 — expanded feature-pack state (optional)
    const void* rolling_medium   = nullptr,  // const RollingStats<F, 256>*
    const void* rolling_baseline = nullptr,  // const RollingStats<F, 1024>*
    const void* cumdelta_state   = nullptr,  // const CumDeltaState<F>*
    const void* tick_rate_state  = nullptr,  // const TickRateState*
    uint64_t timestamp_us = 0,
    // Track E.3 (2026-04-26) — depth-derived buy gate. Optional FPN_Binary<F>*
    // (passed as void* to keep the signature uniform with the other
    // optional pointers above and avoid template-parameter coupling). When
    // non-null AND cfg.min_book_imbalance > 0 AND *book_imbalance < min,
    // every core's pending_params gets GATE_FLAG_BUY_BLOCKED set after
    // Strategy_BuildParameters runs, vetoing entries until the imbalance
    // recovers. Caller passes from DepthSharedState (live) or
    // DepthReplayState (backtest) — symmetric across both paths.
    const void* book_imbalance = nullptr,     // const FPN_Binary<F>*
    // v4.5 Wave 1 (2026-04-27) — D.1/D.2/D.4 feature pack expansion.
    // Optional state pointers; when non-null, threaded into MLBuildContext
    // so ML_BuildParameters → Regime_ComputeSignals populates the new
    // RegimeSignals fields. Symmetric live + backtest. Null-safe.
    const void* book_imb_history = nullptr,   // const BookImbalanceHistory<F, 1024>*
    const void* flow_state       = nullptr,   // const FlowState*
    const void* large_trade_state = nullptr,  // const LargeTradeState<F, 1024>*
    // v4.6 Wave 2 (2026-04-27) — D.3 spread dynamics state + current
    // spread / mid_price from BookSnapshot. Same null-safe pattern.
    const void* spread_state    = nullptr,    // const SpreadState<F, 1024>*
    double      current_spread  = 0.0,        // BookSnapshot::spread → double
    double      current_mid_price = 0.0,      // BookSnapshot::mid_price → double
    uint64_t    now_us          = 0           // v5.12.1.B clock hoist; 0 = legacy
) {
    int rebuilt = 0;
    // Track E.3: compute the book-imbalance veto once before the per-core
    // loop. The check is global (the order book is the same for every
    // core), so we evaluate once and OR the flag into each core's flags
    // below. cfg.min_book_imbalance==0 disables the gate entirely (legacy
    // behavior; pre-E.3 cfg ships with min=0).
    int book_imbalance_blocked = 0;
    if (book_imbalance && !FPN_IsZero(config->min_book_imbalance)) {
        const FPN_Binary<F>* bi = (const FPN_Binary<F>*)book_imbalance;
        book_imbalance_blocked = FPN_LessThan(*bi, config->min_book_imbalance) ? 1 : 0;
    }
    // v4.7.38 (Phase C.1): per-core loop body extracted to EventLoop_RebuildOneCore.
    // Both centralized path (this loop) and per-core slow-path (Phase C.2)
    // call OneCore with the same arguments — guarantees train-serve parity
    // by sharing the SAME function across all execution paths. STRATEGY_NONE
    // skip is the caller's responsibility (cheap conditional).
    for (int slot = 0; slot < state->registered_count; ++slot) {
        if (state->nodes[slot].strategy_id == STRATEGY_NONE) continue;
        EventLoop_RebuildOneCore(
            state, slot, rolling, config, rolling_long, ror_regressor, ema_price,
            current_price, rolling_medium, rolling_baseline, cumdelta_state,
            tick_rate_state, timestamp_us, book_imbalance, book_imb_history,
            flow_state, large_trade_state, spread_state, current_spread,
            current_mid_price, book_imbalance_blocked,
            now_us);  // v5.12.1.B clock hoist passthrough
        ++rebuilt;
    }
    return rebuilt;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// walk every registered core. for each one, dispatch to its strategy's
// _BuildParameters via Strategy_BuildParameters and store the result in
// pending_params. mark dirty so the next _PushParameters call hands them off
// to the execution cores via the seqlock.
//
// this is the upper layer of the parameter pipeline:
//   _RebuildAll (this function): strategy → pending_params, mark dirty
//   _PushParameters (phase 05):  pending_params → ExecutionCore.param_slot
//
// runs on the controller core, slow path. typical cadence is once per ~3 sim
// seconds aligned with the existing PortfolioController slow path. cost is
// dominated by ML nodes' model inference when present — ~550ns/boosting-round
// (3-class) + ~10µs fixed PER predict call PER arm (measured 2026-08-22; a
// 1050-tree 3-arm ensemble ≈ 650µs+, an H8 concern tracked under the
// SP_SECTION_ML_INFER bracket) — while non-ML strategy dispatch stays ~1µs/core.
//
// returns the number of cores that had their pending_params rebuilt. cores
// with strategy_id == STRATEGY_NONE are skipped (they get a zeroed pack from
// the dispatcher but we don't bother marking them dirty since the execution
// core's permission stays at 0 anyway).
//
// pitfall P5.6 alignment: this is called from the slow path AFTER
// RollingStats_Push, so the rolling state is fresh.
//======================================================================
// [END_FUNCTION]_[EventLoop_RebuildAllParameters]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoop_UpdateRollingStateOneCore]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [ML_INFERENCE]]
// [REFERENCE]_[INVARIANT]_[H4]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one node's slow_state ingestion — Money crosses to FPN_Binary EXACTLY ONCE (D-122 seam), then rolling/regime/flow pushes; UpdateEmaPriceAllCores (producer replication) shares the section]
// [REFERENCE]_[DECISION]_[D-122]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EventLoop_UpdateRollingStateOneCore(
    EventLoopState<F>* state, int slot,
    Money price_m, Money volume_m, uint64_t timestamp_us,
    int is_buyer_maker,
    FPN_Binary<F> depth_imbalance, FPN_Binary<F> depth_spread,
    int depth_enabled) {
    if (slot < 0 || slot >= MAX_EXECUTION_NODES) return;
    auto* sst = state->nodes[slot].slow_state;
    if (!sst) return;
    if (Money_IsZero(price_m)) return;  // pre-warmup tick — skip pushes

    // D-122 feature-domain ingress: the money tick crosses to binary EXACTLY ONCE
    // here; every rolling/regime/flow consumer below stays FPN_Binary (feature domain).
    FPN_Binary<F> price  = Money_ToBinary(price_m);
    FPN_Binary<F> volume = Money_ToBinary(volume_m);

    // PARITY-047 — forward the trade side. Omitting it let RollingStats_Push's
    // defaulted `int is_buyer_maker = 0` absorb the argument silently, so ALL
    // volume routed to buy, sell_volume_sum never left zero, and volume_delta
    // computed buy/buy = exactly +1.0 forever. That pinned FEAT_VOLUME_DELTA to a
    // constant and left MeanReversion's falling-knife buy gate, HALT_VOL_DELTA and
    // SHALT_MOM_NO_FLOW permanently open/unreachable.
    RollingStats_Push(&sst->rolling_short,    price, volume, is_buyer_maker);
    RollingStats_Push(&sst->rolling_long,     price, volume, is_buyer_maker);
    RollingStats_Push(&sst->rolling_medium,   price, volume, is_buyer_maker);
    RollingStats_Push(&sst->rolling_baseline, price, volume, is_buyer_maker);
    CumDelta_Push(&sst->cumdelta_state, volume, is_buyer_maker);
    TickRate_Push(&sst->tick_rate_state, timestamp_us);

    LinearRegression3XResult<F> slope_sample;
    slope_sample.model.slope     = sst->rolling_short.price_slope;
    slope_sample.model.intercept = FPN_Zero<F>();
    slope_sample.r_squared       = sst->rolling_short.price_r_squared;
    RORRegressor_Push(&sst->regime_ror, slope_sample);

    double signed_vol = FPN_ToDouble(volume);
    if (is_buyer_maker) signed_vol = -signed_vol;
    FlowState_Push(&sst->flow_state, timestamp_us, signed_vol);
    LargeTradeState_Push(&sst->large_trade_state, volume);

    if (depth_enabled) {
        BookImbHistory_Push(&sst->book_imb_history, depth_imbalance);
        SpreadState_Push(&sst->spread_state, depth_spread);
    }
}

// Per-tick replication of ema_price across all engines' slow_state.
// Producer thread is sole writer; loops cheaply (one FPN_Binary copy per engine).
template <unsigned F>
inline void EventLoop_UpdateEmaPriceAllCores(
    EventLoopState<F>* state, FPN_Binary<F> ema_price) {
    for (int c = 0; c < state->registered_count; ++c) {
        if (state->nodes[c].slow_state) {
            state->nodes[c].slow_state->ema_price = ema_price;
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.1.2 — pushes (price, volume, timestamp, depth) into ONE engine's slow_state.
// Single-writer rule: caller must be the sole writer of state.nodes[c].slow_state
// for the duration of this call:
//   - per_node_slow: per-core slow-path c writes own only
//   - backtest: linear iteration, single-thread
//
// Mirrors the cadence-update logic that v5.0.x had in producer's fan_out
// (in EngineSharded.hpp pre-v5.1.2). Centralized in this
// helper so both callers (per_node_slow + backtest) do exactly the same
// work — train-serve parity is structural.
//
// Inputs:
//   price, volume          — current tick (slow-path-cadence sample)
//   timestamp_us           — wall-clock us of this update
//   is_buyer_maker         — Binance flag (1 = aggressive sell, 0 = aggressive buy)
//   depth_imbalance        — book imbalance from depth WS / replay (FPN_Zero if depth disabled)
//   depth_spread           — spread (FPN_Zero if depth disabled)
//   depth_enabled          — gate the depth-history pushes
//======================================================================
// [END_FUNCTION]_[EventLoop_UpdateRollingStateOneCore]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoop_RebuildOneCore]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [ML_INFERENCE] [CFG_FLOW]]
// [REFERENCE]_[INVARIANT]_[[H8] [H22]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[THE per-node rebuild body — lazy-rebuild gate, ResolveForCore + per-node gate cache, session mult, regime classify, strategy dispatch -> pending_params + DIRTY; shared by every execution path (train-serve parity by construction)]
// [REFERENCE]_[CLASS]_[[2] [9] [25] [26] [44]]
// [REFERENCE]_[DECISION]_[[D-170] [D-190] [D-211] [D-221]]
// [REFERENCE]_[PARITY]_[PARITY-1]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-40]
// [REFERENCE]_[DESIGN_SPEC]_[representation-migration-completeness.md]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W = 128, unsigned WL = 512>
inline void EventLoop_RebuildOneCore(
    EventLoopState<F>* state,
    int slot,
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    const RollingStats<F, WL>* rolling_long,
    const void* ror_regressor,
    const void* ema_price,
    const void* current_price,
    const void* rolling_medium,
    const void* rolling_baseline,
    const void* cumdelta_state,
    const void* tick_rate_state,
    uint64_t timestamp_us,
    const void* book_imbalance,
    const void* book_imb_history,
    const void* flow_state,
    const void* large_trade_state,
    const void* spread_state,
    double      current_spread,
    double      current_mid_price,
    int         book_imbalance_blocked,
    uint64_t    now_us = 0) {  // v5.12.1.B clock hoist; 0 = compute internally
    // v5.14.9.B.0 — populate engine-wide gate cache (FOREACH_SLOW_PATH_GATE
    // ENGINE_WIDE entries: lazy_rebuild + ws_flatten today). Per-core
    // duplicate write across the engine-wide loop is acceptable (cfg
    // values identical across cores; ~3ns idempotent OR-reduction).
    // Reads downstream via BITMAP_IS_SET(state->global_gate_state.flags, ...)
    if (state) {
        SLOW_PATH_GATE_AUTOPOPULATE_ENGINE_WIDE(state->global_gate_state, *config);
    }
    // v5.12.2.B — lazy rebuild predicate. Evaluated at function entry so
    // we skip the heavy body when slow_state hasn't changed materially
    // since last rebuild. Three escape clauses force a full rebuild:
    //   (1) MASK_LAZY_REBUILD_ACTIVE off (BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_LAZY_REBUILD_ENABLED)=0; default; preserves baseline)
    //   (2) caller didn't pass now_us (legacy + test path) — can't time-bound
    //   (3) sst is null OR last_rebuild bookkeeping is unset (warmup)
    // Otherwise check the time-bound + price-delta predicates.
    if (state && BITMAP_IS_SET(state->global_gate_state.flags, MASK_LAZY_REBUILD_ACTIVE)
        && now_us != 0
        && slot >= 0 && slot < MAX_EXECUTION_NODES) {
        auto* sst_lazy = state->nodes[slot].slow_state;
        if (sst_lazy && sst_lazy->us_at_last_rebuild != 0
            && !FPN_IsZero(sst_lazy->price_at_last_rebuild)
            && rolling) {
            // Time-bound force: rebuild every force_period_us regardless.
            uint64_t age_us = (now_us > sst_lazy->us_at_last_rebuild)
                ? (now_us - sst_lazy->us_at_last_rebuild) : 0;
            int time_force = (age_us >= config->lazy_rebuild_force_period_us);
            // Price-delta force: rebuild when |Δprice| / last_price > threshold.
            FPN_Binary<F> price_now = rolling->price_avg;
            FPN_Binary<F> price_last = sst_lazy->price_at_last_rebuild;
            FPN_Binary<F> delta = FPN_Sub(price_now, price_last);
            // |delta| (was a direct sign-bit clear on sign-magnitude; 16B two's-comp → FPN_Abs, branchless).
            FPN_Binary<F> abs_delta = FPN_Abs(delta);
            FPN_Binary<F> rel_delta = FPN_DivNoAssert(abs_delta, price_last);
            int price_force = FPN_GreaterThan(rel_delta,
                Money_ToBinary(config->lazy_rebuild_price_threshold_pct));  // cadence heuristic, feature-side
            if (!time_force && !price_force) {
                // Lazy-skip path. Mark dirty=1 so PushParameters publishes
                // pending_params with fresh publish_tick (v5.12.1.B
                // staleness gate stays satisfied). pending_params payload
                // is unchanged from the last full rebuild — the republish
                // is purely for tick freshness.
                NODE_STATE_FLAG_SET(state->nodes[slot], DIRTY);
                return;
            }
        }
    }
    {
        // Single-iteration scope (was inside `for` loop body before extraction).
        // v4.0 per-core overrides: resolve the cfg for this core. Stack-local
        // copy with override fields swapped in. Strategies receive a "fully
        // resolved" cfg and don't need to know about the override mechanism.
        ControllerConfig<F> resolved_cfg =
            ControllerConfig_ResolveForCore(*config, slot);
        // v5.14.9.B.0 — populate per-core slow-path gate cache (PER_NODE
        // entries of FOREACH_SLOW_PATH_GATE) from resolved_cfg. ML_BuildParameters
        // reads via BITMAP_IS_SET(mctx->gate_state->flags, MASK_<NAME>); the
        // mctx.gate_state pointer is wired downstream where mctx is constructed.
        if (state && slot >= 0 && slot < MAX_EXECUTION_NODES) {
            SLOW_PATH_GATE_AUTOPOPULATE_PER_NODE(
                state->nodes[slot].gate_state, resolved_cfg);
        }
        // v4.0.3 D6: session-aware volume multiplier. Each session has its
        // own typical volume profile — cfg can require lower volume during
        // Asian session (when BTC is quieter) than US session (when busier).
        // Mirrors legacy PortfolioController. Time-of-day from system clock —
        // assumes engine clock is synced (it should be for live trading).
        time_t now = time(nullptr);
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        FPN_Binary<F> session_mult = FPN_FromDouble<F>(1.0);
        // v5.15.5.B.5 — branchless hour-of-day dispatch via SESSION_BY_HOUR[24]
        // lookup table (FOREACH_SESSION_PHASE registry). Replaces 4-way data-
        // dependent if/else cascade (~25% mispredict at session transitions
        // pre-.B.5) with single load + index. Per CLAUDE.md item 28
        // (latency-vs-cache framework) + closes TECH_DEBT-040.
        const FPN_Binary<F> session_mult_lookup[tt::SESSION_PHASE_COUNT] = {
#define X(NAME_U, name_l, START, END, MULT, DOC) resolved_cfg.session_##name_l##_mult,
            FOREACH_SESSION_PHASE(X)
#undef X
        };
        int hour = tm_utc.tm_hour;
        if (hour < 0) hour = 0;
        if (hour > 23) hour = 23;
        session_mult = session_mult_lookup[tt::SESSION_BY_HOUR[hour]];
        if (!FPN_IsZero(session_mult)) {
            // A24 (.E.0.10, D-211 option c): write the per-NODE slice nodes[slot], NOT the
            // flat resolved_cfg field. The live consumer (the Strategy_BuildParameters call below)
            // reads &resolved_cfg.nodes[slot]; ResolveForCore populates the slice but the
            // session/D10/spike mutations historically wrote the flat field → silently inert
            // (Class 44-B, H22 per-node-purity violation). nodes[slot] is the canonical
            // per-node view. resolved_cfg is stack-local → no seqlock concern.
            resolved_cfg.nodes[slot].volume_multiplier =
                FPN_Mul(resolved_cfg.nodes[slot].volume_multiplier, session_mult);
        }

        // v4.2.1: idle-cycle counter. Bump every rebuild; reset to 0 in
        // OnEvent's entry branch on every fill. When the threshold is
        // exceeded the pnl_feeder ring buffer is reset so adaptive
        // feedback (D10) doesn't keep applying shifts based on stale
        // outcomes from before a long quiet period. Mirrors the recovery
        // intent of legacy `idle_reset_cycles` without the filter-decay
        // step (sharded recomputes resolved_cfg fresh each rebuild, so
        // there's no live-filter drift to undo).
        state->nodes[slot].idle_cycles++;
        // v5.9.1 — boot-time per-core warmup-complete log (V5_9_AUDIT-#9).
        // Fires once per session per core, on the rebuild cycle that first
        // observes rolling.count >= min_warmup_samples. Distinct from the
        // global startup gate that releases all cores from CONTROLLER_WARMUP
        // simultaneously — operator wants per-core readiness because in
        // per_node_slow arch each core's slow path runs at its own cadence.
        if (!NODE_STATE_FLAG_IS_SET(state->nodes[slot], WARMUP_LOG_EMITTED)) {
            int wmin = (int)config->min_warmup_samples;
            if (wmin <= 0) wmin = 64;  // engine default (matches ShardedSnapshot fallback)
            if (rolling->count >= wmin) {
                // v5.9.4a — name the strategy so operator can distinguish ML
                // vs non-ML cores in mixed deployments. Bounds-checked via
                // static_assert on STRATEGY_SHORT_NAMES at its X-macro
                // declaration in StrategyInterface.hpp.
                int sid = state->nodes[slot].strategy_id;
                const char* sname = (sid >= 0 && sid < NUM_STRATEGIES)
                                  ? STRATEGY_SHORT_NAMES[sid] : "unknown";
                fprintf(stderr, "[node %d] warmup complete (%d/%d samples) — %s active\n",
                        slot, rolling->count, wmin, sname);
                NODE_STATE_FLAG_SET(state->nodes[slot], WARMUP_LOG_EMITTED);
            }
        }
        if (config->idle_reset_cycles > 0 &&
            state->nodes[slot].idle_cycles >= config->idle_reset_cycles) {
            state->nodes[slot].pnl_feeder.head  = 0;
            state->nodes[slot].pnl_feeder.count = 0;
            // Don't clear the actual price_samples — they're FPN_Zero already
            // when count==0 since the regression code reads only [0..count).
        }

        // v4.0.3 D10: adaptive feedback. Compute regression slope of recent
        // realized P&L. Negative slope = losing recently → tighten filters
        // (raise entry_offset_pct, raise volume_multiplier). Positive slope
        // = winning → loosen (lower offset, lower vol_mult). Scaled by
        // cfg.filter_scale and clamped to [offset_min, offset_max] +
        // [vol_mult_min, vol_mult_max] bounds. Mirrors legacy
        // MeanReversion_Adapt / Momentum_Adapt feedback loops.
        if (state->nodes[slot].pnl_feeder.count >= 4 &&
            !FPN_IsZero(resolved_cfg.filter_scale)) {
            LinearRegression3XResult<F> reg =
                RegressionFeederX_Compute(&state->nodes[slot].pnl_feeder);
            FPN_Binary<F> slope = reg.model.slope;
            // Only apply if R² is meaningful (otherwise slope is noise).
            if (FPN_GreaterThan(reg.r_squared, FPN_FromDouble<F>(0.20))) {
                // shift = -slope × filter_scale  (negative slope → positive shift = tighter)
                FPN_Binary<F> shift = FPN_Mul(slope, resolved_cfg.filter_scale);
                shift = FPN_Negate(shift);  // negate (was a sign-bit flip; 16B two's-comp)
                // Apply to entry_offset_pct, clamped to [offset_min, offset_max]
                // A24 (.E.0.10, D-211 option c): read+write the per-NODE slice nodes[slot]
                // (the value the consumer reads + the value D6 just mutated above). The
                // clamp BOUNDS (offset_min/max, vol_mult_min/max) are read-only + resolve-
                // equal on flat vs slice (ResolveForCore folds overrides into both), so they
                // stay flat — only the MUTATED fields move to the slice.
                Money new_offset = Money_Add(resolved_cfg.nodes[slot].entry_offset_pct, Money_FromBinary(shift));  // D-170 egress
                if (Money_Lt(new_offset, resolved_cfg.offset_min))
                    new_offset = resolved_cfg.offset_min;
                if (Money_Gt(new_offset, resolved_cfg.offset_max))
                    new_offset = resolved_cfg.offset_max;
                resolved_cfg.nodes[slot].entry_offset_pct = new_offset;
                // Apply to volume_multiplier same direction (tighter when losing)
                FPN_Binary<F> new_vmult = FPN_Add(resolved_cfg.nodes[slot].volume_multiplier, shift);
                if (FPN_LessThan(new_vmult, resolved_cfg.vol_mult_min))
                    new_vmult = resolved_cfg.vol_mult_min;
                if (FPN_GreaterThan(new_vmult, resolved_cfg.vol_mult_max))
                    new_vmult = resolved_cfg.vol_mult_max;
                resolved_cfg.nodes[slot].volume_multiplier = new_vmult;
            }
        }

        // v5.14.5.B.0.A — universalize regime classification.
        // Runs for ALL cores (was AUTO-only pre-v5.14.5.B.0). Closes the
        // architectural limitation where ML strategies couldn't read
        // hysteresed current_regime; opens v5.14.5.B's regime-context
        // ML features (regime_class_onehot etc.).
        //
        // Cost (slow-path): ~50-100ns per non-AUTO core × 16 cores = ~1.6µs/cycle.
        // Slow-path budget = 100µs p99; impact = 0.0016% — well within budget.
        // HOT_PATH_CHANGELOG entry filed.
        //
        // AUTO-mode-only logic (strategy resolution + ratchet adjustment on
        // transition) stays gated separately below.
        uint8_t effective_strategy_id = state->nodes[slot].strategy_id;
        int old_regime = state->nodes[slot].regime_state.current_regime;
        int new_regime = old_regime;  // default if compute path not active
        if (ror_regressor && ema_price && rolling_long) {
            const RORRegressor<F>* ror_in = (const RORRegressor<F>*)ror_regressor;
            const FPN_Binary<F>* ema_in          = (const FPN_Binary<F>*)ema_price;
            RegimeSignals<F> sig;
            // v4.3 — pass expanded state so regime classification sees the same
            // features the ML core sees (consistency).
            Regime_ComputeSignals(&sig, rolling, rolling_long, ror_in, *ema_in,
                                   (const RollingStats<F, 256>*)rolling_medium,
                                   (const RollingStats<F, 1024>*)rolling_baseline,
                                   (const CumDeltaState<F>*)cumdelta_state,
                                   (const TickRateState*)tick_rate_state,
                                   timestamp_us,
                                   book_imb_history, flow_state, large_trade_state,
                                   spread_state, current_spread, current_mid_price);
            new_regime = Regime_Classify(&state->nodes[slot].regime_state,
                                          &sig, &resolved_cfg);

            // v5.4.0 Phase 0.1 — log per-cycle regime classification
            // (universalized v5.14.5.B.0.A; was AUTO-only pre-fix).
            if (tt::Health_LogEnabled(tt::HEALTH_INFO)) {
                tt::Health_Log(tt::HEALTH_INFO, "regime", slot,
                    "old=%d new=%d ema_sma_spread=%g "
                    "short_r2=%g ror_slope=%g hyst=%d/%d short_count=%d",
                    old_regime, new_regime,
                    FPN_ToDouble(sig.ema_sma_spread),
                    FPN_ToDouble(sig.short_r2),
                    FPN_ToDouble(sig.ror_slope),
                    state->nodes[slot].regime_state.hysteresis_count,
                    state->nodes[slot].regime_state.hysteresis_threshold,
                    sig.short_count);
            }
        }

        // AUTO-only: resolve concrete strategy + ratchet adjustment on transition.
        // Pre-v5.14.5.B.0.A this block ALSO did the compute+classify; now it
        // just consumes the result.
        if (effective_strategy_id == STRATEGY_AUTO &&
            ror_regressor && ema_price && rolling_long) {
            int resolved = Regime_ToStrategy(state->nodes[slot].regime_state.current_regime);
            // v5.4.0 Phase 0.1 health log moved to universal classification
            // block above (v5.14.5.B.0.A). AUTO-only emission would log
            // resolved_strat=N — that field is recoverable from regime via
            // REGIME_STRATEGY_TABLE so no info loss; logs now fire for all
            // cores so operator gets regime visibility regardless of strategy.

            // Don't recurse into STRATEGY_AUTO (defensive — REGIME_STRATEGY_TABLE
            // shouldn't return AUTO, but safer to clamp).
            if (resolved != STRATEGY_AUTO && resolved != STRATEGY_NONE) {
                effective_strategy_id = (uint8_t)resolved;
            } else {
                effective_strategy_id = STRATEGY_MEAN_REVERSION;  // safe default
            }
            // v4.0.3 D11: regime change mid-position → tighten ratchet_sl
            // closer to current price (lock in any open profit ahead of
            // strategy switch). Only trigger when regime ACTUALLY changed
            // (not just hysteresis-pending) and a position is open.
            // v5.4.0 Phase 3.1: also write ratchet_tp to widen/tighten
            // TP per transition shape — restoring the legacy
            // Regime_AdjustPositions behavior that was orphaned in sharded
            // (postmortem F4: the legacy function writes pos->take_profit_price,
            // which the hot path doesn't read).
            if (new_regime != old_regime &&
                (state->oms->portfolio.active_bitmap &
                 Sharded_NodeSlotMask(slot, BITMAP_IS_SET(config->lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED)))) {
                // v5.5.4 (Class 2 / Class 9 hybrid): SL tighten on regime
                // transition. The legacy D11 path here wrote ratchet_sl
                // DIRECTLY without the v5.1.7 fee-floor cap. Symptom (user
                // 2026-04-30 paper trade history): on AUTO regime switch
                // with an open position, ratchet_sl could be set to
                // price_avg - stddev which is ABOVE entry × 0.997 in
                // low-vol regimes, causing exits at near-entry prices that
                // lose to fees ("TP firing too tight" pattern). Now route
                // through Strategy_WriteRatchetSL per-slot so the cap is
                // applied (entry × (1 - 3 × fee_rate_taker)). Same shape
                // as the strategy-specific trailing path. Iterate this
                // core's slot(s) so we have entry_price per slot for the
                // cap math.
                FPN_Binary<F> tight_sl = FPN_Sub(rolling->price_avg,
                                           rolling->price_stddev);
                int partial_on = BITMAP_IS_SET(config->lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED) ? 1 : 0;
                uint16_t my_mask = BITMAP_NODE_SLOT_MASK(slot, partial_on);
                uint16_t bm = (uint16_t)(state->oms->portfolio.active_bitmap & my_mask);
                while (bm) {
                    int pidx = __builtin_ctz(bm);
                    bm &= (uint16_t)(bm - 1);
                    Money entry_p = state->oms->portfolio.positions[pidx].entry_price;
                    if (!Money_IsZero(entry_p)) {
                        Strategy_WriteRatchetSL(state, slot, Money_FromBinary(tight_sl),  // D-170 egress
                                                  entry_p, &resolved_cfg);
                    }
                }

                // v5.4.0 Phase 3.1: TP retune per transition shape.
                // Widening transitions (RANGING→TRENDING, MILD_TREND→TRENDING)
                // ratchet TP UP to let positions run further. Tightening
                // transitions (TRENDING→RANGING) lock in profit by ratcheting
                // TP UP to a closer target (price + tight_offset). The TP
                // ratchet is max-only via Strategy_WriteRatchetTP, so a
                // tightening proposal that would *lower* TP is silently
                // dropped — this preserves the "ratchet locks gains" intent.
                bool widen = (old_regime == REGIME_RANGING && new_regime == REGIME_TRENDING) ||
                             (old_regime == REGIME_MILD_TREND && new_regime == REGIME_TRENDING);
                bool tighten = ((old_regime == REGIME_TRENDING || old_regime == REGIME_TRENDING_DOWN
                                 || old_regime == REGIME_MILD_TREND) &&
                                new_regime == REGIME_RANGING);
                if (widen) {
                    // Wider TP target: rolling avg + stddev × momentum_tp_mult.
                    // Mirrors legacy Regime_AdjustPositions RANGING→TRENDING case.
                    FPN_Binary<F> tp_offset = FPN_Mul(rolling->price_stddev,
                                                resolved_cfg.momentum_tp_mult);
                    FPN_Binary<F> wide_tp   = FPN_Add(rolling->price_avg, tp_offset);
                    Strategy_WriteRatchetTP(state, slot, Money_FromBinary(wide_tp));  // D-170 egress
                } else if (tighten) {
                    // Tighter TP target: lock in profit by ratcheting TP up
                    // to current_price + small offset. Only advances if
                    // higher than existing ratchet_tp (max-only semantics).
                    FPN_Binary<F> tight_offset = FPN_Mul(rolling->price_stddev,
                                                   FPN_FromDouble<F>(0.5));
                    FPN_Binary<F> tight_tp     = FPN_Add(rolling->price_avg, tight_offset);
                    Strategy_WriteRatchetTP(state, slot, Money_FromBinary(tight_tp));  // D-170 egress
                }
            }
        }
        // Phase 6prep sharded c13/c15: pack ML extras for ML cores. Non-ML cores
        // get nullptr (the dispatcher passes it through and ML_BuildParameters
        // never runs anyway). Stack-allocated; ML_BuildParameters dereferences
        // and copies what it needs synchronously.
        MLBuildContext ml_ctx{};
        void* dispatch_ctx = nullptr;
        if (effective_strategy_id == STRATEGY_ML) {
            ml_ctx.model_handle   = state->nodes[slot].model_handle;
            ml_ctx.ensemble_zoo   = state->nodes[slot].ensemble_handle;  // v5.10.0a.G.5 — nullptr-safe; single-zoo when null
            ml_ctx.current_regime_id = state->nodes[slot].regime_state.current_regime;  // v5.10.0a.G.7
            ml_ctx.confidence     = &state->nodes[slot].confidence;
            ml_ctx.out_prediction = &state->nodes[slot].staged_prediction;
            ml_ctx.out_confidence = &state->nodes[slot].last_confidence;
            // v4.0 train-serve parity: pass through ROR + EMA from engine
            // slow path so Regime_ComputeSignals can produce the full
            // feature set the backtest path produces during training.
            ml_ctx.ror_regressor  = (void*)ror_regressor;
            ml_ctx.ema_price      = (void*)ema_price;
            // v4.3 — feature-pack expansion state from the engine slow path
            ml_ctx.rolling_medium  = (void*)rolling_medium;
            ml_ctx.rolling_baseline= (void*)rolling_baseline;
            ml_ctx.cumdelta_state  = (void*)cumdelta_state;
            ml_ctx.tick_rate_state = (void*)tick_rate_state;
            ml_ctx.timestamp_us    = timestamp_us;
            // v4.5 Wave 1 — D.1/D.2/D.4 state passthrough
            ml_ctx.book_imb_history  = (void*)book_imb_history;
            ml_ctx.flow_state        = (void*)flow_state;
            ml_ctx.large_trade_state = (void*)large_trade_state;
            // v4.6 Wave 2 — D.3 spread state + current values
            ml_ctx.spread_state       = (void*)spread_state;
            ml_ctx.current_spread     = current_spread;
            ml_ctx.current_mid_price  = current_mid_price;
            // v5.9.0b — ML observability pass-through (V5_9_AUDIT-#2, #3).
            // Caller-owned per-core storage; ML_BuildParameters reads/writes
            // through these pointers + the entry log emitter reads them at
            // fill time.
            // v5.15.5.B.3 — model_load_failed migrated from NodeContext int (was display_meta after .B.2)
            // to a node_state_flags bitmap bit. mctx field changed from int* to int-by-value;
            // copy current bit state into the int.
            ml_ctx.model_load_failed           = NODE_STATE_FLAG_IS_SET(state->nodes[slot], MODEL_LOAD_FAILED) ? 1 : 0;
            ml_ctx.model_corrupt               = NODE_STATE_FLAG_IS_SET(state->nodes[slot], MODEL_CORRUPT) ? 1 : 0;  // v5.15.5.E.0.10 A6 (D-221)
            ml_ctx.last_ml_critical_log_us     = &state->display_meta[slot].last_ml_critical_log_us;
            ml_ctx.out_threshold               = &state->display_meta[slot].last_ml_threshold;
            ml_ctx.out_effective_threshold     = &state->display_meta[slot].last_ml_effective_threshold;
            ml_ctx.nan_feature_events_total    = &state->display_meta[slot].nan_feature_events_total;
            ml_ctx.nan_prediction_events_total = &state->display_meta[slot].nan_prediction_events_total;
            // v5.9.1 — wire SHALT pointer through MLBuildContext so the
            // confidence hard-block path can attribute SHALT_LOW_CONFIDENCE.
            // Same address the dispatcher passes via its strategy_halt_reason
            // parameter; ml_ctx is the only path ML_BuildParameters has into
            // it without changing the dispatcher signature.
            ml_ctx.out_strategy_halt_reason    = &state->nodes[slot].strategy_halt_reason;
            // v5.11.18 main — per-core feature mask. Pointer to the cfg
            // field directly; no copy. Features_PackAll inside
            // ML_BuildParameters reads through this pointer when non-null.
            // When operator hasn't set core_<slot>_feature_mask in cfg,
            // the cfg parser at v5.11.18a defaults to 0xFFFF..F (all
            // features enabled); pointer is non-null but mask = all-on
            // produces bytewise-identical output to pre-v5.11.18.
            ml_ctx.feature_mask = &resolved_cfg.node_feature_mask[slot];
            // v5.14.9.B.0 — pointer to the per-core slow-path gate cache
            // populated above by SLOW_PATH_GATE_AUTOPOPULATE_PER_NODE.
            // ML_BuildParameters reads gate predicates via BITMAP_IS_SET.
            ml_ctx.gate_state = (void*)&state->nodes[slot].gate_state;
            // v5.14.9.B — soft risk degradation ladder factor sink. ML_BuildParameters
            // writes per-cycle factor; ShardedSnapshot mirrors to PerNodeSnap.
            ml_ctx.out_confidence_factor = &state->nodes[slot].last_confidence_factor;
            // v5.14.1.G — portfolio turnover wire. Pointer to per-core
            // NodeContext.turnover; ML_BuildParameters' buy-side blend
            // populator pushes top-K mask each cycle. void* in MLBuildContext
            // avoids include cycle (StrategyParameters.hpp doesn't include
            // ControllerEventLoop.hpp). topk read from resolved cfg.
            ml_ctx.turnover_state = (void*)&state->nodes[slot].turnover;
            ml_ctx.turnover_topk  = resolved_cfg.confidence_turnover_topk;
            // v5.13.0.B — sell-side ML wiring. Reset per-cycle; ML_Build-
            // Parameters writes the blended exit_predictor probability when
            // BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_USE_EXIT_MODEL) && exit_predictor_count > 0. Slow-path body
            // post-RebuildOneCore reads + acts on the value (fires OMS submit
            // if above cfg.exit_threshold and any positions are open).
            state->nodes[slot].last_exit_prediction       = 0.0;
            state->nodes[slot].last_exit_dominant_horizon = -1;
            ml_ctx.out_exit_prediction       = &state->nodes[slot].last_exit_prediction;
            ml_ctx.out_exit_dominant_horizon = &state->nodes[slot].last_exit_dominant_horizon;
            // 2026-08-22 (B-12 exit-skip-when-flat) — thread the REAL position
            // state (Class-13 sub-C: never substitute a constant). The exit-
            // submit consumer discards the prediction when flat, so
            // ML_BuildParameters skips the exit-arm predicts entirely on
            // flat cycles. Same mask + partial_on source as that consumer
            // (EngineCommon exit-submit block).
            {
                int oms_partial_on = BITMAP_IS_SET(state->oms->oms_state_flags,
                                                   tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
                ml_ctx.node_has_open_position =
                    (state->oms->portfolio.active_bitmap &
                     BITMAP_NODE_SLOT_MASK(slot, oms_partial_on)) != 0;
            }
            // v5.15.5.A.6 — buy-side per-horizon barrier observability sinks.
            // Reset per-cycle dispatch fields; shadow event counter stays
            // monotonic (don't reset across cycles).
            state->nodes[slot].last_buy_dominant_horizon = -1;
            state->nodes[slot].last_barrier_mode_used    = 0;  // LEGACY default
            ml_ctx.out_buy_dominant_horizon   = &state->nodes[slot].last_buy_dominant_horizon;
            ml_ctx.out_barrier_mode_used      = &state->nodes[slot].last_barrier_mode_used;
            ml_ctx.barrier_shadow_event_count = &state->display_meta[slot].barrier_shadow_event_count;
            dispatch_ctx = &ml_ctx;
        }
        // v4.0.4: stash the resolved strategy for GUI display. For non-AUTO
        // cores this just mirrors strategy_id; for AUTO it's the regime-
        // resolved concrete strategy.
        state->nodes[slot].resolved_strategy_id = effective_strategy_id;

        // v5.4.0 Phase 2.1 — Adapt before BuildParameters. Per the strategy
        // contract (StrategyInterface.hpp), Adapt mutates per-core state and
        // BuildParameters reads it. No-op when state is null (AUTO cores
        // pre-Phase 3, NONE cores). portfolio_delta passed as zero on the
        // sharded slow path — pre-v5.4 strategies that consumed it (legacy
        // path) are out of band; sharded uses pnl_feeder for the
        // rebuild-time feedback loop (the D10 adaptive-shift block above).
        // v5.4.0 Phase 2.4 — also pass ema_price (per-tick replicated by
        // producer) so EmaCross's Adapt branch can update its prev_ema +
        // last_ema_slope tracking.
        // v5.6.2 / MOVED HERE at E.1.2 D-421: reset strategy_halt_reason every rebuild.
        // Strategies set this to a SHALT_* code when zero-gating for strategy-internal
        // reasons; SHALT_OK = no veto.
        //
        // WHY IT MOVED: this line used to sit ~59 lines BELOW the Strategy_BuildParameters
        // call that writes the field — unconditionally, same straight-line block, no
        // intervening control flow — so it clobbered the dispatcher's own output on every
        // pass. 17 of the 20 FOREACH_SHALT codes could therefore never be observed; only
        // SHALT_RECOVERY and SHALT_EXIT_PREDICTED (both written after the old reset point)
        // ever survived, which is why the panel showed something and the hole went unnoticed
        // from bc37c62 (2026-04-30) until now. StrategyInterface.hpp already specified the
        // correct placement — "reset to SHALT_OK at the top of each rebuild" — and the code
        // contradicted its own contract.
        //
        // NOTE the asymmetry with its former neighbour: halt_reason's reset stays where it
        // is, BELOW, because ITS only producers (zero_gate) are below it. The two lines look
        // interchangeable and have opposite correct placements — do not re-merge them.
        //
        // ENFORCED since D-421: tools/check_reset_before_producer.py pins BOTH resets against
        // their producers (pre-commit Check M2 on this file + a HARD doc-sweep row). A comment
        // is the weakest guard for an ordering property; that is why this one has a tool behind
        // it now. Careful reading the note above: "stays BELOW" is about position relative to the
        // OTHER reset, not to its producers — both resets are reset-FIRST. I mis-encoded exactly
        // that when writing the rule, and the tool's first run caught it.
        state->nodes[slot].strategy_halt_reason = SHALT_OK;

        Strategy_AdaptPerCore(
            state, slot, effective_strategy_id,
            rolling->price_avg,         // current_price proxy (slow-path doesn't see live tick)
            FPN_Zero<F>(),              // portfolio_delta — fed via pnl_feeder above, not here
            state->oms->portfolio.active_bitmap,
            &resolved_cfg,
            (const FPN_Binary<F>*)ema_price    // v5.4.0 Phase 2.4 — for EmaCross
        );

        // v5.15.5.F.4c.3 WIP2c.2 — per-core single-param sig (Class 25 closure).
        // resolved_cfg already merged per-core overrides via ResolveForCore at
        // slow-path entry; its .nodes[slot] reflects the post-resolve view.
        // poll_interval pre-resolved from global cfg as scalar arg.
        // 2026-08-22 (latency deep-dive #4) — ML_INFER attribution bracket.
        // Samples ONLY when dispatch_ctx is wired (ML nodes); nested inside
        // the caller's REBUILD bracket by design (sub-attribution, not
        // additive). The per-node branch is effectively static (a node's
        // strategy is fixed between swaps) — predicted after the first cycle.
        // E.1.2.E leaf 7 — zero the ML_PREDICT accumulators so what we read
        // after the dispatch is THIS cycle's inference cost, not a running
        // total. Thread-local + single-writer (this node's slow thread), so no
        // cross-thread reset hazard.
        if (dispatch_ctx) { tt::ml_predict_cycles = 0; tt::ml_predict_count = 0; }
        uint64_t _ml_infer_t0 = dispatch_ctx ? __rdtsc() : 0;
        Strategy_BuildParameters(
            effective_strategy_id,
            rolling,
            &resolved_cfg.nodes[slot],
            state->nodes[slot].allocated_balance,
            &state->nodes[slot].pending_params,
            rolling_long,
            dispatch_ctx,
            state->nodes[slot].strategy_state,   // v5.4.0 Phase 2.1 — typed-cast inside dispatcher
            &state->nodes[slot].strategy_halt_reason,  // v5.6.2 — dispatcher writes
                                                       // SHALT_* codes for fee-floor /
                                                       // cost-gate / no-signal paths.
            now_us,  // v5.14.1.B.2 (PARITY-001) — threaded through to ML_BuildParameters
                     // for composite confidence freshness. Already plumbed to this fn
                     // (param :1951) since v5.12.1.B clock hoist; live = clock_gettime
                     // at slow-path entry, backtest = tick.timestamp (deterministic).
            (int)config->poll_interval   // WIP2c.2 — caller-resolved global; per-node consumer
                                          // reads tick→time conversion via this scalar arg.
        );
        if (dispatch_ctx) {
            uint64_t _ml_infer_t1 = __rdtsc();
            NodeLatencyStats_Sample(
                &state->display_meta[slot].slow_path_breakdown[tt::SP_SECTION_ML_INFER],
                _ml_infer_t1 - _ml_infer_t0, _ml_infer_t1);
            // The inference-only half. Sampled from the accumulator the three
            // Model_Predict* entry points fed during the dispatch above, so it
            // covers every predict site including any not enumerated here.
            // ML_INFER - ML_PREDICT = the cost no backend swap can remove.
            NodeLatencyStats_Sample(
                &state->display_meta[slot].slow_path_breakdown[tt::SP_SECTION_ML_PREDICT],
                tt::ml_predict_cycles, _ml_infer_t1);
        }

        // 2026-08-22 — SHALT_WARMING attribution (no-signal-investigation #6).
        // During warmup every strategy runs starved and lands in the
        // NO_SIGNAL catch-all — indistinguishable from "model ran and
        // declined", which is exactly the ambiguity that mis-routed the
        // 2026-08-22 investigation. Upgrade ONLY the catch-all (a specific
        // code like FEE_FLOOR/RECOVERY survives — more actionable than the
        // blanket warmup fact). Producer-side write BELOW the dispatcher,
        // consistent with the D-421 reset-before-producer ordering (the
        // reset stays above the dispatch; this is a refining producer).
        {
            int _wmin = (int)config->min_warmup_samples;
            if (_wmin <= 0) _wmin = 64;  // engine default (matches the warmup-log + ShardedSnapshot fallbacks)
            if (state->nodes[slot].strategy_halt_reason == SHALT_NO_SIGNAL
                && rolling->count < _wmin) {
                state->nodes[slot].strategy_halt_reason = SHALT_WARMING;
            }
        }

        // v4.0.3 D9: clear ratchet_sl when no position active on this core,
        // so stale trailing state from previous trade doesn't leak into the
        // next entry. Engine slow-path code below SETS ratchet_sl when a
        // position is active and trailing should kick in.
        // Partials-aware: core's portfolio slot(s) come from the helper
        // (slot N or 2N+0/2N+1 depending on partial_exit_enabled).
        bool slot_active = (state->oms->portfolio.active_bitmap &
                             Sharded_NodeSlotMask(slot, BITMAP_IS_SET(config->lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED))) != 0;
        if (!slot_active) {
            state->nodes[slot].pending_params.ratchet_sl = Money_Zero();
            // A19 (.E.0.10): clear ratchet_tp SYMMETRICALLY — a stale TP-ratchet from a prior
            // trade must not leak into the next entry's effective_tp = Money_Max(tp, ratchet_tp)
            // (H22 per-trade purity; the SL side already clears above — the TP side was the
            // missing half, a cross-trade representation leak per representation-migration-completeness.md).
            state->nodes[slot].pending_params.ratchet_tp = Money_Zero();
        }

        // v5.4.0 Phase 2.2: strategy-specific exit-adjust for cores with
        // open slots. Writes ratchet_sl via Strategy_WriteRatchetSL (fee-floor
        // capped). Coexists with the generic EventLoop_TrailingSLRatchetOneCore
        // — both write the same field with max-only semantics; the higher
        // proposal wins.
        if (slot_active) {
            Strategy_ExitAdjustPerCore(state, slot, effective_strategy_id,
                                        Money_FromBinary(rolling->price_avg),  // slow-path price proxy (D-170)
                                        rolling, &resolved_cfg);
        }

        // v4.0.3 cross-cutting checks applied uniformly across all strategies.
        // Each is a "zero-gate if violated" filter — preserves the strategy's
        // intended TP/SL/qty but disables the entry trigger. Halt reasons are
        // tracked per-core for GUI display.
        //
        // v5.8.3: halt_reason is now a HALT_* enum from FOREACH_HALT_REASON
        // (StrategyInterface.hpp). See registry there for code semantics.
        // Correct HERE (unlike its former neighbour): halt_reason's only producers are the
        // zero_gate lambda + the imbalance check, both BELOW this line. strategy_halt_reason's
        // reset moved ABOVE the dispatch block at E.1.2 D-421 — see the comment there for why
        // the two are not interchangeable.
        state->nodes[slot].halt_reason = HALT_OK;

        // v5.12.1.A.3 — post-flatten recovery refusal. Gated on
        // recovery_until_us > 0 so the common case (no recovery active)
        // pays just one atomic load (~5ns); active case adds one clock
        // read (~50ns) only while in the recovery window. Auto-clears
        // via EventLoop_TryClearRecovery once the deadline elapses.
        //
        // v5.12.1.B (clock hoist): if caller passed now_us != 0, reuse
        // it. Saves ~50ns/cycle in flatten-recovery window. Default 0
        // (tests, legacy paths) → fall back to internal clock read.
        // v5.12.1.B.3 — staleness-gate post-pass. Fills GateParameters
        // fields from cfg uniformly across all strategies. Branchless: flag
        // bit is OR'd in; max_age value is unconditional. Hot path's
        // branchless mask check uses these.
        if (BITMAP_IS_SET(config->gate_cfg_flags, MASK_GATE_CFG_PARAM_STALENESS_GATE_ENABLED)) {
            state->nodes[slot].pending_params.flags
                |= GATE_FLAG_STALENESS_ENABLED;
        }
        state->nodes[slot].pending_params.param_max_age_ticks
            = config->param_max_age_ticks;

        {
            uint64_t recovery_until = state->oms->recovery_until_us.load(
                std::memory_order_acquire);
            if (recovery_until > 0) {
                uint64_t effective_now_us = (now_us != 0) ? now_us :
                    (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                if (effective_now_us < recovery_until) {
                    // Refuse new entries this cycle. Block via flag —
                    // works for buy-above (Momentum) AND buy-below
                    // (DIP/MR) strategies. SHALT_RECOVERY surfaces in
                    // GUI / health log for operator forensics.
                    state->nodes[slot].pending_params.flags
                        |= GATE_FLAG_BUY_BLOCKED;
                    state->nodes[slot].strategy_halt_reason = SHALT_RECOVERY;
                } else {
                    // Recovery window elapsed — clear flatten state so
                    // a future staleness event can re-fire. CAS-based;
                    // only one slow-path thread wins the clear.
                    EventLoop_TryClearRecovery(state->oms, effective_now_us);
                }
            }
        }
        // v5.5.1 (Bug B-FLAT — addressed the "latent zero_gate bug" called out
        // in the Track E.3 comment below). Pre-fix: zero_gate set
        // bg_price_threshold = 0 to disable entries. This works for buy-below
        // strategies (price < 0 is never true). For BUY_ABOVE (Momentum),
        // BG_Evaluate's "price > 0" check is ALWAYS true, so zero_gate did
        // not actually block momentum — entries fired on EVERY tick. Combined
        // with the per-core budget cap zeroing trade_size, momentum cores
        // emitted a stream of $0-qty entries that immediately exited at
        // entry price (FLAT in trade history, $0 In/Out columns).
        //
        // Fix: also set GATE_FLAG_BUY_BLOCKED. The flag mechanism (Track E.3
        // pattern) works for both directions. Keep the bg_price_threshold = 0
        // write for backward compat with buy-below paths and for any
        // downstream code that special-cases the zero threshold.
        auto zero_gate = [&](uint8_t reason) {
            state->nodes[slot].pending_params.bg_price_threshold = Money_Zero();
            state->nodes[slot].pending_params.flags |= GATE_FLAG_BUY_BLOCKED;
            if (state->nodes[slot].halt_reason == HALT_OK)  // first reason wins
                state->nodes[slot].halt_reason = reason;
        };

        // Track E.3 (2026-04-26) — book_imbalance veto via flag (not
        // bg_price_threshold=0). The flag mechanism works for buy-above
        // (momentum) AND buy-below strategies; zero_gate above only works
        // for buy-below because bg_price_threshold=0 always satisfies
        // "price > 0" for momentum (latent zero_gate bug, pre-existing —
        // separate fix). Flag is recomputed every rebuild, so when
        // imbalance recovers Strategy_BuildParameters' fresh `out->flags`
        // assignment naturally drops the BLOCKED bit.
        if (book_imbalance_blocked) {
            state->nodes[slot].pending_params.flags |= GATE_FLAG_BUY_BLOCKED;
            if (state->nodes[slot].halt_reason == HALT_OK)
                state->nodes[slot].halt_reason = HALT_IMBALANCE;
        }

        // Phase 2.2: per-core budget enforcement. Clamp the strategy's
        // requested qty against remaining allocation, and zero-gate
        // entirely when budget is exhausted (open_notional >= allocated).
        // The clamp is the meaningful path under multi-position-per-core;
        // the halt fires when a core is already fully deployed and a
        // strategy still wants to enter. Today (single-position-per-core,
        // sizing = full allocation per trade) the clamp is mostly defensive
        // — catches bugs where intended_qty gets corrupted to a huge value
        // — and it'll matter structurally when multi-position-per-core lands.
        // All FPN_Binary-pure, slow path. NOTE: FPN_Binary<F> is signed — we explicitly
        // compare open_notional >= allocated (rather than relying on
        // FPN_SubSat saturating to zero on underflow, which it doesn't).
        {
            Money alloc       = state->nodes[slot].allocated_balance;
            Money open_n      = state->nodes[slot].node_open_notional;
            Money entry_price = state->nodes[slot].pending_params.bg_price_threshold;
            if (Money_Ge(open_n, alloc)) {
                // Fully or over-deployed — no slot-room for another entry.
                // Zero-gate with HALT_NODE_BUDGET. Trade size also clamped
                // to zero so any downstream consumer of trade_size sees
                // an honest zero rather than a stale value.
                state->nodes[slot].pending_params.trade_size = Money_Zero();
                zero_gate(HALT_NODE_BUDGET);
            } else if (!Money_IsZero(entry_price)) {
                // Budget remaining is positive — clamp qty to
                // (budget_remaining / entry_price). Under single-position-
                // per-core today, open_n is 0 when this branch runs (we hit
                // the GE branch above when deployed), so budget_remaining
                // == alloc and the clamp is a no-op. Multi-position-per-core
                // would land here with partial budget, producing a real clamp.
                Money budget_remaining = Money_Sub(alloc, open_n);  // > 0 by branch
                Money max_qty = Money_Div(budget_remaining, entry_price);
                state->nodes[slot].pending_params.trade_size =
                    Money_Min(state->nodes[slot].pending_params.trade_size, max_qty);
            }
        }

        // Phase 3 — per-core kill switch: MTM peak/drawdown tracking + trip
        // evaluation. Realized P&L from oms->realized_pnl already includes
        // closed exits booked by this core; unrealized comes from MTM walking
        // open positions (only this core's slot under single-position-per-core).
        // current_value = allocated + realized + unrealized.
        // peak ratchets up via FPN_Max; trip fires when dd_pct exceeds
        // threshold AND drop exceeds min_kill_loss floor (so tiny allocs
        // don't trip on rounding noise).
        //
        // MTM is best-effort: if current_price is null (legacy callers / tests
        // not passing it), we fall back to realized-only — peak/dd computed
        // without the unrealized term. enable_mtm_kill_switch=0 forces this
        // realized-only mode regardless of whether current_price was passed.
        {
            Money alloc     = state->nodes[slot].allocated_balance;
            Money realized  = state->nodes[slot].node_realized;
            Money unrealized = Money_Zero();
            const Money* px_in = (const Money*)current_price;
            // Partials-aware MTM walk: under partials, core c's positions
            // live in slots 2c and 2c+1 (one Position per leg, each with
            // independent qty). Sum unrealized across both. Without
            // partials, only slot c is walked.
            if (BITMAP_IS_SET(config->risk_cfg_flags, MASK_RISK_CFG_MTM_KILL_SWITCH_ENABLED) && px_in && !Money_IsZero(*px_in)) {
                uint16_t mask = Sharded_NodeSlotMask(slot, BITMAP_IS_SET(config->lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED));
                uint16_t bm   = state->oms->portfolio.active_bitmap & mask;
                while (bm) {
                    int s = __builtin_ctz(bm);
                    bm &= (uint16_t)(bm - 1);
                    Position<F>& pos = state->oms->portfolio.positions[s];
                    unrealized = Money_Add(unrealized,  // D-190 single-source (mark gross via the canonical helper)
                                           Money_FillGross(pos.entry_price, *px_in, pos.quantity));
                }
            }
            Money current_value = Money_Add(alloc, Money_Add(realized, unrealized));
            // Peak ratchet (branchless via FPN_Max). Initialize to alloc on
            // first sight if peak is still zero (first rebuild after init).
            if (Money_IsZero(state->nodes[slot].node_peak_balance)) {
                state->nodes[slot].node_peak_balance = alloc;
            }
            state->nodes[slot].node_peak_balance =
                Money_Max(state->nodes[slot].node_peak_balance, current_value);
            // Drawdown computation. Skip if peak is zero (defensive — should
            // never happen after the init bump above, but handles a freshly
            // reset state). dd = (peak - current) / peak.
            Money drop = Money_Sub(state->nodes[slot].node_peak_balance, current_value);
            if (Money_Gt(drop, Money_Zero()) &&
                Money_Gt(state->nodes[slot].node_peak_balance, Money_Zero())) {
                state->nodes[slot].node_dd_pct = Money_Div(drop,
                    state->nodes[slot].node_peak_balance);
            } else {
                state->nodes[slot].node_dd_pct = Money_Zero();
            }
            // Trip evaluation. Threshold: per-core override if set, else
            // global max_drawdown_pct. Trip ALSO requires drop > min_kill_loss
            // so a tiny allocation doesn't trip on rounding noise.
            if (!NODE_STATE_FLAG_IS_SET(state->nodes[slot], KILL_TRIPPED)) {
                // E.1.1 ③/B — reads nodes[slot].max_drawdown_pct (raw-copied from node_max_drawdown_pct[slot]
                // in PopulateCoresFromFlat, 0=inherit preserved) — byte-identical to the legacy array read.
                Money threshold = !Money_IsZero(config->nodes[slot].max_drawdown_pct)
                    ? config->nodes[slot].max_drawdown_pct
                    : config->max_drawdown_pct;
                if (Money_Gt(state->nodes[slot].node_dd_pct, threshold) &&
                    Money_Gt(drop, config->min_kill_loss)) {
                    NODE_STATE_FLAG_SET(state->nodes[slot], KILL_TRIPPED);
                    state->nodes[slot].node_ks_trips_total++;
                    double dd_pct_d  = Money_ToDouble(state->nodes[slot].node_dd_pct) * 100.0;
                    double drop_d    = Money_ToDouble(drop);
                    double peak_d    = Money_ToDouble(state->nodes[slot].node_peak_balance);
                    double current_d = Money_ToDouble(current_value);
                    fprintf(stderr, "[sharded] NODE KILL: node %d tripped — "
                            "dd=%.2f%% drop=$%.2f peak=$%.2f current=$%.2f\n",
                            slot, dd_pct_d, drop_d, peak_d, current_d);
                    // 2A — alert via Notify subsystem alongside stderr log.
                    // Per-kind cooldown (NK_NODE_KILL_TRIP=10) collapses
                    // back-to-back trips on the same core to one alert per
                    // window. Backtest leaves g_notify null → no-op.
                    if (g_notify) {
                        char body[256];
                        snprintf(body, sizeof(body),
                                 "Node %d kill tripped: dd=%.2f%% drop=$%.2f "
                                 "peak=$%.2f current=$%.2f. Entries halted on "
                                 "this node until manual reset.",
                                 slot, dd_pct_d, drop_d, peak_d, current_d);
                        Notify_Send(g_notify, NOTIFY_ALERT, NK_NODE_KILL_TRIP,
                                    "Per-node kill switch tripped", body);
                    }
                }
            }
            if (NODE_STATE_FLAG_IS_SET(state->nodes[slot], KILL_TRIPPED)) {
                zero_gate(HALT_NODE_KILL);
            }
        }

        // SL COOLDOWN: decrement counter; if still active, zero-gate.
        if (state->nodes[slot].sl_cooldown_remaining > 0) {
            state->nodes[slot].sl_cooldown_remaining--;
            zero_gate(HALT_SL_COOLDOWN);
        }
        // SPACING: zero-gate if proposed entry too close to last entry.
        // SPIKE-RELAXATION (D5): when current volume is a spike (>= max ×
        // spike_threshold), reduce the spacing requirement by
        // spike_spacing_reduction. Real volume bursts are valid second-entry
        // opportunities — don't artificially space them out.
        ControllerConfig<F> spacing_cfg = resolved_cfg;
        if (!FPN_IsZero(rolling->volume_max) &&
            !FPN_IsZero(resolved_cfg.spike_threshold)) {
            FPN_Binary<F> ratio_thresh = FPN_Mul(rolling->volume_max,
                FPN_DivNoAssert(FPN_FromDouble<F>(1.0), resolved_cfg.spike_threshold));
            // Note: spike active when current_volume × spike_threshold >= max.
            // Equivalent: current >= max / spike_threshold.
            // We check the latest volume_avg as the "current" representative.
            if (FPN_GreaterThanOrEqual(rolling->volume_avg, ratio_thresh)) {
                // A24 (.E.0.10, D-211 option c): relax the per-NODE slice nodes[slot] — the
                // gate Strategy_SpacingOk (:2980) + the GUI diag (:2974, A32 display↔exec
                // inversion) both read spacing_cfg.nodes[slot]. Writing the flat field left
                // both the gate inert AND the diag lying. spacing_cfg is the scratch copy
                // that isolates the relaxation to this spacing check (kept deliberately).
                spacing_cfg.nodes[slot].spacing_multiplier = FPN_Mul(
                    spacing_cfg.nodes[slot].spacing_multiplier,
                    resolved_cfg.spike_spacing_reduction);
            }
        }
        // v5.6.3: capture spacing comparands for GUI diagnostic readout.
        // Single-source rule — these are the SAME values
        // Strategy_SpacingOk reads internally, exposed for display.
        {
            Money a = state->nodes[slot].pending_params.bg_price_threshold;
            Money b = state->nodes[slot].last_entry_price;
            Money abs_dist = Money_Ge(a, b)
                ? Money_Sub(a, b) : Money_Sub(b, a);
            FPN_Binary<F> min_dist = FPN_Mul(rolling->price_stddev,
                                       spacing_cfg.nodes[slot].spacing_multiplier);  // A24: read the slice the gate reads (A32 display↔exec fix)
            state->display_meta[slot].diag_spacing_actual = Money_ToBinary(abs_dist);  // display mirror stays binary
            state->display_meta[slot].diag_spacing_floor  = min_dist;
        }
        if (!Strategy_SpacingOk(state->nodes[slot].pending_params.bg_price_threshold,
                                 state->nodes[slot].last_entry_price,
                                 rolling, &spacing_cfg.nodes[slot])) {
            zero_gate(HALT_SPACING);
        }
        // VWAP gate: forces entries below VWAP — buy retracements, not pumps.
        if (!FPN_IsZero(resolved_cfg.vwap_offset) && !FPN_IsZero(rolling->vwap)) {
            FPN_Binary<F> vwap_threshold = FPN_Sub(rolling->vwap,
                FPN_Mul(rolling->vwap, resolved_cfg.vwap_offset));
            // v5.6.3: capture both sides for GUI.
            state->display_meta[slot].diag_vwap_actual    =
                Money_ToBinary(state->nodes[slot].pending_params.bg_price_threshold);
            state->display_meta[slot].diag_vwap_threshold = vwap_threshold;
            if (Money_Gt(state->nodes[slot].pending_params.bg_price_threshold,
                                 Money_FromBinary(vwap_threshold))) {
                zero_gate(HALT_VWAP);
            }
        }
        // LONG-SLOPE gate: blocks buys in confirmed downtrends.
        if (!FPN_IsZero(resolved_cfg.min_long_slope) && rolling_long &&
            !FPN_IsZero(rolling_long->price_avg)) {
            FPN_Binary<F> long_rel_slope = FPN_DivNoAssert(rolling_long->price_slope,
                                                     rolling_long->price_avg);
            // v5.6.3: capture for GUI.
            state->display_meta[slot].diag_long_slope     = long_rel_slope;
            state->display_meta[slot].diag_long_slope_min = resolved_cfg.min_long_slope;
            if (FPN_LessThan(long_rel_slope, resolved_cfg.min_long_slope)) {
                zero_gate(HALT_LONG_SLOPE);
            }
        }
        // VOLUME DELTA gate: blocks heavy dumps.
        if (!FPN_IsZero(resolved_cfg.min_buy_delta)) {
            // v5.6.3: capture both sides regardless of pass/fail so the
            // GUI shows current state (display invariant: always show
            // when cfg enabled).
            state->display_meta[slot].diag_volume_delta     = rolling->volume_delta;
            state->display_meta[slot].diag_volume_delta_min = resolved_cfg.min_buy_delta;
            if (FPN_LessThan(rolling->volume_delta, resolved_cfg.min_buy_delta)) {
                zero_gate(HALT_VOL_DELTA);
            }
        }
        // MIN STDDEV gate: skip dead markets.
        if (!FPN_IsZero(resolved_cfg.min_stddev_pct) && !FPN_IsZero(rolling->price_avg)) {
            FPN_Binary<F> stddev_ratio = FPN_DivNoAssert(rolling->price_stddev,
                                                    rolling->price_avg);
            // v5.6.3: capture for GUI.
            state->display_meta[slot].diag_stddev_pct     = stddev_ratio;
            state->display_meta[slot].diag_stddev_pct_min = resolved_cfg.min_stddev_pct;
            if (FPN_LessThan(stddev_ratio, resolved_cfg.min_stddev_pct)) {
                zero_gate(HALT_MIN_STDDEV);
            }
        }
        // v5.6.3: capture tp_pct + fee floor for GUI. Same formula as
        // the dispatcher's fee-floor BUY_BLOCKED path
        // (in StrategyParameters.hpp). Capture here so the
        // collapsing-header readout shows actual vs floor regardless
        // of whether BUY_BLOCKED fired.
        {
            // v5.15.5.F.4d.1.B.8 — Class 26 sub-shape B fix: per-core fee_rate_taker
            // (UNINDEXED-GLOBAL closure for display↔execution divergence at fee-floor diag).
            // resolved_cfg.fee_rate_taker stays at GLOBAL post-ResolveForCore (fee_rate_taker NOT
            // in ControllerConfig.hpp's PER_NODE_OVERRIDE_FIELDS); per-core value lives at
            // resolved_cfg.nodes[slot].fee_rate_taker. Sister-pattern: the Strategy_TpFloor call above
            // correctly uses &resolved_cfg.nodes[slot] — proven right shape.
            Money fee_taker = !Money_IsZero(resolved_cfg.nodes[slot].fee_rate_taker)
                ? resolved_cfg.nodes[slot].fee_rate_taker : resolved_cfg.nodes[slot].fee_rate;
            state->display_meta[slot].diag_tp_pct_actual =
                Money_ToBinary(state->nodes[slot].pending_params.tp_pct);
            state->display_meta[slot].diag_tp_pct_floor =
                Money_ToBinary(Money_Mul(fee_taker, Money_FromInt(3)));
        }
        // FEE FLOOR: ratchet TP up so it clears at least
        // entry × fee_rate × fee_floor_mult. Round-trip fees are 2×fee_rate,
        // so fee_floor_mult=5 means TP must clear ~2.5× round-trip fees + margin.
        // No-op when bg_price_threshold is zero (no entry), or
        // fee_floor_mult/fee_rate is zero.
        if (!Money_IsZero(state->nodes[slot].pending_params.bg_price_threshold)) {
            Money entry = state->nodes[slot].pending_params.bg_price_threshold;
            Money current_tp = state->nodes[slot].pending_params.sg_take_profit_price;
            // tp_amount = current_tp - entry; if it's negative that's already broken
            // (means strategy set TP below entry — leave alone, it's strategy's bug).
            if (Money_Gt(current_tp, entry)) {
                Money tp_amount = Money_Sub(current_tp, entry);
                Money floored = Strategy_TpFloor(entry, tp_amount, &resolved_cfg.nodes[slot]);
                if (Money_Gt(floored, tp_amount)) {
                    state->nodes[slot].pending_params.sg_take_profit_price =
                        Money_Add(entry, floored);
                }
            }
        }
        // Mirror the pack's TP/SL/qty into the controller-side intended_*
        // fields so OnEvent uses the freshly computed values when the next
        // entry fires. This ties the strategy output to the entry handler.
        state->nodes[slot].intended_tp  = state->nodes[slot].pending_params.sg_take_profit_price;
        state->nodes[slot].intended_sl  = state->nodes[slot].pending_params.sg_stop_loss_price;
        state->nodes[slot].intended_qty = state->nodes[slot].pending_params.trade_size;
        NODE_STATE_FLAG_SET(state->nodes[slot], DIRTY);

        // v5.6.6: gate-state edge-trigger health log emit. Pack the four
        // hot-path-relevant fields into a single uint16_t and compare
        // against the previous packed state. Emit cat="gate" only on
        // transition. This means MB/min during steady state (no log
        // traffic) and a single line per gate-state flip — easy to grep
        // post-hoc when diagnosing missed trades.
        //
        // Pack layout:
        //   bits  0..3 : halt_reason          (clamped to 15)
        //   bits  4..7 : strategy_halt_reason (clamped to 15)
        //   bit   8     : BUY_BLOCKED
        //   bit   9     : permission
        if (tt::Health_LogEnabled(tt::HEALTH_INFO)) {
            uint8_t  hr   = state->nodes[slot].halt_reason          & 0x0F;
            uint8_t  shr  = state->nodes[slot].strategy_halt_reason & 0x0F;
            uint8_t  bb   = (state->nodes[slot].pending_params.flags
                              & GATE_FLAG_BUY_BLOCKED) ? 1 : 0;
            uint8_t  perm = state->nodes[slot].core
                ? __atomic_load_n(&state->nodes[slot].core->permission,
                                   __ATOMIC_ACQUIRE)
                : 0;
            uint16_t packed = (uint16_t)(hr | (shr << 4)
                                          | (bb   << 8) | (perm << 9));
            if (packed != state->display_meta[slot].prev_gate_log_state) {
                tt::Health_Log(tt::HEALTH_INFO, "gate", slot,
                    "halt=%u shalt=%u blocked=%u perm=%u "
                    "gate=%g price=%g",
                    (unsigned)hr, (unsigned)shr,
                    (unsigned)bb, (unsigned)perm,
                    Money_ToDouble(state->nodes[slot].pending_params.bg_price_threshold),
                    FPN_ToDouble(rolling->price_avg));
                state->display_meta[slot].prev_gate_log_state = packed;
            }
        }

        // v5.12.2.B — record the full-rebuild bookkeeping so the next
        // call's lazy predicate can compare against it. Only reaches here
        // when we DIDN'T take the lazy-skip path (= a real rebuild ran).
        if (state && slot >= 0 && slot < MAX_EXECUTION_NODES) {
            auto* sst_lazy = state->nodes[slot].slow_state;
            if (sst_lazy && rolling) {
                sst_lazy->us_at_last_rebuild = now_us;
                sst_lazy->price_at_last_rebuild = rolling->price_avg;
            }
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v4.7.38 (Phase C.1): single-core variant of RebuildAllParameters.
// Caller must precompute book_imbalance_blocked (cheap — one FPN_Binary compare)
// and skip cores with strategy_id == STRATEGY_NONE before calling.
//
// v5.12.1.B (clock hoist): optional `now_us` param at end. When non-zero,
// caller has already read system_clock at slow-path entry; recovery check
// uses it instead of doing its own clock_gettime. Default 0 = back-compat
// (legacy callers, tests). Saves ~50ns/cycle in flatten-recovery window
// when caller hoists. See CLAUDE.md item 16 (reuse-audit principle).
//======================================================================
// [END_FUNCTION]_[EventLoop_RebuildOneCore]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoop_PushParameters]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CONCURRENCY]]
// [SYNC]_[SEQLOCK]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONLY writer of core->param_slot (P4.7) — DIRTY nodes' pending_params pushed via the triple-buffered slot; wait-free on the hot consumer]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int EventLoop_PushParameters(EventLoopState<F>* state,
                                     uint64_t publish_tick = 0) {
    // v5.12.1.B.2 — publish_tick threads through to ParameterSlot_Write so
    // hot-path's staleness gate can detect stale slow-path. Default 0 =
    // back-compat for legacy + test callers (warmup sentinel; gate inert).
    int pushed = 0;
    for (int slot = 0; slot < state->registered_count; ++slot) {
        if (!NODE_STATE_FLAG_IS_SET(state->nodes[slot], DIRTY)) continue;
        ExecutionCore<F>* core = state->nodes[slot].core;
        if (core == nullptr) {
            NODE_STATE_FLAG_CLR(state->nodes[slot], DIRTY);
            continue;
        }
        ExecutionCore_SetParameters(core, state->nodes[slot].pending_params,
                                     publish_tick);
        NODE_STATE_FLAG_CLR(state->nodes[slot], DIRTY);
        ++pushed;
    }
    return pushed;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// walk all registered cores. for each one with dirty == 1, atomically push
// pending_params through ExecutionCore_SetParameters (which goes through the
// triple-buffered ParameterSlot). clear the dirty flag. returns the number of
// cores pushed (for monitoring).
//
// the push is wait-free on the consumer side. the execution core can keep
// ticking through this entire function and will see either the old or the
// new params atomically — never a torn intermediate.
//
// CRITICAL P4.7 invariant: this is the ONLY function that writes to a core's
// param_slot. _OnEvent must NEVER touch core->param_slot directly because
// the execution core is reading it concurrently. _OnEvent only updates the
// controller-side Portfolio + balance + intended_tp/sl/qty.
//======================================================================
// [END_FUNCTION]_[EventLoop_PushParameters]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoop_KillSwitchEvaluate]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CAPITAL_BEARING] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[account-level kill switch — balance floor + peak-drawdown trip clears every node's permission; ConfigureKillSwitch / ClearAllPermissions / KillSwitchTrip (manual) share the section]
//======================================================================
// [CODE]
//======================================================================
// KILL SWITCH — phase 09: configure the kill switch thresholds. pass FPN_Zero for either parameter to
// disable that condition. typical settings:
//   min_balance = 100.0          (bankruptcy floor)
//   max_drawdown_pct = 0.20      (20% drawdown from peak balance)
//
// the kill switch is evaluated on the slow path via _KillSwitchEvaluate. when
// it trips, every registered core's permission is cleared with RELEASE
// semantics, blocking new entries on the next hot-path tick. active positions
// continue to exit normally — see plan section "Active position handling".
//======================================================================================================
template <unsigned F>
inline void EventLoopState_ConfigureKillSwitch(EventLoopState<F>* state,
                                                Money min_balance,
                                                Money max_drawdown_pct) {
    state->oms->ks_min_balance      = min_balance;
    state->oms->ks_max_drawdown_pct = max_drawdown_pct;
}

// helper: clear permission on every registered core. used by both
// _KillSwitchEvaluate (when a condition fires) and _KillSwitchTrip (manual).
template <unsigned F>
inline void EventLoop_ClearAllPermissions(EventLoopState<F>* state) {
    for (int slot = 0; slot < state->registered_count; ++slot) {
        ExecutionCore<F>* core = state->nodes[slot].core;
        if (core) ExecutionCore_SetPermission(core, 0);
    }
}

// manual trip — used by external monitors (e.g. orphan recovery, operator
// command) that need to halt trading without going through the threshold check.
// idempotent: trips_total only bumps on a state transition.
template <unsigned F>
inline void EventLoop_KillSwitchTrip(EventLoopState<F>* state) {
    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    if (!BITMAP_IS_SET(state->oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED)) {
        BITMAP_SET(state->oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED);
        state->oms->ks_trips_total++;
    }
    EventLoop_ClearAllPermissions(state);
}

//------------------------------------------------------------------------------
// [SECTION]_[KILL SWITCH EVALUATE]
//------------------------------------------------------------------------------
// returns 1 if the switch newly tripped on this call, 0 otherwise. always safe
// to call. evaluates two conditions:
//   1. balance < ks_min_balance (when ks_min_balance > 0)
//   2. drawdown > ks_max_drawdown_pct (when ks_max_drawdown_pct > 0)
//      drawdown = (ks_peak_balance - balance) / ks_peak_balance
//
// the function is idempotent: once tripped, subsequent calls return 0 even if
// the conditions still hold. caller must call _Unpause to reset and re-arm.
//
// pitfall P9.2: caller drains events BEFORE evaluating so any in-flight exits
// are folded into balance first. the EventLoop_RunController loop already does
// this via DrainEvents → KillSwitchEvaluate ordering.
//
// pitfall P9.4: when permission is cleared, future Set/Restore must be paired
// with a valid strategy assignment. _Unpause enforces this.
//======================================================================================================
template <unsigned F>
inline int EventLoop_KillSwitchEvaluate(EventLoopState<F>* state) {
    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    if (BITMAP_IS_SET(state->oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED)) return 0;  // already tripped, no double-action

    int trip = 0;

    // condition 1: hard balance floor
    if (!Money_IsZero(state->oms->ks_min_balance) &&
        Money_Lt(state->oms->balance, state->oms->ks_min_balance)) {
        trip = 1;
    }

    // condition 2: drawdown from peak
    if (!Money_IsZero(state->oms->ks_max_drawdown_pct) &&
        !Money_IsZero(state->oms->ks_peak_balance)) {
        Money drop = Money_Sub(state->oms->ks_peak_balance, state->oms->balance);
        // only consider positive drops (balance below peak)
        if (Money_Gt(drop, Money_Zero())) {
            // drawdown = drop / peak. NoAssert variant: peak is non-zero per
            // the guard above so this can't trip the production assert path.
            Money dd = Money_Div(drop, state->oms->ks_peak_balance);
            if (Money_Gt(dd, state->oms->ks_max_drawdown_pct)) trip = 1;
        }
    }

    if (!trip) return 0;

    // Trip: clear permission on every core. This is the primary action.
    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    BITMAP_SET(state->oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED);
    state->oms->ks_trips_total++;
    EventLoop_ClearAllPermissions(state);
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EventLoop_KillSwitchEvaluate]
//======================================================================

//------------------------------------------------------------------------------
// UNPAUSE — restore permission on cores with assigned strategies
// (doc for EventLoop_Unpause, defined below after the exit-mechanism family)
//------------------------------------------------------------------------------
// reset the tripped flag and grant permission to every core that has a valid
// strategy assignment. cores with strategy_id == STRATEGY_NONE stay paused
// (pitfall P9.7 — they were never authorized to trade).
//
// the caller is expected to have re-pushed fresh parameters via
// EventLoop_PushParameters BEFORE calling this. setting permission=1 against a
// stale parameter pack is the bug pitfall P9.4 warns about; the safe sequence
// is RebuildAllParameters → PushParameters → Unpause.
//
// returns the number of cores that had permission restored.
//======================================================================================================

//======================================================================
// [FUNCTION]_[EventLoop_TimeExitOneCore]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[force-close legs held past max_hold_ticks with gain below the floor (v4.7.17 extract — one site for live + backtest); future-stamp underflow guard]
// [REFERENCE]_[DECISION]_[D-103]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EventLoop_TimeExitOneCore(EventLoopState<F>* state,
                                       OrderManagerState<F>* oms,
                                       const ControllerConfig<F>& cfg,
                                       uint64_t now_tick,
                                       double current_price,
                                       int node_id) {
    // Build per-core slot mask: slot c (partials off) or slots 2c+0..1 (on).
    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    int partial_on = BITMAP_IS_SET(oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
    uint16_t my_mask = BITMAP_NODE_SLOT_MASK(node_id, partial_on);
    uint16_t bm = (uint16_t)(oms->portfolio.active_bitmap & my_mask);

    while (bm) {
        int slot = __builtin_ctz(bm);
        bm &= (uint16_t)(bm - 1);

        // Note: state->nodes[] is indexed by node_id, not slot. With partials,
        // both leg-A (slot 2c) and leg-B (slot 2c+1) share the same NodeContext
        // at index node_id. last_entry_tick is stamped only on leg-A entries
        // by design (per-trade signal), so checking via node_id is correct.
        uint64_t entry_t = state->nodes[node_id].last_entry_tick;
        if (entry_t == 0) continue;  // never stamped (shouldn't happen if active)
        if (entry_t > now_tick) {
            // Snapshot-drift guard. See "Snapshot Tick-Counter Drift"
            // invariant in CLAUDE.md.
            fprintf(stderr,
                "[time-exit] node %d slot %d: stale entry_tick from snapshot "
                "(entry_t=%llu > now_tick=%llu); resetting.\n",
                node_id, slot, (unsigned long long)entry_t,
                (unsigned long long)now_tick);
            state->nodes[node_id].last_entry_tick = now_tick;
            continue;
        }
        uint64_t elapsed = now_tick - entry_t;
        // v5.12.3.C — per-core override. 0 = use global; >0 = override.
        // Branchless mask-select would obscure intent; slow-path branch is
        // negligible (~1ns per cycle).
        uint32_t max_hold = cfg.node_time_exit_ticks[node_id];
        if (max_hold == 0) max_hold = cfg.nodes[node_id].max_hold_ticks;
        if (elapsed < max_hold) continue;

        double entry_d = Money_ToDouble(oms->portfolio.positions[slot].entry_price);
        if (entry_d <= 0.0) continue;
        double gain_pct = (current_price - entry_d) / entry_d;
        double min_gain = Money_ToDouble(cfg.min_hold_gain_pct);
        if (gain_pct >= min_gain) continue;  // still profitable enough; keep it

        // Force-close via OMS_PushSubmit (drainer is sole Submit caller).
        // v5.15.5.C.4 Phase D5 — routed through OMS_PushExitForSlot helper.
        // v5.15.5.F.4c.3 WIP2d-1.B.1 — per-core cfg required for Order_BindPreResolved at submit.
        Money qty       = oms->portfolio.positions[slot].quantity;
        Money price_fpn = Money{ money_from_double_payload(current_price) };  // D-103 ingress bridge
        tt::OMS_PushExitForSlot(oms, (int16_t)slot,
                                qty, state->nodes[node_id].strategy_id, price_fpn,
                                /*leg*/(uint8_t)0, &cfg.nodes[node_id]);

        fprintf(stderr,
            "[time-exit] node %d slot %d: held %lu ticks, gain %.3f%%\n",
            node_id, slot, (unsigned long)elapsed, gain_pct * 100.0);
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Walk the active-position bitmap, force-close any leg held longer than
// cfg.max_hold_ticks WITH gross gain below cfg.min_hold_gain_pct.
// Profitable positions are kept (still working). No-op when
// cfg.max_hold_ticks == 0 (default). Same logic in live + backtest now —
// adding a new exit gate updates ONE site.
//
// Defensive guard: entry_t > now_tick (snapshot-restored future stamp)
// would underflow the uint64 subtraction → time-exit fires every cycle.
// Reset to current and skip when detected; next genuine entry stamps fresh.
//
// Caller supplies the elapsed-tick basis + current price:
//   - Live: `ticks_produced.load()` and `last_price.load()` atomics
//   - Backtest: `tick_index` and `tick.price` (from RunTick)
//
// Counts every leg-exit as one heartbeat increment (matches live's pre-
// extraction wiring). Uses OrderManager_Submit directly because time-exit
// bypasses the SG-driven event path.
//
// v4.7.38 (Phase C.1): per-core helper, processes only this core's slots.
// Caller-checked preconditions (cfg.max_hold_ticks > 0 && current_price > 0).
//======================================================================
// [END_FUNCTION]_[EventLoop_TimeExitOneCore]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoop_FlattenAll]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CAPITAL_BEARING] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[WS-dropout emergency flatten (v5.12.1.A.2) — market exits for every active slot via the drainer queue; HONEST shortfall count (A3); CheckWsStaleness (CAS-win gate) + TryClearRecovery share the family]
// [REFERENCE]_[DECISION]_[[D-294] [D-295]]
// [REFERENCE]_[INVARIANT]_[H20]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int EventLoop_FlattenAll(EventLoopState<F>* state,
                                 OrderManagerState<F>* oms,
                                 const PerNodeCfg<F>* nodes,
                                 double current_price,
                                 int reason_code) {
    // v5.15.5.F.4c.3 WIP2d-1.B.1: `cores` REQUIRED — per-core array pointer (caller passes
    // `cfg.nodes`). Multi-slot dispatch fn; per cfg-scope-discipline § "consumer over per-core array."
    int submitted = 0;
    uint16_t bm = oms->portfolio.active_bitmap;
    // A3 (.E.0.10): snapshot the requested count BEFORE the loop consumes `bm`, so a
    // submit_queue-full shortfall (emergency + partials) is made visible, not silently lost.
    const int requested = __builtin_popcount((unsigned)bm);
    // event_price is for log/audit (the actual market fill happens at
    // exchange-side price). FPN_Zero on degenerate price preserves
    // existing OMS conventions.
    Money price_fpn = (current_price > 0.0)
        ? Money{ money_from_double_payload(current_price) }
        : Money_Zero();
    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    int partial_on = BITMAP_IS_SET(oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
    while (bm) {
        int slot = __builtin_ctz(bm);
        bm &= (uint16_t)(bm - 1);
        // Branchless (H20): slot → owning node via the shared Sharded_SlotNode (pure ALU shift by
        // partial_on ∈ {0,1}; no cmov — the accessor is THE single source, D-294/D-295).
        int logical_core = Sharded_SlotNode(slot, partial_on);
        Money qty = oms->portfolio.positions[slot].quantity;
        uint8_t sid = state->nodes[logical_core].strategy_id;
        // A8 (.E.0.10): the leg index is meaningful ONLY under partials (even slot = leg A,
        // odd = leg B); without partials there is no leg, so gate on partial_on (branchless
        // ALU; 0 or 1). NOTE: corrects the bare `slot&1` first proposed for A8 — that would
        // mislabel odd-numbered STANDALONE slots as leg B when partials are OFF. No
        // Order_GetLeg consumer reads this yet → correct-of-intent future-proofing, not a live fix.
        uint8_t leg = (uint8_t)((slot & 1) & partial_on);
        // v5.15.5.C.4 Phase D5 — routed through OMS_PushExitForSlot helper.
        // v5.15.5.F.4c.3 WIP2d-1.B.1 — per-core cfg for Order_BindPreResolved at submit.
        // A3 (.E.0.10): count what ACTUALLY queued — OMS_PushExitForSlot forwards
        // OMS_PushSubmit's success bool. Under a full submit_queue this push can fail; the
        // prior unconditional `submitted++` reported it as flattened while the leg stayed
        // OPEN on the emergency path.
        submitted += (int)tt::OMS_PushExitForSlot(oms, (int16_t)slot, qty, sid, price_fpn,
                                                  leg, &nodes[logical_core]);
    }
    if (submitted < requested) {
        // A3 (.E.0.10): the durable HONEST-COUNT half of the half-flatten fix — make the
        // shortfall LOUD on the emergency path. The actual RETRY (re-fire the dropped legs)
        // needs new plumbing and is .E.1-subsumed (drainer→per-node absorption); the
        // FlattenAll characterization test pins this behavior so .E.1 can't silently regress it.
        std::fprintf(stderr,
            "[OMS] FlattenAll SHORTFALL: %d/%d position(s) queued (reason=%d) — "
            "submit_queue full; %d leg(s) STILL OPEN on the emergency path.\n",
            submitted, requested, reason_code, requested - submitted);
    } else if (submitted > 0) {
        std::fprintf(stderr,
            "[OMS] FlattenAll: %d position(s) submitted "
            "(reason=%d, price=%.2f)\n",
            submitted, reason_code, current_price);
    }
    return submitted;
}

// EventLoop_CheckWsStaleness: read producer's last_ws_tick_us, compute
// gap vs caller-supplied now_us, fire EventLoop_FlattenAll on CAS-win
// when gap exceeds threshold. Pre-warmup (last_ws_tick_us == 0) is "no
// flatten".
//
// LATENCY OPTIMIZATION (v5.12.1.A.2): now_us is a PARAMETER, not read
// internally. This lets per-core slow-path share its single
// system_clock::now() read with the existing sp_last_tick_us update
// (in the per-node slow-path body) — saves ~50-100ns of vDSO clock_gettime
// per slow-path cycle per core. Caller passes its own measurement
// (live: system_clock; backtest: tick.timestamp for determinism).
//
// Returns: 0 if not breached or CAS lost; > 0 (count of submits) when
// this thread won the CAS and fired the flatten.
//
// Branchless considerations: cfg-flag check is the dominant fast path
// (BITMAP_IS_SET(cfg.risk_cfg_flags, MASK_RISK_CFG_WS_DEAD_TIME_FLATTEN_ENABLED) = 0 by default → early return ~5ns,
// no atomic load). Once enabled, the predicates are sequential branches
// — slow-path branches are fine; the rare-true outcome justifies branch
// over branchless mask compute.
template <unsigned F>
inline int EventLoop_CheckWsStaleness(EventLoopState<F>* state,
                                       const ControllerConfig<F>& cfg,
                                       double current_price,
                                       uint64_t now_us) {
    // v5.14.9.B.0 — refresh engine-wide gate cache from cfg before reading.
    // Cheap (~3ns OR-reduction); defensive against callers that haven't run
    // RebuildOneCore yet (CheckWsStaleness fires every slow-path cycle in
    // EngineSharded; RebuildOneCore fires every poll_interval cycle, so the
    // gate cache could be missing/stale on the first few CheckWsStaleness
    // calls before warmup). Re-populating here makes the cache source-of-
    // truth equivalent to inline cfg read while preserving the registry-
    // driven discipline.
    if (state) {
        SLOW_PATH_GATE_AUTOPOPULATE_ENGINE_WIDE(state->global_gate_state, cfg);
    }
    bool _ws_gate = state
        ? BITMAP_IS_SET(state->global_gate_state.flags, MASK_WS_FLATTEN_ACTIVE)
        : (BITMAP_IS_SET(cfg.risk_cfg_flags, MASK_RISK_CFG_WS_DEAD_TIME_FLATTEN_ENABLED) != 0);
    if (!_ws_gate) return 0;

    // Pre-warmup sentinel: producer hasn't published any tick yet.
    uint64_t last = state->ws_telemetry.last_tick_us.load(std::memory_order_acquire);
    if (last == 0) return 0;

    // Defensive: clock skew or NTP reset could leave now_us < last;
    // treat as 0 gap. Avoids spurious flatten on clock anomalies.
    uint64_t gap_us = (now_us > last) ? (now_us - last) : 0;
    uint64_t threshold_us =
        (uint64_t)cfg.ws_dead_time_flatten_threshold_secs * 1000000ULL;

    if (gap_us < threshold_us) return 0;

    // CAS: only one slow-path thread wins the flatten across multiple
    // cores' concurrent calls. Subsequent calls in the same staleness
    // window see flatten_pending == 1 and short-circuit. Reset of the
    // flag (post-reconcile) lands in v5.12.1.A.3.
    int expected = 0;
    if (!state->oms->flatten_pending.compare_exchange_strong(
            expected, 1,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return 0;  // another core already won; no double-flatten
    }

    // v5.12.1.A.3 — set recovery deadline BEFORE firing flatten so
    // RebuildOneCore (which checks recovery_until_us) sees the new
    // window even on the same slow-path tick. release ordering pairs
    // with RebuildOneCore's acquire load.
    uint64_t deadline_us = now_us +
        ((uint64_t)cfg.recovery_delay_secs * 1000000ULL);
    state->oms->recovery_until_us.store(deadline_us,
                                         std::memory_order_release);

    std::fprintf(stderr,
        "[OMS] WS staleness: gap=%.1fs > threshold=%ds; "
        "firing OMS_FlattenAll. Recovery refusal until +%ds.\n",
        (double)gap_us / 1.0e6,
        cfg.ws_dead_time_flatten_threshold_secs,
        cfg.recovery_delay_secs);
    return EventLoop_FlattenAll(state, state->oms, cfg.nodes, current_price,
                                 /*reason*/1);
}

//------------------------------------------------------------------------------
// [SECTION]_[POST-FLATTEN RECOVERY EXPIRY (v5.12.1.A.3)]
//------------------------------------------------------------------------------
// CheckWsStaleness sets oms->recovery_until_us when it fires a flatten.
// Once the deadline elapses, this helper auto-clears flatten_pending +
// recovery_until_us so a future staleness event can re-fire.
//
// Caller (slow-path) invokes whenever recovery_until_us > 0 — gated
// at the call site so the common case (no recovery active) pays zero.
//
// Branchless: compare-and-swap with witness; success means we won the
// clear race across multiple slow-path threads. CAS-ordering puts a
// release on the cleared atomics so a later staleness fire sees zeroed
// state.
//======================================================================================================
template <unsigned F>
inline int EventLoop_TryClearRecovery(OrderManagerState<F>* oms,
                                       uint64_t now_us) {
    uint64_t until = oms->recovery_until_us.load(std::memory_order_acquire);
    if (until == 0) return 0;            // not active
    if (now_us < until) return 0;        // still inside window
    // Recovery window expired. CAS the deadline back to 0; only the
    // winner clears flatten_pending. Multi-core race-safe.
    if (oms->recovery_until_us.compare_exchange_strong(
            until, 0,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        oms->flatten_pending.store(0, std::memory_order_release);
        std::fprintf(stderr,
            "[OMS] post-flatten recovery window elapsed; trading "
            "may resume on next slow-path cycle.\n");
        return 1;
    }
    return 0;  // another thread won the clear
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Live-only safety net for extended WS dropouts during real-money trading.
// Slow-path reads producer's last_ws_tick_us (set in EngineSharded fan_out
// at every WS tick); when the gap to local_now_us exceeds
// cfg.ws_dead_time_flatten_threshold_secs and the gate cfg flag is set,
// CAS-wins one slow-path thread invokes EventLoop_FlattenAll to push
// market-exit commands for every active position into the standard
// drainer queue.
//
// Disabled by default (BITMAP_IS_SET(cfg.risk_cfg_flags, MASK_RISK_CFG_WS_DEAD_TIME_FLATTEN_ENABLED) = 0); flip to 1
// BEFORE live-capital deployment. Backtest must keep it 0 — backtest's
// tick-driven last_ws_tick_us would otherwise produce a huge gap vs
// local clock and fire phantom flattens.
//
// SLOW PATH only. NOT branchless (early returns are cheaper than
// branchless mask compute when the predicate is almost-always false —
// cfg flag default 0 → first branch returns immediately, ~5ns total).
// When enabled, full check costs ~100-200ns (clock_gettime via vDSO +
// atomic load + comparisons). Well within 100μs slow-path budget.
//
// Hot path UNTOUCHED.
//
// EventLoop_FlattenAll: walk active-position bitmap, push market exits
// via OMS_PushSubmit. Drainer is sole Submit caller (CLAUDE.md item 5)
// so we go through PushSubmit, not Submit directly. Returns count of
// commands queued (0 if portfolio empty). Idempotent — caller (CAS
// winner) invokes once per flatten event; subsequent CAS-failed callers
// don't re-enter.
//======================================================================
// [END_FUNCTION]_[EventLoop_FlattenAll]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoop_TrailingSLRatchetOneCore]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[D9 trailing SL — ratchet_sl = price - stddev*mult, fee-floored (v5.1.7) so an SG exit always clears round-trip fees; max-only, picked up via the seqlock push]
// [REFERENCE]_[CLASS]_[26]
// [REFERENCE]_[INVARIANT]_[H20]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W>
inline void EventLoop_TrailingSLRatchetOneCore(EventLoopState<F>* state,
                                                 const ControllerConfig<F>& cfg,
                                                 const RollingStats<F, W>& rolling,
                                                 double current_price,
                                                 int node_id) {
    double stddev_d     = FPN_ToDouble(rolling.price_stddev);
    double trail_dist_d = stddev_d * FPN_ToDouble(cfg.nodes[node_id].sl_trail_mult);
    double hold_thresh  = FPN_ToDouble(cfg.nodes[node_id].tp_hold_score);

    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    int partial_on = BITMAP_IS_SET(state->oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
    uint16_t my_mask = BITMAP_NODE_SLOT_MASK(node_id, partial_on);
    uint16_t bm = (uint16_t)(state->oms->portfolio.active_bitmap & my_mask);

    // v5.1.7: fee-floor on the ratchet. The trailing-SL ratchet writes
    // ratchet_sl which the hot-path SG uses as effective_sl = max(sl, ratchet_sl).
    // Without a floor, the ratchet can push effective_sl above
    // entry × (1 - fee_rate × 2), causing the FIRST tiny pullback to fire
    // SG → exit at near-breakeven gross which becomes net-negative after
    // round-trip fees. Cap the ratchet at entry × (1 - 3 × fee_rate_taker)
    // to guarantee any SG-fired exit clears fees with at least 1× fee_rate
    // of margin.
    // v5.15.5.F.4d.1.B.8 — Class 26 sub-shape B fix: per-core fee_rate_taker (UNINDEXED-GLOBAL closure).
    // cfg.fee_rate_taker is GLOBAL; per-core consumers MUST read cfg.nodes[node_id].fee_rate_taker.
    // H20 branchless: pre-resolve node_cfg ref (single array index via CSE) + ternary select (cmov-lowerable).
    // Sister-canonical: the node_cfg->X pattern in StrategyParameters.hpp.
    const auto& node_cfg = cfg.nodes[node_id];
    double fee_taker_d = Money_ToDouble(!Money_IsZero(node_cfg.fee_rate_taker)
        ? node_cfg.fee_rate_taker : node_cfg.fee_rate);
    double fee_floor_pct = 3.0 * fee_taker_d;

    while (bm) {
        int slot = __builtin_ctz(bm);
        bm &= (uint16_t)(bm - 1);
        double entry_d = Money_ToDouble(state->oms->portfolio.positions[slot].entry_price);
        if (entry_d <= 0.0) continue;
        double gain_pct = (current_price - entry_d) / entry_d;
        if (gain_pct < hold_thresh) continue;  // not yet trailing

        // v5.1.7: clamp the proposed ratchet floor.
        double new_ratchet_d = current_price - trail_dist_d;
        double sl_floor_d    = entry_d * (1.0 - fee_floor_pct);
        if (new_ratchet_d > sl_floor_d) {
            // Trail wants to ratchet ABOVE the fee floor → would close
            // positions that haven't yet earned 3× fees of margin. Cap.
            new_ratchet_d = sl_floor_d;
        }

        // Note: pending_params is per-CORE (one queue per core). Both legs
        // share it under partials. Index via node_id, not slot.
        Money new_ratchet = Money{ money_from_double_payload(new_ratchet_d) };
        Money existing    = state->nodes[node_id].pending_params.ratchet_sl;
        if (Money_Gt(new_ratchet, existing)) {
            state->nodes[node_id].pending_params.ratchet_sl = new_ratchet;
            NODE_STATE_FLAG_SET(state->nodes[node_id], DIRTY);  // force push next cycle
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v4.7.17 (extracted from EngineSharded) — for each active position with gross gain >= cfg.tp_hold_score, write a
// trailing SL = current_price - (price_stddev × cfg.sl_trail_mult) into
// pending_params.ratchet_sl. Hot path picks it up via the existing
// seqlock on the next param push. Only ratchets UP (FPN_Max in hot path
// drops smaller ratchet values). No-op when cfg.tp_hold_score == 0
// (default) OR cfg.sl_trail_mult == 0 OR rolling.price_stddev == 0.
//
// Caller supplies current price:
//   - Live: `last_price.load()` atomic
//   - Backtest: `tick.price`
//
// Reads rolling.price_stddev directly (caller-owned RollingStats).
//
// v4.7.38 (Phase C.1): per-core helper. Caller-checked preconditions.
// Implicitly fixes a pre-existing bug where the original walked
// active_bitmap by slot but indexed state->nodes[slot] (per-core array) —
// under partials, slot 1 (core 0's leg B) wrongly read/wrote core 1's
// pending_params. OneCore correctly uses node_id for nodes[] indexing
// while still iterating per-slot for portfolio.positions[].
//======================================================================
// [END_FUNCTION]_[EventLoop_TrailingSLRatchetOneCore]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoop_BreakevenOnProfitOneCore]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[v5.15.2 breakeven-on-profit ratchet (TECH_DEBT-024 close) — one-shot SL to fee-floored breakeven once net-profitable; composes with trailing-SL via max-only ratchet_sl]
// [REFERENCE]_[CLASS]_[26]
// [REFERENCE]_[INVARIANT]_[H20]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EventLoop_BreakevenOnProfitOneCore(EventLoopState<F>* state,
                                                const ControllerConfig<F>& cfg,
                                                double current_price,
                                                int node_id) {
    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    int partial_on = BITMAP_IS_SET(state->oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
    uint16_t my_mask = BITMAP_NODE_SLOT_MASK(node_id, partial_on);
    uint16_t bm = (uint16_t)(state->oms->portfolio.active_bitmap & my_mask);

    // Net-profit threshold: round-trip taker fees (entry + exit). Below
    // this, the ratchet would close the position at net-negative; the
    // fee floor on the ratchet_sl prevents that, but skipping the math
    // when gain_pct < 2*fee saves the FPN_FromDouble + compare per cycle.
    // v5.15.5.F.4d.1.B.8 — Class 26 sub-shape B fix: per-core fee_rate_taker (UNINDEXED-GLOBAL closure).
    // H20 branchless: pre-resolve node_cfg ref + ternary select (sister to HIGH-1 above + StrategyParameters.hpp's node_cfg->X pattern).
    const auto& node_cfg = cfg.nodes[node_id];
    double fee_taker_d = Money_ToDouble(!Money_IsZero(node_cfg.fee_rate_taker)
        ? node_cfg.fee_rate_taker : node_cfg.fee_rate);
    double net_profit_threshold = 2.0 * fee_taker_d;
    double fee_floor_pct        = 3.0 * fee_taker_d;

    while (bm) {
        int slot = __builtin_ctz(bm);
        bm &= (uint16_t)(bm - 1);
        double entry_d = Money_ToDouble(state->oms->portfolio.positions[slot].entry_price);
        if (entry_d <= 0.0) continue;
        double gain_pct = (current_price - entry_d) / entry_d;
        if (gain_pct < net_profit_threshold) continue;  // not net-profitable yet

        // Ratchet SL to fee-floored breakeven (entry × (1 - 3 × fee)).
        // pending_params.ratchet_sl is max-only; if trailing-SL already
        // proposed a higher floor (gain > tp_hold_score path), that wins.
        Money breakeven_sl = Money{ money_from_double_payload(entry_d * (1.0 - fee_floor_pct)) };
        Money existing     = state->nodes[node_id].pending_params.ratchet_sl;
        if (Money_Gt(breakeven_sl, existing)) {
            state->nodes[node_id].pending_params.ratchet_sl = breakeven_sl;
            NODE_STATE_FLAG_SET(state->nodes[node_id], DIRTY);
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// One-shot ratchet of SL to fee-floored breakeven when an open position
// crosses net-profitable (gain > round-trip taker fees). Mirrors trailing-
// SL ratchet's OneCore/Wrapper precedent + writes to the same
// pending_params.ratchet_sl with max-only semantics — composes cleanly
// with trailing-SL when both enabled (trailing wins via max once gain
// exceeds tp_hold_score; breakeven holds the floor below).
//
// Independent of trailing-SL preconditions (sl_trail_mult / tp_hold_score
// can both be zero; breakeven still fires).
//
// Slow-path cost: per-cycle when bit set; ~80-150ns per active position
// (active_bitmap walk + entry-price load + gain compute + FPN_Binary compare).
// Bit unset → wrapper early-exits in ~1ns. Below 100µs slow-path budget.
//======================================================================
// [END_FUNCTION]_[EventLoop_BreakevenOnProfitOneCore]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoop_Unpause]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[reset the trip + re-grant permission to strategy-assigned nodes (doc block above the exit family; safe sequence: Rebuild -> Push -> Unpause per P9.4); SlowPath (no-op hook) + RunController (controller main loop) share the section]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int EventLoop_Unpause(EventLoopState<F>* state) {
    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    BITMAP_CLR(state->oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED);
    int resumed = 0;
    for (int slot = 0; slot < state->registered_count; ++slot) {
        ExecutionCore<F>* core = state->nodes[slot].core;
        if (!core) continue;
        if (state->nodes[slot].strategy_id == STRATEGY_NONE) continue;
        ExecutionCore_SetPermission(core, 1);
        ++resumed;
    }
    return resumed;
}

//------------------------------------------------------------------------------
// [SECTION]_[SLOW PATH HOOK]
//------------------------------------------------------------------------------
// phase 04 doesn't run the existing PortfolioController_SlowPath. that wires
// in via phase 13's migration flag, where the per-core mode bypasses the
// existing slow path entirely (events ARE the slow path now) and the
// legacy mode keeps the old behavior. for now this hook is a no-op and the
// run-loop is purely event-driven.
//------------------------------------------------------------------------------
template <unsigned F>
inline int EventLoop_SlowPath(EventLoopState<F>* state) {
    (void)state;
    return 0;  // no-op for phase 04
}

//------------------------------------------------------------------------------
// [SECTION]_[RUN CONTROLLER]
//------------------------------------------------------------------------------
// the controller core thread main loop. spins draining events with a small
// adaptive backoff: after several empty polls, sleep briefly to avoid burning
// the core at 100%. tuned for a busy execution core that fires events at
// most once per second per strategy.
//
// the shutdown flag is read on each iteration. caller (engine main) flips it
// on SIGTERM or kill switch to break the loop cleanly.
//
// timing budgets per iteration (rough, on i5-1035G4 with reasonable load):
//   - drain pass:           1-50 µs (depends on event count)
//   - param push (phase 05): 1 µs
//   - slow path (phase 13):  100-500 µs (rare path, runs every K iterations)
//   - sleep / pause:         0 to 100 µs depending on idle state
//
// budget allows ~1000 iterations/sec under load, ~10000/sec idle.
//======================================================================================================
template <unsigned F>
inline void EventLoop_RunController(EventLoopState<F>* state,
                                     volatile int* shutdown_flag) {
    int idle_count = 0;
    while (*shutdown_flag == 0) {
        int drained = EventLoop_DrainEvents(state);
        EventLoop_PushParameters(state);   // phase 05
        EventLoop_SlowPath(state);         // phase 13

        if (drained == 0) {
            // adaptive backoff: pause hint first, then short sleep
            ++idle_count;
            if (idle_count < 16) {
                __builtin_ia32_pause();
            } else {
                timespec ts{};
                ts.tv_sec = 0;
                ts.tv_nsec = 100'000;  // 100 µs sleep when idle
                nanosleep(&ts, nullptr);
            }
        } else {
            idle_count = 0;
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EventLoop_Unpause]
//======================================================================

}  // namespace tt
