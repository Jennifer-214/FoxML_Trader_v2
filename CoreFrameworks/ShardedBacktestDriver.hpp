// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [SHARDED BACKTEST DRIVER]
//
// Phase 11 of the per-core sharded engine. Single-threaded driver that runs an
// EventLoopState + N execution cores against a recorded tick stream. Used by
// the backtest suite (and by phase 11 functional tests) to validate the
// per-core architecture against deterministic synthetic data.
//
// Why single-threaded:
//   Backtests need bit-for-bit determinism so changes to the engine can be
//   validated against historical results. The production hot path runs each
//   core on its own thread, but the backtest collapses everything onto one
//   thread and ticks each core in sequence per tick. The execution core code
//   path is identical — no thread spawning, no SPSC contention, just a
//   for-loop fan-out. Same logic, same outcomes.
//
// Per tick the driver does:
//   1. RollingStats_Push (feeds the slow-path strategy parameter rebuild)
//   2. For each registered core: ExecutionCore_Tick(core, tick)
//   3. EventLoop_DrainEvents (process any entries / exits the cores fired)
//   4. On slow-path cadence (every N ticks):
//        - EventLoop_RebuildAllParameters (strategy → fresh gate params)
//        - EventLoop_PushParameters (atomic push to each core's seqlock)
//        - EventLoop_KillSwitchEvaluate (drawdown / floor check)
//
// At end-of-stream the driver does one final drain to flush any in-flight
// events from the last tick.
//
// Determinism notes:
//   - Fan-out order is core 0 → core 1 → ... → core N. Documented and stable.
//   - DrainEvents uses round-robin which is deterministic given fixed core
//     count.
//   - Slow-path interval is tick-driven, not wall-clock. Same ticks → same
//     slow-path firing pattern.
//   - The execution cores have no random sources, no time-of-day reads, no
//     atomic counters that depend on scheduling. Pure functions of input.
//
// Phase 13 will wire this driver into the production BacktestEngine via a
// thin shim — Backtest_Run dispatches to ShardedBacktest_Run when the
// engine_mode config is set to "sharded".
//======================================================================================================

#pragma once

#include "../ML_Headers/FlowFeatures.hpp"  // v4.5 Wave 1 — D.1/D.2/D.4
#include "../ML_Headers/LinearRegression3X.hpp"
#include "../ML_Headers/ROR_regressor.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../Strategies/RegimeDetector.hpp"  // CumDeltaState, TickRateState
#include "ControllerEventLoop.hpp"
#include "ExecutionCore.hpp"
#include "OrderManager.hpp"
#include "Tick.hpp"

#include <cstdint>

namespace tt {

//======================================================================================================
// [DRIVER STATE]
//======================================================================================================
// Bag of pointers to everything the driver needs to step a single tick. The
// caller owns all the underlying objects and is responsible for their
// lifetime; the driver just orchestrates them. Keeping it as a struct of
// pointers (instead of inlining the state) lets the production BacktestEngine
// reuse its existing PortfolioController-adjacent objects without copying.
//
// rolling and config are optional (nullptr disables the slow-path strategy
// rebuild). Useful for tests that want pure event drain validation without
// the full strategy pipeline.
//======================================================================================================
template <unsigned F, unsigned W = 128, unsigned WL = 512>
struct ShardedBacktestDriver {
    EventLoopState<F>*       state;
    RollingStats<F, W>*      rolling;       // short window, nullptr = no strategy rebuild
    RollingStats<F, WL>*     rolling_long;  // long window (W=512), optional, fed in step alongside short
    const ControllerConfig<F>* config;      // optional, must be set if rolling is set
    OrderManagerState<F>*    oms;           // optional, nullptr = no OMS tick after drain
    int slow_path_interval;                 // ticks between slow-path firings (e.g. 64)
    uint64_t slow_path_runs;                // observability counter

