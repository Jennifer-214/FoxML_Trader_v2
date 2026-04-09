// test_oms_concurrent.cpp — OMS phase 02 concurrent stress test
//
// Validates the SPSC contracts that phase 02 depends on, under realistic
// multi-threaded load. Run under TSan to catch races on:
//
//   1. The adapter submission queue (drainer thread → adapter worker thread).
//      This is what BinanceAdapter wraps with its real worker. Here we use a
//      stand-in AsyncTestAdapter that does the same SPSCRing dance without
//      the BinanceOrderAPI REST call.
//
//   2. The OMS result queue (adapter worker thread → drainer thread via
//      OrderManager_Tick). This is the load-bearing single-consumer queue
//      the OMS uses to fold async results back into the order table.
//
//   3. The atomic counters in OrderManagerState (total_submitted/filled/
//      rejected) which the TUI reads from a third thread but the test reads
//      from the drainer thread for assertions.
//
// What it does NOT validate:
//   - The real BinanceAdapter end-to-end (needs testnet credentials and
//     a real REST endpoint). The per-thread BinanceOrderAPI claim is
//     structural — code review verifies it, not this test.
//   - The OnEvent → OMS bridge in EngineSharded.hpp (that's a production
//     integration test, not a unit test).
//
// Build:
//   cmake -B build_tsan -DTSAN=ON
//   cmake --build build_tsan --target test_oms_concurrent
//   ./build_tsan/test_oms_concurrent
//
// Or in the regular build for a smoke check (won't catch races):
//   cmake --build build --target test_oms_concurrent
//   ./build/test_oms_concurrent

#include "CoreFrameworks/ExchangeAdapter.hpp"
#include "CoreFrameworks/Order.hpp"
#include "CoreFrameworks/OrderManager.hpp"
#include "CoreFrameworks/SPSCRing.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

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
// [ASYNC TEST ADAPTER]
//======================================================================================================
// Stand-in for BinanceAdapter that does the same SPSC dance without any
// REST. The drainer thread (test-side) calls submit_market_buy/sell which
// pushes a PendingSubmission onto the SPSC submission queue. The internal
// worker thread pops, builds a synthetic OrderResult, and invokes the
// callback (which pushes a CMD_FILL_RESULT into the OMS result queue).
//
// Mirrors the real BinanceAdapter's threading model exactly so TSan reports
// against this test apply 1:1 to the real adapter at runtime.
//======================================================================================================
struct AsyncTestPending {
    uint64_t      client_id;
    OrderType     type;
    uint8_t       _pad0[7];
    double        qty;
    OrderCallback cb;
    void*         user_ctx;
};

constexpr size_t ASYNC_TEST_QUEUE_SIZE = 256;

struct AsyncTestAdapter {
    SPSCRing<AsyncTestPending, ASYNC_TEST_QUEUE_SIZE> submission_queue;
    std::thread       worker;
    std::atomic<int>  shutdown_flag;
    std::atomic<uint64_t> processed_count;
    int               next_success;          // 1 = mark callbacks success, 0 = failure
    double            sim_fill_price;
};

static void async_worker_loop(AsyncTestAdapter* a) {
    while (a->shutdown_flag.load(std::memory_order_acquire) == 0) {
        AsyncTestPending p;
        if (!SPSCRing_TryPop(&a->submission_queue, &p)) {
            // Mimic the real BinanceAdapter sleep, scaled down for the test
            std::this_thread::sleep_for(std::chrono::microseconds(20));
            continue;
        }

        OrderResult result;
        std::memset(&result, 0, sizeof(result));
        if (a->next_success) {
            result.success        = 1;
            std::strncpy(result.exchange_id, "STRESS_TEST_001",
                         sizeof(result.exchange_id) - 1);
            result.avg_fill_price = a->sim_fill_price;
            result.fill_qty       = p.qty;
        } else {
            result.success    = 0;
            result.error_code = 999;
            std::strncpy(result.error_message, "stress test simulated failure",
                         sizeof(result.error_message) - 1);
        }
        if (p.cb) p.cb(p.user_ctx, p.client_id, &result);
        a->processed_count.fetch_add(1, std::memory_order_relaxed);
    }
    // Drain anything still queued at shutdown so we dont leak callbacks.
    AsyncTestPending p;
    while (SPSCRing_TryPop(&a->submission_queue, &p)) {
        OrderResult result;
        std::memset(&result, 0, sizeof(result));
        result.success = a->next_success;
        if (p.cb) p.cb(p.user_ctx, p.client_id, &result);
        a->processed_count.fetch_add(1, std::memory_order_relaxed);
    }
}

