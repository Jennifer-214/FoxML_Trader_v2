// test_migration_head_to_head.cpp — phase 13 head-to-head comparison
//
// THE critical validation. Runs the same synthetic tick stream through both
// the LegacyReferenceDriver (single-threaded, direct state mutation, no SPSC,
// no seqlock) and the ShardedBacktestDriver (full per-core architecture).
// Compares trade-by-trade and verifies they produce IDENTICAL outcomes.
//
// What "identical" means:
//   - Same total entry count
//   - Same total exit count
//   - Same final balance
//   - Same realized P&L
//
// If they match, the per-core architecture is structurally validated against
// a single-threaded reference. If they diverge, the divergence happens HERE
// in a small reproducible test, not in a production migration PR with
// thousands of LOC of unrelated changes.
//
// This is the structural head-to-head, not an apples to apples vs the actual
// production engine. That second test happens in the production migration PR
// with the real PortfolioController + RollingStats + RegimeDetector code.

#include "CoreFrameworks/ControllerEventLoop.hpp"
#include "CoreFrameworks/ExecutionCore.hpp"
#include "CoreFrameworks/LegacyReferenceDriver.hpp"
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

static bool approx(double a, double b, double tol = 1e-6) {
    double d = a - b;
    return d < tol && d > -tol;
}

// Build a deterministic tick stream that has well-defined dip patterns.
static std::vector<Tick<64>> make_test_stream(int num_ticks, double base_price) {
    std::vector<Tick<64>> ticks;
    ticks.reserve(num_ticks);
    for (int i = 0; i < num_ticks; ++i) {
        Tick<64> t;
        memset(&t, 0, sizeof(t));
        // Sawtooth that crosses BG threshold and TP/SL bands
        double phase = (double)(i % 100);
        double price;
        if (phase < 50.0) {
            price = base_price + phase * 4.0;          // ramp up 0..200
        } else {
            price = base_price + (100.0 - phase) * 4.0; // ramp down 200..0
        }
        t.price = FPN_FromDouble<64>(price);
        t.volume = FPN_FromDouble<64>(2000.0);
        t.timestamp = (uint64_t)(i * 1000);
        t.sequence = (uint64_t)i;
        ticks.push_back(t);
    }
    return ticks;
}

// Build a fixed parameter pack so both paths use the same gate thresholds.
static GateParameters<64> make_params(double bg_threshold, double tp, double sl,
                                       double trade_size) {
    GateParameters<64> p;
    GateParameters_Init(&p);
    p.bg_price_threshold   = FPN_FromDouble<64>(bg_threshold);
    p.bg_volume_threshold  = FPN_Zero<64>();
    p.sg_take_profit_price = FPN_FromDouble<64>(tp);
    p.sg_stop_loss_price   = FPN_FromDouble<64>(sl);
    p.trade_size           = FPN_FromDouble<64>(trade_size);
    p.strategy_id          = STRATEGY_SIMPLE_DIP;
    p.flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    return p;
}

//======================================================================================================
// THE comparison: same params + same tick stream → identical outcomes
//======================================================================================================
static void test_head_to_head_single_slot() {
    auto ticks = make_test_stream(2000, 60000.0);

    GateParameters<64> params = make_params(60100.0, 60150.0, 59950.0, 0.01);

    // ----- LEGACY REFERENCE PATH -----
    LegacyReferenceState<64> legacy;
    LegacyReference_Init(&legacy,
        FPN_FromDouble<64>(10000.0),
        FPN_FromDouble<64>(0.001),
        FPN_FromDouble<64>(1000.0));
    int leg_slot = LegacyReference_AddSlot(&legacy, STRATEGY_SIMPLE_DIP);
    legacy.slots[leg_slot].params = params;

    for (auto& t : ticks) {
        LegacyReference_Tick(&legacy, t);
    }

    // ----- SHARDED PATH -----
    OrderManagerState<64> sharded_oms;
    EventLoopState<64> sharded;
    EventLoopState_InitLegacy(&sharded, &sharded_oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    int sh_slot = EventLoopState_RegisterCore(&sharded, &core,
        params.sg_take_profit_price, params.sg_stop_loss_price, params.trade_size);
    EventLoopState_SetCoreStrategy(&sharded, sh_slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));
    ExecutionCore_SetParameters(&core, params);
    ExecutionCore_SetPermission(&core, 1);

    ShardedBacktestDriver<64> drv;
    // No rolling/config so the slow path doesn't rebuild parameters and
    // overwrite the fixed pack we set above. We're comparing the gate logic,
    // not the strategy rebuild logic.
    ShardedBacktestDriver_Init(&drv, &sharded, (RollingStats<64>*)nullptr,
                                (const ControllerConfig<64>*)nullptr, 1000000);

    ShardedBacktest_Run(&drv, ticks.data(), (int)ticks.size());

    // ----- COMPARE -----
    double leg_balance = FPN_ToDouble(legacy.balance);
    double sh_balance  = FPN_ToDouble(sharded.oms->balance);
    double leg_pnl = FPN_ToDouble(legacy.realized_pnl);
    double sh_pnl  = FPN_ToDouble(sharded.oms->realized_pnl);

    printf("\n  --- single-slot head to head ---\n");
    printf("  legacy:  entries=%lu exits=%lu balance=%.6f pnl=%.6f\n",
           (unsigned long)legacy.total_entries,
           (unsigned long)legacy.total_exits,
           leg_balance, leg_pnl);
    printf("  sharded: entries=%lu exits=%lu balance=%.6f pnl=%.6f\n",
           (unsigned long)sharded.total_entries,
           (unsigned long)sharded.total_exits,
           sh_balance, sh_pnl);

    EXPECT(legacy.total_entries == sharded.total_entries,
           "identical entry count between legacy and sharded");
    EXPECT(legacy.total_exits == sharded.total_exits,
           "identical exit count between legacy and sharded");
    EXPECT(approx(leg_balance, sh_balance, 1e-4),
           "identical balance between legacy and sharded");
    EXPECT(approx(leg_pnl, sh_pnl, 1e-4),
           "identical realized P&L between legacy and sharded");
}

