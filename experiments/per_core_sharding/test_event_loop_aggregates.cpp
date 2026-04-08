// test_event_loop_aggregates.cpp — phase 10 functional tests
//
// Validates the EventLoopAggregates adapter that bridges the per-core
// EventLoopState to the existing TUI/GUI snapshot fields.
//
//   1. Empty state aggregates have zeros across the board
//   2. After register + entry, balance unchanged but active_position_count == 1
//   3. Unrealized P&L is zero when mark_price == 0
//   4. Unrealized P&L = qty * (mark - entry) when mark_price > 0
//   5. Equity == balance when no positions are open
//   6. Equity == balance + unrealized when a position is open
//   7. After exit, balance updates and unrealized goes back to 0
//   8. Kill switch state is reflected in aggregates
//   9. Drawdown computed against equity (mark to market), not balance
//  10. Multi-core multi-position unrealized aggregates correctly

#include "CoreFrameworks/ControllerEventLoop.hpp"
#include "CoreFrameworks/EventLoopAggregates.hpp"
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

static bool approx(double a, double b, double tol = 1e-3) {
    double d = a - b;
    return d < tol && d > -tol;
}

// Build an entry event for slot N at price/timestamp.
static TradeEvent<64> make_entry(int slot, double price, uint64_t ts) {
    TradeEvent<64> e;
    e.type = TRADE_EVENT_ENTRY;
    e.core_id = (uint16_t)slot;
    e.price = FPN_FromDouble<64>(price);
    e.timestamp = ts;
    return e;
}

static TradeEvent<64> make_exit(int slot, double price, uint64_t ts) {
    TradeEvent<64> e;
    e.type = TRADE_EVENT_EXIT;
    e.core_id = (uint16_t)slot;
    e.price = FPN_FromDouble<64>(price);
    e.timestamp = ts;
    return e;
}

//======================================================================================================
// test 1: empty state has zero aggregates
//======================================================================================================
static void test_empty_state() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    EventLoopAggregates agg = EventLoop_GetAggregates(&state, FPN_Zero<64>());
    EXPECT(approx(agg.balance, 10000.0), "balance == starting");
    EXPECT(approx(agg.realized_pnl, 0.0), "no realized pnl");
    EXPECT(approx(agg.unrealized_pnl, 0.0), "no unrealized");
    EXPECT(approx(agg.equity, 10000.0), "equity == balance");
    EXPECT(agg.active_position_count == 0, "no active positions");
    EXPECT(agg.registered_cores == 0, "no cores registered");
    EXPECT(agg.total_entries == 0, "no entries");
    EXPECT(agg.total_exits == 0, "no exits");
    EXPECT(agg.kill_switch_tripped == 0, "ks not tripped");
}

//======================================================================================================
// test 2: register + entry → active count goes up, balance unchanged
//======================================================================================================
static void test_after_entry() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60500.0),
        FPN_FromDouble<64>(59500.0),
        FPN_FromDouble<64>(0.01));
    EventLoopState_SetCoreStrategy(&state, slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));

    EventLoop_OnEvent(&state, make_entry(slot, 60100.0, 1000));

    EventLoopAggregates agg = EventLoop_GetAggregates(&state, FPN_Zero<64>());
    EXPECT(agg.active_position_count == 1, "1 active position");
    EXPECT(agg.registered_cores == 1, "1 core registered");
    EXPECT(agg.total_entries == 1, "1 entry processed");
    // balance was reduced by entry fee (notional * fee_rate)
    // notional = 60100 * 0.01 = 601, fee = 0.001 * 601 = 0.601
    // ... actually the fee is recorded on the position but not deducted from
    // balance until the exit (matches existing fee model). So balance is still 10000.
    EXPECT(approx(agg.balance, 10000.0), "balance unchanged on entry (fee deferred)");
}

//======================================================================================================
// test 3: mark_price = 0 → unrealized stays at 0
//======================================================================================================
static void test_unrealized_zero_when_no_mark() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0), FPN_FromDouble<64>(0.01));
    EventLoopState_SetCoreStrategy(&state, slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));
    EventLoop_OnEvent(&state, make_entry(slot, 60100.0, 1000));

    EventLoopAggregates agg = EventLoop_GetAggregates(&state, FPN_Zero<64>());
    EXPECT(approx(agg.unrealized_pnl, 0.0), "unrealized = 0 with no mark");
    EXPECT(approx(agg.equity, agg.balance), "equity = balance with no mark");
}

//======================================================================================================
// test 4: unrealized = qty * (mark - entry)
//======================================================================================================
static void test_unrealized_with_mark() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0), FPN_FromDouble<64>(0.01));
    EventLoopState_SetCoreStrategy(&state, slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));
    EventLoop_OnEvent(&state, make_entry(slot, 60100.0, 1000));

    // Mark price is 60200, entry was 60100, qty 0.01 → unrealized = 1.0
    EventLoopAggregates agg = EventLoop_GetAggregates(&state, FPN_FromDouble<64>(60200.0));
    EXPECT(approx(agg.unrealized_pnl, 1.0, 0.01), "unrealized = qty*(mark-entry)");
    EXPECT(approx(agg.equity, 10001.0, 0.01), "equity = balance + unrealized");
}

