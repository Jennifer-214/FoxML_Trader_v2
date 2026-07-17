// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/BinanceAdapter.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[concrete ExchangeAdapter over BinanceOrderAPI — SPSC drainer->worker submission queue; PER-THREAD API instances, never a shared one with a mutex]
// [CONTAINS]
//   - [STRUCT]_[PendingSubmission]
//   - [STRUCT]_[BinanceAdapterState]
//   - [FUNCTION]_[BinanceAdapter_WorkerLoop]
//   - [FUNCTION]_[BinanceAdapter_Init]
//   - [FUNCTION]_[BinanceAdapter_ShutdownState]
//   - [FUNCTION]_[BinanceAdapter_SubmitMarketBuy]
//   - [FUNCTION]_[BinanceAdapter_SubmitMarketSell]
//   - [FUNCTION]_[BinanceAdapter_GetBalancesImpl]
//   - [FUNCTION]_[BinanceAdapter_QueryOrderImpl]
//   - [FUNCTION]_[BinanceAdapter_ShutdownImpl]
//   - [FUNCTION]_[BinanceAdapter_Get]
//======================================================================================================
// Concrete ExchangeAdapter implementation that wraps the existing
// DataStream/BinanceOrderAPI.hpp behind an async submission queue.
// Phase 02 of the OMS plan — see plans/oms/02_async_submission/plan.md.
//
// THREAD-SAFETY MODEL (read this before changing anything):
//
//   BinanceOrderAPI is NOT thread-safe. Each instance has mutable
//   per-call state (sockfd, ssl, connected — see the BinanceOrderAPI
//   struct in DataStream/BinanceOrderAPI.hpp) that races under concurrent
//   calls. Two threads in BinanceOrderAPI_MarketBuy on the same instance
//   would corrupt sockfd/ssl. The 2026-04-08 OMS survey caught this and
//   the master plan now states it explicitly.
//
//   The fix: PER-THREAD BinanceOrderAPI instances. Each worker thread in
//   the adapter owns exactly one BinanceOrderAPI in workers_api[i] and
//   never touches any other slot. No locks. No sharing. The sockfd/ssl
//   state stays local to one thread for its entire lifetime.
//
//   NEVER refactor this to use a single BinanceOrderAPI with a mutex.
//   That adds contention for no benefit AND doesn't solve the back-to-back
//   heap corruption history at CHANGELOG.md 3.0.19 (2026-03-24). The
//   per-thread approach is intentional, not lazy.
//
// WORKER POOL SIZE:
//
//   Phase 02 ships with worker_count = 1. The 2026-04-08 survey
//   recommended starting at 1 to validate the async pipeline without
//   exposing the back-to-back heap corruption class. Once a 24-hour
//   testnet soak is clean, scale to 2-4 in a follow-on commit.
//
//   Trade-off when scaling: 1 worker serializes orders (~50-200 ms each
//   on testnet) but maintains arrival order. N workers complete in
//   parallel but lose arrival order. For single-asset trading the
//   ordering loss is mostly cosmetic.
//
// QUEUE MODEL:
//
//   submission_queue: SPSC drainer→worker. Drainer thread (the only
//   caller of OrderManager_Submit) pushes; the worker thread pops and
//   submits to Binance. With worker_count=1 this is straightforward
//   SPSC. When scaling to multiple workers, replace with per-worker
//   queues or a real MPMC ring — the SPSC contract breaks under
//   multiple consumers.
//
//   The completion callback (OrderManager's CMD_FILL_RESULT path) flows
//   back to the OMS via the OMS's own result_queue, not through this
//   adapter. The adapter just calls the callback function pointer the
//   OMS provided at submit time.
//======================================================================================================

#pragma once

#include "../DataStream/BinanceOrderAPI.hpp"
#include "../Limits.hpp"
#include "ExchangeAdapter.hpp"
#include "Order.hpp"
#include "ShardedOrderLatency.hpp"
#include "SPSCRing.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace tt {

