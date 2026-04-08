// test_sharded_backtest.cpp — phase 11 functional tests
//
// Validates the ShardedBacktestDriver against synthetic tick streams. The
// driver is the single-threaded replay loop that phase 13 will plug into the
// production BacktestEngine, so these tests are the architectural acceptance
// gate for that integration.
//
//   1. Empty tick stream → no crash, no trades
//   2. Tick stream with no strategy → no trades (P9.4 / P11.7 graceful warmup)
//   3. Tick stream with SimpleDip strategy → entries fire on dips
//   4. Slow path cadence — interval=64 ticks, 256 ticks → 4 slow path runs
//   5. Determinism — run same stream twice, identical balance + trade count
//   6. Multi-core fan out — 3 cores all see the same tick stream
//   7. Kill switch trip mid-run blocks new entries; in-flight exits still flow
//   8. Final drain catches the last tick's events

#include "CoreFrameworks/ControllerEventLoop.hpp"
#include "CoreFrameworks/ExecutionCore.hpp"
#include "CoreFrameworks/ShardedBacktestDriver.hpp"
#include "CoreFrameworks/Tick.hpp"
#include "ML_Headers/RollingStats.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace tt;

static int failures = 0;

#define EXPECT(cond, msg)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);     \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

// Build a synthetic tick stream that ramps up then dips. The dip creates a
// SimpleDip entry trigger; the recovery creates an exit trigger.
static std::vector<Tick<64>> make_dip_stream(int num_ticks, double base_price) {
    std::vector<Tick<64>> ticks;
    ticks.reserve(num_ticks);
    for (int i = 0; i < num_ticks; ++i) {
        Tick<64> t;
        memset(&t, 0, sizeof(t));
        // Sawtooth: ramp up over 50 ticks, dip back down 25 ticks, repeat.
        double phase = (double)(i % 75);
        double price = base_price;
        if (phase < 50.0) {
            price = base_price + phase * 2.0;  // ramp up
        } else {
            price = base_price + (50.0 - (phase - 50.0)) * 2.0;  // dip down
        }
        t.price = FPN_FromDouble<64>(price);
        t.volume = FPN_FromDouble<64>(2000.0);  // high enough to clear vol gate
        t.timestamp = (uint64_t)(i * 1000);  // 1ms between ticks
        t.sequence = (uint64_t)i;
        ticks.push_back(t);
    }
    return ticks;
}

//======================================================================================================
// test 1: empty stream is a no-op
//======================================================================================================
static void test_empty_stream() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    ShardedBacktestDriver<64> drv;
    ShardedBacktestDriver_Init(&drv, &state, (RollingStats<64>*)nullptr,
                                (const ControllerConfig<64>*)nullptr, 64);

    ShardedBacktest_Run(&drv, (Tick<64>*)nullptr, 0);

    EXPECT(state.total_entries == 0, "no entries on empty stream");
    EXPECT(state.total_exits == 0, "no exits on empty stream");
    EXPECT(drv.slow_path_runs == 0, "no slow path runs");
}

//======================================================================================================
// test 2: cores with no strategy assigned → no trades
//======================================================================================================
static void test_no_strategy_no_trades() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0), FPN_FromDouble<64>(0.01));
    // No SetCoreStrategy call → strategy stays at STRATEGY_NONE
    // Permission stays at 0 because we never armed it.

    auto rolling = RollingStats_Init<64, 128>();
    ControllerConfig<64> config = ControllerConfig_Default<64>();

    ShardedBacktestDriver<64> drv;
    ShardedBacktestDriver_Init(&drv, &state, &rolling, &config, 64);

    auto ticks = make_dip_stream(200, 60000.0);
    ShardedBacktest_Run(&drv, ticks.data(), (int)ticks.size());

    EXPECT(state.total_entries == 0, "NONE strategy → no entries");
    EXPECT(state.total_exits == 0, "NONE strategy → no exits");
    EXPECT(drv.slow_path_runs > 0, "slow path still ran");
}

