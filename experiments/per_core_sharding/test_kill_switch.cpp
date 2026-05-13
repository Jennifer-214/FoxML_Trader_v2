// test_kill_switch.cpp — phase 09 functional tests
//
// Validates the per-core kill switch:
//
//   1. Default disabled — fresh state, evaluate is a no-op
//   2. Min balance trip — drop balance below floor, evaluate fires once
//   3. Drawdown trip — peak balance is set, balance falls more than max_dd
//   4. Trip clears all permissions — every registered core's permission goes 0
//   5. Trip is idempotent — second evaluate on same condition returns 0
//   6. Manual trip helper works
//   7. Active position can still exit after trip — SG path is unaffected
//   8. Unpause restores permission only on cores with strategy != NONE (P9.7)
//   9. Unpause clears the tripped flag

#include "CoreFrameworks/ControllerEventLoop.hpp"
#include "CoreFrameworks/ExecutionCore.hpp"
#include "CoreFrameworks/Tick.hpp"

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

// Helper: register N cores into a state, all with SimpleDip strategy.
template <int N>
static void register_n_cores(EventLoopState<64>& state,
                             ExecutionCore<64> (&cores)[N],
                             SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> (&rings)[N]) {
    for (int i = 0; i < N; ++i) {
        SPSCRing_Init(&rings[i]);
        ExecutionCore_Init(&cores[i], 0, &rings[i]);
        int slot = EventLoopState_RegisterCore(&state, &cores[i],
            FPN_FromDouble<64>(60100.0),
            FPN_FromDouble<64>(59900.0),
            FPN_FromDouble<64>(0.01));
        EventLoopState_SetCoreStrategy(&state, slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));
        // Manually arm permission so we can later observe it being cleared.
        ExecutionCore_SetPermission(&cores[i], 1);
    }
}

//======================================================================================================
// test 1: default state has kill switch disabled
//======================================================================================================
static void test_default_disabled() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    EXPECT(!BITMAP_IS_SET(state.oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED),"default not tripped");
    EXPECT(state.oms->ks_trips_total == 0, "no trips counted");
    EXPECT(FPN_IsZero(state.oms->ks_min_balance), "min balance unset");
    EXPECT(FPN_IsZero(state.oms->ks_max_drawdown_pct), "max drawdown unset");

    int tripped = EventLoop_KillSwitchEvaluate(&state);
    EXPECT(tripped == 0, "default evaluate is no-op");
    EXPECT(!BITMAP_IS_SET(state.oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED),"still not tripped after evaluate");
}

//======================================================================================================
// test 2: balance below min_balance trips the switch
//======================================================================================================
static void test_balance_floor_trip() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));
    EventLoopState_ConfigureKillSwitch(&state,
        FPN_FromDouble<64>(5000.0),    // min_balance
        FPN_Zero<64>());               // dd disabled

    // Balance starts at 10k, well above 5k floor — no trip.
    int t1 = EventLoop_KillSwitchEvaluate(&state);
    EXPECT(t1 == 0, "above floor: no trip");

    // Drop balance to 4999 — below floor, should trip.
    state.oms->balance = FPN_FromDouble<64>(4999.0);
    int t2 = EventLoop_KillSwitchEvaluate(&state);
    EXPECT(t2 == 1, "below floor: trip fires");
    EXPECT(BITMAP_IS_SET(state.oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED),"state shows tripped");
    EXPECT(state.oms->ks_trips_total == 1, "trips counter bumped once");
}

//======================================================================================================
// test 3: drawdown from peak trips the switch
//======================================================================================================
static void test_drawdown_trip() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));
    EventLoopState_ConfigureKillSwitch(&state,
        FPN_Zero<64>(),                 // floor disabled
        FPN_FromDouble<64>(0.20));      // 20% max drawdown

    // peak starts at 10k. balance at 10k — 0% drawdown, no trip.
    int t1 = EventLoop_KillSwitchEvaluate(&state);
    EXPECT(t1 == 0, "0% drawdown: no trip");

    // Push peak up to 12k via a synthetic exit accounting.
    state.oms->balance = FPN_FromDouble<64>(12000.0);
    if (FPN_GreaterThan(state.oms->balance, state.oms->ks_peak_balance)) {
        state.oms->ks_peak_balance = state.oms->balance;
    }

    // Drop balance to 9500 — drawdown = 2500/12000 = ~20.83% > 20% → trip.
    state.oms->balance = FPN_FromDouble<64>(9500.0);
    int t2 = EventLoop_KillSwitchEvaluate(&state);
    EXPECT(t2 == 1, "20.83% drawdown: trip fires");
    EXPECT(BITMAP_IS_SET(state.oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED),"state shows tripped");
}