static int async_submit_market_buy(void* ctx, uint64_t client_id, double qty,
                                    OrderCallback cb, void* user) {
    AsyncTestAdapter* a = (AsyncTestAdapter*)ctx;
    AsyncTestPending p;
    p.client_id = client_id;
    p.type      = ORDER_MARKET_BUY;
    p.qty       = qty;
    p.cb        = cb;
    p.user_ctx  = user;
    return SPSCRing_TryPush(&a->submission_queue, p) ? 1 : 0;
}

static int async_submit_market_sell(void* ctx, uint64_t client_id, double qty,
                                     OrderCallback cb, void* user) {
    AsyncTestAdapter* a = (AsyncTestAdapter*)ctx;
    AsyncTestPending p;
    p.client_id = client_id;
    p.type      = ORDER_MARKET_SELL;
    p.qty       = qty;
    p.cb        = cb;
    p.user_ctx  = user;
    return SPSCRing_TryPush(&a->submission_queue, p) ? 1 : 0;
}

static int async_get_balances(void*, double* base, double* quote) {
    *base = 0.0; *quote = 100000.0; return 1;
}

static int async_query_order(void*, const char*, OrderResult* out) {
    std::memset(out, 0, sizeof(*out));
    return 1;
}

static void async_shutdown_impl(void*) {}

static void async_init(AsyncTestAdapter* a) {
    SPSCRing_Init(&a->submission_queue);
    a->shutdown_flag.store(0, std::memory_order_relaxed);
    a->processed_count.store(0, std::memory_order_relaxed);
    a->next_success    = 1;
    a->sim_fill_price  = 60000.0;
    a->worker = std::thread(async_worker_loop, a);
}

static void async_shutdown_join(AsyncTestAdapter* a) {
    a->shutdown_flag.store(1, std::memory_order_release);
    if (a->worker.joinable()) a->worker.join();
}

static ExchangeAdapter<64> async_adapter_get(AsyncTestAdapter* state) {
    ExchangeAdapter<64> adapter;
    adapter.submit_market_buy  = async_submit_market_buy;
    adapter.submit_market_sell = async_submit_market_sell;
    adapter.get_balances       = async_get_balances;
    adapter.query_order        = async_query_order;
    adapter.shutdown           = async_shutdown_impl;
    adapter.ctx                = state;
    return adapter;
}

//======================================================================================================
// test 1: sustained submit/tick under concurrent worker — happy path
//======================================================================================================
// 1000 successful orders with rate limiting (the OMS table holds 16, so we
// must wait for tick to drain before submitting more). Validates that the
// SPSC contracts hold across many round trips and the counters end up
// consistent.
//======================================================================================================
static void test_sustained_happy_path() {
    constexpr int NUM_ORDERS = 1000;

    AsyncTestAdapter mock;
    async_init(&mock);

    OrderManagerState<64> oms;
    ExchangeAdapter<64> adapter = async_adapter_get(&mock);
    OrderManager_Init(&oms, adapter, /*live_trading=*/1, FPN_Zero<64>(), FPN_Zero<64>());

    int submitted = 0;
    int tries = 0;
    constexpr int MAX_TRIES = NUM_ORDERS * 100;
    while (submitted < NUM_ORDERS && tries < MAX_TRIES) {
        ++tries;
        uint64_t id = OrderManager_Submit(&oms, /*core_id=*/0,
                                           ORDER_MARKET_BUY,
                                           FPN_FromDouble<64>(0.001));
        if (id != 0) ++submitted;
        OrderManager_Tick(&oms);
        std::this_thread::yield();
    }
    EXPECT(submitted == NUM_ORDERS, "happy path: all NUM_ORDERS submitted within retry budget");

    // Drain remaining results until everything is in a terminal state.
    int drain_tries = 0;
    constexpr int MAX_DRAIN = 100000;
    while ((OrderManager_TotalFilled(&oms) + OrderManager_TotalRejected(&oms)) < (uint64_t)NUM_ORDERS
           && drain_tries < MAX_DRAIN) {
        OrderManager_Tick(&oms);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        ++drain_tries;
    }

    async_shutdown_join(&mock);
    // Final tick to consume any results that arrived during the worker drain.
    OrderManager_Tick(&oms);

    EXPECT(OrderManager_TotalSubmitted(&oms) == (uint64_t)NUM_ORDERS,
           "happy path: total_submitted == NUM_ORDERS");
    EXPECT(OrderManager_TotalFilled(&oms) == (uint64_t)NUM_ORDERS,
           "happy path: total_filled == NUM_ORDERS");
    EXPECT(OrderManager_TotalRejected(&oms) == 0,
           "happy path: zero rejections");
    EXPECT(OrderManager_InflightCount(&oms) == 0,
           "happy path: all OMS slots freed");
    EXPECT(mock.processed_count.load() == (uint64_t)NUM_ORDERS,
           "happy path: worker processed all submissions");
}

