// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/ShardedBacktestDriver.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[single-threaded backtest driver — same execution-core code path as live, collapsed onto one thread for bit-for-bit determinism]
// [CONTAINS]
//   - [STRUCT]_[ShardedBacktestDriver]
//   - [FUNCTION]_[ShardedBacktestDriver_Init]
//   - [FUNCTION]_[ShardedBacktest_RunTick]
//   - [FUNCTION]_[ShardedBacktest_Run]
//======================================================================================================
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
#include "../DataStream/BinanceDepth.hpp"  // v5.15.5.F.4d.1.B.4 WIP-13 — BookSnapshot<F> for EngineCommon_SlowPathCycleAllCores 9-arg signature
#include "ControllerEventLoop.hpp"
#include "EngineCommon.hpp"  // v5.15.5.F.4d.1.B.4 WIP-13 — train-serve execution-layer parity helpers (Phase C.4 BACKTEST migration)
#include "ExecutionCore.hpp"
#include "OrderManager.hpp"
#include "Tick.hpp"

#include <cstdint>

namespace tt {

//======================================================================
// [STRUCT]_[ShardedBacktestDriver]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[caller-owned pointer bag the driver orchestrates — optional slow-path/OMS/train-serve/depth state, NULL = legacy behavior per field]
//======================================================================
// [CODE]
//======================================================================
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
    // these on slow-path firings (mirroring EngineSharded_Run's producer
    // slow-path pushes in EngineSharded/Run.hpp)
    // and threads them into EventLoop_RebuildAllParameters so ML strategies
    // see the same RegimeSignals features the live path produces. NULL fields
    // are skipped — driver works the same as before for legacy callers.
    RollingStats<F, 256>*  rolling_medium;
    RollingStats<F, 1024>* rolling_baseline;
    CumDeltaState<F>*      cumdelta_state;
    TickRateState*         tick_rate_state;
    RORRegressor<F>*       regime_ror;
    const FPN_Binary<F>*          ema_price;       // caller updates per-tick before RunTick
    // Track E.3 — depth-derived gate input. Caller updates this pointer (or
    // its target) per-tick from DepthReplayState's current snapshot. Driver
    // threads it to RebuildAllParameters; RebuildAll vetoes buys when
    // *book_imbalance < cfg.min_book_imbalance. NULL = no depth feed
    // (cfg.min_book_imbalance gate stays inert, pre-E.3 behavior).
    const FPN_Binary<F>*          book_imbalance;

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
    const FPN_Binary<F>*          current_spread;     // BookSnapshot::spread holder
    const FPN_Binary<F>*          current_mid_price;  // BookSnapshot::mid_price holder

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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Bag of pointers to everything the driver needs to step a single tick. The
// caller owns all the underlying objects and is responsible for their
// lifetime; the driver just orchestrates them. Keeping it as a struct of
// pointers (instead of inlining the state) lets the production BacktestEngine
// reuse its existing PortfolioController-adjacent objects without copying.
//
// rolling and config are optional (nullptr disables the slow-path strategy
// rebuild). Useful for tests that want pure event drain validation without
// the full strategy pipeline.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[176B]
// [ALIGN]_[8]
// [CACHE_LINES]_[3]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[ShardedBacktestDriver]
//======================================================================

//======================================================================
// [FUNCTION]_[ShardedBacktestDriver_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[wire the driver to caller-owned deps; NULL rolling/config skips strategy rebuild; optional Track-E.1 state assigned post-Init]
//======================================================================
// [CODE]
//======================================================================
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Wire the driver to its dependencies. The state must already be initialized
// and have its execution cores registered + strategies assigned. Set rolling
// and config to nullptr to skip the slow-path strategy rebuild step (the
// driver still drains events and runs the kill switch).
//======================================================================
// [END_FUNCTION]_[ShardedBacktestDriver_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[ShardedBacktest_RunTick]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST] [DETERMINISM]]
// [REFERENCE]_[INVARIANT]_[H22]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one tick end-to-end — fixed-order core fan-out, drain, OMS tick + post-fill, slow-path cadence via EngineCommon_SlowPathCycleAllCores (M5 parity); ordering is load-bearing (P11.6)]
// [REFERENCE]_[DECISION]_[D-122]
// [REFERENCE]_[MEMORY]_[feedback_audit_canonical_sister_before_new_infra]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W = 128, unsigned WL = 512>
inline void ShardedBacktest_RunTick(ShardedBacktestDriver<F, W, WL>* drv,
                                     const Tick<F>& tick,
                                     int tick_index) {
    // 1. Fan out to every registered execution core. Order is fixed
    //    (slot 0 first) for determinism.
    for (int slot = 0; slot < drv->state->registered_count; ++slot) {
        ExecutionCore<F>* core = drv->state->nodes[tt::NodeIdx{(int16_t)slot}].core;
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
        // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
        int dc = BITMAP_IS_SET(drv->oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED)
            ? drv->state->registered_count * 2 : drv->state->registered_count;
        OMS_DrainSubmit(drv->oms, dc);
    }

    // 2b. In event_log_mode=1, portfolio mutation happens in OMS_Tick
    //     (the fill handler), not in OnEvent. Tick the OMS so the fill
    //     handler runs after the drain enqueued synthetic results.
    if (drv->oms) OrderManager_Tick(drv->oms);

    // 2c. v4.7.15 — train-serve parity. When OMS runs in event_log_mode=1
    //     (live default since v4.7.1), per-fill bookkeeping
    //     (node_open_notional, node_fees, ConfidenceScorer feedback,
    //     wins/losses, SL cooldown) is populated by OrderManager_Tick
    //     into FillRecords + masks but NOT applied to the NodeContexts
    //     until DrainPostFill consumes them. Live engine calls this on
    //     the drainer thread after each OrderManager_Tick (the drainer
    //     loop in EngineSharded/Run.hpp). Mirror the same call here so
    //     backtest NodeContexts match live for identical inputs. Safe to
    //     call when masks are zero (no fills this tick) — the function
    //     early-exits per slot.
    if (drv->oms &&
        BITMAP_ANY(drv->oms->oms_state_flags, tt::MASK_OMS_STATE_EVENT_LOG_MODE) &&
        drv->config) {
        // E.1.2.C leg 0 — through the SHARED binder (was a hand-rolled 4-arg
        // call that silently defaulted drift/ic_variant/node_cfg; live-parity
        // by construction now, incl. the exit-bandit flag + per-node fee).
        EngineCommon_DrainPostFill(*drv->state, *drv->oms, *drv->config);
    }

    // 2d. D-444 / A-2 T7-N1 — apply pending FillEvents PER TICK, not at the slow-path
    //     cadence: the per-trade stats sampler below (BacktestSharded.hpp — realized_pnl
    //     DELTA sign → win/loss counts, per-tick balance → MaxDrawdown/equity curve)
    //     requires the ledger to move per FILL exactly as the leaf's direct writes did.
    //     Apply-at-cadence would collapse multiple trades into ONE net delta (silent
    //     training/WF-visible stats change) AND make backtest ring overflow ROUTINE.
    //     No-op pop until the Phase-3 leaves emit. The cadence ComposeAndKillEval keeps
    //     its own internal apply (a harmless empty pop after this).
    if (drv->oms) {
        (void)EngineCommon_FillRingsApply(*drv->state, *drv->oms);
    }

    // 3. Slow path on cadence. tick_index is 0-based so we fire every
    //    slow_path_interval ticks starting from interval-1.
    //
    // CRITICAL: RollingStats is pushed HERE inside the slow path, NOT once
    // per tick. The legacy PortfolioController_Tick also samples this way
    // (its slow-path RollingStats_Push block), so 128 samples covers
    // ~128*poll_interval ticks of history, not just 128 ticks. Sampling
    // every tick narrows the window 64x and produces tighter dip thresholds
    // that miss the trades the legacy path takes.
    if (((tick_index + 1) % drv->slow_path_interval) == 0) {
        // D-122 feature ingress: money tick -> binary feature domain, ONE cast.
        FPN_Binary<64> price_b  = Money_ToBinary(tick.price);
        FPN_Binary<64> volume_b = Money_ToBinary(tick.volume);
        // PARITY-047 — forward the trade side here too. The defaulted
        // `int is_buyer_maker = 0` on RollingStats_Push had been absorbing the
        // omission on BOTH paths, so volume_delta was pinned at +1.0 identically
        // in training and serving (dead, not divergent). Fixed together with the
        // live site so the two never disagree.
        const int tick_side = (int)tick.is_buyer_maker;
        if (drv->rolling) {
            RollingStats_Push(drv->rolling, price_b, volume_b, tick_side);
        }
        if (drv->rolling_long) {
            RollingStats_Push(drv->rolling_long, price_b, volume_b, tick_side);
        }
        // Track E.1 — push v4.3 state at the same cadence EngineSharded_Run's
        // producer slow-path block does. Each guarded so callers that don't
        // supply the state get prior behavior.
        if (drv->rolling_medium) {
            RollingStats_Push(drv->rolling_medium, price_b, volume_b, tick_side);
        }
        if (drv->rolling_baseline) {
            RollingStats_Push(drv->rolling_baseline, price_b, volume_b, tick_side);
        }
        if (drv->cumdelta_state) {
            // is_buyer_maker available on Tick<F> per the v4.3 plumbing.
            CumDelta_Push(drv->cumdelta_state, volume_b, tick.is_buyer_maker);
        }
        if (drv->tick_rate_state) {
            TickRate_Push(drv->tick_rate_state, tick.timestamp);
        }
        if (drv->regime_ror && drv->rolling) {
            // Slope-of-slopes feed (mirrors EngineSharded_Run's producer
            // RORRegressor_Push + the legacy PortfolioController sister).
            // RORRegressor takes a
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
            double signed_vol = Money_ToDouble(tick.volume);
            if (tick.is_buyer_maker) signed_vol = -signed_vol;
            FlowState_Push((FlowState*)drv->flow_state,
                            tick.timestamp, signed_vol);
        }
        if (drv->large_trade_state) {
            LargeTradeState_Push(
                (LargeTradeState<F, 1024>*)drv->large_trade_state,
                Money_ToBinary(tick.volume));
        }
        // v4.6 Wave 2 — push current spread into z-score ring. Caller must
        // have updated *drv->current_spread before RunTick.
        if (drv->spread_state && drv->current_spread) {
            SpreadState_Push(
                (SpreadState<F, 1024>*)drv->spread_state,
                *drv->current_spread);
        }
        // v5.12.1.A.1+.2 — publish synthetic tick timestamp to
        // EventLoopState::last_ws_tick_us. Backtest stays on tick.timestamp
        // (deterministic + replay-safe) while LIVE producer (.A.2) switched
        // to local system_clock. Backtest+live mismatch is harmless ONLY
        // when cfg.ws_dead_time_flatten_enabled=0 (the default outside
        // live deployment) — the staleness check returns immediately.
        // Operator MUST NOT enable the flatten gate during backtest.
        if (drv->state) {
            // v5.15.5.B.2 — wrapped in WsHeartbeatTelemetry alignas(64) cluster.
            drv->state->ws_telemetry.last_tick_us.store(tick.timestamp,
                                               std::memory_order_release);
            // v5.12.1.A.2 — backtest also runs CheckWsStaleness for parity
            // with live slow-path call sites. Pass tick.timestamp as
            // now_us (deterministic; matches the field we just published).
            // Gate is inert at default cfg flag = 0; if operator enables
            // it during backtest, gap == 0 → still no flatten (publish
            // and check use same value). Determinism preserved.
            if (drv->config && drv->oms) {
                double current_price = Money_ToDouble(tick.price);
                EventLoop_CheckWsStaleness(drv->state, *drv->config,
                                            current_price,
                                            tick.timestamp);
            }
        }
        // BACKTEST slow-path-cycle via EngineCommon helper per train-serve
        // execution-layer parity (M5 first canonical). Single call to
        // EngineCommon_SlowPathCycleAllCores fans across per-core slow-path body.
        //
        // KEEPS (producer-thread sisters in LIVE; kill_switch + ema_price must
        // fire in BOTH paths since backtest has no producer thread to mirror
        // LIVE's ema_price replication + KillSwitchEvaluate):
        //   - EmaPrice replication (LIVE producer-thread sister)
        //   - KillSwitchEvaluate (LIVE producer-thread sister)
        if (drv->ema_price) {
            EventLoop_UpdateEmaPriceAllCores(drv->state, *drv->ema_price);
        }
        if (drv->config && drv->oms && drv->rolling) {
            // Caller-precompute per v1.6 O2 bytewise-identical math discipline.
            // BookSnapshot per v1.7.3 N-6 9-arg signature (sister-canonical reuse of
            // BookSnapshot<F> from DataStream/BinanceDepth.hpp per feedback_audit_canonical_sister_before_new_infra).
            // Field-mapping verified at v1.7.4: drv->book_imbalance / drv->current_spread /
            // drv->current_mid_price are POINTER types (const FPN_Binary<F>*) requiring deref before
            // assign (v1.7.4 NEW-4 closure); null-check each before deref.
            BookSnapshot<F> depth = BookSnapshot_Init<F>();
            if (drv->book_imbalance)    { depth.imbalance = *drv->book_imbalance; }
            if (drv->current_spread)    { depth.spread    = *drv->current_spread; }
            if (drv->current_mid_price) { depth.mid_price = *drv->current_mid_price; }

            // PARITY-047 — pass the REAL trade side. The LIVE path now sources
            // (price, volume, side) from one seqlock'd tick sample, so both sides
            // of the train/serve boundary feed the same bit here. Before this,
            // LIVE passed a hardcoded 0 while this driver fed the real bit to
            // CumDelta_Push/FlowState_Push below — a divergence on FEAT_CUMDELTA
            // and FEAT_FLOW_10S/1M/5M (M5).
            EngineCommon_SlowPathCycleAllCores<F>(
                *drv->config, *drv->state, *drv->oms,
                tick.price, tick.volume, tick.timestamp,
                (int)tick.is_buyer_maker,
                (uint64_t)tick_index, depth);
        }
        // E.1.3 P2-a (D-440; M5) — the SAME shared compose + BOTH kill evals the live drainer
        // calls at its cycle tail (the global eval lives INSIDE it now — the standalone call
        // this driver used to make is absorbed; single kill authority, both paths).
        // oms is OPTIONAL on this driver (nullptr = no OMS harness) — no OMS, no money/kill.
        if (drv->oms) {
            EngineCommon_ComposeAndKillEval(*drv->state, *drv->oms, *drv->config,
                                            tick.price, (uint64_t)tick_index);
        } else {
            EventLoop_KillSwitchEvaluate(drv->state);   // harness mode keeps the bare eval
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Step the entire engine through a single tick. tick_index is 0-based and is
// used to determine slow-path cadence (firing every slow_path_interval ticks).
//
// The order is load-bearing for determinism — RollingStats first so the
// slow-path rebuild sees fresh stats, then fan-out to cores, then drain
// events, then slow-path on cadence. Pitfall P11.6 covers this ordering
// requirement.
//======================================================================
// [END_FUNCTION]_[ShardedBacktest_RunTick]
//======================================================================

//======================================================================
// [FUNCTION]_[ShardedBacktest_Run]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[whole-stream convenience wrapper — RunTick loop + final drain/OMS-tick/post-fill flush mirroring live's shutdown flush]
//======================================================================
// [CODE]
//======================================================================
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
        // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
        int dc = BITMAP_IS_SET(drv->oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED)
            ? drv->state->registered_count * 2 : drv->state->registered_count;
        OMS_DrainSubmit(drv->oms, dc);
    }
    if (drv->oms) OrderManager_Tick(drv->oms);
    // v4.7.15: drain post-fill in mode 1 to match live's final-flush loop
    // (the shutdown flush in EngineSharded/Run.hpp). Without this, the last
    // tick's FillRecords sit in the OMS buffers and never apply to NodeContexts.
    if (drv->oms &&
        BITMAP_ANY(drv->oms->oms_state_flags, tt::MASK_OMS_STATE_EVENT_LOG_MODE) &&
        drv->config) {
        // E.1.2.C leg 0 — through the SHARED binder (was a hand-rolled 4-arg
        // call that silently defaulted drift/ic_variant/node_cfg; live-parity
        // by construction now, incl. the exit-bandit flag + per-node fee).
        EngineCommon_DrainPostFill(*drv->state, *drv->oms, *drv->config);
    }
    // D-444 / I-1 MED-4 — FINAL apply: without it, fills booked by this flush would sit
    // in the rings unapplied (a conservation break the moment the apply is ledger-bearing;
    // live's shutdown sister gets this via its final ComposeAndKillEval).
    if (drv->oms) {
        (void)EngineCommon_FillRingsApply(*drv->state, *drv->oms);
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Convenience wrapper that runs a whole tick stream end-to-end and does the
// final event drain. For tests that just want to feed an array and check the
// outcome.
//======================================================================
// [END_FUNCTION]_[ShardedBacktest_Run]
//======================================================================

}  // namespace tt