//======================================================================================================
// test 4: trip clears every core's permission
//======================================================================================================
static void test_trip_clears_permissions() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    constexpr int N = 4;
    ExecutionCore<64> cores[N];
    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> rings[N];
    register_n_cores<N>(state, cores, rings);

    // All four start armed.
    for (int i = 0; i < N; ++i) {
        EXPECT(__atomic_load_n(&cores[i].permission, __ATOMIC_ACQUIRE) == 1,
               "core armed before trip");
    }

    // Manual trip via helper.
    EventLoop_KillSwitchTrip(&state);
    EXPECT(BITMAP_IS_SET(state.oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED),"tripped");

    // All four should now be cleared.
    for (int i = 0; i < N; ++i) {
        EXPECT(__atomic_load_n(&cores[i].permission, __ATOMIC_ACQUIRE) == 0,
               "core permission cleared after trip");
    }
}

//======================================================================================================
// test 5: trip is idempotent — second call returns 0
//======================================================================================================
static void test_trip_idempotent() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));
    EventLoopState_ConfigureKillSwitch(&state, FPN_FromDouble<64>(5000.0), FPN_Zero<64>());
    state.oms->balance = FPN_FromDouble<64>(4000.0);  // below floor

    int t1 = EventLoop_KillSwitchEvaluate(&state);
    int t2 = EventLoop_KillSwitchEvaluate(&state);
    int t3 = EventLoop_KillSwitchEvaluate(&state);
    EXPECT(t1 == 1, "first call trips");
    EXPECT(t2 == 0, "second call returns 0");
    EXPECT(t3 == 0, "third call returns 0");
    EXPECT(state.oms->ks_trips_total == 1, "trips counted exactly once");
}

//======================================================================================================
// test 6: active position can still exit after kill switch trips
//======================================================================================================
// This is the critical "active positions exit normally" guarantee from the
// plan. The trip clears permission (no new entries) but SG still fires and the
// resulting exit event still flows through OnEvent and updates balance.
//======================================================================================================
static void test_active_position_can_exit_after_trip() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60500.0),
        FPN_FromDouble<64>(59500.0),
        FPN_FromDouble<64>(0.01));
    EventLoopState_SetCoreStrategy(&state, slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));

    // Manually open a position via OnEvent (entry).
    TradeEvent<64> entry;
    entry.type = TRADE_EVENT_ENTRY;
    entry.core_id = (uint16_t)slot;
    entry.price = FPN_FromDouble<64>(60100.0);
    entry.timestamp = 1000;
    EventLoop_OnEvent(&state, entry);
    EXPECT(Portfolio_SlotActive(&state.oms->portfolio, slot) == 1, "position open");
    EXPECT(state.total_entries == 1, "entry counted");

    // Trip the kill switch.
    EventLoop_KillSwitchTrip(&state);
    EXPECT(BITMAP_IS_SET(state.oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED),"tripped");
    EXPECT(__atomic_load_n(&core.permission, __ATOMIC_ACQUIRE) == 0, "permission cleared");

    // Now an exit event should still be processable — SG fired on the hot path
    // (we simulate by directly pushing the event through OnEvent, equivalent
    // to the drain step picking it up from the event ring).
    TradeEvent<64> exit;
    exit.type = TRADE_EVENT_EXIT;
    exit.core_id = (uint16_t)slot;
    exit.price = FPN_FromDouble<64>(60500.0);
    exit.timestamp = 2000;
    EventLoop_OnEvent(&state, exit);

    EXPECT(Portfolio_SlotActive(&state.oms->portfolio, slot) == 0, "position closed");
    EXPECT(state.total_exits == 1, "exit processed even with kill switch tripped");
    // P&L was credited: entry 60100, exit 60500, qty 0.01 → gross +4.0,
    // minus fees ~1.205 → net positive. Just sanity check balance moved.
    double final_balance = FPN_ToDouble(state.oms->balance);
    EXPECT(final_balance > 10000.0, "balance updated from exit P&L");
}

