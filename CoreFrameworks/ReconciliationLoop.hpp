// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [RECONCILIATION LOOP]
//
// Phase 05 of the OMS plan. Periodic self-healing safety net that verifies
// local OMS state against the exchange's actual balances. Runs on its own
// thread with its own BinanceOrderAPI instance (per-thread, not shared).
//
// Architecture:
//   - Separate thread wakes every interval_secs (default 30)
//   - Queries exchange balances via its own REST instance
//   - Compares against oms->balance (known race with drainer — tolerable,
//     reconciliation is advisory)
//   - Excludes in-flight orders from the comparison (SUBMITTED/ACKNOWLEDGED
//     orders have committed capital that hasn't been confirmed yet)
//   - On drift beyond tolerance: pushes CMD_RECONCILE into a dedicated
//     SPSC ring. The drainer's OrderManager_Tick drains it and applies
//     the correction.
//
// Threading:
//   Reconciler thread is the sole producer of reconcile_queue.
//   Drainer thread is the sole consumer. SPSC contract holds.
//
// The reconciler's REST instance connects to the same host with the same
// credentials as the adapter workers, but on its own socket/SSL session.
// No contention with the adapter worker thread.
//======================================================================================================

#pragma once

#include "../DataStream/BinanceOrderAPI.hpp"
#include "../Limits.hpp"
#include "OrderManager.hpp"
#include "SPSCRing.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace tt {

constexpr size_t RECONCILE_QUEUE_SIZE = 64;

//======================================================================================================
// [STATE]
//======================================================================================================
template <unsigned F>
struct ReconciliationLoopState {
    // Own BinanceOrderAPI instance — never shared with adapter workers.
    BinanceOrderAPI rest_api;

    // Pointer to the OMS. Read-only from the reconciler's perspective —
    // it reads oms->balance, oms->portfolio, oms->order_bitmap to compute
    // expected state. The drainer is the only writer. The race between
    // drainer writes and reconciler reads is tolerable: worst case is a
    // single-cycle false drift that the next pass corrects.
    OrderManagerState<F>* oms;

    // Output: SPSC ring for CMD_RECONCILE → drainer
    SPSCRing<Command, RECONCILE_QUEUE_SIZE> reconcile_queue;

    // Config
    int    interval_secs;       // default 30
    double balance_tolerance;   // default 0.01 (1 cent USDT)
    double qty_tolerance;       // default 1e-6 BTC

    // Thread
    std::thread thread;
    std::atomic<int> shutdown_requested;
    std::atomic<int> trigger_now;  // set externally (e.g. WS reconnect)

    // Observability (atomic for TUI reads)
    std::atomic<uint64_t> total_polls;
    std::atomic<uint64_t> drift_corrections;
    std::atomic<double>   last_drift_usdt;
};

