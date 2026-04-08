// test_strategy_parameters.cpp — phase 06 functional tests
//
// Validates the strategy → GateParameters pipeline:
//
//   1. SimpleDip_BuildParameters produces expected gate values from synthetic
//      RollingStats + ControllerConfig
//   2. Stub strategies (MR, Momentum, EmaCross) produce sensible non-zero
//      packs with correct strategy_id
//   3. Strategy_BuildParameters dispatcher routes by strategy_id
//   4. STRATEGY_NONE returns a safe zero pack
//   5. EventLoop_RebuildAllParameters fills pending_params for all registered
//      cores and marks them dirty
//   6. End-to-end: register cores, set strategies, push synthetic ticks into
//      RollingStats, rebuild, push parameters, verify execution cores see
//      the correct gate thresholds via ParameterSlot_Read

#include "CoreFrameworks/ControllerConfig.hpp"
#include "CoreFrameworks/ControllerEventLoop.hpp"
#include "CoreFrameworks/ExecutionCore.hpp"
#include "CoreFrameworks/Tick.hpp"
#include "ML_Headers/RollingStats.hpp"
#include "Strategies/StrategyParameters.hpp"

#include <cstdio>

using namespace tt;

static int failures = 0;

#define EXPECT(cond, msg)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);     \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

// Helper: float comparison with small tolerance for FPN round-trip
static bool approx(double a, double b, double tol = 1e-3) {
    double d = a - b;
    return d < tol && d > -tol;
}

// Helper: build a RollingStats with synthetic samples around a target price.
// Pushes 64 samples of (price, volume) so the rolling stats are fully
// populated and price_max / volume_avg / price_avg are deterministic.
static RollingStats<64, 128> make_rolling_stats(double base_price, double volume) {
    auto rs = RollingStats_Init<64, 128>();
    for (int i = 0; i < 64; ++i) {
        // Slight up-and-down so the max is deterministic at base_price + 50
        double p = base_price + (double)((i * 7) % 100) - 50.0;
        if (i == 0) p = base_price + 50.0;  // make sure max hits 50 above base
        RollingStats_Push(&rs, FPN_FromDouble<64>(p), FPN_FromDouble<64>(volume));
    }
    return rs;
}

//======================================================================================================
// test 1: SimpleDip_BuildParameters produces expected gate values
//======================================================================================================
static void test_simpledip_basic() {
    auto rolling = make_rolling_stats(60000.0, 1000.0);
    ControllerConfig<64> config = ControllerConfig_Default<64>();

    // Override config to known values for predictable arithmetic
    config.entry_offset_pct  = FPN_FromDouble<64>(0.001);   // 0.1% dip
    config.volume_multiplier = FPN_FromDouble<64>(2.0);     // 2x avg volume
    config.take_profit_pct   = FPN_FromDouble<64>(0.005);   // 0.5% TP
    config.stop_loss_pct     = FPN_FromDouble<64>(0.0025);  // 0.25% SL

    GateParameters<64> out;
    SimpleDip_BuildParameters(&rolling, &config, FPN_FromDouble<64>(1000.0), &out);

    double recent_high = FPN_ToDouble(rolling.price_max);
    double expected_entry = recent_high * (1.0 - 0.001);
    double expected_tp    = expected_entry * (1.0 + 0.005);
    double expected_sl    = expected_entry * (1.0 - 0.0025);

    EXPECT(approx(FPN_ToDouble(out.bg_price_threshold), expected_entry, 0.01),
           "bg_price_threshold = recent_high * (1 - dip)");
    EXPECT(approx(FPN_ToDouble(out.sg_take_profit_price), expected_tp, 0.01),
           "tp = entry * (1 + tp_pct)");
    EXPECT(approx(FPN_ToDouble(out.sg_stop_loss_price), expected_sl, 0.01),
           "sl = entry * (1 - sl_pct)");
    EXPECT(out.strategy_id == STRATEGY_SIMPLE_DIP, "strategy_id == SIMPLE_DIP");
    EXPECT(out.flags & GATE_FLAG_TP_ENABLED, "TP flag set");
    EXPECT(out.flags & GATE_FLAG_SL_ENABLED, "SL flag set");

    // trade_size = balance / expected_entry. balance is 1000, entry ~60050.
    double expected_qty = 1000.0 / expected_entry;
    EXPECT(approx(FPN_ToDouble(out.trade_size), expected_qty, 0.0001),
           "trade_size = balance / entry");

    // volume threshold = avg * 2.0. The synthetic stream has fixed volume so avg ≈ 1000.
    EXPECT(FPN_ToDouble(out.bg_volume_threshold) > 1500.0,
           "volume threshold > 1500 (2x of ~1000 avg)");
}

