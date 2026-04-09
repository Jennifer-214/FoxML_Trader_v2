// test_event_log_head_to_head.cpp — phase 03 chunk 5
//
// Runs the same synthetic tick stream through the sharded backtest driver
// in both mode 0 (legacy) and mode 1 (event log), then asserts byte-identical
// final balance + portfolio state. This is the acceptance criterion for phase 03.
//
// Mode 0 (legacy): EventLoop_OnEvent mutates portfolio + balance directly.
//   OrderManager_Submit short-circuits (paper mode, mode 0), OMS_Tick is a no-op.
//
// Mode 1 (event log): OnEvent just bumps counters, but to get portfolio mutation
//   we need to route each TradeEvent through OrderManager_Submit so the OMS fill
//   handler runs inside OrderManager_Tick. This is the same pattern the production
//   drainer in EngineSharded.hpp uses — open-coded drain with a Submit call after
//   each event. The test uses a custom DrainEventsWithOMS helper to replicate that.
//
// The tick stream is a sawtooth around $60k that triggers SimpleDip entries and
// exits. slow_path_interval=8 so strategy parameters get rebuilt often enough
// for trades to fire within 2000 ticks.

#include "CoreFrameworks/ControllerEventLoop.hpp"
#include "CoreFrameworks/ExecutionCore.hpp"
#include "CoreFrameworks/OrderManager.hpp"
#include "CoreFrameworks/ShardedBacktestDriver.hpp"
#include "CoreFrameworks/Tick.hpp"
#include "ML_Headers/RollingStats.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace tt;

static int failures = 0;
static int assertions = 0;

#define EXPECT(cond, msg)                                                       \
    do {                                                                        \
        ++assertions;                                                           \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);     \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

//======================================================================================================
// [DRAIN WITH OMS SUBMIT — mode 1 backtest drain pattern]
//======================================================================================================
// Mirrors the open-coded drain in EngineSharded.hpp. For each TradeEvent popped
// from a core's event ring:
//   1. Snapshot qty BEFORE OnEvent (CloseSlot clears it on exit)
//   2. Call OnEvent (mode 0: full portfolio mutation; mode 1: just counter bump)
//   3. If mode 1: call OrderManager_Submit so the fill handler runs in OMS_Tick
//
// This is the integration bridge that makes mode 1 work in the backtest path.
// The production ShardedBacktestDriver doesn't have this yet — it only does
// DrainEvents + OMS_Tick. This test proves the pattern works and that mode 1
// produces identical results to mode 0 when the Submit calls are present.
//======================================================================================================
template <unsigned F>
static int DrainEventsWithOMS(EventLoopState<F>* state, OrderManagerState<F>* oms) {
    int total_drained = 0;
    for (int slot = 0; slot < state->registered_count; ++slot) {
        ExecutionCore<F>* core = state->cores[slot].core;
        if (core == nullptr) continue;

        for (int i = 0; i < MAX_EVENTS_PER_DRAIN_PER_CORE; ++i) {
            TradeEvent<F> event;
            if (!SPSCRing_TryPop(&core->event_ring, &event)) break;

            bool is_entry = (event.type & TRADE_EVENT_ENTRY) != 0;
            bool is_exit  = (event.type & TRADE_EVENT_EXIT)  != 0;

            // snapshot exit qty BEFORE OnEvent because CloseSlot clears it
            FPN<F> order_qty = FPN_Zero<F>();
            if (is_exit) {
                order_qty = state->oms->portfolio.positions[slot].quantity;
            } else if (is_entry) {
                order_qty = state->cores[slot].intended_qty;
            }

            EventLoop_OnEvent(state, event);
            ++total_drained;

            // Mode 1: route through OMS so the fill handler runs
            if (oms->event_log_mode == 1 && (is_entry || is_exit)) {
                if (!FPN_IsZero(order_qty)) {
                    OrderManager_Submit(oms,
                        (int16_t)slot,
                        is_entry ? ORDER_MARKET_BUY : ORDER_MARKET_SELL,
                        order_qty,
                        state->cores[slot].intended_tp,
                        state->cores[slot].intended_sl,
                        state->cores[slot].strategy_id,
                        event.price);
                }
            }
        }
    }
    return total_drained;
}

