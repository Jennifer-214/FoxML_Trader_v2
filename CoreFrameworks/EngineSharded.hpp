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
#include "../ML_Headers/BuildFlags.hpp"  // v5.9.5h: BUILD_FLAGS_HASH() for cross-build drift WARN
#include "../Strategies/StrategyParameters.hpp"
#include "../Strategies/StrategyLifecycle.hpp"  // v5.4.0 Phase 1.2: Strategy_InitPerCore / _FreePerCore
#include "../DataStream/BinanceUserData.hpp"
#include "BinanceAdapter.hpp"
#include "ReconciliationLoop.hpp"
#include "Reconcile.hpp"  // v5.2.1: boot-time exchange-truth reconcile (parse + decide)
#include "../MemHeaders/HealthLog.hpp"  // v5.4.0 Phase 0.1: structured JSONL diagnostic log
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
#include <x86intrin.h>  // v4.7.42: __rdtsc for slow-path latency sampling
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
#include <unistd.h>  // sysconf(_SC_NPROCESSORS_ONLN) — POSIX, also available on macOS

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
// [SMART SLOW-PATH CPU PIN ASSIGNMENT — v5.1.5]
//======================================================================================================
// Read /sys/devices/system/cpu/cpuN/topology/thread_siblings_list to learn
// which CPUs share a physical core (SMT siblings). Choose slow-path pins
// that AVOID landing on SMT siblings of the producer/hot-path/drainer
// threads — those siblings contend with the busiest threads on the box
// for L1/L2 cache and execution units.
//
// Returns 1 on success (writes N pins to out_pins[0..N-1]), 0 on failure
// (caller should fall back to the simple round-robin auto-derive).
//======================================================================================================
static inline int EngineSharded_GetSiblingCPU(int cpu_id) {
#ifdef __linux__
    char path[256];
    snprintf(path, sizeof(path),
        "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu_id);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);
    // Format: "cpu_id,sibling_id" or "cpu_id-sibling_id" — parse two ints.
    int a = -1, b = -1;
    if (sscanf(buf, "%d,%d", &a, &b) == 2 || sscanf(buf, "%d-%d", &a, &b) == 2) {
        if (a == cpu_id) return b;
        if (b == cpu_id) return a;
    }
    return -1;  // single-thread-per-core CPU
#else
    (void)cpu_id;
    return -1;
#endif
}

// Computes slow-path pin assignment that avoids SMT-sharing with busy
// threads. Strategy:
//   1. Build set of "hot" CPUs = {producer_cpu, hot_path[0..N-1], drainer_cpu}
//   2. Build set of "tainted" CPUs = SMT siblings of hot CPUs
//   3. Build candidate list: CPUs not in hot ∪ tainted (true idle)
//   4. If we have enough candidates, assign in order
//   5. Otherwise fall back to including tainted CPUs (still better than
//      colliding with hot — round-robin among the tainted pool)
//
// out_pins[0..num_slow-1] gets the chosen CPU IDs. Returns 1 on success.
static inline int EngineSharded_SmartSlowPathPins(int producer_cpu,
                                                    int drainer_cpu,
                                                    int num_hot,
                                                    int num_slow,
                                                    int* out_pins) {
    if (num_slow <= 0 || num_slow > 16) return 0;
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc < 2) return 0;

    // Mark hot CPUs (running busy work).
    bool hot[64] = {false};
    if (producer_cpu >= 0 && producer_cpu < (int)nproc) hot[producer_cpu] = true;
    if (drainer_cpu  >= 0 && drainer_cpu  < (int)nproc) hot[drainer_cpu] = true;
    for (int i = 0; i < num_hot && i < 16; ++i) {
        int hcpu = i + 1;  // hot-path i pins to CPU i+1 by convention
        if (hcpu >= 0 && hcpu < (int)nproc) hot[hcpu] = true;
    }

    // Mark tainted = SMT siblings of hot CPUs.
    bool tainted[64] = {false};
    for (int i = 0; i < (int)nproc && i < 64; ++i) {
        if (!hot[i]) continue;
        int sib = EngineSharded_GetSiblingCPU(i);
        if (sib >= 0 && sib < (int)nproc) tainted[sib] = true;
    }

    // First pass: pick truly idle CPUs (not hot, not tainted).
    int chosen[16] = {0};
    int chosen_count = 0;
    for (int i = 0; i < (int)nproc && chosen_count < num_slow; ++i) {
        if (!hot[i] && !tainted[i]) {
            chosen[chosen_count++] = i;
        }
    }
    // Second pass: if we still need more, fall back to tainted (better
    // than landing on hot CPUs themselves).
    for (int i = 0; i < (int)nproc && chosen_count < num_slow; ++i) {
        if (tainted[i] && !hot[i]) {
            // Skip if already chosen
            bool already = false;
            for (int j = 0; j < chosen_count; ++j) if (chosen[j] == i) already = true;
            if (!already) chosen[chosen_count++] = i;
        }
    }
    // Third pass (rare — small CPU box): wrap around to hot CPUs.
    for (int i = 0; i < (int)nproc && chosen_count < num_slow; ++i) {
        bool already = false;
        for (int j = 0; j < chosen_count; ++j) if (chosen[j] == i) already = true;
        if (!already) chosen[chosen_count++] = i;
    }

    if (chosen_count < num_slow) return 0;
    for (int i = 0; i < num_slow; ++i) out_pins[i] = chosen[i];
    return 1;
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

//======================================================================================================
// [v5.10.2.A — POST-LOAD VALIDATOR]
//======================================================================================================
// Cross-zoo validator extracted from EngineSharded_Run boot loop body
// (parity-check Findings #3 + #7 + #10). Subsumes THREE existing WARN/REFUSE
// subgroups uniformly across single zoo + ensemble parallel-array handles:
//
//   1. v5.9.4 + v5.9.5h xgb-and-friends WARN
//      (training_poll_interval + xgb_hyperparams + build_flags_hash)
//      gated by !acknowledge_cross_binary_version_drift
//   2. v5.9.5i inference_cfg drift detection
//      Tier 1 (freshness_tau, threshold_scale, barrier_gate_enabled) REFUSE in strict
//      Tier 2 (hard_block, bandit, fees) WARN regardless
//      gated by !acknowledge_inference_cfg_drift
//
// Writes cfg_drift_tier1/tier2_count + strict_refused into ctx.
// Returns 0 on accept, -1 on REFUSE in strict mode (Tier 1 mismatch).
//
// Callable from:
//   - EngineSharded_Run boot loop (replaces inline blocks)
//   - EngineSharded_Run hot swap branch (post-CoreModelZoo_LoadFromDir)
//
// Ensemble support: pass &ml_ensemble_zoos[i] for ezoo when
// state.cores[i].ensemble_handle != nullptr, else nullptr.
// Closes parity-check Finding #7 (drift block iterated single-zoo only).
//
// Hot-swap rollback semantics: helper returns -1 on Tier 1 REFUSE, but
// caller in hot-swap context logs + leaves (model_load_failed=1) rather
// than crashing the engine — pre-swap state isn't snapshotted, so true
// rollback would require additional infrastructure (deferred to v5.10.X).
//======================================================================================================
template <unsigned F>
static inline int CoreModelZoo_ValidateAgainstCfg(
    CoreModelZoo<F>* zoo,
    EnsembleModelZoo<F>* ezoo,                       // nullptr when ensemble inactive
    const ControllerConfig<F>& cfg,
    int core_id,
    int strict_mode,                                  // cfg.held_out_gate_strict
    int acknowledge_inference_cfg_drift,              // suppresses drift block
    int acknowledge_cross_binary_version_drift,       // suppresses xgb/poll/build_flags WARN
    CoreContext<F>* ctx                               // for cfg_drift_* counter writeback
) {
    int strict = (strict_mode == 1);
    int tier1_count = 0;
    int tier2_count = 0;
    int tier1_refused_count = 0;

    // Inner check: applies xgb-and-friends WARN (subgroup 1) + drift (subgroup 2)
    // to a single ModelHandle. Lambda captures the per-core context (logs,
    // counters) so per-handle work stays tight. h_idx >= 0 means ensemble
    // member at slot [h_idx]; -1 means single-zoo.
    auto check_handle = [&](ModelHandle<F>* h, const char* role_name, int h_idx) {
        if (!h) return;
        // Distinguishable log prefix: "core 0" vs "core 0 ensemble[2]"
        char loc[64];
        if (h_idx < 0) {
            snprintf(loc, sizeof(loc), "core %d", core_id);
        } else {
            snprintf(loc, sizeof(loc), "core %d ensemble[%d]", core_id, h_idx);
        }

        // === Subgroup 1: xgb-and-friends WARN (training_poll_interval +
        //                  xgb_hyperparams + build_flags_hash) ===
        if (!acknowledge_cross_binary_version_drift) {
            if (h->has_training_poll_interval &&
                h->training_poll_interval != cfg.poll_interval) {
                fprintf(stderr,
                    "[poll_interval] WARN: %s role=%s stamp claims "
                    "training_poll_interval=%u but cfg.poll_interval=%u "
                    "(set acknowledge_cross_binary_version_drift=1 to suppress)\n",
                    loc, role_name,
                    (unsigned)h->training_poll_interval,
                    (unsigned)cfg.poll_interval);
            }
            if (h->has_xgb_hyperparams) {
                double cfg_subsample = FPN_ToDouble(cfg.xgb_subsample);
                double cfg_colsample = FPN_ToDouble(cfg.xgb_colsample_bytree);
                if (fabs(h->stamp_xgb_subsample - cfg_subsample) > 1e-6) {
                    fprintf(stderr,
                        "[xgb_hyperparams] WARN: %s role=%s stamp "
                        "claims xgb_subsample=%.4f but cfg=%.4f\n",
                        loc, role_name, h->stamp_xgb_subsample, cfg_subsample);
                }
                if (fabs(h->stamp_xgb_colsample_bytree - cfg_colsample) > 1e-6) {
                    fprintf(stderr,
                        "[xgb_hyperparams] WARN: %s role=%s stamp "
                        "claims xgb_colsample_bytree=%.4f but cfg=%.4f\n",
                        loc, role_name, h->stamp_xgb_colsample_bytree,
                        cfg_colsample);
                }
                if (h->stamp_xgb_min_child_weight != cfg.xgb_min_child_weight) {
                    fprintf(stderr,
                        "[xgb_hyperparams] WARN: %s role=%s stamp "
                        "claims xgb_min_child_weight=%d but cfg=%d\n",
                        loc, role_name, h->stamp_xgb_min_child_weight,
                        cfg.xgb_min_child_weight);
                }
                if (h->stamp_xgb_seed != cfg.xgb_seed) {
                    fprintf(stderr,
                        "[xgb_hyperparams] WARN: %s role=%s stamp "
                        "claims xgb_seed=%d but cfg=%d\n",
                        loc, role_name, h->stamp_xgb_seed, cfg.xgb_seed);
                }
                if (strcmp(h->stamp_xgb_tree_method, cfg.xgb_tree_method) != 0) {
                    fprintf(stderr,
                        "[xgb_hyperparams] WARN: %s role=%s stamp "
                        "claims xgb_tree_method=%s but cfg=%s\n",
                        loc, role_name, h->stamp_xgb_tree_method,
                        cfg.xgb_tree_method);
                }
            }
            if (h->has_build_flags_hash &&
                h->stamp_build_flags_hash != tt::BUILD_FLAGS_HASH()) {
                fprintf(stderr,
                    "[build_flags] WARN: %s role=%s stamp claims "
                    "build_flags_hash=%016lx but current build is %016lx "
                    "(cross-build drift; set acknowledge_cross_binary_version_drift=1 to suppress)\n",
                    loc, role_name,
                    (unsigned long)h->stamp_build_flags_hash,
                    (unsigned long)tt::BUILD_FLAGS_HASH());
            }
            // v5.11.42 D.1 — xgb_train_nthread mode-divergence WARN.
            // Stamp's nthread=1 + cfg nthread>1 → operator trained in
            // parallel multi-horizon mode (which pins to 1) but engine
            // would now retrain at higher nthread → bytewise model
            // divergence. Forensic only — model already trained, can't
            // be retrained at load. Operator notification.
            if (h->has_stamp_xgb_train_nthread &&
                h->stamp_xgb_train_nthread != cfg.xgb_train_nthread) {
                fprintf(stderr,
                    "[xgb_train_nthread] WARN: %s role=%s stamp claims "
                    "xgb_train_nthread=%d but cfg.xgb_train_nthread=%d "
                    "(mode divergence; stamp=1 indicates parallel multi-horizon "
                    "training, cfg>1 indicates serial mode would diverge "
                    "bytewise on retrain; set acknowledge_cross_binary_version_drift=1 "
                    "to suppress)\n",
                    loc, role_name,
                    h->stamp_xgb_train_nthread,
                    cfg.xgb_train_nthread);
            }
        }

        // === Subgroup 2: inference_cfg drift (Tier 1 REFUSE in strict;
        //                  Tier 2 WARN regardless) ===
        if (!acknowledge_inference_cfg_drift && h->has_stamp_inference_cfg) {
            double cfg_cts = FPN_ToDouble(cfg.confidence_threshold_scale);
            double cfg_chb = FPN_ToDouble(cfg.confidence_hard_block_threshold);
            double cfg_tau = FPN_ToDouble(cfg.confidence_freshness_tau);

            // Tier 1: directly affects serving math
            bool tier1_drift = false;
            if (fabs(h->stamp_inf_freshness_tau - cfg_tau) > 1e-6) {
                fprintf(stderr,
                    "[inference_cfg] %s: %s role=%s stamp claims "
                    "confidence_freshness_tau=%.2f but cfg=%.2f\n",
                    strict ? "REFUSE (Tier 1, strict mode)" : "WARN (Tier 1)",
                    loc, role_name, h->stamp_inf_freshness_tau, cfg_tau);
                tier1_drift = true;
                ++tier1_count;
            }
            if (fabs(h->stamp_inf_confidence_threshold_scale - cfg_cts) > 1e-6) {
                fprintf(stderr,
                    "[inference_cfg] %s: %s role=%s stamp claims "
                    "confidence_threshold_scale=%.4f but cfg=%.4f\n",
                    strict ? "REFUSE (Tier 1, strict mode)" : "WARN (Tier 1)",
                    loc, role_name, h->stamp_inf_confidence_threshold_scale, cfg_cts);
                tier1_drift = true;
                ++tier1_count;
            }
            if (h->stamp_inf_barrier_gate_enabled != cfg.barrier_gate_enabled) {
                fprintf(stderr,
                    "[inference_cfg] %s: %s role=%s stamp claims "
                    "barrier_gate_enabled=%d but cfg=%d\n",
                    strict ? "REFUSE (Tier 1, strict mode)" : "WARN (Tier 1)",
                    loc, role_name, h->stamp_inf_barrier_gate_enabled,
                    cfg.barrier_gate_enabled);
                tier1_drift = true;
                ++tier1_count;
            }
            if (tier1_drift && strict) ++tier1_refused_count;

            // Tier 2: WARN regardless of strict mode
            if (fabs(h->stamp_inf_confidence_hard_block_threshold - cfg_chb) > 1e-6) {
                fprintf(stderr,
                    "[inference_cfg] WARN (Tier 2): %s role=%s stamp "
                    "claims confidence_hard_block_threshold=%.4f but cfg=%.4f\n",
                    loc, role_name, h->stamp_inf_confidence_hard_block_threshold,
                    cfg_chb);
                ++tier2_count;
            }
            if (h->has_stamp_bandit && cfg.bandit_enabled) {
                double cfg_bbr = FPN_ToDouble(cfg.bandit_blend_ratio);
                if (fabs(h->stamp_inf_bandit_blend_ratio - cfg_bbr) > 1e-6) {
                    fprintf(stderr,
                        "[inference_cfg] WARN (Tier 2): %s role=%s stamp "
                        "claims bandit_blend_ratio=%.4f but cfg=%.4f\n",
                        loc, role_name, h->stamp_inf_bandit_blend_ratio, cfg_bbr);
                }
            }
            if (h->has_stamp_fees && cfg.cost_gate_enabled) {
                double cfg_frm = FPN_ToDouble(cfg.fee_rate_maker);
                double cfg_frt = FPN_ToDouble(cfg.fee_rate_taker);
                if (fabs(h->stamp_inf_fee_rate_maker - cfg_frm) > 1e-6) {
                    fprintf(stderr,
                        "[inference_cfg] WARN (Tier 2): %s role=%s stamp "
                        "claims fee_rate_maker=%.6f but cfg=%.6f\n",
                        loc, role_name, h->stamp_inf_fee_rate_maker, cfg_frm);
                }
                if (fabs(h->stamp_inf_fee_rate_taker - cfg_frt) > 1e-6) {
                    fprintf(stderr,
                        "[inference_cfg] WARN (Tier 2): %s role=%s stamp "
                        "claims fee_rate_taker=%.6f but cfg=%.6f\n",
                        loc, role_name, h->stamp_inf_fee_rate_taker, cfg_frt);
                }
            }
        }
    };

    // 1. Single zoo: 4 roles
    //    CoreModelZoo struct uses `exit` (singular) per CoreModelZoo.hpp:60
    if (zoo) {
        check_handle(&zoo->buy_signal, "buy_signal", -1);
        check_handle(&zoo->barrier,    "barrier",    -1);
        check_handle(&zoo->regime,     "regime",     -1);
        check_handle(&zoo->exit,       "exit",       -1);
    }
    // 2. Ensemble handles (Finding #7 closure): 4 roles × N horizons
    //    EnsembleModelZoo struct uses `exit_predictor` (NOT `exit`)
    //    per CoreModelZoo.hpp:616
    if (ezoo && ezoo->active) {
        for (int h = 0; h < ezoo->buy_signal_count; ++h)
            check_handle(&ezoo->buy_signal[h], "buy_signal", h);
        for (int h = 0; h < ezoo->barrier_count; ++h)
            check_handle(&ezoo->barrier[h], "barrier", h);
        for (int h = 0; h < ezoo->regime_count; ++h)
            check_handle(&ezoo->regime[h], "regime", h);
        for (int h = 0; h < ezoo->exit_predictor_count; ++h)
            check_handle(&ezoo->exit_predictor[h], "exit", h);
    }

    // Writeback drift counters (Finding #10 closure: now updated on hot-swap too)
    if (ctx) {
        ctx->cfg_drift_tier1_count = (uint8_t)(tier1_count > 255 ? 255 : tier1_count);
        ctx->cfg_drift_tier2_count = (uint8_t)(tier2_count > 255 ? 255 : tier2_count);
        ctx->cfg_drift_strict_refused = (tier1_refused_count > 0) ? 1 : 0;
    }

    if (tier1_refused_count > 0 && strict) {
        fprintf(stderr,
            "[inference_cfg] FATAL: core %d had %d Tier 1 mismatch(es) "
            "in strict mode. Set held_out_gate_strict=0 (warn-only) "
            "OR acknowledge_inference_cfg_drift=1 to bypass, "
            "OR retrain the model with current cfg.\n",
            core_id, tier1_refused_count);
        return -1;  // REFUSE
    }
    return 0;
}

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

    // v5.4.0 Phase 0.1 — wire Health log per cfg. Empty path = disabled
    // (Health_Log calls become no-ops). Non-empty path = JSONL output;
    // every Health_Log call appends a line. See MemHeaders/HealthLog.hpp.
    tt::Health_LogConfigure(cfg.health_log_path, cfg.health_log_level);
    if (cfg.health_log_path[0]) {
        tt::Health_Log(tt::HEALTH_INFO, "engine", -1,
            "engine_start arch=sharded num_cores=%u health_log_level=%d",
            (unsigned)cfg.num_execution_cores, cfg.health_log_level);
    }

    // v5.7.2 — hardcoded strategy boot guard. AUTO (regime-gated) is the
    // recommended assignment for live/paper runs because it routes
    // strategy selection through the regime classifier. Hardcoded
    // strategies fire regardless of regime, which is fine for backtest
    // comparison but risky for capital. Per the v5.7 regime audit
    // (DOCS/changelogs/2026-04-30-regime-classifier-audit.md): the
    // 2026-04-30 "MOM enters in RANGING" symptom traced to Core 0
    // being hardcoded MOM, not an AUTO classifier bug.
    //
    // Behavior:
    //   paper run + hardcoded core → WARN to stderr + health log
    //   live run (use_real_money=1) + hardcoded core →
    //     ERROR + abort UNLESS acknowledge_hardcoded_strategy_in_live=1
    {
        int hardcoded_count = 0;
        char hardcoded_list[256] = {0};
        size_t pos = 0;
        for (uint16_t i = 0; i < cfg.num_execution_cores && i < 16; ++i) {
            uint8_t sid = cfg.core_strategies[i];
            if (sid != STRATEGY_AUTO && sid != STRATEGY_NONE) {
                hardcoded_count++;
                if (pos < sizeof(hardcoded_list) - 16) {
                    const char* sname = (sid < NUM_STRATEGIES)
                        ? STRATEGY_SHORT_NAMES[sid] : "?";
                    pos += (size_t)snprintf(hardcoded_list + pos,
                        sizeof(hardcoded_list) - pos,
                        "%score_%u=%s", pos > 0 ? ", " : "", i, sname);
                }
            }
        }
        if (hardcoded_count > 0) {
            const bool live = (cfg.use_real_money != 0);
            const bool ack  = (cfg.acknowledge_hardcoded_strategy_in_live != 0);
            if (live && !ack) {
                fprintf(stderr,
                    "[sharded] ERROR: live mode (use_real_money=1) with "
                    "%d hardcoded strategy core(s): %s\n"
                    "[sharded]        AUTO is recommended for live capital "
                    "(regime-gated strategy selection).\n"
                    "[sharded]        To override: set "
                    "acknowledge_hardcoded_strategy_in_live=1 in engine.cfg.\n",
                    hardcoded_count, hardcoded_list);
                tt::Health_Log(tt::HEALTH_INFO, "engine", -1,
                    "boot_abort reason=hardcoded_strategy_in_live cores=%s",
                    hardcoded_list);
                return;  // refuse to boot
            }
            // paper, or live+ack — WARN only
            fprintf(stderr,
                "[sharded] WARN: %d hardcoded strategy core(s): %s. "
                "AUTO is recommended for live/paper runs (regime-gated). "
                "Hardcoded is fine for backtest comparisons.\n",
                hardcoded_count, hardcoded_list);
            if (cfg.health_log_path[0]) {
                tt::Health_Log(tt::HEALTH_INFO, "engine", -1,
                    "boot_warn hardcoded_strategy_count=%d cores=%s "
                    "live=%d acknowledged=%d",
                    hardcoded_count, hardcoded_list,
                    (int)live, (int)ack);
            }
        }
    }

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

    // v5.11.6.A — InitArena: single mmap'd region for all init-time
    // allocations (PortfolioController rolling stats × 3 + cumdelta_state +
    // per-core CoreSlowState + per-core strategy state). MAP_POPULATE
    // pre-faults all pages at boot so first slow-path cycle never page-faults.
    //
    // Sizing (measured at boot 2026-05-07):
    //   - CoreSlowState<64> ≈ 278 KB / core × 16 cores = 4.4 MB
    //   - RollingStats × 3 + CumDeltaState  ≈ 60 KB
    //   - Strategy state × 16 cores         ≈ 80 KB
    //   - Headroom for future growth        ≈ ~3 MB
    //   Total: 8 MB. Within the mlockall envelope (RLIMIT_MEMLOCK ≥ 256 MB
    //   per the deployment runbook).
    static tt::InitArena g_init_arena;
    // v5.11.22 — operator-gated MAP_HUGETLB. Default 0 = use 4 KB pages.
    // Set cfg.init_arena_use_hugepages=1 + reserve hugepages at the OS
    // level (sudo sysctl -w vm.nr_hugepages=4) for ~512× fewer TLB
    // entries on the 8 MB arena. InitArena_Create silently falls back
    // to non-HUGETLB on failure (with a stderr WARN) — never fatal.
    int arena_extra_flags = cfg.init_arena_use_hugepages ? MAP_HUGETLB : 0;
    g_init_arena = tt::InitArena_Create(8 * 1024 * 1024, arena_extra_flags);
    tt::InitArena_Global() = &g_init_arena;

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
    // v5.1.2 — full per-core data plane. All rolling stats, regime, flow,
    // depth-history state lives in EACH engine's `state.cores[c].slow_state`
    // (heap-allocated by EventLoopState_Init). Three callers update them:
    //   - centralized:    producer's fan_out + slow-path body via
    //                     EventLoop_UpdateRollingStateAllCores helper
    //   - per_core_slow:  per-core slow-path lambda updates own slow_state
    //   - backtest:       same helper, single-thread, linear iteration
    //
    // Pre-v5.1.2 the producer maintained ONE shared copy at function scope.
    // Removed in v5.1.2; readers now use state.cores[c].slow_state pointers.
    // ema_price stays at producer scope since it's per-tick (replicated to
    // all N engines via EventLoop_UpdateEmaPriceAllCores in fan_out).
    FPN<F> ema_price = FPN_Zero<F>();
    // EMA alpha matches PortfolioController_Init's default (gate_ema_alpha
    // is the cfg key). Computed at boot from cfg; updated each tick.
    FPN<F> ema_alpha = !FPN_IsZero(cfg.gate_ema_alpha)
                       ? cfg.gate_ema_alpha
                       : FPN_FromDouble<F>(0.1);

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
            // v5.10.0a.G.5 — per-core ensemble zoo, mirrors single-zoo allocation.
            // Default empty = ezoo->active=0 = single-zoo path runs unchanged.
            static EnsembleModelZoo<F> ml_ensemble_zoos[MAX_EXECUTION_CORES];
            EnsembleModelZoo_Init(&ml_ensemble_zoos[i]);
            int backend = cfg.ml_backend ? cfg.ml_backend : MODEL_BACKEND_XGBOOST;

            int loaded = 0;
            if (cfg.core_model_dir[i][0]) {
                // path 1: zoo from directory (auto-discovered roles).
                // v5.9.4 — pass cfg.acknowledge_cross_binary_version_drift
                // through so per-role load suppresses minor-drift WARN
                // when operator deliberately deploys a v5.x.y model on
                // a v5.x.z engine.
                // v5.11.18 main — pass per-core feature_mask through. When
                // operator's cfg has core_<i>_feature_mask=0xHEXVAL set
                // (default 0xFFFF..F = all features enabled), the stamp's
                // feature_mask_train must match or load refuses. When mask
                // is the all-on default, expected_feature_mask=0 (skip
                // check; legacy stamps still load).
                uint64_t mask_for_load = (cfg.core_feature_mask[i] != 0xFFFFFFFFFFFFFFFFULL)
                    ? cfg.core_feature_mask[i] : 0;
                loaded = CoreModelZoo_LoadFromDir(&ml_zoos[i], cfg.core_model_dir[i],
                    backend, /*secret=*/nullptr, /*gap=*/0.05,
                    /*strict=*/cfg.held_out_gate_strict,
                    cfg.acknowledge_cross_binary_version_drift,
                    /*expected_feature_mask=*/mask_for_load);
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
                        // v5.9.0b: surface load failure to operator via TUI/health log
                        state.cores[i].model_load_failed = 1;
                    }
                }
            }
            // v5.11.60 — ensemble auto-detect MUST run regardless of single-zoo
            // load result. Pre-fix this was inside the `if (loaded)` block,
            // which meant multi-horizon-only deployments (where the base path
            // models/<dir> doesn't exist — only `<dir>_horizon_<H>` siblings
            // do) silently failed: single-zoo load returns 0 because base
            // path is absent, then ensemble auto-detect is skipped, then the
            // ML core boots with NO model and `model_load_failed=1`.
            //
            // Symptom: engine.log shows
            //   [ML] zoo loaded 0 role(s) from <base> (mask=0x0)
            //   [ML] strategy initialized — no model loaded (predictions disabled)
            // and ML Status panel shows "core N: model: LOAD FAILED" forever,
            // even though the _horizon_<H> sibling dirs are fully populated.
            // Operator hit this on a multi-horizon training output that
            // produced only sibling dirs, no base.
            //
            // v5.10.0a.G.5 — try ensemble auto-detect on the base dir.
            // No-op when no _horizon_<H> siblings present; ezoo->active=0
            // → single-zoo path runs unchanged.
            int ensemble_loaded = 0;
            if (cfg.core_model_dir[i][0]) {
                // v5.10.1.C — Plumb cfg-derived strict/gap/secret/drift args
                // (parity-check Finding #6). Without these, ensemble auto-detect
                // silently bypassed cfg.held_out_gate_strict in ensemble mode.
                int n_loaded = EnsembleModelZoo_AutoDetectFromDir(
                    &ml_ensemble_zoos[i],
                    cfg.core_model_dir[i],
                    backend,
                    cfg.held_out_stamp_secret,
                    FPN_ToDouble(cfg.gap_acceptable_threshold),
                    cfg.held_out_gate_strict,
                    cfg.acknowledge_cross_binary_version_drift);
                if (n_loaded > 0 && ml_ensemble_zoos[i].active) {
                    fprintf(stderr, "[sharded] core %d: ensemble active "
                                    "(primary=%s, %d horizons; %d total models)\n",
                            i,
                            ml_ensemble_zoos[i].primary_role_name[0]
                                ? ml_ensemble_zoos[i].primary_role_name : "(none)",
                            ml_ensemble_zoos[i].primary_count, n_loaded);
                    // v5.10.0a.G.7 — initialize per-regime bandits + cfg-driven mode
                    EnsembleModelZoo_InitBandits(&ml_ensemble_zoos[i],
                                                   cfg.ensemble_bandit_eta,
                                                   cfg.ensemble_min_warmup_predictions);
                    const char* mode = cfg.core_ensemble_blend_mode[i][0]
                                      ? cfg.core_ensemble_blend_mode[i]
                                      : cfg.ensemble_blend_mode;
                    strncpy(ml_ensemble_zoos[i].blend_mode, mode,
                            sizeof(ml_ensemble_zoos[i].blend_mode) - 1);
                    ml_ensemble_zoos[i].blend_mode[
                        sizeof(ml_ensemble_zoos[i].blend_mode) - 1] = '\0';
                    EnsembleModelZoo_SetDisabledHorizons(&ml_ensemble_zoos[i],
                        cfg.core_disabled_horizons[i]);
                    // v5.10.0a.G.9 — overlay persisted bandit state (if any)
                    EnsembleModelZoo_LoadBanditState(&ml_ensemble_zoos[i],
                                                      cfg.core_model_dir[i]);
                    EnsembleModelZoo_SetBanditSaveInterval(&ml_ensemble_zoos[i],
                        cfg.ensemble_bandit_save_interval);
                    state.cores[i].ensemble_handle = &ml_ensemble_zoos[i];
                    ensemble_loaded = 1;
                } else {
                    state.cores[i].ensemble_handle = nullptr;
                }
            }
            // v5.11.60 — model_load_failed only fires if BOTH single-zoo AND
            // ensemble paths failed. Pre-fix the `else` block fired even when
            // ensemble would have succeeded (because ensemble was inside `if
            // (loaded)`).
            if (!loaded && !ensemble_loaded) {
                // v5.9.0b: ML strategy was selected but model didn't load.
                // Distinct from "no model configured" (which is operator
                // intent — leave flag at 0). Here: strategy=ML + load
                // attempted + failed → surface to operator.
                state.cores[i].model_load_failed = 1;
            }

            // v5.10.2.A — POST-LOAD VALIDATOR (extracted; closes parity-check
            // Findings #3 + #7 + #10). Replaces the v5.9.4a + v5.9.5h xgb-and-
            // friends WARN block AND the v5.9.5i inference_cfg drift block with
            // a single call. Now iterates ensemble parallel-array handles too
            // (Finding #7), and is callable from the hot-swap branch (Finding #3).
            if (loaded && cfg.core_model_dir[i][0]) {
                CoreModelZoo<F>* zoo = &ml_zoos[i];
                EnsembleModelZoo<F>* ezoo = state.cores[i].ensemble_handle
                    ? &ml_ensemble_zoos[i] : nullptr;
                CoreModelZoo_ValidateAgainstCfg<F>(
                    zoo, ezoo, cfg, /*core_id=*/i,
                    cfg.held_out_gate_strict,
                    cfg.acknowledge_inference_cfg_drift,
                    cfg.acknowledge_cross_binary_version_drift,
                    &state.cores[i]);
                // Note: validator returns -1 on REFUSE in strict mode but the
                // existing v5.9.5i semantics here were "log loudly + leave
                // handle loaded" (TODO v5.10: free handle + return-from-boot
                // to enforce refuse properly). The validator preserves this:
                // counters are written, FATAL log fires, but engine continues.
                // Hot-swap branch handles REFUSE differently (model_load_failed).
            }

            // Phase 6prep sharded c12: re-init ConfidenceScorer with cfg
            // tunables. EventLoopState_Init left it at safe defaults; for
            // ML cores we want the user's window/tau settings active.
            // v4.7.32: read per-core resolved cfg so per-core
            // confidence_freshness_tau override actually takes effect.
            // confidence_window stays global (INT not in X-macro yet).
            const auto& ov_conf = cfg.core_overrides[i];
            FPN<F> tau_eff = !FPN_IsZero(ov_conf.confidence_freshness_tau)
                ? ov_conf.confidence_freshness_tau
                : cfg.confidence_freshness_tau;
            ConfidenceScorer_Init(&state.cores[i].confidence,
                                  (int)cfg.confidence_window,
                                  FPN_ToDouble(tau_eff));
        }

        // v5.4.0 Phase 1.3 — wire Strategy_InitPerCore. Allocates the
        // strategy state struct matching state.cores[i].strategy_id.
        // Pre-warmup: strategies' state structs get garbage initial values
        // (rolling stats are empty); first slow-path _Adapt cadence after
        // warmup converges them to sane values. This is safe because
        // permission=0 until warmup completes, so no entries can fire
        // with garbage state.
        //
        // Pre-v5.4 status: this call was MISSING in the sharded path —
        // the entire strategy state lifecycle was orphaned (postmortem F7).
        if (state.cores[i].strategy_id != STRATEGY_NONE) {
            tt::Strategy_InitPerCore(&state, i, state.cores[i].strategy_id,
                                      &state.cores[i].slow_state->rolling_short,
                                      &cfg);
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
                                                  cfg.partial_exit_enabled ? 1 : 0,
                                                  &cfg);  // v5.5.5
            (void)loaded;  // logged inside; nothing else to do here
        } else {
            // v5.2.2 (live reconciliation Phase 1 wiring): exchange truth
            // is authoritative in live mode. Fetch account + open orders +
            // recent trades and reconcile against local state. Refuses
            // boot on critical disagreement (exchange has BTC, local has
            // 0 positions). Apply path is dry-run by default — flip
            // cfg.reconcile_dry_run=0 deliberately for production after
            // verifying logs.
            //
            // Phase 2 (deferred): actually CANCEL flagged orders, REPLAY
            // missed fills via OrderManager_ProcessFillCommand, FORCE-CLOSE
            // local slots that don't match exchange. Requires
            // BinanceOrderAPI_CancelOrder + Command synthesis helpers
            // that don't exist yet. v5.2.2 ships dry-run + critical
            // refusal — surfaces all the disagreements without applying.
            fprintf(stderr, "[snapshot] LIVE mode: reconciling with exchange truth\n");

            BinanceOrderAPI* api = &g_sharded_binance_adapter.workers_api[0];
            double exch_usdt = 0.0, exch_btc = 0.0;
            BinanceOrderAPI_GetBalances(api, &exch_usdt, &exch_btc);

            char open_buf[16384] = {0};
            char trades_buf[65536] = {0};
            BinanceOrderAPI_GetOpenOrders(api, open_buf, sizeof(open_buf));
            BinanceOrderAPI_GetMyTrades(api, /*since_trade_id=*/0,
                                         trades_buf, sizeof(trades_buf));

            tt::ReconcileOpenOrder orders[16];
            tt::ReconcileTrade     trades[256];
            int n_orders = tt::Reconcile_ParseOpenOrders(open_buf, orders, 16);
            int n_trades = tt::Reconcile_ParseMyTrades(trades_buf, trades, 256);

            int local_open = __builtin_popcount(oms.portfolio.active_bitmap);
            tt::ReconcileResult rr = tt::Reconcile_Decide(
                exch_usdt, exch_btc,
                orders, n_orders,
                trades, n_trades,
                local_open);

            tt::Reconcile_LogReport(rr, cfg.reconcile_dry_run);

            if (rr.refused_boot) {
                fprintf(stderr,
                    "[reconcile] CRITICAL — refusing to boot. "
                    "Resolve disagreement manually then restart.\n");
                BinanceAdapter_ShutdownState(&g_sharded_binance_adapter);
                std::signal(SIGINT,  prev_int);
                std::signal(SIGTERM, prev_term);
                return;
            }

            if (!cfg.reconcile_dry_run) {
                fprintf(stderr,
                    "[reconcile] WARN: reconcile_dry_run=0 set but "
                    "Phase 2 apply path not implemented yet. No changes "
                    "applied. Set dry_run=1 explicitly to silence this.\n");
            }
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
    TUISnapshot_InitSeq(&g_shared);  // v5.11.3.B — seq starts at 0 (idx=0, parity=stable)
    g_shared.quit_requested = 0;
    g_shared.pause_requested = 0;
    g_shared.reload_requested = 0;
    g_shared.drag_slot = -1;
    for (int i = 0; i < 16; ++i) g_shared.swap_strategy_requested[i] = STRATEGY_NONE;
    for (int i = 0; i < 16; ++i) g_shared.manual_close_requested[i]  = 0;
    // v5.10.0c — hot model swap state. memset above already zeroed
    // these, but explicit init clarifies intent: 0 = no pending swap.
    for (int i = 0; i < 16; ++i) {
        g_shared.swap_model_path_requested[i] = 0;
        g_shared.pending_model_path[i][0]     = '\0';
    }
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

    // v4.7.39 (Phase C.2): Reset Paper coordination flag for per-core
    // slow-path threads. Producer's reset handler sets this before
    // touching shared state, slow-paths park on it, producer clears
    // when reset completes. Declared at EngineSharded_Run scope so BOTH
    // the producer thread lambda (which writes it during reset) AND the
    // per-core slow-path lambdas (which read it at top of poll loop)
    // can capture it by reference. No-op when engine_arch=centralized
    // (no slow-paths exist; flag is just unused).
    std::atomic<bool> paper_reset_in_progress{false};

    //----------------------------------------------------------------------
    // v5.0.2 (Engine Topology): function-scope arrays so the GUI publish
    // block (inside the producer thread lambda) can hand them to
    // TUI_PopulateTopology each tick. Hot-path CPUs are derived from the
    // pin convention (CPU i+1 for hot-path i); slow-path CPUs are filled
    // during the slow-path spawn loop further below; -1 = unpinned.
    //----------------------------------------------------------------------
    int  topo_hot_cpu[16];
    int  topo_slow_cpu[16];
    uint32_t topo_poll_interval[16];
    for (int i = 0; i < 16; ++i) {
        topo_hot_cpu[i]  = (i < num_cores) ? (i + 1) : -1;
        topo_slow_cpu[i] = -1;
        topo_poll_interval[i] = (i < num_cores)
            ? ControllerConfig_ResolveForCore(cfg, i).poll_interval
            : 0u;
    }

    //----------------------------------------------------------------------
    // v5.1.3 — fee-floor warning. A profitable round-trip needs gross
    // P&L > 2× fee_rate (entry + exit fees, both at taker rate worst-case).
    // We use 3× as the warning threshold so a winning trade clears fees
    // with at least 1× fee_rate of profit margin (otherwise rounding +
    // slippage erode the edge).
    //
    // Per-core resolved take_profit_pct must be >= 3 × fee_rate_taker.
    // Strategies that emit dynamic TP (e.g. EMA's stddev-based) can still
    // trip the runtime guard inside Strategy_BuildParameters even if cfg
    // looks fine here — boot warning catches static-cfg cases only.
    //----------------------------------------------------------------------
    {
        double fee_taker = FPN_ToDouble(cfg.fee_rate_taker);
        if (fee_taker <= 0.0) fee_taker = FPN_ToDouble(cfg.fee_rate);  // fallback
        double tp_floor = 3.0 * fee_taker;
        for (int i = 0; i < num_cores; ++i) {
            ControllerConfig<F> rc = ControllerConfig_ResolveForCore(cfg, i);
            double tp_pct = FPN_ToDouble(rc.take_profit_pct);
            if (tp_pct > 0.0 && tp_pct < tp_floor) {
                fprintf(stderr,
                    "[sharded] WARN: core %d take_profit_pct=%.4f%% is below "
                    "the fee floor (3 × taker=%.4f%% = %.4f%%). Winning trades "
                    "will be net-negative after fees. Recommend tp_pct >= %.4f%%.\n",
                    i, tp_pct * 100.0, fee_taker * 100.0,
                    tp_floor * 100.0, tp_floor * 100.0);
            }
        }
    }
    int  topo_producer_cpu = 0;
    int  topo_drainer_cpu  = num_cores + 1;
    long topo_nproc        = sysconf(_SC_NPROCESSORS_ONLN);
    if (topo_nproc < 1) topo_nproc = 1;

    //----------------------------------------------------------------------
    // Producer thread — generates synthetic ticks and fans out to all cores
    //----------------------------------------------------------------------
    std::thread producer([&producer_done, &ticks_produced, &bcfg, &last_price, &last_volume,
                          &cfg, &state, &oms, num_cores, use_synthetic, tsc_ghz,
                          &ema_price, &ema_alpha, live_trading,
                          &paper_reset_in_progress,
                          &topo_hot_cpu, &topo_slow_cpu, &topo_poll_interval,
                          topo_producer_cpu, topo_drainer_cpu, topo_nproc] {
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
                        &cfg, &state, &oms, &slow_path_counter, slow_path_interval, tsc_ghz,
                        &ema_price, ema_alpha, live_trading,
                        &paper_reset_in_progress,
                        &topo_hot_cpu, &topo_slow_cpu, &topo_poll_interval,
                        topo_producer_cpu, topo_drainer_cpu, topo_nproc]
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
            // v5.12.1.A.1+.2 — publish LOCAL wall-clock us of this tick to
            // EventLoopState::last_ws_tick_us. Producer is the SOLE writer;
            // slow-path threads + GUI read with acquire ordering.
            //
            // SEMANTICS REFINED in .A.2: was ts_us (Binance exchange time)
            // in the .A.1 commit; switched to local system_clock here so
            // EventLoop_CheckWsStaleness can compare against another local
            // clock read self-consistently (no NTP/skew dependency).
            // Backtest still uses tick.timestamp (synthetic, deterministic);
            // operator MUST keep cfg.ws_dead_time_flatten_enabled=0 in
            // backtest to avoid phantom-flatten under that mismatch.
            uint64_t local_now_us = (uint64_t)
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            state.last_ws_tick_us.store(local_now_us, std::memory_order_release);
            (void)ts_us;  // ts_us still used by other fan_out consumers

            // v5.1.4: GUI drag-TP/SL pickup runs per-tick, NOT at slow-path
            // cadence. Pre-v5.1.4 this lived in the cadence block and gave
            // 5-33s of perceived latency. Cost: one atomic_load + branch on
            // -1 per tick (~5ns on a modern x86). Branch is taken only on
            // the rare tick where a drag has just happened.
#ifdef USE_IMGUI_GUI
            {
                int slot = __atomic_load_n(&g_shared.drag_slot, __ATOMIC_ACQUIRE);
                if (slot >= 0 && slot < 16) {
                    int is_tp = g_shared.drag_is_tp;
                    double dprice = g_shared.drag_price;
                    __atomic_store_n(&g_shared.drag_slot, -1, __ATOMIC_RELEASE);
                    auto *pos = &state.oms->portfolio.positions[slot];
                    if (state.oms->portfolio.active_bitmap & (uint16_t)(1u << slot)) {
                        if (is_tp) pos->take_profit_price = FPN_FromDouble<F>(dprice);
                        else       pos->stop_loss_price   = FPN_FromDouble<F>(dprice);
                        fprintf(stderr, "[sharded] GUI drag: slot %d %s -> $%.2f\n",
                                slot, is_tp ? "TP" : "SL", dprice);
                    }
                }
            }
#endif

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
            // v5.1.2 (full symmetric decoupling): replicate ema_price to
            // ALL engines' slow_state in BOTH arches. Single-writer is
            // the producer thread. Cost: N FPN copies per tick — trivial.
            EventLoop_UpdateEmaPriceAllCores(&state, ema_price);

            // Phase 8a (post-coding c7) — record raw tick to CSV when enabled.
            // No-op when record_ticks=0 (the gate is inside TickRecorder_Push).
            // v5.1.2 architectural carry-forward — is_buyer_maker isn't available
            // on the sharded fan_out's scalar bus; producer + recorder + slow-path
            // all pass 0. (Legacy path passes the real value from BinanceStream
            // tick read.) Train-serve parity preserved (both broken the same way)
            // but FEAT_VOLUME_DELTA in slow-path RollingStats is locked at +1.0
            // → effectively zero-information. Full closure (~4h) plumbs through
            // a g_last_buyer_maker scalar; deferred to v5.10.X or v5.11+.
            // See plans/plan_checks/parity-2026-05-06-full.md Finding #5.
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
                // v5.1.2 (full symmetric decoupling): centralized arch
                // pushes per-cadence state into ALL N engines' slow_state
                // via the shared helper. per_core_slow lambdas do their
                // own pushes — skip here. Helper handles depth-history
                // pushes via depth_enabled flag (we pass FPN_Zero placeholders
                // for depth state when depth_enabled=0; the producer's main
                // slow-path body below handles depth-history symmetrically).
                if (cfg.engine_arch != ENGINE_ARCH_PER_CORE_SLOW) {
                    EventLoop_UpdateRollingStateAllCores(
                        &state, t.price, t.volume, ts_us,
                        is_buyer_maker,
                        FPN_Zero<F>(), FPN_Zero<F>(),
                        /*depth_enabled=*/0);  // depth-history pushed in slow-path body
                }
#ifdef USE_IMGUI_GUI
                // v4.0 hot-swap strategy: GUI requests are picked up here.
                // STRATEGY_NONE (0xFF) = no request; any other value swaps
                // the core's strategy. Open positions are honored — the swap
                // waits until the position closes naturally so the old
                // strategy's TP/SL still applies to its own entry.
                // v4.7.39 (Phase C.2): in per_core_slow mode each slow-path
                // thread handles its own swap-pending pickup. Producer skips.
                if (cfg.engine_arch != ENGINE_ARCH_PER_CORE_SLOW)
                for (int c = 0; c < num_cores && c < 16; ++c) {
                    uint8_t pending = __atomic_load_n(
                        &g_shared.swap_strategy_requested[c], __ATOMIC_ACQUIRE);
                    if (pending == STRATEGY_NONE) continue;
                    // v4.7.28: partials-aware open-position check. With
                    // partial_exit_enabled=1 each core owns 2 slots
                    // (leg A at 2c, leg B at 2c+1). Pre-v4.7.28 this
                    // checked bit `c` only — for Core 2 that's bit 2,
                    // which under partials is actually Core 1's leg A.
                    // If Core 1 had a position open, Core 2's swap would
                    // defer forever even though Core 2 has nothing open.
                    int partial_on = state.oms->partial_exit_enabled ? 1 : 0;
                    uint16_t open_mask = partial_on
                        ? (uint16_t)((1u << (c * 2)) | (1u << (c * 2 + 1)))
                        : (uint16_t)(1u << c);
                    if ((state.oms->portfolio.active_bitmap & open_mask) != 0) {
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

                // v5.1.4: drag-TP/SL pickup MOVED out of the slow-path cadence
                // block to fire every tick — see fan_out's tick-rate handler
                // at line ~870. Pre-v5.1.4 this lived here and added 5-33s of
                // latency on every TP/SL drag, plus dropped back-to-back drags
                // when the second one overwrote the slot before the engine
                // saw the first. Tick-rate pickup gives ~tens of ms latency
                // and reduces the back-to-back drop window proportionally.

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
                // v5.1.2: centralized arch pushes depth-history into ALL
                // N engines' slow_state. per_core_slow lambdas do their
                // own pushes. The "rolling" inputs are nullptr here —
                // we're only pushing depth fields; the helper short-
                // circuits if `price` is zero, so pass current price.
                if (cfg.depth_enabled &&
                    cfg.engine_arch != ENGINE_ARCH_PER_CORE_SLOW) {
                    for (int c = 0; c < state.registered_count; ++c) {
                        auto* sst = state.cores[c].slow_state;
                        if (!sst) continue;
                        BookImbHistory_Push(&sst->book_imb_history, book_imb);
                        SpreadState_Push(&sst->spread_state, book_spread);
                    }
                }
                // v5.1.2 (full symmetric decoupling): centralized arch
                // calls per-core RebuildAllParameters which reads each
                // engine's OWN slow_state. per_core_slow lambdas skip
                // (they call OneCore directly with their own pointers).
                if (cfg.engine_arch != ENGINE_ARCH_PER_CORE_SLOW)
                EventLoop_RebuildAllParameters_PerCore(&state, &cfg,
                    FPN_IsZero(mtm_price) ? nullptr : &mtm_price,
                    rebuild_ts_us,
                    cfg.depth_enabled ? &book_imb : nullptr,
                    FPN_ToDouble(book_spread),
                    FPN_ToDouble(book_mid));

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
                // v4.7.39 (Phase C.2): per_core_slow inlines the push inside
                // each slow-path thread (after RebuildOneCore). Producer skips.
                if (cfg.engine_arch != ENGINE_ARCH_PER_CORE_SLOW)
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
                // v4.7.39 (Phase C.2): in per_core_slow mode, each slow-path
                // grants its own permission. Producer skips.
                // v5.1.2: read per-core slow_state count (engine 0 — all
                // engines push at same cadence with same input, counts equal).
                if (cfg.engine_arch != ENGINE_ARCH_PER_CORE_SLOW &&
                    state.cores[0].slow_state &&
                    state.cores[0].slow_state->rolling_short.count >= (int)min_samples) {
                    for (int c = 0; c < num_cores; ++c) {
                        if (state.cores[c].strategy_id != STRATEGY_NONE) {
                            ExecutionCore_SetPermission(&cores[c], 1);
                        }
                    }
                }

                // v4.7.17: time-exit + trailing SL ratchet extracted to shared
                // helpers in ControllerEventLoop.hpp so backtest + live evolve
                // identically when these features are enabled. Pre-v4.7.17 both
                // were inlined here, leaving backtest silently no-op when user
                // set max_hold_ticks > 0 or tp_hold_score > 0 → train-serve drift.
                // v4.7.39 (Phase C.2): in per_core_slow mode, each slow-path
                // calls TimeExitOneCore + TrailingSLRatchetOneCore for its own
                // core. Producer skips the centralized iteration.
                // v5.1.2: TrailingSLRatchet still takes a single rolling
                // ref (legacy wrapper signature). For centralized arch we
                // pass engine 0's slow_state's rolling_short — all engines
                // have identical pushes so any is fine. Per-engine ratchet
                // reads its own state inside the OneCore call internally.
                if (cfg.engine_arch != ENGINE_ARCH_PER_CORE_SLOW)
                {
                    uint64_t now_tick      = ticks_produced.load(std::memory_order_relaxed);
                    double   current_price = last_price.load(std::memory_order_relaxed);
                    EventLoop_TimeExit(&state, state.oms, cfg, now_tick, current_price);
                    if (state.cores[0].slow_state) {
                        EventLoop_TrailingSLRatchet(&state, cfg,
                            state.cores[0].slow_state->rolling_short, current_price);
                    }
                    // v5.12.1.A.2 — WS-staleness emergency-flatten gate.
                    // Default cfg.ws_dead_time_flatten_enabled=0 → 5ns
                    // early return. Live deployment (=1) → ~150ns/cycle
                    // including vDSO clock_gettime; fires OMS_FlattenAll
                    // when producer goes silent for > threshold seconds.
                    // Centralized arch reads clock once for the gate;
                    // per_core_slow path (below) shares clock with the
                    // existing sp_last_tick_us update (~50ns saving).
                    uint64_t centralized_now_us = (uint64_t)
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                    EventLoop_CheckWsStaleness(&state, cfg, current_price,
                                                centralized_now_us);
                }

#ifdef USE_IMGUI_GUI
                // Populate TUISnapshot for the GUI — same double-buffered
                // pattern as legacy engine in main.cpp:845-912.
                {
                    // v5.11.3.B — seqlock publish: parity bit flips odd → fill
                    // back → flip even (idx toggled). Reader retries if mid-write.
                    auto pub = TUISnapshot_Publish_Begin(&g_shared);
                    TUISnapshot *bs = pub.back;
                    const TUISnapshot *fs = pub.front;
                    // carry graph history ring buffers from front buffer
                    memcpy(bs->price_history, fs->price_history, sizeof(bs->price_history));
                    memcpy(bs->volume_history, fs->volume_history, sizeof(bs->volume_history));
                    memcpy(bs->pnl_history, fs->pnl_history, sizeof(bs->pnl_history));
                    bs->graph_head = fs->graph_head;
                    bs->graph_count = fs->graph_count;
                    // populate from sharded state
                    // v5.1.2: TUI snapshot reads engine 0's slow_state since
                    // all engines have identical pushes (same input/cadence).
                    auto* sst0 = state.cores[0].slow_state;
                    if (sst0) {
                        TUI_CopySnapshotSharded(bs, &state, &sst0->rolling_short,
                                                 &sst0->rolling_long, &cfg, price_d, volume_d);
                    }
                    TUI_PopulatePerCoreLatency(bs, cores, num_cores, tsc_ghz);
                    // v5.0.1 (Phase H): slow-path latency from CoreContext.
                    TUI_PopulatePerCoreSlowPathLatency(bs, &state, tsc_ghz);
                    // v5.0.2 (Phase H): topology — system + thread layout.
                    TUI_PopulateTopology(bs, cfg.engine_arch,
                                          topo_producer_cpu, topo_drainer_cpu,
                                          (int)topo_nproc,
                                          cfg.slow_path_pin_offset,
                                          topo_hot_cpu, topo_slow_cpu,
                                          topo_poll_interval);
                    // v5.0.3 (Engine Topology advanced): live thread state.
                    TUI_PopulateAdvancedTopology(bs, &state, &oms);
                    // v4.7.18: paper-reset seq for history-clearing panels
                    bs->paper_reset_seq = (uint32_t)g_shared.paper_reset_seq;
                    // v5.11.4.B — health log WARN on first non-zero observation
                    // of async log writer trouble (parity-check Section J).
                    // One-shot per process: the moment ring_full_spins or
                    // writer_realloc_failed_count cross zero, emit a single
                    // WARN line. Operator can grep the health log for the
                    // string to detect silent writer-thread distress.
                    {
                        static uint64_t prev_ring_full = 0;
                        static uint64_t prev_realloc_failed = 0;
                        if (bs->oms_log_ring_full_spins != prev_ring_full) {
                            if (prev_ring_full == 0) {
                                tt::Health_Log(tt::HEALTH_WARN, "oms_log", -1,
                                    "async writer ring saturation: ring_full_spins=%llu "
                                    "(drainer is spin-waiting for writer to drain; "
                                    "investigate disk health or bump ring size)",
                                    (unsigned long long)bs->oms_log_ring_full_spins);
                            }
                            prev_ring_full = bs->oms_log_ring_full_spins;
                        }
                        if (bs->oms_log_writer_realloc_failed != prev_realloc_failed) {
                            if (prev_realloc_failed == 0) {
                                tt::Health_Log(tt::HEALTH_CRITICAL, "oms_log", -1,
                                    "async writer realloc OOM: writer_realloc_failed=%llu "
                                    "(events may be dropped; entries[] cannot grow)",
                                    (unsigned long long)bs->oms_log_writer_realloc_failed);
                            }
                            prev_realloc_failed = bs->oms_log_writer_realloc_failed;
                        }
                        // v5.11.5.D — log_full_drops first-non-zero WARN
                        // (parity-check J.1). Distinct from ring_full_spins:
                        // log_full_drops fires when the mmap'd entries[]
                        // buffer is saturated; events have been DROPPED.
                        // Operator may need to bump
                        // ORDER_EVENT_LOG_MAX_CAPACITY or run an offline
                        // log-rotation step.
                        static uint64_t prev_log_drops = 0;
                        if (bs->oms_log_full_drops != prev_log_drops) {
                            if (prev_log_drops == 0) {
                                tt::Health_Log(tt::HEALTH_CRITICAL, "oms_log", -1,
                                    "event log capacity exhausted: log_full_drops=%llu "
                                    "(events DROPPED; bump ORDER_EVENT_LOG_MAX_CAPACITY "
                                    "or rotate the log)",
                                    (unsigned long long)bs->oms_log_full_drops);
                            }
                            prev_log_drops = bs->oms_log_full_drops;
                        }
                    }
                    // append current data point to graph ring buffers
                    bs->price_history[bs->graph_head] = bs->price;
                    bs->volume_history[bs->graph_head] = bs->volume;
                    bs->pnl_history[bs->graph_head] = bs->total_pnl;
                    bs->graph_head = (bs->graph_head + 1) % TUISnapshot::GRAPH_LEN;
                    if (bs->graph_count < TUISnapshot::GRAPH_LEN) bs->graph_count++;
                    TUISnapshot_Publish_End(&g_shared);  // v5.11.3.B — flips parity even, idx toggled
                }
                // check GUI quit request
                if (g_shared.quit_requested) {
                    g_engine_sharded_shutdown = 1;
                }
                // paper reset: zero balance, clear positions, reset counters
                if (g_shared.paper_reset_requested && !cfg.use_real_money) {
                    g_shared.paper_reset_requested = 0;
                    // v4.7.39 (Phase C.2): coordinate Reset Paper with per-core
                    // slow-path threads. Set the in-progress flag → slow-paths
                    // park (yield) at top of their loop. After reset completes,
                    // clear the flag → slow-paths resume with fresh state.
                    // No-op when engine_arch=centralized (slow-paths don't
                    // exist; the flag is just unused).
                    paper_reset_in_progress.store(true, std::memory_order_release);
                    // Brief yield to let slow-paths observe the flag and park.
                    // Worst case they don't yet — shared state writes below
                    // proceed concurrently, slow-path's next read sees fresh
                    // values (eventually consistent acceptable for slow-path).
                    std::this_thread::yield();
                    state.oms->balance      = cfg.starting_balance;
                    state.oms->realized_pnl = FPN_Zero<F>();
                    state.oms->ks_peak_balance = cfg.starting_balance;
                    state.oms->kill_switch_tripped = 0;
                    // v5.5.6 (recurring-bugs Class 5): OMS counters added in
                    // Phase 8 (maker/taker breakdown) + v5.4.4 (snapshot
                    // persistence) were never wired into Reset Paper. Result:
                    // Account header showed cumulative fees across resets
                    // (e.g. $93 with realized=$-3.18 from one trade), and
                    // gross = realized + unrealized + fees gave nonsense
                    // numbers. User reported on 2026-04-30. Now zeroed
                    // alongside balance + realized_pnl so all session
                    // counters are scoped consistently.
                    state.oms->total_fees          = FPN_Zero<F>();
                    state.oms->total_maker_fees    = FPN_Zero<F>();
                    state.oms->total_taker_fees    = FPN_Zero<F>();
                    state.oms->maker_fills_count   = 0;
                    state.oms->taker_fills_count   = 0;
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
                        // v4.7.26: clear v4.7.21 pairing state. Without this,
                        // leg A closed pre-reset stays stashed in
                        // partner_pending_pnl; leg B closes post-reset and
                        // pairs against the stale stash → ghost loss bumps
                        // core_losses without a corresponding fill, leaving
                        // the panel showing W:0 L:N with 0-fills counter.
                        state.cores[c].partner_pending_pnl    = FPN_Zero<F>();
                        state.cores[c].partner_pending_active = 0;
                        // v4.7.26: clear v4.7.25 gross accumulators. Without
                        // this, post-reset avg W/L can read stale gross
                        // values divided by the freshly-zero W/L counters
                        // → divide-by-zero hides the issue, but a single
                        // post-reset trade then produces a misleading mean.
                        state.cores[c].core_gross_wins   = FPN_Zero<F>();
                        state.cores[c].core_gross_losses = FPN_Zero<F>();
                        // v5.4.3 (recurring-bugs Class 5): also clear
                        // sl_cooldown_remaining + idle_cycles. Without
                        // these, a pre-reset SL exit leaves the core
                        // zero-gated post-reset (halt_reason=6) for N
                        // ticks user thinks fresh. idle_cycles same
                        // pattern — death-spiral counter shouldn't
                        // carry pre-reset state.
                        state.cores[c].sl_cooldown_remaining = 0;
                        state.cores[c].idle_cycles           = 0;
                    }
                    // v4.7.18: rotate the trade history CSV to a timestamped
                    // backup so the GUI's Trade History panel goes blank
                    // instead of mixing pre-reset rows with new ones.
                    if (state.oms->trade_log) {
                        ShardedTradeLog_Rotate(state.oms->trade_log);
                    }
                    // v4.7.18: also truncate the OMS event log on disk so
                    // the next engine restart doesn't replay 40+ zombie
                    // events from the prior session into the fresh state.
                    OrderEventLog_Reset(&state.oms->event_log);
                    // v4.7.18: bump the reset sequence counter so retained-
                    // history GUI panels (Per-Core P&L ring buffer, equity
                    // curve, etc.) can clear themselves.
                    g_shared.paper_reset_seq++;
                    fprintf(stderr, "[sharded] paper reset: balance=$%.2f "
                                    "(seq=%u, trade log + event log rotated)\n",
                            FPN_ToDouble(cfg.starting_balance),
                            (unsigned)g_shared.paper_reset_seq);
                    // v4.7.39 (Phase C.2): reset complete; release slow-path
                    // threads. They were parked on this flag — next loop
                    // iteration sees fresh state.
                    paper_reset_in_progress.store(false, std::memory_order_release);
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
                    // v4.7.32: read partial_exit_pct from per-core override
                    // when set (0 = inherit). Pre-fix it always read global,
                    // making the per-core override a silent no-op.
                    double full_qty = FPN_ToDouble(state.cores[slot].intended_qty);
                    const auto& ov = cfg.core_overrides[slot];
                    FPN<F> partial_pct = !FPN_IsZero(ov.partial_exit_pct)
                        ? ov.partial_exit_pct : cfg.partial_exit_pct;
                    if (partial_on && event.leg == PARTIAL_LEG_A) {
                        order_qty_d = full_qty * FPN_ToDouble(partial_pct);
                    } else if (partial_on && event.leg == PARTIAL_LEG_B) {
                        order_qty_d = full_qty * (1.0 - FPN_ToDouble(partial_pct));
                    } else {
                        order_qty_d = full_qty;
                    }
                }

                EventLoop_OnEvent(&state, event);
                ++total_drained;

                if ((is_entry || is_exit) && order_qty_d > 0.0) {
                    // v4.7.2: leg B's intended_tp must be TP2, not TP1.
                    // intended_tp on the core is leg A's absolute TP. For
                    // leg B, scale the TP-distance by cfg.tp2_mult — same
                    // computation the hot path does for live_tp_b, just
                    // expressed in absolute price form so Position.tp +
                    // snapshot persistence reflect the actual TP2. Keeps
                    // the panel display honest AND prevents
                    // snapshot-restore-while-paired from reviving leg B
                    // with TP1 instead of TP2.
                    FPN<F> leg_tp = state.cores[slot].intended_tp;
                    if (is_entry && partial_on && event.leg == PARTIAL_LEG_B) {
                        FPN<F> tp_dist_a = FPN_Sub(state.cores[slot].intended_tp, event.price);
                        // v4.7.32: per-core tp2_mult override (0 = inherit).
                        const auto& ov_tp2 = cfg.core_overrides[slot];
                        FPN<F> tp2_mult_eff = !FPN_IsZero(ov_tp2.tp2_mult)
                            ? ov_tp2.tp2_mult : cfg.tp2_mult;
                        FPN<F> tp_dist_b = FPN_Mul(tp_dist_a, tp2_mult_eff);
                        leg_tp = FPN_Add(event.price, tp_dist_b);
                    }
                    // v4.7.37 (Phase B reordered): push through OMS_PushSubmit
                    // instead of calling Submit directly. Drainer drains the
                    // queue + calls Submit serially — preserves OMS single-
                    // caller contract for when Phase C spawns N producers.
                    OMS_PushSubmit(&oms,
                        (int16_t)portfolio_slot,  // P.3: actual slot, not core_id
                        is_entry ? ORDER_MARKET_BUY : ORDER_MARKET_SELL,
                        FPN_FromDouble<F>(order_qty_d),
                        leg_tp,
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
                        // v4.7.6: wall-clock entry time so GUI's "Hold"
                        // column can show real elapsed minutes for open
                        // positions. Microseconds since epoch — divide
                        // by 60_000_000 in the snapshot copy for minutes.
                        state.cores[slot].last_entry_wall_us = (uint64_t)
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
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
        EventLoop_DrainPostFill(&state, &oms, cfg.sl_cooldown_cycles,
                                 cfg.ensemble_trade_reward_mult,
                                 cfg.confidence_ic_floor,
                                 cfg.confidence_ic_floor_window,
                                 cfg.auto_kill_on_drift);
    };

    // v4.7.8: manual force-close requests from the GUI. User clicks a
    // button on the Positions panel → GUI sets manual_close_requested[slot]=1
    // → drainer reads + emits a synthetic SELL via OrderManager_Submit
    // bypassing the hot-path SG. Race with the hot path (ExecutionCore
    // could fire a real SL on the same slot in the same window) is
    // tolerated: HandleFill checks the bitmap before CloseSlot, so a
    // double-close becomes a no-op on the second attempt. Slow-path only.
    //
    // GUI-only: g_shared lives in the GUI build's #ifdef USE_IMGUI_GUI
    // block. ANSI TUI / headless builds compile a no-op lambda so the
    // drainer thread doesn't have to fork on build flags.
#ifdef USE_IMGUI_GUI
    TUISharedState* shared_ptr = &g_shared;
    auto drain_manual_closes = [&state, &oms, &cfg, &last_price, shared_ptr]() {
        for (int slot = 0; slot < MAX_PORTFOLIO_POSITIONS; ++slot) {
            if (!shared_ptr->manual_close_requested[slot]) continue;
            shared_ptr->manual_close_requested[slot] = 0;
            // Skip if no open position at this slot — defensive against
            // double-clicks or races with auto-close.
            if ((oms.portfolio.active_bitmap & (uint16_t)(1u << slot)) == 0) {
                std::fprintf(stderr,
                    "[manual-close] slot %d: no active position, ignoring\n", slot);
                continue;
            }
            FPN<F> qty = oms.portfolio.positions[slot].quantity;
            if (FPN_IsZero(qty)) continue;
            // Map slot → core_id for strategy_id + leg lookup
            int partial_on = cfg.partial_exit_enabled ? 1 : 0;
            int core_id = partial_on ? (slot >> 1) : slot;
            int leg     = partial_on ? (slot & 1)  : 0;
            if (core_id < 0 || core_id >= state.registered_count) continue;
            uint8_t strategy_id = state.cores[core_id].strategy_id;
            // Use latest tick price as fill price for paper mode. Live
            // mode would route to a real adapter SELL — same Submit call.
            FPN<F> fill_px = FPN_FromDouble<F>(
                last_price.load(std::memory_order_relaxed));
            if (FPN_IsZero(fill_px)) {
                fill_px = oms.portfolio.positions[slot].entry_price;  // safe fallback
            }
            // v4.7.37 (Phase B reordered): push through OMS_PushSubmit so
            // the drainer thread serializes Submit calls. Manual close is a
            // GUI-driven event; without funneling, this site races with
            // other producer-thread Submits when Phase C spawns multiple.
            OMS_PushSubmit(&oms,
                (int16_t)slot, ORDER_MARKET_SELL,
                qty,
                FPN_Zero<F>(), FPN_Zero<F>(),
                strategy_id,
                fill_px,
                (uint8_t)leg);
            // v4.7.19: counter bumps moved to EventLoop_DrainPostFill —
            // see the doctrine note there. Pre-v4.7.19 we bumped here
            // BEFORE Submit could fail (queue full, slot already closed,
            // etc.), causing 7-vs-5 counter-vs-CSV drift. Now bumps fire
            // exactly when HandleFill writes a CSV row.
            // Clear the matching ExecutionCore active flag so the hot
            // path doesn't re-emit on the next tick (race-tolerant —
            // worst case is one duplicate exit event that HandleFill
            // dedups via the empty-slot bitmap check).
            if (state.cores[core_id].core) {
                if (leg == 0) state.cores[core_id].core->active   = 0;
                else          state.cores[core_id].core->active_b = 0;
            }
            std::fprintf(stderr,
                "[manual-close] slot %d (core %d leg %s): force-exit @ %.2f, qty %.6f\n",
                slot, core_id, leg == 0 ? "A" : "B",
                FPN_ToDouble(fill_px), FPN_ToDouble(qty));
        }
    };
#else
    // ANSI / headless build: no GUI → no manual close requests possible.
    // No-op lambda so the drainer loop doesn't need build-flag forks.
    auto drain_manual_closes = []() {};
#endif

    std::thread drainer([&state, &oms, &producer_done, &drain_with_submit,
                         &drain_post_fill, &drain_manual_closes] {
        EngineSharded_PinThread(state.registered_count + 1);
        while (!g_engine_sharded_shutdown) {
            // Sequence per cycle:
            //   1. drain_with_submit / drain_manual_closes — these push
            //      SubmitCommands into oms.submit_queues (v4.7.37; was direct
            //      OrderManager_Submit calls before Phase B).
            //   2. OMS_DrainSubmit — drainer (sole Submit caller) pops the
            //      queues and calls Submit serially. Preserves OMS contract.
            //   3. OrderManager_Tick — drains result_queue / ws_result_queue
            //      / reconcile_queue, calls HandleFill on completed orders.
            //   4. drain_post_fill — applies per-core CoreContext updates
            //      from FillRecords.
            int total_drained = drain_with_submit();
            drain_manual_closes();
            // v5.4.1 Bug B2: under partials, ExecutionCore producers push
            // SubmitCommands keyed by portfolio_slot (0..2N-1, where N =
            // num_execution_cores). DrainSubmit must walk all queues that
            // may have been written, not just queues 0..N-1. Pre-fix,
            // cores beyond num_cores under partials had their submits
            // stuck in undrained queues forever — silent zero-trade state.
            int drain_count = oms.partial_exit_enabled
                ? state.registered_count * 2 : state.registered_count;
            OMS_DrainSubmit(&oms, drain_count);  // v4.7.37
            OrderManager_Tick(&oms);
            drain_post_fill();

            if (total_drained == 0) std::this_thread::yield();
            if (producer_done.load(std::memory_order_acquire)) {
                for (int k = 0; k < 16; ++k) {
                    drain_with_submit();
                    drain_manual_closes();
                    // v5.4.1 Bug B2: same partials-aware drain count as the
                    // main loop above.
                    int dc = oms.partial_exit_enabled
                        ? state.registered_count * 2 : state.registered_count;
                    OMS_DrainSubmit(&oms, dc);  // v4.7.37
                    OrderManager_Tick(&oms);
                    drain_post_fill();
                }
                break;
            }
        }
    });

    // v4.7.39 (Phase C.2 of per-core slow-path migration): when
    // engine_arch=per_core_slow, spawn N per-core slow-path threads. Each
    // runs OneCore helpers (RebuildOneCore, TimeExitOneCore,
    // TrailingSLRatchetOneCore) on a fixed cadence. Producer continues
    // doing GLOBAL work (RollingStats pushes, depth state, snapshot save,
    // GUI publish, account-level KillSwitchEvaluate). Per-core sections
    // in producer's slow-path body are gated on engine_arch.
    //
    // v5.0.2: slow-path pinning. cfg.slow_path_pin_offset:
    //   < 0  → no pin (OS-scheduled, original v5.0 behavior)
    //   == 0 → auto: base = drainer_cpu + 1 = num_cores + 2
    //   > 0  → explicit base
    // Slow-path c pins to (base + c) mod nproc. HT-sharing acceptable
    // (slow-path is jitter-tolerant). Best-effort: pin failure is logged
    // and execution continues unpinned.
    //
    // Reset Paper coordination: paper_reset_in_progress atomic declared
    // earlier (~line 786) so fan_out lambda's reset handler can reference
    // it. Slow-paths park (yield) when set; producer's reset handler sets,
    // runs reset, clears.
    std::vector<std::thread> slow_paths;
    if (cfg.engine_arch == ENGINE_ARCH_PER_CORE_SLOW) {
        // v5.1.5: smart pin selection. With cfg.slow_path_pin_offset == 0
        // (auto), use SmartSlowPathPins to AVOID landing on SMT siblings of
        // producer/hot-path/drainer CPUs. Pre-v5.1.5 the auto-derive was a
        // simple (num_cores + 2 + c) % nproc, which on a 16-thread Intel
        // (8 phys × 2 SMT) put slow-paths 2,3 on CPUs 8,9 = SMT siblings of
        // producer/hot-path-0 → 2× slowdown vs slow-paths 0,1 on idle
        // physical cores 6,7. Smart logic prefers truly-idle CPUs first,
        // then SMT siblings of idle cores, then falls back gracefully.
        int sp_pins[16];
        long nproc = topo_nproc;
        bool smart_ok = false;
        int sp_pin_base = -1;  // <0 = no pin
        if (cfg.slow_path_pin_offset == 0) {
            smart_ok = EngineSharded_SmartSlowPathPins(
                /*producer_cpu=*/topo_producer_cpu,
                /*drainer_cpu=*/topo_drainer_cpu,
                /*num_hot=*/num_cores,
                /*num_slow=*/num_cores,
                sp_pins);
            if (smart_ok) {
                sp_pin_base = sp_pins[0];  // for log only
                fprintf(stderr, "[sharded] engine_arch=per_core_slow: spawning %d "
                                "slow-path threads (smart pin: ", num_cores);
                for (int c = 0; c < num_cores; ++c) {
                    fprintf(stderr, "%s%d", c ? "," : "", sp_pins[c]);
                }
                fprintf(stderr, ", nproc %ld)\n", nproc);
            } else {
                // Fallback: simple (num_cores + 2 + c) % nproc
                sp_pin_base = num_cores + 2;
                fprintf(stderr, "[sharded] engine_arch=per_core_slow: spawning %d "
                                "slow-path threads (smart pin failed; fallback base CPU %d, nproc %ld)\n",
                        num_cores, sp_pin_base, nproc);
            }
        } else if (cfg.slow_path_pin_offset > 0) {
            sp_pin_base = cfg.slow_path_pin_offset;
            fprintf(stderr, "[sharded] engine_arch=per_core_slow: spawning %d "
                            "slow-path threads (explicit pin base CPU %d, nproc %ld)\n",
                    num_cores, sp_pin_base, nproc);
        } else {
            // < 0: no pin
            fprintf(stderr, "[sharded] engine_arch=per_core_slow: spawning %d "
                            "slow-path threads (UNPINNED, slow_path_pin_offset=%d)\n",
                    num_cores, cfg.slow_path_pin_offset);
        }
        slow_paths.reserve(num_cores);
        for (int c = 0; c < num_cores; ++c) {
            int sp_cpu;
            if (smart_ok) {
                sp_cpu = sp_pins[c];
            } else if (sp_pin_base >= 0) {
                sp_cpu = (int)((sp_pin_base + c) % nproc);
            } else {
                sp_cpu = -1;
            }
            topo_slow_cpu[c] = sp_cpu;  // v5.0.2: capture for topology panel
            slow_paths.emplace_back([c, sp_cpu, &state, &oms, &cores, &cfg,
                                      &ticks_produced, &last_price, &last_volume,
                                      &paper_reset_in_progress]() {
                // v5.0.2: best-effort pin to chosen CPU. Failure logged,
                // execution continues unpinned.
                if (sp_cpu >= 0) {
                    EngineSharded_PinThread(sp_cpu);
                }
                // v4.7.40 (Phase D): per-core poll interval from resolved
                // cfg. If core has core_N_poll_interval set, use that;
                // otherwise inherit global cfg.poll_interval.
                ControllerConfig<F> resolved_init =
                    ControllerConfig_ResolveForCore(cfg, c);
                int slow_path_interval = (int)resolved_init.poll_interval;
                if (slow_path_interval < 1) slow_path_interval = 100;
                fprintf(stderr,
                    "[slow-path-%d] poll_interval=%d ticks (override=%u, global=%u) "
                    "pinned_cpu=%d\n",
                    c, slow_path_interval,
                    (unsigned)cfg.core_overrides[c].poll_interval,
                    (unsigned)cfg.poll_interval,
                    sp_cpu);
                // v4.7.42 (Phase E): enable per-core slow-path latency stats.
                // Sampled around the per-cycle work below (RebuildOneCore +
                // PushParameters + TimeExitOneCore + TrailingSL + permission).
                CoreLatencyStats_Enable(&state.cores[c].slow_path_latency);
                // v5.1.1: enable per-section breakdown stats.
                for (int s = 0; s < CoreContext<F>::SP_SECTION_COUNT; ++s) {
                    CoreLatencyStats_Enable(&state.cores[c].slow_path_breakdown[s]);
                }
                uint64_t last_seen_tick = 0;
                while (!g_engine_sharded_shutdown) {
                    // v5.0.3: user pause via paused_engines_mask bit c.
#ifdef USE_IMGUI_GUI
                    if (g_shared.paused_engines_mask & (uint16_t)(1u << c)) {
                        state.cores[c].sp_state.store(3, std::memory_order_relaxed);
                        state.cores[c].sp_yield_count.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                        continue;
                    }
#endif
                    // Reset Paper coordination — park while reset runs.
                    if (paper_reset_in_progress.load(std::memory_order_acquire)) {
                        state.cores[c].sp_state.store(1, std::memory_order_relaxed);
                        state.cores[c].sp_yield_count.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                        continue;
                    }
                    // Cadence — wake when enough ticks have passed.
                    uint64_t now_tick = ticks_produced.load(std::memory_order_acquire);
                    if (now_tick - last_seen_tick < (uint64_t)slow_path_interval) {
                        state.cores[c].sp_state.store(2, std::memory_order_relaxed);
                        state.cores[c].sp_yield_count.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                        continue;
                    }
                    last_seen_tick = now_tick;
                    state.cores[c].sp_state.store(0, std::memory_order_relaxed);  // running

                    // v4.7.42 (Phase E): rdtsc-bracket the per-cycle work for
                    // slow-path latency stats. Sample after work completes.
                    uint64_t _sp_t0 = __rdtsc();
                    // v5.1.1: per-section breakdown — section start markers.
                    // Each `_sec_t*` captures rdtsc between sections; the
                    // delta becomes that section's sample.
                    uint64_t _sec_t_other_start = _sp_t0;

                    // Skip cores with STRATEGY_NONE (caller responsibility per
                    // OneCore contract; OneCore would no-op on STRATEGY_NONE
                    // body because Strategy_BuildParameters dispatcher skips it,
                    // but the explicit check here is cheap and clarifying).
                    if (state.cores[c].strategy_id == STRATEGY_NONE) continue;

                    // === Per-core swap-pending pickup ===
                    // Mirrors the producer-thread swap walker — but checks only
                    // THIS core's request slot. Race-safe: __atomic_load gives
                    // acquire on g_shared.swap_strategy_requested[c].
#ifdef USE_IMGUI_GUI
                    {
                        uint8_t pending = __atomic_load_n(
                            &g_shared.swap_strategy_requested[c],
                            __ATOMIC_ACQUIRE);
                        if (pending != STRATEGY_NONE) {
                            int partial_on = state.oms->partial_exit_enabled ? 1 : 0;
                            uint16_t open_mask = partial_on
                                ? (uint16_t)((1u << (c * 2)) | (1u << (c * 2 + 1)))
                                : (uint16_t)(1u << c);
                            if ((state.oms->portfolio.active_bitmap & open_mask) == 0) {
                                if (pending == STRATEGY_ML &&
                                    state.cores[c].model_handle == NULL) {
                                    fprintf(stderr,
                                        "[slow-path-%d] refusing swap to ML — "
                                        "no model loaded\n", c);
                                    __atomic_store_n(
                                        &g_shared.swap_strategy_requested[c],
                                        STRATEGY_NONE, __ATOMIC_RELEASE);
                                } else {
                                    uint8_t old_strat = state.cores[c].strategy_id;
                                    state.cores[c].strategy_id = pending;
                                    __atomic_store_n(
                                        &g_shared.swap_strategy_requested[c],
                                        STRATEGY_NONE, __ATOMIC_RELEASE);
                                    fprintf(stderr,
                                        "[slow-path-%d] strategy swapped %u -> %u\n",
                                        c, (unsigned)old_strat, (unsigned)pending);
                                }
                            }
                            // else: position open; leave pending — try next cycle
                        }
                    }
                    // === v5.10.0c — Per-core model hot-swap pickup ===
                    // Mirrors the strategy hot-swap pattern above, but the
                    // payload is a directory path (string) instead of a
                    // strategy id (uint8). On request: free + reinit + reload
                    // ml_zoos[c] from the new dir; update model_handle on
                    // success, NULL it on failure (engine falls back to
                    // SimpleDip via ML_BuildParameters dispatcher).
                    {
                        uint8_t mswap = __atomic_load_n(
                            &g_shared.swap_model_path_requested[c],
                            __ATOMIC_ACQUIRE);
                        if (mswap) {
                            int partial_on = state.oms->partial_exit_enabled ? 1 : 0;
                            uint16_t open_mask = partial_on
                                ? (uint16_t)((1u << (c * 2)) | (1u << (c * 2 + 1)))
                                : (uint16_t)(1u << c);
                            int has_open = (state.oms->portfolio.active_bitmap & open_mask) != 0;

                            if (has_open && !cfg.acknowledge_hot_swap_with_open_positions) {
                                // Defer: leave flag set, retry next slow-path cycle.
                                // Operator opts in via cfg field if they want
                                // entry+exit on different models.
                            } else {
                                char new_path[256];
                                strncpy(new_path, g_shared.pending_model_path[c], 255);
                                new_path[255] = '\0';

                                if (new_path[0] == '\0') {
                                    fprintf(stderr,
                                        "[hot_swap] core %d REFUSED: empty path\n", c);
                                    __atomic_store_n(
                                        &g_shared.swap_model_path_requested[c], 0,
                                        __ATOMIC_RELEASE);
                                } else {
                                    // Cast model_handle back to the typed zoo
                                    // pointer set at boot (line ~805). NULL
                                    // means the core wasn't STRATEGY_ML at
                                    // boot, so no zoo storage was allocated;
                                    // hot-swap requires operator pre-config.
                                    CoreModelZoo<F>* swap_zoo =
                                        (CoreModelZoo<F>*)state.cores[c].model_handle;
                                    if (swap_zoo == nullptr) {
                                        fprintf(stderr,
                                            "[hot_swap] core %d REFUSED: "
                                            "core not ML at boot (set "
                                            "core_%d_strategy=ml + restart "
                                            "to enable hot-swap)\n", c, c);
                                        __atomic_store_n(
                                            &g_shared.swap_model_path_requested[c], 0,
                                            __ATOMIC_RELEASE);
                                    } else if (state.cores[c].ensemble_handle != nullptr) {
                                        // v5.10.2.B — REFUSE hot swap when ensemble
                                        // is active (parity-check Finding #4).
                                        // Dispatcher reads ensemble_zoo first
                                        // (StrategyParameters.hpp:794); a single-zoo
                                        // swap would be a silent no-op for actual
                                        // inference. Operator must restart engine
                                        // to swap the horizon set.
                                        // Full ensemble swap (Free + Init + AutoDetect
                                        // + bandit reload) is Option B, deferred to
                                        // v5.10.2.X if operator wants it.
                                        fprintf(stderr,
                                            "[hot_swap] core %d REFUSED: ensemble inference "
                                            "active; swap of single-zoo model would not "
                                            "affect actual predictions. Restart engine "
                                            "with new core_%d_model_dir to swap horizon set.\n",
                                            c, c);
                                        __atomic_store_n(
                                            &g_shared.swap_model_path_requested[c], 0,
                                            __ATOMIC_RELEASE);
                                    } else {
                                        int swap_backend = cfg.ml_backend
                                            ? cfg.ml_backend
                                            : MODEL_BACKEND_XGBOOST;
                                        // Free old + reinit + reload. Same-thread
                                        // (slow-path c is single-reader/writer for
                                        // its zoo); brief window with empty zoo
                                        // is safe — ML inference also runs on this
                                        // same slow-path thread, can't preempt itself.
                                        CoreModelZoo_Free(swap_zoo);
                                        CoreModelZoo_Init(swap_zoo);
                                        int loaded = CoreModelZoo_LoadFromDir(
                                            swap_zoo, new_path, swap_backend,
                                            /*secret=*/nullptr, /*gap=*/0.05,
                                            /*strict=*/cfg.held_out_gate_strict,
                                            cfg.acknowledge_cross_binary_version_drift);
                                        if (loaded > 0) {
                                            state.cores[c].model_load_failed = 0;
                                            fprintf(stderr,
                                                "[hot_swap] core %d swapped to %s "
                                                "(%d roles loaded)\n",
                                                c, new_path, loaded);
                                            // v5.10.2.A — Run post-load validator
                                            // on the newly-swapped zoo (parity-check
                                            // Finding #3 closure: hot swap was
                                            // bypassing inference_cfg drift detection
                                            // + xgb_hyperparams WARN at boot).
                                            // Hot-swap context: log-and-leave on
                                            // Tier 1 REFUSE; operator manually reverts
                                            // via cfg+restart (true rollback deferred
                                            // — pre-swap state isn't snapshotted).
                                            int validate_rc = CoreModelZoo_ValidateAgainstCfg<F>(
                                                swap_zoo,
                                                /*ezoo=*/nullptr,  // single-zoo only here; ensemble swap REFUSED in B
                                                cfg, /*core_id=*/c,
                                                cfg.held_out_gate_strict,
                                                cfg.acknowledge_inference_cfg_drift,
                                                cfg.acknowledge_cross_binary_version_drift,
                                                &state.cores[c]);
                                            if (validate_rc < 0) {
                                                state.cores[c].model_load_failed = 1;
                                                fprintf(stderr,
                                                    "[hot_swap] core %d REFUSED post-load "
                                                    "validation in strict mode; new model "
                                                    "loaded but flagged degraded. Operator "
                                                    "must reconcile cfg vs stamp + restart.\n", c);
                                            }
                                        } else {
                                            // Load failed; null the handle so
                                            // dispatcher falls back to SimpleDip.
                                            state.cores[c].model_handle = NULL;
                                            state.cores[c].model_load_failed = 1;
                                            fprintf(stderr,
                                                "[hot_swap] core %d REFUSED: "
                                                "no roles loaded from %s\n",
                                                c, new_path);
                                        }
                                        __atomic_store_n(
                                            &g_shared.swap_model_path_requested[c], 0,
                                            __ATOMIC_RELEASE);
                                    }
                                }
                            }
                        }
                    }
#endif
                    // === Read shared market state (eventually-consistent) ===
                    // Producer is single writer; slow-paths read with relaxed
                    // ordering. Stale-by-poll-interval is acceptable for
                    // slow-path strategy dispatch (always was — pre-migration
                    // producer's slow-path also operated on whatever rolling
                    // values were current at slow-path entry).
                    FPN<F> book_imb = FPN_Zero<F>();
                    double book_spread_d = 0.0, book_mid_d = 0.0;
                    if (cfg.depth_enabled) {
                        int dactive = __atomic_load_n(&g_depth_shared.active_idx,
                                                       __ATOMIC_ACQUIRE);
                        book_imb     = g_depth_shared.snapshots[dactive].imbalance;
                        book_spread_d = FPN_ToDouble(g_depth_shared.snapshots[dactive].spread);
                        book_mid_d    = FPN_ToDouble(g_depth_shared.snapshots[dactive].mid_price);
                    }

                    // Pre-loop scalar (matches RebuildAllParameters wrapper).
                    int book_imbalance_blocked = 0;
                    if (cfg.depth_enabled && !FPN_IsZero(cfg.min_book_imbalance)) {
                        book_imbalance_blocked = FPN_LessThan(book_imb,
                            cfg.min_book_imbalance) ? 1 : 0;
                    }

                    // mtm_price for MTM kill switch (in RebuildOneCore body).
                    double price_d = last_price.load(std::memory_order_relaxed);
                    double volume_d = last_volume.load(std::memory_order_relaxed);
                    FPN<F> mtm_price = price_d > 0.0
                        ? FPN_FromDouble<F>(price_d) : FPN_Zero<F>();

                    uint64_t rebuild_ts_us =
                        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();

                    // v5.1.2 (full symmetric): use shared OneCore helper.
                    // Single-writer is this thread (per_core_slow's c).
                    auto* sst = state.cores[c].slow_state;
                    FPN<F> vol = volume_d > 0.0 ? FPN_FromDouble<F>(volume_d) : FPN_Zero<F>();
                    FPN<F> bs = cfg.depth_enabled ?
                        FPN_FromDouble<F>(book_spread_d) : FPN_Zero<F>();
                    EventLoop_UpdateRollingStateOneCore(
                        &state, c,
                        mtm_price, vol, rebuild_ts_us,
                        /*is_buyer_maker=*/0, // TODO(parity-check Finding #5): plumb through scalar bus (v5.10.X)
                        cfg.depth_enabled ? book_imb : FPN_Zero<F>(),
                        bs,
                        cfg.depth_enabled ? 1 : 0);

                    // v5.1.1: bracket OTHER section (depth read + swap pickup
                    // + per-cadence pushes setup).
                    uint64_t _sec_t_rebuild_start = __rdtsc();
                    CoreLatencyStats_Sample(
                        &state.cores[c].slow_path_breakdown[CoreContext<F>::SP_SECTION_OTHER],
                        _sec_t_rebuild_start - _sec_t_other_start, _sec_t_rebuild_start);

                    // === Strategy dispatch + gate parameter rebuild ===
                    // v5.1.0: pass per-core slow_state pointers instead of
                    // producer-shared state. Each engine reads ONLY its own.
                    EventLoop_RebuildOneCore(
                        &state, c, &sst->rolling_short, &cfg, &sst->rolling_long,
                        &sst->regime_ror, &sst->ema_price,
                        FPN_IsZero(mtm_price) ? nullptr : &mtm_price,
                        &sst->rolling_medium, &sst->rolling_baseline,
                        &sst->cumdelta_state, &sst->tick_rate_state, rebuild_ts_us,
                        cfg.depth_enabled ? &book_imb : nullptr,
                        &sst->book_imb_history, &sst->flow_state,
                        &sst->large_trade_state, &sst->spread_state,
                        book_spread_d, book_mid_d, book_imbalance_blocked);

                    // v5.1.1: bracket REBUILD section.
                    uint64_t _sec_t_push_start = __rdtsc();
                    CoreLatencyStats_Sample(
                        &state.cores[c].slow_path_breakdown[CoreContext<F>::SP_SECTION_REBUILD],
                        _sec_t_push_start - _sec_t_rebuild_start, _sec_t_push_start);

                    // === Push pending_params via seqlock (was inside
                    // PushParameters wrapper; inline for per-core path).
                    if (state.cores[c].dirty) {
                        ExecutionCore<F>* core = state.cores[c].core;
                        if (core) {
                            ExecutionCore_SetParameters(core,
                                state.cores[c].pending_params);
                        }
                        state.cores[c].dirty = 0;
                    }

                    // v5.1.1: bracket PUSH_PARAMS section.
                    uint64_t _sec_t_te_start = __rdtsc();
                    CoreLatencyStats_Sample(
                        &state.cores[c].slow_path_breakdown[CoreContext<F>::SP_SECTION_PUSH_PARAMS],
                        _sec_t_te_start - _sec_t_push_start, _sec_t_te_start);

                    // === Time exit + trailing SL ratchet (per-core) ===
                    if (cfg.max_hold_ticks > 0 && price_d > 0.01) {
                        EventLoop_TimeExitOneCore(&state, &oms, cfg,
                            now_tick, price_d, c);
                    }
                    // v5.1.1: bracket TIME_EXIT section.
                    uint64_t _sec_t_tsl_start = __rdtsc();
                    CoreLatencyStats_Sample(
                        &state.cores[c].slow_path_breakdown[CoreContext<F>::SP_SECTION_TIME_EXIT],
                        _sec_t_tsl_start - _sec_t_te_start, _sec_t_tsl_start);

                    if (!FPN_IsZero(cfg.sl_trail_mult) &&
                        !FPN_IsZero(cfg.tp_hold_score) &&
                        !FPN_IsZero(sst->rolling_short.price_stddev) &&
                        price_d > 0.01) {
                        EventLoop_TrailingSLRatchetOneCore(&state, cfg,
                            sst->rolling_short, price_d, c);
                    }
                    // v5.1.1: bracket TRAIL_SL section. Tail-end "OTHER"
                    // (warmup permission + post-cycle book-keeping) folds
                    // into the next iteration's _sec_t_other_start delta —
                    // negligible (<100ns) so we don't add another bracket.
                    uint64_t _sec_t_tail = __rdtsc();
                    CoreLatencyStats_Sample(
                        &state.cores[c].slow_path_breakdown[CoreContext<F>::SP_SECTION_TRAIL_SL],
                        _sec_t_tail - _sec_t_tsl_start, _sec_t_tail);

                    // === Warmup permission grant (per-core check) ===
                    uint32_t min_samples = cfg.min_warmup_samples > 0
                        ? cfg.min_warmup_samples : 64;
                    if (sst->rolling_short.count >= (int)min_samples &&
                        state.cores[c].strategy_id != STRATEGY_NONE) {
                        ExecutionCore_SetPermission(&cores[c], 1);
                    }

                    // v4.7.42 (Phase E): close rdtsc bracket + sample.
                    uint64_t _sp_t1 = __rdtsc();
                    CoreLatencyStats_Sample(&state.cores[c].slow_path_latency,
                                             _sp_t1 - _sp_t0, _sp_t1);

                    // v5.0.3: post-cycle book-keeping for the topology panel.
                    // v5.12.1.A.2 — single system_clock::now() read here is
                    // SHARED with EventLoop_CheckWsStaleness immediately
                    // below. Pre-v5.12.1.A.2 the gate had its own clock
                    // read (~50-100ns extra per cycle per core). Sharing
                    // saves that cost; sp_last_tick_us semantics preserved
                    // (post-cycle wall-clock).
                    {
                        uint64_t now_us =
                            (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                        state.cores[c].sp_last_tick_us.store(now_us, std::memory_order_relaxed);
                        state.cores[c].sp_cycles_total.fetch_add(1, std::memory_order_relaxed);
                        // v5.12.1.A.2 — WS-staleness emergency-flatten gate.
                        // Reuses now_us above (no extra clock read). Default
                        // cfg.ws_dead_time_flatten_enabled=0 → 5ns early
                        // return. CAS in CheckWsStaleness ensures only one
                        // core's slow-path wins the flatten across all
                        // concurrent calls.
                        EventLoop_CheckWsStaleness(&state, cfg, price_d,
                                                    now_us);
                    }

                    // NOTE: DrainPostFill stays on the drainer thread (single
                    // writer of last_*_mask is HandleFill on drainer; same
                    // thread reads + clears via DrainPostFill wrapper). No
                    // need for atomic mask conversion in C.2.
                    //
                    // NOTE: KillSwitchEvaluate is GLOBAL (account-level
                    // drawdown), runs on producer thread. Per-core kill
                    // switch state is mutated INSIDE RebuildOneCore.
                    //
                    // NOTE: Drag TP/SL pickup + manual close stay on
                    // drainer + producer threads respectively. They submit
                    // via OMS_PushSubmit (Phase B) — thread-safe.
                }
                fprintf(stderr, "[slow-path-%d] thread exiting\n", c);
            });
        }
    }

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
        if (ezoo && ezoo->active && ezoo->initialized_bandits &&
            cfg.core_model_dir[i][0]) {
            int saved = EnsembleModelZoo_SaveBanditState(
                ezoo, cfg.core_model_dir[i],
                /*regime_names=*/nullptr);
            if (saved) {
                fprintf(stderr, "[sharded] core %d: saved bandit state to "
                                "%s/bandit_state.json\n",
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
    if (!slow_paths.empty()) {
        fprintf(stderr, "[sharded]   joining slow-paths (%zu)...\n", slow_paths.size());
        for (auto& sp : slow_paths) sp.join();
    }
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

    // v5.11.6.A — InitArena destroy MOVED to after per-struct frees
    // (Strategy_FreePerCore loop below at line ~3274). Pre-fix ordering
    // had InitArena_Destroy here BEFORE the strategy free loop, which
    // would zero out InitArena_Global() while strategy frees were still
    // running — InitArena_Owns check in Strategy_FreePerCore would
    // return 0 and `delete` would run on already-unmapped memory.
    // Closes parity-check 2026-05-07 v5.11.6 sprint-exit Finding 7b.

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

    // Restore previous signal handlers so subsequent code paths see the
    // original behavior (legacy engine doesn't install one, so this resets
    // to SIG_DFL).
    std::signal(SIGINT,  prev_int);
    std::signal(SIGTERM, prev_term);
}

}  // namespace tt
