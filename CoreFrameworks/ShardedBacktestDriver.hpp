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

#include "../ML_Headers/RollingStats.hpp"
#include "ControllerEventLoop.hpp"
#include "ExecutionCore.hpp"
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
    int slow_path_interval;                 // ticks between slow-path firings (e.g. 64)
    uint64_t slow_path_runs;                // observability counter
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
                                        RollingStats<F, WL>* rolling_long = nullptr) {
    drv->state              = state;
    drv->rolling            = rolling;
    drv->rolling_long       = rolling_long;
    drv->config             = config;
    drv->slow_path_interval = slow_path_interval > 0 ? slow_path_interval : 64;
    drv->slow_path_runs     = 0;
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
        if (drv->rolling && drv->config) {
            EventLoop_RebuildAllParameters(drv->state, drv->rolling, drv->config, drv->rolling_long);
        }
        EventLoop_PushParameters(drv->state);
        EventLoop_KillSwitchEvaluate(drv->state);
        drv->slow_path_runs++;
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
}

}  // namespace tt
