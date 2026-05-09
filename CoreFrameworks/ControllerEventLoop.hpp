// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [CONTROLLER EVENT LOOP]
//
// Per-core sharded engine controller-side. Reads TradeEvents from each
// registered execution core's event ring, processes entries and exits into
// the canonical Portfolio state, updates balance and realized P&L.
//
// Architecture:
//   - controller core owns this loop, runs on its own pinned CPU
//   - each execution core registered via EventLoopState_RegisterCore
//   - core_id maps directly to portfolio slot (no indirection)
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
//   (in CoreContext) and applied to the portfolio at entry-event time.
//======================================================================================================

#pragma once

#include "../Limits.hpp"
#include "../ML_Headers/ConfidenceScore.hpp"
#include "../ML_Headers/LinearRegression3X.hpp"  // v4.0.3 D10 RegressionFeederX
#include "../ML_Headers/RollingStats.hpp"
#include "../ML_Headers/ROR_regressor.hpp"        // v5.1.0 — RORRegressor on CoreContext::slow_state
#include "../ML_Headers/FlowFeatures.hpp"         // v5.1.0 — FlowState etc on CoreContext::slow_state
#include "../MemHeaders/HealthLog.hpp"
#include "../MemHeaders/InitArena.hpp"  // v5.11.6.A — unified mmap arena for init allocations
#include "../ML_Headers/FeatureRegistry.hpp"  // v5.9.0b: FEATURE_REGISTRY_HASH() in entry log           // v5.4.0 Phase 0.1 — structured JSONL diagnostic log
#include "../Strategies/StrategyParameters.hpp"
// Strategies/StrategyLifecycle.hpp included LATER (post-EventLoopState
// definition) to avoid the include cycle: SL needs CoreContext +
// EventLoopState, and ControllerEventLoop.hpp defines them at line ~404.
#include "CoreLatencyStats.hpp"  // v4.7.42 — slow_path_latency on CoreContext
#include "ExecutionCore.hpp"
#include "Notify.hpp"
#include "OrderManager.hpp"
#include "Portfolio.hpp"
#include "ShardedTradeLog.hpp"
#include "TradeEvent.hpp"

#include <cstdint>
#include <ctime>
#include <chrono>      // v5.12.1.A.2 — system_clock for WS staleness math

namespace tt {

//======================================================================================================
// [CORE SLOW STATE — v5.1.0 per-core data plane]
//======================================================================================================
// Each engine owns its rolling/regime/flow state instead of reading
// producer-owned shared state. See plans/2026-04-28-v5.1-data-plane-
// decouple.md for the full design.
//
// SINGLE-WRITER RULE per engine:
//   - centralized arch:    Producer thread writes ALL N cores at fan_out
//                          + slow-path body, looping c=0..N
//   - per_core_slow arch:  Producer writes per-tick (ema_price) to all N
//                          cores in fan_out; per-cadence updates done by
//                          per-core slow-path c on its OWN slow_state
//   - backtest:            Single thread; loops c=0..N like centralized
//
// READ PATTERN: per-core slow-path body (centralized: producer; per_core_
// slow: per-core thread) reads state.cores[c].slow_state. In per_core_
// slow, ema_price is producer-written; per-cadence fields are written by
// the per-core thread itself (no cross-thread for those). ema_price has
// eventual-consistency cross-thread reads (relaxed loads — same as
// pre-v5.1.0 shared design but now per-core targets).
//
// Window sizes are template-fixed (RollingStats<F, 128> etc.). Per-core
// override of window size is a future enhancement requiring runtime-
// sized buffers — currently every engine uses identical windows.
//======================================================================================================
template <unsigned F>
struct CoreSlowState {
    // Per-cadence rolling stats (RegimeDetector inputs).
    RollingStats<F, 128>    rolling_short;
    RollingStats<F, 512>    rolling_long;
    RollingStats<F, 256>    rolling_medium;
    RollingStats<F, 1024>   rolling_baseline;

    // Per-cadence regime / flow state.
    RORRegressor<F>         regime_ror;
    CumDeltaState<F>        cumdelta_state;
    TickRateState           tick_rate_state;
    BookImbalanceHistory<F, 1024> book_imb_history;
    FlowState               flow_state;
    LargeTradeState<F, 1024> large_trade_state;
    SpreadState<F, 1024>    spread_state;

    // Per-tick: EMA price. Updated EVERY tick in producer's fan_out;
    // hot-tail consumers (Regime_ComputeSignals) read at slow-path
    // cadence. In per_core_slow this is a producer→slow-path cross-
    // thread read (eventual consistency, x86-acceptable on aligned word).
    FPN<F>                  ema_price;

