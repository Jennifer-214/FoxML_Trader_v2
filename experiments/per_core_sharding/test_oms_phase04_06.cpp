// test_oms_phase04_06.cpp — OMS phases 04-06 functional tests
//
// Validates the new code paths introduced in phases 04-06:
//   1. WS fill via ws_result_queue (CMD_WS_FILL)
//   2. ACK-only REST result → ORDER_ACKNOWLEDGED
//   3. WS fill dedup (order already FILLED by REST)
//   4. Surprise fill (order_id == 0) skipped
//   5. CMD_RECONCILE balance correction
//   6. HandleFill entry + exit (extracted helper)
//   7. Unified ProcessFillCommand dispatch

#include "CoreFrameworks/ExchangeAdapter.hpp"
#include "CoreFrameworks/Order.hpp"
#include "CoreFrameworks/OrderManager.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>

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

static bool approx(double a, double b, double tol = 0.01) {
    return std::fabs(a - b) < tol;
}

// Helper: create a paper-mode OMS with mode 1 (event log) enabled.
static void init_oms_mode1(OrderManagerState<64>* oms, double balance = 10000.0) {
    ExchangeAdapter<64> empty{};
    OrderManager_Init(oms, empty, /*live_trading=*/0,
                      FPN_FromDouble<64>(balance),
                      FPN_FromDouble<64>(0.001),  // 0.1% fee
                      /*event_log_mode=*/1);
}

// Helper: push a fill Command directly into a queue.
static void push_fill(SPSCRing<Command, OMS_RESULT_QUEUE_SIZE>* q,
                       uint8_t cmd_type, uint64_t order_id,
                       double price, double qty) {
    Command cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type     = cmd_type;
    cmd.order_id = order_id;
    cmd.result.success        = 1;
    cmd.result.avg_fill_price = price;
    cmd.result.fill_qty       = qty;
    strncpy(cmd.result.exchange_id, "TEST_001",
            sizeof(cmd.result.exchange_id) - 1);
    SPSCRing_TryPush(q, cmd);
}

// Helper: push an ACK-only Command (price=0, qty=0).
static void push_ack(SPSCRing<Command, OMS_RESULT_QUEUE_SIZE>* q,
                      uint64_t order_id) {
    Command cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type     = CMD_FILL_RESULT;
    cmd.order_id = order_id;
    cmd.result.success        = 1;
    cmd.result.avg_fill_price = 0.0;
    cmd.result.fill_qty       = 0.0;
    strncpy(cmd.result.exchange_id, "ACK_001",
            sizeof(cmd.result.exchange_id) - 1);
    SPSCRing_TryPush(q, cmd);
}

//======================================================================================================
// test 1: WS fill via ws_result_queue marks order FILLED
//======================================================================================================
static void test_ws_fill() {
    OrderManagerState<64> oms;
    init_oms_mode1(&oms);

    // Submit a buy order (mode 1 paper: allocates slot, pushes synthetic fill
    // to result_queue). But we'll manually test the WS path instead.
    // First, manually allocate a slot in SUBMITTED state.
    oms.order_bitmap |= 1;  // slot 0
    Order_Init(&oms.orders[0], 42, 0, ORDER_MARKET_BUY);
    oms.orders[0].state = ORDER_SUBMITTED;
    oms.orders[0].requested_qty = FPN_FromDouble<64>(0.01);
    oms.orders[0].intended_tp   = FPN_FromDouble<64>(61000.0);
    oms.orders[0].intended_sl   = FPN_FromDouble<64>(59000.0);
    oms.orders[0].strategy_id   = 2;

    // Push a WS fill into ws_result_queue
    push_fill(&oms.ws_result_queue, CMD_WS_FILL, 42, 60000.0, 0.01);

    // Tick should process it
    OrderManager_Tick(&oms);

    EXPECT(oms.orders[0].state == ORDER_FILLED, "WS fill: order marked FILLED");
    EXPECT(OrderManager_TotalFilled(&oms) == 1, "WS fill: total_filled == 1");
    EXPECT(oms.order_bitmap == 0, "WS fill: slot freed");
    // Mode 1: portfolio should have the position
    EXPECT(oms.portfolio.active_bitmap == 1, "WS fill: portfolio slot 0 active");
    EXPECT(approx(FPN_ToDouble(oms.portfolio.positions[0].entry_price), 60000.0),
           "WS fill: entry_price correct");

    OrderManager_Shutdown(&oms);
}

//======================================================================================================
// test 2: ACK-only REST result → ORDER_ACKNOWLEDGED (slot stays open)
//======================================================================================================
static void test_ack_only() {
    OrderManagerState<64> oms;
    init_oms_mode1(&oms);

    oms.order_bitmap |= 1;
    Order_Init(&oms.orders[0], 99, 0, ORDER_MARKET_BUY);
    oms.orders[0].state = ORDER_SUBMITTED;

    // Push ACK-only (price=0, qty=0)
    push_ack(&oms.result_queue, 99);

    OrderManager_Tick(&oms);

    EXPECT(oms.orders[0].state == ORDER_ACKNOWLEDGED, "ACK: state is ACKNOWLEDGED");
    EXPECT(oms.order_bitmap == 1, "ACK: slot NOT freed (waiting for WS fill)");
    EXPECT(OrderManager_TotalFilled(&oms) == 0, "ACK: total_filled still 0");
    EXPECT(oms.portfolio.active_bitmap == 0, "ACK: portfolio not mutated yet");

    OrderManager_Shutdown(&oms);
}