    // Track E.1 — train-serve parity state. Optional. When set, driver pushes
    // these on slow-path firings (mirroring EngineSharded_Run lines 794-818)
    // and threads them into EventLoop_RebuildAllParameters so ML strategies
    // see the same RegimeSignals features the live path produces. NULL fields
    // are skipped — driver works the same as before for legacy callers.
    RollingStats<F, 256>*  rolling_medium;
    RollingStats<F, 1024>* rolling_baseline;
    CumDeltaState<F>*      cumdelta_state;
    TickRateState*         tick_rate_state;
    RORRegressor<F>*       regime_ror;
    const FPN<F>*          ema_price;       // caller updates per-tick before RunTick
    // Track E.3 — depth-derived gate input. Caller updates this pointer (or
    // its target) per-tick from DepthReplayState's current snapshot. Driver
    // threads it to RebuildAllParameters; RebuildAll vetoes buys when
    // *book_imbalance < cfg.min_book_imbalance. NULL = no depth feed
    // (cfg.min_book_imbalance gate stays inert, pre-E.3 behavior).
    const FPN<F>*          book_imbalance;

    // v4.5 Wave 1 — D.1/D.2/D.4 state for the new microstructure features.
    // Driver mutates these on slow-path firings (Push functions in
    // ML_Headers/FlowFeatures.hpp) and threads pointers through to
    // EventLoop_RebuildAllParameters. Type-erased to keep the driver's
    // template surface bounded; ML_BuildParameters re-typed at the
    // consumer end. NULL = legacy callers (pre-Wave-1 behavior).
    void*                  book_imb_history;   // BookImbalanceHistory<F, 1024>*
    void*                  flow_state;         // FlowState*
    void*                  large_trade_state;  // LargeTradeState<F, 1024>*
    // v4.6 Wave 2 — D.3 spread dynamics. spread_state ring; current
    // spread + mid_price observed from depth state (caller-owned holders
    // — caller updates per-tick before RunTick). Driver pushes spread to
    // the ring on slow-path; passes current values straight through to
    // RebuildAllParameters.
    void*                  spread_state;       // SpreadState<F, 1024>*
    const FPN<F>*          current_spread;     // BookSnapshot::spread holder
    const FPN<F>*          current_mid_price;  // BookSnapshot::mid_price holder

