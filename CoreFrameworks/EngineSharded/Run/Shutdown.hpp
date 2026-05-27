// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [EngineSharded/Run/Shutdown.hpp — engine shutdown helpers (pre-join state save + post-join cleanup)]
//======================================================================================================
// Sub-sub-file of CoreFrameworks/EngineSharded/Run.hpp (split per file-size-split-discipline.md
// at v5.15.5.F.4d.1.B.6 Phase B Step B.4.1; second-tier subfolder pattern to bring Run.hpp
// under the 1,500-line source-header threshold).
//
// Contains:
//   - EngineSharded_Shutdown_PreJoin<F> — final state save BEFORE thread joins start.
//     Snapshot save (paper mode only) + per-core bandit state save + position-still-open
//     advisory warning. Runs while producer/executors/drainer/slow-paths still alive.
//   - EngineSharded_Shutdown_PostJoin<F, BENCH> — post-join resource cleanup + final dumps.
//     Final-counter dump + DumpLatency call + ORDER LATENCY dump + OMS COUNTERS dump +
//     reconciler/userdata/adapter shutdown + Strategy_FreePerCore loop + InitArena
//     destroy + drainer-cycle bench histogram dump (BENCH only) + signal handler restore.
//
// **Hot-path discipline:** N/A — both functions run only AFTER hot path has fully ceased
// (PostJoin) or just before thread joining begins (PreJoin runs while threads alive but
// engine_sharded_shutdown is set, so threads are exiting). No latency impact from header
// location.
//
// **Block-scope-statics-as-args (B.2 lesson):** Many block-scope statics referenced
// (g_reconciler / g_sharded_binance_adapter / g_user_data / g_init_arena / g_notify /
// g_sharded_order_lat / g_engine_drainer_cycle_hist). All passed by reference — cannot
// reference function-local statics from a hoisted header function. Sister to Async.hpp +
// AnsiTui.hpp + SlowPathThreads.hpp captures-as-args translation.
//
// **`g_notify` global pointer:** Lives in NotifyState header (`Notify.hpp`) — it's a
// file-shared inline global (Notify backend wiring). Caller passes by reference so the
// helper can nullptr it after Shutdown.
//
// **BENCH template param:** PostJoin uses `if constexpr (BENCH)` to emit drainer-cycle
// bench histogram summary at engine shutdown. Same dispatch as the caller-side BENCH
// gate, preserved verbatim.
//======================================================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <csignal>

#include "../../../FixedPoint/FixedPointN.hpp"

#include "../../ControllerConfig.hpp"           // ControllerConfig<F>
#include "../../ControllerEventLoop.hpp"        // EventLoopState
#include "../../ExecutionCore.hpp"              // ExecutionCore<F>
#include "../../OrderManager.hpp"               // OrderManagerState + _Shutdown / _Total*
#include "../../ReconciliationLoop.hpp"         // ReconciliationLoopState<F> + _Shutdown
#include "../../ShardedSnapshot.hpp"            // ShardedSnapshot_Save
#include "../../ShardedSnapshotPersist.hpp"     // (linkage)
#include "../../ShardedOrderLatency.hpp"        // ShardedOrderLatency
#include "../../EnsembleHotSwap.hpp"            // EnsembleModelZoo<F>
#include "../../BinanceAdapter.hpp"             // BinanceAdapterState + _ShutdownState
#include "../../../Strategies/StrategyLifecycle.hpp"  // Strategy_FreePerCore
#include "../../../DataStream/BinanceUserData.hpp"  // BinanceUserDataState + _Shutdown
#include "../../../MemHeaders/BitmapMacros.hpp"     // BITMAP_IS_SET
#include "../../../MemHeaders/LatencyHistogram.hpp" // LatencyHistogram + _Percentile
#include "../../../MemHeaders/HealthLog.hpp"        // (linkage)
#include "../../../ML_Headers/RollingStats.hpp"     // (linkage)

#include "../Boot.hpp"     // g_engine_sharded_shutdown
#include "../Async.hpp"    // g_engine_drainer_cycle_hist
#include "Latency.hpp"     // EngineSharded_DumpLatency<F>

// InitArena lives in a different layout; pull header transitively (already included via
// EngineCommon / EventLoopState — but list explicitly for clarity).
// (InitArena_Used / InitArena_Destroy / InitArena_Global declarations live in same TU
//  that defines them; reachable via Strategies/StrategyLifecycle.hpp → ControllerEventLoop.hpp
//  transitive includes already pulled above.)

// parent_index: CoreFrameworks/EngineSharded/Run.hpp