//======================================================================================================
// [RECONCILE PASS]
//======================================================================================================
// One reconciliation cycle. Queries exchange balances, computes expected
// balances from OMS state, reports drift.
//
// Returns 1 if drift was detected and a CMD_RECONCILE was pushed, 0 if clean.
//======================================================================================================
template <unsigned F>
static inline int ReconciliationLoop_Pass(ReconciliationLoopState<F>* s) {
    s->total_polls.fetch_add(1, std::memory_order_relaxed);

    // Query exchange balances via our own REST instance.
    double exchange_usdt = 0.0, exchange_btc = 0.0;
    int ok = BinanceOrderAPI_GetBalances(&s->rest_api, &exchange_usdt, &exchange_btc);
    if (!ok) {
        fprintf(stderr, "[Reconciler] GetBalances failed, skipping pass\n");
        return 0;
    }

    // Expected USDT balance from OMS (known race — snapshot, not locked).
    double oms_balance = FPN_ToDouble(s->oms->balance);

    // Exclude in-flight orders: scan the order table for orders that have
    // been submitted but not yet filled. Their notional value is committed
    // capital that the exchange has reserved but we haven't confirmed.
    double inflight_buy_notional = 0.0;
    uint16_t bm = s->oms->order_bitmap;
    for (int i = 0; i < MAX_INFLIGHT_ORDERS; ++i) {
        if (((bm >> i) & 1) == 0) continue;
        const Order<F>& o = s->oms->orders[i];
        if (o.state == ORDER_SUBMITTED || o.state == ORDER_ACKNOWLEDGED) {
            if (o.type == (uint8_t)ORDER_MARKET_BUY) {
                // estimate notional from event_price * requested_qty
                double price = FPN_ToDouble(o.event_price);
                double qty   = FPN_ToDouble(o.requested_qty);
                inflight_buy_notional += price * qty;
            }
        }
    }

    // Expected USDT = oms_balance - inflight_buy_notional
    // (in-flight buys reduce our available USDT but the exchange has
    // already reserved the funds)
    double expected_usdt = oms_balance - inflight_buy_notional;
    double drift_usdt = exchange_usdt - expected_usdt;

    s->last_drift_usdt.store(drift_usdt, std::memory_order_relaxed);

    if (std::fabs(drift_usdt) <= s->balance_tolerance) {
        return 0;  // within tolerance, no correction needed
    }

    // Drift detected — push CMD_RECONCILE to the drainer.
    fprintf(stderr, "[Reconciler] DRIFT detected: exchange=$%.4f expected=$%.4f "
                     "drift=$%.4f (inflight_buy=$%.4f)\n",
                     exchange_usdt, expected_usdt, drift_usdt, inflight_buy_notional);

    Command cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_RECONCILE;
    cmd.order_id = 0;  // no specific order
    cmd.result.success = 1;
    cmd.result.avg_fill_price = drift_usdt;  // repurpose: drift amount
    cmd.result.fill_qty       = exchange_usdt;  // repurpose: actual balance
    cmd.result.error_code     = 0;
    snprintf(cmd.result.error_message, sizeof(cmd.result.error_message),
             "drift=%.4f exchange=%.4f expected=%.4f",
             drift_usdt, exchange_usdt, expected_usdt);

    // Phase 0.3 fix: push to the OMS's reconcile_queue (which OMS_Tick
    // drains). Previously we pushed to our own s->reconcile_queue, which
    // nothing reads — drift corrections were silently dropped on the floor.
    // s->reconcile_queue stays initialized for now (callers may reference
    // it); it's dead code that should be cleaned up in a follow-on commit.
    if (!SPSCRing_TryPush(&s->oms->reconcile_queue, cmd)) {
        fprintf(stderr, "[Reconciler] oms->reconcile_queue full, dropping correction\n");
        return 0;
    }

    s->drift_corrections.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

//======================================================================================================
// [THREAD BODY]
//======================================================================================================
template <unsigned F>
static inline void reconcile_thread_body(ReconciliationLoopState<F>* s) {
    while (s->shutdown_requested.load(std::memory_order_acquire) == 0) {
        // Sleep for interval_secs, checking shutdown and trigger_now every 100ms.
        int wait_cycles = s->interval_secs * 10;
        for (int i = 0; i < wait_cycles; ++i) {
            if (s->shutdown_requested.load(std::memory_order_acquire)) return;
            if (s->trigger_now.load(std::memory_order_relaxed)) {
                s->trigger_now.store(0, std::memory_order_relaxed);
                break;  // immediate reconcile
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (s->shutdown_requested.load(std::memory_order_acquire)) return;
        ReconciliationLoop_Pass(s);
    }
}

//======================================================================================================
// [INIT]
//======================================================================================================
template <unsigned F>
static inline int ReconciliationLoop_Init(ReconciliationLoopState<F>* s,
                                           const char* host,
                                           const char* api_key,
                                           const char* api_secret,
                                           const char* symbol,
                                           OrderManagerState<F>* oms,
                                           int interval_secs = 30,
                                           double balance_tolerance = 0.01) {
    s->oms = oms;
    s->interval_secs     = interval_secs;
    s->balance_tolerance = balance_tolerance;
    s->qty_tolerance     = 1e-6;
    s->shutdown_requested.store(0, std::memory_order_relaxed);
    s->trigger_now.store(0, std::memory_order_relaxed);
    s->total_polls.store(0, std::memory_order_relaxed);
    s->drift_corrections.store(0, std::memory_order_relaxed);
    s->last_drift_usdt.store(0.0, std::memory_order_relaxed);
    SPSCRing_Init(&s->reconcile_queue);

    if (!BinanceOrderAPI_Init(&s->rest_api, host, api_key, api_secret, symbol)) {
        fprintf(stderr, "[Reconciler] failed to init REST API instance\n");
        return 0;
    }
    return 1;
}

//======================================================================================================
// [START]
//======================================================================================================
template <unsigned F>
static inline void ReconciliationLoop_Start(ReconciliationLoopState<F>* s) {
    s->thread = std::thread(reconcile_thread_body<F>, s);
    fprintf(stderr, "[Reconciler] thread started (interval=%ds tolerance=$%.4f)\n",
            s->interval_secs, s->balance_tolerance);
}

//======================================================================================================
// [TRIGGER NOW — external callers can force an immediate pass]
//======================================================================================================
template <unsigned F>
static inline void ReconciliationLoop_TriggerNow(ReconciliationLoopState<F>* s) {
    s->trigger_now.store(1, std::memory_order_relaxed);
}

//======================================================================================================
// [SHUTDOWN]
//======================================================================================================
template <unsigned F>
static inline void ReconciliationLoop_Shutdown(ReconciliationLoopState<F>* s) {
    s->shutdown_requested.store(1, std::memory_order_release);
    if (s->thread.joinable()) s->thread.join();
    BinanceOrderAPI_Cleanup(&s->rest_api);
    fprintf(stderr, "[Reconciler] shutdown (polls=%llu corrections=%llu)\n",
            (unsigned long long)s->total_polls.load(),
            (unsigned long long)s->drift_corrections.load());
}

}  // namespace tt
