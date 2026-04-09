// test_order_event_log.cpp — phase 03 chunk 2 functional tests
//
// Validates the OrderEventLog append-only log and the Portfolio_FromEventLog
// deterministic fold function.
//
//   1. Init produces clean state (count=0, capacity>0, next_event_id=1)
//   2. Append assigns monotonic event_ids
//   3. Multiple appends grow the buffer correctly
//   4. Fold: single entry+exit pair produces correct balance
//   5. Fold: multiple cores with interleaved entry/exit
//   6. Fold determinism: same events replayed twice produce identical state
//   7. Fold ignores non-fill events (SUBMITTED, REJECTED)
//   8. Free cleans up
//   9. MakeFill and MakeRejection helpers produce correct events

#include "CoreFrameworks/OrderEventLog.hpp"
#include "CoreFrameworks/Order.hpp"

#include <cstdio>
#include <cmath>

using namespace tt;

static int failures = 0;

#define EXPECT(cond, msg)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);     \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static bool approx(double a, double b, double tol = 0.01) {
    return std::fabs(a - b) < tol;
}

//======================================================================================================
// test 1: init produces clean state
//======================================================================================================
static void test_init() {
    OrderEventLog<64> log;
    OrderEventLog_Init(&log);

    EXPECT(log.count == 0, "init: count == 0");
    EXPECT(log.capacity >= ORDER_EVENT_LOG_INIT_CAPACITY, "init: capacity >= default");
    EXPECT(log.next_event_id == 1, "init: next_event_id == 1");
    EXPECT(log.entries != nullptr, "init: entries allocated");

    OrderEventLog_Free(&log);
    EXPECT(log.entries == nullptr, "free: entries nulled");
    EXPECT(log.count == 0, "free: count zeroed");
}

//======================================================================================================
// test 2: append assigns monotonic event_ids
//======================================================================================================
static void test_append_monotonic_ids() {
    OrderEventLog<64> log;
    OrderEventLog_Init(&log);

    OrderEvent<64> e1 = OrderEvent_MakeFill<64>(
        100, 1000, ORDER_MARKET_BUY, 0,
        FPN_FromDouble<64>(60000.0), FPN_FromDouble<64>(0.01),
        FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0));
    OrderEvent<64> e2 = OrderEvent_MakeRejection<64>(
        101, 1001, ORDER_MARKET_SELL, 1, "test reject");

    int ok1 = OrderEventLog_Append(&log, e1);
    int ok2 = OrderEventLog_Append(&log, e2);

    EXPECT(ok1 == 1, "append: first succeeds");
    EXPECT(ok2 == 1, "append: second succeeds");
    EXPECT(log.count == 2, "append: count == 2");
    EXPECT(log.entries[0].event_id == 1, "append: first event_id == 1");
    EXPECT(log.entries[1].event_id == 2, "append: second event_id == 2");
    EXPECT(log.entries[0].type == OEVT_FULL_FILL, "append: first is fill");
    EXPECT(log.entries[1].type == OEVT_REJECTED, "append: second is rejection");

    OrderEventLog_Free(&log);
}

//======================================================================================================
// test 3: fold — single entry+exit pair
//======================================================================================================
// entry at $60000, qty 0.01 BTC, exit at $60100
// gross = (60100 - 60000) * 0.01 = $1.00
// entry fee = 60000 * 0.01 * 0.001 = $0.60
// exit fee = 60100 * 0.01 * 0.001 = $0.601
// net = 1.00 - 0.60 - 0.601 = -$0.201
// final balance = 10000 + (-0.201) = $9999.799
//======================================================================================================
static void test_fold_single_pair() {
    OrderEventLog<64> log;
    OrderEventLog_Init(&log);

    FPN<64> entry_price = FPN_FromDouble<64>(60000.0);
    FPN<64> exit_price  = FPN_FromDouble<64>(60100.0);
    FPN<64> qty         = FPN_FromDouble<64>(0.01);
    FPN<64> tp          = FPN_FromDouble<64>(60500.0);
    FPN<64> sl          = FPN_FromDouble<64>(59500.0);

    // Entry fill on core 0
    OrderEventLog_Append(&log, OrderEvent_MakeFill<64>(
        1, 1000, ORDER_MARKET_BUY, 0, entry_price, qty, tp, sl));
    // Exit fill on core 0
    OrderEventLog_Append(&log, OrderEvent_MakeFill<64>(
        2, 2000, ORDER_MARKET_SELL, 0, exit_price, qty,
        FPN_Zero<64>(), FPN_Zero<64>()));

    FPN<64> start_bal = FPN_FromDouble<64>(10000.0);
    FPN<64> fee_rate  = FPN_FromDouble<64>(0.001);
    FoldResult<64> res = Portfolio_FromEventLog(&log, start_bal, fee_rate);

    double final_bal = FPN_ToDouble(res.balance);
    EXPECT(res.fills_processed == 2, "fold pair: 2 fills processed");
    EXPECT(approx(final_bal, 9999.799, 0.01), "fold pair: balance ~9999.80");
    EXPECT(res.portfolio.active_bitmap == 0, "fold pair: no open positions");

    OrderEventLog_Free(&log);
}

