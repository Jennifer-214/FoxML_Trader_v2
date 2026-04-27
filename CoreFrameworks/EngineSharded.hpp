// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ENGINE — SHARDED MODE]
//
// Phase 13 of the per-core sharding migration. The "experimental sharded
// engine" entry point that main.cpp dispatches to when engine_mode = sharded.
//
// What this is RIGHT NOW (be honest about it):
//   - A latency testbed that runs the per-core hot path under real
//     concurrency on the user's hardware
//   - Synthetic tick generator (sawtooth around $60k) feeds N execution
//     core threads via SPSC tick rings
//   - Per-core CoreLatencyStats are enabled, so every tick gets sampled
//   - On Ctrl+C the threads join cleanly and per-core latency is dumped
//
// What this is NOT yet:
//   - A real trading path (no Binance websocket integration)
//   - A complete strategy port (only SimpleDip is wired, and the parameter
//     pack in this runner uses fixed thresholds, not Strategy_BuildParameters)
//   - Production-grade in any way — strategy parameter rebuilds and
//     production gating logic are still pending follow-up
//
// Purpose of this file:
//   Lets Jennifer flip `engine_mode = sharded` in engine.cfg, run
//   `./build/engine`, and immediately get real per-core latency numbers
//   under multi-threaded execution on her actual hardware. Side-by-side
//   testable against the legacy live engine (which is reached by leaving
//   engine_mode at single_core, the default).
//
// Shutdown:
//   Catches SIGINT and SIGTERM via the existing main.cpp signal handler
//   pattern. The shutdown flag is checked by every thread loop. All threads
//   exit within ~10ms of the flag being raised.
//======================================================================================================

#pragma once

#include "../DataStream/BinanceCrypto.hpp"
#include "../DataStream/BinanceOrderAPI.hpp"
#include "../DataStream/TickRecorder.hpp"  // Phase 8a (post-coding c7)
#include "Notify.hpp"                     // Phase 8b (post-coding c8)
#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../Strategies/StrategyParameters.hpp"
#include "../DataStream/BinanceUserData.hpp"
#include "BinanceAdapter.hpp"
#include "ReconciliationLoop.hpp"
#include "ShardedLiveSafety.hpp"   // Phase 0: orphan recovery, force-close, reconcile
#include "ShardedSnapshot.hpp"
#include "ShardedSnapshotPersist.hpp"  // Phase 4: persistent state across restarts
#include "ControllerEventLoop.hpp"
#include "CoreLatencyStats.hpp"
#include "ControllerConfig.hpp"
#include "ExchangeAdapter.hpp"
#include "ExecutionCore.hpp"
#include "GateParameters.hpp"
#include "OrderManager.hpp"
#include "ShardedOrderLatency.hpp"
#include "SPSCRing.hpp"
#include "Tick.hpp"

#ifdef USE_IMGUI_GUI
#include "../GUI/CandleAccumulator.hpp"
#include "../GUI/GuiThread.hpp"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace tt {

// File-scope shutdown flag the SIGINT handler flips. The handler is installed
// only while EngineSharded_Run is active, so this flag is only set when the
// sharded engine is the one that wants to know about it.
static volatile std::sig_atomic_t g_engine_sharded_shutdown = 0;

//======================================================================================================
// [ORDER LATENCY STATS — file-static instance]
//======================================================================================================
// the type and helper functions live in CoreFrameworks/ShardedOrderLatency.hpp
// (extracted during the OMS phase 01 refactor so OrderManager.hpp can call
// Sample without circular includes). this file just owns the singleton instance
// the TUI render loop reads from. OrderManager_Init takes a pointer to it so
// the OMS can sample each REST round trip into the same counters the TUI
// already displays.
//======================================================================================================
static ShardedOrderLatency g_sharded_order_lat;

// Pointer to the GUI's quit_requested flag, set by EngineSharded_Run after
// g_shared is constructed. The signal handler writes through it so SDL's GUI
// thread (which loops on quit_requested) exits in lockstep with the engine
// threads (which loop on g_engine_sharded_shutdown). Without this, Ctrl+C
// flips g_engine_sharded_shutdown but the GUI thread keeps running until the
// main thread reaches its post-join cleanup — which can hang if SDL's event
// dispatch holds resources the joiner is waiting on. Two flags, one signal.
static volatile sig_atomic_t* g_engine_sharded_gui_quit_ptr = nullptr;

extern "C" inline void EngineSharded_SignalHandler(int sig) {
    (void)sig;
    g_engine_sharded_shutdown = 1;
    if (g_engine_sharded_gui_quit_ptr) {
        *g_engine_sharded_gui_quit_ptr = 1;
    }
}

//======================================================================================================
// [TSC CALIBRATION]
//======================================================================================================
// Quick TSC frequency calibration so the latency dump can show ns alongside
// raw cycles. ~50ms of busy work, plenty accurate for diagnostic display.
//======================================================================================================
static inline double EngineSharded_CalibrateTscGhz() {
    auto wall0 = std::chrono::high_resolution_clock::now();
    uint32_t hi0, lo0;
    asm volatile("mfence\n\tlfence\n\trdtsc\n\t" : "=a"(lo0), "=d"(hi0));
    uint64_t t0 = ((uint64_t)hi0 << 32) | lo0;

    volatile uint64_t x = 0;
    for (uint64_t i = 0; i < 50'000'000; ++i) x ^= i;
    (void)x;

    uint32_t hi1, lo1;
    asm volatile("rdtscp\n\tlfence\n\t" : "=a"(lo1), "=d"(hi1) : : "rcx");
    uint64_t t1 = ((uint64_t)hi1 << 32) | lo1;
    auto wall1 = std::chrono::high_resolution_clock::now();

    double wall_ns = std::chrono::duration<double, std::nano>(wall1 - wall0).count();
    double cycles = (double)(t1 - t0);
    return cycles / wall_ns;  // GHz
}

//======================================================================================================
// [PIN THREAD TO CORE]
//======================================================================================================
// Best-effort core pinning. Returns 1 on success, 0 on failure (logged but
// not fatal — if pinning fails the engine still runs, just with potentially
// worse tail latency due to scheduler migration).
//======================================================================================================
static inline int EngineSharded_PinThread(int cpu_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (rc != 0) {
        fprintf(stderr, "[sharded] WARN: pthread_setaffinity_np(%d) failed: %d\n", cpu_id, rc);
        return 0;
    }
    return 1;
#else
    (void)cpu_id;
    return 0;
#endif
}

//======================================================================================================
// [LATENCY DUMP]
//======================================================================================================
// Dumps per-core latency stats in a compact table after the run finishes.
// One row per core, all converted to ns via the calibrated TSC frequency.
//======================================================================================================
template <unsigned F>
static inline void EngineSharded_DumpLatency(const ExecutionCore<F>* cores,
                                              int num_cores, double tsc_ghz) {
    fprintf(stderr, "\n");
    fprintf(stderr, "================================================================\n");
    fprintf(stderr, "[sharded] PER-CORE LATENCY (samples are p-stats from 256 most recent ticks)\n");
    fprintf(stderr, "================================================================\n");
    fprintf(stderr, "core   samples       min        p50        p95        p99        max        avg\n");
    fprintf(stderr, "----   --------   --------   --------   --------   --------   --------   --------\n");
    for (int i = 0; i < num_cores; ++i) {
        CoreLatencySnapshot s = CoreLatencyStats_Snapshot(&cores[i].latency_stats, tsc_ghz);
        if (s.total_count == 0) {
            fprintf(stderr, " %2d        0     -          -          -          -          -          -\n", i);
            continue;
        }
        fprintf(stderr, " %2d   %8lu   %6.0f ns   %6.0f ns   %6.0f ns   %6.0f ns   %6.0f ns   %6.0f ns\n",
                i, (unsigned long)s.total_count,
                s.min_ns, s.p50_ns, s.p95_ns, s.p99_ns, s.max_ns, s.avg_ns);
    }
    fprintf(stderr, "================================================================\n");
    fprintf(stderr, "Note: rdtsc bracketing has a ~25-30ns floor on this CPU.\n");
    fprintf(stderr, "Subtract the floor from min/p50 for actual hot-path work cost.\n");
    fprintf(stderr, "Max outliers are usually kernel preemption (try chrt -f 90 +\n");
    fprintf(stderr, "isolcpus on a real production box for cleaner tails).\n");
    fprintf(stderr, "================================================================\n");
}

