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
#include "../ML_Headers/RollingStats.hpp"
#include "../Strategies/StrategyParameters.hpp"
#include "ExecutionCore.hpp"
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
inline void EventLoop_OnEvent(EventLoopState<F>* state, const TradeEvent<F>& event) {
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
    const void* ema_price     = nullptr     // const FPN<F>*
) {
    int rebuilt = 0;
    for (int slot = 0; slot < state->registered_count; ++slot) {
        if (state->cores[slot].strategy_id == STRATEGY_NONE) continue;
        // v4.0 per-core overrides: resolve the cfg for this core. Stack-local
        // copy with override fields swapped in. Strategies receive a "fully
        // resolved" cfg and don't need to know about the override mechanism.
        ControllerConfig<F> resolved_cfg =
            ControllerConfig_ResolveForCore(*config, slot);
        // Phase 6prep sharded c13/c15: pack ML extras for ML cores. Non-ML cores
        // get nullptr (the dispatcher passes it through and ML_BuildParameters
        // never runs anyway). Stack-allocated; ML_BuildParameters dereferences
        // and copies what it needs synchronously.
        MLBuildContext ml_ctx{};
        void* dispatch_ctx = nullptr;
        if (state->cores[slot].strategy_id == STRATEGY_ML) {
            ml_ctx.model_handle   = state->cores[slot].model_handle;
            ml_ctx.confidence     = &state->cores[slot].confidence;
            ml_ctx.out_prediction = &state->cores[slot].staged_prediction;
            ml_ctx.out_confidence = &state->cores[slot].last_confidence;
            // v4.0 train-serve parity: pass through ROR + EMA from engine
            // slow path so Regime_ComputeSignals can produce the full
            // feature set the backtest path produces during training.
            ml_ctx.ror_regressor  = (void*)ror_regressor;
            ml_ctx.ema_price      = (void*)ema_price;
            dispatch_ctx = &ml_ctx;
        }
        Strategy_BuildParameters(
            state->cores[slot].strategy_id,
            rolling,
            &resolved_cfg,
            state->cores[slot].allocated_balance,
            &state->cores[slot].pending_params,
            rolling_long,
            dispatch_ctx
        );

        // v4.0.3 cross-cutting checks applied uniformly across all strategies:
        //
        // SPACING: zero-gate if the proposed entry is too close to this
        // core's last entry. Prevents clustering positions at similar prices
        // (which produces correlated wins/losses, not independent diversification).
        // Mirrors legacy PortfolioController spacing logic.
        if (!Strategy_SpacingOk(state->cores[slot].pending_params.bg_price_threshold,
                                 state->cores[slot].last_entry_price,
                                 rolling, &resolved_cfg)) {
            state->cores[slot].pending_params.bg_price_threshold = FPN_Zero<F>();
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