    // v5.12.2.B — lazy slow-path rebuild bookkeeping. Updated at the
    // END of every full RebuildOneCore execution. The next RebuildOneCore
    // call uses these to decide whether to skip the rebuild body when:
    //   (a) cfg.lazy_rebuild_enabled = 1 AND
    //   (b) (now_us - us_at_last_rebuild) < cfg.lazy_rebuild_force_period_us AND
    //   (c) |price_avg - price_at_last_rebuild| / price_at_last_rebuild
    //       < cfg.lazy_rebuild_price_threshold_pct
    // When all three hold, RebuildOneCore returns early after marking
    // pending_params for republish (so v5.12.1.B's publish_tick stays
    // fresh). Single-writer (this core's slow-path); no atomics.
    uint64_t                us_at_last_rebuild;
    FPN<F>                  price_at_last_rebuild;
};

template <unsigned F>
inline void CoreSlowState_Init(CoreSlowState<F>* s) {
    s->rolling_short    = RollingStats_Init<F, 128>();
    s->rolling_long     = RollingStats_Init<F, 512>();
    s->rolling_medium   = RollingStats_Init<F, 256>();
    s->rolling_baseline = RollingStats_Init<F, 1024>();
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

//======================================================================================================
// [CORE CONTEXT]
//======================================================================================================
// per-core controller-side state. one slot per registered execution core.
// the core_id field is implicit: cores[i].core->core_id == i. holds the
// "intended" trade parameters that the controller wants applied at the next
// entry — phase 05 will replace this with a real parameter-slot push protocol,
// for phase 04 it's just a hot-update field the controller writes between
// entries and reads at entry time.
//
// statistics (entries_processed / exits_processed) are bumped from the drain
// loop, useful for monitoring per-core throughput in the TUI later.
//======================================================================================================
template <unsigned F>
struct CoreContext {
    ExecutionCore<F>* core;        // registered execution core pointer
    FPN<F> intended_tp;            // TP to apply when this core's next entry fires
    FPN<F> intended_sl;            // SL to apply when this core's next entry fires
    FPN<F> intended_qty;           // quantity to size the next entry to
    FPN<F> allocated_balance;      // capital share for this core (set by Regime_AllocateCores in phase 06+)
    GateParameters<F> pending_params; // staged params, pushed on next _PushParameters
    uint8_t  strategy_id;          // STRATEGY_* constant; STRATEGY_NONE means "do not trade"
    uint8_t  dirty;                // 1 = pending_params should be pushed to the core
    uint8_t  _pad[6];
    void*    model_handle;         // CoreModelZoo<F>* for STRATEGY_ML cores (nullptr for others)
    void*    ensemble_handle;      // v5.10.0a.G.5 — EnsembleModelZoo<F>* when multi-horizon active; nullptr = single-zoo path (default)
    uint64_t entries_processed;    // bumped on entry event
    uint64_t exits_processed;      // bumped on exit event
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
    // v5.10.0e — drift detection. Sampled post-fill (when
    // ConfidenceScorer_Update fires for ML cores). Engine emits CRITICAL
    // log on sustained-breach + optionally trips per-core kill_switch
    // when cfg.auto_kill_on_drift=1. See ConfidenceScore.hpp DriftHistory.
    DriftHistory drift_history;
    double staged_prediction;      // prediction from last ML rebuild
    double active_prediction;      // prediction at last entry submit (0 = no open pos)
    double last_confidence;        // most recent ConfidenceScorer_Compute result
    // v5.13.0.B — sell-side ML prediction. Written by ML_BuildParameters
    // when cfg.use_exit_model && exit_predictor_count > 0. Slow-path body
    // post-RebuildOneCore checks against cfg.exit_threshold and fires
    // OMS_PushSubmit for any open positions on this core's slot(s).
    // Default 0.0 = no exit prediction this cycle. Reset each cycle.
    double last_exit_prediction;
    int    last_exit_dominant_horizon;  // -1 = none; otherwise [0..exit_predictor_count)
    // v5.9.0b — ML observability extensions (V5_9_AUDIT-#2, #3).
    // Surface model load failures, ML decision context, and NaN counters
    // to the operator via TUISnapshot + ML Status panel + entry log.
    int      model_load_failed;            // 1 = model attempted but refused/missing (distinct from "no model configured")
    // v5.9.5i — cfg drift counters (populated in EngineSharded boot;
    // TUI_CopySnapshotSharded mirrors to PerCoreSnap; ML Status panel
    // renders summary).
    uint8_t  cfg_drift_tier1_count;
    uint8_t  cfg_drift_tier2_count;
    uint8_t  cfg_drift_strict_refused;
    uint64_t last_ml_critical_log_us;      // rate-limit gate for ML→SimpleDip CRITICAL log (per-core)
    double   last_ml_threshold;            // ml_buy_threshold at last decision (display + entry log)
    double   last_ml_effective_threshold;  // post-confidence-damping threshold actually used
    uint32_t nan_feature_events_total;     // count of Features_PackAll -1 sentinel returns on this core
    uint32_t nan_prediction_events_total;  // count of Model_Predict NaN/Inf events on this core
    // v5.9.1 — edge-trigger for boot-time per-core warmup-complete log.
    // RebuildOneCore checks (rolling_short.count >= min_warmup_samples)
    // every cycle; fires the log exactly once per core (and per session)
    // by setting this flag. Distinct from the global startup gate at
    // EngineSharded.hpp:1420 (which uses core 0's count to release ALL
    // cores from CONTROLLER_WARMUP). Per-core readiness lives here.
    uint8_t warmup_log_emitted;
    // v4.0.3 spacing: last entry price for this core, set by drainer on
    // entry submit. Strategy _BuildParameters checks
    // |new_entry - last_entry_price| < stddev × spacing_multiplier and
    // zero-gates if too close, preventing entry clustering at similar
    // prices. Mirrors legacy PortfolioController spacing logic.
    FPN<F> last_entry_price;
    uint64_t last_entry_tick;      // for time-based exit (A3)
    // v4.7.6: wall-clock microseconds at the leg-A entry stamp site so
    // GUI can show "hold time" for open positions. Independent of
    // last_entry_tick (which is a producer count, not seconds).
    uint64_t last_entry_wall_us;
    // v4.0.3 D7 SL cooldown: after a stop-loss exit, pause entries on this
    // core for N slow-path cycles. Decremented each rebuild; entries
    // zero-gated while > 0. Optionally adaptive — scales by trend confidence
    // at SL time (cfg.sl_cooldown_adaptive).
    uint32_t sl_cooldown_remaining;
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
    // v5.6.3 — gate diagnostic comparands. Captured by the controller's
    // post-Strategy_BuildParameters gate checks (spacing, vwap, long-slope,
    // vol-delta, min-stddev) so the GUI can show actual vs threshold per
    // gate without recomputing (single-source rule —
    // EXECUTION_DISPLAY_INVARIANTS.md). Snapshot copies into PerCoreSnap
    // diag_* fields. Reset by the rebuild loop before each pass.
    FPN<F> diag_spacing_actual;     // |bg_threshold - last_entry|
    FPN<F> diag_spacing_floor;      // stddev * spacing_multiplier
    FPN<F> diag_vwap_actual;        // bg_price_threshold
    FPN<F> diag_vwap_threshold;     // vwap - vwap*vwap_offset
    FPN<F> diag_long_slope;         // long_rel_slope
    FPN<F> diag_long_slope_min;     // cfg.min_long_slope
    FPN<F> diag_volume_delta;       // rolling.volume_delta
    FPN<F> diag_volume_delta_min;   // cfg.min_buy_delta
    FPN<F> diag_stddev_pct;         // rolling.price_stddev / rolling.price_avg
    FPN<F> diag_stddev_pct_min;     // cfg.min_stddev_pct
    FPN<F> diag_tp_pct_actual;      // out.tp_pct
    FPN<F> diag_tp_pct_floor;       // 3 * fee_rate_taker
    // v5.6.6: previous packed gate-state byte. Used by the gate-state
    // edge-trigger health log emit to detect transitions. Layout:
    //   bits 0..3 : halt_reason          (0..10 fits in 4 bits)
    //   bits 4..7 : strategy_halt_reason (0..10)
    //   bit  8     : (BUY_BLOCKED >> 5) & 1
    //   bit  9     : permission
    // Stored as uint16_t. Fresh state computed at end of RebuildOneCore;
    // emit cat="gate" log only when packed_now != prev_gate_log_state.
    uint16_t prev_gate_log_state;
    // v4.0.3 B: per-core regime state for STRATEGY_AUTO. Tracks current
    // regime + hysteresis so the auto-mode core's strategy choice doesn't
    // flap on noise. Each AUTO core has its own state — different cores
    // can detect different regimes if their cfg differs.
    RegimeState<F> regime_state;
    // v4.0.3 D10: per-core P&L regression feeder for adaptive feedback.
    // Drainer pushes realized return on each exit; slow path reads slope
    // to shift resolved_cfg.entry_offset_pct + volume_multiplier within
    // [offset_min/max] + [vol_mult_min/max] bounds. Mirrors legacy
    // MeanReversion_Adapt / Momentum_Adapt regression-driven adaptation.
    RegressionFeederX<F> pnl_feeder;
    // v4.0.4: resolved strategy_id (after AUTO regime resolution). Equals
    // strategy_id for non-AUTO cores. For AUTO cores, holds the concrete
    // strategy from REGIME_STRATEGY_TABLE for the current detected regime.
    // Snapshot reads this for the GUI display so AUTO rows show e.g. "AUTO (DIP)".
    uint8_t resolved_strategy_id;
    // v4.0.4: per-core P&L tracking. The OMS keeps a single global
    // realized_pnl across all cores (since portfolio is shared); these
    // counters split it out by source core for the Account panel and any
    // future per-core kill switch / risk re-allocation logic. Updated in
    // EventLoop_OnEvent exit branch, alongside oms->realized_pnl.
    FPN<F> core_realized;          // sum of net P&L from this core's exits
    FPN<F> core_fees;              // sum of fees paid by this core's fills
    uint32_t core_wins;            // exits with net > 0
    uint32_t core_losses;          // exits with net <= 0
    // v4.7.21: per-trade W/L pairing under partial exits. When partials are
    // enabled, leg A and leg B close as separate fills, but they belong to
    // ONE trade idea. Counting each leg independently overstates trade count
    // and loses the "did this idea make money?" signal — e.g. leg A=TP1
    // (+small) plus leg B=SL (-larger) is net negative but per-leg counts
    // 1W + 1L. We pair them by stashing the first leg's net P&L; when the
    // partner closes we compute total net and bump core_wins/core_losses by 1.
    // partials disabled → bypass pairing, per-leg-A logic is correct
    // (single-leg trades, no partner exists).
    FPN<F> partner_pending_pnl;
    uint8_t partner_pending_active;
    uint8_t _pad_partner[7];
    // v4.7.25: per-core gross win/loss accumulators, mirroring the legacy
    // single_core's ctrl->gross_wins / ctrl->gross_losses. Sum of net P&L
    // for winning trades (gross_wins) and absolute net P&L for losing
    // trades (gross_losses, stored unsigned). Updated alongside core_wins
    // / core_losses — same per-trade semantics under partials (sum the
    // pair, classify by sign, accumulate into the matching gross bucket).
    // Pre-v4.7.25 sharded snapshot left snap->avg_win / avg_loss /
    // profit_factor / expectancy at zero — these accumulators feed those
    // fields in TUI_CopySnapshotSharded.
    FPN<F> core_gross_wins;
    FPN<F> core_gross_losses;
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
    FPN<F> core_open_notional;
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
    // Manual reset via TUISharedState::kill_reset_per_core[N] resets the
    // trip flag and refreshes peak to current. Aggregate OMS-level breaker
    // remains as backstop for whole-account drawdown.
    FPN<F> core_peak_balance;       // peak of current_value over core's lifetime
    FPN<F> core_dd_pct;             // current drawdown % (display field, recomputed each rebuild)
    uint8_t core_kill_tripped;      // 1 = killed; entries zero-gated with HALT_CORE_KILL
    uint8_t  _pad_kill[3];          // alignment
    uint32_t core_ks_trips_total;   // lifetime trip count (for forensics)
    // v4.2.1 — slow-path cycles since last fill on this core. Resets on
    // entry. Used as a "death-spiral" detector: if a core hasn't fired
    // in many cycles, the pnl_feeder is full of stale regression data
    // that's no longer informative — clear it so adaptive feedback
    // (D10) doesn't keep applying shifts based on ancient outcomes.
    // Mirrors legacy PortfolioController_Tick line 1641 mechanism but
    // doesn't need the filter-decay step (sharded recomputes
    // resolved_cfg fresh each rebuild, so there's no live-filter state
    // to drift back toward defaults).
    uint32_t idle_cycles;
    // v4.7.42 (Phase E): per-core slow-path latency profiling. Mirrors
    // ExecutionCore::latency_stats (hot-path) for the slow-path. Sampled
    // around the per-core slow-path body in engine_arch=per_core_slow.
    // In centralized mode, total_count stays 0 (no samples collected —
    // single producer slow-path doesn't break per-core, by design).
    CoreLatencyStats slow_path_latency;
    // v5.1.1 + v5.1.3 (slow-path work breakdown): per-section profiling.
    // Same single-writer rule as slow_path_latency.
    //
    // Sections (label rework in v5.1.3 — earlier "Other" lumped the heavy
    // rolling-state pushes with trivial setup; now split honestly):
    //   0=ROLLING   — EventLoop_UpdateRollingStateOneCore + cadence setup
    //                 (depth read, swap pickup, mtm_price). DOMINATES the
    //                 cycle: 4 RollingStats pushes × O(W) FPN math, full-
    //                 window recompute. Expect ~100-300µs in steady state
    //                 with W=1024 baseline.
    //   1=REBUILD   — EventLoop_RebuildOneCore: regime classify + strategy
    //                 dispatch + gate compute. Expect 5-30µs.
    //   2=PUSH      — seqlock push of pending_params to ExecutionCore.
    //                 Expect ~100-500ns (single FPN copy + atomic).
    //   3=TIME_EXIT — EventLoop_TimeExitOneCore. Expect ~100-300ns.
    //   4=TRAIL_SL  — EventLoop_TrailingSLRatchetOneCore. Expect ~100-300ns.
    //
    // Sum-of-sections ≈ slow_path_latency total (within rdtsc bracket noise).
    // ~10ns _Sample × 5 = ~50ns/cycle overhead, < 0.1% of typical cycle.
    static constexpr int SP_SECTION_ROLLING     = 0;
    static constexpr int SP_SECTION_REBUILD     = 1;
    static constexpr int SP_SECTION_PUSH        = 2;
    static constexpr int SP_SECTION_TIME_EXIT   = 3;
    static constexpr int SP_SECTION_TRAIL_SL    = 4;
    static constexpr int SP_SECTION_COUNT       = 5;
    // Back-compat aliases — earlier names kept so external callers don't
    // break. New code should use the names above.
    static constexpr int SP_SECTION_OTHER       = SP_SECTION_ROLLING;
    static constexpr int SP_SECTION_PUSH_PARAMS = SP_SECTION_PUSH;
    CoreLatencyStats slow_path_breakdown[SP_SECTION_COUNT];
    // v5.0.3 (Engine Topology advanced): live thread observability fields.
    // Single-writer is the slow-path thread that owns this core (or the
    // producer in centralized mode for cores it iterates). GUI publish
    // reads relaxed and copies into TUISnapshot::PerCoreSnap.
    //   sp_last_tick_us: wall-clock us at end of last cycle. Compare to
    //                    "now" for cadence-drift display in topology panel.
    //   sp_cycles_total: monotonic count of completed slow-path cycles.
    //   sp_yield_count:  monotonic count of cadence yields + parks.
    //   sp_state:        coarse thread state — 0=running, 1=parked
    //                    (reset_in_progress), 2=cadence-yield, 3=paused
    //                    (user via paused_engines_mask). Updated at the
    //                    transition points; readers see eventual values.
    std::atomic<uint64_t> sp_last_tick_us;
    std::atomic<uint64_t> sp_cycles_total;
    std::atomic<uint64_t> sp_yield_count;
    std::atomic<uint8_t>  sp_state;
    // v5.1.0 (per-core data-plane decoupling): each engine OWNS its
    // rolling/regime/flow state. Centralized: producer writes all N.
    // per_core_slow: per-core slow-path c writes its own (per-cadence
    // fields) + producer writes ema_price to all N (per-tick). See
    // CoreSlowState<F> doc above.
    //
    // POINTER (not inline) because CoreSlowState is ~3MB per engine and
    // 16 inline copies would overflow the 8MB default stack on tests
    // that put EventLoopState on the stack. Heap-allocated in
    // EventLoopState_Init; freed in EventLoopState_Free. Slow-path-only
    // access — the indirection cost is negligible at slow-path cadence.
    CoreSlowState<F>* slow_state;

    // v5.4.0 Phase 1.1 — per-strategy state (lifecycle stage 1 + 2: Init/Adapt).
    // Heap-allocated by Strategy_InitPerCore at engine boot; freed by
    // Strategy_FreePerCore on shutdown / strategy hot-swap. Concrete type
    // depends on strategy_state_kind:
    //   STRATEGY_MOMENTUM       → MomentumState<F>*
    //   STRATEGY_MEAN_REVERSION → MeanReversionState<F>*
    //   STRATEGY_SIMPLE_DIP     → SimpleDipState<F>*
    //   STRATEGY_EMA_CROSS      → EmaCrossState<F>*
    //   STRATEGY_ML             → MLStrategyState<F>*
    //   STRATEGY_AUTO/NONE      → nullptr (no state needed)
    // void* used to avoid pulling all strategy headers into ControllerEventLoop.hpp.
    // Concrete typing happens at Strategy_InitPerCore call sites where each
    // strategy's header is included.
    //
    // Single-writer per core (the per-core slow-path thread), single-reader
    // (same thread reading state for _Adapt and _BuildParameters). No
    // cross-thread access — strategy state stays per-engine just like
    // slow_state above.
    //
    // Snapshot persistence (v5.4 → SHARDED_SNAPSHOT_VERSION 4): only
    // strategy_state_kind is persisted. On load, Strategy_InitPerCore
    // is called to reallocate state from scratch matching the persisted
    // kind. Treated as session-only — strategies' adapted parameters
    // converge within a few cadences post-restart, so this is acceptable
    // for v5.4. Full persistence deferred to v5.5.0.
    void*    strategy_state;        // owned by this core's slow-path thread
    uint8_t  strategy_state_kind;   // matches strategy_id at allocation; 0xFF = uninitialized
    uint8_t  _pad_strategy_state[7];
};

//======================================================================================================
// [EVENT LOOP STATE]
//======================================================================================================
// holds everything the controller core needs to process events from N
// execution cores. cores array is indexed by registration order; once a core
// is registered its slot index is its core_id forever (no compaction on
// unregister, just mark inactive).
//
// Phase 03 chunk 1B: all financial state (portfolio, balance, realized_pnl,
// fee_rate, kill switch, trade log) now lives in OrderManagerState. the OMS
// pointer is the ONLY path to reach them. EventLoopState is a thin dispatcher
// that owns per-core contexts, event counters, and the OMS back-pointer.
//
// total_events_processed is a heartbeat / liveness counter useful for
// detecting a stalled controller (TUI shows it growing each frame).
//======================================================================================================
template <unsigned F>
struct alignas(64) EventLoopState {
    CoreContext<F> cores[MAX_EXECUTION_CORES];
    int registered_count;
    uint64_t total_events_processed;
    uint64_t total_entries;
    uint64_t total_exits;
    // Phase 03 chunk 1B: OMS back-pointer. MUST be non-null after Init —
    // all financial state reads go through oms->. EventLoopState_Init takes
    // the OMS pointer as its second argument and stores it here. callers that
    // pass nullptr will get crashes in any accessor or OnEvent call.
    OrderManagerState<F>* oms;
    // v5.12.1.A — wall-clock us of last WS tick received. Single-writer
    // (the producer thread in live, or the backtest driver in offline);
    // multiple-reader (per-core slow paths for the v5.12.1.A WS-staleness
    // emergency-flatten gate, and the GUI/TUI heartbeat indicator added in
    // v5.12.1.C). Initialized to 0 in EventLoopState_Init; rises
    // monotonically once ticks start flowing. The slow-path check is
    //   gap_us = now_us - last_ws_tick_us;
    //   if (gap_us >= cfg.ws_dead_time_flatten_threshold_secs * 1e6 &&
    //       cfg.ws_dead_time_flatten_enabled) → OMS_FlattenAll(...)
    // Pre-warmup (last_ws_tick_us == 0) is treated as "no flatten" so the
    // engine doesn't fire a phantom flatten before the first tick arrives.
    std::atomic<uint64_t> last_ws_tick_us;
    // v5.12.1.C — WS heartbeat throughput tracking. Producer fan_out
    // increments the bucket for the current second; the slow-path / GUI
    // sums all buckets within the last 5 seconds. ws_bucket_last_sec[i]
    // holds the wall-clock second when bucket i was last touched (so
    // stale buckets don't contribute). Single-writer (producer), single-
    // reader (snapshot publisher). Relaxed atomic on ws_ticks_per_5s
    // because monotonic + sub-tick accuracy doesn't matter for display.
    std::atomic<uint64_t> ws_ticks_per_5s;
    uint64_t ws_bucket_last_sec[5];   // producer-only writer; not atomic
    uint32_t ws_bucket_count[5];      // producer-only writer; not atomic
};

}  // namespace tt
// v5.4.0 Phase 2.1 — Strategy_AdaptPerCore / Strategy_InitPerCore /
// Strategy_FreePerCore live in StrategyLifecycle.hpp. Included here
// (post-EventLoopState definition) so the dispatcher can refer to
// CoreContext / EventLoopState without an include cycle.
#include "../Strategies/StrategyLifecycle.hpp"
namespace tt {

//======================================================================================================
// [INIT]
//======================================================================================================
// zero the dispatcher state, install the OMS back-pointer. all financial
// state (portfolio, balance, fee_rate, kill switch, trade log) lives in the
// OMS — EventLoopState just holds per-core contexts and event counters.
// the OMS must be initialized via OrderManager_Init BEFORE calling this.
//======================================================================================================
template <unsigned F>
inline void EventLoopState_Init(EventLoopState<F>* state,
                                OrderManagerState<F>* oms) {
    state->registered_count = 0;
    state->total_events_processed = 0;
    state->total_entries = 0;
    state->total_exits = 0;
    state->oms = oms;
    // v5.12.1.A — pre-warmup sentinel; first tick from producer/backtest
    // sets it to a monotonic wall-clock us value.
    state->last_ws_tick_us.store(0, std::memory_order_relaxed);
    // v5.12.1.C — heartbeat throughput tracking init.
    state->ws_ticks_per_5s.store(0, std::memory_order_relaxed);
    for (int i = 0; i < 5; ++i) {
        state->ws_bucket_last_sec[i] = 0;
        state->ws_bucket_count[i] = 0;
    }
    for (int i = 0; i < MAX_EXECUTION_CORES; i++) {
        state->cores[i].core = nullptr;
        state->cores[i].intended_tp = FPN_Zero<F>();
        state->cores[i].intended_sl = FPN_Zero<F>();
        state->cores[i].intended_qty = FPN_Zero<F>();
        state->cores[i].allocated_balance = FPN_Zero<F>();
        GateParameters_Init(&state->cores[i].pending_params);
        state->cores[i].strategy_id = STRATEGY_NONE;  // pitfall P6.5: explicit init
        state->cores[i].dirty = 0;
        state->cores[i].model_handle = nullptr;
        state->cores[i].ensemble_handle = nullptr;  // v5.10.0a.G.5 default
        state->cores[i].entries_processed = 0;
        state->cores[i].exits_processed = 0;
        // Phase 6prep sharded: ConfidenceScorer with safe defaults. EngineSharded
        // re-inits with cfg values for STRATEGY_ML cores after this; non-ML cores
        // keep these defaults (the scorer never gets fed, so it stays inert).
        ConfidenceScorer_Init(&state->cores[i].confidence,
                              CONFIDENCE_IC_WINDOW_DEFAULT,
                              CONFIDENCE_FRESHNESS_TAU_DEFAULT);
        // v5.14.1.B.1 (PARITY-003) — at this site cfg may be unavailable
        // (state init runs before EngineSharded re-inits with cfg values).
        // Boot sequence guarantees EngineSharded.hpp:1244 re-runs Init +
        // BindCompositeCfg with cfg AFTER this site for STRATEGY_ML cores.
        // Non-ML cores keep the safe defaults from Init alone (their scorer
        // is never fed via ConfidenceScorer_UpdateAndMark, so composite is
        // moot). No BindCompositeCfg here — defer to EngineSharded.
        // v5.10.0e — drift history starts empty; samples land post-fill.
        DriftHistory_Init(&state->cores[i].drift_history);
        state->cores[i].staged_prediction = 0.0;
        state->cores[i].active_prediction = 0.0;
        state->cores[i].last_confidence = 0.0;
        state->cores[i].last_entry_price = FPN_Zero<F>();
        state->cores[i].last_entry_tick  = 0;
        state->cores[i].last_entry_wall_us = 0;
        state->cores[i].sl_cooldown_remaining = 0;
        state->cores[i].halt_reason = HALT_OK;
        state->cores[i].strategy_halt_reason = SHALT_OK;
        // v5.6.6: sentinel = 0xFFFF so the first rebuild ALWAYS emits a
        // baseline gate log entry. Subsequent emits are edge-triggered.
        state->cores[i].prev_gate_log_state = 0xFFFF;
        // v4.0.3 B: regime state per AUTO core. Hysteresis threshold of 3
        // matches legacy default — requires 3 consecutive cycles of new
        // regime detection before switching.
        Regime_Init(&state->cores[i].regime_state, 3);
        // v4.0.3 D10: P&L feeder per core for adaptive filter shifts.
        state->cores[i].pnl_feeder = RegressionFeederX_Init<F>();
        state->cores[i].resolved_strategy_id = STRATEGY_NONE;  // v4.0.4
        // v4.0.4: per-core P&L counters
        state->cores[i].core_realized = FPN_Zero<F>();
        state->cores[i].core_fees = FPN_Zero<F>();
        state->cores[i].core_wins = 0;
        state->cores[i].core_losses = 0;
        // v4.7.21: pending-partner pairing state (per-trade W/L under partials)
        state->cores[i].partner_pending_pnl = FPN_Zero<F>();
        state->cores[i].partner_pending_active = 0;
        // v4.7.25: gross win/loss accumulators
        state->cores[i].core_gross_wins   = FPN_Zero<F>();
        state->cores[i].core_gross_losses = FPN_Zero<F>();
        // v4.7.42 (Phase E): per-core slow-path latency stats — mirrors
        // ExecutionCore::latency_stats. Init zeros + disabled until engine
        // explicitly enables (CoreLatencyStats_Enable).
        CoreLatencyStats_Init(&state->cores[i].slow_path_latency);
        // v5.1.1: per-section breakdown stats.
        for (int s = 0; s < CoreContext<F>::SP_SECTION_COUNT; ++s) {
            CoreLatencyStats_Init(&state->cores[i].slow_path_breakdown[s]);
        }
        // v5.0.3 (Engine Topology advanced): observability fields.
        state->cores[i].sp_last_tick_us.store(0, std::memory_order_relaxed);
        state->cores[i].sp_cycles_total.store(0, std::memory_order_relaxed);
        state->cores[i].sp_yield_count.store(0, std::memory_order_relaxed);
        state->cores[i].sp_state.store(0, std::memory_order_relaxed);
        // v5.1.0 (per-core data plane): per-engine slow_state allocation.
        // ~3MB per engine × 16 cores would overflow default stack if inline.
        //
        // v5.11.6.A — InitArena-backed allocation (replaces `new`). The arena
        // bumps from a single mmap'd region (MAP_POPULATE pre-faulted at
        // engine boot). Engine sets InitArena_Global() before this Init;
        // tests leave it nullptr and get the `new` fallback.
        if (auto* arena = tt::InitArena_Global()) {
            void* mem = tt::InitArena_Alloc(arena, sizeof(CoreSlowState<F>),
                                             alignof(CoreSlowState<F>));
            if (mem) {
                state->cores[i].slow_state = new (mem) CoreSlowState<F>();
            } else {
                state->cores[i].slow_state = new CoreSlowState<F>();
            }
        } else {
            state->cores[i].slow_state = new CoreSlowState<F>();
        }
        CoreSlowState_Init(state->cores[i].slow_state);
        // v5.4.0 Phase 1.1: per-strategy state. Allocated by
        // Strategy_InitPerCore at engine boot AFTER cfg is read so the
        // dispatcher knows which kind to allocate. Init here just to
        // nullptr/0xFF — caller is responsible for calling
        // Strategy_InitPerCore. _Free handles the cleanup symmetrically.
        state->cores[i].strategy_state      = nullptr;
        state->cores[i].strategy_state_kind = 0xFF;  // 0xFF = uninitialized sentinel
        // Phase 2.1: per-core open notional (sum of entry_price × qty)
        state->cores[i].core_open_notional = FPN_Zero<F>();
        // Phase 3: per-core kill switch state. peak starts at zero; the
        // first slow-path rebuild bumps it to (allocated + 0 + 0) which
        // then becomes the high-water mark.
        state->cores[i].core_peak_balance     = FPN_Zero<F>();
        state->cores[i].core_dd_pct           = FPN_Zero<F>();
        state->cores[i].core_kill_tripped     = 0;
        state->cores[i].core_ks_trips_total   = 0;
        // v4.2.1: idle-cycle counter for death-spiral detection
        state->cores[i].idle_cycles = 0;
        // v5.9.0b — ML observability fields. Without explicit init, the
        // hard-block / nan-counter / rate-limit-log paths read garbage on
        // fresh-state runs. Caught by v5.9.1 parity audit (V5_9_AUDIT-#9
        // follow-up). Same pattern as the staged_prediction / last_confidence
        // init above — every CoreContext field declared in v5.9 needs to
        // land here.
        state->cores[i].model_load_failed              = 0;
        state->cores[i].cfg_drift_tier1_count          = 0;
        state->cores[i].cfg_drift_tier2_count          = 0;
        state->cores[i].cfg_drift_strict_refused       = 0;
        state->cores[i].last_ml_critical_log_us        = 0;
        state->cores[i].last_ml_threshold              = 0.0;
        state->cores[i].last_ml_effective_threshold    = 0.0;
        state->cores[i].nan_feature_events_total       = 0;
        state->cores[i].nan_prediction_events_total    = 0;
        // v5.13.0.B — sell-side ML prediction state. Reset to 0 each cycle
        // by RebuildOneCore via mctx wiring; init here for first-cycle
        // safety (slow-path post-rebuild check reads this before any
        // RebuildOneCore writes can have happened on cold boot).
        state->cores[i].last_exit_prediction           = 0.0;
        state->cores[i].last_exit_dominant_horizon     = -1;
        // v5.9.1 — edge-trigger flag for boot-time per-core warmup-complete
        // log. Set to 1 once per session per core after the first slow-path
        // rebuild that observes rolling.count >= min_warmup_samples.
        state->cores[i].warmup_log_emitted             = 0;
    }
}

//======================================================================================================
// [INIT LEGACY — convenience for tests]
//======================================================================================================
// test helper that creates an OMS + wires it into the EventLoopState in one
// call. the caller provides the OMS on the stack alongside the state. uses a
// default-constructed (zeroed) ExchangeAdapter and live_trading=0 (paper mode).
//======================================================================================================
template <unsigned F>
inline void EventLoopState_InitLegacy(EventLoopState<F>* state,
                                       OrderManagerState<F>* oms,
                                       FPN<F> starting_balance,
                                       FPN<F> fee_rate) {
    ExchangeAdapter<F> empty{};
    OrderManager_Init(oms, empty, 0, starting_balance, fee_rate);
    EventLoopState_Init(state, oms);
}

//======================================================================================================
// [FREE — v5.1.0]
//======================================================================================================
// Free heap-allocated CoreSlowState pointers. Idempotent: safe to call
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
    // destructor (RAII at scope exit, OrderManager.hpp:316). Don't stop
    // the writer here — the test might still use `oms` after this Free
    // returns; premature stop would race with subsequent OMS work.
    for (int i = 0; i < MAX_EXECUTION_CORES; ++i) {
        if (state->cores[i].slow_state) {
            // v5.11.6.A — if the arena owns this allocation, it's freed
            // by InitArena_Destroy at engine shutdown — skip delete here.
            // Otherwise (test path / no arena), `new` allocated it and
            // we delete normally.
            //
            // Placement-new'd objects need explicit destructor call before
            // the arena reclaims their memory, but CoreSlowState is
            // trivially destructible (no pointers it owns; all FPN +
            // POD). For non-trivial types, add a manual ->~CoreSlowState<F>()
            // here when the arena is in use.
            if (!tt::InitArena_Owns(tt::InitArena_Global(),
                                     state->cores[i].slow_state)) {
                delete state->cores[i].slow_state;
            }
            state->cores[i].slow_state = nullptr;
        }
        // strategy_state is freed by caller via Strategy_FreePerCore
        // before this function (see contract above). If it's still
        // non-null here, it's a leak — log to help catch the missing
        // call. Continue rather than dispatch (we don't know the kind
        // here without the strategy headers).
        if (state->cores[i].strategy_state) {
            fprintf(stderr,
                "[EventLoopState_Free] WARN: slot %d has non-null strategy_state "
                "kind=%u — caller forgot to call Strategy_FreePerCore. Leaking.\n",
                i, state->cores[i].strategy_state_kind);
            state->cores[i].strategy_state = nullptr;  // prevent dangling
            state->cores[i].strategy_state_kind = 0xFF;
        }
    }
}

//======================================================================================================
// [REGISTER CORE]
//======================================================================================================
// claim a slot for this execution core. core_id is set to the slot index so
// future events from this core route to the right portfolio slot.
//
// returns the assigned slot (== core_id), or -1 if MAX_EXECUTION_CORES is full.
//
// the intended_tp / intended_sl / intended_qty parameters are the trade
// parameters the controller wants to apply on the next entry. phase 05 will
// allow the controller to update them between entries via a separate push
// protocol; for phase 04 they're set once at registration and reused.
//======================================================================================================
template <unsigned F>
inline int EventLoopState_RegisterCore(EventLoopState<F>* state,
                                       ExecutionCore<F>* core,
                                       FPN<F> intended_tp,
                                       FPN<F> intended_sl,
                                       FPN<F> intended_qty) {
    if (state->registered_count >= MAX_EXECUTION_CORES) return -1;
    int slot = state->registered_count++;
    state->cores[slot].core         = core;
    state->cores[slot].intended_tp  = intended_tp;
    state->cores[slot].intended_sl  = intended_sl;
    state->cores[slot].intended_qty = intended_qty;
    state->cores[slot].entries_processed = 0;
    state->cores[slot].exits_processed   = 0;
    core->core_id = (uint16_t)slot;
    return slot;
}

//======================================================================================================
// [PARTIAL EXITS — LEG SLOT MAPPING]
//======================================================================================================
// Per `plans/partial-exits-sharded.md` (P.1, 2026-04-27): when
// `cfg.partial_exit_enabled=1`, each core owns TWO portfolio slots — one
// for leg A (first half exits at TP1), one for leg B (second half rides
// to TP2 or shared SL). When disabled, each core owns ONE slot at index
// == core_id (legacy single-position behavior).
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
// refuses to start if num_execution_cores × 2 > MAX_PORTFOLIO_POSITIONS.
//
// LEG INDICES — the constants live in CoreFrameworks/TradeEvent.hpp so
// ExecutionCore_Tick (which doesn't include this header) can use the same
// names. Re-stated here as a comment for readability:
//   PARTIAL_LEG_A = 0
//   PARTIAL_LEG_B = 1

// Returns the portfolio slot index for (core_id, leg) given the cfg.
// leg=0 always returns a valid slot; leg=1 returns -1 when partial_exit_-
// enabled=0. Caller-side: ignore leg=1 result when partials disabled.
//
// All slow-path / boot-time. Trivially inlined.
static inline int Sharded_LegSlot(int core_id, int leg, int partial_exit_enabled) {
    if (core_id < 0) return -1;
    if (!partial_exit_enabled) {
        // Single-slot mode: leg index ignored, slot == core_id
        return (leg == PARTIAL_LEG_A) ? core_id : -1;
    }
    // Pair mode: leg A = 2c, leg B = 2c+1
    if (leg != PARTIAL_LEG_A && leg != PARTIAL_LEG_B) return -1;
    int slot = core_id * 2 + leg;
    if (slot >= MAX_PORTFOLIO_POSITIONS) return -1;
    return slot;
}

// Build the bitmap mask for core c's portfolio slot(s). Under partials,
// core c owns slots 2c and 2c+1 (legs A and B). Without partials,
// core c owns just slot c. Used by slow-path checks that ask "is core
// c currently in any position?" or "what portfolio slots does this
// core occupy?".
static inline uint16_t Sharded_CoreSlotMask(int core_id, int partial_exit_enabled) {
    if (core_id < 0 || core_id >= MAX_PORTFOLIO_POSITIONS) return 0;
    if (partial_exit_enabled) {
        int sa = core_id * 2, sb = core_id * 2 + 1;
        if (sb >= MAX_PORTFOLIO_POSITIONS) return 0;
        return (uint16_t)((1u << sa) | (1u << sb));
    }
    return (uint16_t)(1u << core_id);
}

// Boot-time validation. Returns 1 if cfg + capacity are consistent, 0
// otherwise (and prints the reason to stderr). Call from engine startup
// AFTER cfg load, BEFORE core registration.
//
// Failure modes:
//   - partial_exit_enabled=1 AND num_execution_cores * 2 > MAX_PORTFOLIO_POSITIONS
//   - num_execution_cores < 1 (caller should already validate)
//   - partial_exit_pct outside (0.0, 1.0) when partials enabled
template <unsigned F>
static inline int Sharded_ValidatePartialExitCfg(const ControllerConfig<F>* cfg) {
    if (!cfg->partial_exit_enabled) return 1;  // disabled = always valid
    int n_cores = (int)cfg->num_execution_cores;
    if (n_cores < 1) {
        std::fprintf(stderr,
            "[partial-exits] num_execution_cores=%d invalid; needs >= 1\n",
            n_cores);
        return 0;
    }
    int max_pair_cores = MAX_PORTFOLIO_POSITIONS / 2;
    if (n_cores > max_pair_cores) {
        std::fprintf(stderr,
            "[partial-exits] partial_exit_enabled=1 caps num_execution_cores "
            "at %d (got %d). Each core uses TWO portfolio slots in pair mode "
            "(leg A + leg B); MAX_PORTFOLIO_POSITIONS=%d → %d cores max.\n"
            "  Either: (a) reduce num_execution_cores to %d or fewer, or\n"
            "          (b) set partial_exit_enabled=0 in your cfg.\n",
            max_pair_cores, n_cores, MAX_PORTFOLIO_POSITIONS, max_pair_cores,
            max_pair_cores);
        return 0;
    }
    double pct = FPN_ToDouble(cfg->partial_exit_pct);
    if (pct <= 0.0 || pct >= 1.0) {
        std::fprintf(stderr,
            "[partial-exits] partial_exit_pct=%.4f invalid; needs (0, 1) "
            "exclusive. 0.5 = exit half at TP1, half rides to TP2.\n",
            pct);
        return 0;
    }
    std::fprintf(stderr,
        "[partial-exits] enabled: %d cores using %d slots (legs A+B), "
        "TP1 exits %.0f%% of qty\n",
        n_cores, n_cores * 2, pct * 100.0);
    return 1;
}

//======================================================================================================
// [SET CORE STRATEGY]
//======================================================================================================
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
// FPN<F> currency, not percentage.
//======================================================================================================
template <unsigned F>
inline void EventLoopState_SetCoreStrategy(EventLoopState<F>* state, int slot,
                                            uint8_t strategy_id,
                                            FPN<F> allocated_balance) {
    if (slot < 0 || slot >= state->registered_count) return;
    state->cores[slot].strategy_id       = strategy_id;
    state->cores[slot].allocated_balance = allocated_balance;
}

//======================================================================================================
// [ATTACH TRADE LOG — phase 08]
//======================================================================================================
// install an optional CSV trade log. pass nullptr to disable. ownership stays
// with the caller (the engine main usually); EventLoop never opens or closes
// the log. attach is idempotent — calling twice replaces the previous pointer.
//
// pitfall P8.2: only the controller core writes to this log. don't share the
// pointer with GUI or other threads. if the GUI wants to display trades it
// must read the CSV file from disk, never call the recorder directly.
//======================================================================================================
template <unsigned F>
inline void EventLoopState_AttachTradeLog(EventLoopState<F>* state,
                                          ShardedTradeLog* log) {
    state->oms->trade_log = log;
}

//======================================================================================================
// [ATTACH OMS — phase 03 chunk 1B]
//======================================================================================================
// Replace the OMS pointer. Mainly useful for re-wiring after Init if the
// OMS was constructed separately (e.g. EngineSharded.hpp pre-1B callers).
// EventLoopState_Init already wires the OMS pointer, so most call sites
// don't need this anymore.
//======================================================================================================
template <unsigned F>
inline void EventLoopState_AttachOms(EventLoopState<F>* state,
                                      OrderManagerState<F>* oms) {
    state->oms = oms;
}

//======================================================================================================
// [BANK ACCESSORS — phase 03 chunk 1B]
//======================================================================================================
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
inline FPN<F> EventLoopState_Balance(const EventLoopState<F>* state) {
    return state->oms->balance;
}

template <unsigned F>
inline FPN<F> EventLoopState_RealizedPnl(const EventLoopState<F>* state) {
    return state->oms->realized_pnl;
}

template <unsigned F>
inline FPN<F> EventLoopState_FeeRate(const EventLoopState<F>* state) {
    return state->oms->fee_rate;
}

template <unsigned F>
inline const Portfolio<F>* EventLoopState_Portfolio(const EventLoopState<F>* state) {
    return &state->oms->portfolio;
}

template <unsigned F>
inline Portfolio<F>* EventLoopState_PortfolioMut(EventLoopState<F>* state) {
    return &state->oms->portfolio;
}

template <unsigned F>
inline FPN<F> EventLoopState_KsMinBalance(const EventLoopState<F>* state) {
    return state->oms->ks_min_balance;
}

template <unsigned F>
inline FPN<F> EventLoopState_KsMaxDrawdownPct(const EventLoopState<F>* state) {
    return state->oms->ks_max_drawdown_pct;
}

template <unsigned F>
inline FPN<F> EventLoopState_KsPeakBalance(const EventLoopState<F>* state) {
    return state->oms->ks_peak_balance;
}

template <unsigned F>
inline uint8_t EventLoopState_KillSwitchTripped(const EventLoopState<F>* state) {
    return state->oms->kill_switch_tripped;
}

template <unsigned F>
inline uint64_t EventLoopState_KsTripsTotal(const EventLoopState<F>* state) {
    return state->oms->ks_trips_total;
}

template <unsigned F>
inline ShardedTradeLog* EventLoopState_TradeLog(const EventLoopState<F>* state) {
    return state->oms->trade_log;
}

//======================================================================================================
// [SET INTENDED PARAMS]
//======================================================================================================
// for phase 04, the controller updates the intended TP/SL/qty for a core
// directly through this helper. phase 05 replaces it with an atomic
// ParameterSlot push that races safely against the execution core. for now
// it's just a write to the controller-side struct, no synchronization needed
// because the execution core never reads from CoreContext (only from its own
// gate_params, which we DO NOT touch from this function — see P4.7).
//======================================================================================================
template <unsigned F>
inline void EventLoopState_SetIntendedParams(EventLoopState<F>* state, int slot,
                                              FPN<F> tp, FPN<F> sl, FPN<F> qty) {
    if (slot < 0 || slot >= state->registered_count) return;
    state->cores[slot].intended_tp  = tp;
    state->cores[slot].intended_sl  = sl;
    state->cores[slot].intended_qty = qty;
}

//======================================================================================================
// [DRAIN POST-FILL — mode 1 per-core stats consumer]
//======================================================================================================
// Run by the drainer thread after every OrderManager_Tick. Consumes the
// FillRecords + masks populated by OrderManager_HandleFill and applies
// per-core CoreContext updates that the legacy mode-0 path used to do
// inside EventLoop_OnEvent: core_open_notional, core_realized, core_fees,
// core_wins/core_losses, ConfidenceScorer feedback, SL cooldown,
// pnl_feeder push.
//
// Slot → core_id mapping is partials-aware via oms->partial_exit_enabled.
// Both legs of a paired trade route their stats to the SAME CoreContext
// (one per core, not one per leg) — partials only split the exit
// schedule, not the allocation.
//
// Per-leg vs per-trade fields (load-bearing under partials):
//   - PER-LEG (every exit fill contributes): core_realized,
//     core_open_notional, core_fees. Each leg has its own qty + entry
//     notional + fee, so all aggregate.
//   - PER-TRADE (leg-A only fires the signal): core_wins/core_losses,
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
// the same thread is sole writer). FPN-pure on the per-core math.
//======================================================================================================
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
template <unsigned F>
inline void EventLoop_DrainPostFillOneCore(EventLoopState<F>* state,
                                             OrderManagerState<F>* oms,
                                             uint32_t sl_cooldown_cycles,
                                             int core_id,
                                             double ensemble_trade_reward_mult = 4.0,
                                             // v5.10.0e — runtime IC drift detection.
                                             // Defaults preserve pre-v5.10.0e behavior:
                                             // floor=0 → DriftHistory_CheckBreach skip
                                             // (avg < 0 floor never fires).
                                             double drift_floor                = 0.0,
                                             uint32_t drift_window_seconds     = 86400u,
                                             int      drift_auto_kill          = 0,
                                             // v5.13.4 — sell-side bandit reward
                                             // attribution. Default 0 = disabled
                                             // (preserves pre-v5.13.4 behavior; legacy
                                             // test callers using 3-arg form unaffected).
                                             int      exit_bandit_enabled      = 0,
                                             double   fee_rate_taker_for_cf    = 0.001) {
    const int partial_on = oms->partial_exit_enabled ? 1 : 0;
    uint16_t my_mask = partial_on
        ? (uint16_t)((1u << (core_id * 2)) | (1u << (core_id * 2 + 1)))
        : (uint16_t)(1u << core_id);
    const int max_slot = partial_on ? state->registered_count * 2
                                    : state->registered_count;

    CoreContext<F>& ctx = state->cores[core_id];

    // v5.4.1 Bug B diagnostic: log when this core has bits to process.
    // Cheap (cfg-gated, no-op when disabled). Helps narrow whether the
    // mask plumbing or the increment math is the cause of stuck-zero
    // per-core counters.
    {
        uint16_t hit_open  = (uint16_t)(oms->last_opened_mask & my_mask);
        uint16_t hit_close = (uint16_t)(oms->last_closed_mask & my_mask);
        if ((hit_open || hit_close) && tt::Health_LogEnabled(tt::HEALTH_INFO)) {
            tt::Health_Log(tt::HEALTH_INFO, "drain", core_id,
                "my_mask=0x%x last_open=0x%x last_close=0x%x partial=%d realized_pre=%g fees_pre=%g wins=%u losses=%u",
                (unsigned)my_mask,
                (unsigned)oms->last_opened_mask,
                (unsigned)oms->last_closed_mask,
                partial_on,
                FPN_ToDouble(ctx.core_realized),
                FPN_ToDouble(ctx.core_fees),
                ctx.core_wins, ctx.core_losses);
        }
    }

    // ---- Entries: open_notional / fees + heartbeat counters ----
    uint16_t open_mask = (uint16_t)(oms->last_opened_mask & my_mask);
    while (open_mask) {
        int slot = __builtin_ctz(open_mask);
        open_mask &= (uint16_t)(open_mask - 1);
        if (slot < 0 || slot >= max_slot) continue;
        const auto& rec = oms->last_fill[slot];
        ctx.core_open_notional = FPN_Add(ctx.core_open_notional, rec.entry_notional);
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
            tt::Health_Log(tt::HEALTH_INFO, "entry", core_id,
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
                FPN_ToDouble(pos.entry_price),
                FPN_ToDouble(pos.quantity),
                FPN_ToDouble(rec.entry_notional),
                FPN_ToDouble(rec.entry_fee),
                FPN_ToDouble(ctx.diag_tp_pct_actual),
                FPN_ToDouble(ctx.diag_tp_pct_floor),
                FPN_ToDouble(ctx.diag_stddev_pct),
                FPN_ToDouble(ctx.diag_long_slope),
                FPN_ToDouble(ctx.diag_volume_delta),
                // v5.9.0b — ML decision context. Zero for non-ML cores.
                is_ml ? ctx.active_prediction : 0.0,
                is_ml ? ctx.last_ml_threshold : 0.0,
                is_ml ? ctx.last_ml_effective_threshold : 0.0,
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
        const auto& rec = oms->last_fill[slot];

        // Per-leg accounting: every exit fill contributes.
        ctx.core_realized      = FPN_Add(ctx.core_realized, rec.exit_net_pnl);
        ctx.core_open_notional = FPN_SubSat(ctx.core_open_notional, rec.exit_entry_notional);
        ctx.core_fees          = FPN_AddSat(ctx.core_fees, rec.exit_total_fees);
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
        if (tt::Health_LogEnabled(tt::HEALTH_INFO)) {
            double realized = oms->last_realized_return[slot];
            tt::Health_Log(tt::HEALTH_INFO, "exit", core_id,
                "slot=%d strat=%u resolved=%u was_win=%d realized_ret=%g "
                "net_pnl=%g entry_notional=%g total_fees=%g leg_a=%d",
                slot, (unsigned)ctx.strategy_id,
                (unsigned)ctx.resolved_strategy_id,
                (int)rec.was_win, realized,
                FPN_ToDouble(rec.exit_net_pnl),
                FPN_ToDouble(rec.exit_entry_notional),
                FPN_ToDouble(rec.exit_total_fees),
                is_leg_a ? 1 : 0);
        }

        // v4.7.21 W/L pairing under partials (unchanged).
        if (partial_on) {
            if (ctx.partner_pending_active) {
                FPN<F> total_net = FPN_Add(ctx.partner_pending_pnl, rec.exit_net_pnl);
                if (FPN_GreaterThan(total_net, FPN_Zero<F>())) {
                    ctx.core_wins++;
                    ctx.core_gross_wins = FPN_Add(ctx.core_gross_wins, total_net);
                } else {
                    ctx.core_losses++;
                    ctx.core_gross_losses = FPN_Add(ctx.core_gross_losses,
                                                    FPN_Sub(FPN_Zero<F>(), total_net));
                }
                ctx.partner_pending_pnl = FPN_Zero<F>();
                ctx.partner_pending_active = 0;
            } else {
                ctx.partner_pending_pnl = rec.exit_net_pnl;
                ctx.partner_pending_active = 1;
            }
        }

        // Per-trade signals: leg A only.
        if (is_leg_a) {
            if (!partial_on) {
                ctx.core_wins   += (rec.was_win ? 1u : 0u);
                ctx.core_losses += (rec.was_win ? 0u : 1u);
                if (rec.was_win) {
                    ctx.core_gross_wins = FPN_Add(ctx.core_gross_wins, rec.exit_net_pnl);
                } else {
                    ctx.core_gross_losses = FPN_Add(ctx.core_gross_losses,
                                                    FPN_Sub(FPN_Zero<F>(), rec.exit_net_pnl));
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
                    double ic_now = RollingIC_Compute(&ctx.confidence.ic);
                    DriftHistory_Push(&ctx.drift_history, ic_now, now_us);
                    double avg_ic = 0.0;
                    int    n_samples = 0;
                    int    breach = DriftHistory_CheckBreach(
                        &ctx.drift_history, now_us,
                        (uint64_t)drift_window_seconds * 1000000ULL,
                        drift_floor, &avg_ic, &n_samples);
                    if (breach && !ctx.drift_history.breached) {
                        // First breach — log CRITICAL + record onset.
                        // Rate-limit at 60s per core to avoid log spam if
                        // breach toggles around the threshold.
                        ctx.drift_history.breached = 1;
                        ctx.drift_history.breach_first_us = now_us;
                        static uint64_t s_drift_log_us[16] = {0};
                        Health_LogCriticalRateLimited(
                            &s_drift_log_us[core_id & 15], 60000000ULL,
                            core_id, "drift",
                            "IC=%.4f below floor=%.4f over %us window (%d samples)",
                            avg_ic, drift_floor,
                            (unsigned)drift_window_seconds, n_samples);
                        if (drift_auto_kill && !ctx.drift_history.kill_tripped) {
                            state->cores[core_id].core_kill_tripped = 1;
                            state->cores[core_id].core_ks_trips_total++;
                            ctx.drift_history.kill_tripped = 1;
                            static uint64_t s_drift_kill_log_us[16] = {0};
                            Health_LogCriticalRateLimited(
                                &s_drift_kill_log_us[core_id & 15], 60000000ULL,
                                core_id, "drift",
                                "AUTO-KILL: per-core kill_switch tripped due to "
                                "sustained IC drift");
                        }
                    } else if (!breach && ctx.drift_history.breached) {
                        // Recovery — clear breach state, log info
                        ctx.drift_history.breached = 0;
                        fprintf(stderr,
                            "[drift] core %d RECOVERED: IC=%.4f above floor=%.4f\n",
                            core_id, avg_ic, drift_floor);
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
            // void* since CoreContext can't depend on EnsembleModelZoo<F>
            // directly (would force ML_Headers visibility). Bandit feed
            // is rare (~1 per closed trade) — cost negligible.
            if (ctx.ensemble_handle) {
                auto* ezoo = static_cast<EnsembleModelZoo<F>*>(ctx.ensemble_handle);
                if (ezoo->active && ezoo->initialized_bandits) {
                    double bal_d = FPN_ToDouble(oms->balance);
                    if (bal_d > 0.0) {
                        double pnl_d = FPN_ToDouble(rec.exit_net_pnl);
                        double pnl_bps = (pnl_d / bal_d) * 10000.0;
                        EnsembleModelZoo_TradeCloseReward(ezoo, pnl_bps,
                                                            ensemble_trade_reward_mult);
                    }
                }
            }
        }

        // v5.13.4 — sell-side bandit reward attribution. Per-LEG (not
        // leg-A only) since each slot's exit decision is independent
        // under partials. Conditions for crediting exit_bandit:
        //   1. cfg.exit_bandit_enabled
        //   2. per-slot last_exit_was_predicted captured at submit
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
        if (exit_bandit_enabled
            && slot < (int)MAX_PORTFOLIO_POSITIONS
            && oms->last_exit_was_predicted[slot] == 1
            && oms->flatten_pending.load(std::memory_order_acquire) == 0
            && ctx.ensemble_handle) {
            auto* ezoo = static_cast<EnsembleModelZoo<F>*>(ctx.ensemble_handle);
            int chosen_arm = (int)oms->last_exit_predicted_arm[slot];
            int regime     = (int)oms->last_exit_predicted_regime[slot];
            if (ezoo->initialized_exit_bandits
                && chosen_arm >= 0
                && chosen_arm < ezoo->exit_predictor_count
                && regime >= 0 && regime < NUM_REGIMES) {
                // Original TP locked at entry — captures the trade's
                // intended TP target without staleness from later
                // ratchet writes (those modify take_profit_price not
                // original_tp).
                FPN<F> entry_p   = oms->portfolio.positions[slot].entry_price;
                FPN<F> orig_tp   = oms->portfolio.positions[slot].original_tp;
                double entry_d   = FPN_ToDouble(entry_p);
                double orig_tp_d = FPN_ToDouble(orig_tp);
                if (entry_d > 0.0 && orig_tp_d > entry_d) {
                    double tp_pct = (orig_tp_d - entry_d) / entry_d;
                    double hypothetical_pnl_bps =
                        (tp_pct - 2.0 * fee_rate_taker_for_cf) * 10000.0;
                    double notional_d = FPN_ToDouble(rec.exit_entry_notional);
                    double actual_pnl_bps = (notional_d > 0.0)
                        ? FPN_ToDouble(rec.exit_net_pnl) / notional_d * 10000.0
                        : 0.0;
                    double reward_bps = actual_pnl_bps - hypothetical_pnl_bps;
                    Bandit_Update(&ezoo->exit_bandits[regime],
                                  chosen_arm, reward_bps);
                }
            }
        }
        // v5.13.0.B + v5.13.4 — clear per-slot exit-prediction state
        // post-attribution (single-use per trade).
        if (slot < (int)MAX_PORTFOLIO_POSITIONS) {
            oms->last_exit_was_predicted[slot]    = 0;
            oms->last_exit_predicted_p[slot]      = 0.0;
            oms->last_exit_predicted_arm[slot]    = -1;
            oms->last_exit_predicted_regime[slot] = -1;
        }
    }
    oms->last_closed_mask &= (uint16_t)~my_mask;  // clear only my bits
}

// Wrapper: iterate registered cores. Centralized + backtest paths use this.
template <unsigned F>
inline void EventLoop_DrainPostFill(EventLoopState<F>* state,
                                     OrderManagerState<F>* oms,
                                     uint32_t sl_cooldown_cycles,
                                     double ensemble_trade_reward_mult = 4.0,
                                     // v5.10.0e — drift detection params (forwarded
                                     // to OneCore; defaults preserve pre-v5.10.0e
                                     // behavior).
                                     double drift_floor                = 0.0,
                                     uint32_t drift_window_seconds     = 86400u,
                                     int      drift_auto_kill          = 0,
                                     // v5.13.4 — sell-side bandit forwarded to
                                     // OneCore. Default 0/0.001 = disabled +
                                     // typical taker rate (defensive when cfg
                                     // unwired in legacy callers).
                                     int      exit_bandit_enabled      = 0,
                                     double   fee_rate_taker_for_cf    = 0.001) {
    for (int c = 0; c < state->registered_count; ++c) {
        EventLoop_DrainPostFillOneCore(state, oms, sl_cooldown_cycles, c,
                                         ensemble_trade_reward_mult,
                                         drift_floor, drift_window_seconds,
                                         drift_auto_kill,
                                         exit_bandit_enabled,
                                         fee_rate_taker_for_cf);
    }
}

//======================================================================================================
// [ON EVENT]
//======================================================================================================
// process one TradeEvent. dispatches to entry or exit handling based on
// event.type bits. updates portfolio + balance + statistics. does NOT call
// kill switch eval, regime update, or any code that might mutate gate_params
// (those go through deferred dirty-flag mechanisms in later phases).
//
// entry handling:
//   - look up CoreContext for event.core_id (the source core)
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
//======================================================================================================
template <unsigned F>
inline void EventLoop_OnEvent(EventLoopState<F>* state, const TradeEvent<F>& event_in) {
    // v4.2.1 — paper-mode slippage simulation. In live, event.price comes
    // from the WS executionReport (already includes real exchange slippage).
    // In paper, event.price is the tick that triggered the gate; adjust to
    // model realistic worst-case execution: BUY fills above gate price,
    // SELL fills below trigger price. Mirrors legacy PortfolioController
    // behavior (PortfolioController.hpp:1041 + :659).
    //
    // Mutate a local copy so the caller's event is untouched.
    TradeEvent<F> event = event_in;
    if (!state->oms->live_trading && !FPN_IsZero(state->oms->slippage_pct)) {
        FPN<F> slip = FPN_Mul(event.price, state->oms->slippage_pct);
        if (event.type & TRADE_EVENT_ENTRY) {
            event.price = FPN_Add(event.price, slip);
        } else if (event.type & TRADE_EVENT_EXIT) {
            event.price = FPN_Sub(event.price, slip);
        }
    }
    int slot = (int)event.core_id;
    if (slot < 0 || slot >= state->registered_count) return;

    CoreContext<F>* ctx = &state->cores[slot];
    bool is_entry = (event.type & TRADE_EVENT_ENTRY) != 0;
    bool is_exit  = (event.type & TRADE_EVENT_EXIT)  != 0;

    // same-tick entry+exit is impossible by ExecutionCore_Tick construction
    // (mutually exclusive masks), but assert against it explicitly so a fuzz
    // test or future bug can't sneak through.
    if (is_entry && is_exit) return;

    // === EVENT LOG MODE 1: OMS owns portfolio mutation ===
    // In mode 1 the fill handler inside OMS_Tick opens/closes portfolio
    // slots and updates balance. OnEvent just bumps counters so the
    // statistics stay correct for the TUI and the drainer loop.
    if (state->oms->event_log_mode == 1) {
        // v4.7.19: do NOT bump heartbeat counters here. OnEvent fires for
        // every TradeEvent the drainer pops — but the actual fill (and
        // CSV write) happens later in OMS_Tick → HandleFill. Bumping here
        // over-counts when Submit fails, when the result_queue is full,
        // or when the slot was already closed by a racing manual-close.
        // The counters are now bumped atomically with the CSV write in
        // EventLoop_DrainPostFill (which walks last_opened_mask /
        // last_closed_mask that HandleFill populates per actual fill).
        // OnEvent's mode-1 path is now a pure no-op return.
        (void)ctx;
        (void)is_entry;
        (void)is_exit;
        return;
    }

    // === MODE 0: legacy OnEvent path (unchanged) ===
    if (is_entry) {
        // Compute entry fee = entry_price * qty * fee_rate
        // Phase 8: synchronous market BUY = taker by exchange definition.
        // OMS HandleFill (mode 1) will book the actual maker/taker fee from
        // the WS executionReport. This sync accounting is optimistic.
        FPN<F> notional = FPN_Mul(event.price, ctx->intended_qty);
        FPN<F> entry_fee = FPN_Mul(notional, state->oms->fee_rate_taker);
        Portfolio_OpenSlot(&state->oms->portfolio, slot,
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
        ctx->core_open_notional = FPN_Add(ctx->core_open_notional, notional);
        // CSV: record AFTER portfolio mutation so the slot is consistent if the
        // log call inspects it (currently it doesn't, but kept defensive).
        if (state->oms->trade_log) {
            ShardedTradeLog_RecordEntry(state->oms->trade_log, event,
                                        ctx->strategy_id,
                                        event.price,
                                        ctx->intended_qty,
                                        entry_fee,
                                        state->oms->balance);
        }
        return;
    }

    if (is_exit) {
        // close slot returns gross. apply both entry fee (already paid at fill
        // time, recorded in position) and exit fee (computed from exit notional).
        // Snapshot the position fields BEFORE CloseSlot clears the bit, so the
        // CSV row sees the entry_price + qty even though the slot is "closed".
        FPN<F> entry_price_snap = state->oms->portfolio.positions[slot].entry_price;
        FPN<F> qty_snap = state->oms->portfolio.positions[slot].quantity;
        FPN<F> entry_fee = state->oms->portfolio.positions[slot].entry_fee;
        FPN<F> gross = Portfolio_CloseSlot(&state->oms->portfolio, slot, event.price);
        FPN<F> exit_notional = FPN_Mul(event.price, qty_snap);
        // Phase 8: TP/SL exit = market sell = always taker by exchange def.
        FPN<F> exit_fee = FPN_Mul(exit_notional, state->oms->fee_rate_taker);
        FPN<F> total_fee = FPN_Add(entry_fee, exit_fee);
        FPN<F> net = FPN_Sub(gross, total_fee);
        state->oms->balance = FPN_Add(state->oms->balance, net);
        state->oms->realized_pnl = FPN_Add(state->oms->realized_pnl, net);
        // v4.0.4: per-core P&L bookkeeping. The OMS keeps a single global
        // accumulator (one portfolio); we split it back out by source core
        // for the Account panel so users can see which core is making/losing
        // money. core_fees adds the entry+exit fee for this fill.
        // Branchless win/loss: FPN_GreaterThan returns 1/0, used as integer
        // mask. Slow path so cost is irrelevant — kept branchless for
        // consistency with the rest of the engine.
        ctx->core_realized = FPN_Add(ctx->core_realized, net);
        ctx->core_fees = FPN_Add(ctx->core_fees, total_fee);
        uint32_t is_win = (uint32_t)FPN_GreaterThan(net, FPN_Zero<F>());
        ctx->core_wins   += is_win;
        ctx->core_losses += (1u - is_win);
        // Phase 2.1: subtract the SAME entry notional we added at entry time.
        // Use entry_price_snap × qty_snap, NOT exit_price × qty_snap — the
        // latter would leak residue per round trip (positive when winning,
        // negative when losing) and drift the budget tracker unboundedly.
        // FPN_SubSat saturates at zero if state ever becomes inconsistent
        // (defensive against future bugs; should never trigger in practice).
        FPN<F> entry_notional_snap = FPN_Mul(entry_price_snap, qty_snap);
        ctx->core_open_notional = FPN_SubSat(ctx->core_open_notional, entry_notional_snap);
        // Phase 09: track peak balance for drawdown-based kill switch.
        // Cheap on the slow path; the comparison is one FPN compare per exit.
        if (FPN_GreaterThan(state->oms->balance, state->oms->ks_peak_balance)) {
            state->oms->ks_peak_balance = state->oms->balance;
        }
        ctx->exits_processed++;
        state->total_exits++;
        state->total_events_processed++;
        // CSV: pitfall P8.7 — log AFTER net/total_fee/balance are computed.
        if (state->oms->trade_log) {
            ShardedTradeLog_RecordExit(state->oms->trade_log, event,
                                       ctx->strategy_id,
                                       entry_price_snap,
                                       event.price,
                                       qty_snap,
                                       net,
                                       total_fee,
                                       state->oms->balance);
        }
        return;
    }
}

//======================================================================================================
// [DRAIN EVENTS]
//======================================================================================================
// round-robin across all registered cores. for each core, pop up to
// MAX_EVENTS_PER_DRAIN_PER_CORE events from its event ring and process each
// via _OnEvent. returns total events processed in this pass.
//
// the per-core cap prevents one chatty core from monopolizing a drain pass
// and starving the others (pitfall P4.1). under sustained burst load, the
// loop iterates fast enough that 16 events × 16 cores = 256 events per pass
// is plenty for realistic event rates (a busy strategy fires entries at
// most a few times per second).
//======================================================================================================
template <unsigned F>
inline int EventLoop_DrainEvents(EventLoopState<F>* state) {
    int total_drained = 0;
    for (int slot = 0; slot < state->registered_count; ++slot) {
        ExecutionCore<F>* core = state->cores[slot].core;
        if (core == nullptr) continue;

        for (int i = 0; i < MAX_EVENTS_PER_DRAIN_PER_CORE; ++i) {
            TradeEvent<F> event;
            if (!SPSCRing_TryPop(&core->event_ring, &event)) break;
            EventLoop_OnEvent(state, event);
            ++total_drained;
        }
    }
    return total_drained;
}

//======================================================================================================
// [QUEUE PARAMETERS — controller marks a core dirty]
//======================================================================================================
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
//======================================================================================================
template <unsigned F>
inline void EventLoop_QueueParameters(EventLoopState<F>* state, int slot,
                                       const GateParameters<F>& new_params) {
    if (slot < 0 || slot >= state->registered_count) return;
    state->cores[slot].pending_params = new_params;
    state->cores[slot].dirty = 1;
}

//======================================================================================================
// [REBUILD ALL PARAMETERS — slow-path entry point, phase 06+]
//======================================================================================================
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
// ~1µs per core (one Strategy_BuildParameters call) so 16 cores → ~16µs per
// rebuild. that's well inside the slow-path budget.
//
// returns the number of cores that had their pending_params rebuilt. cores
// with strategy_id == STRATEGY_NONE are skipped (they get a zeroed pack from
// the dispatcher but we don't bother marking them dirty since the execution
// core's permission stays at 0 anyway).
//
// pitfall P5.6 alignment: this is called from the slow path AFTER
// RollingStats_Push, so the rolling state is fresh.
//======================================================================================================
template <unsigned F, unsigned W = 128, unsigned WL = 512>
inline int EventLoop_RebuildAllParameters(
    EventLoopState<F>* state,
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    const RollingStats<F, WL>* rolling_long = nullptr,
    const void* ror_regressor = nullptr,    // const RORRegressor<F>*
    const void* ema_price     = nullptr,    // const FPN<F>*
    const void* current_price = nullptr,    // const FPN<F>* — Phase 3 MTM
    // v4.3 — expanded feature-pack state (optional)
    const void* rolling_medium   = nullptr,  // const RollingStats<F, 256>*
    const void* rolling_baseline = nullptr,  // const RollingStats<F, 1024>*
    const void* cumdelta_state   = nullptr,  // const CumDeltaState<F>*
    const void* tick_rate_state  = nullptr,  // const TickRateState*
    uint64_t timestamp_us = 0,
    // Track E.3 (2026-04-26) — depth-derived buy gate. Optional FPN<F>*
    // (passed as void* to keep the signature uniform with the other
    // optional pointers above and avoid template-parameter coupling). When
    // non-null AND cfg.min_book_imbalance > 0 AND *book_imbalance < min,
    // every core's pending_params gets GATE_FLAG_BUY_BLOCKED set after
    // Strategy_BuildParameters runs, vetoing entries until the imbalance
    // recovers. Caller passes from DepthSharedState (live) or
    // DepthReplayState (backtest) — symmetric across both paths.
    const void* book_imbalance = nullptr,     // const FPN<F>*
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
        const FPN<F>* bi = (const FPN<F>*)book_imbalance;
        book_imbalance_blocked = FPN_LessThan(*bi, config->min_book_imbalance) ? 1 : 0;
    }
    // v4.7.38 (Phase C.1): per-core loop body extracted to EventLoop_RebuildOneCore.
    // Both centralized path (this loop) and per-core slow-path (Phase C.2)
    // call OneCore with the same arguments — guarantees train-serve parity
    // by sharing the SAME function across all execution paths. STRATEGY_NONE
    // skip is the caller's responsibility (cheap conditional).
    for (int slot = 0; slot < state->registered_count; ++slot) {
        if (state->cores[slot].strategy_id == STRATEGY_NONE) continue;
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

//======================================================================================================
// [PER-CORE ROLLING STATE UPDATE — v5.1.2]
//======================================================================================================
// Pushes (price, volume, timestamp, depth) into ONE engine's slow_state.
// Single-writer rule: caller must be the sole writer of state.cores[c].slow_state
// for the duration of this call:
//   - centralized: producer iterates c=0..N
//   - per_core_slow: per-core slow-path c writes own only
//   - backtest: linear iteration, single-thread
//
// Mirrors the cadence-update logic that v5.0.x had in producer's fan_out
// (lines ~881-915 of EngineSharded.hpp pre-v5.1.2). Centralized in this
// helper so all 3 callers do exactly the same work — train-serve parity
// is structural.
//
// Inputs:
//   price, volume          — current tick (slow-path-cadence sample)
//   timestamp_us           — wall-clock us of this update
//   is_buyer_maker         — Binance flag (1 = aggressive sell, 0 = aggressive buy)
//   depth_imbalance        — book imbalance from depth WS / replay (FPN_Zero if depth disabled)
//   depth_spread           — spread (FPN_Zero if depth disabled)
//   depth_enabled          — gate the depth-history pushes
//======================================================================================================
template <unsigned F>
inline void EventLoop_UpdateRollingStateOneCore(
    EventLoopState<F>* state, int slot,
    FPN<F> price, FPN<F> volume, uint64_t timestamp_us,
    int is_buyer_maker,
    FPN<F> depth_imbalance, FPN<F> depth_spread,
    int depth_enabled) {
    if (slot < 0 || slot >= MAX_EXECUTION_CORES) return;
    auto* sst = state->cores[slot].slow_state;
    if (!sst) return;
    if (FPN_IsZero(price)) return;  // pre-warmup tick — skip pushes

    RollingStats_Push(&sst->rolling_short,    price, volume);
    RollingStats_Push(&sst->rolling_long,     price, volume);
    RollingStats_Push(&sst->rolling_medium,   price, volume);
    RollingStats_Push(&sst->rolling_baseline, price, volume);
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

// Multi-core variant. Loops c=0..N calling OneCore. Used by centralized
// arch and backtest. per_core_slow lambdas call OneCore directly.
template <unsigned F>
inline void EventLoop_UpdateRollingStateAllCores(
    EventLoopState<F>* state,
    FPN<F> price, FPN<F> volume, uint64_t timestamp_us,
    int is_buyer_maker,
    FPN<F> depth_imbalance, FPN<F> depth_spread,
    int depth_enabled) {
    for (int c = 0; c < state->registered_count; ++c) {
        EventLoop_UpdateRollingStateOneCore(state, c,
            price, volume, timestamp_us, is_buyer_maker,
            depth_imbalance, depth_spread, depth_enabled);
    }
}

// Per-tick replication of ema_price across all engines' slow_state.
// Producer thread is sole writer; loops cheaply (one FPN copy per engine).
template <unsigned F>
inline void EventLoop_UpdateEmaPriceAllCores(
    EventLoopState<F>* state, FPN<F> ema_price) {
    for (int c = 0; c < state->registered_count; ++c) {
        if (state->cores[c].slow_state) {
            state->cores[c].slow_state->ema_price = ema_price;
        }
    }
}

//======================================================================================================
// [PER-CORE REBUILD WRAPPER — v5.1.2]
//======================================================================================================
// Iterates RebuildOneCore for each c using THAT engine's slow_state as
// the read source. Replaces RebuildAllParameters in centralized arch +
// backtest. per_core_slow's lambda already calls OneCore directly with
// per-core pointers.
//======================================================================================================
template <unsigned F>
inline int EventLoop_RebuildAllParameters_PerCore(
    EventLoopState<F>* state,
    const ControllerConfig<F>* config,
    const FPN<F>* current_price,    // const FPN<F>* — Phase 3 MTM (nullptr if disabled)
    uint64_t timestamp_us,
    const FPN<F>* book_imbalance,   // nullptr if depth disabled
    double current_spread,
    double current_mid_price,
    uint64_t now_us = 0) {  // v5.12.1.B clock hoist; 0 = compute internally
    int rebuilt = 0;
    int book_imbalance_blocked = 0;
    if (book_imbalance && !FPN_IsZero(config->min_book_imbalance)) {
        book_imbalance_blocked = FPN_LessThan(*book_imbalance,
            config->min_book_imbalance) ? 1 : 0;
    }
    for (int slot = 0; slot < state->registered_count; ++slot) {
        if (state->cores[slot].strategy_id == STRATEGY_NONE) continue;
        const auto* sst = state->cores[slot].slow_state;
        if (!sst) continue;  // defensive (shouldn't happen post-Init)
        EventLoop_RebuildOneCore(
            state, slot,
            &sst->rolling_short, config, &sst->rolling_long,
            &sst->regime_ror, &sst->ema_price,
            current_price,
            &sst->rolling_medium, &sst->rolling_baseline,
            &sst->cumdelta_state, &sst->tick_rate_state,
            timestamp_us,
            book_imbalance,
            &sst->book_imb_history, &sst->flow_state,
            &sst->large_trade_state, &sst->spread_state,
            current_spread, current_mid_price, book_imbalance_blocked,
            now_us);  // v5.12.1.B clock hoist
        ++rebuilt;
    }
    return rebuilt;
}

// v4.7.38 (Phase C.1): single-core variant of RebuildAllParameters.
// Caller must precompute book_imbalance_blocked (cheap — one FPN compare)
// and skip cores with strategy_id == STRATEGY_NONE before calling.
//
// v5.12.1.B (clock hoist): optional `now_us` param at end. When non-zero,
// caller has already read system_clock at slow-path entry; recovery check
// uses it instead of doing its own clock_gettime. Default 0 = back-compat
// (legacy callers, tests). Saves ~50ns/cycle in flatten-recovery window
// when caller hoists. See CLAUDE.md item 16 (reuse-audit principle).
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
    // v5.12.2.B — lazy rebuild predicate. Evaluated at function entry so
    // we skip the heavy body when slow_state hasn't changed materially
    // since last rebuild. Three escape clauses force a full rebuild:
    //   (1) cfg.lazy_rebuild_enabled == 0 (default; preserves baseline)
    //   (2) caller didn't pass now_us (legacy + test path) — can't time-bound
    //   (3) sst is null OR last_rebuild bookkeeping is unset (warmup)
    // Otherwise check the time-bound + price-delta predicates.
    if (config->lazy_rebuild_enabled && now_us != 0
        && state && slot >= 0 && slot < MAX_EXECUTION_CORES) {
        auto* sst_lazy = state->cores[slot].slow_state;
        if (sst_lazy && sst_lazy->us_at_last_rebuild != 0
            && !FPN_IsZero(sst_lazy->price_at_last_rebuild)
            && rolling) {
            // Time-bound force: rebuild every force_period_us regardless.
            uint64_t age_us = (now_us > sst_lazy->us_at_last_rebuild)
                ? (now_us - sst_lazy->us_at_last_rebuild) : 0;
            int time_force = (age_us >= config->lazy_rebuild_force_period_us);
            // Price-delta force: rebuild when |Δprice| / last_price > threshold.
            FPN<F> price_now = rolling->price_avg;
            FPN<F> price_last = sst_lazy->price_at_last_rebuild;
            FPN<F> delta = FPN_Sub(price_now, price_last);
            // |delta| via direct sign-bit clear (FPN is sign-magnitude).
            FPN<F> abs_delta = delta;
            abs_delta.sign = 0;
            FPN<F> rel_delta = FPN_DivNoAssert(abs_delta, price_last);
            int price_force = FPN_GreaterThan(rel_delta,
                config->lazy_rebuild_price_threshold_pct);
            if (!time_force && !price_force) {
                // Lazy-skip path. Mark dirty=1 so PushParameters publishes
                // pending_params with fresh publish_tick (v5.12.1.B
                // staleness gate stays satisfied). pending_params payload
                // is unchanged from the last full rebuild — the republish
                // is purely for tick freshness.
                state->cores[slot].dirty = 1;
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
        // v4.0.3 D6: session-aware volume multiplier. Each session has its
        // own typical volume profile — cfg can require lower volume during
        // Asian session (when BTC is quieter) than US session (when busier).
        // Mirrors legacy PortfolioController. Time-of-day from system clock —
        // assumes engine clock is synced (it should be for live trading).
        time_t now = time(nullptr);
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        FPN<F> session_mult = FPN_FromDouble<F>(1.0);
        int hour = tm_utc.tm_hour;
        if (hour >= 0 && hour < 7)         session_mult = resolved_cfg.session_asian_mult;
        else if (hour >= 7 && hour < 13)   session_mult = resolved_cfg.session_european_mult;
        else if (hour >= 13 && hour < 20)  session_mult = resolved_cfg.session_us_mult;
        else                                session_mult = resolved_cfg.session_overnight_mult;
        if (!FPN_IsZero(session_mult)) {
            resolved_cfg.volume_multiplier =
                FPN_Mul(resolved_cfg.volume_multiplier, session_mult);
        }

        // v4.2.1: idle-cycle counter. Bump every rebuild; reset to 0 in
        // OnEvent's entry branch on every fill. When the threshold is
        // exceeded the pnl_feeder ring buffer is reset so adaptive
        // feedback (D10) doesn't keep applying shifts based on stale
        // outcomes from before a long quiet period. Mirrors the recovery
        // intent of legacy `idle_reset_cycles` without the filter-decay
        // step (sharded recomputes resolved_cfg fresh each rebuild, so
        // there's no live-filter drift to undo).
        state->cores[slot].idle_cycles++;
        // v5.9.1 — boot-time per-core warmup-complete log (V5_9_AUDIT-#9).
        // Fires once per session per core, on the rebuild cycle that first
        // observes rolling.count >= min_warmup_samples. Distinct from the
        // global startup gate that releases all cores from CONTROLLER_WARMUP
        // simultaneously — operator wants per-core readiness because in
        // per_core_slow arch each core's slow path runs at its own cadence.
        if (!state->cores[slot].warmup_log_emitted) {
            int wmin = (int)config->min_warmup_samples;
            if (wmin <= 0) wmin = 64;  // engine default (matches ShardedSnapshot fallback)
            if (rolling->count >= wmin) {
                // v5.9.4a — name the strategy so operator can distinguish ML
                // vs non-ML cores in mixed deployments. Bounds-checked via
                // static_assert on STRATEGY_SHORT_NAMES at the X-macro
                // declaration (StrategyInterface.hpp:151).
                int sid = state->cores[slot].strategy_id;
                const char* sname = (sid >= 0 && sid < NUM_STRATEGIES)
                                  ? STRATEGY_SHORT_NAMES[sid] : "unknown";
                fprintf(stderr, "[core %d] warmup complete (%d/%d samples) — %s active\n",
                        slot, rolling->count, wmin, sname);
                state->cores[slot].warmup_log_emitted = 1;
            }
        }
        if (config->idle_reset_cycles > 0 &&
            state->cores[slot].idle_cycles >= config->idle_reset_cycles) {
            state->cores[slot].pnl_feeder.head  = 0;
            state->cores[slot].pnl_feeder.count = 0;
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
        if (state->cores[slot].pnl_feeder.count >= 4 &&
            !FPN_IsZero(resolved_cfg.filter_scale)) {
            LinearRegression3XResult<F> reg =
                RegressionFeederX_Compute(&state->cores[slot].pnl_feeder);
            FPN<F> slope = reg.model.slope;
            // Only apply if R² is meaningful (otherwise slope is noise).
            if (FPN_GreaterThan(reg.r_squared, FPN_FromDouble<F>(0.20))) {
                // shift = -slope × filter_scale  (negative slope → positive shift = tighter)
                FPN<F> shift = FPN_Mul(slope, resolved_cfg.filter_scale);
                shift.sign = !shift.sign;  // negate
                // Apply to entry_offset_pct, clamped to [offset_min, offset_max]
                FPN<F> new_offset = FPN_Add(resolved_cfg.entry_offset_pct, shift);
                if (FPN_LessThan(new_offset, resolved_cfg.offset_min))
                    new_offset = resolved_cfg.offset_min;
                if (FPN_GreaterThan(new_offset, resolved_cfg.offset_max))
                    new_offset = resolved_cfg.offset_max;
                resolved_cfg.entry_offset_pct = new_offset;
                // Apply to volume_multiplier same direction (tighter when losing)
                FPN<F> new_vmult = FPN_Add(resolved_cfg.volume_multiplier, shift);
                if (FPN_LessThan(new_vmult, resolved_cfg.vol_mult_min))
                    new_vmult = resolved_cfg.vol_mult_min;
                if (FPN_GreaterThan(new_vmult, resolved_cfg.vol_mult_max))
                    new_vmult = resolved_cfg.vol_mult_max;
                resolved_cfg.volume_multiplier = new_vmult;
            }
        }

        // v4.0.3 B: STRATEGY_AUTO regime mode. The core's nominal strategy_id
        // is AUTO; we compute the current regime from the same RegimeSignals
        // the ML pack uses, classify with hysteresis, and resolve to a
        // concrete strategy via REGIME_STRATEGY_TABLE. The dispatcher never
        // sees STRATEGY_AUTO — it sees the resolved id (MR/MOM/DIP/EMA).
        // Each AUTO core tracks its own regime_state independently.
        uint8_t effective_strategy_id = state->cores[slot].strategy_id;
        if (effective_strategy_id == STRATEGY_AUTO &&
            ror_regressor && ema_price && rolling_long) {
            const RORRegressor<F>* ror_in = (const RORRegressor<F>*)ror_regressor;
            const FPN<F>* ema_in          = (const FPN<F>*)ema_price;
            RegimeSignals<F> sig;
            // v4.3 — pass expanded state so AUTO regime classification sees
            // the same features the ML core will see (consistency for the
            // few signals Regime_Classify reads beyond just slope/R²).
            Regime_ComputeSignals(&sig, rolling, rolling_long, ror_in, *ema_in,
                                   (const RollingStats<F, 256>*)rolling_medium,
                                   (const RollingStats<F, 1024>*)rolling_baseline,
                                   (const CumDeltaState<F>*)cumdelta_state,
                                   (const TickRateState*)tick_rate_state,
                                   timestamp_us,
                                   book_imb_history, flow_state, large_trade_state,
                                   spread_state, current_spread, current_mid_price);
            int old_regime = state->cores[slot].regime_state.current_regime;
            int new_regime = Regime_Classify(&state->cores[slot].regime_state,
                                              &sig, &resolved_cfg);
            int resolved = Regime_ToStrategy(state->cores[slot].regime_state.current_regime);
            // v5.4.0 Phase 0.1 — log AUTO-core regime classification per cadence.
            // Cheap (cfg-gated, no-op when disabled). Captures regime transitions
            // and the inputs that drove them — runtime visibility into the
            // "regime stuck on RANGING" symptom (F3).
            if (tt::Health_LogEnabled(tt::HEALTH_INFO)) {
                tt::Health_Log(tt::HEALTH_INFO, "regime", slot,
                    "old=%d new=%d resolved_strat=%d ema_sma_spread=%g "
                    "short_r2=%g ror_slope=%g hyst=%d/%d short_count=%d",
                    old_regime, new_regime, resolved,
                    FPN_ToDouble(sig.ema_sma_spread),
                    FPN_ToDouble(sig.short_r2),
                    FPN_ToDouble(sig.ror_slope),
                    state->cores[slot].regime_state.hysteresis_count,
                    state->cores[slot].regime_state.hysteresis_threshold,
                    sig.short_count);
            }
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
                 Sharded_CoreSlotMask(slot, config->partial_exit_enabled))) {
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
                FPN<F> tight_sl = FPN_Sub(rolling->price_avg,
                                           rolling->price_stddev);
                int partial_on = config->partial_exit_enabled ? 1 : 0;
                uint16_t my_mask = partial_on
                    ? (uint16_t)((1u << (slot * 2)) | (1u << (slot * 2 + 1)))
                    : (uint16_t)(1u << slot);
                uint16_t bm = (uint16_t)(state->oms->portfolio.active_bitmap & my_mask);
                while (bm) {
                    int pidx = __builtin_ctz(bm);
                    bm &= (uint16_t)(bm - 1);
                    FPN<F> entry_p = state->oms->portfolio.positions[pidx].entry_price;
                    if (!FPN_IsZero(entry_p)) {
                        Strategy_WriteRatchetSL(state, slot, tight_sl,
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
                    FPN<F> tp_offset = FPN_Mul(rolling->price_stddev,
                                                resolved_cfg.momentum_tp_mult);
                    FPN<F> wide_tp   = FPN_Add(rolling->price_avg, tp_offset);
                    Strategy_WriteRatchetTP(state, slot, wide_tp);
                } else if (tighten) {
                    // Tighter TP target: lock in profit by ratcheting TP up
                    // to current_price + small offset. Only advances if
                    // higher than existing ratchet_tp (max-only semantics).
                    FPN<F> tight_offset = FPN_Mul(rolling->price_stddev,
                                                   FPN_FromDouble<F>(0.5));
                    FPN<F> tight_tp     = FPN_Add(rolling->price_avg, tight_offset);
                    Strategy_WriteRatchetTP(state, slot, tight_tp);
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
            ml_ctx.model_handle   = state->cores[slot].model_handle;
            ml_ctx.ensemble_zoo   = state->cores[slot].ensemble_handle;  // v5.10.0a.G.5 — nullptr-safe; single-zoo when null
            ml_ctx.current_regime_id = state->cores[slot].regime_state.current_regime;  // v5.10.0a.G.7
            ml_ctx.confidence     = &state->cores[slot].confidence;
            ml_ctx.out_prediction = &state->cores[slot].staged_prediction;
            ml_ctx.out_confidence = &state->cores[slot].last_confidence;
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
            ml_ctx.model_load_failed           = &state->cores[slot].model_load_failed;
            ml_ctx.last_ml_critical_log_us     = &state->cores[slot].last_ml_critical_log_us;
            ml_ctx.out_threshold               = &state->cores[slot].last_ml_threshold;
            ml_ctx.out_effective_threshold     = &state->cores[slot].last_ml_effective_threshold;
            ml_ctx.nan_feature_events_total    = &state->cores[slot].nan_feature_events_total;
            ml_ctx.nan_prediction_events_total = &state->cores[slot].nan_prediction_events_total;
            // v5.9.1 — wire SHALT pointer through MLBuildContext so the
            // confidence hard-block path can attribute SHALT_LOW_CONFIDENCE.
            // Same address the dispatcher passes via its strategy_halt_reason
            // parameter; ml_ctx is the only path ML_BuildParameters has into
            // it without changing the dispatcher signature.
            ml_ctx.out_strategy_halt_reason    = &state->cores[slot].strategy_halt_reason;
            // v5.11.18 main — per-core feature mask. Pointer to the cfg
            // field directly; no copy. Features_PackAll inside
            // ML_BuildParameters reads through this pointer when non-null.
            // When operator hasn't set core_<slot>_feature_mask in cfg,
            // the cfg parser at v5.11.18a defaults to 0xFFFF..F (all
            // features enabled); pointer is non-null but mask = all-on
            // produces bytewise-identical output to pre-v5.11.18.
            ml_ctx.feature_mask = &resolved_cfg.core_feature_mask[slot];
            // v5.13.0.B — sell-side ML wiring. Reset per-cycle; ML_Build-
            // Parameters writes the blended exit_predictor probability when
            // cfg.use_exit_model && exit_predictor_count > 0. Slow-path body
            // post-RebuildOneCore reads + acts on the value (fires OMS submit
            // if above cfg.exit_threshold and any positions are open).
            state->cores[slot].last_exit_prediction       = 0.0;
            state->cores[slot].last_exit_dominant_horizon = -1;
            ml_ctx.out_exit_prediction       = &state->cores[slot].last_exit_prediction;
            ml_ctx.out_exit_dominant_horizon = &state->cores[slot].last_exit_dominant_horizon;
            dispatch_ctx = &ml_ctx;
        }
        // v4.0.4: stash the resolved strategy for GUI display. For non-AUTO
        // cores this just mirrors strategy_id; for AUTO it's the regime-
        // resolved concrete strategy.
        state->cores[slot].resolved_strategy_id = effective_strategy_id;

        // v5.4.0 Phase 2.1 — Adapt before BuildParameters. Per the strategy
        // contract (StrategyInterface.hpp), Adapt mutates per-core state and
        // BuildParameters reads it. No-op when state is null (AUTO cores
        // pre-Phase 3, NONE cores). portfolio_delta passed as zero on the
        // sharded slow path — pre-v5.4 strategies that consumed it (legacy
        // path) are out of band; sharded uses pnl_feeder for the
        // rebuild-time feedback loop (line ~1537 above).
        // v5.4.0 Phase 2.4 — also pass ema_price (per-tick replicated by
        // producer) so EmaCross's Adapt branch can update its prev_ema +
        // last_ema_slope tracking.
        Strategy_AdaptPerCore(
            state, slot, effective_strategy_id,
            rolling->price_avg,         // current_price proxy (slow-path doesn't see live tick)
            FPN_Zero<F>(),              // portfolio_delta — fed via pnl_feeder above, not here
            state->oms->portfolio.active_bitmap,
            &resolved_cfg,
            (const FPN<F>*)ema_price    // v5.4.0 Phase 2.4 — for EmaCross
        );

        Strategy_BuildParameters(
            effective_strategy_id,
            rolling,
            &resolved_cfg,
            state->cores[slot].allocated_balance,
            &state->cores[slot].pending_params,
            rolling_long,
            dispatch_ctx,
            state->cores[slot].strategy_state,   // v5.4.0 Phase 2.1 — typed-cast inside dispatcher
            &state->cores[slot].strategy_halt_reason,  // v5.6.2 — dispatcher writes
                                                       // SHALT_* codes for fee-floor /
                                                       // cost-gate / no-signal paths.
            now_us  // v5.14.1.B.2 (PARITY-001) — threaded through to ML_BuildParameters
                    // for composite confidence freshness. Already plumbed to this fn
                    // (param :1951) since v5.12.1.B clock hoist; live = clock_gettime
                    // at slow-path entry, backtest = tick.timestamp (deterministic).
        );

        // v4.0.3 D9: clear ratchet_sl when no position active on this core,
        // so stale trailing state from previous trade doesn't leak into the
        // next entry. Engine slow-path code below SETS ratchet_sl when a
        // position is active and trailing should kick in.
        // Partials-aware: core's portfolio slot(s) come from the helper
        // (slot N or 2N+0/2N+1 depending on partial_exit_enabled).
        bool slot_active = (state->oms->portfolio.active_bitmap &
                             Sharded_CoreSlotMask(slot, config->partial_exit_enabled)) != 0;
        if (!slot_active) {
            state->cores[slot].pending_params.ratchet_sl = FPN_Zero<F>();
        }

        // v5.4.0 Phase 2.2: strategy-specific exit-adjust for cores with
        // open slots. Writes ratchet_sl via Strategy_WriteRatchetSL (fee-floor
        // capped). Coexists with the generic EventLoop_TrailingSLRatchetOneCore
        // — both write the same field with max-only semantics; the higher
        // proposal wins.
        if (slot_active) {
            Strategy_ExitAdjustPerCore(state, slot, effective_strategy_id,
                                        rolling->price_avg, rolling, &resolved_cfg);
        }

        // v4.0.3 cross-cutting checks applied uniformly across all strategies.
        // Each is a "zero-gate if violated" filter — preserves the strategy's
        // intended TP/SL/qty but disables the entry trigger. Halt reasons are
        // tracked per-core for GUI display.
        //
        // v5.8.3: halt_reason is now a HALT_* enum from FOREACH_HALT_REASON
        // (StrategyInterface.hpp). See registry there for code semantics.
        state->cores[slot].halt_reason = HALT_OK;
        // v5.6.2: reset strategy_halt_reason every rebuild. Strategies
        // set this to a SHALT_* code when zero-gating for strategy-
        // internal reasons. SHALT_OK = no veto.
        state->cores[slot].strategy_halt_reason = SHALT_OK;

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
        if (config->param_staleness_gate_enabled) {
            state->cores[slot].pending_params.flags
                |= GATE_FLAG_STALENESS_ENABLED;
        }
        state->cores[slot].pending_params.param_max_age_ticks
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
                    state->cores[slot].pending_params.flags
                        |= GATE_FLAG_BUY_BLOCKED;
                    state->cores[slot].strategy_halt_reason = SHALT_RECOVERY;
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
            state->cores[slot].pending_params.bg_price_threshold = FPN_Zero<F>();
            state->cores[slot].pending_params.flags |= GATE_FLAG_BUY_BLOCKED;
            if (state->cores[slot].halt_reason == HALT_OK)  // first reason wins
                state->cores[slot].halt_reason = reason;
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
            state->cores[slot].pending_params.flags |= GATE_FLAG_BUY_BLOCKED;
            if (state->cores[slot].halt_reason == HALT_OK)
                state->cores[slot].halt_reason = HALT_IMBALANCE;
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
        // All FPN-pure, slow path. NOTE: FPN<F> is signed — we explicitly
        // compare open_notional >= allocated (rather than relying on
        // FPN_SubSat saturating to zero on underflow, which it doesn't).
        {
            FPN<F> alloc       = state->cores[slot].allocated_balance;
            FPN<F> open_n      = state->cores[slot].core_open_notional;
            FPN<F> entry_price = state->cores[slot].pending_params.bg_price_threshold;
            if (FPN_GreaterThanOrEqual(open_n, alloc)) {
                // Fully or over-deployed — no slot-room for another entry.
                // Zero-gate with HALT_CORE_BUDGET. Trade size also clamped
                // to zero so any downstream consumer of trade_size sees
                // an honest zero rather than a stale value.
                state->cores[slot].pending_params.trade_size = FPN_Zero<F>();
                zero_gate(HALT_CORE_BUDGET);
            } else if (!FPN_IsZero(entry_price)) {
                // Budget remaining is positive — clamp qty to
                // (budget_remaining / entry_price). Under single-position-
                // per-core today, open_n is 0 when this branch runs (we hit
                // the GE branch above when deployed), so budget_remaining
                // == alloc and the clamp is a no-op. Multi-position-per-core
                // would land here with partial budget, producing a real clamp.
                FPN<F> budget_remaining = FPN_Sub(alloc, open_n);  // > 0 by branch
                FPN<F> max_qty = FPN_DivNoAssert(budget_remaining, entry_price);
                state->cores[slot].pending_params.trade_size =
                    FPN_Min(state->cores[slot].pending_params.trade_size, max_qty);
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
            FPN<F> alloc     = state->cores[slot].allocated_balance;
            FPN<F> realized  = state->cores[slot].core_realized;
            FPN<F> unrealized = FPN_Zero<F>();
            const FPN<F>* px_in = (const FPN<F>*)current_price;
            // Partials-aware MTM walk: under partials, core c's positions
            // live in slots 2c and 2c+1 (one Position per leg, each with
            // independent qty). Sum unrealized across both. Without
            // partials, only slot c is walked.
            if (config->enable_mtm_kill_switch && px_in && !FPN_IsZero(*px_in)) {
                uint16_t mask = Sharded_CoreSlotMask(slot, config->partial_exit_enabled);
                uint16_t bm   = state->oms->portfolio.active_bitmap & mask;
                while (bm) {
                    int s = __builtin_ctz(bm);
                    bm &= (uint16_t)(bm - 1);
                    Position<F>& pos = state->oms->portfolio.positions[s];
                    FPN<F> diff = FPN_Sub(*px_in, pos.entry_price);
                    unrealized = FPN_Add(unrealized, FPN_Mul(diff, pos.quantity));
                }
            }
            FPN<F> current_value = FPN_Add(alloc, FPN_Add(realized, unrealized));
            // Peak ratchet (branchless via FPN_Max). Initialize to alloc on
            // first sight if peak is still zero (first rebuild after init).
            if (FPN_IsZero(state->cores[slot].core_peak_balance)) {
                state->cores[slot].core_peak_balance = alloc;
            }
            state->cores[slot].core_peak_balance =
                FPN_Max(state->cores[slot].core_peak_balance, current_value);
            // Drawdown computation. Skip if peak is zero (defensive — should
            // never happen after the init bump above, but handles a freshly
            // reset state). dd = (peak - current) / peak.
            FPN<F> drop = FPN_Sub(state->cores[slot].core_peak_balance, current_value);
            if (FPN_GreaterThan(drop, FPN_Zero<F>()) &&
                FPN_GreaterThan(state->cores[slot].core_peak_balance, FPN_Zero<F>())) {
                state->cores[slot].core_dd_pct = FPN_DivNoAssert(drop,
                    state->cores[slot].core_peak_balance);
            } else {
                state->cores[slot].core_dd_pct = FPN_Zero<F>();
            }
            // Trip evaluation. Threshold: per-core override if set, else
            // global max_drawdown_pct. Trip ALSO requires drop > min_kill_loss
            // so a tiny allocation doesn't trip on rounding noise.
            if (state->cores[slot].core_kill_tripped == 0) {
                FPN<F> threshold = !FPN_IsZero(config->core_max_drawdown_pct[slot])
                    ? config->core_max_drawdown_pct[slot]
                    : config->max_drawdown_pct;
                if (FPN_GreaterThan(state->cores[slot].core_dd_pct, threshold) &&
                    FPN_GreaterThan(drop, config->min_kill_loss)) {
                    state->cores[slot].core_kill_tripped   = 1;
                    state->cores[slot].core_ks_trips_total++;
                    double dd_pct_d  = FPN_ToDouble(state->cores[slot].core_dd_pct) * 100.0;
                    double drop_d    = FPN_ToDouble(drop);
                    double peak_d    = FPN_ToDouble(state->cores[slot].core_peak_balance);
                    double current_d = FPN_ToDouble(current_value);
                    fprintf(stderr, "[sharded] CORE KILL: core %d tripped — "
                            "dd=%.2f%% drop=$%.2f peak=$%.2f current=$%.2f\n",
                            slot, dd_pct_d, drop_d, peak_d, current_d);
                    // 2A — alert via Notify subsystem alongside stderr log.
                    // Per-kind cooldown (NK_CORE_KILL_TRIP=10) collapses
                    // back-to-back trips on the same core to one alert per
                    // window. Backtest leaves g_notify null → no-op.
                    if (g_notify) {
                        char body[256];
                        snprintf(body, sizeof(body),
                                 "Core %d kill tripped: dd=%.2f%% drop=$%.2f "
                                 "peak=$%.2f current=$%.2f. Entries halted on "
                                 "this core until manual reset.",
                                 slot, dd_pct_d, drop_d, peak_d, current_d);
                        Notify_Send(g_notify, NOTIFY_ALERT, NK_CORE_KILL_TRIP,
                                    "Per-core kill switch tripped", body);
                    }
                }
            }
            if (state->cores[slot].core_kill_tripped) {
                zero_gate(HALT_CORE_KILL);
            }
        }

        // SL COOLDOWN: decrement counter; if still active, zero-gate.
        if (state->cores[slot].sl_cooldown_remaining > 0) {
            state->cores[slot].sl_cooldown_remaining--;
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
            FPN<F> ratio_thresh = FPN_Mul(rolling->volume_max,
                FPN_DivNoAssert(FPN_FromDouble<F>(1.0), resolved_cfg.spike_threshold));
            // Note: spike active when current_volume × spike_threshold >= max.
            // Equivalent: current >= max / spike_threshold.
            // We check the latest volume_avg as the "current" representative.
            if (FPN_GreaterThanOrEqual(rolling->volume_avg, ratio_thresh)) {
                spacing_cfg.spacing_multiplier = FPN_Mul(
                    resolved_cfg.spacing_multiplier,
                    resolved_cfg.spike_spacing_reduction);
            }
        }
        // v5.6.3: capture spacing comparands for GUI diagnostic readout.
        // Single-source rule — these are the SAME values
        // Strategy_SpacingOk reads internally, exposed for display.
        {
            FPN<F> a = state->cores[slot].pending_params.bg_price_threshold;
            FPN<F> b = state->cores[slot].last_entry_price;
            FPN<F> abs_dist = FPN_GreaterThanOrEqual(a, b)
                ? FPN_Sub(a, b) : FPN_Sub(b, a);
            FPN<F> min_dist = FPN_Mul(rolling->price_stddev,
                                       spacing_cfg.spacing_multiplier);
            state->cores[slot].diag_spacing_actual = abs_dist;
            state->cores[slot].diag_spacing_floor  = min_dist;
        }
        if (!Strategy_SpacingOk(state->cores[slot].pending_params.bg_price_threshold,
                                 state->cores[slot].last_entry_price,
                                 rolling, &spacing_cfg)) {
            zero_gate(HALT_SPACING);
        }
        // VWAP gate: forces entries below VWAP — buy retracements, not pumps.
        if (!FPN_IsZero(resolved_cfg.vwap_offset) && !FPN_IsZero(rolling->vwap)) {
            FPN<F> vwap_threshold = FPN_Sub(rolling->vwap,
                FPN_Mul(rolling->vwap, resolved_cfg.vwap_offset));
            // v5.6.3: capture both sides for GUI.
            state->cores[slot].diag_vwap_actual    =
                state->cores[slot].pending_params.bg_price_threshold;
            state->cores[slot].diag_vwap_threshold = vwap_threshold;
            if (FPN_GreaterThan(state->cores[slot].pending_params.bg_price_threshold,
                                 vwap_threshold)) {
                zero_gate(HALT_VWAP);
            }
        }
        // LONG-SLOPE gate: blocks buys in confirmed downtrends.
        if (!FPN_IsZero(resolved_cfg.min_long_slope) && rolling_long &&
            !FPN_IsZero(rolling_long->price_avg)) {
            FPN<F> long_rel_slope = FPN_DivNoAssert(rolling_long->price_slope,
                                                     rolling_long->price_avg);
            // v5.6.3: capture for GUI.
            state->cores[slot].diag_long_slope     = long_rel_slope;
            state->cores[slot].diag_long_slope_min = resolved_cfg.min_long_slope;
            if (FPN_LessThan(long_rel_slope, resolved_cfg.min_long_slope)) {
                zero_gate(HALT_LONG_SLOPE);
            }
        }
        // VOLUME DELTA gate: blocks heavy dumps.
        if (!FPN_IsZero(resolved_cfg.min_buy_delta)) {
            // v5.6.3: capture both sides regardless of pass/fail so the
            // GUI shows current state (display invariant: always show
            // when cfg enabled).
            state->cores[slot].diag_volume_delta     = rolling->volume_delta;
            state->cores[slot].diag_volume_delta_min = resolved_cfg.min_buy_delta;
            if (FPN_LessThan(rolling->volume_delta, resolved_cfg.min_buy_delta)) {
                zero_gate(HALT_VOL_DELTA);
            }
        }
        // MIN STDDEV gate: skip dead markets.
        if (!FPN_IsZero(resolved_cfg.min_stddev_pct) && !FPN_IsZero(rolling->price_avg)) {
            FPN<F> stddev_ratio = FPN_DivNoAssert(rolling->price_stddev,
                                                    rolling->price_avg);
            // v5.6.3: capture for GUI.
            state->cores[slot].diag_stddev_pct     = stddev_ratio;
            state->cores[slot].diag_stddev_pct_min = resolved_cfg.min_stddev_pct;
            if (FPN_LessThan(stddev_ratio, resolved_cfg.min_stddev_pct)) {
                zero_gate(HALT_MIN_STDDEV);
            }
        }
        // v5.6.3: capture tp_pct + fee floor for GUI. Same formula as
        // the dispatcher's fee-floor BUY_BLOCKED path
        // (StrategyParameters.hpp:875-895). Capture here so the
        // collapsing-header readout shows actual vs floor regardless
        // of whether BUY_BLOCKED fired.
        {
            FPN<F> fee_taker = !FPN_IsZero(resolved_cfg.fee_rate_taker)
                ? resolved_cfg.fee_rate_taker : resolved_cfg.fee_rate;
            state->cores[slot].diag_tp_pct_actual =
                state->cores[slot].pending_params.tp_pct;
            state->cores[slot].diag_tp_pct_floor =
                FPN_Mul(fee_taker, FPN_FromDouble<F>(3.0));
        }
        // FEE FLOOR: ratchet TP up so it clears at least
        // entry × fee_rate × fee_floor_mult. Round-trip fees are 2×fee_rate,
        // so fee_floor_mult=5 means TP must clear ~2.5× round-trip fees + margin.
        // No-op when bg_price_threshold is zero (no entry), or
        // fee_floor_mult/fee_rate is zero.
        if (!FPN_IsZero(state->cores[slot].pending_params.bg_price_threshold)) {
            FPN<F> entry = state->cores[slot].pending_params.bg_price_threshold;
            FPN<F> current_tp = state->cores[slot].pending_params.sg_take_profit_price;
            // tp_amount = current_tp - entry; if it's negative that's already broken
            // (means strategy set TP below entry — leave alone, it's strategy's bug).
            if (FPN_GreaterThan(current_tp, entry)) {
                FPN<F> tp_amount = FPN_Sub(current_tp, entry);
                FPN<F> floored = Strategy_TpFloor(entry, tp_amount, &resolved_cfg);
                if (FPN_GreaterThan(floored, tp_amount)) {
                    state->cores[slot].pending_params.sg_take_profit_price =
                        FPN_Add(entry, floored);
                }
            }
        }
        // Mirror the pack's TP/SL/qty into the controller-side intended_*
        // fields so OnEvent uses the freshly computed values when the next
        // entry fires. This ties the strategy output to the entry handler.
        state->cores[slot].intended_tp  = state->cores[slot].pending_params.sg_take_profit_price;
        state->cores[slot].intended_sl  = state->cores[slot].pending_params.sg_stop_loss_price;
        state->cores[slot].intended_qty = state->cores[slot].pending_params.trade_size;
        state->cores[slot].dirty = 1;

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
            uint8_t  hr   = state->cores[slot].halt_reason          & 0x0F;
            uint8_t  shr  = state->cores[slot].strategy_halt_reason & 0x0F;
            uint8_t  bb   = (state->cores[slot].pending_params.flags
                              & GATE_FLAG_BUY_BLOCKED) ? 1 : 0;
            uint8_t  perm = state->cores[slot].core
                ? __atomic_load_n(&state->cores[slot].core->permission,
                                   __ATOMIC_ACQUIRE)
                : 0;
            uint16_t packed = (uint16_t)(hr | (shr << 4)
                                          | (bb   << 8) | (perm << 9));
            if (packed != state->cores[slot].prev_gate_log_state) {
                tt::Health_Log(tt::HEALTH_INFO, "gate", slot,
                    "halt=%u shalt=%u blocked=%u perm=%u "
                    "gate=%g price=%g",
                    (unsigned)hr, (unsigned)shr,
                    (unsigned)bb, (unsigned)perm,
                    FPN_ToDouble(state->cores[slot].pending_params.bg_price_threshold),
                    FPN_ToDouble(rolling->price_avg));
                state->cores[slot].prev_gate_log_state = packed;
            }
        }

        // v5.12.2.B — record the full-rebuild bookkeeping so the next
        // call's lazy predicate can compare against it. Only reaches here
        // when we DIDN'T take the lazy-skip path (= a real rebuild ran).
        if (state && slot >= 0 && slot < MAX_EXECUTION_CORES) {
            auto* sst_lazy = state->cores[slot].slow_state;
            if (sst_lazy && rolling) {
                sst_lazy->us_at_last_rebuild = now_us;
                sst_lazy->price_at_last_rebuild = rolling->price_avg;
            }
        }
    }
}

//======================================================================================================
// [PARAMETER PUSH — atomic, slow-path]
//======================================================================================================
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
//======================================================================================================
template <unsigned F>
inline int EventLoop_PushParameters(EventLoopState<F>* state,
                                     uint64_t publish_tick = 0) {
    // v5.12.1.B.2 — publish_tick threads through to ParameterSlot_Write so
    // hot-path's staleness gate can detect stale slow-path. Default 0 =
    // back-compat for legacy + test callers (warmup sentinel; gate inert).
    int pushed = 0;
    for (int slot = 0; slot < state->registered_count; ++slot) {
        if (state->cores[slot].dirty == 0) continue;
        ExecutionCore<F>* core = state->cores[slot].core;
        if (core == nullptr) {
            state->cores[slot].dirty = 0;
            continue;
        }
        ExecutionCore_SetParameters(core, state->cores[slot].pending_params,
                                     publish_tick);
        state->cores[slot].dirty = 0;
        ++pushed;
    }
    return pushed;
}

//======================================================================================================
// [KILL SWITCH — phase 09]
//======================================================================================================
// configure the kill switch thresholds. pass FPN_Zero for either parameter to
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
                                                FPN<F> min_balance,
                                                FPN<F> max_drawdown_pct) {
    state->oms->ks_min_balance      = min_balance;
    state->oms->ks_max_drawdown_pct = max_drawdown_pct;
}

// helper: clear permission on every registered core. used by both
// _KillSwitchEvaluate (when a condition fires) and _KillSwitchTrip (manual).
template <unsigned F>
inline void EventLoop_ClearAllPermissions(EventLoopState<F>* state) {
    for (int slot = 0; slot < state->registered_count; ++slot) {
        ExecutionCore<F>* core = state->cores[slot].core;
        if (core) ExecutionCore_SetPermission(core, 0);
    }
}

// manual trip — used by external monitors (e.g. orphan recovery, operator
// command) that need to halt trading without going through the threshold check.
// idempotent: trips_total only bumps on a state transition.
template <unsigned F>
inline void EventLoop_KillSwitchTrip(EventLoopState<F>* state) {
    if (state->oms->kill_switch_tripped == 0) {
        state->oms->kill_switch_tripped = 1;
        state->oms->ks_trips_total++;
    }
    EventLoop_ClearAllPermissions(state);
}

//======================================================================================================
// [KILL SWITCH EVALUATE]
//======================================================================================================
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
    if (state->oms->kill_switch_tripped) return 0;  // already tripped, no double-action

    int trip = 0;

    // condition 1: hard balance floor
    if (!FPN_IsZero(state->oms->ks_min_balance) &&
        FPN_LessThan(state->oms->balance, state->oms->ks_min_balance)) {
        trip = 1;
    }

    // condition 2: drawdown from peak
    if (!FPN_IsZero(state->oms->ks_max_drawdown_pct) &&
        !FPN_IsZero(state->oms->ks_peak_balance)) {
        FPN<F> drop = FPN_Sub(state->oms->ks_peak_balance, state->oms->balance);
        // only consider positive drops (balance below peak)
        if (FPN_GreaterThan(drop, FPN_Zero<F>())) {
            // drawdown = drop / peak. NoAssert variant: peak is non-zero per
            // the guard above so this can't trip the production assert path.
            FPN<F> dd = FPN_DivNoAssert(drop, state->oms->ks_peak_balance);
            if (FPN_GreaterThan(dd, state->oms->ks_max_drawdown_pct)) trip = 1;
        }
    }

    if (!trip) return 0;

    // Trip: clear permission on every core. This is the primary action.
    state->oms->kill_switch_tripped = 1;
    state->oms->ks_trips_total++;
    EventLoop_ClearAllPermissions(state);
    return 1;
}

//======================================================================================================
// [UNPAUSE — restore permission on cores with assigned strategies]
//======================================================================================================
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

//======================================================================================================
// [TIME-EXIT] (v4.7.17 — extracted from EngineSharded for backtest parity)
//======================================================================================================
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
//======================================================================================================
// v4.7.38 (Phase C.1): per-core helper, processes only this core's slots.
// Caller-checked preconditions (cfg.max_hold_ticks > 0 && current_price > 0).
// Wrapper EventLoop_TimeExit checks once + iterates.
template <unsigned F>
inline void EventLoop_TimeExitOneCore(EventLoopState<F>* state,
                                       OrderManagerState<F>* oms,
                                       const ControllerConfig<F>& cfg,
                                       uint64_t now_tick,
                                       double current_price,
                                       int core_id) {
    // Build per-core slot mask: slot c (partials off) or slots 2c+0..1 (on).
    int partial_on = oms->partial_exit_enabled ? 1 : 0;
    uint16_t my_mask = partial_on
        ? (uint16_t)((1u << (core_id * 2)) | (1u << (core_id * 2 + 1)))
        : (uint16_t)(1u << core_id);
    uint16_t bm = (uint16_t)(oms->portfolio.active_bitmap & my_mask);

    while (bm) {
        int slot = __builtin_ctz(bm);
        bm &= (uint16_t)(bm - 1);

        // Note: state->cores[] is indexed by core_id, not slot. With partials,
        // both leg-A (slot 2c) and leg-B (slot 2c+1) share the same CoreContext
        // at index core_id. last_entry_tick is stamped only on leg-A entries
        // by design (per-trade signal), so checking via core_id is correct.
        uint64_t entry_t = state->cores[core_id].last_entry_tick;
        if (entry_t == 0) continue;  // never stamped (shouldn't happen if active)
        if (entry_t > now_tick) {
            // Snapshot-drift guard. See "Snapshot Tick-Counter Drift"
            // invariant in CLAUDE.md.
            fprintf(stderr,
                "[time-exit] core %d slot %d: stale entry_tick from snapshot "
                "(entry_t=%llu > now_tick=%llu); resetting.\n",
                core_id, slot, (unsigned long long)entry_t,
                (unsigned long long)now_tick);
            state->cores[core_id].last_entry_tick = now_tick;
            continue;
        }
        uint64_t elapsed = now_tick - entry_t;
        // v5.12.3.C — per-core override. 0 = use global; >0 = override.
        // Branchless mask-select would obscure intent; slow-path branch is
        // negligible (~1ns per cycle).
        uint32_t max_hold = cfg.core_time_exit_ticks[core_id];
        if (max_hold == 0) max_hold = cfg.max_hold_ticks;
        if (elapsed < max_hold) continue;

        double entry_d = FPN_ToDouble(oms->portfolio.positions[slot].entry_price);
        if (entry_d <= 0.0) continue;
        double gain_pct = (current_price - entry_d) / entry_d;
        double min_gain = FPN_ToDouble(cfg.min_hold_gain_pct);
        if (gain_pct >= min_gain) continue;  // still profitable enough; keep it

        // Force-close via OMS_PushSubmit (drainer is sole Submit caller).
        FPN<F> qty       = oms->portfolio.positions[slot].quantity;
        FPN<F> price_fpn = FPN_FromDouble<F>(current_price);
        OMS_PushSubmit(oms, (int16_t)slot, ORDER_MARKET_SELL,
                        qty, FPN_Zero<F>(), FPN_Zero<F>(),
                        state->cores[core_id].strategy_id, price_fpn);

        fprintf(stderr,
            "[time-exit] core %d slot %d: held %lu ticks, gain %.3f%%\n",
            core_id, slot, (unsigned long)elapsed, gain_pct * 100.0);
    }
}

// Wrapper: preconditions + iterate. Existing callers (centralized + backtest)
// keep working unchanged.
template <unsigned F>
inline void EventLoop_TimeExit(EventLoopState<F>* state,
                                OrderManagerState<F>* oms,
                                const ControllerConfig<F>& cfg,
                                uint64_t now_tick,
                                double current_price) {
    if (cfg.max_hold_ticks == 0)  return;
    if (current_price <= 0.01)    return;

    for (int c = 0; c < state->registered_count; ++c) {
        EventLoop_TimeExitOneCore(state, oms, cfg, now_tick, current_price, c);
    }
}

//======================================================================================================
// [WS-STALENESS EMERGENCY FLATTEN] (v5.12.1.A.2)
//======================================================================================================
// Live-only safety net for extended WS dropouts during real-money trading.
// Slow-path reads producer's last_ws_tick_us (set in EngineSharded fan_out
// at every WS tick); when the gap to local_now_us exceeds
// cfg.ws_dead_time_flatten_threshold_secs and the gate cfg flag is set,
// CAS-wins one slow-path thread invokes EventLoop_FlattenAll to push
// market-exit commands for every active position into the standard
// drainer queue.
//
// Disabled by default (cfg.ws_dead_time_flatten_enabled = 0); flip to 1
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
//======================================================================================================
// EventLoop_FlattenAll: walk active-position bitmap, push market exits
// via OMS_PushSubmit. Drainer is sole Submit caller (CLAUDE.md item 5)
// so we go through PushSubmit, not Submit directly. Returns count of
// commands queued (0 if portfolio empty). Idempotent — caller (CAS
// winner) invokes once per flatten event; subsequent CAS-failed callers
// don't re-enter.
template <unsigned F>
inline int EventLoop_FlattenAll(EventLoopState<F>* state,
                                 OrderManagerState<F>* oms,
                                 double current_price,
                                 int reason_code) {
    int submitted = 0;
    uint16_t bm = oms->portfolio.active_bitmap;
    // event_price is for log/audit (the actual market fill happens at
    // exchange-side price). FPN_Zero on degenerate price preserves
    // existing OMS conventions.
    FPN<F> price_fpn = (current_price > 0.0)
        ? FPN_FromDouble<F>(current_price)
        : FPN_Zero<F>();
    int partial_on = oms->partial_exit_enabled ? 1 : 0;
    while (bm) {
        int slot = __builtin_ctz(bm);
        bm &= (uint16_t)(bm - 1);
        // logical_core: with partials, slots 2c+0/+1 → core c (shift-1).
        // No partials, slot == core. Branchless via partial_on multiplier
        // would obscure intent; 100us slow-path budget makes branch fine.
        int logical_core = partial_on ? (slot >> 1) : slot;
        FPN<F> qty = oms->portfolio.positions[slot].quantity;
        uint8_t sid = state->cores[logical_core].strategy_id;
        OMS_PushSubmit(oms, (int16_t)slot, ORDER_MARKET_SELL,
                        qty, FPN_Zero<F>(), FPN_Zero<F>(),
                        sid, price_fpn);
        submitted++;
    }
    if (submitted > 0) {
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
// (EngineSharded.hpp:2890) — saves ~50-100ns of vDSO clock_gettime
// per slow-path cycle per core. Caller passes its own measurement
// (live: system_clock; backtest: tick.timestamp for determinism).
//
// Returns: 0 if not breached or CAS lost; > 0 (count of submits) when
// this thread won the CAS and fired the flatten.
//
// Branchless considerations: cfg-flag check is the dominant fast path
// (cfg.ws_dead_time_flatten_enabled = 0 by default → early return ~5ns,
// no atomic load). Once enabled, the predicates are sequential branches
// — slow-path branches are fine; the rare-true outcome justifies branch
// over branchless mask compute.
template <unsigned F>
inline int EventLoop_CheckWsStaleness(EventLoopState<F>* state,
                                       const ControllerConfig<F>& cfg,
                                       double current_price,
                                       uint64_t now_us) {
    // Fast-path: gate disabled (default). Inlined check; no atomic load.
    if (!cfg.ws_dead_time_flatten_enabled) return 0;

    // Pre-warmup sentinel: producer hasn't published any tick yet.
    uint64_t last = state->last_ws_tick_us.load(std::memory_order_acquire);
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
    return EventLoop_FlattenAll(state, state->oms, current_price,
                                 /*reason*/1);
}

//======================================================================================================
// [POST-FLATTEN RECOVERY EXPIRY] (v5.12.1.A.3)
//======================================================================================================
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

//======================================================================================================
// [TRAILING-SL RATCHET / D9] (v4.7.17 — extracted from EngineSharded)
//======================================================================================================
// For each active position with gross gain >= cfg.tp_hold_score, write a
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
//======================================================================================================
// v4.7.38 (Phase C.1): per-core helper. Caller-checked preconditions.
// Implicitly fixes a pre-existing bug where the original walked
// active_bitmap by slot but indexed state->cores[slot] (per-core array) —
// under partials, slot 1 (core 0's leg B) wrongly read/wrote core 1's
// pending_params. OneCore correctly uses core_id for cores[] indexing
// while still iterating per-slot for portfolio.positions[].
template <unsigned F, unsigned W>
inline void EventLoop_TrailingSLRatchetOneCore(EventLoopState<F>* state,
                                                 const ControllerConfig<F>& cfg,
                                                 const RollingStats<F, W>& rolling,
                                                 double current_price,
                                                 int core_id) {
    double stddev_d     = FPN_ToDouble(rolling.price_stddev);
    double trail_dist_d = stddev_d * FPN_ToDouble(cfg.sl_trail_mult);
    double hold_thresh  = FPN_ToDouble(cfg.tp_hold_score);

    int partial_on = state->oms->partial_exit_enabled ? 1 : 0;
    uint16_t my_mask = partial_on
        ? (uint16_t)((1u << (core_id * 2)) | (1u << (core_id * 2 + 1)))
        : (uint16_t)(1u << core_id);
    uint16_t bm = (uint16_t)(state->oms->portfolio.active_bitmap & my_mask);

    // v5.1.7: fee-floor on the ratchet. The trailing-SL ratchet writes
    // ratchet_sl which the hot-path SG uses as effective_sl = max(sl, ratchet_sl).
    // Without a floor, the ratchet can push effective_sl above
    // entry × (1 - fee_rate × 2), causing the FIRST tiny pullback to fire
    // SG → exit at near-breakeven gross which becomes net-negative after
    // round-trip fees. Cap the ratchet at entry × (1 - 3 × fee_rate_taker)
    // to guarantee any SG-fired exit clears fees with at least 1× fee_rate
    // of margin.
    double fee_taker_d = FPN_ToDouble(cfg.fee_rate_taker);
    if (fee_taker_d <= 0.0) fee_taker_d = FPN_ToDouble(cfg.fee_rate);
    double fee_floor_pct = 3.0 * fee_taker_d;

    while (bm) {
        int slot = __builtin_ctz(bm);
        bm &= (uint16_t)(bm - 1);
        double entry_d = FPN_ToDouble(state->oms->portfolio.positions[slot].entry_price);
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
        // share it under partials. Index via core_id, not slot.
        FPN<F> new_ratchet = FPN_FromDouble<F>(new_ratchet_d);
        FPN<F> existing    = state->cores[core_id].pending_params.ratchet_sl;
        if (FPN_GreaterThan(new_ratchet, existing)) {
            state->cores[core_id].pending_params.ratchet_sl = new_ratchet;
            state->cores[core_id].dirty = 1;  // force push next cycle
        }
    }
}

// Wrapper: preconditions + iterate.
template <unsigned F, unsigned W>
inline void EventLoop_TrailingSLRatchet(EventLoopState<F>* state,
                                         const ControllerConfig<F>& cfg,
                                         const RollingStats<F, W>& rolling,
                                         double current_price) {
    if (FPN_IsZero(cfg.sl_trail_mult))   return;
    if (FPN_IsZero(cfg.tp_hold_score))   return;
    if (FPN_IsZero(rolling.price_stddev)) return;
    if (current_price <= 0.01)            return;

    for (int c = 0; c < state->registered_count; ++c) {
        EventLoop_TrailingSLRatchetOneCore(state, cfg, rolling, current_price, c);
    }
}

template <unsigned F>
inline int EventLoop_Unpause(EventLoopState<F>* state) {
    state->oms->kill_switch_tripped = 0;
    int resumed = 0;
    for (int slot = 0; slot < state->registered_count; ++slot) {
        ExecutionCore<F>* core = state->cores[slot].core;
        if (!core) continue;
        if (state->cores[slot].strategy_id == STRATEGY_NONE) continue;
        ExecutionCore_SetPermission(core, 1);
        ++resumed;
    }
    return resumed;
}

//======================================================================================================
// [SLOW PATH HOOK]
//======================================================================================================
// phase 04 doesn't run the existing PortfolioController_SlowPath. that wires
// in via phase 13's migration flag, where the per-core mode bypasses the
// existing slow path entirely (events ARE the slow path now) and the
// legacy mode keeps the old behavior. for now this hook is a no-op and the
// run-loop is purely event-driven.
//======================================================================================================
template <unsigned F>
inline int EventLoop_SlowPath(EventLoopState<F>* state) {
    (void)state;
    return 0;  // no-op for phase 04
}

//======================================================================================================
// [RUN CONTROLLER]
//======================================================================================================
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

}  // namespace tt