//======================================================================================================
// [RUN]
//======================================================================================================
// The sharded engine main entry point. Called from main.cpp when
// engine_mode == ENGINE_MODE_SHARDED.
//
// Behavior:
//   1. Calibrate TSC for ns conversion
//   2. Build EventLoopState + N execution cores from config
//   3. Spawn 1 producer thread (synthetic ticks, sawtooth around $60k)
//   4. Spawn N executor threads (one per core, each pinned if possible)
//   5. Spawn 1 drainer thread on the controller core
//   6. Enable per-core CoreLatencyStats
//   7. Run until shutdown_flag is raised
//   8. Join all threads, dump per-core latency
//
// shutdown_flag is the same volatile int main.cpp uses for SIGINT/SIGTERM.
// Pass &g_shutdown_requested or whichever variable you have.
//======================================================================================================
template <unsigned F>
static inline void EngineSharded_Run(ControllerConfig<F>& cfg,
                                      const BinanceConfig& bcfg) {
    // Install our own SIGINT/SIGTERM handler so threads can shut down cleanly.
    // Save the previous handlers so we can restore them on exit (in case the
    // legacy engine path runs after us in some test setup).
    g_engine_sharded_shutdown = 0;
    auto prev_int  = std::signal(SIGINT,  EngineSharded_SignalHandler);
    auto prev_term = std::signal(SIGTERM, EngineSharded_SignalHandler);
    // Wire the Binance reconnect helper to our shutdown flag so its delay
    // sleep is interruptible. Without this, closing the GUI during a
    // reconnect window blocks for up to cfg.reconnect_delay seconds.
    g_binance_shutdown_flag = &g_engine_sharded_shutdown;

    // Try to open the real Binance stream. If it fails — or if the cfg
    // explicitly forces synthetic mode — fall back to the synthetic tick
    // generator so the latency testbed still runs.
    static BinanceStream bs;  // static so the producer thread can reach it
    bool use_synthetic;
    if (cfg.sharded_force_synthetic) {
        use_synthetic = true;  // explicit cfg override, don't even try Binance
    } else {
        use_synthetic = !BinanceStream_Init(&bs, &bcfg);
    }

    fprintf(stderr, "\n");
    fprintf(stderr, "================================================================\n");
    fprintf(stderr, "[sharded] STARTING in per-core sharded mode\n");
    fprintf(stderr, "[sharded] num_execution_cores = %u\n", (unsigned)cfg.num_execution_cores);

    // Partial exits P.1 (2026-04-27): validate cfg before allocating cores.
    // When partial_exit_enabled=1, refuses to start if num_execution_cores * 2
    // exceeds MAX_PORTFOLIO_POSITIONS (each core uses 2 portfolio slots in
    // pair mode). When disabled (default), returns 1 unconditionally.
    // Logs the activation line "[partial-exits] enabled: ..." so live engine
    // operators can see whether partials are armed at boot.
    //
    // NOTE: P.1 only validates + announces. Actual partial-exit BEHAVIOR
    // (hot-path TP1 detection, OMS leg-aware booking, strategy dual-TP
    // wiring) lands in P.2-P.4. With partial_exit_enabled=1 in this build,
    // the validation passes but trades still execute single-leg.
    if (!Sharded_ValidatePartialExitCfg(&cfg)) {
        fprintf(stderr, "[sharded] FATAL: partial-exit cfg validation failed. "
                        "Refusing to start.\n");
        return;
    }
    if (use_synthetic) {
        fprintf(stderr, "[sharded] Binance stream not available — using SYNTHETIC ticks\n");
    } else {
        fprintf(stderr, "[sharded] Binance stream connected — using REAL market ticks\n");
        fprintf(stderr, "[sharded] symbol: %s\n", bcfg.symbol);
    }

    // === Live trading setup (use_real_money) ===
    // Mirrors the legacy engine pattern in main.cpp:212-237. Loads secrets,
    // initializes the BinanceAdapter (which spawns one worker thread per
    // BinanceOrderAPI instance — currently 1 worker for phase 02), prints
    // a 10-second warning if running against PRODUCTION (not testnet).
    // The adapter state is static so the drainer lambda can capture it
    // across the function scope.
    //
    // Phase 02 changed this from a single BinanceOrderAPI to a
    // BinanceAdapterState because the underlying BinanceOrderAPI is not
    // thread-safe. The adapter owns one BinanceOrderAPI instance per
    // worker thread (per-thread, not shared). See BinanceAdapter.hpp.
    static BinanceAdapterState g_sharded_binance_adapter;
    bool live_trading = (cfg.use_real_money != 0);
    if (live_trading) {
        char api_key[128] = {}, api_secret[128] = {};
        if (!LoadSecrets("secrets.cfg", api_key, api_secret)) {
            fprintf(stderr, "[sharded] ERROR: use_real_money=1 but secrets.cfg missing or incomplete\n");
            std::signal(SIGINT, prev_int);
            std::signal(SIGTERM, prev_term);
            return;
        }
        const char *rest_host = bcfg.use_testnet ? "testnet.binance.vision" : "api.binance.us";
        // Phase 02 ships with worker_count = 1. Scale up to 2-4 in a
        // follow-on commit after the back-to-back stress test passes.
        if (!BinanceAdapter_Init(&g_sharded_binance_adapter, rest_host,
                                  api_key, api_secret, bcfg.symbol,
                                  &g_sharded_order_lat, /*worker_count=*/1)) {
            fprintf(stderr, "[sharded] ERROR: failed to init BinanceAdapter at %s\n", rest_host);
            std::signal(SIGINT, prev_int);
            std::signal(SIGTERM, prev_term);
            return;
        }
        fprintf(stderr, "[sharded] LIVE TRADING ENABLED on %s — real orders will be placed\n",
                bcfg.use_testnet ? "TESTNET" : "PRODUCTION");
        if (!bcfg.use_testnet) {
            fprintf(stderr, "[sharded] SAFETY: real money. Starting in 10 seconds... (Ctrl+C to abort)\n");
            for (int i = 0; i < 10 && !g_engine_sharded_shutdown; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (g_engine_sharded_shutdown) {
                fprintf(stderr, "[sharded] aborted before start.\n");
                BinanceAdapter_ShutdownState(&g_sharded_binance_adapter);
                std::signal(SIGINT, prev_int);
                std::signal(SIGTERM, prev_term);
                return;
            }
        }
        fprintf(stderr, "[sharded] mode: LIVE — orders to %s\n",
                bcfg.use_testnet ? "TESTNET" : "PRODUCTION");
    } else {
        fprintf(stderr, "[sharded] mode: PAPER — internal state only, no orders submitted.\n");
    }
    fprintf(stderr, "[sharded] Press Ctrl+C to stop and dump per-core stats.\n");
    fprintf(stderr, "================================================================\n");

    ShardedOrderLatency_Reset(&g_sharded_order_lat);

    double tsc_ghz = EngineSharded_CalibrateTscGhz();
    fprintf(stderr, "[sharded] TSC calibrated at %.4f GHz\n", tsc_ghz);

    int num_cores = (int)cfg.num_execution_cores;
    if (num_cores < 1) num_cores = 1;
    if (num_cores > MAX_EXECUTION_CORES) num_cores = MAX_EXECUTION_CORES;

    // Phase 0.1 — orphan BTC recovery + live starting balance.
    // Mirrors legacy main.cpp behavior: query exchange balances at boot, and
    // if any BTC exists (sharded has no persistence yet, so BTC at boot is
    // by definition orphan from a prior session), market-sell to recover
    // USDT. Use the post-recovery USDT as the starting balance for the OMS
    // — the cfg's starting_balance is only used in paper mode.
    //
    // Notify isn't initialized yet (it's set up later, after OMS init), so
    // alerts at this point go to stderr only. That's acceptable: stderr is
    // line-buffered to a file (logging/engine.log), and any failure here
    // is fatal anyway — the user will see it on next start.
    FPN<F> live_starting_balance = cfg.starting_balance;
    if (live_trading) {
        double usdt_recovered = 0.0, btc_remaining = 0.0;
        if (!EngineSharded_OrphanRecovery(&g_sharded_binance_adapter,
                                           /*notify=*/nullptr,
                                           &usdt_recovered,
                                           &btc_remaining)) {
            fprintf(stderr, "[sharded] FATAL: orphan recovery failed — refusing to start with unknown exchange state\n");
            BinanceAdapter_ShutdownState(&g_sharded_binance_adapter);
            std::signal(SIGINT, prev_int);
            std::signal(SIGTERM, prev_term);
            return;
        }
        live_starting_balance = FPN_FromDouble<F>(usdt_recovered);
        fprintf(stderr, "[sharded] LIVE starting balance set from exchange: $%.2f\n",
                usdt_recovered);
    }

    // OMS phase 03 chunk 1B: construct the OMS first with the bank state,
    // then wire the EventLoopState to point at it. Financial state lives
    // in OrderManagerState; EventLoopState is a thin dispatcher.
    ExchangeAdapter<F> exchange_adapter{};  // value-init: all pointers null (paper mode default)
    if (live_trading) {
        exchange_adapter = BinanceAdapter_Get<F>(&g_sharded_binance_adapter);
    }
    OrderManagerState<F> oms;
    OrderManager_Init(&oms, exchange_adapter, live_trading ? 1 : 0,
                      live_starting_balance, cfg.fee_rate,
                      (int)cfg.oms_event_log_mode);
    // Phase 8 (post-coding c9) — explicit maker/taker rates so HandleFill's
    // per-fill rate selection actually works. Init defaults both = fee_rate
    // (legacy compat); engine layer sets the real values from cfg here.
    // For live mode, this picks up the cfg-loaded maker/taker rates. For
    // backtest sharded, these are also set explicitly there (BacktestSharded.hpp).
    oms.fee_rate_maker = cfg.fee_rate_maker;
    oms.fee_rate_taker = cfg.fee_rate_taker;
    // v4.3.2 (Track C.1) — Binance BNB-pay 25% fee discount. Apply at
    // boot so all fee math sites (sizing's no-trade-band, Fee_Compute,
    // OnEvent's exit fee, kill-switch margin estimation) see the
    // discounted rates uniformly. User must also enable BNB fee payment
    // in Binance UI for this to actually apply on live fills.
    if (cfg.pay_fees_in_bnb) {
        FPN<F> bnb_factor = FPN_FromDouble<F>(0.75);
        oms.fee_rate_maker = FPN_Mul(oms.fee_rate_maker, bnb_factor);
        oms.fee_rate_taker = FPN_Mul(oms.fee_rate_taker, bnb_factor);
        fprintf(stderr,
            "[sharded] BNB fee discount ENABLED — maker=%.4f%% taker=%.4f%% "
            "(verify Binance UI 'pay fees in BNB' is also on)\n",
            FPN_ToDouble(oms.fee_rate_maker) * 100.0,
            FPN_ToDouble(oms.fee_rate_taker) * 100.0);
    }
    // v4.2.1 — paper-mode slippage simulation. Cfg-driven (engine.cfg
    // slippage_pct). Live mode reads exchange fill prices directly so this
    // value is ignored (EventLoop_OnEvent gates on live_trading).
    oms.slippage_pct = cfg.slippage_pct;
    // Partials geometry mirrored to OMS for the post-fill drainer's
    // slot→core_id mapping. Set once at init — toggle requires snapshot v3
    // reload anyway (see Snapshot Re-Activation Invariant).
    oms.partial_exit_enabled = cfg.partial_exit_enabled ? 1 : 0;

    // Trade log CSV — same pattern as legacy engine in main.cpp
    static ShardedTradeLog g_sharded_trade_log;
    ShardedTradeLog_Init(&g_sharded_trade_log, bcfg.symbol);
    oms.trade_log = &g_sharded_trade_log;

    EventLoopState<F> state;
    EventLoopState_Init(&state, &oms);

    // Phase 04: start the user data websocket for real-time fills.
    // Uses its own BinanceOrderAPI instance for listen key REST calls.
    // The ws_result_queue is a dedicated SPSC ring inside the OMS.
    static BinanceUserDataState g_user_data;
    if (live_trading) {
        char ud_api_key[128] = {}, ud_api_secret[128] = {};
        LoadSecrets("secrets.cfg", ud_api_key, ud_api_secret);
        const char* ws_host = bcfg.use_testnet
            ? "testnet.binance.vision" : "stream.binance.com";
        const char* rest_host = bcfg.use_testnet
            ? "testnet.binance.vision" : "api.binance.us";
        if (BinanceUserData_Init(&g_user_data, ws_host, rest_host,
                                  ud_api_key, ud_api_secret, bcfg.symbol,
                                  &oms.ws_result_queue)) {
            BinanceUserData_Start(&g_user_data);
            g_sharded_binance_adapter.ws_active.store(1, std::memory_order_release);
            fprintf(stderr, "[sharded] user data websocket started\n");
        } else {
            fprintf(stderr, "[sharded] user data websocket init failed, "
                             "falling back to REST-only fills\n");
        }
    }

    // Phase 8b (post-coding c8) — NotifyState for operational alerts.
    // Same setup as legacy path: stderr backend by default, command backend
    // (popen) for dunst/Discord/Slack/Telegram/etc when configured. Off by
    // default (notify_enabled=0). Notify_Send call sites in
    // PortfolioController + BinanceCrypto/Depth/UserData are shared headers
    // — they fire in both modes; g_notify being non-null is what gates
    // actual delivery.
    static NotifyState g_notify_state;
    static NotifyCommandState g_notify_cmd_state;
    if (cfg.notify_enabled) {
        NotifyBackendFn backend = NotifyBackend_Stderr;
        void *backend_state = nullptr;
        if (cfg.notify_backend == 1) {
            if (cfg.notify_command[0] == '\0') {
                fprintf(stderr, "[sharded] notify backend=command but notify_command "
                                "is empty — falling back to stderr\n");
            } else {
                strncpy(g_notify_cmd_state.template_str, cfg.notify_command,
                        sizeof(g_notify_cmd_state.template_str) - 1);
                g_notify_cmd_state.template_str[sizeof(g_notify_cmd_state.template_str) - 1] = '\0';
                backend = NotifyBackend_Command;
                backend_state = &g_notify_cmd_state;
            }
        } else if (cfg.notify_backend != 0) {
            fprintf(stderr, "[sharded] notify backend=%d not recognized — "
                            "falling back to stderr\n", cfg.notify_backend);
        }
        NotifyState_Init(&g_notify_state, backend, backend_state,
                         (uint64_t)cfg.notify_cooldown_secs * 1000000ULL);
        if (g_notify_state.worker_started) {
            g_notify = &g_notify_state;
            fprintf(stderr, "[sharded] notify enabled (backend=%s, cooldown=%us)\n",
                    backend == NotifyBackend_Stderr ? "stderr" : "command",
                    cfg.notify_cooldown_secs);
        }
    }

    // Phase 8a (post-coding c7) — TickRecorder for raw market tick CSV audit.
    // Same pattern as legacy path. Off by default (record_ticks=0).
    static TickRecorder g_tick_rec;
    TickRecorder_Init(&g_tick_rec, bcfg.symbol, cfg.record_ticks, cfg.record_max_days);

    // Phase 8a (post-coding c6) — depth feed + DepthRecorder.
    // Same setup as main.cpp's legacy path, runs only when depth_enabled=1.
    // Track E.3 (2026-04-26): book_imbalance is now consumed in the slow
    // path below via EventLoop_RebuildAllParameters. Symmetric with
    // BacktestSharded_Run reading from DepthReplayState.
    static DepthRecorder g_depth_rec;
    static DepthSharedState<F> g_depth_shared;
    static pthread_t g_depth_tid = 0;
    DepthRecorder_Init(&g_depth_rec, bcfg.symbol, "data", cfg.record_max_days,
                       cfg.record_depth && cfg.depth_enabled);
    if (cfg.depth_enabled) {
        const char *depth_host;
        int depth_port;
        if (bcfg.use_testnet)         { depth_host = "testnet.binance.vision";   depth_port = 443; }
        else if (bcfg.use_binance_us) { depth_host = "stream.binance.us";        depth_port = 9443; }
        else                          { depth_host = "data-stream.binance.vision"; depth_port = 443; }

        if (DepthStream_Init<F>(&g_depth_shared, bcfg.symbol,
                                 depth_host, depth_port,
                                 /*reconnect_delay=*/2) == 0) {
            g_depth_shared.recorder = cfg.record_depth ? &g_depth_rec : NULL;
            pthread_create(&g_depth_tid, NULL, depth_thread_fn<F>, &g_depth_shared);
            fprintf(stderr, "[sharded] depth feed active (%s:%d %s@depth5@100ms)%s\n",
                    depth_host, depth_port, bcfg.symbol,
                    cfg.record_depth ? " — recording" : "");
        } else {
            fprintf(stderr, "[sharded] depth feed init failed — continuing without depth\n");
        }
    }

    // Phase 05: reconciliation poller — own REST instance, periodic
    // balance check, pushes CMD_RECONCILE to oms.reconcile_queue.
    static ReconciliationLoopState<F> g_reconciler;
    if (live_trading) {
        char rc_key[128] = {}, rc_secret[128] = {};
        LoadSecrets("secrets.cfg", rc_key, rc_secret);
        const char* rc_host = bcfg.use_testnet
            ? "testnet.binance.vision" : "api.binance.us";
        if (ReconciliationLoop_Init(&g_reconciler, rc_host, rc_key, rc_secret,
                                     bcfg.symbol, &oms, 30, 0.01)) {
            ReconciliationLoop_Start(&g_reconciler);
        } else {
            fprintf(stderr, "[sharded] reconciler init failed\n");
        }
    }

    // Per-core resources. Static so they live in BSS, not the stack —
    // ExecutionCore is ~66KB and num_cores * size could blow the stack.
    static SPSCRing<Tick<F>, EXECUTION_CORE_TICK_RING_SIZE> tick_rings[MAX_EXECUTION_CORES];
    static ExecutionCore<F> cores[MAX_EXECUTION_CORES];
    // Rolling stats for the slow-path strategy rebuild. Producer thread
    // pushes ticks into these on cadence and rebuilds gate params from them.
    static RollingStats<F, 128> rolling_short = RollingStats_Init<F, 128>();
    static RollingStats<F, 512> rolling_long  = RollingStats_Init<F, 512>();
    // v4.0 train-serve parity: RORRegressor + EMA price track the same state
    // legacy PortfolioController maintains, so ML_BuildParameters can call
    // Regime_ComputeSignals and produce ALL the features ModelFeatures_Pack
    // reads — not just a subset. Without these, sig->ror_slope,
    // sig->ema_sma_spread, sig->ema_above_sma stayed at zero in sharded
    // while backtest produced them non-zero, causing model train/serve drift.
    static RORRegressor<F> regime_ror = RORRegressor_Init<F>();
    static FPN<F> ema_price = FPN_Zero<F>();
    // v4.3 — feature-pack expansion. Shared per-engine (not per-core) since
    // these describe market state, not core state. Same population cadence
    // as legacy PortfolioController so train-serve parity is preserved.
    static RollingStats<F, 256>  rolling_medium   = RollingStats_Init<F, 256>();
    static RollingStats<F, 1024> rolling_baseline = RollingStats_Init<F, 1024>();
    static CumDeltaState<F>      cumdelta_state;
    static TickRateState         tick_rate_state;
    static int                   v43_state_initialized = 0;
    if (!v43_state_initialized) {
        CumDelta_Init(&cumdelta_state);
        TickRate_Init(&tick_rate_state);
        v43_state_initialized = 1;
    }
    // EMA alpha matches PortfolioController_Init's default (gate_ema_alpha
    // is the cfg key). Computed at boot from cfg; updated each tick.
    FPN<F> ema_alpha = !FPN_IsZero(cfg.gate_ema_alpha)
                       ? cfg.gate_ema_alpha
                       : FPN_FromDouble<F>(0.1);
    rolling_short = RollingStats_Init<F, 128>();
    rolling_long  = RollingStats_Init<F, 512>();
    rolling_medium = RollingStats_Init<F, 256>();
    rolling_baseline = RollingStats_Init<F, 1024>();
    CumDelta_Init(&cumdelta_state);
    TickRate_Init(&tick_rate_state);

    // v4.5 Wave 1 — D.1/D.2/D.4 state. Same lifetime + reset shape as the
    // v4.3 state above. BookImbalanceHistory pushes from the slow-path
    // book_imbalance read; FlowState + LargeTradeState push from per-tick
    // flow + size in fan_out.
    static BookImbalanceHistory<F, 1024> book_imb_history;
    static FlowState                     flow_state;
    static LargeTradeState<F, 1024>      large_trade_state;
    BookImbHistory_Init(&book_imb_history);
    FlowState_Init(&flow_state);
    LargeTradeState_Init(&large_trade_state);
    // v4.6 Wave 2 — D.3 spread state. Pushed at slow-path with current
    // spread from g_depth_shared.snapshots[active].spread.
    static SpreadState<F, 1024> spread_state;
    SpreadState_Init(&spread_state);

    // Per-core risk allocation: split the configured starting balance across
    // cores. Each core gets its own mini-portfolio that's risk_pct of the
    // total. With risk_pct=10% and 4 cores starting at $10k, each core gets
    // $250 to risk on a single trade.
    double total_balance = FPN_ToDouble(cfg.starting_balance);
    double default_risk = FPN_ToDouble(cfg.risk_pct);
    if (default_risk <= 0.0) default_risk = 0.10;
    double default_per_core = (total_balance * default_risk) / (double)num_cores;
    if (default_per_core < 1.0) default_per_core = 1.0;

    for (int i = 0; i < num_cores; ++i) {
        SPSCRing_Init(&tick_rings[i]);
        ExecutionCore_Init(&cores[i], (uint16_t)i, &tick_rings[i]);
        EventLoopState_RegisterCore(&state, &cores[i],
            FPN_Zero<F>(),  // intended_tp will be set by slow-path rebuild
            FPN_Zero<F>(),  // intended_sl ditto
            FPN_Zero<F>()); // intended_qty ditto
        // per-core risk: use core-specific override if set, else shared/even split
        double core_balance = default_per_core;
        if (!FPN_IsZero(cfg.core_risk_pct[i])) {
            core_balance = total_balance * FPN_ToDouble(cfg.core_risk_pct[i]);
            if (core_balance < 1.0) core_balance = 1.0;
        }
        EventLoopState_SetCoreStrategy(&state, i,
            cfg.core_strategies[i],
            FPN_FromDouble<F>(core_balance));

        // Load ML model zoo for STRATEGY_ML cores. Three resolution paths:
        //   1. core_N_model_dir set → auto-discover all roles in directory
        //      (barrier.json, buy_signal.json, regime.json, exit.json)
        //   2. core_N_model_path set → load single buy_signal model (legacy)
        //   3. ml_model_path set globally → load single buy_signal (legacy fallback)
        // The dispatcher passes model_handle as void* — we point it at the zoo.
        if (cfg.core_strategies[i] == STRATEGY_ML) {
            static CoreModelZoo<F> ml_zoos[MAX_EXECUTION_CORES];
            CoreModelZoo_Init(&ml_zoos[i]);
            int backend = cfg.ml_backend ? cfg.ml_backend : MODEL_BACKEND_XGBOOST;

            int loaded = 0;
            if (cfg.core_model_dir[i][0]) {
                // path 1: zoo from directory (auto-discovered roles)
                loaded = CoreModelZoo_LoadFromDir(&ml_zoos[i], cfg.core_model_dir[i], backend);
                fprintf(stderr, "[sharded] core %d: zoo from %s, %d role(s) loaded\n",
                        i, cfg.core_model_dir[i], loaded);
            } else {
                // paths 2-3: legacy single buy_signal model
                const char* model_path = cfg.core_model_path[i][0]
                    ? cfg.core_model_path[i] : cfg.ml_model_path;
                if (model_path[0]) {
                    loaded = CoreModelZoo_LoadLegacy(&ml_zoos[i], model_path, backend);
                    if (loaded) {
                        fprintf(stderr, "[sharded] core %d: legacy buy_signal model loaded from %s\n",
                                i, model_path);
                    } else {
                        fprintf(stderr, "[sharded] core %d: ML model load FAILED (%s), "
                                         "falling back to SimpleDip\n", i, model_path);
                    }
                }
            }

            if (loaded) {
                state.cores[i].model_handle = &ml_zoos[i];
                // stupid-proof verification: read expected.cfg from the run
                // bundle and warn (or fail in strict mode) on any mismatch
                // between the model's training config and the live engine.cfg.
                if (cfg.core_model_dir[i][0]) {
                    int verify_ok = CoreModelZoo_VerifyExpected(&ml_zoos[i],
                        cfg.core_model_dir[i],
                        cfg.barrier_gate_enabled,
                        FPN_ToDouble(cfg.ml_buy_threshold),
                        cfg.model_verify_strict, i,
                        // v4.3.1: train-serve cadence + feature format check
                        cfg.poll_interval,
                        (unsigned)MODEL_FORMAT_VERSION);
                    if (!verify_ok && cfg.model_verify_strict > 0) {
                        // strict mode + mismatch: detach model, treat as "no model loaded"
                        // (executor falls back to SimpleDip per ML_BuildParameters)
                        fprintf(stderr, "[sharded] core %d: ML model UNLOADED due to strict verify failure\n", i);
                        CoreModelZoo_Free(&ml_zoos[i]);
                        state.cores[i].model_handle = NULL;
                    }
                }
            }

            // Phase 6prep sharded c12: re-init ConfidenceScorer with cfg
            // tunables. EventLoopState_Init left it at safe defaults; for
            // ML cores we want the user's window/tau settings active.
            ConfidenceScorer_Init(&state.cores[i].confidence,
                                  (int)cfg.confidence_window,
                                  FPN_ToDouble(cfg.confidence_freshness_tau));
        }

        // Cores start permission=0. The slow-path rebuild grants permission
        // once it has enough rolling-stats samples to compute meaningful
        // gate thresholds.
        ExecutionCore_SetPermission(&cores[i], 0);

        // Enable per-core latency sampling — the whole point of this mode
        CoreLatencyStats_Enable(&cores[i].latency_stats);
    }

    // Phase 4 — load persisted state. Cores are now registered + initialized
    // with cfg-derived values; this overlays any previous session's regime
    // hysteresis, pnl_feeder, kill switch peak/trips, P&L counters,
    // open positions. Refuses cleanly on bad magic / version / core-count
    // mismatch (logs + starts fresh — never crashes).
    //
    // Snapshot path: data/sharded_snapshot.dat. Live mode skips load —
    // orphan recovery already established the live balance from the
    // exchange, and persisted positions could be wrong if the user manually
    // traded between sessions. Paper mode is the safe-to-resume case.
    {
        const char* snapshot_path = "data/sharded_snapshot.dat";
        // Ensure data/ exists (mkdir is idempotent — silent if it does)
        mkdir("data", 0755);
        if (!live_trading) {
            int loaded = ShardedSnapshot_Load<F>(&state, snapshot_path,
                                                  cfg.partial_exit_enabled ? 1 : 0);
            (void)loaded;  // logged inside; nothing else to do here
        } else {
            fprintf(stderr, "[snapshot] LIVE mode: skipping snapshot load "
                            "(exchange-truth-of-state)\n");
        }
    }

    std::atomic<bool> producer_done{false};
    std::atomic<uint64_t> ticks_produced{0};
    std::atomic<uint64_t> ticks_consumed_total{0};
    // Latest market data for the live TUI panel. Producer writes, render
    // thread reads. Doubles are 8-byte aligned so the read/write is atomic
    // on x86 even without an explicit atomic; we relax that for simplicity
    // since the TUI is informational, not load-bearing.
    std::atomic<double> last_price{0.0};
    std::atomic<double> last_volume{0.0};

    //----------------------------------------------------------------------
    // GUI thread (ImGui build only)
    //----------------------------------------------------------------------
    // Same double-buffered TUISharedState pattern as the legacy engine in
    // main.cpp. The producer thread's slow-path populates TUISnapshot via
    // TUI_CopySnapshotSharded. The GUI thread reads the front buffer.
#ifdef USE_IMGUI_GUI
    static TUISharedState g_shared;
    memset(&g_shared, 0, sizeof(g_shared));
    g_shared.config_path = "engine_sharded.cfg";
    g_shared.active_idx = 0;
    g_shared.quit_requested = 0;
    g_shared.pause_requested = 0;
    g_shared.reload_requested = 0;
    g_shared.drag_slot = -1;
    for (int i = 0; i < 16; ++i) g_shared.swap_strategy_requested[i] = STRATEGY_NONE;
    // Wire the signal handler's GUI-quit pointer to this g_shared. After
    // this assignment, SIGINT will set BOTH g_engine_sharded_shutdown AND
    // g_shared.quit_requested in one atomic-ish step, ensuring the GUI
    // thread can drop out of its SDL event loop in the same tick as the
    // engine threads see the shutdown flag.
    g_engine_sharded_gui_quit_ptr = &g_shared.quit_requested;
    static CandleAccumulator g_candle_acc;
    CandleAccumulator_Init(&g_candle_acc, 60);
    g_shared.candle_acc = &g_candle_acc;
    pthread_t gui_tid;
    pthread_create(&gui_tid, NULL, gui_thread_fn, &g_shared);
#endif

    //----------------------------------------------------------------------
    // Producer thread — generates synthetic ticks and fans out to all cores
    //----------------------------------------------------------------------
    std::thread producer([&producer_done, &ticks_produced, &bcfg, &last_price, &last_volume,
                          &cfg, &state, num_cores, use_synthetic, tsc_ghz,
                          &ema_price, &ema_alpha, &regime_ror, live_trading] {
        EngineSharded_PinThread(0);  // best-effort pin to CPU 0
        uint64_t seq = 0;
        // v4.3.1 — slow_path_interval now reads cfg.poll_interval to match
        // backtest (which always used cfg.poll_interval=100 default).
        // Pre-v4.3.1 sharded hardcoded this to 8, which meant rolling
        // stats covered 12.5× less time history at SERVING (sharded) than
        // at TRAINING (backtest) — silent train-serve drift on every
        // RollingStats-derived feature (slope, R², variance, EMA spread,
        // etc.). Aligning eliminates the drift.
        // Cost: sharded slow path fires 12.5× less often. Slow path is
        // not perf-critical (microseconds per fire, runs on dedicated
        // producer CPU). User can override via engine.cfg poll_interval=N
        // for tighter cadence at the cost of train-serve parity.
        int slow_path_interval = (int)cfg.poll_interval;
        if (slow_path_interval < 1) slow_path_interval = 100;
        fprintf(stderr, "[sharded] slow_path_interval = %d ticks "
                        "(from cfg.poll_interval — matches backtest cadence)\n",
                slow_path_interval);
        int slow_path_counter = 0;

        // Helper that fans a single tick out to every core's tick ring,
        // updates rolling stats, and runs the slow-path rebuild on cadence.
        auto fan_out = [num_cores, &seq, &ticks_produced, &last_price, &last_volume,
                        &cfg, &state, &slow_path_counter, slow_path_interval, tsc_ghz,
                        &ema_price, ema_alpha, &regime_ror, live_trading,
                        &rolling_medium, &rolling_baseline, &cumdelta_state, &tick_rate_state,
                        &book_imb_history, &flow_state, &large_trade_state,
                        &spread_state]
                       (double price_d, double volume_d, uint64_t ts_us,
                        int is_buyer_maker = 0) {  // v4.3 — defaulted for synthetic paths
            Tick<F> t;
            memset(&t, 0, sizeof(t));
            t.price = FPN_FromDouble<F>(price_d);
            t.volume = FPN_FromDouble<F>(volume_d);
            t.timestamp = ts_us;
            t.sequence = seq++;
            t.is_buyer_maker = (uint8_t)(is_buyer_maker ? 1 : 0);
            for (int c = 0; c < num_cores; ++c) {
                while (!SPSCRing_TryPush(&tick_rings[c], t)) {
                    if (g_engine_sharded_shutdown) return false;
                }
                if (g_engine_sharded_shutdown) return false;
            }
            ticks_produced.fetch_add(1, std::memory_order_relaxed);
            last_price.store(price_d, std::memory_order_relaxed);
            last_volume.store(volume_d, std::memory_order_relaxed);

            // v4.0 train-serve parity: update EMA price on every tick (matches
            // legacy PortfolioController_Tick behavior). Used by ML feature
            // pack — without this, sig->ema_sma_spread + sig->ema_above_sma
            // stay zero in sharded while backtest produces them non-zero.
            // First-tick branchless: if ema is zero, take current price as-is;
            // otherwise standard exponential smoothing.
            FPN<F> one_minus_alpha = FPN_Sub(FPN_FromDouble<F>(1.0), ema_alpha);
            FPN<F> ema_new = FPN_Add(
                FPN_Mul(ema_price, ema_alpha),
                FPN_Mul(t.price, one_minus_alpha));
            if (FPN_IsZero(ema_price)) ema_price = t.price;
            else                       ema_price = ema_new;

            // Phase 8a (post-coding c7) — record raw tick to CSV when enabled.
            // No-op when record_ticks=0 (the gate is inside TickRecorder_Push).
            // is_buyer_maker not available from the sharded fan_out yet; pass 0.
            // (Legacy path passes the real value from BinanceStream tick read.)
            TickRecorder_Push(&g_tick_rec, price_d, volume_d, (int64_t)ts_us, 0);

#ifdef USE_IMGUI_GUI
            // Feed candles for the chart panel (same pattern as main.cpp:396)
            CandleAccumulator_Push(&g_candle_acc, price_d, volume_d, 0);
#endif

            // Slow path: feed rolling stats and rebuild gate parameters
            // every poll_interval ticks. Matches the legacy controller's
            // sampling cadence so the per-core threshold computation gets
            // the same input distribution.
            slow_path_counter++;
            if (slow_path_counter >= slow_path_interval) {
                slow_path_counter = 0;
                RollingStats_Push(&rolling_short, t.price, t.volume);
                RollingStats_Push(&rolling_long,  t.price, t.volume);
                // v4.3 — feed expanded feature-pack state. is_buyer_maker not
                // available from sharded fan_out yet (TODO: thread it from
                // BinanceStream). For now, default to 0 = treat all volume as
                // taker-buy aggression. This matches train-serve parity for
                // the backtest's CSV-aware path only when we plumb is_buyer_maker
                // through both. Until that plumbing lands in v4.3.1, cumdelta
                // skews high-aggression-positive in live; backtest still gets
                // the real signed delta from CSV.
                RollingStats_Push(&rolling_medium, t.price, t.volume);
                RollingStats_Push(&rolling_baseline, t.price, t.volume);
                CumDelta_Push(&cumdelta_state, t.volume, is_buyer_maker);
                TickRate_Push(&tick_rate_state, ts_us);
                // v4.0 train-serve parity: feed rolling slope to RORRegressor
                // (slope of slopes = trend acceleration). Matches legacy at
                // PortfolioController.hpp:1552. Without this, sig->ror_slope
                // is zero in sharded while backtest produces it non-zero.
                {
                    LinearRegression3XResult<F> slope_sample;
                    slope_sample.model.slope     = rolling_short.price_slope;
                    slope_sample.model.intercept = FPN_Zero<F>();
                    slope_sample.r_squared       = rolling_short.price_r_squared;
                    RORRegressor_Push(&regime_ror, slope_sample);
                }
                // v4.5 Wave 1 — push flow + large-trade state at slow-path
                // cadence (mirror of ShardedBacktestDriver). BookImbalance-
                // History pushed below after we read the active depth
                // snapshot. signed_vol mirrors CumDelta sign convention.
                {
                    double signed_vol = volume_d;
                    if (is_buyer_maker) signed_vol = -signed_vol;
                    FlowState_Push(&flow_state, ts_us, signed_vol);
                    LargeTradeState_Push(&large_trade_state, t.volume);
                }
#ifdef USE_IMGUI_GUI
                // v4.0 hot-swap strategy: GUI requests are picked up here.
                // STRATEGY_NONE (0xFF) = no request; any other value swaps
                // the core's strategy. Open positions are honored — the swap
                // waits until the position closes naturally so the old
                // strategy's TP/SL still applies to its own entry.
                for (int c = 0; c < num_cores && c < 16; ++c) {
                    uint8_t pending = __atomic_load_n(
                        &g_shared.swap_strategy_requested[c], __ATOMIC_ACQUIRE);
                    if (pending == STRATEGY_NONE) continue;
                    if ((state.oms->portfolio.active_bitmap &
                         (uint16_t)(1u << c)) != 0) {
                        continue;  // position open; defer until exit
                    }
                    // v4.0 audit: refuse swap-to-ML if this core never had
                    // a model loaded. Otherwise ML_BuildParameters would
                    // silently fall back to SimpleDip while the GUI claims
                    // the core is ML — confusing failure mode. model_handle
                    // is set at engine boot for cores configured with
                    // core_N_strategy=ml + a model path; we can't load a
                    // model mid-session because that requires file I/O off
                    // the controller thread.
                    if (pending == STRATEGY_ML &&
                        state.cores[c].model_handle == NULL) {
                        fprintf(stderr,
                                "[sharded] core %d: refusing swap to ML — no model "
                                "loaded for this core. Set core_%d_model_dir or "
                                "core_%d_model_path in engine.cfg + restart.\n",
                                c, c, c);
                        __atomic_store_n(&g_shared.swap_strategy_requested[c],
                                         STRATEGY_NONE, __ATOMIC_RELEASE);
                        continue;
                    }
                    uint8_t old_strat = state.cores[c].strategy_id;
                    state.cores[c].strategy_id = pending;
                    __atomic_store_n(&g_shared.swap_strategy_requested[c],
                                     STRATEGY_NONE, __ATOMIC_RELEASE);
                    fprintf(stderr,
                            "[sharded] core %d: strategy swapped %u -> %u\n",
                            c, (unsigned)old_strat, (unsigned)pending);
                }

                // v4.0: GUI drag-TP/SL pickup. Mirrors the legacy main.cpp
                // handler at main.cpp:585. Sharded mode previously ignored
                // drag_slot (only legacy consumed it), so dragging TP/SL
                // lines on the chart silently did nothing in sharded mode.
                // Writes go directly to the OMS portfolio's position fields.
                {
                    int slot = __atomic_load_n(&g_shared.drag_slot, __ATOMIC_ACQUIRE);
                    if (slot >= 0 && slot < 16) {
                        int is_tp = g_shared.drag_is_tp;
                        double dprice = g_shared.drag_price;
                        __atomic_store_n(&g_shared.drag_slot, -1, __ATOMIC_RELEASE);
                        auto *pos = &state.oms->portfolio.positions[slot];
                        if (state.oms->portfolio.active_bitmap & (uint16_t)(1u << slot)) {
                            if (is_tp)
                                pos->take_profit_price = FPN_FromDouble<F>(dprice);
                            else
                                pos->stop_loss_price = FPN_FromDouble<F>(dprice);
                            fprintf(stderr, "[sharded] GUI drag: slot %d %s -> $%.2f\n",
                                    slot, is_tp ? "TP" : "SL", dprice);
                        }
                    }
                }

                // v4.0 hot-reload: GUI Settings panel writes engine.cfg and
                // sets reload_requested. Sharded mode previously ignored this
                // (only legacy main.cpp consumed it), so per-core overrides
                // edited via the GUI never took effect until restart. Re-read
                // engine.cfg, copy reloadable fields into the in-memory cfg.
                //
                // Boot-only fields are preserved (engine_mode, num_execution_cores,
                // model paths, starting_balance, fee_rate_maker/taker, exchange
                // routing, recording flags). Tunables are bulk-copied including
                // the per-core overrides array.
                if (__atomic_exchange_n(&g_shared.reload_requested, 0,
                                         __ATOMIC_ACQ_REL)) {
                    ControllerConfig<F> new_cfg = ControllerConfig_Load<F>("engine.cfg");
                    // boot-only: preserve fields that the running engine cannot
                    // change live (would require thread restart, file I/O off
                    // the controller thread, or different per-core wiring).
                    new_cfg.engine_mode         = cfg.engine_mode;
                    new_cfg.num_execution_cores = cfg.num_execution_cores;
                    new_cfg.starting_balance    = cfg.starting_balance;
                    for (int c = 0; c < 16; ++c) {
                        memcpy(new_cfg.core_model_path[c],
                               cfg.core_model_path[c],
                               sizeof(cfg.core_model_path[c]));
                        memcpy(new_cfg.core_model_dir[c],
                               cfg.core_model_dir[c],
                               sizeof(cfg.core_model_dir[c]));
                    }
                    cfg = new_cfg;

                    // Phase 2.1: recompute allocated_balance after reload.
                    // Pre-2.1 the allocated_balance field was set ONCE in
                    // EngineSharded_Run init and never refreshed — changing
                    // risk_pct or core_N_risk_pct in cfg was silently
                    // stale. Mirror the startup formula here.
                    // NOTE: don't touch core_open_notional during reload —
                    // open positions still exist; the new allocation either
                    // expands or shrinks the budget but the deployed
                    // notional is unchanged.
                    {
                        double total_balance = FPN_ToDouble(cfg.starting_balance);
                        double default_risk  = FPN_ToDouble(cfg.risk_pct);
                        if (default_risk <= 0.0) default_risk = 0.10;
                        double default_per_core = (total_balance * default_risk) / (double)num_cores;
                        if (default_per_core < 1.0) default_per_core = 1.0;
                        for (int c = 0; c < num_cores; ++c) {
                            double core_balance = default_per_core;
                            if (!FPN_IsZero(cfg.core_risk_pct[c])) {
                                core_balance = total_balance * FPN_ToDouble(cfg.core_risk_pct[c]);
                                if (core_balance < 1.0) core_balance = 1.0;
                            }
                            state.cores[c].allocated_balance = FPN_FromDouble<F>(core_balance);
                        }
                    }
                    fprintf(stderr, "[sharded] cfg hot-reloaded "
                            "(per-core overrides + tunables refreshed, allocations recomputed)\n");
                }
                // Phase 3: process per-core kill resets BEFORE the rebuild
                // so a freshly-reset core can be re-evaluated this cycle.
                // Each reset clears the trip flag and refreshes peak to the
                // current value. GUI-only — g_shared lives inside the
                // USE_IMGUI_GUI ifdef.
                for (int c = 0; c < num_cores; ++c) {
                    if (g_shared.kill_reset_per_core[c]) {
                        g_shared.kill_reset_per_core[c] = 0;
                        state.cores[c].core_kill_tripped = 0;
                        state.cores[c].core_peak_balance = FPN_Zero<F>();
                        state.cores[c].core_dd_pct = FPN_Zero<F>();
                        fprintf(stderr, "[sharded] core %d kill switch RESET\n", c);
                    }
                }
#endif
                // Phase 3: pass current_price for MTM kill switch evaluation.
                // Read once from the producer atomic; tracker is realized-only
                // on the first slow path before any tick has been seen.
                FPN<F> mtm_price = FPN_FromDouble<F>(
                    last_price.load(std::memory_order_relaxed));
                // v4.3 — pass expanded feature-pack state for the model's
                // medium-horizon features. Same pointers go to both the AUTO
                // regime resolution branch (above the dispatch) and the ML
                // strategy branch (via MLBuildContext) — single source of
                // truth, train-serve parity preserved.
                uint64_t rebuild_ts_us = (uint64_t)
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                // Track E.3 (post-coding c14, finally landed 2026-04-26)
                // — feed book_imbalance from the depth thread into the
                // per-core slow-path rebuild. The depth thread runs only
                // when cfg.depth_enabled=1 and writes to active_idx with
                // RELEASE; we read with ACQUIRE for matching ordering.
                // When depth_enabled=0 OR the depth thread hasn't received
                // a snapshot yet, the value stays at FPN_Zero (init) —
                // RebuildAllParameters' gate check fails closed if cfg.
                // min_book_imbalance>0, which is the desired semantics
                // (no data → no buys, since we can't evaluate the gate).
                FPN<F> book_imb;
                FPN<F> book_spread   = FPN_Zero<F>();
                FPN<F> book_mid      = FPN_Zero<F>();
                if (cfg.depth_enabled) {
                    int dactive = __atomic_load_n(&g_depth_shared.active_idx,
                                                   __ATOMIC_ACQUIRE);
                    book_imb    = g_depth_shared.snapshots[dactive].imbalance;
                    book_spread = g_depth_shared.snapshots[dactive].spread;
                    book_mid    = g_depth_shared.snapshots[dactive].mid_price;
                } else {
                    book_imb = FPN_Zero<F>();
                }
                // v4.5 Wave 1 — push book_imbalance into history at slow-path
                // cadence. Mirror in BacktestSharded driver. Push happens
                // BEFORE RebuildAllParameters so Regime_ComputeSignals reads
                // the freshly-updated history.
                if (cfg.depth_enabled) {
                    BookImbHistory_Push(&book_imb_history, book_imb);
                    // v4.6 Wave 2 — push spread into z-score ring
                    SpreadState_Push(&spread_state, book_spread);
                }
                EventLoop_RebuildAllParameters(&state, &rolling_short, &cfg, &rolling_long,
                                                &regime_ror, &ema_price,
                                                FPN_IsZero(mtm_price) ? nullptr : &mtm_price,
                                                &rolling_medium, &rolling_baseline,
                                                &cumdelta_state, &tick_rate_state,
                                                rebuild_ts_us,
                                                cfg.depth_enabled ? &book_imb : nullptr,
                                                /* book_imb_history */ &book_imb_history,
                                                /* flow_state       */ &flow_state,
                                                /* large_trade_state*/ &large_trade_state,
                                                /* spread_state     */ &spread_state,
                                                /* current_spread   */ FPN_ToDouble(book_spread),
                                                /* current_mid_price*/ FPN_ToDouble(book_mid));

                // Phase 4 — periodic snapshot save. Once every ~1024 slow-path
                // cycles, paper mode only. With slow_path_interval=8 ticks and
                // ~10 ticks/sec that's roughly every 13 minutes — frequent
                // enough to bound state-loss-on-crash, infrequent enough to
                // not spam the disk. Atomic rename means a crash mid-save
                // leaves the previous good file intact.
                static int save_counter = 0;
                if (!live_trading && (++save_counter >= 1024)) {
                    save_counter = 0;
                    ShardedSnapshot_Save<F>(&state, "data/sharded_snapshot.dat",
                                              cfg.partial_exit_enabled ? 1 : 0);
                }
                EventLoop_PushParameters(&state);
                // KNOWN RACE (audit 2026-04-09): KillSwitchEvaluate reads
                // oms->balance from this (producer) thread while the drainer
                // thread writes it via OnEvent / OMS_Tick fill handler.
                // FPN<64> is 64 words — torn reads are possible under
                // concurrent writes. Probability is low at current event
                // rates (~1 exit/sec vs 5 Hz slow path). Consequence:
                // false-positive or missed kill switch trip from a garbage
                // FPN comparison. Pre-existing race (sharded engine always
                // had producer + drainer on separate threads).
                // TODO: move kill switch eval to drainer thread, or use an
                // atomic balance snapshot for the comparison.
                EventLoop_KillSwitchEvaluate(&state);
                // Warmup gating: don't grant permission until rolling stats
                // have meaningful data. Pre-v4.0.1 this was `count >= 1`,
                // which let MR (buy-below-avg) AND MOM (buy-above-avg) BOTH
                // fire on the second tick — every restart instantly opened
                // 2 positions with garbage TP/SL because rolling.price_avg
                // and rolling.price_stddev hadn't stabilized yet.
                //
                // Use cfg.min_warmup_samples if set (>0), else half the
                // short window (= 64). 64 samples gives a stable enough
                // rolling stddev for momentum sizing to compute non-trivial
                // TP/SL distances and prevents the both-strategies-fire
                // race during the first few ticks.
                uint32_t min_samples = cfg.min_warmup_samples > 0
                    ? cfg.min_warmup_samples : 64;
                if (rolling_short.count >= (int)min_samples) {
                    for (int c = 0; c < num_cores; ++c) {
                        if (state.cores[c].strategy_id != STRATEGY_NONE) {
                            ExecutionCore_SetPermission(&cores[c], 1);
                        }
                    }
                }

                // v4.0.3 A3: time-based exit. Walk active positions, force-close
                // any held longer than cfg.max_hold_ticks WITH gain below
                // cfg.min_hold_gain_pct. Profitable positions are kept (they're
                // still working). Mirrors legacy PortfolioController behavior.
                // No-op when max_hold_ticks=0 (disabled, the default).
                if (cfg.max_hold_ticks > 0) {
                    uint64_t now_tick = ticks_produced.load(std::memory_order_relaxed);
                    double current_price_d = last_price.load(std::memory_order_relaxed);
                    if (current_price_d > 0.01) {
                        uint16_t bm = state.oms->portfolio.active_bitmap;
                        while (bm) {
                            int slot = __builtin_ctz(bm);
                            bm &= (uint16_t)(bm - 1);
                            // elapsed = events since this core's last entry stamp
                            uint64_t entry_t = state.cores[slot].last_entry_tick;
                            if (entry_t == 0) continue;  // never stamped (shouldn't happen if active)
                            // Bug fix (2026-04-27): defensive guard against
                            // future entry_tick values. ticks_produced resets
                            // to 0 each engine session, but last_entry_tick is
                            // persisted via ShardedSnapshotPersist. After a
                            // restart with active positions in the snapshot,
                            // entry_t can be > now_tick → uint64 subtraction
                            // underflows to ~2^64 → time-exit fires every
                            // cycle, submitting redundant SELL orders that
                            // never clear the slot in the panel. Skip until
                            // next genuine entry stamps it freshly.
                            if (entry_t > now_tick) {
                                fprintf(stderr,
                                    "[sharded] core %d: stale entry_tick from "
                                    "snapshot (entry_t=%llu > now_tick=%llu); "
                                    "resetting to current tick. Time-exit "
                                    "skipped this cycle.\n",
                                    slot, (unsigned long long)entry_t,
                                    (unsigned long long)now_tick);
                                state.cores[slot].last_entry_tick = now_tick;
                                continue;
                            }
                            uint64_t elapsed = now_tick - entry_t;
                            if (elapsed < cfg.max_hold_ticks) continue;
                            // gross % gain since entry
                            double entry_d = FPN_ToDouble(state.oms->portfolio.positions[slot].entry_price);
                            if (entry_d <= 0.0) continue;
                            double gain_pct = (current_price_d - entry_d) / entry_d;
                            double min_gain = FPN_ToDouble(cfg.min_hold_gain_pct);
                            if (gain_pct >= min_gain) continue;  // still profitable enough; keep it
                            // Submit force-close. OMS HandleFill will close the slot
                            // and book net P&L exactly like a normal SG-triggered exit.
                            FPN<F> qty = state.oms->portfolio.positions[slot].quantity;
                            FPN<F> price_fpn = FPN_FromDouble<F>(current_price_d);
                            tt::OrderManager_Submit(state.oms,
                                (int16_t)slot, ORDER_MARKET_SELL,
                                qty, FPN_Zero<F>(), FPN_Zero<F>(),
                                state.cores[slot].strategy_id, price_fpn);
                            fprintf(stderr,
                                "[sharded] core %d: time-exit (held %lu ticks, gain %.3f%%)\n",
                                slot, (unsigned long)elapsed, gain_pct * 100.0);
                        }
                    }
                }

                // v4.0.3 D9: Trailing SL ratchet. For each active position,
                // if gross gain >= cfg.tp_hold_score, compute trailing target
                // = current_price - (stddev × sl_trail_mult). Write to
                // pending_params.ratchet_sl which the hot path picks up via
                // the existing seqlock on next param push. Only ratchets UP
                // (FPN_Max in hot path means lower ratchet values are ignored).
                if (!FPN_IsZero(cfg.sl_trail_mult) &&
                    !FPN_IsZero(rolling_short.price_stddev) &&
                    !FPN_IsZero(cfg.tp_hold_score)) {
                    double cur_d = last_price.load(std::memory_order_relaxed);
                    if (cur_d > 0.01) {
                        double stddev_d = FPN_ToDouble(rolling_short.price_stddev);
                        double trail_dist_d = stddev_d * FPN_ToDouble(cfg.sl_trail_mult);
                        double hold_thresh = FPN_ToDouble(cfg.tp_hold_score);
                        uint16_t bm = state.oms->portfolio.active_bitmap;
                        while (bm) {
                            int slot = __builtin_ctz(bm);
                            bm &= (uint16_t)(bm - 1);
                            double entry_d = FPN_ToDouble(state.oms->portfolio.positions[slot].entry_price);
                            if (entry_d <= 0.0) continue;
                            double gain_pct = (cur_d - entry_d) / entry_d;
                            if (gain_pct < hold_thresh) continue;  // not yet trailing
                            double new_sl_d = cur_d - trail_dist_d;
                            // Write new ratchet into pending_params; hot path
                            // FPN_Max ensures we never lower an existing ratchet.
                            FPN<F> new_ratchet = FPN_FromDouble<F>(new_sl_d);
                            FPN<F> existing = state.cores[slot].pending_params.ratchet_sl;
                            if (FPN_GreaterThan(new_ratchet, existing)) {
                                state.cores[slot].pending_params.ratchet_sl = new_ratchet;
                                state.cores[slot].dirty = 1;  // force push
                            }
                        }
                    }
                }

#ifdef USE_IMGUI_GUI
                // Populate TUISnapshot for the GUI — same double-buffered
                // pattern as legacy engine in main.cpp:845-912.
                {
                    int back = !__atomic_load_n(&g_shared.active_idx, __ATOMIC_ACQUIRE);
                    int front = !back;
                    TUISnapshot *bs = &g_shared.snapshots[back];
                    const TUISnapshot *fs = &g_shared.snapshots[front];
                    // carry graph history ring buffers from front buffer
                    memcpy(bs->price_history, fs->price_history, sizeof(bs->price_history));
                    memcpy(bs->volume_history, fs->volume_history, sizeof(bs->volume_history));
                    memcpy(bs->pnl_history, fs->pnl_history, sizeof(bs->pnl_history));
                    bs->graph_head = fs->graph_head;
                    bs->graph_count = fs->graph_count;
                    // populate from sharded state
                    TUI_CopySnapshotSharded(bs, &state, &rolling_short, &rolling_long,
                                             &cfg, price_d, volume_d);
                    TUI_PopulatePerCoreLatency(bs, cores, num_cores, tsc_ghz);
                    // append current data point to graph ring buffers
                    bs->price_history[bs->graph_head] = bs->price;
                    bs->volume_history[bs->graph_head] = bs->volume;
                    bs->pnl_history[bs->graph_head] = bs->total_pnl;
                    bs->graph_head = (bs->graph_head + 1) % TUISnapshot::GRAPH_LEN;
                    if (bs->graph_count < TUISnapshot::GRAPH_LEN) bs->graph_count++;
                    __atomic_store_n(&g_shared.active_idx, back, __ATOMIC_RELEASE);
                }
                // check GUI quit request
                if (g_shared.quit_requested) {
                    g_engine_sharded_shutdown = 1;
                }
                // paper reset: zero balance, clear positions, reset counters
                if (g_shared.paper_reset_requested && !cfg.use_real_money) {
                    g_shared.paper_reset_requested = 0;
                    state.oms->balance      = cfg.starting_balance;
                    state.oms->realized_pnl = FPN_Zero<F>();
                    state.oms->ks_peak_balance = cfg.starting_balance;
                    state.oms->kill_switch_tripped = 0;
                    Portfolio_Init(&state.oms->portfolio);
                    state.total_entries = 0;
                    state.total_exits   = 0;
                    state.total_events_processed = 0;
                    for (int c = 0; c < num_cores; ++c) {
                        state.cores[c].entries_processed = 0;
                        state.cores[c].exits_processed   = 0;
                        // Phase 2.1: reset per-core counters too. Without
                        // this, a paper reset would leak realized P&L /
                        // budget state from the previous "session" into
                        // the new one — wrong reading on every panel.
                        state.cores[c].core_realized      = FPN_Zero<F>();
                        state.cores[c].core_fees          = FPN_Zero<F>();
                        state.cores[c].core_wins          = 0;
                        state.cores[c].core_losses        = 0;
                        state.cores[c].core_open_notional = FPN_Zero<F>();
                        // Phase 3: reset per-core kill switch state too.
                        // Trip flags + peak + dd all clear so cores can
                        // trade fresh after a paper reset.
                        state.cores[c].core_peak_balance   = FPN_Zero<F>();
                        state.cores[c].core_dd_pct         = FPN_Zero<F>();
                        state.cores[c].core_kill_tripped   = 0;
                        state.cores[c].core_ks_trips_total = 0;
                    }
                    fprintf(stderr, "[sharded] paper reset: balance=$%.2f\n",
                            FPN_ToDouble(cfg.starting_balance));
                }
#endif
            }
            return true;
        };

        if (use_synthetic) {
            // SYNTHETIC FALLBACK — sawtooth around $60100 (straddles a sample
            // BG threshold and TP/SL bands). Used when BinanceStream_Init
            // failed (e.g. running offline / no network).
            while (!g_engine_sharded_shutdown) {
                double phase = (double)(seq % 200);
                double price = 60050.0 + (phase < 100.0 ? phase : (200.0 - phase));
                // small random volume variation for realistic candle rendering
                double vol = 1.5 + (double)((seq * 7 + 13) % 100) / 100.0;
                if (!fan_out(price, vol, (uint64_t)(seq * 1000))) break;
                // throttle to ~3 ticks/sec to match real BTC feed cadence.
                // without this the chart gets one massive candle and the
                // GUI oscillates wildly from millions of ticks/sec.
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
        } else {
            // REAL BINANCE FEED — same poll/drain pattern as the legacy
            // engine main loop, but instead of calling
            // PortfolioController_Tick we fan each tick out to N execution
            // core tick rings.
            while (!g_engine_sharded_shutdown) {
                int ready = BinanceStream_Poll(&bs, bcfg.poll_timeout_ms);
                if (g_engine_sharded_shutdown) break;
                if (!(ready & POLL_SOCKET)) continue;
                while (!g_engine_sharded_shutdown) {
                    DataStream<F> ds;
                    int ok = BinanceStream_ReadTick(&bs, &ds);
                    if (!ok) {
                        fprintf(stderr, "[sharded] Binance disconnect — reconnecting\n");
                        BinanceStream_Reconnect(&bs, &bcfg);
                        break;
                    }
                    // Convert to Tick<F>. ds.timestamp_ms is already milliseconds
                    // since epoch from Binance; expand to micros for the per-core
                    // Tick.timestamp field.
                    if (!fan_out(ds.price_d, ds.volume_d,
                                  (uint64_t)time(NULL) * 1000000ULL,
                                  ds.is_buyer_maker)) {
                        break;
                    }
                    if (!BinanceStream_HasPending(&bs)) break;
                }
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    //----------------------------------------------------------------------
    // Executor threads — one per execution core
    //----------------------------------------------------------------------
    // Lambdas can't capture static arrays directly, so we pass the index
    // in by value and let the lambda body reference the file-scope statics.
    std::vector<std::thread> executors;
    executors.reserve(num_cores);
    for (int i = 0; i < num_cores; ++i) {
        executors.emplace_back([i, &producer_done, &ticks_consumed_total] {
            EngineSharded_PinThread(i + 1);  // CPU 1..N (CPU 0 = producer)
            Tick<F> t;
            uint64_t local_consumed = 0;
            while (!g_engine_sharded_shutdown) {
                if (SPSCRing_TryPop(&tick_rings[i], &t)) {
                    ExecutionCore_Tick(&cores[i], t);
                    local_consumed++;
                }
                if (producer_done.load(std::memory_order_acquire) &&
                    SPSCRing_Depth(&tick_rings[i]) == 0) {
                    break;
                }
            }
            ticks_consumed_total.fetch_add(local_consumed, std::memory_order_relaxed);
        });
    }

    //----------------------------------------------------------------------
    // Drainer thread — controller side
    //----------------------------------------------------------------------
    // open-coded drain so we can hook order submission AFTER each event.
    // identical structure to EventLoop_DrainEvents but with an OrderManager_Submit
    // call inserted between OnEvent and the loop continue, then a single
    // OrderManager_Tick at the end of each pass. paper mode passes through
    // the OMS as a no-op (OrderManager_Tick marks orders FILLED immediately
    // without touching the api); live mode does the synchronous REST call
    // inside OrderManager_Tick and brackets it with steady_clock for the
    // order latency stats. phase 02 will move the REST call to a worker
    // thread so the drainer no longer blocks.
    //
    // why open-code instead of adding a hook to ControllerEventLoop.hpp:
    // keeps the generic event loop free of REST dependencies (it stays
    // testable in isolation), and the live trading concern is contained
    // in this file where it belongs. the OMS itself is generic — the
    // file-specific concern is wiring it into the per-tick drain pattern.
    //
    // qty source rule:
    //   entry: read from cores[slot].intended_qty (matches what OnEvent will
    //          write into portfolio.positions[slot].quantity via OpenSlot)
    //   exit:  read from portfolio.positions[slot].quantity BEFORE OnEvent,
    //          since CloseSlot inside OnEvent clears the slot
    // Extracted drain+Submit helper. Called from both the main drainer loop
    // and the trailing shutdown drain. One site to update when adding new
    // Submit parameters or event types.
    auto drain_with_submit = [&state, &oms, &ticks_produced, &cfg]() -> int {
        int total_drained = 0;
        for (int slot = 0; slot < state.registered_count; ++slot) {
            ExecutionCore<F>* core = state.cores[slot].core;
            if (core == nullptr) continue;
            for (int i = 0; i < MAX_EVENTS_PER_DRAIN_PER_CORE; ++i) {
                TradeEvent<F> event;
                if (!SPSCRing_TryPop(&core->event_ring, &event)) break;

                bool is_entry = (event.type & TRADE_EVENT_ENTRY) != 0;
                bool is_exit  = (event.type & TRADE_EVENT_EXIT)  != 0;

                // P.3: map (core_id, leg) → portfolio slot. When
                // partial_exit_enabled=0, slot == core_id (1:1 mapping
                // preserves pre-P.3 behavior). When enabled, slot = 2*c+leg.
                int partial_on = cfg.partial_exit_enabled ? 1 : 0;
                int portfolio_slot = Sharded_LegSlot(slot, (int)event.leg, partial_on);
                if (portfolio_slot < 0) {
                    // Defensive: malformed event (e.g. leg=1 without partials
                    // enabled). Drop + log; don't crash the drainer.
                    fprintf(stderr,
                        "[sharded] drainer: invalid (core=%d, leg=%u) for "
                        "partial_enabled=%d → dropping event\n",
                        slot, (unsigned)event.leg, partial_on);
                    continue;
                }

                // snapshot exit qty BEFORE OnEvent because CloseSlot clears it
                double order_qty_d = 0.0;
                if (is_exit) {
                    // Exit qty: read from the LEG's portfolio slot (leg A's
                    // qty for leg A exit, leg B's for leg B exit). With
                    // partials disabled, portfolio_slot == slot == core_id
                    // and behavior is identical to pre-P.3.
                    order_qty_d = FPN_ToDouble(
                        state.oms->portfolio.positions[portfolio_slot].quantity);
                } else if (is_entry) {
                    // Entry qty: split intended_qty between legs by
                    // partial_exit_pct. Leg A gets partial_pct, leg B gets
                    // (1 - partial_pct). When partials disabled, leg is
                    // always 0 and we use the full intended_qty (no split).
                    double full_qty = FPN_ToDouble(state.cores[slot].intended_qty);
                    if (partial_on && event.leg == PARTIAL_LEG_A) {
                        order_qty_d = full_qty * FPN_ToDouble(cfg.partial_exit_pct);
                    } else if (partial_on && event.leg == PARTIAL_LEG_B) {
                        order_qty_d = full_qty * (1.0 - FPN_ToDouble(cfg.partial_exit_pct));
                    } else {
                        order_qty_d = full_qty;
                    }
                }

                EventLoop_OnEvent(&state, event);
                ++total_drained;

                if ((is_entry || is_exit) && order_qty_d > 0.0) {
                    OrderManager_Submit(&oms,
                        (int16_t)portfolio_slot,  // P.3: actual slot, not core_id
                        is_entry ? ORDER_MARKET_BUY : ORDER_MARKET_SELL,
                        FPN_FromDouble<F>(order_qty_d),
                        state.cores[slot].intended_tp,
                        state.cores[slot].intended_sl,
                        state.cores[slot].strategy_id,
                        event.price,
                        event.leg);  // P.3: leg propagated to Order
                    // Phase 6prep sharded c13: snapshot the staged prediction
                    // into active_prediction at entry submit. Persists across
                    // the entry→exit window so the IC update at exit pairs the
                    // realized return with the prediction that actually
                    // triggered the trade — not the latest rebuild's value.
                    // Only on leg A entry — leg B is part of the same trade,
                    // shouldn't double-stamp the prediction.
                    if (is_entry && event.leg == PARTIAL_LEG_A &&
                        state.cores[slot].strategy_id == STRATEGY_ML) {
                        state.cores[slot].active_prediction =
                            state.cores[slot].staged_prediction;
                    }
                    // v4.0.3 spacing + time-based exit: stamp this entry
                    // for cross-cutting checks on the next rebuild. Only
                    // on leg A entry (one trade = one entry stamp).
                    if (is_entry && event.leg == PARTIAL_LEG_A) {
                        state.cores[slot].last_entry_price = event.price;
                        state.cores[slot].last_entry_tick  = ticks_produced.load(std::memory_order_relaxed);
                    }
                }
            }
        }
        return total_drained;
    };

    // Phase 6prep sharded c14: feed the ConfidenceScorer with realized returns
    // from each just-closed position. Bitmap is cleared after drain so the
    // next Tick starts fresh. ML cores only — non-ML cores ignored even if
    // the bit is set (defensive: shouldn't happen since only ML strategy
    // emits trades that produce predictions, but cheap to guard).
    // Mode 1 per-fill drainer pass — implementation in
    // ControllerEventLoop.hpp::EventLoop_DrainPostFill. Same producer-
    // consumer contract: drainer thread (here) consumes what
    // OrderManager_Tick produces. Extracted to a standalone function so
    // it's directly unit-testable without standing up a producer thread.
    auto drain_post_fill = [&state, &oms, &cfg]() {
        EventLoop_DrainPostFill(&state, &oms, cfg.sl_cooldown_cycles);
    };

    std::thread drainer([&state, &oms, &producer_done, &drain_with_submit, &drain_post_fill] {
        EngineSharded_PinThread(state.registered_count + 1);
        while (!g_engine_sharded_shutdown) {
            int total_drained = drain_with_submit();
            OrderManager_Tick(&oms);
            drain_post_fill();

            if (total_drained == 0) std::this_thread::yield();
            if (producer_done.load(std::memory_order_acquire)) {
                for (int k = 0; k < 16; ++k) {
                    drain_with_submit();
                    OrderManager_Tick(&oms);
                    drain_post_fill();
                }
                break;
            }
        }
    });

#ifdef USE_IMGUI_GUI
    // GUI mode: the GUI thread handles rendering. Just wait for shutdown.
    while (!g_engine_sharded_shutdown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (g_shared.quit_requested) g_engine_sharded_shutdown = 1;
    }
#else
    // Live TUI render loop. Refreshes ~5x/sec on stdout with the per-core
    // latency table front and center. Uses the same warm color palette as
    // the legacy TUI for visual consistency. Hides the cursor while running
    // and clears the screen on each refresh; restores both on exit.
    //
    // ANSI escape codes used:
    //   \033[?25l   hide cursor
    //   \033[?25h   show cursor
    //   \033[2J     clear screen
    //   \033[H      cursor home (1;1)
    //   \033[K      clear to end of line
    //
    // Color tokens borrowed from DataStream/TUIAnsi.hpp.
    #define SH_RESET   "\033[0m"
    #define SH_BOLD    "\033[1m"
    #define SH_WHEAT   "\033[38;2;220;198;150m"
    #define SH_SAND    "\033[38;2;190;170;140m"
    #define SH_PEACH   "\033[38;2;230;165;120m"
    #define SH_FG      "\033[38;2;200;190;170m"
    #define SH_DIM     "\033[38;2;120;115;105m"
    #define SH_GREEN   "\033[38;2;140;195;130m"
    #define SH_RED     "\033[38;2;210;120;120m"
    #define SH_PNL(v)  ((v) >= 0.0 ? SH_GREEN : SH_RED)

    fprintf(stdout, "\033[?25l");  // hide cursor
    fflush(stdout);

    auto t_start = std::chrono::steady_clock::now();
    while (!g_engine_sharded_shutdown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Header
        fprintf(stdout, "\033[H");  // cursor home, no clear (avoids flicker)
        fprintf(stdout, SH_BOLD SH_PEACH "  /l、" SH_RESET "  " SH_BOLD SH_PEACH "FOXML TRADER" SH_RESET "  " SH_DIM "(per-core sharded)" SH_RESET "\033[K\n");
        fprintf(stdout, SH_DIM " ( °_ ° 7" SH_RESET "  " SH_DIM "engine v3.7.2" SH_RESET "  " SH_FG "%s" SH_RESET "\033[K\n",
                use_synthetic ? "synthetic ticks" : "real Binance feed");
        fprintf(stdout, SH_DIM "  ド  ヘ" SH_RESET "\033[K\n");
        fprintf(stdout, SH_DIM " じし_,)ノ" SH_RESET "\033[K\n");
        fprintf(stdout, "\033[K\n");

        // Top bar
        auto now = std::chrono::steady_clock::now();
        long uptime = std::chrono::duration_cast<std::chrono::seconds>(now - t_start).count();
        double bal = FPN_ToDouble(state.oms->balance);
        double pnl = FPN_ToDouble(state.oms->realized_pnl);
        int active = __builtin_popcount(state.oms->portfolio.active_bitmap);
        fprintf(stdout, " " SH_DIM "STATE: " SH_RESET SH_FG "ACTIVE" SH_RESET
                "  " SH_DIM "│" SH_RESET "  " SH_DIM "UPTIME: " SH_RESET SH_FG "%02ld:%02ld:%02ld" SH_RESET
                "  " SH_DIM "│" SH_RESET "  " SH_DIM "MODE: " SH_RESET SH_PEACH "SHARDED" SH_RESET "\033[K\n",
                uptime / 3600, (uptime / 60) % 60, uptime % 60);
        fprintf(stdout, "\033[K\n");

        // Market + account
        double price_d = last_price.load(std::memory_order_relaxed);
        double vol_d = last_volume.load(std::memory_order_relaxed);
        // compute equity = balance + unrealized P&L across all open positions
        double unrealized = 0.0;
        {
            uint16_t bm = state.oms->portfolio.active_bitmap;
            while (bm) {
                int s = __builtin_ctz(bm);
                bm &= (uint16_t)(bm - 1);
                double entry = FPN_ToDouble(state.oms->portfolio.positions[s].entry_price);
                double qty   = FPN_ToDouble(state.oms->portfolio.positions[s].quantity);
                unrealized += (price_d - entry) * qty;
            }
        }
        double equity = bal + unrealized;
        fprintf(stdout, " " SH_DIM " PRICE " SH_RESET SH_BOLD SH_WHEAT "$%.2f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "VOL " SH_RESET SH_FG "%.4f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "BAL " SH_RESET SH_FG "$%.2f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "EQUITY " SH_RESET SH_BOLD "%s$%.2f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "POS " SH_RESET SH_FG "%d/%u" SH_RESET "\033[K\n",
                price_d, vol_d, bal, SH_PNL(equity - 10000.0), equity, active, (unsigned)num_cores);
        fprintf(stdout, " " SH_DIM " P&L " SH_RESET SH_BOLD "%s$%+.4f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "UNREAL " SH_RESET SH_BOLD "%s$%+.4f" SH_RESET "\033[K\n",
                SH_PNL(pnl), pnl, SH_PNL(unrealized), unrealized);
        fprintf(stdout, "\033[K\n");

        // Counters
        fprintf(stdout, " " SH_DIM " produced  " SH_RESET SH_FG "%lu" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "consumed  " SH_RESET SH_FG "%lu" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "entries  " SH_RESET SH_FG "%lu" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "exits  " SH_RESET SH_FG "%lu" SH_RESET "\033[K\n",
                (unsigned long)ticks_produced.load(),
                (unsigned long)ticks_consumed_total.load(),
                (unsigned long)state.total_entries,
                (unsigned long)state.total_exits);
        fprintf(stdout, "\033[K\n");

        // Per-core latency table — the headline
        fprintf(stdout, SH_BOLD SH_PEACH " PER-CORE LATENCY" SH_RESET SH_DIM "  (last 256 samples per core)" SH_RESET "\033[K\n");
        fprintf(stdout, "  " SH_DIM "core   samples       min        p50        p95        p99        max        avg" SH_RESET "\033[K\n");
        for (int i = 0; i < num_cores; ++i) {
            CoreLatencySnapshot ls = CoreLatencyStats_Snapshot(&cores[i].latency_stats, tsc_ghz);
            if (ls.total_count == 0) {
                fprintf(stdout, "  " SH_FG " %2d   " SH_DIM "%8s   %6s ns   %6s ns   %6s ns   %6s ns   %6s ns   %6s ns" SH_RESET "\033[K\n",
                        i, "0", "-", "-", "-", "-", "-", "-");
            } else {
                fprintf(stdout, "  " SH_FG " %2d   " SH_DIM "%8lu   " SH_FG "%6.0f ns   %6.0f ns   %6.0f ns   %6.0f ns   %6.0f ns   %6.0f ns" SH_RESET "\033[K\n",
                        i, (unsigned long)ls.total_count,
                        ls.min_ns, ls.p50_ns, ls.p95_ns, ls.p99_ns, ls.max_ns, ls.avg_ns);
            }
        }

        // Per-core position details — shows which core has a position,
        // entry price, quantity, unrealized P&L, and TP/SL levels.
        // only renders rows for active positions to keep the display compact.
        if (active > 0) {
            fprintf(stdout, "\033[K\n");
            fprintf(stdout, SH_BOLD SH_PEACH " POSITIONS" SH_RESET "\033[K\n");
            fprintf(stdout, "  " SH_DIM "core   strategy     entry          qty       unreal         TP             SL" SH_RESET "\033[K\n");
            uint16_t bm = state.oms->portfolio.active_bitmap;
            while (bm) {
                int s = __builtin_ctz(bm);
                bm &= (uint16_t)(bm - 1);
                double entry_d = FPN_ToDouble(state.oms->portfolio.positions[s].entry_price);
                double qty_d   = FPN_ToDouble(state.oms->portfolio.positions[s].quantity);
                double tp_d    = FPN_ToDouble(state.oms->portfolio.positions[s].take_profit_price);
                double sl_d    = FPN_ToDouble(state.oms->portfolio.positions[s].stop_loss_price);
                double unreal_d = (price_d - entry_d) * qty_d;
                const char* strat = (s < state.registered_count && state.cores[s].strategy_id < NUM_STRATEGIES)
                    ? STRATEGY_SHORT_NAMES[state.cores[s].strategy_id] : "?";
                fprintf(stdout, "  " SH_FG " %2d    " SH_PEACH "%-5s" SH_RESET
                        "  " SH_FG "$%10.2f   %10.6f   " SH_BOLD "%s$%+8.4f" SH_RESET
                        "   " SH_GREEN "$%.2f" SH_RESET "   " SH_RED "$%.2f" SH_RESET "\033[K\n",
                        s, strat, entry_d, qty_d,
                        SH_PNL(unreal_d), unreal_d,
                        tp_d, sl_d);
            }
        }
        fprintf(stdout, "\033[K\n");

        // Order latency line — only meaningful in live mode, but always shown
        // so paper users see the placeholder. all values in microseconds.
        uint64_t ord_count    = g_sharded_order_lat.count.load(std::memory_order_relaxed);
        uint64_t ord_failures = g_sharded_order_lat.failures.load(std::memory_order_relaxed);
        uint64_t ord_total_us = g_sharded_order_lat.total_us.load(std::memory_order_relaxed);
        uint64_t ord_min_us   = g_sharded_order_lat.min_us.load(std::memory_order_relaxed);
        uint64_t ord_max_us   = g_sharded_order_lat.max_us.load(std::memory_order_relaxed);
        fprintf(stdout, "\033[K\n");
        if (live_trading) {
            if (ord_count > 0) {
                double ord_avg_us = (double)ord_total_us / (double)ord_count;
                fprintf(stdout, " " SH_BOLD SH_PEACH "ORDER LATENCY" SH_RESET SH_DIM " (live)" SH_RESET
                        "  " SH_DIM "orders" SH_RESET " " SH_FG "%lu" SH_RESET
                        "  " SH_DIM "fail" SH_RESET " " SH_FG "%lu" SH_RESET
                        "  " SH_DIM "min" SH_RESET " " SH_FG "%lu µs" SH_RESET
                        "  " SH_DIM "avg" SH_RESET " " SH_FG "%.0f µs" SH_RESET
                        "  " SH_DIM "max" SH_RESET " " SH_FG "%lu µs" SH_RESET "\033[K\n",
                        (unsigned long)ord_count, (unsigned long)ord_failures,
                        (unsigned long)ord_min_us, ord_avg_us, (unsigned long)ord_max_us);
            } else {
                fprintf(stdout, " " SH_BOLD SH_PEACH "ORDER LATENCY" SH_RESET SH_DIM " (live, no orders yet)" SH_RESET "\033[K\n");
            }
        } else {
            fprintf(stdout, " " SH_DIM "ORDER LATENCY: paper mode (no orders submitted)" SH_RESET "\033[K\n");
        }

        // OMS counter line. shows the OrderManager-side view: submissions
        // it received from the drainer, fills it observed (paper mode bumps
        // both immediately, live mode bumps filled when the adapter callback
        // comes back), rejections, and how many orders are still in flight
        // in the OMS table. orthogonal to the per-core latency above.
        uint64_t oms_sub      = OrderManager_TotalSubmitted(&oms);
        uint64_t oms_fill     = OrderManager_TotalFilled(&oms);
        uint64_t oms_rej      = OrderManager_TotalRejected(&oms);
        int      oms_inflight = OrderManager_InflightCount(&oms);
        fprintf(stdout, " " SH_BOLD SH_PEACH "OMS" SH_RESET
                "          " SH_DIM "submitted" SH_RESET " " SH_FG "%lu" SH_RESET
                "  " SH_DIM "filled" SH_RESET " " SH_FG "%lu" SH_RESET
                "  " SH_DIM "rejected" SH_RESET " " SH_FG "%lu" SH_RESET
                "  " SH_DIM "inflight" SH_RESET " " SH_FG "%d" SH_RESET "\033[K\n",
                (unsigned long)oms_sub, (unsigned long)oms_fill,
                (unsigned long)oms_rej, oms_inflight);

        // User data WS status line (phase 04)
        if (live_trading) {
            int ws_conn = g_user_data.ws_connected.load(std::memory_order_relaxed);
            uint64_t ws_fills = g_user_data.fills_received.load(std::memory_order_relaxed);
            uint64_t ws_events = g_user_data.events_received.load(std::memory_order_relaxed);
            fprintf(stdout, " " SH_BOLD SH_PEACH "WS FILLS" SH_RESET
                    "     " SH_DIM "status" SH_RESET " %s"
                    "  " SH_DIM "fills" SH_RESET " " SH_FG "%lu" SH_RESET
                    "  " SH_DIM "events" SH_RESET " " SH_FG "%lu" SH_RESET "\033[K\n",
                    ws_conn ? (SH_GREEN "CONNECTED" SH_RESET)
                            : (SH_RED "DISCONNECTED" SH_RESET),
                    (unsigned long)ws_fills, (unsigned long)ws_events);
            // Reconciler status
            uint64_t rc_polls = g_reconciler.total_polls.load(std::memory_order_relaxed);
            uint64_t rc_corr  = g_reconciler.drift_corrections.load(std::memory_order_relaxed);
            double   rc_drift = g_reconciler.last_drift_usdt.load(std::memory_order_relaxed);
            fprintf(stdout, " " SH_BOLD SH_PEACH "RECONCILE" SH_RESET
                    "    " SH_DIM "polls" SH_RESET " " SH_FG "%lu" SH_RESET
                    "  " SH_DIM "corrections" SH_RESET " " SH_FG "%lu" SH_RESET
                    "  " SH_DIM "drift" SH_RESET " %s$%.4f" SH_RESET "\033[K\n",
                    (unsigned long)rc_polls, (unsigned long)rc_corr,
                    SH_PNL(rc_drift), rc_drift);
        }

        // Footer
        fprintf(stdout, "\033[K\n");
        fprintf(stdout, " " SH_DIM "Press Ctrl+C to stop and dump final stats. Subtract ~25-30ns rdtsc floor for actual work cost." SH_RESET "\033[K\n");
        // Clear any trailing rows from a previous render with more cores
        for (int i = 0; i < 4; ++i) fprintf(stdout, "\033[K\n");
        fflush(stdout);
    }

    fprintf(stdout, "\033[?25h");  // show cursor
    fprintf(stdout, "\n");
    fflush(stdout);

    #undef SH_RESET
    #undef SH_BOLD
    #undef SH_WHEAT
    #undef SH_SAND
    #undef SH_PEACH
    #undef SH_FG
    #undef SH_DIM
    #undef SH_GREEN
    #undef SH_RED
    #undef SH_PNL
#endif  // USE_IMGUI_GUI else (ANSI TUI)

    fprintf(stderr, "[sharded] shutdown requested, joining threads...\n");

    // Phase 4 — final snapshot save BEFORE force-close. Captures the
    // pre-close state so a restart can resume regime hysteresis +
    // pnl_feeder + kill switch peak. Force-close mutates portfolio +
    // realized_pnl; we save first so the persisted state matches the
    // engine's "intent" rather than an in-progress liquidation.
    // Paper mode only — live mode treats exchange state as truth.
    if (!live_trading) {
        if (ShardedSnapshot_Save<F>(&state, "data/sharded_snapshot.dat",
                                      cfg.partial_exit_enabled ? 1 : 0)) {
            fprintf(stderr, "[snapshot] final save: data/sharded_snapshot.dat\n");
        } else {
            fprintf(stderr, "[snapshot] final save FAILED — next restart starts fresh\n");
        }
    }

    // Phase 0.2 — force-close on shutdown. Refuse to silently exit with
    // open positions. Submits market sells via OrderManager_Submit (same
    // path as TP/SL exits) and waits up to 30s for the bitmap to clear as
    // fills come through user-data WS → drainer → OnEvent. Drainer is
    // still running at this point — the joins below come after.
    // Paper mode: clears bitmap locally (no exchange interaction).
    {
        int remaining = EngineSharded_ForceCloseOnShutdown<F>(
            &oms, live_trading ? &g_sharded_binance_adapter : nullptr,
            g_notify, /*timeout_secs=*/30);
        if (remaining > 0) {
            fprintf(stderr,
                "[sharded] WARNING: %d position(s) could not be force-closed before exit. "
                "Manual intervention required to flatten on Binance before next session.\n",
                remaining);
        }
    }
    // Per-stage shutdown logging — when the process refuses to exit on close,
    // these tell us WHICH thread is hung. Without them every shutdown bug
    // looks the same from outside ("the engine doesn't die"). Each stage
    // also signals its dedicated quit flag BEFORE joining so the thread has
    // a chance to see it on its next loop iteration. v4.0.1 added per-stage
    // signals after observing producer+depth could be in mid-reconnect with
    // their dedicated flags not yet set.
    fprintf(stderr, "[sharded]   joining depth thread...\n");
    if (g_depth_tid != 0) {
        __atomic_store_n(&g_depth_shared.quit_requested, 1, __ATOMIC_RELEASE);
        pthread_join(g_depth_tid, NULL);
    }
    fprintf(stderr, "[sharded]   joining producer...\n");
    producer.join();
    fprintf(stderr, "[sharded]   joining executors (%d)...\n", num_cores);
    for (auto& e : executors) e.join();
    fprintf(stderr, "[sharded]   joining drainer...\n");
    drainer.join();
#ifdef USE_IMGUI_GUI
    fprintf(stderr, "[sharded]   joining GUI...\n");
    g_shared.quit_requested = 1;
    pthread_join(gui_tid, NULL);
#endif
    fprintf(stderr, "[sharded]   closing recorders...\n");
    DepthRecorder_Close(&g_depth_rec);
    TickRecorder_Close(&g_tick_rec);  // Phase 8a (post-coding c7)
    fprintf(stderr, "[sharded]   shutting down notify worker...\n");
    if (g_notify) {
        NotifyState_Shutdown(g_notify);
        g_notify = nullptr;
    }
    fprintf(stderr, "[sharded]   shutting down OMS...\n");
    OrderManager_Shutdown(&oms);

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

    // Restore previous signal handlers so subsequent code paths see the
    // original behavior (legacy engine doesn't install one, so this resets
    // to SIG_DFL).
    std::signal(SIGINT,  prev_int);
    std::signal(SIGTERM, prev_term);
}

}  // namespace tt