//======================================================================================================
// test 2: Strategy_BuildParameters dispatcher routes by id
//======================================================================================================
static void test_dispatcher_routing() {
    auto rolling = make_rolling_stats(60000.0, 1000.0);
    ControllerConfig<64> config = ControllerConfig_Default<64>();
    FPN<64> bal = FPN_FromDouble<64>(500.0);

    GateParameters<64> dip, mr, mom, ema, none;

    Strategy_BuildParameters(STRATEGY_SIMPLE_DIP,    &rolling, &config, bal, &dip);
    Strategy_BuildParameters(STRATEGY_MEAN_REVERSION, &rolling, &config, bal, &mr);
    Strategy_BuildParameters(STRATEGY_MOMENTUM,       &rolling, &config, bal, &mom);
    Strategy_BuildParameters(STRATEGY_EMA_CROSS,      &rolling, &config, bal, &ema);
    Strategy_BuildParameters(STRATEGY_NONE,           &rolling, &config, bal, &none);

    EXPECT(dip.strategy_id == STRATEGY_SIMPLE_DIP,    "dispatcher → SimpleDip");
    EXPECT(mr.strategy_id  == STRATEGY_MEAN_REVERSION, "dispatcher → MeanReversion");
    EXPECT(mom.strategy_id == STRATEGY_MOMENTUM,       "dispatcher → Momentum");
    EXPECT(ema.strategy_id == STRATEGY_EMA_CROSS,      "dispatcher → EmaCross");
    EXPECT(none.strategy_id == STRATEGY_NONE,          "dispatcher → NONE");

    // The non-NONE strategies should produce non-zero gate thresholds
    EXPECT(!FPN_IsZero(dip.bg_price_threshold), "SimpleDip produces non-zero gates");
    EXPECT(!FPN_IsZero(mr.bg_price_threshold),  "MR stub produces non-zero gates");
    EXPECT(!FPN_IsZero(mom.bg_price_threshold), "Momentum stub produces non-zero gates");
    EXPECT(!FPN_IsZero(ema.bg_price_threshold), "EmaCross stub produces non-zero gates");
    EXPECT(FPN_IsZero(none.bg_price_threshold), "NONE produces zero gates");
}

//======================================================================================================
// test 3: STRATEGY_NONE produces a zero pack with no permission to trade
//======================================================================================================
static void test_strategy_none() {
    auto rolling = make_rolling_stats(60000.0, 1000.0);
    ControllerConfig<64> config = ControllerConfig_Default<64>();

    GateParameters<64> out;
    Strategy_BuildParameters(STRATEGY_NONE, &rolling, &config, FPN_FromDouble<64>(1000.0), &out);

    EXPECT(out.strategy_id == STRATEGY_NONE, "strategy_id is NONE");
    EXPECT(FPN_IsZero(out.bg_price_threshold), "BG threshold is zero");
    EXPECT(FPN_IsZero(out.sg_take_profit_price), "TP price is zero");
    EXPECT(FPN_IsZero(out.sg_stop_loss_price), "SL price is zero");
    EXPECT(out.flags == 0, "no gate flags set");
}

//======================================================================================================
// test 4: EventLoop_RebuildAllParameters fills pending_params for all cores
//======================================================================================================
static void test_rebuild_all_parameters() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr_a, tr_b;
    SPSCRing_Init(&tr_a); SPSCRing_Init(&tr_b);

    ExecutionCore<64> ca, cb;
    ExecutionCore_Init(&ca, 0, &tr_a);
    ExecutionCore_Init(&cb, 0, &tr_b);

    int sa = EventLoopState_RegisterCore(&state, &ca,
        FPN_FromDouble<64>(60100.0), FPN_FromDouble<64>(59900.0), FPN_FromDouble<64>(0.01));
    int sb = EventLoopState_RegisterCore(&state, &cb,
        FPN_FromDouble<64>(60200.0), FPN_FromDouble<64>(59800.0), FPN_FromDouble<64>(0.01));

    // assign different strategies to the two cores
    EventLoopState_SetCoreStrategy(&state, sa, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(2000.0));
    EventLoopState_SetCoreStrategy(&state, sb, STRATEGY_MEAN_REVERSION, FPN_FromDouble<64>(3000.0));

    auto rolling = make_rolling_stats(60000.0, 1000.0);
    ControllerConfig<64> config = ControllerConfig_Default<64>();
    config.entry_offset_pct = FPN_FromDouble<64>(0.001);
    config.take_profit_pct  = FPN_FromDouble<64>(0.005);
    config.stop_loss_pct    = FPN_FromDouble<64>(0.0025);

    int rebuilt = EventLoop_RebuildAllParameters(&state, &rolling, &config);
    EXPECT(rebuilt == 2, "rebuilt both cores");
    EXPECT(state.cores[sa].dirty == 1, "core a marked dirty");
    EXPECT(state.cores[sb].dirty == 1, "core b marked dirty");
    EXPECT(state.cores[sa].pending_params.strategy_id == STRATEGY_SIMPLE_DIP,
           "core a pending = SimpleDip");
    EXPECT(state.cores[sb].pending_params.strategy_id == STRATEGY_MEAN_REVERSION,
           "core b pending = MeanReversion");

    // intended_tp / intended_sl / intended_qty should mirror the pending pack
    EXPECT(FPN_ToDouble(state.cores[sa].intended_tp) ==
           FPN_ToDouble(state.cores[sa].pending_params.sg_take_profit_price),
           "intended_tp mirrors pending");
    EXPECT(FPN_ToDouble(state.cores[sa].intended_qty) ==
           FPN_ToDouble(state.cores[sa].pending_params.trade_size),
           "intended_qty mirrors pending");
}

