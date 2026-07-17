// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/ReconciliationLoop.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CONCURRENCY] [SUPPORTIVE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the advisory venue-reconcile safety net — own thread + own REST instance; detect-only alerts via CMD_RECONCILE]
// [CONTAINS]
//   - [STRUCT]_[ReconciliationLoopState]
//   - [FUNCTION]_[ReconciliationLoop_Pass]
//   - [FUNCTION]_[reconcile_thread_body]
//   - [FUNCTION]_[ReconciliationLoop_Init]
//   - [FUNCTION]_[ReconciliationLoop_Shutdown]
//======================================================================================================

#pragma once

#include "../DataStream/BinanceOrderAPI.hpp"
#include "OrderManager.hpp"
#include "SPSCRing.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace tt {

constexpr size_t RECONCILE_QUEUE_SIZE = 64;

//======================================================================
// [STRUCT]_[ReconciliationLoopState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CONCURRENCY]]
// [THREAD]_[[RECONCILER_WRITER] [GUI_READER]]
// [SYNC]_[ATOMIC]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[reconciler thread state — own REST instance + read-only OMS view + observability atomics the TUI reads]
//======================================================================
// [CODE]
//======================================================================
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

    // DEAD (Phase 0.3 fix — see ReconciliationLoop_Pass): the Pass pushes to
    // oms->reconcile_queue, NOT this ring; nothing reads this one. Kept
    // initialized (callers may reference it); cleanup is the follow-on the
    // Pass comment tracks (TECH_DEBT-192 dead-code cluster).
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]_[architecture + threading]
//----------------------------------------------------------------------
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
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//----------------------------------------------------------------------
// [SIZE]_[17088B]
// [ALIGN]_[64]
// [CACHE_LINES]_[267]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[ReconciliationLoopState]
//======================================================================

//======================================================================
// [FUNCTION]_[ReconciliationLoop_Pass]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [LIVE_TRADING]]
// [REFERENCE]_[DECISION]_[D-216]
// [REFERENCE]_[INVARIANT]_[H4]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one reconcile cycle — venue free-USDT vs OMS_ExpectedFreeCash (Money, H4); drift pushes a detect-only alert]
//======================================================================
// [CODE]
//======================================================================
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

    // A21 (D-216): expected FREE-USDT in Money (H4). The old formula compared flat-account
    // balance (oms->balance = start + sum realized; a BUY never debits it) against venue
    // free-USDT, so it diverged by the whole open-position notional the instant a position
    // opened -> structural false drift (-> a false kill-switch trip downstream).
    // ReconciliationLoop_ExpectedFreeCash nets out the committed open-position cost +
    // inflight-reserved cash. The reconciler is ADVISORY (detect-only): drift is PUSHED for
    // an alert; ProcessReconcile NEVER writes oms->balance (D-216). The authoritative
    // venue-net correction + the BTC/qty leg defer to .E.1.
    Money exchange_money = Money{ money_from_double_payload(exchange_usdt) };  // venue ingress (<=8dp; string-direct D-123 -> .E.1/.E.3)
    Money expected       = OMS_ExpectedFreeCash(s->oms);
    Money drift          = Money_Sub(exchange_money, expected);
    double drift_usdt    = Money_ToDouble(drift);       // repurposed-field + log (display only)
    double expected_usdt = Money_ToDouble(expected);

    s->last_drift_usdt.store(drift_usdt, std::memory_order_relaxed);

    // Tolerance: convert-at-compare (cfg field stays double; boundary-stable).
    // TODO(.E.1): re-derive the band for the multi-leg Money sum (1c too tight once >=1
    // Money_Mul rounds at 8dp per open leg) + make the cfg field Money at parse.
    Money tol = Money{ money_from_double_payload(s->balance_tolerance) };
    if (Money_Le(Money_Abs(drift), tol)) {
        return 0;  // within tolerance
    }

    // Drift detected -- push CMD_RECONCILE so the drainer ALERTS (detect-only; no write).
    fprintf(stderr, "[Reconciler] DRIFT detected: exchange=$%.4f expected=$%.4f drift=$%.4f\n",
                     exchange_usdt, expected_usdt, drift_usdt);

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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// One reconciliation cycle. Queries exchange balances, computes expected
// balances from OMS state, reports drift.
//
// Returns 1 if drift was detected and a CMD_RECONCILE was pushed, 0 if clean.
//======================================================================
// [END_FUNCTION]_[ReconciliationLoop_Pass]
//======================================================================

//======================================================================
// [FUNCTION]_[reconcile_thread_body]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CONCURRENCY] [SUPPORTIVE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the reconciler thread loop — interval sleep in 100ms shutdown/trigger-aware slices, then Pass]
//======================================================================
// [CODE]
//======================================================================
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[reconcile_thread_body]
//======================================================================

//======================================================================
// [FUNCTION]_[ReconciliationLoop_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[wire the reconciler — cfg + counters + its own REST instance; thread NOT started until Start]
//======================================================================
// [CODE]
//======================================================================
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ReconciliationLoop_Init]
//======================================================================

//----------------------------------------------------------------------
// [SECTION]_[start / trigger-now — thread launch + external immediate-pass hook]
//----------------------------------------------------------------------
template <unsigned F>
static inline void ReconciliationLoop_Start(ReconciliationLoopState<F>* s) {
    s->thread = std::thread(reconcile_thread_body<F>, s);
    fprintf(stderr, "[Reconciler] thread started (interval=%ds tolerance=$%.4f)\n",
            s->interval_secs, s->balance_tolerance);
}

// External callers (e.g. WS reconnect) can force an immediate pass.
template <unsigned F>
static inline void ReconciliationLoop_TriggerNow(ReconciliationLoopState<F>* s) {
    s->trigger_now.store(1, std::memory_order_relaxed);
}

//======================================================================
// [FUNCTION]_[ReconciliationLoop_Shutdown]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[signal + join the reconciler thread + tear down its REST instance; logs the poll/correction totals]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
static inline void ReconciliationLoop_Shutdown(ReconciliationLoopState<F>* s) {
    s->shutdown_requested.store(1, std::memory_order_release);
    if (s->thread.joinable()) s->thread.join();
    BinanceOrderAPI_Cleanup(&s->rest_api);
    fprintf(stderr, "[Reconciler] shutdown (polls=%llu corrections=%llu)\n",
            (unsigned long long)s->total_polls.load(),
            (unsigned long long)s->drift_corrections.load());
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ReconciliationLoop_Shutdown]
//======================================================================

}  // namespace tt