//======================================================================================================
// [SYNTHETIC TICK STREAM]
//======================================================================================================
// Sawtooth around $60000 with a $500 amplitude. The dip phase creates SimpleDip
// entry triggers; the recovery phase crosses TP/SL thresholds so exits fire.
// With tp_pct=0.001 (0.1%) TP is ~$60 above entry, well within the $500 swing.
// With sl_pct=0.003 (0.3%) SL is ~$180 below entry, also within range.
// 2000 ticks with slow_path_interval=8 gives 250 slow path runs.
//======================================================================================================
static std::vector<Tick<64>> make_tick_stream(int num_ticks) {
    std::vector<Tick<64>> ticks;
    ticks.reserve(num_ticks);
    for (int i = 0; i < num_ticks; ++i) {
        Tick<64> t;
        memset(&t, 0, sizeof(t));
        // Triangle wave: ramp up 250 ticks ($60000 -> $60500), ramp down 250 ticks.
        // Period = 500 ticks, amplitude = $500.
        double phase = (double)(i % 500);
        double price;
        if (phase < 250.0) {
            price = 60000.0 + phase * 2.0;  // ramp up to $60500
        } else {
            price = 60000.0 + (500.0 - phase) * 2.0;  // ramp down to $60000
        }
        t.price     = FPN_FromDouble<64>(price);
        t.volume    = FPN_FromDouble<64>(2000.0);
        t.timestamp = (uint64_t)(i * 1000);
        t.sequence  = (uint64_t)i;
        ticks.push_back(t);
    }
    return ticks;
}

//======================================================================================================
// [RUN MODE]
//======================================================================================================
// Constructs a full engine stack (OMS, EventLoopState, 2 execution cores, rolling
// stats, ShardedBacktestDriver) and runs the tick stream through it. The event_log_mode
// parameter controls whether mode 0 (legacy) or mode 1 (event log) is used.
//
// For mode 0: uses ShardedBacktest_Run directly (EventLoop_DrainEvents does portfolio
// mutation inside OnEvent, OMS_Tick is a no-op because Submit short-circuited).
//
// For mode 1: uses a custom per-tick loop that calls DrainEventsWithOMS instead of
// the built-in DrainEvents, so Submit routes fills through the OMS fill handler.
//
// Output parameters because OrderManagerState contains std::atomic fields
// (non-movable). Caller provides pre-allocated OMS + state on the stack or as
// statics.
//======================================================================================================
static void run_mode(int event_log_mode, const std::vector<Tick<64>>& ticks,
                     const ControllerConfig<64>& cfg,
                     OrderManagerState<64>* oms_out,
                     EventLoopState<64>* state_out) {
    // 1. Construct OMS with the specified event_log_mode
    ExchangeAdapter<64> empty{};
    OrderManager_Init(oms_out, empty, /*live_trading=*/0,
                      cfg.starting_balance, cfg.fee_rate, event_log_mode);

    // 2. Construct EventLoopState
    EventLoopState_Init(state_out, oms_out);

    // 3. Set up execution cores (2 cores, SimpleDip). These are function-local
    //    statics because ExecutionCore is ~66KB and would blow the stack. Each
    //    call reinitializes them.
    static SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_rings[2];
    static ExecutionCore<64> cores[2];
    for (int i = 0; i < 2; ++i) {
        SPSCRing_Init(&tick_rings[i]);
        ExecutionCore_Init(&cores[i], (uint16_t)i, &tick_rings[i]);
        EventLoopState_RegisterCore(state_out, &cores[i],
            FPN_Zero<64>(), FPN_Zero<64>(), FPN_Zero<64>());
        EventLoopState_SetCoreStrategy(state_out, i,
            (uint8_t)STRATEGY_SIMPLE_DIP,
            FPN_FromDouble<64>(5000.0));
        ExecutionCore_SetPermission(&cores[i], 1);
    }

    // 4. Rolling stats
    static RollingStats<64, 128> rolling;
    static RollingStats<64, 512> rolling_long;
    rolling = RollingStats_Init<64, 128>();
    rolling_long = RollingStats_Init<64, 512>();

    // 5. Build driver
    int slow_path_interval = 8;
    ShardedBacktestDriver<64> drv;
    ShardedBacktestDriver_Init(&drv, state_out, &rolling, &cfg, slow_path_interval,
                               &rolling_long, oms_out);

    // 6. Run ticks
    int num_ticks = (int)ticks.size();

    if (event_log_mode == 0) {
        // Mode 0: use the standard driver directly. OnEvent does portfolio
        // mutation, OMS_Tick is a no-op (paper mode short-circuits Submit).
        ShardedBacktest_Run(&drv, ticks.data(), num_ticks);
    } else {
        // Mode 1: open-coded per-tick loop with DrainEventsWithOMS so that
        // each TradeEvent gets routed through OrderManager_Submit and the
        // fill handler in OMS_Tick does portfolio mutation.
        for (int i = 0; i < num_ticks; ++i) {
            const Tick<64>& tick = ticks[i];

            // 6a. Fan out to every registered execution core
            for (int slot = 0; slot < state_out->registered_count; ++slot) {
                ExecutionCore<64>* core = state_out->cores[slot].core;
                if (core) ExecutionCore_Tick(core, tick);
            }

            // 6b. Drain events with OMS submit integration
            DrainEventsWithOMS(state_out, oms_out);

            // 6c. OMS Tick — process the fills that Submit just enqueued
            OrderManager_Tick(oms_out);

            // 6d. Slow path on cadence (same logic as ShardedBacktestDriver)
            if (((i + 1) % slow_path_interval) == 0) {
                RollingStats_Push(&rolling, tick.price, tick.volume);
                RollingStats_Push(&rolling_long, tick.price, tick.volume);
                EventLoop_RebuildAllParameters(state_out, &rolling, &cfg, &rolling_long);
                EventLoop_PushParameters(state_out);
                EventLoop_KillSwitchEvaluate(state_out);
            }
        }
        // Final drain
        DrainEventsWithOMS(state_out, oms_out);
        OrderManager_Tick(oms_out);
    }
}

