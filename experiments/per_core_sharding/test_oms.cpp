// test_oms.cpp — OMS phase 01+02 functional tests
//
// Validates the OrderManager state machine and the callback flow that
// connects the adapter worker thread back into OMS_Tick. Uses a synchronous
// MockAdapter (fires the callback inside submit_market_buy/sell) so the
// tests don't need real threading.
//
// What's covered:
//   1. OrderManager_Init produces clean state
//   2. Order_IsTerminal predicate covers all terminal states
//   3. Paper mode short-circuits Submit (no slot, no adapter call)
//   4. Live mode with null adapter rejects with error
//   5. Live mode with mock adapter allocates a slot, fires callback,
//      then Tick consumes the result and marks FILLED
//   6. Live mode rejection — adapter callback reports failure, Tick
//      marks REJECTED
//   7. Order table full — submitting MAX_INFLIGHT_ORDERS+1 orders drops
//      the overflow
//   8. Tick with no pending results is a no-op
//   9. Counters increment correctly across mixed paper/live submissions
//
// What's NOT covered (deferred):
//   - Real threading concurrency (test_oms_concurrent.cpp would do that
//     under TSan, similar to test_execution_core_concurrent.cpp)
//   - Real BinanceAdapter end-to-end (needs testnet credentials, lives
//     in the manual smoke test plan)
//   - Phase 03 event log integration (not built yet)

#include "CoreFrameworks/ExchangeAdapter.hpp"
#include "CoreFrameworks/Order.hpp"
#include "CoreFrameworks/OrderManager.hpp"

#include <cstdio>
#include <cstring>

using namespace tt;

static int failures = 0;

#define EXPECT(cond, msg)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);     \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

//======================================================================================================
// [MOCK ADAPTER]
//======================================================================================================
// Synchronous mock that fires the OMS callback inside submit_market_buy/sell.
// next_success / next_fill_price / next_fill_qty configure the next outcome;
// submit_count and last_qty give the test something to assert against.
//======================================================================================================
struct MockAdapter {
    int    next_success;          // 1 = the callback reports success
    double next_fill_price;
    double next_fill_qty;         // 0 = use the requested qty
    int    submit_count;
    double last_qty;
    OrderType last_type;
};

static int mock_submit_market_buy(void* ctx, uint64_t client_id, double qty,
                                   OrderCallback cb, void* user) {
    MockAdapter* m = (MockAdapter*)ctx;
    m->submit_count++;
    m->last_qty  = qty;
    m->last_type = ORDER_MARKET_BUY;

    OrderResult result;
    std::memset(&result, 0, sizeof(result));
    result.success = m->next_success;
    if (m->next_success) {
        std::strncpy(result.exchange_id, "MOCK_BUY_001",
                     sizeof(result.exchange_id) - 1);
        result.avg_fill_price = m->next_fill_price;
        result.fill_qty       = (m->next_fill_qty > 0) ? m->next_fill_qty : qty;
    } else {
        result.error_code = 999;
        std::strncpy(result.error_message, "mock buy failure",
                     sizeof(result.error_message) - 1);
    }
    cb(user, client_id, &result);
    return 1;
}

static int mock_submit_market_sell(void* ctx, uint64_t client_id, double qty,
                                    OrderCallback cb, void* user) {
    MockAdapter* m = (MockAdapter*)ctx;
    m->submit_count++;
    m->last_qty  = qty;
    m->last_type = ORDER_MARKET_SELL;

    OrderResult result;
    std::memset(&result, 0, sizeof(result));
    result.success = m->next_success;
    if (m->next_success) {
        std::strncpy(result.exchange_id, "MOCK_SELL_001",
                     sizeof(result.exchange_id) - 1);
        result.avg_fill_price = m->next_fill_price;
        result.fill_qty       = (m->next_fill_qty > 0) ? m->next_fill_qty : qty;
    } else {
        result.error_code = 999;
        std::strncpy(result.error_message, "mock sell failure",
                     sizeof(result.error_message) - 1);
    }
    cb(user, client_id, &result);
    return 1;
}

static int mock_get_balances(void*, double* base, double* quote) {
    *base = 1.0;
    *quote = 100000.0;
    return 1;
}

static int mock_query_order(void*, const char*, OrderResult* out) {
    std::memset(out, 0, sizeof(*out));
    return 1;
}

static void mock_shutdown(void*) {}