//======================================================================================================
// test 2: sustained submit/tick with mixed rejection — failure path concurrency
//======================================================================================================
// Same shape as test 1 but the mock fires failures. Validates that the
// REJECTED accounting is race-free across the worker callback and the
// drainer's Tick.
//======================================================================================================
static void test_sustained_rejection_path() {
    constexpr int NUM_ORDERS = 500;

    AsyncTestAdapter mock;
    async_init(&mock);
    mock.next_success = 0;  // every callback reports failure

    OrderManagerState<64> oms;
    ExchangeAdapter<64> adapter = async_adapter_get(&mock);
    OrderManager_Init(&oms, adapter, /*live_trading=*/1, FPN_Zero<64>(), FPN_Zero<64>());

    int submitted = 0;
    int tries = 0;
    constexpr int MAX_TRIES = NUM_ORDERS * 100;
    while (submitted < NUM_ORDERS && tries < MAX_TRIES) {
        ++tries;
        uint64_t id = OrderManager_Submit(&oms, /*core_id=*/1,
                                           ORDER_MARKET_SELL,
                                           FPN_FromDouble<64>(0.002));
        if (id != 0) ++submitted;
        OrderManager_Tick(&oms);
        std::this_thread::yield();
    }
    EXPECT(submitted == NUM_ORDERS, "reject path: all NUM_ORDERS submitted");

    int drain_tries = 0;
    constexpr int MAX_DRAIN = 100000;
    while ((OrderManager_TotalFilled(&oms) + OrderManager_TotalRejected(&oms)) < (uint64_t)NUM_ORDERS
           && drain_tries < MAX_DRAIN) {
        OrderManager_Tick(&oms);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        ++drain_tries;
    }

    async_shutdown_join(&mock);
    OrderManager_Tick(&oms);

    EXPECT(OrderManager_TotalSubmitted(&oms) == (uint64_t)NUM_ORDERS,
           "reject path: total_submitted == NUM_ORDERS");
    EXPECT(OrderManager_TotalFilled(&oms) == 0,
           "reject path: zero fills");
    EXPECT(OrderManager_TotalRejected(&oms) == (uint64_t)NUM_ORDERS,
           "reject path: all rejected");
    EXPECT(OrderManager_InflightCount(&oms) == 0,
           "reject path: all slots freed");
}