namespace tt {

//======================================================================================================
// EngineSharded_Shutdown_PreJoin — final state save BEFORE thread joins start
//======================================================================================================
// Runs after g_engine_sharded_shutdown raised + before producer/drainer/executors/slow-paths
// joined. Captures pre-close state so a restart can resume regime hysteresis + pnl_feeder +
// kill switch peak. Force-close mutates portfolio + realized_pnl; this snapshot is taken
// first so persisted state matches engine's "intent" rather than in-progress liquidation.
//======================================================================================================
template <unsigned F>
inline void EngineSharded_Shutdown_PreJoin(
    bool live_trading,
    int num_cores,
    ControllerConfig<F>& cfg,
    EventLoopState<F>& state,
    OrderManagerState<F>& oms
) {
    // Phase 4 — final snapshot save BEFORE force-close. Captures the
    // pre-close state so a restart can resume regime hysteresis +
    // pnl_feeder + kill switch peak. Force-close mutates portfolio +
    // realized_pnl; we save first so the persisted state matches the
    // engine's "intent" rather than an in-progress liquidation.
    // Paper mode only — live mode treats exchange state as truth.
    if (!live_trading) {
        // v5.15.5.C.2 (S3a + S4): canonical mirror via bit-packed oms_state_flags.
        if (ShardedSnapshot_Save<F>(&state, "data/sharded_snapshot.dat",
                                      BITMAP_IS_SET(oms.oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED))) {
            fprintf(stderr, "[snapshot] final save: data/sharded_snapshot.dat\n");
        } else {
            fprintf(stderr, "[snapshot] final save FAILED — next restart starts fresh\n");
        }
    }

    // v5.10.0a.G.9 — final bandit state save. Each active ensemble core
    // flushes to its own <core_model_dir>/bandit_state.json. Survives
    // restart so weights resume rather than re-learn from uniform.
    // Live AND paper modes both save (live: deployed weights inform
    // next session; paper: same thing for backtest-style sessions).
    // Reaches the ezoo via ctx.ensemble_handle (registered at
    // line ~853) since the static array's name is scope-limited
    // to the init for-loop.
    for (int i = 0; i < num_cores; ++i) {
        auto* ezoo = static_cast<EnsembleModelZoo<F>*>(
            state.cores[i].ensemble_handle);
        if (ezoo && BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE) && BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY) &&
            cfg.core_model_dir[i][0]) {
            int saved = EnsembleModelZoo_SaveBanditState(
                ezoo, cfg.core_model_dir[i],
                /*regime_names=*/nullptr);
            if (saved) {
                fprintf(stderr, "[sharded] core %d: saved bandit state to "
                                "%s/bandit_state.json\n",
                        i, cfg.core_model_dir[i]);
            }
            // v5.13.4.C — sell-side bandit shutdown save. Skips silently
            // when initialized_exit_bandits=0 (no exit models loaded).
            int saved_exit = EnsembleModelZoo_SaveExitBanditState(
                ezoo, cfg.core_model_dir[i],
                /*regime_names=*/nullptr);
            if (saved_exit) {
                fprintf(stderr, "[sharded] core %d: saved exit_bandit "
                                "state to %s/exit_bandit_state.json\n",
                        i, cfg.core_model_dir[i]);
            }
        }
    }

    // v5.4.5 (recurring-bugs Class 9 — shutdown UX): positions PERSIST
    // across restart. Engine is designed to run 24/7; on operator-initiated
    // shutdown (Ctrl+C, GUI close), the snapshot saved above captures open
    // positions and the next session resumes management.
    //
    // Pre-fix: this site called EngineSharded_ForceCloseOnShutdown with a
    // 30s timeout, which submitted market SELLs for every open position
    // and blocked the join sequence waiting for fills. Symptom on shutdown
    // with positions open: terminal hung for up to 30s after Ctrl+C with
    // no output, because force-close runs BEFORE the
    // "[sharded] joining threads..." prints. User reported this as
    // "engine doesn't exit cleanly when positions are open."
    //
    // Force-close logic preserved in the codebase (EngineSharded_ForceCloseOnShutdown)
    // for callers that explicitly want it (e.g. risk-managed live shutdown
    // sequences). The default 24/7 paper/live operation just persists.
    {
        int still_open = __builtin_popcount(oms.portfolio.active_bitmap);
        if (still_open > 0) {
            fprintf(stderr,
                "[sharded] %d position(s) open on shutdown — persisting via "
                "snapshot for next session (force-close disabled by default; "
                "see EngineSharded_ForceCloseOnShutdown if you need exchange "
                "flatten on exit).\n",
                still_open);
        }
    }
}