//======================================================================================================
// test 5: after exit, balance + realized update, unrealized goes back to 0
//======================================================================================================
static void test_after_exit() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0), FPN_FromDouble<64>(0.01));
    EventLoopState_SetCoreStrategy(&state, slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));

    EventLoop_OnEvent(&state, make_entry(slot, 60100.0, 1000));
    EventLoop_OnEvent(&state, make_exit(slot, 60500.0, 2000));

    EventLoopAggregates agg = EventLoop_GetAggregates(&state, FPN_FromDouble<64>(60500.0));
    EXPECT(agg.active_position_count == 0, "position closed");
    EXPECT(approx(agg.unrealized_pnl, 0.0), "no unrealized after close");
    // gross = (60500-60100)*0.01 = 4.0
    // entry_fee = 60100*0.01*0.001 = 0.601
    // exit_fee  = 60500*0.01*0.001 = 0.605
    // net = 4.0 - 0.601 - 0.605 = 2.794
    EXPECT(approx(agg.realized_pnl, 2.794, 0.01), "realized P&L after fees");
    EXPECT(approx(agg.balance, 10002.794, 0.01), "balance updated by net");
    EXPECT(approx(agg.equity, agg.balance), "equity = balance with no positions");
}

//======================================================================================================
// test 6: kill switch trip is reflected
//======================================================================================================
static void test_kill_switch_in_aggregates() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    EventLoop_KillSwitchTrip(&state);
    EventLoopAggregates agg = EventLoop_GetAggregates(&state, FPN_Zero<64>());
    EXPECT(agg.kill_switch_tripped == 1, "kill switch reflected in aggregates");
}

//======================================================================================================
// test 7: drawdown is computed against equity (mark to market)
//======================================================================================================
static void test_drawdown_against_equity() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0), FPN_FromDouble<64>(1.0));
    EventLoopState_SetCoreStrategy(&state, slot, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));

    // Push peak balance up by hand (would normally come from prior trades)
    state.ks_peak_balance = FPN_FromDouble<64>(12000.0);

    // Open a position at 60000 with qty 1.0
    EventLoop_OnEvent(&state, make_entry(slot, 60000.0, 1000));

    // Mark price drops to 58000 → unrealized = 1*(58000-60000) = -2000
    // equity = 10000 + (-2000) = 8000
    // drawdown vs peak 12000 = 4000 = 33.3%
    EventLoopAggregates agg = EventLoop_GetAggregates(&state, FPN_FromDouble<64>(58000.0));
    EXPECT(approx(agg.unrealized_pnl, -2000.0, 0.5), "unrealized loss");
    EXPECT(approx(agg.equity, 8000.0, 0.5), "equity reflects mark to market");
    EXPECT(approx(agg.max_drawdown, 4000.0, 0.5), "drawdown is peak - equity");
    EXPECT(approx(agg.max_drawdown_pct, 4000.0/12000.0, 0.001), "drawdown pct");
}

//======================================================================================================
// test 8: multi-core multi-position aggregation
//======================================================================================================
static void test_multi_core_aggregation() {
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
    }

    // Three open positions at three different entry prices
    EventLoop_OnEvent(&state, make_entry(0, 60000.0, 1000));
    EventLoop_OnEvent(&state, make_entry(1, 60100.0, 1100));
    EventLoop_OnEvent(&state, make_entry(2, 60200.0, 1200));

    // Mark price 60150
    // pos 0: 0.01 * (60150 - 60000) =  1.5
    // pos 1: 0.01 * (60150 - 60100) =  0.5
    // pos 2: 0.01 * (60150 - 60200) = -0.5
    // total unrealized = 1.5
    EventLoopAggregates agg = EventLoop_GetAggregates(&state, FPN_FromDouble<64>(60150.0));
    EXPECT(agg.active_position_count == 3, "3 active positions");
    EXPECT(agg.registered_cores == 3, "3 cores registered");
    EXPECT(approx(agg.unrealized_pnl, 1.5, 0.01), "unrealized aggregates correctly");
    EXPECT(approx(agg.equity, 10001.5, 0.01), "equity reflects aggregate unrealized");
}

//======================================================================================================
// main
//======================================================================================================
int main() {
    printf("=== EventLoop aggregates tests ===\n\n");

    test_empty_state();
    printf("empty_state                       ok\n");
    test_after_entry();
    printf("after_entry                       ok\n");
    test_unrealized_zero_when_no_mark();
    printf("unrealized_zero_when_no_mark      ok\n");
    test_unrealized_with_mark();
    printf("unrealized_with_mark              ok\n");
    test_after_exit();
    printf("after_exit                        ok\n");
    test_kill_switch_in_aggregates();
    printf("kill_switch_in_aggregates         ok\n");
    test_drawdown_against_equity();
    printf("drawdown_against_equity           ok\n");
    test_multi_core_aggregation();
    printf("multi_core_aggregation            ok\n");

    printf("\n=== %s (%d failures) ===\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