//======================================================================================================
// test 3: rapid burst submission — table-full pressure under concurrent worker
//======================================================================================================
// Submits orders without yielding between them so the OMS table fills up
// fast. Verifies that table-full drops are counted and that the system
// stays consistent (no double-fills, no negative counters).
//======================================================================================================
static void test_burst_with_table_pressure() {
    constexpr int BURST_TRIES = 100;

    AsyncTestAdapter mock;
    async_init(&mock);

    OrderManagerState<64> oms;
    ExchangeAdapter<64> adapter = async_adapter_get(&mock);
    OrderManager_Init(&oms, adapter, /*live_trading=*/1, FPN_Zero<64>(), FPN_Zero<64>());

    // Burst BURST_TRIES submissions without ticking between them — most will
    // fail with table-full after the first 16 (since theres no Tick to free
    // slots between submits, but the worker might drain a few via callbacks).
    int submitted_ids = 0;
    int dropped       = 0;
    for (int i = 0; i < BURST_TRIES; ++i) {
        uint64_t id = OrderManager_Submit(&oms, /*core_id=*/0,
                                           ORDER_MARKET_BUY,
                                           FPN_FromDouble<64>(0.001));
        if (id != 0) ++submitted_ids;
        else         ++dropped;
    }
    // submitted + dropped == BURST_TRIES (every call returns either an id or 0).
    EXPECT(submitted_ids + dropped == BURST_TRIES, "burst: every call accounted for");
    EXPECT(submitted_ids > 0, "burst: at least some submissions accepted");
    EXPECT(submitted_ids <= BURST_TRIES, "burst: cant exceed total tries");

    // Drain everything that the worker has buffered.
    int drain_tries = 0;
    constexpr int MAX_DRAIN = 100000;
    while ((OrderManager_TotalFilled(&oms) + OrderManager_TotalRejected(&oms))
                < (uint64_t)submitted_ids
           && drain_tries < MAX_DRAIN) {
        OrderManager_Tick(&oms);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        ++drain_tries;
    }

    async_shutdown_join(&mock);
    OrderManager_Tick(&oms);

    // Conservation: total_submitted (which the OMS bumps for accepted slots)
    // must equal the count we observed accepted on the producer side.
    EXPECT(OrderManager_TotalSubmitted(&oms) == (uint64_t)submitted_ids,
           "burst: OMS submitted counter matches accepted count");
    // All accepted orders eventually reach a terminal state.
    EXPECT(OrderManager_TotalFilled(&oms) + OrderManager_TotalRejected(&oms)
               == (uint64_t)submitted_ids,
           "burst: every accepted order reached terminal");
    EXPECT(OrderManager_InflightCount(&oms) == 0,
           "burst: all slots freed at end");
}

//======================================================================================================
// test 4: zero ticks during submit — only the worker is firing callbacks
//======================================================================================================
// Verifies that even without OMS_Tick interspersed, callbacks pushed into
// the result queue from the worker thread don't corrupt the queue. The
// final Tick after worker shutdown drains everything.
//
// This is the "drainer is busy with other work for a long time" scenario.
// The result queue must hold up under sustained writes from one thread
// without any reads.
//======================================================================================================
static void test_no_intermediate_tick() {
    constexpr int NUM_ORDERS = 16;  // table size — fits without needing tick

    AsyncTestAdapter mock;
    async_init(&mock);

    OrderManagerState<64> oms;
    ExchangeAdapter<64> adapter = async_adapter_get(&mock);
    OrderManager_Init(&oms, adapter, /*live_trading=*/1, FPN_Zero<64>(), FPN_Zero<64>());

    int submitted = 0;
    for (int i = 0; i < NUM_ORDERS; ++i) {
        uint64_t id = OrderManager_Submit(&oms, /*core_id=*/0,
                                           ORDER_MARKET_BUY,
                                           FPN_FromDouble<64>(0.001));
        if (id != 0) ++submitted;
        // No Tick here — let the worker buffer all results in the queue.
    }
    EXPECT(submitted == NUM_ORDERS, "no-tick: all 16 fit before any Tick");

    // Wait for the worker to process everything we submitted.
    int wait_tries = 0;
    constexpr int MAX_WAIT = 10000;
    while (mock.processed_count.load() < (uint64_t)submitted && wait_tries < MAX_WAIT) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        ++wait_tries;
    }
    EXPECT(mock.processed_count.load() == (uint64_t)submitted,
           "no-tick: worker processed every submission");

    async_shutdown_join(&mock);

    // NOW tick to drain the result queue. All results should arrive in one go.
    OrderManager_Tick(&oms);

    EXPECT(OrderManager_TotalFilled(&oms) == (uint64_t)submitted,
           "no-tick: final tick drained all results");
    EXPECT(OrderManager_InflightCount(&oms) == 0,
           "no-tick: all slots freed after final tick");
}

//======================================================================================================
// MAIN
//======================================================================================================
int main() {
    fprintf(stderr, "test_oms_concurrent — OMS phase 02 stress test under real worker thread\n");
    fprintf(stderr, "(run under TSan to validate the SPSC contracts)\n");
    fprintf(stderr, "=======================================================================\n");

    test_sustained_happy_path();
    test_sustained_rejection_path();
    test_burst_with_table_pressure();
    test_no_intermediate_tick();

    fprintf(stderr, "=======================================================================\n");
    if (failures == 0) {
        fprintf(stderr, "PASS — all OMS concurrent tests passed\n");
        return 0;
    } else {
        fprintf(stderr, "FAIL — %d test failure(s)\n", failures);
        return 1;
    }
}