//======================================================================================================
// EngineSharded_Shutdown_PostJoin — post-thread-join cleanup + final dumps
//======================================================================================================
// Runs after all threads (producer + executors + drainer + slow-paths + GUI) have been
// joined. Order matters:
//   1. Counter dump (uses state + oms read-only)
//   2. EngineSharded_DumpLatency<F>(cores, num_cores, tsc_ghz)
//   3. ORDER LATENCY dump (live only)
//   4. OMS COUNTERS dump (always)
//   5. ReconciliationLoop_Shutdown (REST-using; must precede adapter teardown)
//   6. BinanceUserData_Shutdown (WS-using; must precede adapter teardown)
//   7. BinanceAdapter_ShutdownState (joins worker threads, cleans BinanceOrderAPI)
//   8. Strategy_FreePerCore loop (per-strategy state release; matches Init symmetry)
//   9. InitArena_Destroy (munmap; ORDER: AFTER Strategy_FreePerCore per v5.11.6.D —
//      pre-fix had Destroy BEFORE strategy frees → InitArena_Owns returned 0 → delete
//      ran on unmapped memory)
//   10. drainer-cycle bench histogram dump (BENCH only)
//   11. Signal handler restore (SIGINT + SIGTERM)
//
// BENCH template param matches the caller-side dispatch for the optional histogram
// summary at the end (`if constexpr (BENCH)`).
//======================================================================================================
template <unsigned F, bool BENCH>
inline void EngineSharded_Shutdown_PostJoin(
    bool live_trading,
    int num_cores,
    double tsc_ghz,
    std::atomic<uint64_t>& ticks_produced,
    std::atomic<uint64_t>& ticks_consumed_total,
    EventLoopState<F>& state,
    OrderManagerState<F>& oms,
    const ExecutionCore<F>* cores,
    ReconciliationLoopState<F>& g_reconciler,
    BinanceAdapterState& g_sharded_binance_adapter,
    BinanceUserDataState& g_user_data,
    InitArena& g_init_arena,
    ShardedOrderLatency& g_sharded_order_lat,
    void (*prev_int)(int),
    void (*prev_term)(int)
) {
    fprintf(stderr, "[sharded] all threads joined.\n");
    fprintf(stderr, "[sharded] final: produced=%lu consumed=%lu entries=%lu exits=%lu balance=%.4f\n",
            (unsigned long)ticks_produced.load(),
            (unsigned long)ticks_consumed_total.load(),
            (unsigned long)state.total_entries,
            (unsigned long)state.total_exits,
            FPN_ToDouble(state.oms->balance));

    EngineSharded_DumpLatency<F>(cores, num_cores, tsc_ghz);

    // Order latency final dump (only meaningful in live mode)
    if (live_trading) {
        uint64_t ord_count    = g_sharded_order_lat.count.load(std::memory_order_relaxed);
        uint64_t ord_failures = g_sharded_order_lat.failures.load(std::memory_order_relaxed);
        uint64_t ord_total_us = g_sharded_order_lat.total_us.load(std::memory_order_relaxed);
        uint64_t ord_min_us   = g_sharded_order_lat.min_us.load(std::memory_order_relaxed);
        uint64_t ord_max_us   = g_sharded_order_lat.max_us.load(std::memory_order_relaxed);
        fprintf(stderr, "\n");
        fprintf(stderr, "================================================================\n");
        fprintf(stderr, "[sharded] ORDER LATENCY (BinanceOrderAPI round trip, microseconds)\n");
        fprintf(stderr, "================================================================\n");
        if (ord_count > 0) {
            double ord_avg_us = (double)ord_total_us / (double)ord_count;
            fprintf(stderr, "  orders submitted : %lu\n", (unsigned long)ord_count);
            fprintf(stderr, "  failures         : %lu\n", (unsigned long)ord_failures);
            fprintf(stderr, "  min              : %lu µs\n", (unsigned long)ord_min_us);
            fprintf(stderr, "  avg              : %.0f µs\n", ord_avg_us);
            fprintf(stderr, "  max              : %lu µs\n", (unsigned long)ord_max_us);
        } else {
            fprintf(stderr, "  no orders submitted during this run.\n");
        }
        fprintf(stderr, "================================================================\n");
    }

    // OMS counter final dump. always shown (paper mode + live mode both
    // bump these). zero in paper mode if no entries fired during the run.
    {
        uint64_t oms_sub      = OrderManager_TotalSubmitted(&oms);
        uint64_t oms_fill     = OrderManager_TotalFilled(&oms);
        uint64_t oms_rej      = OrderManager_TotalRejected(&oms);
        int      oms_inflight = OrderManager_InflightCount(&oms);
        fprintf(stderr, "\n");
        fprintf(stderr, "================================================================\n");
        fprintf(stderr, "[sharded] OMS COUNTERS (OrderManager state at shutdown)\n");
        fprintf(stderr, "================================================================\n");
        fprintf(stderr, "  total submitted  : %lu\n", (unsigned long)oms_sub);
        fprintf(stderr, "  total filled     : %lu\n", (unsigned long)oms_fill);
        fprintf(stderr, "  total rejected   : %lu\n", (unsigned long)oms_rej);
        fprintf(stderr, "  in-flight at end : %d\n", oms_inflight);
        fprintf(stderr, "================================================================\n");
    }

    // Shut down the reconciler first (it queries balances via REST — needs
    // to stop before the REST instances are cleaned up).
    if (live_trading) {
        ReconciliationLoop_Shutdown(&g_reconciler);
    }

    // Shut down the user data websocket BEFORE the adapter (the WS thread
    // may still be issuing REST calls for listen key refresh).
    if (live_trading) {
        g_sharded_binance_adapter.ws_active.store(0, std::memory_order_relaxed);
        BinanceUserData_Shutdown(&g_user_data);
    }

    // Tear down the BinanceAdapter (joins worker threads, cleans up each
    // worker's BinanceOrderAPI instance). No-op if live_trading was 0
    // since BinanceAdapter_Init was never called — worker_count stays 0.
    if (live_trading) {
        BinanceAdapter_ShutdownState(&g_sharded_binance_adapter);
    }

    // v5.4.0 Phase 1.3 — release per-strategy state. Symmetric with the
    // Strategy_InitPerCore call above. Pre-v5.4 this was missing because
    // strategy state was never allocated; post-v5.4 it must be released
    // to avoid LeakSanitizer noise (and good hygiene anyway).
    for (int c = 0; c < state.registered_count; ++c) {
        tt::Strategy_FreePerCore(&state, c);
    }

    // v5.11.6.D — destroy the init-time arena AFTER all per-struct frees
    // (above + EventLoopState_Free path). The arena's munmap reclaims
    // the entire mmap'd region in one syscall. Reset the global FIRST
    // so any caller that observes the global post-shutdown sees nullptr
    // (clean fallback). Closes parity-check 2026-05-07 v5.11.6
    // sprint-exit Finding 7b — the prior ordering ran arena Destroy
    // BEFORE Strategy_FreePerCore, which would have caused
    // InitArena_Owns to return 0 and `delete` to run on already-unmapped
    // memory. Restart path was the trigger; main shutdown path was
    // OK because process-exit reclaims everything.
    fprintf(stderr, "[sharded]   destroying init arena (%zu/%zu bytes used)...\n",
            tt::InitArena_Used(&g_init_arena),
            g_init_arena.capacity);
    tt::InitArena_Global() = nullptr;
    tt::InitArena_Destroy(&g_init_arena);

    // v5.15.5.C.3 Phase 7.B — emit drainer-cycle bench histogram summary at
    // engine shutdown. Compile-time elided when BENCH=false (production);
    // no output. When BENCH=true, single stderr line with p50/p99/max +
    // total samples — operator sees the slow-path drainer latency profile
    // for this run.
    if constexpr (BENCH) {
        const uint64_t p50 = LatencyHistogram_Percentile(&g_engine_drainer_cycle_hist, 0.50);
        const uint64_t p99 = LatencyHistogram_Percentile(&g_engine_drainer_cycle_hist, 0.99);
        const uint64_t max = g_engine_drainer_cycle_hist.max_observed;
        const uint64_t total = g_engine_drainer_cycle_hist.total_count.load(std::memory_order_relaxed);
        std::fprintf(stderr,
            "[OMS_BENCH] drainer cycle: p50=%llu cy, p99=%llu cy, max=%llu cy "
            "(samples=%llu)\n",
            (unsigned long long)p50, (unsigned long long)p99, (unsigned long long)max,
            (unsigned long long)total);
    }

    // Restore previous signal handlers so subsequent code paths see the
    // original behavior (legacy engine doesn't install one, so this resets
    // to SIG_DFL).
    std::signal(SIGINT,  prev_int);
    std::signal(SIGTERM, prev_term);
}

} // namespace tt