//======================================================================================================
// [COMPARISON HELPERS]
//======================================================================================================
static void compare_positions(const Portfolio<64>& p0, const Portfolio<64>& p1, int slot) {
    char msg[128];
    snprintf(msg, sizeof(msg), "slot %d entry_price match", slot);
    EXPECT(FPN_Equal(p0.positions[slot].entry_price, p1.positions[slot].entry_price), msg);
    snprintf(msg, sizeof(msg), "slot %d quantity match", slot);
    EXPECT(FPN_Equal(p0.positions[slot].quantity, p1.positions[slot].quantity), msg);
    snprintf(msg, sizeof(msg), "slot %d take_profit_price match", slot);
    EXPECT(FPN_Equal(p0.positions[slot].take_profit_price, p1.positions[slot].take_profit_price), msg);
    snprintf(msg, sizeof(msg), "slot %d stop_loss_price match", slot);
    EXPECT(FPN_Equal(p0.positions[slot].stop_loss_price, p1.positions[slot].stop_loss_price), msg);
}

//======================================================================================================
// [TEST: head-to-head mode 0 vs mode 1]
//======================================================================================================
static void test_head_to_head() {
    printf("--- head-to-head: mode 0 (legacy) vs mode 1 (event log) ---\n\n");

    // Build config
    ControllerConfig<64> cfg = ControllerConfig_Default<64>();
    cfg.starting_balance  = FPN_FromDouble<64>(10000.0);
    cfg.fee_rate          = FPN_FromDouble<64>(0.001);
    cfg.entry_offset_pct  = FPN_FromDouble<64>(0.002);   // 0.2% dip to trigger entry
    cfg.volume_multiplier = FPN_FromDouble<64>(0.5);     // forgiving so vol gate passes
    cfg.take_profit_pct   = FPN_FromDouble<64>(0.001);   // 0.1% TP (~$60 above entry)
    cfg.stop_loss_pct     = FPN_FromDouble<64>(0.003);   // 0.3% SL (~$180 below entry)

    // Build tick stream (shared between both runs)
    constexpr int NUM_TICKS = 2000;
    auto ticks = make_tick_stream(NUM_TICKS);

    // OMS and state are statics because OrderManagerState contains atomics
    // (non-movable) and SPSCRing (large). Separate sets for each mode.
    static OrderManagerState<64> oms0, oms1;
    static EventLoopState<64> state0, state1;

    // Run mode 0 (legacy)
    printf("  running mode 0 (legacy)...\n");
    run_mode(0, ticks, cfg, &oms0, &state0);

    // Run mode 1 (event log)
    printf("  running mode 1 (event log)...\n");
    run_mode(1, ticks, cfg, &oms1, &state1);

    // Report what happened
    printf("\n  mode 0: entries=%llu exits=%llu balance=%.6f rpnl=%.6f\n",
           (unsigned long long)state0.total_entries,
           (unsigned long long)state0.total_exits,
           FPN_ToDouble(oms0.balance),
           FPN_ToDouble(oms0.realized_pnl));
    printf("  mode 1: entries=%llu exits=%llu balance=%.6f rpnl=%.6f\n",
           (unsigned long long)state1.total_entries,
           (unsigned long long)state1.total_exits,
           FPN_ToDouble(oms1.balance),
           FPN_ToDouble(oms1.realized_pnl));

    // Sanity: both modes must have actually traded
    EXPECT(state0.total_entries > 0, "mode 0 produced entries");
    EXPECT(state0.total_exits > 0,  "mode 0 produced exits");
    EXPECT(state1.total_entries > 0, "mode 1 produced entries");
    EXPECT(state1.total_exits > 0,  "mode 1 produced exits");

    // Headline assertion: FPN_Equal (bit-identical), not approx
    EXPECT(FPN_Equal(oms0.balance, oms1.balance),
           "BALANCE: mode 0 and mode 1 byte-identical");
    EXPECT(FPN_Equal(oms0.realized_pnl, oms1.realized_pnl),
           "REALIZED P&L: mode 0 and mode 1 byte-identical");

    // Portfolio state
    EXPECT(oms0.portfolio.active_bitmap == oms1.portfolio.active_bitmap,
           "active_bitmap match");

    // Per-position comparison for any active slots
    uint16_t active = oms0.portfolio.active_bitmap | oms1.portfolio.active_bitmap;
    while (active) {
        int slot = __builtin_ctz(active);
        compare_positions(oms0.portfolio, oms1.portfolio, slot);
        active &= active - 1;
    }

    // Event counters
    EXPECT(state0.total_entries == state1.total_entries,
           "total_entries match");
    EXPECT(state0.total_exits == state1.total_exits,
           "total_exits match");

    // Mode 1 specific: event log was populated
    EXPECT(oms1.event_log.count > 0,
           "mode 1 event log populated");
    // Mode 0 should NOT have populated the event log
    EXPECT(oms0.event_log.count == 0,
           "mode 0 event log empty");

    printf("\n  event log: %zu events recorded in mode 1\n",
           oms1.event_log.count);

    // Cross-check: fold the event log and verify it matches the live state
    FoldResult<64> fold = Portfolio_FromEventLog(&oms1.event_log,
                                                 cfg.starting_balance,
                                                 cfg.fee_rate);
    EXPECT(FPN_Equal(fold.balance, oms1.balance),
           "event log fold balance matches live mode 1 balance");
    EXPECT(FPN_Equal(fold.realized_pnl, oms1.realized_pnl),
           "event log fold realized_pnl matches live mode 1 realized_pnl");
    EXPECT(fold.portfolio.active_bitmap == oms1.portfolio.active_bitmap,
           "event log fold active_bitmap matches live mode 1");

    printf("  event log fold: balance=%.6f rpnl=%.6f fills=%d (cross-check passed)\n",
           FPN_ToDouble(fold.balance),
           FPN_ToDouble(fold.realized_pnl),
           fold.fills_processed);

    // Cleanup event log allocations
    OrderManager_Shutdown(&oms0);
    OrderManager_Shutdown(&oms1);
}