static ExchangeAdapter<64> mock_adapter_get(MockAdapter* state) {
    ExchangeAdapter<64> adapter;
    adapter.submit_market_buy  = mock_submit_market_buy;
    adapter.submit_market_sell = mock_submit_market_sell;
    adapter.get_balances       = mock_get_balances;
    adapter.query_order        = mock_query_order;
    adapter.shutdown           = mock_shutdown;
    adapter.ctx                = state;
    return adapter;
}

//======================================================================================================
// test 1: OrderManager_Init produces clean state
//======================================================================================================
static void test_init() {
    OrderManagerState<64> oms;
    ExchangeAdapter<64> empty{};
    OrderManager_Init(&oms, empty, /*live_trading=*/0);

    EXPECT(oms.order_bitmap == 0, "init: bitmap empty");
    EXPECT(oms.next_order_id == 1, "init: next_order_id starts at 1");
    EXPECT(oms.live_trading == 0, "init: live_trading = 0");
    EXPECT(OrderManager_TotalSubmitted(&oms) == 0, "init: submitted = 0");
    EXPECT(OrderManager_TotalFilled(&oms) == 0, "init: filled = 0");
    EXPECT(OrderManager_TotalRejected(&oms) == 0, "init: rejected = 0");
    EXPECT(OrderManager_InflightCount(&oms) == 0, "init: inflight = 0");
}

//======================================================================================================
// test 2: Order_IsTerminal covers all terminal states
//======================================================================================================
static void test_order_is_terminal() {
    Order<64> o;
    Order_Init(&o, 1, 0, ORDER_MARKET_BUY);

    o.state = ORDER_PENDING;       EXPECT(!Order_IsTerminal(&o), "PENDING not terminal");
    o.state = ORDER_SUBMITTED;     EXPECT(!Order_IsTerminal(&o), "SUBMITTED not terminal");
    o.state = ORDER_ACKNOWLEDGED;  EXPECT(!Order_IsTerminal(&o), "ACKNOWLEDGED not terminal");
    o.state = ORDER_PARTIAL;       EXPECT(!Order_IsTerminal(&o), "PARTIAL not terminal");
    o.state = ORDER_FILLED;        EXPECT(Order_IsTerminal(&o),  "FILLED terminal");
    o.state = ORDER_REJECTED;      EXPECT(Order_IsTerminal(&o),  "REJECTED terminal");
    o.state = ORDER_CANCELED;      EXPECT(Order_IsTerminal(&o),  "CANCELED terminal");
    o.state = ORDER_TIMEOUT;       EXPECT(Order_IsTerminal(&o),  "TIMEOUT terminal");
    o.state = ORDER_UNKNOWN;       EXPECT(!Order_IsTerminal(&o), "UNKNOWN not terminal (needs reconciliation)");
}

//======================================================================================================
// test 3: paper mode short-circuits Submit
//======================================================================================================
static void test_submit_paper_mode() {
    OrderManagerState<64> oms;
    MockAdapter mock{};
    ExchangeAdapter<64> adapter = mock_adapter_get(&mock);
    OrderManager_Init(&oms, adapter, /*live_trading=*/0);

    uint64_t id = OrderManager_Submit(&oms, /*core_id=*/0,
                                       ORDER_MARKET_BUY,
                                       FPN_FromDouble<64>(0.001));

    EXPECT(id == 1, "paper mode: returns next id");
    EXPECT(mock.submit_count == 0, "paper mode: adapter NOT called");
    EXPECT(OrderManager_InflightCount(&oms) == 0, "paper mode: no slot allocated");
    EXPECT(OrderManager_TotalSubmitted(&oms) == 1, "paper mode: submitted bumped");
    EXPECT(OrderManager_TotalFilled(&oms) == 1, "paper mode: filled bumped immediately");
    EXPECT(OrderManager_TotalRejected(&oms) == 0, "paper mode: rejected stays 0");
}

//======================================================================================================
// test 4: live mode with null adapter rejects
//======================================================================================================
static void test_submit_live_null_adapter() {
    OrderManagerState<64> oms;
    ExchangeAdapter<64> empty{};
    OrderManager_Init(&oms, empty, /*live_trading=*/1);

    uint64_t id = OrderManager_Submit(&oms, /*core_id=*/0,
                                       ORDER_MARKET_BUY,
                                       FPN_FromDouble<64>(0.001));

    EXPECT(id == 0, "live + null adapter: returns 0 (failure)");
    EXPECT(OrderManager_InflightCount(&oms) == 0, "live + null adapter: slot freed after rejection");
    EXPECT(OrderManager_TotalSubmitted(&oms) == 1, "live + null adapter: submitted bumped");
    EXPECT(OrderManager_TotalFilled(&oms) == 0, "live + null adapter: filled stays 0");
    EXPECT(OrderManager_TotalRejected(&oms) == 1, "live + null adapter: rejected bumped");
}