//======================================================================================================
// test 4: fold — multiple cores interleaved
//======================================================================================================
// core 0: entry $60000, exit $60100 (net ~= -$0.201 per test 3)
// core 1: entry $60050, exit $60200 (net ~= $0.897)
// total: start $10000, two pairs
//======================================================================================================
static void test_fold_multi_core() {
    OrderEventLog<64> log;
    OrderEventLog_Init(&log);

    FPN<64> qty  = FPN_FromDouble<64>(0.01);
    FPN<64> tp   = FPN_FromDouble<64>(61000.0);
    FPN<64> sl   = FPN_FromDouble<64>(59000.0);

    // Core 0 entry
    OrderEventLog_Append(&log, OrderEvent_MakeFill<64>(
        1, 1000, ORDER_MARKET_BUY, 0,
        FPN_FromDouble<64>(60000.0), qty, tp, sl));
    // Core 1 entry
    OrderEventLog_Append(&log, OrderEvent_MakeFill<64>(
        2, 1001, ORDER_MARKET_BUY, 1,
        FPN_FromDouble<64>(60050.0), qty, tp, sl));
    // Core 0 exit
    OrderEventLog_Append(&log, OrderEvent_MakeFill<64>(
        3, 2000, ORDER_MARKET_SELL, 0,
        FPN_FromDouble<64>(60100.0), qty, FPN_Zero<64>(), FPN_Zero<64>()));
    // Core 1 exit
    OrderEventLog_Append(&log, OrderEvent_MakeFill<64>(
        4, 2001, ORDER_MARKET_SELL, 1,
        FPN_FromDouble<64>(60200.0), qty, FPN_Zero<64>(), FPN_Zero<64>()));

    FPN<64> start_bal = FPN_FromDouble<64>(10000.0);
    FPN<64> fee_rate  = FPN_FromDouble<64>(0.001);
    FoldResult<64> res = Portfolio_FromEventLog(&log, start_bal, fee_rate);

    EXPECT(res.fills_processed == 4, "fold multi: 4 fills");
    EXPECT(res.portfolio.active_bitmap == 0, "fold multi: no open positions");
    double final_bal = FPN_ToDouble(res.balance);
    // both pairs closed, total realized should be positive (~$0.70)
    EXPECT(final_bal > 9999.0 && final_bal < 10001.0,
           "fold multi: balance in expected range");

    OrderEventLog_Free(&log);
}

//======================================================================================================
// test 5: fold determinism — replaying same log twice gives identical results
//======================================================================================================
static void test_fold_determinism() {
    OrderEventLog<64> log;
    OrderEventLog_Init(&log);

    FPN<64> qty = FPN_FromDouble<64>(0.005);
    FPN<64> tp  = FPN_FromDouble<64>(61000.0);
    FPN<64> sl  = FPN_FromDouble<64>(59000.0);

    // Build a sequence of 6 events across 3 cores
    OrderEventLog_Append(&log, OrderEvent_MakeFill<64>(
        1, 100, ORDER_MARKET_BUY, 0,
        FPN_FromDouble<64>(60000.0), qty, tp, sl));
    OrderEventLog_Append(&log, OrderEvent_MakeFill<64>(
        2, 200, ORDER_MARKET_BUY, 1,
        FPN_FromDouble<64>(60100.0), qty, tp, sl));
    OrderEventLog_Append(&log, OrderEvent_MakeFill<64>(
        3, 300, ORDER_MARKET_SELL, 0,
        FPN_FromDouble<64>(60200.0), qty, FPN_Zero<64>(), FPN_Zero<64>()));
    OrderEventLog_Append(&log, OrderEvent_MakeFill<64>(
        4, 400, ORDER_MARKET_BUY, 2,
        FPN_FromDouble<64>(59900.0), qty, tp, sl));
    OrderEventLog_Append(&log, OrderEvent_MakeFill<64>(
        5, 500, ORDER_MARKET_SELL, 1,
        FPN_FromDouble<64>(60300.0), qty, FPN_Zero<64>(), FPN_Zero<64>()));
    OrderEventLog_Append(&log, OrderEvent_MakeFill<64>(
        6, 600, ORDER_MARKET_SELL, 2,
        FPN_FromDouble<64>(60050.0), qty, FPN_Zero<64>(), FPN_Zero<64>()));

    FPN<64> start = FPN_FromDouble<64>(10000.0);
    FPN<64> fee   = FPN_FromDouble<64>(0.001);

    FoldResult<64> r1 = Portfolio_FromEventLog(&log, start, fee);
    FoldResult<64> r2 = Portfolio_FromEventLog(&log, start, fee);

    EXPECT(FPN_Equal(r1.balance, r2.balance), "determinism: balance matches");
    EXPECT(FPN_Equal(r1.realized_pnl, r2.realized_pnl), "determinism: realized_pnl matches");
    EXPECT(r1.fills_processed == r2.fills_processed, "determinism: fills count matches");
    EXPECT(r1.portfolio.active_bitmap == r2.portfolio.active_bitmap, "determinism: bitmap matches");

    OrderEventLog_Free(&log);
}