//======================================================================================================
// [TEST: determinism — run mode 1 twice, identical results]
//======================================================================================================
static void test_mode1_determinism() {
    printf("\n--- determinism: mode 1 run twice ---\n\n");

    ControllerConfig<64> cfg = ControllerConfig_Default<64>();
    cfg.starting_balance  = FPN_FromDouble<64>(10000.0);
    cfg.fee_rate          = FPN_FromDouble<64>(0.001);
    cfg.entry_offset_pct  = FPN_FromDouble<64>(0.002);
    cfg.volume_multiplier = FPN_FromDouble<64>(0.5);
    cfg.take_profit_pct   = FPN_FromDouble<64>(0.001);
    cfg.stop_loss_pct     = FPN_FromDouble<64>(0.003);

    auto ticks = make_tick_stream(2000);

    static OrderManagerState<64> oms_a, oms_b;
    static EventLoopState<64> state_a, state_b;

    run_mode(1, ticks, cfg, &oms_a, &state_a);
    run_mode(1, ticks, cfg, &oms_b, &state_b);

    EXPECT(FPN_Equal(oms_a.balance, oms_b.balance),
           "mode 1 deterministic: balance identical across runs");
    EXPECT(FPN_Equal(oms_a.realized_pnl, oms_b.realized_pnl),
           "mode 1 deterministic: realized_pnl identical across runs");
    EXPECT(state_a.total_entries == state_b.total_entries,
           "mode 1 deterministic: total_entries identical");
    EXPECT(state_a.total_exits == state_b.total_exits,
           "mode 1 deterministic: total_exits identical");
    EXPECT(oms_a.event_log.count == oms_b.event_log.count,
           "mode 1 deterministic: event log count identical");

    printf("  run A: entries=%llu exits=%llu balance=%.6f\n",
           (unsigned long long)state_a.total_entries,
           (unsigned long long)state_a.total_exits,
           FPN_ToDouble(oms_a.balance));
    printf("  run B: entries=%llu exits=%llu balance=%.6f\n",
           (unsigned long long)state_b.total_entries,
           (unsigned long long)state_b.total_exits,
           FPN_ToDouble(oms_b.balance));

    OrderManager_Shutdown(&oms_a);
    OrderManager_Shutdown(&oms_b);
}

//======================================================================================================
// [MAIN]
//======================================================================================================
int main() {
    printf("=== Event log head-to-head test (phase 03 chunk 5) ===\n\n");

    test_head_to_head();
    test_mode1_determinism();

    printf("\n=== %s (%d assertions, %d failures) ===\n",
           failures == 0 ? "PASS" : "FAIL",
           assertions, failures);
    return failures == 0 ? 0 : 1;
}