//======================================================================================================
// test 3: SimpleDip strategy entries fire on the dip half of the sawtooth
//======================================================================================================
static void test_simpledip_trades() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0), FPN_FromDouble<64>(0.01));
    EventLoopState_SetCoreStrategy(&state, slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));

    // Arm permission so the core can take entries once parameters are pushed.
    ExecutionCore_SetPermission(&core, 1);

    auto rolling = RollingStats_Init<64, 128>();
    ControllerConfig<64> config = ControllerConfig_Default<64>();
    config.entry_offset_pct  = FPN_FromDouble<64>(0.001);
    config.volume_multiplier = FPN_FromDouble<64>(0.5);  // forgiving so vol gate passes
    config.take_profit_pct   = FPN_FromDouble<64>(0.005);
    config.stop_loss_pct     = FPN_FromDouble<64>(0.0025);

    ShardedBacktestDriver<64> drv;
    ShardedBacktestDriver_Init(&drv, &state, &rolling, &config, 64);

    auto ticks = make_dip_stream(500, 60000.0);
    ShardedBacktest_Run(&drv, ticks.data(), (int)ticks.size());

    EXPECT(state.total_entries > 0, "SimpleDip produced entries");
    EXPECT(drv.slow_path_runs >= 7, "slow path fired multiple times (500/64 ≈ 7)");
}

//======================================================================================================
// test 4: slow path cadence is exactly tick_count / interval
//======================================================================================================
static void test_slow_path_cadence() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    auto rolling = RollingStats_Init<64, 128>();
    ControllerConfig<64> config = ControllerConfig_Default<64>();

    ShardedBacktestDriver<64> drv;
    ShardedBacktestDriver_Init(&drv, &state, &rolling, &config, 64);

    auto ticks = make_dip_stream(256, 60000.0);
    ShardedBacktest_Run(&drv, ticks.data(), (int)ticks.size());

    // 256 ticks / 64 = 4 slow path runs (at ticks 63, 127, 191, 255).
    EXPECT(drv.slow_path_runs == 4, "exactly 4 slow path runs at interval 64");
}

//======================================================================================================
// test 5: determinism — run twice, identical results
//======================================================================================================
static void test_determinism() {
    auto run_one = [](double& balance_out, uint64_t& entries_out, uint64_t& exits_out) {
        EventLoopState<64> state;
        EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

        SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
        SPSCRing_Init(&tr);
        ExecutionCore<64> core;
        ExecutionCore_Init(&core, 0, &tr);
        int slot = EventLoopState_RegisterCore(&state, &core,
            FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0), FPN_FromDouble<64>(0.01));
        EventLoopState_SetCoreStrategy(&state, slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));
        ExecutionCore_SetPermission(&core, 1);

        auto rolling = RollingStats_Init<64, 128>();
        ControllerConfig<64> config = ControllerConfig_Default<64>();
        config.entry_offset_pct  = FPN_FromDouble<64>(0.001);
        config.volume_multiplier = FPN_FromDouble<64>(0.5);
        config.take_profit_pct   = FPN_FromDouble<64>(0.005);
        config.stop_loss_pct     = FPN_FromDouble<64>(0.0025);

        ShardedBacktestDriver<64> drv;
        ShardedBacktestDriver_Init(&drv, &state, &rolling, &config, 64);

        auto ticks = make_dip_stream(500, 60000.0);
        ShardedBacktest_Run(&drv, ticks.data(), (int)ticks.size());

        balance_out = FPN_ToDouble(state.balance);
        entries_out = state.total_entries;
        exits_out   = state.total_exits;
    };

    double bal1 = 0, bal2 = 0;
    uint64_t e1 = 0, e2 = 0;
    uint64_t x1 = 0, x2 = 0;

    run_one(bal1, e1, x1);
    run_one(bal2, e2, x2);

    EXPECT(bal1 == bal2, "identical balance across runs");
    EXPECT(e1 == e2, "identical entry count");
    EXPECT(x1 == x2, "identical exit count");
}

//======================================================================================================
// test 6: multi-core fan out — 3 cores all see the same tick stream
//======================================================================================================
static void test_multi_core_fan_out() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> rings[3];
    ExecutionCore<64> cores[3];
    for (int i = 0; i < 3; ++i) {
        SPSCRing_Init(&rings[i]);
        ExecutionCore_Init(&cores[i], 0, &rings[i]);
        EventLoopState_RegisterCore(&state, &cores[i],
            FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0), FPN_FromDouble<64>(0.01));
        EventLoopState_SetCoreStrategy(&state, i, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));
        ExecutionCore_SetPermission(&cores[i], 1);
    }

    auto rolling = RollingStats_Init<64, 128>();
    ControllerConfig<64> config = ControllerConfig_Default<64>();
    config.entry_offset_pct  = FPN_FromDouble<64>(0.001);
    config.volume_multiplier = FPN_FromDouble<64>(0.5);
    config.take_profit_pct   = FPN_FromDouble<64>(0.005);
    config.stop_loss_pct     = FPN_FromDouble<64>(0.0025);

    ShardedBacktestDriver<64> drv;
    ShardedBacktestDriver_Init(&drv, &state, &rolling, &config, 64);

    auto ticks = make_dip_stream(500, 60000.0);
    ShardedBacktest_Run(&drv, ticks.data(), (int)ticks.size());

    // Each core gets the SAME tick stream and SAME parameters, so each
    // should fire roughly the same number of entries. Sanity-check that all
    // three cores produced events (none was starved or skipped).
    for (int i = 0; i < 3; ++i) {
        EXPECT(state.cores[i].entries_processed > 0, "each core produced entries");
    }
    EXPECT(state.total_entries >= 3, "aggregate count includes all cores");
}