//======================================================================================================
// test 6: fold ignores non-fill events
//======================================================================================================
static void test_fold_ignores_non_fills() {
    OrderEventLog<64> log;
    OrderEventLog_Init(&log);

    // A SUBMITTED event and a REJECTED event — neither should affect balance
    OrderEvent<64> sub;
    std::memset(&sub, 0, sizeof(sub));
    sub.type       = OEVT_SUBMITTED;
    sub.order_type = ORDER_MARKET_BUY;
    sub.core_id    = 0;
    OrderEventLog_Append(&log, sub);

    OrderEventLog_Append(&log, OrderEvent_MakeRejection<64>(
        2, 200, ORDER_MARKET_BUY, 0, "exchange rejected"));

    FPN<64> start = FPN_FromDouble<64>(10000.0);
    FPN<64> fee   = FPN_FromDouble<64>(0.001);
    FoldResult<64> res = Portfolio_FromEventLog(&log, start, fee);

    EXPECT(res.fills_processed == 0, "non-fill: zero fills processed");
    EXPECT(FPN_Equal(res.balance, start), "non-fill: balance unchanged");
    EXPECT(res.portfolio.active_bitmap == 0, "non-fill: no positions opened");

    OrderEventLog_Free(&log);
}

//======================================================================================================
// test 7: fold with open position (entry but no exit yet)
//======================================================================================================
static void test_fold_open_position() {
    OrderEventLog<64> log;
    OrderEventLog_Init(&log);

    FPN<64> qty = FPN_FromDouble<64>(0.01);
    FPN<64> tp  = FPN_FromDouble<64>(61000.0);
    FPN<64> sl  = FPN_FromDouble<64>(59000.0);

    // Entry only, no exit
    OrderEventLog_Append(&log, OrderEvent_MakeFill<64>(
        1, 100, ORDER_MARKET_BUY, 3,
        FPN_FromDouble<64>(60000.0), qty, tp, sl));

    FPN<64> start = FPN_FromDouble<64>(10000.0);
    FPN<64> fee   = FPN_FromDouble<64>(0.001);
    FoldResult<64> res = Portfolio_FromEventLog(&log, start, fee);

    EXPECT(res.fills_processed == 1, "open pos: 1 fill processed (entry only)");
    // Balance unchanged (entry fee is recorded in position, not deducted from balance)
    EXPECT(FPN_Equal(res.balance, start), "open pos: balance unchanged (no exit yet)");
    EXPECT(res.portfolio.active_bitmap == (1 << 3), "open pos: slot 3 active");
    // Verify position fields
    EXPECT(approx(FPN_ToDouble(res.portfolio.positions[3].entry_price), 60000.0),
           "open pos: entry_price correct");
    EXPECT(approx(FPN_ToDouble(res.portfolio.positions[3].quantity), 0.01),
           "open pos: quantity correct");

    OrderEventLog_Free(&log);
}

//======================================================================================================
// test 8: MakeFill / MakeRejection helpers
//======================================================================================================
static void test_event_helpers() {
    OrderEvent<64> fill = OrderEvent_MakeFill<64>(
        42, 1234, ORDER_MARKET_SELL, 7,
        FPN_FromDouble<64>(65000.0), FPN_FromDouble<64>(0.05),
        FPN_FromDouble<64>(66000.0), FPN_FromDouble<64>(64000.0));

    EXPECT(fill.order_id == 42, "MakeFill: order_id");
    EXPECT(fill.timestamp_us == 1234, "MakeFill: timestamp");
    EXPECT(fill.type == OEVT_FULL_FILL, "MakeFill: type");
    EXPECT(fill.order_type == ORDER_MARKET_SELL, "MakeFill: order_type");
    EXPECT(fill.core_id == 7, "MakeFill: core_id");
    EXPECT(approx(FPN_ToDouble(fill.price), 65000.0), "MakeFill: price");
    EXPECT(approx(FPN_ToDouble(fill.qty), 0.05), "MakeFill: qty");

    OrderEvent<64> rej = OrderEvent_MakeRejection<64>(
        99, 5678, ORDER_MARKET_BUY, 2, "insufficient balance");

    EXPECT(rej.order_id == 99, "MakeRejection: order_id");
    EXPECT(rej.type == OEVT_REJECTED, "MakeRejection: type");
    EXPECT(strcmp(rej.reason, "insufficient balance") == 0, "MakeRejection: reason");
}

//======================================================================================================
// MAIN
//======================================================================================================
int main() {
    fprintf(stderr, "test_order_event_log — phase 03 chunk 2 functional tests\n");
    fprintf(stderr, "=========================================================\n");

    test_init();
    test_append_monotonic_ids();
    test_fold_single_pair();
    test_fold_multi_core();
    test_fold_determinism();
    test_fold_ignores_non_fills();
    test_fold_open_position();
    test_event_helpers();

    fprintf(stderr, "=========================================================\n");
    if (failures == 0) {
        fprintf(stderr, "PASS — all OrderEventLog tests passed\n");
        return 0;
    } else {
        fprintf(stderr, "FAIL — %d test failure(s)\n", failures);
        return 1;
    }
}