//======================================================================
// [STRUCT]_[PendingSubmission]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SYNC]_[SPSC]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one queued order — trivially-copyable SPSC ring MESSAGE (copied by value drainer->worker, never shared in place)]
//======================================================================
// [CODE]
//======================================================================
struct PendingSubmission {
    uint64_t      client_id;
    OrderType     type;
    uint8_t       _pad0[7];
    double        qty;
    OrderCallback cb;
    void*         user_ctx;
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// One pending order awaiting submission to Binance. Fits in the SPSC
// ring; trivially copyable as required by SPSCRing's static_assert.
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
// [SIZE]_[40B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[PendingSubmission]
//======================================================================

//======================================================================
// [STRUCT]_[BinanceAdapterState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-thread BinanceOrderAPI slots (workers_api[i] owned by worker i — no locks, no sharing) + SPSC submission queue + shutdown/ws_active atomics]
//======================================================================
// [CODE]
//======================================================================
constexpr size_t BINANCE_ADAPTER_QUEUE_SIZE = 256;

struct BinanceAdapterState {
    // One BinanceOrderAPI per worker thread. Each worker accesses ONLY
    // its own slot (workers_api[worker_index]). Never share, never lock.
    BinanceOrderAPI workers_api[MAX_BINANCE_WORKERS];

    // Submission queue. Drainer thread (single producer) pushes via
    // BinanceAdapter_SubmitMarketBuy/Sell; worker thread (single
    // consumer when worker_count == 1) pops and submits.
    SPSCRing<PendingSubmission, BINANCE_ADAPTER_QUEUE_SIZE> submission_queue;

    // Worker threads. workers[i] uses workers_api[i] exclusively.
    std::thread workers[MAX_BINANCE_WORKERS];
    int         worker_count;       // active worker threads (start: 1)

    std::atomic<int> shutdown_requested;

    // Optional latency tracker. When non-null, the worker thread brackets
    // each REST round trip with steady_clock and samples the elapsed
    // microseconds. Phase 01 lived inside OMS_Tick; phase 02 moves it
    // here because the REST call now happens on the worker thread, not
    // the drainer thread. The instance still lives with the run loop
    // (g_sharded_order_lat in EngineSharded/Run.hpp) so the existing
    // TUI line keeps working unchanged.
    ShardedOrderLatency* latency;

    // Stats: dropped submissions when the queue was full. Bumped from
    // the producer side (drainer thread). Atomic so the TUI / tests can
    // read without ordering surprises.
    std::atomic<uint64_t> dropped_submissions;