//======================================================================================================
// test 5: live mode with mock adapter — full success path
//======================================================================================================
static void test_submit_live_success() {
    OrderManagerState<64> oms;
    MockAdapter mock{};
    mock.next_success    = 1;
    mock.next_fill_price = 60000.0;
    mock.next_fill_qty   = 0;  // use requested
    ExchangeAdapter<64> adapter = mock_adapter_get(&mock);
    OrderManager_Init(&oms, adapter, /*live_trading=*/1);

    uint64_t id = OrderManager_Submit(&oms, /*core_id=*/2,
                                       ORDER_MARKET_BUY,
                                       FPN_FromDouble<64>(0.0015));

    EXPECT(id == 1, "live success: id assigned");
    EXPECT(mock.submit_count == 1, "live success: adapter called once");
    EXPECT(mock.last_type == ORDER_MARKET_BUY, "live success: adapter sees BUY type");
    EXPECT(mock.last_qty == 0.0015, "live success: adapter sees correct qty");
    EXPECT(OrderManager_TotalSubmitted(&oms) == 1, "live success: submitted bumped");
    // Mock adapter fired callback synchronously, so the result_queue has
    // a CMD_FILL_RESULT waiting. Tick should consume it and mark FILLED.
    OrderManager_Tick(&oms);
    EXPECT(OrderManager_TotalFilled(&oms) == 1, "live success: filled bumped after Tick");
    EXPECT(OrderManager_TotalRejected(&oms) == 0, "live success: rejected stays 0");
    EXPECT(OrderManager_InflightCount(&oms) == 0, "live success: slot freed after Tick");
}

//======================================================================================================
// test 6: live mode rejection — adapter reports failure
//======================================================================================================
static void test_submit_live_rejection() {
    OrderManagerState<64> oms;
    MockAdapter mock{};
    mock.next_success = 0;  // adapter will fail
    ExchangeAdapter<64> adapter = mock_adapter_get(&mock);
    OrderManager_Init(&oms, adapter, /*live_trading=*/1);

    uint64_t id = OrderManager_Submit(&oms, /*core_id=*/3,
                                       ORDER_MARKET_SELL,
                                       FPN_FromDouble<64>(0.002));

    EXPECT(id == 1, "live reject: id still assigned (the failure comes via the callback)");
    EXPECT(mock.submit_count == 1, "live reject: adapter called once");
    EXPECT(mock.last_type == ORDER_MARKET_SELL, "live reject: adapter sees SELL type");
    EXPECT(OrderManager_TotalSubmitted(&oms) == 1, "live reject: submitted bumped");

    OrderManager_Tick(&oms);
    EXPECT(OrderManager_TotalFilled(&oms) == 0, "live reject: filled stays 0");
    EXPECT(OrderManager_TotalRejected(&oms) == 1, "live reject: rejected bumped after Tick");
    EXPECT(OrderManager_InflightCount(&oms) == 0, "live reject: slot freed");
}

//======================================================================================================
// test 7: order table full — submitting beyond MAX_INFLIGHT_ORDERS drops the overflow
//======================================================================================================
static void test_submit_table_full() {
    OrderManagerState<64> oms;
    MockAdapter mock{};
    mock.next_success    = 1;
    mock.next_fill_price = 60000.0;
    ExchangeAdapter<64> adapter = mock_adapter_get(&mock);
    OrderManager_Init(&oms, adapter, /*live_trading=*/1);

    // Fill the order table without ticking. Each Submit allocates a slot
    // and the synchronous mock callback pushes a CMD_FILL_RESULT, but
    // without Tick to drain the result queue the slot stays held.
    // Wait — the synchronous mock fires the callback before Submit returns,
    // and the callback pushes to the result queue, but the slot itself is
    // held by the OMS until Tick consumes the result. So we can fill the
    // bitmap by submitting MAX_INFLIGHT_ORDERS times without ticking.
    int filled = 0;
    for (int i = 0; i < MAX_INFLIGHT_ORDERS; ++i) {
        uint64_t id = OrderManager_Submit(&oms, (int16_t)i,
                                           ORDER_MARKET_BUY,
                                           FPN_FromDouble<64>(0.001));
        if (id != 0) ++filled;
    }
    EXPECT(filled == MAX_INFLIGHT_ORDERS, "table full: all 16 slots fill");
    EXPECT(OrderManager_InflightCount(&oms) == MAX_INFLIGHT_ORDERS,
           "table full: bitmap reflects all slots used");

    // The 17th submission should drop with an error log (returns 0).
    uint64_t overflow = OrderManager_Submit(&oms, /*core_id=*/0,
                                             ORDER_MARKET_BUY,
                                             FPN_FromDouble<64>(0.001));
    EXPECT(overflow == 0, "table full: 17th submission drops");

    // Now tick to drain the result queue and free the slots. There were
    // MAX_INFLIGHT_ORDERS callbacks fired, so the queue should have that
    // many CMD_FILL_RESULTs to process.
    OrderManager_Tick(&oms);
    EXPECT(OrderManager_TotalFilled(&oms) == (uint64_t)MAX_INFLIGHT_ORDERS,
           "table full: tick processes all queued results");
    EXPECT(OrderManager_InflightCount(&oms) == 0,
           "table full: all slots freed after tick");
}