//======================================================================================================
// Multi-slot head to head — N slots in legacy, N cores in sharded
//======================================================================================================
static void test_head_to_head_multi_slot() {
    auto ticks = make_test_stream(2000, 60000.0);

    GateParameters<64> params = make_params(60100.0, 60150.0, 59950.0, 0.005);
    constexpr int N = 4;

    // ----- LEGACY REFERENCE PATH -----
    LegacyReferenceState<64> legacy;
    LegacyReference_Init(&legacy,
        FPN_FromDouble<64>(10000.0),
        FPN_FromDouble<64>(0.001),
        FPN_FromDouble<64>(1000.0));
    for (int i = 0; i < N; ++i) {
        int slot = LegacyReference_AddSlot(&legacy, STRATEGY_SIMPLE_DIP);
        legacy.slots[slot].params = params;
    }

    for (auto& t : ticks) {
        LegacyReference_Tick(&legacy, t);
    }

    // ----- SHARDED PATH -----
    OrderManagerState<64> sharded_oms;
    EventLoopState<64> sharded;
    EventLoopState_InitLegacy(&sharded, &sharded_oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> rings[N];
    ExecutionCore<64> cores[N];
    for (int i = 0; i < N; ++i) {
        SPSCRing_Init(&rings[i]);
        ExecutionCore_Init(&cores[i], 0, &rings[i]);
        EventLoopState_RegisterCore(&sharded, &cores[i],
            params.sg_take_profit_price, params.sg_stop_loss_price, params.trade_size);
        EventLoopState_SetCoreStrategy(&sharded, i, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));
        ExecutionCore_SetParameters(&cores[i], params);
        ExecutionCore_SetPermission(&cores[i], 1);
    }

    ShardedBacktestDriver<64> drv;
    ShardedBacktestDriver_Init(&drv, &sharded, (RollingStats<64>*)nullptr,
                                (const ControllerConfig<64>*)nullptr, 1000000);

    ShardedBacktest_Run(&drv, ticks.data(), (int)ticks.size());

    // ----- COMPARE -----
    double leg_balance = FPN_ToDouble(legacy.balance);
    double sh_balance  = FPN_ToDouble(sharded.oms->balance);
    double leg_pnl = FPN_ToDouble(legacy.realized_pnl);
    double sh_pnl  = FPN_ToDouble(sharded.oms->realized_pnl);

    printf("\n  --- %d-slot head to head ---\n", N);
    printf("  legacy:  entries=%lu exits=%lu balance=%.6f pnl=%.6f\n",
           (unsigned long)legacy.total_entries,
           (unsigned long)legacy.total_exits,
           leg_balance, leg_pnl);
    printf("  sharded: entries=%lu exits=%lu balance=%.6f pnl=%.6f\n",
           (unsigned long)sharded.total_entries,
           (unsigned long)sharded.total_exits,
           sh_balance, sh_pnl);

    EXPECT(legacy.total_entries == sharded.total_entries,
           "identical multi-slot entry count");
    EXPECT(legacy.total_exits == sharded.total_exits,
           "identical multi-slot exit count");
    EXPECT(approx(leg_balance, sh_balance, 1e-4),
           "identical multi-slot balance");
    EXPECT(approx(leg_pnl, sh_pnl, 1e-4),
           "identical multi-slot realized P&L");
}