    // Phase 04: when ws_active == 1, the user data websocket is receiving
    // real-time fills. The worker loop's callback changes behavior: it
    // sends an ACK (exchange_id only, no fill data) instead of a full
    // fill result. The order transitions to ORDER_ACKNOWLEDGED, and the
    // actual fill comes later via the WS queue. When ws_active == 0
    // (WS disconnected or not started), existing phase-02 behavior is
    // preserved — full fill data in the callback.
    std::atomic<int> ws_active;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
// [SIZE]_[12416B]
// [ALIGN]_[64]
// [CACHE_LINES]_[194]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[BinanceAdapterState]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceAdapter_WorkerLoop]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[worker loop — adaptive spin-then-sleep pop, REST call on THIS worker's own API instance, latency bracket, ACK-vs-full-fill callback]
//======================================================================
// [CODE]
//======================================================================
static inline void BinanceAdapter_WorkerLoop(BinanceAdapterState* state, int worker_index) {
    BinanceOrderAPI* api = &state->workers_api[worker_index];

    // v5.11.5.A — adaptive spin-then-sleep replaces the prior fixed
    // sleep_for(200μs). Hot-burst orders (the latency-critical case)
    // get picked up within ~3-6μs of arrival via tight pause-spin;
    // sustained idle falls back to a brief sleep so the worker doesn't
    // burn a core during quiet periods.
    //
    // Trade-off geometry:
    //   - SPIN_BUDGET=2048 pauses ≈ 3-6μs on modern Intel (pause is
    //     5-15 cycles). Worker is "at full attention" for that long
    //     after each completed order.
    //   - SLEEP_US=50 caps the worst-case pickup at ~50μs vs the
    //     prior 200μs, even after long idle.
    //
    // Audit: LATENCY_OPTIMIZATION_AUDIT.md Part 4.2 — "kill 200μs
    // tail on order submit". Closes plans/2026-05-06-MASTER v5.11.5
    // item 1. Futex / cond_var notification deferred (would add
    // producer-side overhead; defer until p99 measurement shows the
    // adaptive spin floor isn't sufficient).
    constexpr int SPIN_BUDGET = 2048;
    int idle_spins = 0;

    while (state->shutdown_requested.load(std::memory_order_acquire) == 0) {
        PendingSubmission p;
        if (!SPSCRing_TryPop(&state->submission_queue, &p)) {
            if (idle_spins < SPIN_BUDGET) {
                __builtin_ia32_pause();
                idle_spins++;
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            continue;
        }
        idle_spins = 0;  // reset budget after each successful pickup

        // Build the result struct on the stack so the callback gets a
        // self-contained copy. error_message is zeroed in case the
        // success path doesn't touch it.
        OrderResult result;
        std::memset(&result, 0, sizeof(result));

        char order_id_buf[32] = {};
        double fill_price = 0.0;
        double fill_qty   = 0.0;
        int    order_complete = 0;   // A17: venue-reported terminal status; consumed in the genuine-fill arm only

        // Bracket the REST call for latency stats. Phase 01 did this on
        // the drainer thread inside OMS_Tick; phase 02 does it here on
        // the worker thread because that's where the REST call now lives.
        auto ts0 = std::chrono::steady_clock::now();
        int ok = (p.type == ORDER_MARKET_BUY)
            ? BinanceOrderAPI_MarketBuy(api, p.qty, order_id_buf,
                                         &fill_price, &fill_qty, p.client_id, &order_complete)
            : BinanceOrderAPI_MarketSell(api, p.qty, order_id_buf,
                                          &fill_price, &fill_qty, p.client_id, &order_complete);
        auto ts1 = std::chrono::steady_clock::now();
        if (state->latency) {
            uint64_t elapsed_us = (uint64_t)
                std::chrono::duration_cast<std::chrono::microseconds>(ts1 - ts0).count();
            ShardedOrderLatency_Sample(state->latency, elapsed_us, ok);
        }

        if (ok) {
            result.success = 1;
            std::strncpy(result.exchange_id, order_id_buf,
                         sizeof(result.exchange_id) - 1);
            result.exchange_id[sizeof(result.exchange_id) - 1] = '\0';
            result.error_code = 0;
            // Phase 04: when ws_active, send ACK only (exchange_id populated
            // but no fill data). The actual fill comes via the WS queue.
            // When ws is inactive, send the full fill data (phase 02 behavior).
            if (state->ws_active.load(std::memory_order_relaxed)) {
                result.avg_fill_price = 0.0;
                result.fill_qty       = 0.0;
            } else {
                result.avg_fill_price = fill_price;
                result.fill_qty       = fill_qty;
                result.order_complete = (uint8_t)order_complete;  // A17 (b): venue "status"==FILLED; ACK arm leaves 0
            }
        } else {
            result.success = 0;
            result.error_code = api->last_error_code;  // actual Binance error code
            std::snprintf(result.error_message, sizeof(result.error_message),
                          "REST error (binance code %d, weight %d)",
                          api->last_error_code, api->rate_limit_weight);
        }

        // Fire the callback. The OMS's callback pushes a CMD_FILL_RESULT
        // into its result_queue and returns immediately, so this stays
        // bounded. If the callback is null (shouldn't happen in practice
        // but defensive), drop the result.
        if (p.cb != nullptr) {
            p.cb(p.user_ctx, p.client_id, &result);
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Pop pending submissions from the SPSC queue, call the appropriate
// BinanceOrderAPI function on this worker's own instance, build an
// OrderResult, and invoke the callback. Sleep briefly when idle to avoid
// burning the core.
//
// Each worker is launched with its own worker_index so it knows which
// workers_api[] slot to use. Lifetime: from BinanceAdapter_Init until
// shutdown_requested flips.
//======================================================================
// [END_FUNCTION]_[BinanceAdapter_WorkerLoop]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceAdapter_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[init each worker's own BinanceOrderAPI (rollback on any failure), then start the worker threads; count clamps to [1, MAX_BINANCE_WORKERS]]
//======================================================================
// [CODE]
//======================================================================
static inline int BinanceAdapter_Init(BinanceAdapterState* state,
                                       const char* host,
                                       const char* api_key,
                                       const char* api_secret,
                                       const char* symbol,
                                       ShardedOrderLatency* latency,
                                       int worker_count) {
    // Don't memset the struct — std::thread and std::atomic are not
    // trivially copyable, and the caller already default-constructed
    // them by declaring the BinanceAdapterState as a local or static.
    // Explicit-init the fields we own.
    SPSCRing_Init(&state->submission_queue);
    state->shutdown_requested.store(0, std::memory_order_relaxed);
    state->dropped_submissions.store(0, std::memory_order_relaxed);
    state->ws_active.store(0, std::memory_order_relaxed);
    state->latency = latency;

    if (worker_count < 1) worker_count = 1;
    if (worker_count > MAX_BINANCE_WORKERS) worker_count = MAX_BINANCE_WORKERS;
    state->worker_count = worker_count;

    // Initialize each worker's BinanceOrderAPI. Each one opens its own
    // socket, its own SSL_CTX, its own TLS session. No sharing.
    for (int i = 0; i < worker_count; ++i) {
        if (!BinanceOrderAPI_Init(&state->workers_api[i],
                                   host, api_key, api_secret, symbol)) {
            std::fprintf(stderr,
                         "[BinanceAdapter] worker %d BinanceOrderAPI_Init failed\n", i);
            // Roll back any earlier successful inits.
            for (int j = 0; j < i; ++j) {
                BinanceOrderAPI_Cleanup(&state->workers_api[j]);
            }
            return 0;
        }
    }

    // Start the worker threads after all instances are valid.
    for (int i = 0; i < worker_count; ++i) {
        state->workers[i] = std::thread(BinanceAdapter_WorkerLoop, state, i);
    }

    std::fprintf(stderr,
                 "[BinanceAdapter] %d worker(s) started, host=%s symbol=%s\n",
                 worker_count, host, symbol);
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Set up the adapter: zero state, initialize each worker's BinanceOrderAPI
// against the given host + credentials, start the worker threads. Returns
// 1 on success, 0 on failure (any worker's BinanceOrderAPI_Init failure
// is fatal — partial init is rolled back).
//
// host typically comes from bcfg.use_testnet selection in the caller —
// "testnet.binance.vision" or "api.binance.us". Credentials come from
// LoadSecrets in the caller. The adapter copies them into each worker's
// BinanceOrderAPI via BinanceOrderAPI_Init's strncpy.
//
// worker_count clamps to [1, MAX_BINANCE_WORKERS]. Phase 02 ships with
// 1; future commits scale up after the back-to-back stress test passes.
//======================================================================
// [END_FUNCTION]_[BinanceAdapter_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceAdapter_ShutdownState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[signal + join workers, cleanup each API instance; idempotent]
//======================================================================
// [CODE]
//======================================================================
static inline void BinanceAdapter_ShutdownState(BinanceAdapterState* state) {
    state->shutdown_requested.store(1, std::memory_order_release);
    for (int i = 0; i < state->worker_count; ++i) {
        if (state->workers[i].joinable()) {
            state->workers[i].join();
        }
    }
    for (int i = 0; i < state->worker_count; ++i) {
        BinanceOrderAPI_Cleanup(&state->workers_api[i]);
    }
    state->worker_count = 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Signal workers to exit, join each one, then cleanup each
// BinanceOrderAPI instance. Idempotent — safe to call twice or to call
// without a successful Init (shutdown_requested is already 0 by default).
//======================================================================
// [END_FUNCTION]_[BinanceAdapter_ShutdownState]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceAdapter_SubmitMarketBuy]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[async BUY submit — SPSC push from the drainer (sole producer); queue-full drops with log + counter, REST happens later on a worker]
//======================================================================
// [CODE]
//======================================================================
static inline int BinanceAdapter_SubmitMarketBuy(void* ctx, uint64_t client_id,
                                                  double qty,
                                                  OrderCallback cb, void* user) {
    BinanceAdapterState* state = (BinanceAdapterState*)ctx;
    PendingSubmission p;
    p.client_id = client_id;
    p.type      = ORDER_MARKET_BUY;
    p.qty       = qty;
    p.cb        = cb;
    p.user_ctx  = user;
    if (!SPSCRing_TryPush(&state->submission_queue, p)) {
        state->dropped_submissions.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[BinanceAdapter] submission queue full, dropping BUY client_id=%llu\n",
                     (unsigned long long)client_id);
        return 0;
    }
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Push a PendingSubmission onto the queue. Returns 1 on success, 0 on
// queue-full failure (drops the submission with a log + counter bump).
// The actual REST call happens later on a worker thread.
//
// The drainer is the only caller, so the SPSC producer-side contract
// holds with worker_count == 1.
//======================================================================
// [END_FUNCTION]_[BinanceAdapter_SubmitMarketBuy]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceAdapter_SubmitMarketSell]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[SELL twin of BinanceAdapter_SubmitMarketBuy — same SPSC contract + queue-full handling]
//======================================================================
// [CODE]
//======================================================================
static inline int BinanceAdapter_SubmitMarketSell(void* ctx, uint64_t client_id,
                                                   double qty,
                                                   OrderCallback cb, void* user) {
    BinanceAdapterState* state = (BinanceAdapterState*)ctx;
    PendingSubmission p;
    p.client_id = client_id;
    p.type      = ORDER_MARKET_SELL;
    p.qty       = qty;
    p.cb        = cb;
    p.user_ctx  = user;
    if (!SPSCRing_TryPush(&state->submission_queue, p)) {
        state->dropped_submissions.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[BinanceAdapter] submission queue full, dropping SELL client_id=%llu\n",
                     (unsigned long long)client_id);
        return 0;
    }
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[BinanceAdapter_SubmitMarketSell]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceAdapter_GetBalancesImpl]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[blocking balance query through worker 0's instance — reconciliation-only; caller serializes against in-flight workers]
//======================================================================
// [CODE]
//======================================================================
static inline int BinanceAdapter_GetBalancesImpl(void* ctx, double* base_out, double* quote_out) {
    BinanceAdapterState* state = (BinanceAdapterState*)ctx;
    if (state->worker_count < 1) return 0;
    // Use worker 0's instance. Caller is responsible for ensuring the
    // worker thread isn't actively making a call right now.
    return BinanceOrderAPI_GetBalances(&state->workers_api[0], quote_out, base_out);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// get_balances and query_order block on the network. They are NOT called
// from the drainer or any hot path. Phase 05 reconciliation uses them.
// Phase 02 just provides workable stubs that go through worker 0's
// BinanceOrderAPI instance — but only call these when no orders are
// in flight (workers idle), otherwise you race against the worker thread
// on its own instance.
//
// Phase 02 limitation: callers must serialize sync queries against the
// worker thread externally. Phase 05 reconciliation handles this by
// pausing submissions during a reconciliation pass.
//======================================================================
// [END_FUNCTION]_[BinanceAdapter_GetBalancesImpl]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceAdapter_QueryOrderImpl]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[blocking order-status query through worker 0's instance -> OrderResult; reconciliation-only, same serialization contract]
//======================================================================
// [CODE]
//======================================================================
static inline int BinanceAdapter_QueryOrderImpl(void* ctx, const char* exchange_id,
                                                 OrderResult* out) {
    BinanceAdapterState* state = (BinanceAdapterState*)ctx;
    if (state->worker_count < 1) return 0;
    double filled_qty = 0.0;
    double avg_price  = 0.0;
    int status = BinanceOrderAPI_GetStatus(&state->workers_api[0], exchange_id,
                                            &filled_qty, &avg_price);
    std::memset(out, 0, sizeof(*out));
    std::strncpy(out->exchange_id, exchange_id, sizeof(out->exchange_id) - 1);
    out->avg_fill_price = avg_price;
    out->fill_qty       = filled_qty;
    out->success = (status == ORDER_STATUS_FILLED || status == ORDER_STATUS_PARTIALLY_FILLED);
    out->error_code = status;
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[BinanceAdapter_QueryOrderImpl]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceAdapter_ShutdownImpl]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[void*-ctx trampoline onto BinanceAdapter_ShutdownState (the vtable shutdown slot)]
//======================================================================
// [CODE]
//======================================================================
static inline void BinanceAdapter_ShutdownImpl(void* ctx) {
    BinanceAdapter_ShutdownState((BinanceAdapterState*)ctx);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[BinanceAdapter_ShutdownImpl]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceAdapter_Get]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[build the ExchangeAdapter<F> function-pointer vtable wired to this state; F unused here (double at the API boundary) but matches the OMS field type]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
static inline ExchangeAdapter<F> BinanceAdapter_Get(BinanceAdapterState* state) {
    ExchangeAdapter<F> adapter;
    adapter.submit_market_buy  = BinanceAdapter_SubmitMarketBuy;
    adapter.submit_market_sell = BinanceAdapter_SubmitMarketSell;
    adapter.get_balances       = BinanceAdapter_GetBalancesImpl;
    adapter.query_order        = BinanceAdapter_QueryOrderImpl;
    adapter.shutdown           = BinanceAdapter_ShutdownImpl;
    adapter.ctx                = state;
    return adapter;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Build the function-pointer vtable that the OMS uses to call into this
// adapter. F is the FPN_Binary width — the concrete adapter doesn't use F
// (everything goes through `double` at the BinanceOrderAPI boundary) but
// the template parameter matches the OMS's ExchangeAdapter<F> field type.
//======================================================================
// [END_FUNCTION]_[BinanceAdapter_Get]
//======================================================================

}  // namespace tt