//======================================================================================================
// test 3: ACK → WS fill sequence (the production live path)
//======================================================================================================
static void test_ack_then_ws_fill() {
    OrderManagerState<64> oms;
    init_oms_mode1(&oms);

    oms.order_bitmap |= 1;
    Order_Init(&oms.orders[0], 77, 0, ORDER_MARKET_BUY);
    oms.orders[0].state         = ORDER_SUBMITTED;
    oms.orders[0].requested_qty = FPN_FromDouble<64>(0.005);
    oms.orders[0].intended_tp   = FPN_FromDouble<64>(61000.0);
    oms.orders[0].intended_sl   = FPN_FromDouble<64>(59000.0);

    // Step 1: REST ACK (no fill data)
    push_ack(&oms.result_queue, 77);
    OrderManager_Tick(&oms);
    EXPECT(oms.orders[0].state == ORDER_ACKNOWLEDGED, "ACK+WS: acknowledged after REST");

    // Step 2: WS fill with actual fill data
    push_fill(&oms.ws_result_queue, CMD_WS_FILL, 77, 60100.0, 0.005);
    OrderManager_Tick(&oms);
    EXPECT(oms.orders[0].state == ORDER_FILLED, "ACK+WS: filled after WS");
    EXPECT(oms.order_bitmap == 0, "ACK+WS: slot freed");
    EXPECT(oms.portfolio.active_bitmap == 1, "ACK+WS: position opened");

    OrderManager_Shutdown(&oms);
}

//======================================================================================================
// test 4: WS fill dedup — order already FILLED by REST
//======================================================================================================
static void test_ws_dedup() {
    OrderManagerState<64> oms;
    init_oms_mode1(&oms);

    // Submit via paper mode 1 (pushes synthetic fill to result_queue)
    uint64_t id = OrderManager_Submit(&oms, 0, ORDER_MARKET_BUY,
                                       FPN_FromDouble<64>(0.01),
                                       FPN_FromDouble<64>(61000.0),
                                       FPN_FromDouble<64>(59000.0),
                                       2, FPN_FromDouble<64>(60000.0));

    // Tick processes the REST fill → order FILLED, slot freed
    OrderManager_Tick(&oms);
    EXPECT(OrderManager_TotalFilled(&oms) == 1, "dedup: REST fill processed");
    EXPECT(oms.order_bitmap == 0, "dedup: slot freed by REST");

    // Now push a duplicate WS fill for the same order_id
    push_fill(&oms.ws_result_queue, CMD_WS_FILL, id, 60000.0, 0.01);
    OrderManager_Tick(&oms);

    // Should be silently skipped (slot not found, order already freed)
    EXPECT(OrderManager_TotalFilled(&oms) == 1, "dedup: total_filled still 1");

    OrderManager_Shutdown(&oms);
}

//======================================================================================================
// test 5: surprise fill (order_id == 0) is skipped
//======================================================================================================
static void test_surprise_fill() {
    OrderManagerState<64> oms;
    init_oms_mode1(&oms);

    push_fill(&oms.ws_result_queue, CMD_WS_FILL, 0, 60000.0, 0.01);
    OrderManager_Tick(&oms);

    EXPECT(OrderManager_TotalFilled(&oms) == 0, "surprise: no fill processed");
    EXPECT(oms.portfolio.active_bitmap == 0, "surprise: portfolio unchanged");

    OrderManager_Shutdown(&oms);
}

//======================================================================================================
// test 6: CMD_RECONCILE corrects balance
//======================================================================================================
static void test_reconcile_correction() {
    OrderManagerState<64> oms;
    init_oms_mode1(&oms, 10000.0);

    // Verify starting balance
    EXPECT(approx(FPN_ToDouble(oms.balance), 10000.0), "reconcile: starting balance");

    // Push a CMD_RECONCILE saying the exchange has $9500
    Command cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_RECONCILE;
    cmd.order_id = 0;
    cmd.result.success = 1;
    cmd.result.avg_fill_price = -500.0;   // drift (repurposed)
    cmd.result.fill_qty       = 9500.0;   // exchange balance (repurposed)
    strncpy(cmd.result.error_message, "drift=-500.0",
            sizeof(cmd.result.error_message) - 1);
    SPSCRing_TryPush(&oms.reconcile_queue, cmd);

    OrderManager_Tick(&oms);

    EXPECT(approx(FPN_ToDouble(oms.balance), 9500.0),
           "reconcile: balance corrected to exchange value");
    // Event log should have an OEVT_RECONCILED entry
    EXPECT(oms.event_log.count == 1, "reconcile: event log has 1 entry");
    EXPECT(oms.event_log.entries[0].type == OEVT_RECONCILED,
           "reconcile: event type is RECONCILED");

    OrderManager_Shutdown(&oms);
}