//======================================================================================================
// test 8: Tick with no pending results is a no-op
//======================================================================================================
static void test_tick_empty() {
    OrderManagerState<64> oms;
    ExchangeAdapter<64> empty{};
    OrderManager_Init(&oms, empty, /*live_trading=*/0);

    // Tick on fresh OMS — should not crash, should not change counters.
    OrderManager_Tick(&oms);
    EXPECT(OrderManager_TotalSubmitted(&oms) == 0, "empty tick: submitted unchanged");
    EXPECT(OrderManager_TotalFilled(&oms) == 0, "empty tick: filled unchanged");
    EXPECT(OrderManager_TotalRejected(&oms) == 0, "empty tick: rejected unchanged");
}

//======================================================================================================
// test 9: counters increment correctly across mixed live submissions
//======================================================================================================
static void test_mixed_live_submissions() {
    OrderManagerState<64> oms;
    MockAdapter mock{};
    mock.next_success    = 1;
    mock.next_fill_price = 60000.0;
    ExchangeAdapter<64> adapter = mock_adapter_get(&mock);
    OrderManager_Init(&oms, adapter, /*live_trading=*/1);

    // 3 successful submissions + tick
    OrderManager_Submit(&oms, 0, ORDER_MARKET_BUY,  FPN_FromDouble<64>(0.001));
    OrderManager_Submit(&oms, 1, ORDER_MARKET_BUY,  FPN_FromDouble<64>(0.001));
    OrderManager_Submit(&oms, 2, ORDER_MARKET_SELL, FPN_FromDouble<64>(0.001));
    OrderManager_Tick(&oms);

    EXPECT(OrderManager_TotalSubmitted(&oms) == 3, "mixed: 3 submitted");
    EXPECT(OrderManager_TotalFilled(&oms) == 3,    "mixed: 3 filled");
    EXPECT(OrderManager_TotalRejected(&oms) == 0,  "mixed: 0 rejected");

    // Switch mock to failure mode and submit one more
    mock.next_success = 0;
    OrderManager_Submit(&oms, 3, ORDER_MARKET_BUY, FPN_FromDouble<64>(0.001));
    OrderManager_Tick(&oms);

    EXPECT(OrderManager_TotalSubmitted(&oms) == 4, "mixed: 4 submitted");
    EXPECT(OrderManager_TotalFilled(&oms) == 3,    "mixed: 3 filled (still)");
    EXPECT(OrderManager_TotalRejected(&oms) == 1,  "mixed: 1 rejected");
    EXPECT(OrderManager_InflightCount(&oms) == 0,  "mixed: all slots freed");
}

//======================================================================================================
// MAIN
//======================================================================================================
int main() {
    fprintf(stderr, "test_oms — OMS phase 01+02 functional tests\n");
    fprintf(stderr, "============================================\n");

    test_init();
    test_order_is_terminal();
    test_submit_paper_mode();
    test_submit_live_null_adapter();
    test_submit_live_success();
    test_submit_live_rejection();
    test_submit_table_full();
    test_tick_empty();
    test_mixed_live_submissions();

    fprintf(stderr, "============================================\n");
    if (failures == 0) {
        fprintf(stderr, "PASS — all OMS tests passed\n");
        return 0;
    } else {
        fprintf(stderr, "FAIL — %d test failure(s)\n", failures);
        return 1;
    }
}