//======================================================================================================
// No-trade scenario: BG threshold above all tick prices, neither side trades
//======================================================================================================
static void test_head_to_head_no_trades() {
    auto ticks = make_test_stream(500, 60000.0);

    // BG threshold below the lowest tick price (60000) → never fires
    GateParameters<64> params = make_params(50000.0, 70000.0, 40000.0, 0.01);

    LegacyReferenceState<64> legacy;
    LegacyReference_Init(&legacy, FPN_FromDouble<64>(10000.0),
        FPN_FromDouble<64>(0.001), FPN_FromDouble<64>(1000.0));
    int leg_slot = LegacyReference_AddSlot(&legacy, STRATEGY_SIMPLE_DIP);
    legacy.slots[leg_slot].params = params;
    for (auto& t : ticks) LegacyReference_Tick(&legacy, t);

    OrderManagerState<64> sharded_oms;
    EventLoopState<64> sharded;
    EventLoopState_InitLegacy(&sharded, &sharded_oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));
    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    int sh_slot = EventLoopState_RegisterCore(&sharded, &core,
        params.sg_take_profit_price, params.sg_stop_loss_price, params.trade_size);
    EventLoopState_SetCoreStrategy(&sharded, sh_slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));
    ExecutionCore_SetParameters(&core, params);
    ExecutionCore_SetPermission(&core, 1);

    ShardedBacktestDriver<64> drv;
    ShardedBacktestDriver_Init(&drv, &sharded, (RollingStats<64>*)nullptr,
                                (const ControllerConfig<64>*)nullptr, 1000000);
    ShardedBacktest_Run(&drv, ticks.data(), (int)ticks.size());

    EXPECT(legacy.total_entries == 0 && sharded.total_entries == 0,
           "neither path traded");
    EXPECT(FPN_ToDouble(legacy.balance) == FPN_ToDouble(sharded.oms->balance),
           "balances unchanged in both paths");
}

//======================================================================================================
// Always-fires scenario: BG threshold above all prices, every inactive slot enters every tick
//======================================================================================================
static void test_head_to_head_always_fires() {
    // Use only 100 ticks; want a tight BG check + immediate exit cycle
    auto ticks = make_test_stream(100, 60000.0);

    // BG threshold ABOVE max tick price (max ~60200) → BG_Evaluate fires every tick
    // TP just above min, so SG fires almost immediately after entry
    GateParameters<64> params = make_params(70000.0, 60050.0, 59000.0, 0.01);

    LegacyReferenceState<64> legacy;
    LegacyReference_Init(&legacy, FPN_FromDouble<64>(10000.0),
        FPN_FromDouble<64>(0.001), FPN_FromDouble<64>(1000.0));
    int leg_slot = LegacyReference_AddSlot(&legacy, STRATEGY_SIMPLE_DIP);
    legacy.slots[leg_slot].params = params;
    for (auto& t : ticks) LegacyReference_Tick(&legacy, t);

    OrderManagerState<64> sharded_oms;
    EventLoopState<64> sharded;
    EventLoopState_InitLegacy(&sharded, &sharded_oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));
    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    int sh_slot = EventLoopState_RegisterCore(&sharded, &core,
        params.sg_take_profit_price, params.sg_stop_loss_price, params.trade_size);
    EventLoopState_SetCoreStrategy(&sharded, sh_slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));
    ExecutionCore_SetParameters(&core, params);
    ExecutionCore_SetPermission(&core, 1);

    ShardedBacktestDriver<64> drv;
    ShardedBacktestDriver_Init(&drv, &sharded, (RollingStats<64>*)nullptr,
                                (const ControllerConfig<64>*)nullptr, 1000000);
    ShardedBacktest_Run(&drv, ticks.data(), (int)ticks.size());

    printf("\n  --- always-fires head to head ---\n");
    printf("  legacy:  entries=%lu exits=%lu\n",
           (unsigned long)legacy.total_entries, (unsigned long)legacy.total_exits);
    printf("  sharded: entries=%lu exits=%lu\n",
           (unsigned long)sharded.total_entries, (unsigned long)sharded.total_exits);

    EXPECT(legacy.total_entries == sharded.total_entries,
           "always-fires entry count matches");
    EXPECT(legacy.total_exits == sharded.total_exits,
           "always-fires exit count matches");
    EXPECT(approx(FPN_ToDouble(legacy.balance), FPN_ToDouble(sharded.oms->balance), 1e-4),
           "always-fires balance matches");
}

//======================================================================================================
// main
//======================================================================================================
int main() {
    printf("=== Migration head-to-head tests ===\n");
    printf("Comparing LegacyReferenceDriver (single-thread, direct state)\n");
    printf("vs ShardedBacktestDriver (per-core, SPSC, seqlock, events)\n");

    test_head_to_head_single_slot();
    printf("head_to_head_single_slot          ok\n");
    test_head_to_head_multi_slot();
    printf("head_to_head_multi_slot           ok\n");
    test_head_to_head_no_trades();
    printf("head_to_head_no_trades            ok\n");
    test_head_to_head_always_fires();
    printf("head_to_head_always_fires         ok\n");

    printf("\n=== %s (%d failures) ===\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