//======================================================================================================
// test 5: STRATEGY_NONE cores are skipped by RebuildAll
//======================================================================================================
static void test_rebuild_skips_none() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);

    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60100.0), FPN_FromDouble<64>(59900.0), FPN_FromDouble<64>(0.01));
    // Don't set a strategy — it stays at STRATEGY_NONE from init
    EXPECT(state.cores[slot].strategy_id == STRATEGY_NONE, "init defaults to NONE");

    auto rolling = make_rolling_stats(60000.0, 1000.0);
    ControllerConfig<64> config = ControllerConfig_Default<64>();

    int rebuilt = EventLoop_RebuildAllParameters(&state, &rolling, &config);
    EXPECT(rebuilt == 0, "no cores rebuilt (all NONE)");
    EXPECT(state.cores[slot].dirty == 0, "core stayed clean");
}

//======================================================================================================
// test 6: end-to-end pipeline — rebuild, push, verify execution core sees params
//======================================================================================================
static void test_end_to_end_pipeline() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);

    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60100.0), FPN_FromDouble<64>(59900.0), FPN_FromDouble<64>(0.01));
    EventLoopState_SetCoreStrategy(&state, slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));

    auto rolling = make_rolling_stats(60000.0, 1000.0);
    ControllerConfig<64> config = ControllerConfig_Default<64>();
    config.entry_offset_pct = FPN_FromDouble<64>(0.001);
    config.take_profit_pct  = FPN_FromDouble<64>(0.005);
    config.stop_loss_pct    = FPN_FromDouble<64>(0.0025);

    // before rebuild + push, core has init defaults (STRATEGY_NONE)
    GateParameters<64> before;
    ParameterSlot_Read(&core.param_slot, &before);
    EXPECT(before.strategy_id == STRATEGY_NONE, "before pipeline: core has NONE");

    // rebuild all + push
    int rebuilt = EventLoop_RebuildAllParameters(&state, &rolling, &config);
    EXPECT(rebuilt == 1, "1 core rebuilt");
    int pushed = EventLoop_PushParameters(&state);
    EXPECT(pushed == 1, "1 core pushed");

    // now the execution core sees the freshly built pack
    GateParameters<64> after;
    ParameterSlot_Read(&core.param_slot, &after);
    EXPECT(after.strategy_id == STRATEGY_SIMPLE_DIP, "after pipeline: core has SimpleDip");
    EXPECT(!FPN_IsZero(after.bg_price_threshold), "BG threshold is set");
    EXPECT(!FPN_IsZero(after.sg_take_profit_price), "TP price is set");
    EXPECT(!FPN_IsZero(after.sg_stop_loss_price), "SL price is set");

    // arm the core and feed it a tick that triggers BG
    core.permission = 1;
    Tick<64> dip_tick;
    dip_tick.price = FPN_FromDouble<64>(50.0);  // way below threshold, definitely fires
    dip_tick.volume = FPN_FromDouble<64>(99999.0);  // way above volume gate
    dip_tick.timestamp = 1'000'000;
    dip_tick.sequence = 1;

    ExecutionCore_Tick(&core, dip_tick);
    EXPECT(core.active == 1, "BG fired and core is in trade");
    EXPECT(SPSCRing_Depth(&core.event_ring) == 1, "entry event pushed");
}

//======================================================================================================
// test 7: dispatcher with unknown strategy id falls through to safe default
//======================================================================================================
static void test_unknown_strategy_id() {
    auto rolling = make_rolling_stats(60000.0, 1000.0);
    ControllerConfig<64> config = ControllerConfig_Default<64>();

    GateParameters<64> out;
    Strategy_BuildParameters((uint8_t)99, &rolling, &config, FPN_FromDouble<64>(100.0), &out);

    EXPECT(out.strategy_id == STRATEGY_NONE, "unknown id → NONE pack");
    EXPECT(FPN_IsZero(out.bg_price_threshold), "no gates set");
    EXPECT(out.flags == 0, "no flags set");
}

//======================================================================================================
// main
//======================================================================================================
int main() {
    printf("=== Strategy parameter pipeline tests ===\n\n");

    test_simpledip_basic();
    printf("simpledip_basic                   ok\n");
    test_dispatcher_routing();
    printf("dispatcher_routing                ok\n");
    test_strategy_none();
    printf("strategy_none                     ok\n");
    test_rebuild_all_parameters();
    printf("rebuild_all_parameters            ok\n");
    test_rebuild_skips_none();
    printf("rebuild_skips_none                ok\n");
    test_end_to_end_pipeline();
    printf("end_to_end_pipeline               ok\n");
    test_unknown_strategy_id();
    printf("unknown_strategy_id               ok\n");

    printf("\n=== %s (%d failures) ===\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
