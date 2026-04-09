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
#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../Strategies/StrategyParameters.hpp"
#include "BinanceAdapter.hpp"
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

extern "C" inline void EngineSharded_SignalHandler(int sig) {
    (void)sig;
    g_engine_sharded_shutdown = 1;
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
static inline void EngineSharded_Run(const ControllerConfig<F>& cfg,
                                      const BinanceConfig& bcfg) {
    // Install our own SIGINT/SIGTERM handler so threads can shut down cleanly.
    // Save the previous handlers so we can restore them on exit (in case the
    // legacy engine path runs after us in some test setup).
    g_engine_sharded_shutdown = 0;
    auto prev_int  = std::signal(SIGINT,  EngineSharded_SignalHandler);
    auto prev_term = std::signal(SIGTERM, EngineSharded_SignalHandler);

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

    EventLoopState<F> state;
    EventLoopState_Init(&state, cfg.starting_balance, cfg.fee_rate);

    // OMS phase 02: route live orders through OrderManager which submits
    // them async via the BinanceAdapter. paper mode short-circuits in
    // OrderManager_Submit and never touches the adapter. live mode passes
    // a wired adapter constructed via BinanceAdapter_Get<F>. Drainer no
    // longer blocks on REST — adapter worker thread handles the round trip
    // and pushes a CMD_FILL_RESULT into the OMS result queue, which the
    // drainer drains via OrderManager_Tick on each pass.
    ExchangeAdapter<F> exchange_adapter{};  // value-init: all pointers null (paper mode default)
    if (live_trading) {
        exchange_adapter = BinanceAdapter_Get<F>(&g_sharded_binance_adapter);
    }
    OrderManagerState<F> oms;
    OrderManager_Init(&oms, exchange_adapter, live_trading ? 1 : 0);

    // Per-core resources. Static so they live in BSS, not the stack —
    // ExecutionCore is ~66KB and num_cores * size could blow the stack.
    static SPSCRing<Tick<F>, EXECUTION_CORE_TICK_RING_SIZE> tick_rings[MAX_EXECUTION_CORES];
    static ExecutionCore<F> cores[MAX_EXECUTION_CORES];
    // Rolling stats for the slow-path strategy rebuild. Producer thread
    // pushes ticks into these on cadence and rebuilds gate params from them.
    static RollingStats<F, 128> rolling_short = RollingStats_Init<F, 128>();
    static RollingStats<F, 512> rolling_long  = RollingStats_Init<F, 512>();
    rolling_short = RollingStats_Init<F, 128>();
    rolling_long  = RollingStats_Init<F, 512>();

    // Per-core risk allocation: split the configured starting balance across
    // cores. Each core gets its own mini-portfolio that's risk_pct of the
    // total. With risk_pct=10% and 4 cores starting at $10k, each core gets
    // $250 to risk on a single trade.
    double total_balance = FPN_ToDouble(cfg.starting_balance);
    double risk_fraction = FPN_ToDouble(cfg.risk_pct);
    if (risk_fraction <= 0.0) risk_fraction = 0.10;
    double per_core_balance = (total_balance * risk_fraction) / (double)num_cores;
    if (per_core_balance < 1.0) per_core_balance = 1.0;

    for (int i = 0; i < num_cores; ++i) {
        SPSCRing_Init(&tick_rings[i]);
        ExecutionCore_Init(&cores[i], (uint16_t)i, &tick_rings[i]);
        EventLoopState_RegisterCore(&state, &cores[i],
            FPN_Zero<F>(),  // intended_tp will be set by slow-path rebuild
            FPN_Zero<F>(),  // intended_sl ditto
            FPN_Zero<F>()); // intended_qty ditto
        EventLoopState_SetCoreStrategy(&state, i,
            (uint8_t)STRATEGY_SIMPLE_DIP,
            FPN_FromDouble<F>(per_core_balance));

        // Cores start permission=0. The slow-path rebuild grants permission
        // once it has enough rolling-stats samples to compute meaningful
        // gate thresholds.
        ExecutionCore_SetPermission(&cores[i], 0);

        // Enable per-core latency sampling — the whole point of this mode
        CoreLatencyStats_Enable(&cores[i].latency_stats);
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
    // Producer thread — generates synthetic ticks and fans out to all cores
    //----------------------------------------------------------------------
    std::thread producer([&producer_done, &ticks_produced, &bcfg, &last_price, &last_volume, &cfg, &state, num_cores, use_synthetic] {
        EngineSharded_PinThread(0);  // best-effort pin to CPU 0
        uint64_t seq = 0;
        // Sharded mode uses a much shorter slow_path_interval than legacy
        // because the per-core path runs the slow path on the producer
        // thread (its own pinned CPU) and the cost is negligible. Tighter
        // cadence means rolling stats stay fresh on slow market feeds.
        int slow_path_interval = 8;
        int slow_path_counter = 0;

        // Helper that fans a single tick out to every core's tick ring,
        // updates rolling stats, and runs the slow-path rebuild on cadence.
        auto fan_out = [num_cores, &seq, &ticks_produced, &last_price, &last_volume,
                        &cfg, &state, &slow_path_counter, slow_path_interval]
                       (double price_d, double volume_d, uint64_t ts_us) {
            Tick<F> t;
            memset(&t, 0, sizeof(t));
            t.price = FPN_FromDouble<F>(price_d);
            t.volume = FPN_FromDouble<F>(volume_d);
            t.timestamp = ts_us;
            t.sequence = seq++;
            for (int c = 0; c < num_cores; ++c) {
                while (!SPSCRing_TryPush(&tick_rings[c], t)) {
                    if (g_engine_sharded_shutdown) return false;
                }
                if (g_engine_sharded_shutdown) return false;
            }
            ticks_produced.fetch_add(1, std::memory_order_relaxed);
            // Update last-seen price/volume for the TUI panel. Relaxed
            // because TUI is informational and races are tolerable.
            last_price.store(price_d, std::memory_order_relaxed);
            last_volume.store(volume_d, std::memory_order_relaxed);

            // Slow path: feed rolling stats and rebuild gate parameters
            // every poll_interval ticks. Matches the legacy controller's
            // sampling cadence so the per-core threshold computation gets
            // the same input distribution.
            slow_path_counter++;
            if (slow_path_counter >= slow_path_interval) {
                slow_path_counter = 0;
                RollingStats_Push(&rolling_short, t.price, t.volume);
                RollingStats_Push(&rolling_long,  t.price, t.volume);
                EventLoop_RebuildAllParameters(&state, &rolling_short, &cfg, &rolling_long);
                EventLoop_PushParameters(&state);
                EventLoop_KillSwitchEvaluate(&state);
                // Grant permission as soon as the rolling stats have any
                // data at all — the first sample gives us a meaningful
                // rolling max for the SimpleDip threshold. Each core is its
                // own mini-portfolio so there's no need for a long warmup.
                if (rolling_short.count >= 1) {
                    for (int c = 0; c < num_cores; ++c) {
                        if (state.cores[c].strategy_id != STRATEGY_NONE) {
                            ExecutionCore_SetPermission(&cores[c], 1);
                        }
                    }
                }
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
                if (!fan_out(price, 2.0, (uint64_t)(seq * 1000))) break;
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
                                  (uint64_t)time(NULL) * 1000000ULL)) {
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
    std::thread drainer([&state, &oms, &producer_done, num_cores] {
        EngineSharded_PinThread(num_cores + 1);  // dedicated controller CPU
        while (!g_engine_sharded_shutdown) {
            int total_drained = 0;
            for (int slot = 0; slot < state.registered_count; ++slot) {
                ExecutionCore<F>* core = state.cores[slot].core;
                if (core == nullptr) continue;
                for (int i = 0; i < MAX_EVENTS_PER_DRAIN_PER_CORE; ++i) {
                    TradeEvent<F> event;
                    if (!SPSCRing_TryPop(&core->event_ring, &event)) break;

                    bool is_entry = (event.type & TRADE_EVENT_ENTRY) != 0;
                    bool is_exit  = (event.type & TRADE_EVENT_EXIT)  != 0;

                    // snapshot exit qty BEFORE OnEvent because CloseSlot clears it
                    double order_qty_d = 0.0;
                    if (is_exit) {
                        order_qty_d = FPN_ToDouble(state.portfolio.positions[slot].quantity);
                    } else if (is_entry) {
                        order_qty_d = FPN_ToDouble(state.cores[slot].intended_qty);
                    }

                    EventLoop_OnEvent(&state, event);
                    ++total_drained;

                    // === ROUTE TO OMS ===
                    // every entry/exit becomes an OrderManager_Submit. paper mode
                    // marks the order FILLED on the next OMS Tick without ever
                    // touching the REST api; live mode runs the synchronous REST
                    // call inside OMS Tick. the qty is converted to FPN<F> per
                    // the FPN-only-accounting rule in CLAUDE.md.
                    if ((is_entry || is_exit) && order_qty_d > 0.0) {
                        OrderManager_Submit(&oms,
                            (int16_t)slot,
                            is_entry ? ORDER_MARKET_BUY : ORDER_MARKET_SELL,
                            FPN_FromDouble<F>(order_qty_d));
                    }
                }
            }
            // Process all PENDING orders the per-core drain just produced.
            // Phase 01: this BLOCKS for the duration of each REST call. With 4
            // cores firing simultaneously, worst case is ~800 ms drainer cycle.
            // Phase 02 moves the REST call to a worker thread so this returns
            // sub-µs again.
            OrderManager_Tick(&oms);

            if (total_drained == 0) std::this_thread::yield();
            if (producer_done.load(std::memory_order_acquire)) {
                // After producer is done, drain a few more times then exit.
                // Each pass also ticks the OMS so any final orders submitted
                // by the trailing exits get processed before shutdown.
                for (int k = 0; k < 16; ++k) {
                    EventLoop_DrainEvents(&state);
                    OrderManager_Tick(&oms);
                }
                break;
            }
        }
    });

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
        double bal = FPN_ToDouble(state.balance);
        double pnl = FPN_ToDouble(state.realized_pnl);
        int active = __builtin_popcount(state.portfolio.active_bitmap);
        fprintf(stdout, " " SH_DIM "STATE: " SH_RESET SH_FG "ACTIVE" SH_RESET
                "  " SH_DIM "│" SH_RESET "  " SH_DIM "UPTIME: " SH_RESET SH_FG "%02ld:%02ld:%02ld" SH_RESET
                "  " SH_DIM "│" SH_RESET "  " SH_DIM "MODE: " SH_RESET SH_PEACH "SHARDED" SH_RESET "\033[K\n",
                uptime / 3600, (uptime / 60) % 60, uptime % 60);
        fprintf(stdout, "\033[K\n");

        // Market + account
        double price_d = last_price.load(std::memory_order_relaxed);
        double vol_d = last_volume.load(std::memory_order_relaxed);
        fprintf(stdout, " " SH_DIM " PRICE " SH_RESET SH_BOLD SH_WHEAT "$%.2f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "VOL " SH_RESET SH_FG "%.4f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "BAL " SH_RESET SH_FG "$%.2f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "P&L " SH_RESET SH_BOLD "%s$%+.4f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "POS " SH_RESET SH_FG "%d/%u" SH_RESET "\033[K\n",
                price_d, vol_d, bal, SH_PNL(pnl), pnl, active, (unsigned)num_cores);
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
        fprintf(stdout, "  " SH_DIM "core      samples       min       p50       p95       p99       max       avg" SH_RESET "\033[K\n");
        for (int i = 0; i < num_cores; ++i) {
            CoreLatencySnapshot ls = CoreLatencyStats_Snapshot(&cores[i].latency_stats, tsc_ghz);
            if (ls.total_count == 0) {
                fprintf(stdout, "  " SH_FG " %2d  " SH_DIM "%10s   %6s   %6s   %6s   %6s   %6s   %6s" SH_RESET "\033[K\n",
                        i, "0", "-", "-", "-", "-", "-", "-");
            } else {
                fprintf(stdout, "  " SH_FG " %2d  " SH_DIM "%10lu  " SH_FG "%5.0fns  %5.0fns  %5.0fns  %5.0fns  %5.0fns  %5.0fns" SH_RESET "\033[K\n",
                        i, (unsigned long)ls.total_count,
                        ls.min_ns, ls.p50_ns, ls.p95_ns, ls.p99_ns, ls.max_ns, ls.avg_ns);
            }
        }

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

    fprintf(stderr, "[sharded] shutdown requested, joining threads...\n");
    producer.join();
    for (auto& e : executors) e.join();
    drainer.join();
    OrderManager_Shutdown(&oms);

    fprintf(stderr, "[sharded] all threads joined.\n");
    fprintf(stderr, "[sharded] final: produced=%lu consumed=%lu entries=%lu exits=%lu balance=%.4f\n",
            (unsigned long)ticks_produced.load(),
            (unsigned long)ticks_consumed_total.load(),
            (unsigned long)state.total_entries,
            (unsigned long)state.total_exits,
            FPN_ToDouble(state.balance));

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