//======================================================================================================
// test 7: kill switch trip mid-run blocks new entries
//======================================================================================================
static void test_kill_switch_mid_run() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0), FPN_FromDouble<64>(0.01));
    EventLoopState_SetCoreStrategy(&state, slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));
    ExecutionCore_SetPermission(&core, 1);

    auto rolling = RollingStats_Init<64, 128>();
    ControllerConfig<64> config = ControllerConfig_Default<64>();
    config.entry_offset_pct  = FPN_FromDouble<64>(0.001);
    config.volume_multiplier = FPN_FromDouble<64>(0.5);
    config.take_profit_pct   = FPN_FromDouble<64>(0.005);
    config.stop_loss_pct     = FPN_FromDouble<64>(0.0025);

    ShardedBacktestDriver<64> drv;
    ShardedBacktestDriver_Init(&drv, &state, &rolling, &config, 64);

    auto ticks = make_dip_stream(1000, 60000.0);

    // Run the first half normally
    for (int i = 0; i < 500; ++i) {
        ShardedBacktest_RunTick(&drv, ticks[i], i);
    }
    uint64_t entries_before = state.total_entries;
    EXPECT(entries_before > 0, "entries fired in first half");

    // Trip the kill switch
    EventLoop_KillSwitchTrip(&state);

    // Run the second half — no NEW entries should fire (permission cleared),
    // but any already-open positions can still exit via SG.
    for (int i = 500; i < 1000; ++i) {
        ShardedBacktest_RunTick(&drv, ticks[i], i);
    }
    EventLoop_DrainEvents(&state);

    EXPECT(state.kill_switch_tripped == 1, "kill switch still tripped");
    // Entries after trip should equal entries before trip — no new ones.
    EXPECT(state.total_entries == entries_before,
           "no new entries after kill switch trip");
}

//======================================================================================================
// test 8: final drain catches the last tick's events
//======================================================================================================
static void test_final_drain() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0), FPN_FromDouble<64>(0.01));
    EventLoopState_SetCoreStrategy(&state, slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));
    ExecutionCore_SetPermission(&core, 1);

    auto rolling = RollingStats_Init<64, 128>();
    ControllerConfig<64> config = ControllerConfig_Default<64>();
    config.entry_offset_pct  = FPN_FromDouble<64>(0.001);
    config.volume_multiplier = FPN_FromDouble<64>(0.5);
    config.take_profit_pct   = FPN_FromDouble<64>(0.005);
    config.stop_loss_pct     = FPN_FromDouble<64>(0.0025);

    ShardedBacktestDriver<64> drv;
    // slow_path_interval intentionally HUGE so the slow path never runs
    // and the only drain is the per-tick + final.
    ShardedBacktestDriver_Init(&drv, &state, &rolling, &config, 1000000);

    auto ticks = make_dip_stream(500, 60000.0);
    ShardedBacktest_Run(&drv, ticks.data(), (int)ticks.size());

    // Even with no slow path runs, all events from in-stream ticks should
    // have been drained either inline (per tick) or by the final drain.
    EXPECT(SPSCRing_Depth(&core.event_ring) == 0,
           "event ring fully drained at end of stream");
    EXPECT(drv.slow_path_runs == 0, "slow path never ran (interval too large)");
}

//======================================================================================================
// main
//======================================================================================================
int main() {
    printf("=== Sharded backtest driver tests ===\n\n");

    test_empty_stream();
    printf("empty_stream                      ok\n");
    test_no_strategy_no_trades();
    printf("no_strategy_no_trades             ok\n");
    test_simpledip_trades();
    printf("simpledip_trades                  ok\n");
    test_slow_path_cadence();
    printf("slow_path_cadence                 ok\n");
    test_determinism();
    printf("determinism                       ok\n");
    test_multi_core_fan_out();
    printf("multi_core_fan_out                ok\n");
    test_kill_switch_mid_run();
    printf("kill_switch_mid_run               ok\n");
    test_final_drain();
    printf("final_drain                       ok\n");

    printf("\n=== %s (%d failures) ===\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