//======================================================================================================
// test 7: HandleFill entry+exit with correct P&L
//======================================================================================================
// entry at $60000, qty 0.01, exit at $60100
// same math as test_order_event_log.cpp test 3
static void test_handle_fill_pnl() {
    OrderManagerState<64> oms;
    init_oms_mode1(&oms, 10000.0);

    // Manually create entry order
    oms.order_bitmap |= 1;
    Order_Init(&oms.orders[0], 1, 0, ORDER_MARKET_BUY);
    oms.orders[0].state         = ORDER_SUBMITTED;
    oms.orders[0].requested_qty = FPN_FromDouble<64>(0.01);
    oms.orders[0].intended_tp   = FPN_FromDouble<64>(60500.0);
    oms.orders[0].intended_sl   = FPN_FromDouble<64>(59500.0);

    // Entry fill via WS
    push_fill(&oms.ws_result_queue, CMD_WS_FILL, 1, 60000.0, 0.01);
    OrderManager_Tick(&oms);
    EXPECT(oms.portfolio.active_bitmap == 1, "pnl: slot 0 active after entry");
    EXPECT(approx(FPN_ToDouble(oms.balance), 10000.0), "pnl: balance unchanged on entry");

    // Exit order
    oms.order_bitmap |= 1;
    Order_Init(&oms.orders[0], 2, 0, ORDER_MARKET_SELL);
    oms.orders[0].state         = ORDER_SUBMITTED;
    oms.orders[0].requested_qty = FPN_FromDouble<64>(0.01);

    // Exit fill via WS
    push_fill(&oms.ws_result_queue, CMD_WS_FILL, 2, 60100.0, 0.01);
    OrderManager_Tick(&oms);
    EXPECT(oms.portfolio.active_bitmap == 0, "pnl: slot 0 closed after exit");

    // gross = (60100 - 60000) * 0.01 = $1.00
    // entry fee = 60000 * 0.01 * 0.001 = $0.60
    // exit fee = 60100 * 0.01 * 0.001 = $0.601
    // net = 1.00 - 0.60 - 0.601 = -$0.201
    // balance = 10000 + (-0.201) = $9999.799
    EXPECT(approx(FPN_ToDouble(oms.balance), 9999.799, 0.01),
           "pnl: balance correct after round trip");
    EXPECT(approx(FPN_ToDouble(oms.realized_pnl), -0.201, 0.01),
           "pnl: realized_pnl correct");

    // Verify event log has both fills
    EXPECT(oms.event_log.count == 2, "pnl: event log has 2 fills");

    OrderManager_Shutdown(&oms);
}

//======================================================================================================
// test 8: mixed REST and WS fills through unified dispatch
//======================================================================================================
static void test_mixed_sources() {
    OrderManagerState<64> oms;
    init_oms_mode1(&oms);

    // Order 1: REST fill (result_queue)
    oms.order_bitmap |= 1;
    Order_Init(&oms.orders[0], 10, 0, ORDER_MARKET_BUY);
    oms.orders[0].state         = ORDER_SUBMITTED;
    oms.orders[0].requested_qty = FPN_FromDouble<64>(0.01);
    oms.orders[0].intended_tp   = FPN_FromDouble<64>(61000.0);
    oms.orders[0].intended_sl   = FPN_FromDouble<64>(59000.0);
    push_fill(&oms.result_queue, CMD_FILL_RESULT, 10, 60000.0, 0.01);

    // Order 2: WS fill (ws_result_queue)
    oms.order_bitmap |= 2;
    Order_Init(&oms.orders[1], 11, 1, ORDER_MARKET_BUY);
    oms.orders[1].state         = ORDER_SUBMITTED;
    oms.orders[1].requested_qty = FPN_FromDouble<64>(0.02);
    oms.orders[1].intended_tp   = FPN_FromDouble<64>(61000.0);
    oms.orders[1].intended_sl   = FPN_FromDouble<64>(59000.0);
    push_fill(&oms.ws_result_queue, CMD_WS_FILL, 11, 60050.0, 0.02);

    // One Tick processes both
    OrderManager_Tick(&oms);

    EXPECT(OrderManager_TotalFilled(&oms) == 2, "mixed: both fills processed");
    EXPECT(oms.order_bitmap == 0, "mixed: both slots freed");
    EXPECT(oms.portfolio.active_bitmap == 3, "mixed: both positions open");

    OrderManager_Shutdown(&oms);
}

//======================================================================================================
// MAIN
//======================================================================================================
int main() {
    fprintf(stderr, "test_oms_phase04_06 — phases 04-06 functional tests\n");
    fprintf(stderr, "====================================================\n");

    test_ws_fill();
    test_ack_only();
    test_ack_then_ws_fill();
    test_ws_dedup();
    test_surprise_fill();
    test_reconcile_correction();
    test_handle_fill_pnl();
    test_mixed_sources();

    fprintf(stderr, "====================================================\n");
    fprintf(stderr, "%s — %d assertions, %d failures\n",
            failures == 0 ? "PASS" : "FAIL", assertions, failures);
    return failures == 0 ? 0 : 1;
}
