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
#include "../Strategies/StrategyParameters.hpp"
#include "ExecutionCore.hpp"
#include "Notify.hpp"
#include "OrderManager.hpp"
#include "Portfolio.hpp"
#include "ShardedTradeLog.hpp"
#include "TradeEvent.hpp"

#include <cstdint>
#include <ctime>

namespace tt {

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
    void*    model_handle;         // ModelHandle<F>* for STRATEGY_ML cores (nullptr for others)
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
    double staged_prediction;      // prediction from last ML rebuild
    double active_prediction;      // prediction at last entry submit (0 = no open pos)
    double last_confidence;        // most recent ConfidenceScorer_Compute result
    // v4.0.3 spacing: last entry price for this core, set by drainer on
    // entry submit. Strategy _BuildParameters checks
    // |new_entry - last_entry_price| < stddev × spacing_multiplier and
    // zero-gates if too close, preventing entry clustering at similar
    // prices. Mirrors legacy PortfolioController spacing logic.
    FPN<F> last_entry_price;
    uint64_t last_entry_tick;      // for time-based exit (A3)
    // v4.0.3 D7 SL cooldown: after a stop-loss exit, pause entries on this
    // core for N slow-path cycles. Decremented each rebuild; entries
    // zero-gated while > 0. Optionally adaptive — scales by trend confidence
    // at SL time (cfg.sl_cooldown_adaptive).
    uint32_t sl_cooldown_remaining;
    // v4.0.3 D8 halt reason: most recent reason the gate was zero-gated.
    // 0 = ok / armed; 1 = spacing; 2 = vwap; 3 = long-slope; 4 = vol-delta;
    // 5 = min-stddev; 6 = sl-cooldown; 7 = warmup; 8 = core-budget (Phase 2.2);
    // 9 = core-kill (Phase 3). Displayed in GUI per core.
    uint8_t  halt_reason;
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
};

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
        state->cores[i].entries_processed = 0;
        state->cores[i].exits_processed = 0;
        // Phase 6prep sharded: ConfidenceScorer with safe defaults. EngineSharded
        // re-inits with cfg values for STRATEGY_ML cores after this; non-ML cores
        // keep these defaults (the scorer never gets fed, so it stays inert).
        ConfidenceScorer_Init(&state->cores[i].confidence,
                              CONFIDENCE_IC_WINDOW_DEFAULT,
                              CONFIDENCE_FRESHNESS_TAU_DEFAULT);
        state->cores[i].staged_prediction = 0.0;
        state->cores[i].active_prediction = 0.0;
        state->cores[i].last_confidence = 0.0;
        state->cores[i].last_entry_price = FPN_Zero<F>();
        state->cores[i].last_entry_tick  = 0;
        state->cores[i].sl_cooldown_remaining = 0;
        state->cores[i].halt_reason = 0;
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
// schedule, not the allocation. Entry-time stamping (active_prediction
// reset, ConfidenceScorer update) fires per-leg-exit, but
// active_prediction was already stashed by leg-A entry; the second update
// just resets it to 0 a second time. Acceptable; future cleanup could
// gate on leg via FillRecord.
//
// All slow-path. Single-threaded (drainer is sole reader, OMS_Tick on
// the same thread is sole writer). FPN-pure on the per-core math.
//======================================================================================================
template <unsigned F>
inline void EventLoop_DrainPostFill(EventLoopState<F>* state,
                                     OrderManagerState<F>* oms,
                                     uint32_t sl_cooldown_cycles) {
    const int partial_on = oms->partial_exit_enabled ? 1 : 0;
    const int max_slot   = partial_on ? state->registered_count * 2
                                      : state->registered_count;

    // ---- Entries: open_notional / fees ----
    uint16_t open_mask = oms->last_opened_mask;
    while (open_mask) {
        int slot = __builtin_ctz(open_mask);
        open_mask &= (uint16_t)(open_mask - 1);
        if (slot < 0 || slot >= max_slot) continue;
        int core_id = partial_on ? (slot >> 1) : slot;
        CoreContext<F>& ctx = state->cores[core_id];
        const auto& rec = oms->last_fill[slot];
        ctx.core_open_notional = FPN_Add(ctx.core_open_notional, rec.entry_notional);
        ctx.core_fees          = FPN_AddSat(ctx.core_fees, rec.entry_fee);
    }
    oms->last_opened_mask = 0;

    // ---- Exits: realized / open_notional decrement / fees / W-L /
    //            ConfidenceScorer / SL cooldown / pnl_feeder ----
    uint16_t close_mask = oms->last_closed_mask;
    while (close_mask) {
        int slot = __builtin_ctz(close_mask);
        close_mask &= (uint16_t)(close_mask - 1);
        if (slot < 0 || slot >= max_slot) continue;
        int core_id = partial_on ? (slot >> 1) : slot;
        CoreContext<F>& ctx = state->cores[core_id];
        const auto& rec = oms->last_fill[slot];

        ctx.core_realized      = FPN_Add(ctx.core_realized, rec.exit_net_pnl);
        ctx.core_open_notional = FPN_SubSat(ctx.core_open_notional, rec.exit_entry_notional);
        ctx.core_fees          = FPN_AddSat(ctx.core_fees, rec.exit_total_fees);
        ctx.core_wins   += (rec.was_win ? 1u : 0u);
        ctx.core_losses += (rec.was_win ? 0u : 1u);

        double realized = oms->last_realized_return[slot];
        if (ctx.strategy_id == STRATEGY_ML) {
            ConfidenceScorer_Update(&ctx.confidence,
                                    ctx.active_prediction, realized);
            ctx.active_prediction = 0.0;
        }
        if (realized < 0.0 && sl_cooldown_cycles > 0) {
            ctx.sl_cooldown_remaining = sl_cooldown_cycles;
        }
        RegressionFeederX_Push(&ctx.pnl_feeder, FPN_FromDouble<F>(realized));
    }
    oms->last_closed_mask = 0;
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
        if (is_entry) {
            ctx->entries_processed++;
            state->total_entries++;
        }
        if (is_exit) {
            ctx->exits_processed++;
            state->total_exits++;
        }
        state->total_events_processed++;
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
    double      current_mid_price = 0.0       // BookSnapshot::mid_price → double
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
    for (int slot = 0; slot < state->registered_count; ++slot) {
        if (state->cores[slot].strategy_id == STRATEGY_NONE) continue;
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
            if (new_regime != old_regime &&
                (state->oms->portfolio.active_bitmap & (uint16_t)(1u << slot))) {
                // Move ratchet to current rolling avg minus tighter offset
                // (stddev × 1.0 = closer than the trailing default).
                FPN<F> tight_sl = FPN_Sub(rolling->price_avg,
                                           rolling->price_stddev);
                if (FPN_GreaterThan(tight_sl,
                        state->cores[slot].pending_params.ratchet_sl)) {
                    state->cores[slot].pending_params.ratchet_sl = tight_sl;
                    state->cores[slot].dirty = 1;
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
            dispatch_ctx = &ml_ctx;
        }
        // v4.0.4: stash the resolved strategy for GUI display. For non-AUTO
        // cores this just mirrors strategy_id; for AUTO it's the regime-
        // resolved concrete strategy.
        state->cores[slot].resolved_strategy_id = effective_strategy_id;

        Strategy_BuildParameters(
            effective_strategy_id,
            rolling,
            &resolved_cfg,
            state->cores[slot].allocated_balance,
            &state->cores[slot].pending_params,
            rolling_long,
            dispatch_ctx
        );

        // v4.0.3 D9: clear ratchet_sl when no position active on this core,
        // so stale trailing state from previous trade doesn't leak into the
        // next entry. Engine slow-path code below SETS ratchet_sl when a
        // position is active and trailing should kick in.
        bool slot_active = (state->oms->portfolio.active_bitmap & (uint16_t)(1u << slot)) != 0;
        if (!slot_active) {
            state->cores[slot].pending_params.ratchet_sl = FPN_Zero<F>();
        }

        // v4.0.3 cross-cutting checks applied uniformly across all strategies.
        // Each is a "zero-gate if violated" filter — preserves the strategy's
        // intended TP/SL/qty but disables the entry trigger. Halt reasons are
        // tracked per-core for GUI display.
        //
        // Reasons: 0=ok, 1=spacing, 2=vwap, 3=long-slope, 4=vol-delta,
        //          5=min-stddev, 6=sl-cooldown, 7=warmup, 8=core-budget,
        //          9=core-kill, 10=book-imbalance (Track E.3)
        state->cores[slot].halt_reason = 0;
        auto zero_gate = [&](uint8_t reason) {
            state->cores[slot].pending_params.bg_price_threshold = FPN_Zero<F>();
            if (state->cores[slot].halt_reason == 0)  // first reason wins
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
            if (state->cores[slot].halt_reason == 0)
                state->cores[slot].halt_reason = 10;
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
                zero_gate(8);
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
            if (config->enable_mtm_kill_switch && px_in &&
                !FPN_IsZero(*px_in) && (state->oms->portfolio.active_bitmap & (1u << slot))) {
                Position<F>& pos = state->oms->portfolio.positions[slot];
                FPN<F> diff = FPN_Sub(*px_in, pos.entry_price);
                unrealized = FPN_Mul(diff, pos.quantity);
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
                zero_gate(9);  // HALT_CORE_KILL
            }
        }

        // SL COOLDOWN: decrement counter; if still active, zero-gate.
        if (state->cores[slot].sl_cooldown_remaining > 0) {
            state->cores[slot].sl_cooldown_remaining--;
            zero_gate(6);
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
        if (!Strategy_SpacingOk(state->cores[slot].pending_params.bg_price_threshold,
                                 state->cores[slot].last_entry_price,
                                 rolling, &spacing_cfg)) {
            zero_gate(1);
        }
        // VWAP gate: forces entries below VWAP — buy retracements, not pumps.
        if (!FPN_IsZero(resolved_cfg.vwap_offset) && !FPN_IsZero(rolling->vwap)) {
            FPN<F> vwap_threshold = FPN_Sub(rolling->vwap,
                FPN_Mul(rolling->vwap, resolved_cfg.vwap_offset));
            if (FPN_GreaterThan(state->cores[slot].pending_params.bg_price_threshold,
                                 vwap_threshold)) {
                zero_gate(2);
            }
        }
        // LONG-SLOPE gate: blocks buys in confirmed downtrends.
        if (!FPN_IsZero(resolved_cfg.min_long_slope) && rolling_long &&
            !FPN_IsZero(rolling_long->price_avg)) {
            FPN<F> long_rel_slope = FPN_DivNoAssert(rolling_long->price_slope,
                                                     rolling_long->price_avg);
            if (FPN_LessThan(long_rel_slope, resolved_cfg.min_long_slope)) {
                zero_gate(3);
            }
        }
        // VOLUME DELTA gate: blocks heavy dumps.
        if (!FPN_IsZero(resolved_cfg.min_buy_delta) &&
            FPN_LessThan(rolling->volume_delta, resolved_cfg.min_buy_delta)) {
            zero_gate(4);
        }
        // MIN STDDEV gate: skip dead markets.
        if (!FPN_IsZero(resolved_cfg.min_stddev_pct) && !FPN_IsZero(rolling->price_avg)) {
            FPN<F> stddev_ratio = FPN_DivNoAssert(rolling->price_stddev,
                                                    rolling->price_avg);
            if (FPN_LessThan(stddev_ratio, resolved_cfg.min_stddev_pct)) {
                zero_gate(5);
            }
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
        ++rebuilt;
    }
    return rebuilt;
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
inline int EventLoop_PushParameters(EventLoopState<F>* state) {
    int pushed = 0;
    for (int slot = 0; slot < state->registered_count; ++slot) {
        if (state->cores[slot].dirty == 0) continue;
        ExecutionCore<F>* core = state->cores[slot].core;
        if (core == nullptr) {
            state->cores[slot].dirty = 0;
            continue;
        }
        ExecutionCore_SetParameters(core, state->cores[slot].pending_params);
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