//======================================================================================================
// test 7: Unpause restores permission only on cores with strategy != NONE (P9.7)
//======================================================================================================
static void test_unpause_skips_none_cores() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> rings[3];
    ExecutionCore<64> cores[3];
    for (int i = 0; i < 3; ++i) {
        SPSCRing_Init(&rings[i]);
        ExecutionCore_Init(&cores[i], 0, &rings[i]);
        EventLoopState_RegisterCore(&state, &cores[i],
            FPN_FromDouble<64>(60100.0),
            FPN_FromDouble<64>(59900.0),
            FPN_FromDouble<64>(0.01));
    }
    // Assign strategies to cores 0 and 2 only; core 1 stays NONE.
    EventLoopState_SetCoreStrategy(&state, 0, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));
    // core 1 left as STRATEGY_NONE
    EventLoopState_SetCoreStrategy(&state, 2, STRATEGY_MOMENTUM, FPN_FromDouble<64>(1000.0));

    // Trip the switch first to clear all permissions.
    EventLoop_KillSwitchTrip(&state);
    for (int i = 0; i < 3; ++i) {
        EXPECT(__atomic_load_n(&cores[i].permission, __ATOMIC_ACQUIRE) == 0,
               "all cleared after trip");
    }

    // Unpause should restore 0 and 2 only.
    int resumed = EventLoop_Unpause(&state);
    EXPECT(resumed == 2, "two cores resumed (NONE skipped)");
    EXPECT(__atomic_load_n(&cores[0].permission, __ATOMIC_ACQUIRE) == 1, "core 0 resumed");
    EXPECT(__atomic_load_n(&cores[1].permission, __ATOMIC_ACQUIRE) == 0, "core 1 (NONE) stays paused");
    EXPECT(__atomic_load_n(&cores[2].permission, __ATOMIC_ACQUIRE) == 1, "core 2 resumed");
    EXPECT(!BITMAP_IS_SET(state.oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED),"tripped flag cleared");
}

//======================================================================================================
// test 8: Unpause re-arms the switch — second trip can fire after unpause
//======================================================================================================
static void test_unpause_rearms_switch() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));
    EventLoopState_ConfigureKillSwitch(&state, FPN_FromDouble<64>(5000.0), FPN_Zero<64>());

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60100.0), FPN_FromDouble<64>(59900.0), FPN_FromDouble<64>(0.01));
    EventLoopState_SetCoreStrategy(&state, 0, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));

    // Trip 1.
    state.oms->balance = FPN_FromDouble<64>(4000.0);
    int t1 = EventLoop_KillSwitchEvaluate(&state);
    EXPECT(t1 == 1, "first trip fires");

    // Unpause (would normally happen after balance was restored externally).
    state.oms->balance = FPN_FromDouble<64>(8000.0);
    int resumed = EventLoop_Unpause(&state);
    EXPECT(resumed == 1, "core resumed");
    EXPECT(!BITMAP_IS_SET(state.oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED),"tripped flag cleared");
    EXPECT(__atomic_load_n(&core.permission, __ATOMIC_ACQUIRE) == 1, "core armed again");

    // Drop balance again — switch should fire a second time.
    state.oms->balance = FPN_FromDouble<64>(3000.0);
    int t2 = EventLoop_KillSwitchEvaluate(&state);
    EXPECT(t2 == 1, "second trip fires after unpause");
    EXPECT(state.oms->ks_trips_total == 2, "trips counter shows two");
}

//======================================================================================================
// main
//======================================================================================================
int main() {
    printf("=== Kill switch tests ===\n\n");

    test_default_disabled();
    printf("default_disabled                  ok\n");
    test_balance_floor_trip();
    printf("balance_floor_trip                ok\n");
    test_drawdown_trip();
    printf("drawdown_trip                     ok\n");
    test_trip_clears_permissions();
    printf("trip_clears_permissions           ok\n");
    test_trip_idempotent();
    printf("trip_idempotent                   ok\n");
    test_active_position_can_exit_after_trip();
    printf("active_position_exit_after_trip   ok\n");
    test_unpause_skips_none_cores();
    printf("unpause_skips_none_cores          ok\n");
    test_unpause_rearms_switch();
    printf("unpause_rearms_switch             ok\n");

    printf("\n=== %s (%d failures) ===\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