    // Track E.1 — slow-path completion hook. Fires AFTER slow-path
    // RollingStats pushes / RebuildAllParameters / KillSwitchEvaluate, BEFORE
    // returning from RunTick. Used by BacktestSharded_Run to collect ML
    // features without touching the driver's tick-stepping logic.
    void (*on_slow_path)(void* ctx,
                         ShardedBacktestDriver<F, W, WL>* drv,
                         const Tick<F>& tick,
                         int tick_index);
    void* hook_ctx;
};

//======================================================================================================
// [INIT]
//======================================================================================================
// Wire the driver to its dependencies. The state must already be initialized
// and have its execution cores registered + strategies assigned. Set rolling
// and config to nullptr to skip the slow-path strategy rebuild step (the
// driver still drains events and runs the kill switch).
//======================================================================================================
template <unsigned F, unsigned W = 128, unsigned WL = 512>
inline void ShardedBacktestDriver_Init(ShardedBacktestDriver<F, W, WL>* drv,
                                        EventLoopState<F>* state,
                                        RollingStats<F, W>* rolling,
                                        const ControllerConfig<F>* config,
                                        int slow_path_interval,
                                        RollingStats<F, WL>* rolling_long = nullptr,
                                        OrderManagerState<F>* oms = nullptr) {
    drv->state              = state;
    drv->rolling            = rolling;
    drv->rolling_long       = rolling_long;
    drv->config             = config;
    drv->oms                = oms;
    drv->slow_path_interval = slow_path_interval > 0 ? slow_path_interval : 64;
    drv->slow_path_runs     = 0;
    // Track E.1 — optional train-serve state + hook. Caller assigns directly
    // after Init when needed (BacktestSharded_Run does this for feature
    // collection). NULL = legacy behavior, slow path skips v4.3 pushes.
    drv->rolling_medium     = nullptr;
    drv->rolling_baseline   = nullptr;
    drv->cumdelta_state     = nullptr;
    drv->tick_rate_state    = nullptr;
    drv->regime_ror         = nullptr;
    drv->ema_price          = nullptr;
    drv->book_imbalance     = nullptr;  // Track E.3
    // v4.5 Wave 1
    drv->book_imb_history   = nullptr;
    drv->flow_state         = nullptr;
    drv->large_trade_state  = nullptr;
    // v4.6 Wave 2
    drv->spread_state       = nullptr;
    drv->current_spread     = nullptr;
    drv->current_mid_price  = nullptr;
    drv->on_slow_path       = nullptr;
    drv->hook_ctx           = nullptr;
}

//======================================================================================================
// [RUN ONE TICK]
//======================================================================================================
// Step the entire engine through a single tick. tick_index is 0-based and is
// used to determine slow-path cadence (firing every slow_path_interval ticks).
//
// The order is load-bearing for determinism — RollingStats first so the
// slow-path rebuild sees fresh stats, then fan-out to cores, then drain
// events, then slow-path on cadence. Pitfall P11.6 covers this ordering
// requirement.
//======================================================================================================
template <unsigned F, unsigned W = 128, unsigned WL = 512>
inline void ShardedBacktest_RunTick(ShardedBacktestDriver<F, W, WL>* drv,
                                     const Tick<F>& tick,
                                     int tick_index) {
    // 1. Fan out to every registered execution core. Order is fixed
    //    (slot 0 first) for determinism.
    for (int slot = 0; slot < drv->state->registered_count; ++slot) {
        ExecutionCore<F>* core = drv->state->cores[slot].core;
        if (core) ExecutionCore_Tick(core, tick);
    }

    // 2. Drain any trade events the cores fired this tick.
    EventLoop_DrainEvents(drv->state);

    // 2a. v4.7.37 (Phase B reordered) — drain submit_queue first. EventLoop_
    //     TimeExit (called below in the slow-path block) and any future
    //     producer-side submit pushes route through OMS_PushSubmit instead
    //     of calling Submit directly. Backtest is single-threaded so the
    //     drain happens inline here, mirroring live's drainer thread.
    if (drv->oms) {
        // v5.4.1 Bug B2: drain count must cover all queues that may have
        // been written. Under partials, ExecutionCore producers push
        // SubmitCommands keyed by portfolio_slot (0..2N-1), so the drain
        // walks 2N queues instead of N.
        int dc = drv->oms->partial_exit_enabled
            ? drv->state->registered_count * 2 : drv->state->registered_count;
        OMS_DrainSubmit(drv->oms, dc);
    }

    // 2b. In event_log_mode=1, portfolio mutation happens in OMS_Tick
    //     (the fill handler), not in OnEvent. Tick the OMS so the fill
    //     handler runs after the drain enqueued synthetic results.
    if (drv->oms) OrderManager_Tick(drv->oms);

    // 2c. v4.7.15 — train-serve parity. When OMS runs in event_log_mode=1
    //     (live default since v4.7.1), per-fill bookkeeping
    //     (core_open_notional, core_fees, ConfidenceScorer feedback,
    //     wins/losses, SL cooldown) is populated by OrderManager_Tick
    //     into FillRecords + masks but NOT applied to the CoreContexts
    //     until DrainPostFill consumes them. Live engine calls this on
    //     the drainer thread after each OrderManager_Tick (EngineSharded
    //     line 1594). Mirror the same call here so backtest CoreContexts
    //     match live for identical inputs. Safe to call when masks are
    //     zero (no fills this tick) — the function early-exits per slot.
    if (drv->oms && drv->oms->event_log_mode != 0 && drv->config) {
        EventLoop_DrainPostFill(drv->state, drv->oms,
                                 drv->config->sl_cooldown_cycles);
    }

    // 3. Slow path on cadence. tick_index is 0-based so we fire every
    //    slow_path_interval ticks starting from interval-1.
    //
    // CRITICAL: RollingStats is pushed HERE inside the slow path, NOT once
    // per tick. The legacy PortfolioController_Tick also samples this way
    // (line 1383-1384 of PortfolioController.hpp), so 128 samples covers
    // ~128*poll_interval ticks of history, not just 128 ticks. Sampling
    // every tick narrows the window 64x and produces tighter dip thresholds
    // that miss the trades the legacy path takes.
    if (((tick_index + 1) % drv->slow_path_interval) == 0) {
        if (drv->rolling) {
            RollingStats_Push(drv->rolling, tick.price, tick.volume);
        }
        if (drv->rolling_long) {
            RollingStats_Push(drv->rolling_long, tick.price, tick.volume);
        }
        // Track E.1 — push v4.3 state at the same cadence EngineSharded_Run
        // does (lines 804-818). Each guarded so callers that don't supply
        // the state get prior behavior.
        if (drv->rolling_medium) {
            RollingStats_Push(drv->rolling_medium, tick.price, tick.volume);
        }
        if (drv->rolling_baseline) {
            RollingStats_Push(drv->rolling_baseline, tick.price, tick.volume);
        }
        if (drv->cumdelta_state) {
            // is_buyer_maker available on Tick<F> per the v4.3 plumbing.
            CumDelta_Push(drv->cumdelta_state, tick.volume, tick.is_buyer_maker);
        }
        if (drv->tick_rate_state) {
            TickRate_Push(drv->tick_rate_state, tick.timestamp);
        }
        if (drv->regime_ror && drv->rolling) {
            // Slope-of-slopes feed (mirrors EngineSharded line 813-818 +
            // legacy PortfolioController.hpp:1552). RORRegressor takes a
            // LinearRegression3XResult; intercept is irrelevant for ROR.
            LinearRegression3XResult<F> slope_sample;
            slope_sample.model.slope     = drv->rolling->price_slope;
            slope_sample.model.intercept = FPN_Zero<F>();
            slope_sample.r_squared       = drv->rolling->price_r_squared;
            RORRegressor_Push(drv->regime_ror, slope_sample);
        }
        // v4.5 Wave 1 — push the new feature-state buffers at the same
        // slow-path cadence as v4.3 state. NULL guards keep legacy
        // callers / tests at pre-Wave-1 behavior (zero features).
        if (drv->book_imb_history && drv->book_imbalance) {
            BookImbHistory_Push(
                (BookImbalanceHistory<F, 1024>*)drv->book_imb_history,
                *drv->book_imbalance);
        }
        if (drv->flow_state) {
            // Signed volume: is_buyer_maker=1 → seller aggression (negative);
            // =0 → buyer aggression (+). Mirrors CumDelta_Push.
            double signed_vol = FPN_ToDouble(tick.volume);
            if (tick.is_buyer_maker) signed_vol = -signed_vol;
            FlowState_Push((FlowState*)drv->flow_state,
                            tick.timestamp, signed_vol);
        }
        if (drv->large_trade_state) {
            LargeTradeState_Push(
                (LargeTradeState<F, 1024>*)drv->large_trade_state,
                tick.volume);
        }
        // v4.6 Wave 2 — push current spread into z-score ring. Caller must
        // have updated *drv->current_spread before RunTick.
        if (drv->spread_state && drv->current_spread) {
            SpreadState_Push(
                (SpreadState<F, 1024>*)drv->spread_state,
                *drv->current_spread);
        }
        if (drv->rolling && drv->config) {
            // v5.1.2 (full symmetric decoupling): backtest pushes to per-
            // core slow_state via the shared helper, then rebuilds via
            // RebuildAllParameters_PerCore. Train-serve parity is now
            // structural — backtest, centralized, and per_core_slow live
            // all consume the same `state.cores[c].slow_state`.
            //
            // The driver's shared rolling state above is still pushed
            // (keeps existing tests + benchmark/regression callers
            // working) but no longer feeds the rebuild call.
            EventLoop_UpdateRollingStateAllCores(
                drv->state, tick.price, tick.volume, tick.timestamp,
                tick.is_buyer_maker,
                drv->book_imbalance ? *drv->book_imbalance : FPN_Zero<F>(),
                drv->current_spread ? *drv->current_spread : FPN_Zero<F>(),
                /*depth_enabled=*/(drv->book_imbalance || drv->current_spread) ? 1 : 0);
            // ema_price replication — same pattern as live producer.
            if (drv->ema_price) {
                EventLoop_UpdateEmaPriceAllCores(drv->state, *drv->ema_price);
            }
            EventLoop_RebuildAllParameters_PerCore(
                drv->state, drv->config,
                /* current_price   */ &tick.price,
                /* timestamp_us    */ tick.timestamp,
                /* book_imbalance  */ drv->book_imbalance,
                /* current_spread  */ drv->current_spread
                                       ? FPN_ToDouble(*drv->current_spread) : 0.0,
                /* current_mid_price*/ drv->current_mid_price
                                       ? FPN_ToDouble(*drv->current_mid_price) : 0.0);
        }
        EventLoop_PushParameters(drv->state);
        EventLoop_KillSwitchEvaluate(drv->state);

        // v4.7.17: same shared time-exit + trailing-SL ratchet helpers the
        // live engine calls (EngineSharded_Run line ~1117). Pre-v4.7.17 both
        // were inlined into EngineSharded only, leaving backtest silently
        // no-op when user enabled cfg.max_hold_ticks or cfg.tp_hold_score
        // → ML model trained on backtest never learned the early-exit
        // pattern that live applies. tick_index is the per-run tick
        // counter, equivalent to live's `ticks_produced.load()`.
        if (drv->oms && drv->config && drv->rolling) {
            double current_price = FPN_ToDouble(tick.price);
            EventLoop_TimeExit(drv->state, drv->oms, *drv->config,
                               (uint64_t)tick_index, current_price);
            EventLoop_TrailingSLRatchet(drv->state, *drv->config,
                                         *drv->rolling, current_price);
        }

        drv->slow_path_runs++;

        // Track E.1 — fire the slow-path hook AFTER all driver work finishes.
        // The hook (when registered by the caller) reads the now-fresh state
        // pointers above and runs feature collection / regime tracking /
        // anything else that wants a slow-path observability point. Driver
        // hot loop stays unchanged when no hook is registered.
        if (drv->on_slow_path) {
            drv->on_slow_path(drv->hook_ctx, drv, tick, tick_index);
        }
    }
}

//======================================================================================================
// [RUN STREAM]
//======================================================================================================
// Convenience wrapper that runs a whole tick stream end-to-end and does the
// final event drain. For tests that just want to feed an array and check the
// outcome.
//======================================================================================================
template <unsigned F, unsigned W = 128, unsigned WL = 512>
inline void ShardedBacktest_Run(ShardedBacktestDriver<F, W, WL>* drv,
                                 const Tick<F>* ticks,
                                 int num_ticks) {
    for (int i = 0; i < num_ticks; ++i) {
        ShardedBacktest_RunTick(drv, ticks[i], i);
    }
    // Final drain — catches anything the last tick fired that the slow path
    // didn't have time to process.
    EventLoop_DrainEvents(drv->state);
    // v4.7.37 (Phase B reordered): drain any pending submit commands from
    // the final iteration BEFORE OMS_Tick so they get filled.
    if (drv->oms) {
        // v5.4.1 Bug B2: drain count must cover all queues that may have
        // been written. Under partials, ExecutionCore producers push
        // SubmitCommands keyed by portfolio_slot (0..2N-1), so the drain
        // walks 2N queues instead of N.
        int dc = drv->oms->partial_exit_enabled
            ? drv->state->registered_count * 2 : drv->state->registered_count;
        OMS_DrainSubmit(drv->oms, dc);
    }
    if (drv->oms) OrderManager_Tick(drv->oms);
    // v4.7.15: drain post-fill in mode 1 to match live's final-flush loop
    // (EngineSharded line 1597-1604). Without this, the last tick's
    // FillRecords sit in the OMS buffers and never apply to CoreContexts.
    if (drv->oms && drv->oms->event_log_mode != 0 && drv->config) {
        EventLoop_DrainPostFill(drv->state, drv->oms,
                                 drv->config->sl_cooldown_cycles);
    }
}

}  // namespace tt
