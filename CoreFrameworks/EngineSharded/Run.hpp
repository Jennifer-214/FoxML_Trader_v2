// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/EngineSharded/Run.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ENTRY_POINT] [LIVE_TRADING] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[THE sharded orchestrator — EngineSharded_Run boot->threads->run-loop->shutdown, plus TSC/pinning/latency-dump utilities and the ANSI TUI; ~30 function-scope statics live here]
// [CONTAINS]
//   - [FUNCTION]_[EngineSharded_CalibrateTscGhz]
//   - [FUNCTION]_[EngineSharded_PinThread]   (+ NodeHotCpu / DrainerCpu / NodeSlowCpuBase topology trio)
//   - [FUNCTION]_[EngineSharded_SmartSlowPathPins] (+ GetSiblingCPU)
//   - [FUNCTION]_[EngineSharded_DumpLatency]
//   - [FUNCTION]_[EngineSharded_Run]
//======================================================================================================
// Sub-file of CoreFrameworks/EngineSharded.hpp (split per file-size-split-discipline.md
// at v5.15.5.F.4d.1.B.6; subfolder pattern first canonical).
//
// Contains (Phase B Step B.4 extraction; closes the subfolder-split scope):
//   - g_sharded_order_lat — inline ShardedOrderLatency singleton (moved here from
//     EngineSharded.hpp per Decision C placement; co-located with DumpLatency +
//     EngineSharded_Run final dump that reads it). C++17 inline-variable discipline
//     (single shared storage across TUs); sister to Boot.hpp + Async.hpp inline globals.
//   - EngineSharded_CalibrateTscGhz — TSC frequency calibration (~50ms busy work).
//   - EngineSharded_PinThread — best-effort core pinning via pthread_setaffinity_np.
//   - EngineSharded_GetSiblingCPU — SMT sibling read from /sys/devices/system/cpu.
//   - EngineSharded_SmartSlowPathPins — slow-path CPU pin assignment avoiding SMT
//     siblings of producer/hot/drainer threads.
//   - EngineSharded_DumpLatency<F> — per-core latency table dump (used at shutdown).
//   - EngineSharded_Run<F, BENCH> — main orchestrator template. The actual sharded
//     engine entry point that main.cpp dispatches to when engine_mode == ENGINE_MODE_SHARDED.
//     Holds ~30 function-scope `static` objects (BinanceStream / BinanceAdapterState /
//     InitArena / BinanceUserData / NotifyState / TickRecorder / DepthRecorder /
//     DepthSharedState / ReconciliationLoop / tick_rings[] / nodes[] / TUISharedState /
//     CandleAccumulator + ShardedTradeLog); these block-scope statics travel with the
//     function definition (function-local storage; not accessible from outside this
//     translation unit). SH_* ANSI color macros + matching #undefs are inside the
//     EngineSharded_Run body (#else USE_IMGUI_GUI branch — ANSI TUI render loop).
//
// **Decision G — atomic extraction unit:** SH_* color macros + EngineSharded_Run body
// + DumpLatency move together (they all share the same file scope post-extract).
//
// **Decision C placement — g_sharded_order_lat:** Co-located with DumpLatency +
// EngineSharded_Run final dump that reads it. Pre-`.B.4` lived in EngineSharded.hpp
// proper; moved here at `.B.4` Phase B Step B.4 since callers all live in this file
// now. `inline` (C++17) discipline preserved — single shared storage across TUs.
//
// **Decision D — sub-file include order:** Run.hpp included LAST in parent shim
// (Boot → SlowPath → Async → Run) so all hoisted helpers (DrainPostFill /
// DrainManualCloses / FanOut / DrainWithSubmit) + globals (g_engine_sharded_shutdown /
// g_engine_sharded_gui_quit_ptr / g_engine_drainer_cycle_hist) referenced inside
// EngineSharded_Run body are available by the time Run.hpp is processed.
//
// **C++17 template explicit-args discipline (B.2 lesson):** ShardedSnapshot_Save<F>
// declared via header include (ShardedSnapshotPersist.hpp) BEFORE the call inside
// EngineSharded_Run body, satisfying C++17 two-phase lookup. Forward-decl-at-global-
// scope (B.3 lesson): TUISharedState + CandleAccumulator forward decls (if needed)
// go OUTSIDE namespace tt to avoid namespace-shadowing pitfall — but here we include
// full headers, so no forward-decls needed.
//======================================================================================================

#pragma once

// Match EngineSharded.hpp's full include set — Run.hpp owns the bulk of the engine
// orchestrator body and its dependencies are the union of all the engine's surfaces.
// (Boot/SlowPath/Async also include the subset each needs; double-include is safe
// via #pragma once.)
#include "../../DataStream/BinanceCrypto.hpp"
#include "../../DataStream/BinanceOrderAPI.hpp"
#include "../../DataStream/TickRecorder.hpp"  // Phase 8a (post-coding c7)
#include "../Notify.hpp"                      // Phase 8b (post-coding c8)
#include "../../FixedPoint/FixedPointN.hpp"
#include "../../ML_Headers/RollingStats.hpp"
#include "../../ML_Headers/BuildFlags.hpp"  // v5.9.5h: BUILD_FLAGS_HASH() for cross-build drift WARN
#include "../LiveReadiness.hpp"  // v5.15.2: LiveReadiness_Verify boot gate + FOREACH_LIVE_READINESS_CHECK
#include "../../Strategies/StrategyParameters.hpp"
#include "../../Strategies/StrategyLifecycle.hpp"  // v5.4.0 Phase 1.2: Strategy_InitPerCore / _FreePerCore
#include "../../DataStream/BinanceUserData.hpp"
#include "../BinanceAdapter.hpp"
#include "../ReconciliationLoop.hpp"
#include "../Reconcile.hpp"  // v5.2.1: boot-time exchange-truth reconcile (parse + decide)
#include "../../MemHeaders/HealthLog.hpp"  // v5.4.0 Phase 0.1: structured JSONL diagnostic log
#include "../ShardedLiveSafety.hpp"   // Phase 0: orphan recovery, force-close, reconcile
#include "../ShardedSnapshot.hpp"
#include "../ShardedSnapshotPersist.hpp"  // Phase 4: persistent state across restarts
#include "../PaperResetArchive.hpp"        // v5.15.5.C.3 Phase 6 — paper-reset archive helpers (Summary_WriteJson + dirname/mkdir)
#include "../../MemHeaders/LatencyHistogram.hpp"  // v5.15.5.C.3 Phase 7.B — drainer-cycle bench gate histogram
#include "../../MemHeaders/DrainerConstants.hpp"  // v5.15.5.C.4 Phase T1 — drainer-thread-stable POD struct (fee_rate_taker_d, drain_count, partial_on)
#include "../../MemHeaders/OmsPhasedDrain.hpp"    // v5.15.5.C.4 Phase F — phase-separated drainer foundation (DrainIntoBuckets + ProcessBucket_*)
#include "../ControllerEventLoop.hpp"
#include "../NodeLatencyStats.hpp"
#include "../ControllerConfig.hpp"
#include "../ExchangeAdapter.hpp"
#include "../ExecutionCore.hpp"
#include "../GateParameters.hpp"
#include "../OrderManager.hpp"
#include "../EngineCommon.hpp"  // v5.15.5.F.4d.1.B.4 — shared train-serve helpers (ApplyBnbDiscount + BootGlobal + BootPerCore + SlowPathCycle*)
#include "../../MemHeaders/OmsPushExitHelper.hpp"  // v5.15.5.C.4 Phase D5 — OMS_PushExitForSlot helper (Class-18 close)
#include "../ShardedOrderLatency.hpp"
#include "../SPSCRing.hpp"
#include "../Tick.hpp"

#ifdef USE_IMGUI_GUI
#include "../../GUI/CandleAccumulator.hpp"
#include "../../GUI/GuiThread.hpp"
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

#include "../HotSwap.hpp"           // v5.15.4 — HotSwap_ShadowLoad_{Ensemble,SingleZoo}
#include "../ModelValidation.hpp"   // v5.14.2.E.1 — NodeModelZoo_ValidateAgainstCfg (extracted; PARITY-012)
#include "../../ML_Headers/FeatureRegistryOverlay.hpp"  // v5.14.3.B — FeatureOverlay_PostLoadVerify

// Sister sub-files in CoreFrameworks/EngineSharded/. Pulled in here as well so
// Run.hpp's EngineSharded_Run body can reference the hoisted helpers + inline
// globals (g_engine_sharded_shutdown / g_engine_drainer_cycle_hist / etc.).
// Parent shim (EngineSharded.hpp) includes all 4 sub-files; this redundancy
// keeps Run.hpp self-contained for IDE navigation + future reuse.
#include "Boot.hpp"     // g_engine_sharded_shutdown / g_engine_sharded_gui_quit_ptr / EngineSharded_SignalHandler
#include "SlowPath.hpp" // EngineSharded_SlowPath_DrainManualCloses (DrainPostFill binder -> EngineCommon_DrainPostFill, E.1.2.C)
#include "Async.hpp"    // g_engine_drainer_cycle_hist / EngineSharded_Async_FanOut / _DrainWithSubmit

// parent_index: CoreFrameworks/EngineSharded.hpp

namespace tt {

//------------------------------------------------------------------------------
// [SECTION]_[order latency stats — the g_sharded_order_lat singleton instance]
//------------------------------------------------------------------------------
// the type and helper functions live in CoreFrameworks/ShardedOrderLatency.hpp
// (extracted during the OMS phase 01 refactor so OrderManager.hpp can call
// Sample without circular includes). this file just owns the singleton instance
// the TUI render loop reads from. OrderManager_Init takes a pointer to it so
// the OMS can sample each REST round trip into the same counters the TUI
// already displays.
//
// v5.15.5.F.4d.1.B.6: converted from `static` to `inline` per C++17 inline-variable
// discipline (Decision C; sister to Boot.hpp pattern). Moved from EngineSharded.hpp
// to here at Phase B Step B.4 — co-located with DumpLatency + EngineSharded_Run final
// dump that reads it (the only consumers).
//======================================================================================================
inline ShardedOrderLatency g_sharded_order_lat;

//======================================================================
// [FUNCTION]_[EngineSharded_CalibrateTscGhz]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[~50ms rdtsc-vs-wall busy calibration so the latency dump can print ns beside raw cycles]
//======================================================================
// [CODE]
//======================================================================
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Quick TSC frequency calibration so the latency dump can show ns alongside
// raw cycles. ~50ms of busy work, plenty accurate for diagnostic display.
//======================================================================
// [END_FUNCTION]_[EngineSharded_CalibrateTscGhz]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineSharded_PinThread]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[best-effort pthread_setaffinity pin (failure = warn, never fatal); the NodeHotCpu/DrainerCpu/NodeSlowCpuBase topology trio shares the section]
//======================================================================
// [CODE]
//======================================================================
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

//------------------------------------------------------------------------------
// [SECTION]_[PER-NODE CPU TOPOLOGY — a NODE owns 2 CPUs (1 hot + 1 slow); node_id ≠ cpu_id.]
//------------------------------------------------------------------------------
// Boot CPU layout for N nodes (= num_execution_nodes):
//   CPU 0      = producer
//   CPU 1..N   = node hot threads   (node i → EngineSharded_NodeHotCpu(i))
//   CPU N+1    = drainer            (EngineSharded_DrainerCpu(N))
//   CPU N+2..  = node slow threads  (EngineSharded_NodeSlowCpuBase(N) fallback base;
//                SmartSlowPathPins overrides slow → a SEPARATE physical core, avoiding SMT siblings)
// So a node spans 2 CPUs on 2 physical cores (the hot/slow split). These helpers name the
// conventions (was scattered magic i+1 / num+1 / num+2) so node↔cpu stays legible — and are the
// single override point if per-node CPU config ever lands (a NON-goal here; this leaf is names-only).
static inline int EngineSharded_NodeHotCpu(int node_id)        { return node_id + 1;   }  // CPU 1..N (CPU 0 = producer)
static inline int EngineSharded_DrainerCpu(int num_nodes)      { return num_nodes + 1; }  // CPU N+1
static inline int EngineSharded_NodeSlowCpuBase(int num_nodes) { return num_nodes + 2; }  // CPU N+2.. (fallback base)
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Best-effort core pinning. Returns 1 on success, 0 on failure (logged but
// not fatal — if pinning fails the engine still runs, just with potentially
// worse tail latency due to scheduler migration).
//======================================================================
// [END_FUNCTION]_[EngineSharded_PinThread]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineSharded_SmartSlowPathPins]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[v5.1.5 slow-path pin assignment that avoids SMT siblings of producer/hot/drainer (3-pass: idle -> tainted -> wraparound); GetSiblingCPU (sysfs read) shares the section]
//======================================================================
// [CODE]
//======================================================================
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
        int hcpu = EngineSharded_NodeHotCpu(i);  // node i hot → CPU i+1 (CPU 0 = producer)
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineSharded_SmartSlowPathPins]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineSharded_DumpLatency]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[shutdown per-node latency table (min/p50/p95/p99/max/avg in ns via the calibrated TSC); notes the ~25-30ns rdtsc floor]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
static inline void EngineSharded_DumpLatency(const ExecutionCore<F>* nodes,
                                              int num_nodes, double tsc_ghz) {
    fprintf(stderr, "\n");
    fprintf(stderr, "================================================================\n");
    fprintf(stderr, "[sharded] PER-CORE LATENCY (samples are p-stats from 256 most recent ticks)\n");
    fprintf(stderr, "================================================================\n");
    fprintf(stderr, "core   samples       min        p50        p95        p99        max        avg\n");
    fprintf(stderr, "----   --------   --------   --------   --------   --------   --------   --------\n");
    for (int i = 0; i < num_nodes; ++i) {
        NodeLatencySnapshot s = NodeLatencyStats_Snapshot(&nodes[i].latency_stats, tsc_ghz);
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Dumps per-core latency stats in a compact table after the run finishes.
// One row per core, all converted to ns via the calibrated TSC frequency.
//======================================================================
// [END_FUNCTION]_[EngineSharded_DumpLatency]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineSharded_Run]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ENTRY_POINT] [LIVE_TRADING] [CONCURRENCY] [CAPITAL_BEARING]]
// [REFERENCE]_[INVARIANT]_[[H3] [H8] [H22] [H21]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the sharded main — TSC calibrate, boot (BNB discount -> BootGlobal -> per-node BootPerCore), spawn producer/drainer/per-node hot+slow threads, run to shutdown, join + latency dump]
// [REFERENCE]_[CLASS]_[[9] [25] [27] [47]]
// [REFERENCE]_[DECISION]_[[D-103] [D-221]]
// [REFERENCE]_[PARITY]_[[PARITY-9] [PARITY-12] [PARITY-23] [PARITY-26] [PARITY-27] [PARITY-28] [PARITY-29] [PARITY-30] [PARITY-32]]
// [REFERENCE]_[TECH_DEBT]_[[TECH_DEBT-2] [TECH_DEBT-119]]
// [REFERENCE]_[DESIGN_SPEC]_[[cfg-scope-discipline.md] [decision-time-data-binding-pattern.md] [phase-separated-drainer-for-safe-cross-temporal-derives.md] [shadow-load-state-transition-pattern.md] [sink-fn-pointer-for-optional-side-effect-pattern.md]]
//======================================================================
// [CODE]
//======================================================================
// The sharded engine main entry point. Called from main.cpp when
// engine_mode == ENGINE_MODE_SHARDED.
//
// Behavior:
//   1. Calibrate TSC for ns conversion
//   2. Build EventLoopState + N execution cores from config
//   3. Spawn 1 producer thread (synthetic ticks, sawtooth around $60k)
//   4. Spawn N executor threads (one per core, each pinned if possible)
//   5. Spawn 1 drainer thread on the controller core
//   6. Enable per-core NodeLatencyStats
//   7. Run until shutdown_flag is raised
//   8. Join all threads, dump per-core latency
//
// shutdown_flag is the same volatile int main.cpp uses for SIGINT/SIGTERM.
// Pass &g_shutdown_requested or whichever variable you have.
//======================================================================================================

// v5.14.2.E.1 — NodeModelZoo_ValidateAgainstCfg moved to its own header
// (CoreFrameworks/ModelValidation.hpp) so BacktestSharded.hpp can call it
// (closes PARITY-012). Same boundary-stable refactor pattern as
// EnsembleHotSwap.hpp from v5.14.2.A. Function definition unchanged.
// Header is included OUTSIDE namespace tt (at top of file, around line 90).
// v5.15.5.C.2 — old #if 0 definition removed (was kept "for context" since
// v5.14.2.E.1 extracted the function to ModelValidation.hpp). See git
// history pre-v5.15.5.C.2 if archeological reference needed.

// E.1.2.C — the v5.14.2 EngineSharded_HotSwapEnsemble legacy in-place
// helper (EnsembleHotSwap.hpp) is DELETED: superseded v5.15.4 by the
// HotSwap_ShadowLoad_* path, zero production callers at HEAD (its old
// call site here was removed with the v5.15.4 cutover; a stale comment
// claiming "the call site is in EngineSharded_Run below" survived it).

//------------------------------------------------------------------------------
// [SECTION]_[Phase 7.B — drainer-cycle bench gate histogram]
//------------------------------------------------------------------------------
template <unsigned F, bool BENCH = false>
static inline void EngineSharded_Run(ControllerConfig<F>& cfg,
                                      const BinanceConfig& bcfg) {
    // Install our own SIGINT/SIGTERM handler so threads can shut down cleanly.
    // Save the previous handlers so we can restore them on exit (in case the
    // legacy engine path runs after us in some test setup).
    g_engine_sharded_shutdown = 0;
    // v5.15.5.C.3 Phase 7.B — reset drainer-cycle bench histogram at boot.
    // Zero-cost when BENCH=false (compile-time elided); at BENCH=true the
    // reset ensures no stale stats from a prior run leak into this session.
    if constexpr (BENCH) {
        LatencyHistogram_Reset(&g_engine_drainer_cycle_hist);
        std::fprintf(stderr,
            "[OMS_BENCH] bench gate ENABLED — drainer cycle latencies will "
            "be recorded into g_engine_drainer_cycle_hist; summary line "
            "emitted at engine shutdown.\n");
    }
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
            "engine_start arch=sharded num_nodes=%u health_log_level=%d",
            (unsigned)cfg.num_execution_nodes, cfg.health_log_level);
    }

    // v5.14.9.B — composite-required-for-ladder boot REFUSE.
    // WIP2d-0 (.F.4c.3) — REFACTORED per cfg-scope-discipline.md § Anti-pattern 1
    // (global default + per-instance override FORBIDDEN). Pre-refactor used the
    // legacy global-default-with-override pattern (cfg.risk_degradation_curve flat
    // + cfg.node_overrides[c].risk_degradation_curve). Post-WIP2d-0 walks
    // cfg.nodes[c].risk_degradation_curve exclusively over active cores —
    // per-core authoritative per Class 25 + Anti-pattern 1 discipline. The
    // resolution gap (override=0 means inherit-from-global) no longer exists;
    // each core's own value is THE value.
    //
    // Safety semantics preserved: refuse boot if ANY active core has ladder
    // enabled AND confidence_composite_enabled=0 engine-wide. Ladder thresholds
    // are tuned for composite confidence scale [0.001, 0.3]; legacy 3-factor IC
    // scale [0.05, 0.5] would silently misbehave.
    {
        bool any_ladder_active = false;
        int  active_node_id    = -1;
        int  active_curve      = (int)CURVE_OFF;
        for (uint16_t c = 0; c < cfg.num_execution_nodes && c < MAX_EXECUTION_NODES; ++c) {
            int curve = cfg.nodes[c].risk_degradation_curve;
            if (curve != (int)CURVE_OFF) {
                any_ladder_active = true;
                active_node_id    = (int)c;
                active_curve      = curve;
                break;
            }
        }
        if (any_ladder_active && BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED) == 0) {
            fprintf(stderr,
                "[boot] FATAL: node %d risk_degradation_curve=%s requires "
                "confidence_composite_enabled=1.\n"
                "  Ladder thresholds are tuned for composite confidence scale; "
                "legacy 3-factor IC scale would silently misbehave.\n"
                "  Set confidence_composite_enabled=1 OR set "
                "node_%d_risk_degradation_curve=OFF.\n",
                active_node_id,
                DegradationCurve_ToString(active_curve),
                active_node_id);
            if (cfg.health_log_path[0]) {
                tt::Health_Log(tt::HEALTH_CRITICAL, "boot", active_node_id,
                    "REFUSE: ladder requires composite (node=%d, curve=%s, composite=0)",
                    active_node_id, DegradationCurve_ToString(active_curve));
            }
            return;  // refuse boot; engine doesn't start
        }
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
        for (uint16_t i = 0; i < cfg.num_execution_nodes && i < 16; ++i) {
            uint8_t sid = cfg.node_strategies[i];
            if (sid != STRATEGY_AUTO && sid != STRATEGY_NONE) {
                hardcoded_count++;
                if (pos < sizeof(hardcoded_list) - 16) {
                    const char* sname = (sid < NUM_STRATEGIES)
                        ? STRATEGY_SHORT_NAMES[sid] : "?";
                    pos += (size_t)snprintf(hardcoded_list + pos,
                        sizeof(hardcoded_list) - pos,
                        "%snode_%u=%s", pos > 0 ? ", " : "", i, sname);
                }
            }
        }
        if (hardcoded_count > 0) {
            const bool live = ControllerConfig_IsLiveCapital(cfg); // NEW-1 — single capital-authority predicate (was cfg.use_real_money)
            const bool ack  = (cfg.acknowledge_hardcoded_strategy_in_live != 0);
            if (live && !ack) {
                fprintf(stderr,
                    "[sharded] ERROR: live mode (trading_mode=live) with "
                    "%d hardcoded strategy node(s): %s\n"
                    "[sharded]        AUTO is recommended for live capital "
                    "(regime-gated strategy selection).\n"
                    "[sharded]        To override: set "
                    "acknowledge_hardcoded_strategy_in_live=1 in engine.cfg.\n",
                    hardcoded_count, hardcoded_list);
                tt::Health_Log(tt::HEALTH_INFO, "engine", -1,
                    "boot_abort reason=hardcoded_strategy_in_live nodes=%s",
                    hardcoded_list);
                return;  // refuse to boot
            }
            // paper, or live+ack — WARN only
            fprintf(stderr,
                "[sharded] WARN: %d hardcoded strategy node(s): %s. "
                "AUTO is recommended for live/paper runs (regime-gated). "
                "Hardcoded is fine for backtest comparisons.\n",
                hardcoded_count, hardcoded_list);
            if (cfg.health_log_path[0]) {
                tt::Health_Log(tt::HEALTH_INFO, "engine", -1,
                    "boot_warn hardcoded_strategy_count=%d nodes=%s "
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
    fprintf(stderr, "[sharded] STARTING in per-node sharded mode\n");
    fprintf(stderr, "[sharded] num_execution_nodes = %u\n", (unsigned)cfg.num_execution_nodes);

    // Partial exits P.1 (2026-04-27): validate cfg before allocating cores.
    // When partial_exit_enabled=1, refuses to start if num_execution_nodes * 2
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
    bool live_trading = ControllerConfig_IsLiveCapital(cfg); // NEW-1 — single capital-authority predicate (was cfg.use_real_money; RBP Class 47)
    if (live_trading) {
        char api_key[128] = {}, api_secret[128] = {};
        if (!LoadSecrets("secrets.cfg", api_key, api_secret)) {
            fprintf(stderr, "[sharded] ERROR: trading_mode=live but secrets.cfg missing or incomplete\n");
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
    fprintf(stderr, "[sharded] Press Ctrl+C to stop and dump per-node stats.\n");
    fprintf(stderr, "================================================================\n");

    ShardedOrderLatency_Reset(&g_sharded_order_lat);

    double tsc_ghz = EngineSharded_CalibrateTscGhz();
    fprintf(stderr, "[sharded] TSC calibrated at %.4f GHz\n", tsc_ghz);

    int num_nodes = (int)cfg.num_execution_nodes;
    if (num_nodes < 1) num_nodes = 1;
    if (num_nodes > MAX_EXECUTION_NODES) num_nodes = MAX_EXECUTION_NODES;

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
    Money live_starting_balance = cfg.starting_balance;
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
        live_starting_balance = Money{ money_from_double_payload(usdt_recovered) };  // D-103 reconcile ingress (exact venue-string parse rides P3 REST rework)
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
    // v5.15.5.C.3 (Finding A) — partial_exit_enabled passed via OrderManager_Init.
    // Pre-Finding A: engine called BITMAP_SET(MASK_OMS_STATE_PARTIAL_EXIT_ENABLED)
    // externally after Init returned (Class-18 mirror at the external SET site).
    // Post-Finding A: passed as parameter; the registry walk inside
    // OMS_INIT_AUTOPOPULATE sets the bit via the BIT-kind row.
    int partial_exit_enabled =
        BITMAP_IS_SET(cfg.lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED) ? 1 : 0;
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — `fee_rate` arg DELETED. Per-core fee_rate flows via Order pre_resolve.
    OrderManager_Init(&oms, exchange_adapter, live_trading ? 1 : 0,
                      partial_exit_enabled,
                      live_starting_balance,
                      (int)cfg.oms_event_log_mode);
    // v5.13.0.B — open calibration log if cfg.calibration_log_path set.
    // No-op when path is empty (default). Failure is non-fatal (logs to
    // stderr; calibration logging disabled this session).
    OrderManager_OpenCalibrationLog(&oms, cfg.calibration_log_path);
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — Class 27 closure: OMS no longer holds scalar fee_rate /
    // fee_rate_maker / fee_rate_taker / slippage_pct fields. Order pre-resolves these
    // per-core via Order_BindPreResolved at submit time. See:
    //   - DESIGN_SPECS/decision-time-data-binding-pattern.md (the principle)
    //   - DOCS/RECURRING_BUG_PATTERNS.md Class 27 (the anti-pattern this closes)
    //   - CoreFrameworks/Order.hpp Order_BindPreResolved (the binding helper)
    //
    // v4.3.2 (Track C.1) — Binance BNB-pay 25% fee discount, applied per-core at boot
    // so cfg.nodes[c].fee_rate_* reflect post-discount values uniformly. Order_BindPreResolved
    // reads cfg.nodes[c].fee_rate_* (already discounted) → o->pre_resolved.fee_rate. User
    // must also enable BNB fee payment in Binance UI for this to actually apply on live fills.
    // v5.15.5.F.4d.1.B.4 Step C.1 — extracted to EngineCommon_ApplyBnbDiscount
    // (closes PARITY-030 by-construction; sister BACKTEST caller invokes same helper
    // at Step C.2). Body preserved verbatim from prior inline at LIVE :690-699 →
    // EngineCommon_ApplyBnbDiscount in CoreFrameworks/EngineCommon.hpp. Non-const cfg mutation; ONE-SHOT
    // pre-loop; THE ONLY non-const-cfg helper in EngineCommon.
    EngineCommon_ApplyBnbDiscount(cfg);
    // v4.2.1 paper-mode slippage simulation — also per-core via cfg.nodes[c].slippage_pct,
    // pre-resolved onto Order at submit via Order_BindPreResolved. Live mode reads exchange
    // fill prices directly (EventLoop_OnEvent gates on live_trading); slippage value ignored.
    // v5.15.5.C.3 (Finding A) — external PARTIAL_EXIT_ENABLED SET call dropped.
    // Bit is now set inside OMS_INIT_AUTOPOPULATE via the BIT-kind registry row
    // for `partial_exit_enabled` (driven by the parameter passed to OrderManager_Init
    // above). Adding a new cfg-derived boot bit flag = ONE row in
    // FOREACH_OMS_FIELD; no more external SET sites needed.

    // Trade log CSV — same pattern as legacy engine in main.cpp
    static ShardedTradeLog g_sharded_trade_log;
    // s5-1b: partials mode threaded in so the log derives TRUE node + leg from
    // the slot-keyed event.node_id (BITMAP_SLOT_NODE) for truthful attribution.
    ShardedTradeLog_Init(&g_sharded_trade_log, bcfg.symbol, partial_exit_enabled);
    oms.trade_log = &g_sharded_trade_log;
    // v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer wire-to-real.
    // Per DESIGN_SPECS/sink-fn-pointer-for-optional-side-effect-pattern.md. Default = noop;
    // set-to-real when trade_log enables → handle_buy_fill / handle_sell_fill dispatch to log emit.
    oms.on_entry_fill_emit = &tt::real_on_entry_fill_emit<F>;
    oms.on_exit_fill_emit  = &tt::real_on_exit_fill_emit<F>;

    // v5.11.6.A — InitArena: single mmap'd region for all init-time
    // allocations (PortfolioController rolling stats × 3 + cumdelta_state +
    // per-core NodeSlowState + per-core strategy state). MAP_POPULATE
    // pre-faults all pages at boot so first slow-path cycle never page-faults.
    //
    // Sizing (measured at boot 2026-05-07):
    //   - NodeSlowState<64> ≈ 278 KB / core × 16 cores = 4.4 MB
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
    // v5.15.5.F.4d.1.B.4 Step C.1 — extracted to EngineCommon_BootGlobal
    // (closes PARITY-026 hotfix sister-discipline as part of TECH_DEBT-119
    // structural fold; BACKTEST sister at Step C.2 uses same helper). Body
    // preserved verbatim from prior inline at LIVE :742 + :749-753 + :760-762 →
    // EngineCommon_BootGlobal in CoreFrameworks/EngineCommon.hpp (Init + ConfigureKillSwitch +
    // Regime_Init loop with cfg-driven hysteresis per cfg.nodes[i].regime_hysteresis).
    //
    // M5 LIVE-only persistence sinks (DepthRecorder + Notify + trade_log + TickRecorder
    // + BinanceUserData + ReconciliationLoop) stay post-helper at caller scope per
    // Decision G (process-lifetime + thread-shared semantics; not BACKTEST-mirrored).
    EngineCommon_BootGlobal(cfg, state, oms);

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
    if (BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_NOTIFY_ENABLED)) {
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
                       cfg.record_depth && BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_DEPTH_ENABLED));
    if (BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_DEPTH_ENABLED)) {
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
    // ExecutionCore is ~66KB and num_nodes * size could blow the stack.
    static SPSCRing<Tick<F>, EXECUTION_NODE_TICK_RING_SIZE> tick_rings[MAX_EXECUTION_NODES];
    static ExecutionCore<F> nodes[MAX_EXECUTION_NODES];
    // v5.1.2 — full per-core data plane. All rolling stats, regime, flow,
    // depth-history state lives in EACH engine's `state.nodes[c].slow_state`
    // (heap-allocated by EventLoopState_Init). Per-core slow-path lambda
    // updates its own slow_state on each cycle.
    //
    // Pre-v5.1.2 the producer maintained ONE shared copy at function scope.
    // Removed in v5.1.2; readers now use state.nodes[c].slow_state pointers.
    // ema_price stays at producer scope since it's per-tick (replicated to
    // all N engines via EventLoop_UpdateEmaPriceAllCores in fan_out).
    FPN_Binary<F> ema_price = FPN_Zero<F>();
    // EMA alpha matches PortfolioController_Init's default (gate_ema_alpha
    // is the cfg key). Computed at boot from cfg; updated each tick.
    FPN_Binary<F> ema_alpha = !FPN_IsZero(cfg.gate_ema_alpha)
                       ? cfg.gate_ema_alpha
                       : FPN_FromDouble<F>(0.1);

    // Per-core risk allocation: split the configured starting balance across
    // cores. Each core gets its own mini-portfolio that's risk_pct of the
    // total. With risk_pct=10% and 4 cores starting at $10k, each core gets
    // $250 to risk on a single trade.
    double total_balance = Money_ToDouble(cfg.starting_balance);
    double default_risk = Money_ToDouble(cfg.risk_pct);
    if (default_risk <= 0.0) default_risk = 0.10;
    double default_per_node = (total_balance * default_risk) / (double)num_nodes;
    if (default_per_node < 1.0) default_per_node = 1.0;

    for (int i = 0; i < num_nodes; ++i) {
        // v5.15.5.F.4d.1.B.4 Step C.1 — per-core boot extracted to
        // EngineCommon_BootPerCore (TECH_DEBT-119 closure + closes PARITY-027/028/029
        // by-construction; BACKTEST sister at Step C.2 invokes same helper). Caller
        // owns: node_balance precompute (O2 bytewise-identical math) + ML zoo
        // aligned_alloc with null-check (LIVE-specific; BACKTEST uses Free+Init static
        // array) + post-helper LIVE-only wires (oms.ezoo_refs + NodeLatencyStats_Enable).
        // Helper body preserved verbatim from prior inline at LIVE :908-1177 →
        // EngineCommon_BootPerCore in CoreFrameworks/EngineCommon.hpp.

        // Per-core risk: use core-specific override if set, else shared/even split
        // (preserved verbatim per v1.6 O2 bytewise-identical math discipline).
        // E.1.1 ③/B — reads nodes[i].risk_pct (raw-copied from node_risk_pct[i] in
        // PopulateCoresFromFlat, 0=inherit preserved) — byte-identical to the legacy array read.
        double node_balance = default_per_node;
        if (!Money_IsZero(cfg.nodes[i].risk_pct)) {
            node_balance = total_balance * Money_ToDouble(cfg.nodes[i].risk_pct);
            if (node_balance < 1.0) node_balance = 1.0;
        }

        // ML zoo allocation (LIVE: aligned_alloc heap with null-check; BACKTEST uses
        // Free+Init static array at Step C.2). v5.15.4 — heap-allocate zoo containers
        // for lifecycle consistency with shadow-load (HotSwap_ShadowLoad_* unconditional
        // free(old_ptr) requires heap-resident containers). alignas(64) on container
        // struct (NodeModelZoo + EnsembleModelZoo; v5.15.4.B) means aligned_alloc(64,
        // sizeof(T)) gives the embedded alignment-sensitive members (ModelHandle,
        // RidgeWeights, etc.) correctly-aligned addresses. Process-exit leak acceptable
        // per existing static-array behavior (no shutdown cleanup of internal allocations).
        // Helper handles all load/init/validate/post-load paths internally.
        NodeModelZoo<F>* zoo_ptr = nullptr;
        EnsembleModelZoo<F>* ezoo_ptr = nullptr;
        if (cfg.node_strategies[i] == STRATEGY_ML) {
            zoo_ptr = (NodeModelZoo<F>*)aligned_alloc(64, sizeof(NodeModelZoo<F>));
            if (!zoo_ptr) {
                fprintf(stderr, "[sharded] node %d: aligned_alloc(NodeModelZoo) "
                                "failed; ML node cannot init\n", i);
                NODE_STATE_FLAG_SET(state.nodes[i], MODEL_LOAD_FAILED);
                continue;
            }
            ezoo_ptr = (EnsembleModelZoo<F>*)aligned_alloc(64, sizeof(EnsembleModelZoo<F>));
            if (!ezoo_ptr) {
                fprintf(stderr, "[sharded] node %d: aligned_alloc(EnsembleModelZoo) "
                                "failed; ML node cannot init\n", i);
                free(zoo_ptr); zoo_ptr = nullptr;
                NODE_STATE_FLAG_SET(state.nodes[i], MODEL_LOAD_FAILED);
                continue;
            }
        }

        // Helper call — closes PARITY-027 (exit-model bind) + PARITY-028 (Bind
        // CompositeCfg + RollingTurnover) + PARITY-029 (Strategy_InitPerCore) by
        // construction. Body covers: SPSCRing_Init + ExecutionCore_Init +
        // EventLoopState_RegisterCore + SetCoreStrategy + full ML branch (load/init/
        // post-load/validate/overlay/ConfidenceScorer/RollingTurnover) +
        // Strategy_InitPerCore + SetPermission. Internal logic identical to prior
        // inline at LIVE :908-1177.
        EngineCommon_BootPerCore(cfg, i, state, tick_rings[i], nodes[i],
                                  zoo_ptr, ezoo_ptr,
                                  Money{ money_from_double_payload(node_balance) });

        // Post-helper LIVE-only wires (M5 persistence + threading observability;
        // Decision B + Decision G — STAY in caller post-helper return).
        // v5.15.5.F.4d Step 7 § F — wire engine-wide oms->ezoo_refs[i] + node_cfg_refs[i]
        // alongside per-core ctx.ensemble_handle. OmsState is engine-wide single instance;
        // per-core arrays indexed by Order::node_id at calib log emit time
        // (real_on_exit_calibration). void* cast to EnsembleModelZoo<F>* /
        // const PerNodeCfg<F>* at consumer.
        if (state.nodes[i].ensemble_handle != nullptr) {
            oms.ezoo_refs[i]     = (void*)ezoo_ptr;
            oms.node_cfg_refs[i] = (const void*)&cfg.nodes[i];
        }
        // Per-core latency sampling — the whole point of this mode (LIVE-only;
        // backtest has no latency profiling at this scope per M5 LIVE-only discipline).
        NodeLatencyStats_Enable(&nodes[i].latency_stats);
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
            // v5.15.5.C.2 (S3a + S4): canonical mirror via bit-packed
            // oms_state_flags (S3a); set at OrderManager_Init from cfg; drainer-path
            // single source of truth (S4).
            int loaded = ShardedSnapshot_Load<F>(&state, snapshot_path,
                                                  BITMAP_IS_SET(oms.oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED),
                                                  &cfg);  // v5.5.5
            (void)loaded;  // logged inside; nothing else to do here
        } else {
            // Live reconciliation: exchange truth is authoritative in live mode.
            // Fetch account + open orders + recent trades and reconcile against
            // local state. Refuses boot on critical disagreement (exchange has BTC,
            // local has 0 positions). Mode dispatch (reconcile_mode, below): STRICT
            // refuses on any disagreement; WARN logs + continues; AUTO_SYNC applies.
            // (v5.14.4.B.1/.B.2 wired the apply: cancel-stale-orders is REAL; missed-
            // fill handling is A20 SEED-DON'T-REPLAY [.E.0.10] — see the AUTO_SYNC case.
            // The old "Phase 2 deferred / dry-run-by-default" framing is RETIRED;
            // cfg.reconcile_dry_run is a dead legacy mirror superseded by reconcile_mode
            // [cfg-flag-orphan cohort, TaskList #9].)
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

            // v5.14.4.B.1 — 3-mode dispatch (FOREACH_RECONCILE_MODE).
            //
            // SHARDED-ONLY (deep audit 2026-05-09 / TECH_DEBT-002 alignment):
            // centralized engine main.cpp:362 does balance check only, NOT
            // reconciliation. This dispatch lives in EngineSharded boot ONLY.
            // When TECH_DEBT-002 (centralized removal) ships, no migration
            // step needed here. If v5.X+ adds Phase 3 heartbeat reconcile
            // (per the Reconcile.hpp phase roadmap), verify the new dispatch is ALSO
            // sharded-only — DO NOT add to legacy path.
            //
            // FUTURE-THINKING: if mode dispatch goes per-cycle (e.g.,
            // AUTO_SYNC_CONTINUOUS in v5.X+), apply branchless-dispatch-discipline (default-off template-bool):
            // template <bool ENABLED> + if constexpr for compile-time
            // elision. Boot dispatch today is operator-initiated +
            // I/O-dominated; branchless irrelevant.
            tt::ReconcileMode mode = (tt::ReconcileMode)cfg.reconcile_mode;
            switch (mode) {
                case tt::RECONCILE_STRICT:
                    // STRICT: refused_boot is the canonical CRITICAL refusal
                    // (already returned above). Additionally refuse if any
                    // cancel_actions surfaced — operator wants ZERO disagreement
                    // tolerance in strict mode (e.g. production deploy where
                    // any zombie order indicates a state-management bug to fix
                    // before continuing).
                    if (rr.cancel_actions > 0) {
                        fprintf(stderr,
                            "[reconcile] STRICT mode + %d cancel_action(s) "
                            "detected (zombie orders or position drift); "
                            "engine boot REFUSED. Set reconcile_mode=warn "
                            "to log + continue, OR reconcile_mode=auto_sync "
                            "to replay missed fills + cancel zombies.\n",
                            rr.cancel_actions);
                        BinanceAdapter_ShutdownState(&g_sharded_binance_adapter);
                        std::signal(SIGINT,  prev_int);
                        std::signal(SIGTERM, prev_term);
                        return;
                    }
                    break;

                case tt::RECONCILE_WARN:
                    // WARN: log + continue. Legacy dry_run=1 behavior preserved.
                    if (rr.cancel_actions > 0) {
                        fprintf(stderr,
                            "[reconcile] WARN mode: %d cancel_action(s) "
                            "detected; continuing (set reconcile_mode=auto_sync "
                            "to apply replay + cancel)\n",
                            rr.cancel_actions);
                    }
                    break;

                case tt::RECONCILE_AUTO_SYNC: {
                    // AUTO_SYNC = composition of N independent helper-actions.
                    // .B.1: A20 (.E.0.10) SEED-DON'T-REPLAY. The boot balance was already seeded
                    //       from the exchange (OrphanRecovery, :653) and ALREADY reflects every
                    //       settled trade in the GetMyTrades(0) window; replaying them via
                    //       Reconcile_ApplyMissedFills DOUBLE-BOOKS balance + opens phantom
                    //       positions. The book is FLAT at live boot (no snapshot load :982;
                    //       orphan-sell flattens BTC; Reconcile_Decide refuses on real
                    //       exchange-position-no-local divergence), so the fills are historical.
                    //       So SEED the watermark to max(fetched) WITHOUT replaying. The warm-tail
                    //       replay (Reconcile_ApplyMissedFills, KEPT for that future path) is
                    //       .E.1-gated behind the WS-fill watermark-bump (the OMS last_seen_trade_id contract) --
                    //       until that lands, the boot watermark is always 0 and no non-zero-
                    //       watermark replay is reachable (H21: do not run an unreachable replay).
                    // .B.2: Reconcile_AutoCancelStale (real exchange cancels; UNAFFECTED -- a
                    //       distinct action from the fill replay) stays below.
                    uint64_t seeded = tt::Reconcile_SeedWatermark(&oms, trades, n_trades);
                    fprintf(stderr,
                        "[reconcile] AUTO_SYNC seed-don't-replay (A20): watermark set to %llu "
                        "WITHOUT replay (boot balance already reflects %d venue trade(s))\n",
                        (unsigned long long)seeded, n_trades);

                    // v5.14.4.B.2 — zombie order cleanup. Lambda injects
                    // the network primitive (BinanceOrderAPI_CancelOrder)
                    // into the template helper without Reconcile.hpp needing
                    // to take a network include. Symmetric with .B.1's
                    // template-deferred OMS dependency injection.
                    int cancelled = tt::Reconcile_AutoCancelStale(
                        [&](const char* order_id) -> int {
                            return BinanceOrderAPI_CancelOrder(api, order_id);
                        },
                        orders, n_orders);
                    fprintf(stderr,
                        "[reconcile] AUTO_SYNC cancel: %d zombie order(s) "
                        "cancelled (out of %d cancel_action(s) detected)\n",
                        cancelled, rr.cancel_actions);
                    break;
                }
            }
        }
    }

    std::atomic<bool> producer_done{false};
    std::atomic<uint64_t> ticks_produced{0};
    std::atomic<uint64_t> ticks_consumed_total{0};
    // Latest market tick, published as ONE seqlock'd sample. Producer writes;
    // the render thread, the manual-close drain, and the per-node SLOW threads
    // read (the slow path cannot pop the per-node SPSC rings — the hot thread is
    // their single consumer — so it needs a published snapshot).
    //
    // PARITY-047 — this REPLACES two independent `std::atomic<double>` lanes whose
    // own comment declared them "informational, not load-bearing" while the slow
    // path's rolling / cum-delta / flow ingest had come to depend on them, and
    // which had no lane at all for `is_buyer_maker`. Three separate relaxed lanes
    // could each land from a DIFFERENT tick; for the two continuous fields that is
    // a small numeric error, but `is_buyer_maker` is CATEGORICAL — a mis-pair
    // attributes a tick's volume to the wrong side outright and biases
    // volume_delta / cum-delta / the flow EWMAs directly. One seqlock window makes
    // the sample self-consistent BY CONSTRUCTION. Reuses the existing generic
    // ParameterSlot<T> (SSoT — no new primitive; already tsan-annotated).
    ParameterSlot<Tick<F>> latest_tick{};

    //----------------------------------------------------------------------
    // [SECTION]_[v5.15.4 — Mode-specific cfg normalize]
    //----------------------------------------------------------------------
    // When trading_mode=LIVE, auto-tighten safety defaults the operator
    // hasn't explicitly set: model_verify_strict 0→1 (STRICT);
    // reconcile_mode WARN→STRICT. Explicit operator overrides honored
    // via cfg_keys_explicit bitmap (parser sets bits at parse time).
    // Runs BEFORE LiveReadiness_Verify so the boot gate sees normalized
    // values. Paper/shadow modes are passthrough.
    ControllerConfig_NormalizeForMode<F>(cfg);

    //----------------------------------------------------------------------
    // [SECTION]_[v5.15.2 — Live-readiness boot gate]
    //----------------------------------------------------------------------
    // Pre-flight checklist via FOREACH_LIVE_READINESS_CHECK. When
    // trading_mode=LIVE, REFUSES boot if any LR_SEV_REFUSE-severity check
    // fails. When PAPER or SHADOW, logs failures as WARN-only + proceeds
    // (visibility-by-default so operators see the full checklist at every
    // boot before flipping to live).
    //
    // Drift state read directly from handle->drift_flags_at_load via
    // aggregate_zoo_drift helper — PerNodeSnap.failure_flags isn't
    // populated until snapshot publish (slow-path tail; AFTER pthread
    // spawns). Boot gate runs before pthread spawns; reads from
    // engine-side source of truth.
    {
        int lr_result = tt::LiveReadiness_Verify<F>(cfg, state);
        if (lr_result < 0) {
            fprintf(stderr,
                "[v5.15.2] engine exit: live-readiness REFUSED. "
                "trading_mode=live + pre-flight failure(s). "
                "Either fix the items above or set trading_mode=paper to ship-test.\n");
            return;
        }
    }

    //----------------------------------------------------------------------
    // [SECTION]_[GUI thread (ImGui build only)]
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

    // Reset Paper coordination flag for per-core slow-path threads.
    // Producer's reset handler sets this before touching shared state,
    // slow-paths park on it, producer clears when reset completes.
    // Declared at EngineSharded_Run scope so BOTH the producer thread
    // lambda (which writes it during reset) AND the per-core slow-path
    // lambdas (which read it at top of poll loop) can capture it by
    // reference.
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
        topo_hot_cpu[i]  = (i < num_nodes) ? (i + 1) : -1;
        topo_slow_cpu[i] = -1;
        topo_poll_interval[i] = (i < num_nodes)
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
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — boot validation now reads per-core fee_rate_taker
    // (Pattern 2 — per-core in-scope; cfg.nodes[i] in loop scope). Each core's tp floor
    // is computed against its own fee_rate_taker since per-core fee rates can differ.
    for (int i = 0; i < num_nodes; ++i) {
        double fee_taker_i = Money_ToDouble(cfg.nodes[i].fee_rate_taker);
        if (fee_taker_i <= 0.0) fee_taker_i = Money_ToDouble(cfg.nodes[i].fee_rate);  // fallback
        double tp_floor_i = 3.0 * fee_taker_i;
        double tp_pct = Money_ToDouble(cfg.nodes[i].take_profit_pct);
        if (tp_pct > 0.0 && tp_pct < tp_floor_i) {
            fprintf(stderr,
                "[sharded] WARN: node %d take_profit_pct=%.4f%% is below "
                "the fee floor (3 × taker=%.4f%% = %.4f%%). Winning trades "
                "will be net-negative after fees. Recommend tp_pct >= %.4f%%.\n",
                i, tp_pct * 100.0, fee_taker_i * 100.0,
                tp_floor_i * 100.0, tp_floor_i * 100.0);
        }
    }
    int  topo_producer_cpu = 0;
    int  topo_drainer_cpu  = EngineSharded_DrainerCpu(num_nodes);  // CPU N+1
    long topo_nproc        = sysconf(_SC_NPROCESSORS_ONLN);
    if (topo_nproc < 1) topo_nproc = 1;

    //----------------------------------------------------------------------
    // [SECTION]_[Producer thread — generates synthetic ticks and fans out to all cores]
    //----------------------------------------------------------------------
    std::thread producer([&producer_done, &ticks_produced, &bcfg, &latest_tick,
                          &cfg, &state, &oms, num_nodes, use_synthetic, tsc_ghz,
                          &ema_price, &ema_alpha, live_trading,
                          &paper_reset_in_progress,
                          &topo_hot_cpu, &topo_slow_cpu, &topo_poll_interval,
                          topo_producer_cpu, topo_drainer_cpu, topo_nproc] {
        EngineSharded_PinThread(topo_producer_cpu);  // producer → CPU 0
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

        // fan_out body extracted to tt::EngineSharded_Async_FanOut<F>() in
        // EngineSharded/Async.hpp at v5.15.5.F.4d.1.B.6 Phase B Step B.2.
        // Thin wrapper lambda preserves the producer thread's `fan_out(...)`
        // call-site shape (2 callers below: synthetic loop + real Binance loop).
        // Lambda captures everything the hoisted function needs as explicit args
        // and forwards through; under non-GUI build, shared_ptr + candle_acc_ptr
        // are nullptr (the hoisted body's #ifdef gates elide their use).
#ifdef USE_IMGUI_GUI
        TUISharedState*    shared_ptr_for_fanout    = &g_shared;
        CandleAccumulator* candle_acc_ptr_for_fanout = &g_candle_acc;
#else
        TUISharedState*    shared_ptr_for_fanout    = nullptr;
        CandleAccumulator* candle_acc_ptr_for_fanout = nullptr;
#endif
        auto fan_out = [num_nodes, &seq, &ticks_produced, &latest_tick,
                        &cfg, &state, &oms, &slow_path_counter, slow_path_interval, tsc_ghz,
                        &ema_price, ema_alpha, live_trading,
                        &paper_reset_in_progress,
                        &topo_hot_cpu, &topo_slow_cpu, &topo_poll_interval,
                        topo_producer_cpu, topo_drainer_cpu, topo_nproc,
                        shared_ptr_for_fanout, candle_acc_ptr_for_fanout]
                       (double price_d, double volume_d, uint64_t ts_us,
                        int is_buyer_maker = 0) -> bool {  // v4.3 — defaulted for synthetic paths
            return tt::EngineSharded_Async_FanOut<F>(
                price_d, volume_d, ts_us, is_buyer_maker,
                // by-value captures
                num_nodes, slow_path_interval, tsc_ghz, ema_alpha, live_trading,
                topo_producer_cpu, topo_drainer_cpu, topo_nproc,
                // by-ref captures
                seq, ticks_produced, latest_tick,
                cfg, state, oms, slow_path_counter, ema_price,
                paper_reset_in_progress,
                topo_hot_cpu, topo_slow_cpu, topo_poll_interval,
                // file-local-static args (block-scope statics in EngineSharded_Run;
                // captured into the wrapper lambda via local access, then forwarded).
                tick_rings, nodes, g_tick_rec, g_depth_shared,
                shared_ptr_for_fanout, candle_acc_ptr_for_fanout);
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
    // [SECTION]_[Executor threads — one per execution core]
    //----------------------------------------------------------------------
    // Lambdas can't capture static arrays directly, so we pass the index
    // in by value and let the lambda body reference the file-scope statics.
    std::vector<std::thread> executors;
    executors.reserve(num_nodes);
    for (int i = 0; i < num_nodes; ++i) {
        executors.emplace_back([i, &producer_done, &ticks_consumed_total] {
            EngineSharded_PinThread(EngineSharded_NodeHotCpu(i));  // node i hot → CPU i+1 (CPU 0 = producer)
            Tick<F> t;
            uint64_t local_consumed = 0;
            while (!g_engine_sharded_shutdown) {
                if (SPSCRing_TryPop(&tick_rings[i], &t)) {
                    ExecutionCore_Tick(&nodes[i], t);
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
    // [SECTION]_[Drainer thread — controller side]
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
    //   entry: read from nodes[slot].intended_qty (matches what OnEvent will
    //          write into portfolio.positions[slot].quantity via OpenSlot)
    //   exit:  read from portfolio.positions[slot].quantity BEFORE OnEvent,
    //          since CloseSlot inside OnEvent clears the slot

    // drain_post_fill + drain_manual_closes lambdas hoisted to
    // EngineSharded/SlowPath.hpp at v5.15.5.F.4d.1.B.6 (Phase B Step B.3).
    //   - EngineCommon_DrainPostFill: the SHARED cfg->args binder into EventLoop_DrainPostFill (E.1.2.C leg 0)
    //   - EngineSharded_SlowPath_DrainManualCloses: MERGED LIVE+NO-OP variants per
    //     Decision H. #ifdef USE_IMGUI_GUI gate moved INSIDE function body. Single source
    //     of truth + sister to .B.4 EngineCommon_BootPerCore dual-cfg shape.
    // shared_ptr declared unconditionally (nullptr under non-GUI build) so the
    // call site is unconditional: SlowPath_DrainManualCloses(state, oms, cfg,
    // latest_tick, shared_ptr). Body's #ifdef inside gates the dereference.
#ifdef USE_IMGUI_GUI
    TUISharedState* shared_ptr = &g_shared;
#else
    TUISharedState* shared_ptr = nullptr;
#endif

    // v5.15.5.F.4d.1.B.6 Phase B Step B.2: drain_with_submit hoisted to
    // EngineSharded/Async.hpp as tt::EngineSharded_Async_DrainWithSubmit<F>().
    // Call sites below pass (state, oms, ticks_produced, cfg) explicitly.
    std::thread drainer([&state, &oms, &ticks_produced, &producer_done,
                         &cfg, &latest_tick, shared_ptr] {
        // v5.15.5.C.4 Phase T1: &cfg added to capture list for DrainerConstants_Init
        // v5.15.5.C.4 Phase F — drainer-local bucket arrays for phase-separated
        // dispatch. ~7 KB stack allocation; reused per cycle (Reset at top of
        // DrainIntoBuckets). NOT added to OmsState (transient per-cycle scratch).
        tt::OmsDrainBuckets drain_buckets;
        EngineSharded_PinThread(state.registered_count + 1);
        while (!g_engine_sharded_shutdown) {
            // v5.15.5.C.3 Phase 7.B — bench gate per-cycle rdtsc bracket.
            // Wraps the 4-step drainer cycle below. Compile-time elided when
            // BENCH=false (production); zero instructions emitted into the
            // drainer body. When BENCH=true, ~10-15 cycle overhead per
            // cycle (rdtsc + histogram bucket bump + min/max compare).
            uint64_t _bench_t0 = 0;
            if constexpr (BENCH) {
                _bench_t0 = (uint64_t)__rdtsc();
            }
            // Sequence per cycle:
            //   1. drain_with_submit / drain_manual_closes — these push
            //      SubmitCommands into oms.submit_queues (v4.7.37; was direct
            //      OrderManager_Submit calls before Phase B).
            //   2. OMS_DrainSubmit — drainer (sole Submit caller) pops the
            //      queues and calls Submit serially. Preserves OMS contract.
            //   3. OrderManager_Tick — drains result_queue / ws_result_queue
            //      / reconcile_queue, calls HandleFill on completed orders.
            //   4. drain_post_fill — applies per-core NodeContext updates
            //      from FillRecords.
            //
            // v5.15.5.C.4 Phase T1 — DrainerConstants cache (drainer-thread-
            // stable cfg + state predicates). One init per cycle; consumers
            // read fields directly. Replaces prior 6× scattered
            // BITMAP_IS_SET(oms_state_flags, PARTIAL_EXIT_ENABLED) reads.
            const tt::DrainerConstants dc =
                tt::DrainerConstants_Init(state.registered_count, cfg, oms);

            int total_drained = tt::EngineSharded_Async_DrainWithSubmit<F>(state, oms, ticks_produced, cfg);
            EngineSharded_SlowPath_DrainManualCloses(state, oms, cfg, latest_tick, shared_ptr);
            OMS_DrainSubmit(&oms, dc.drain_count);  // v4.7.37; v5.15.5.C.4 T1 uses cached dc.drain_count

            // v5.15.5.C.4 Phase F — phase-separated drain (replaces unified
            // OrderManager_Tick). Drain commands into per-direction buckets;
            // process Phase A (closes) → Phase A.5 (DrainPostFill consumer
            // pass; reads CLOSE-form Position state — unlocks Phase G+H
            // derives) → Phase B (opens; Portfolio_OpenSlot fires here) →
            // Phase C (reconciles; phase-invariant safe). See
            // DESIGN_SPECS/phase-separated-drainer-for-safe-cross-temporal-derives.md.
            tt::OrderManager_DrainIntoBuckets(&oms, &drain_buckets);
            tt::OrderManager_ProcessBucket_Closes(&oms, &drain_buckets);  // Phase A
            EngineCommon_DrainPostFill(state, oms, cfg);  // E.1.2.C leg 0 — the shared binder (was EngineSharded_SlowPath_DrainPostFill)                                              // Phase A.5
            tt::OrderManager_ProcessBucket_Opens(&oms, &drain_buckets);   // Phase B
            tt::OrderManager_ProcessBucket_Reconciles(&oms, &drain_buckets);  // Phase C

            // ── Ship-B P3 (S-17): sticky money-flag drain — drainer cycle tail ──
            // Observational only (never feeds back into math; replay runs the same
            // path). OVERFLOW/DIVZERO on the drainer thread = saturated money op
            // somewhere in the cycle — loud operator signal, then re-arm.
            if (__builtin_expect(money_op_flags != 0, 0)) {
                std::fprintf(stderr,
                    "[drainer] MONEY FLAGS tripped this cycle: %s%s— investigate "
                    "(saturation is deterministic but means a value left the "
                    "money closure domain)\n",
                    (money_op_flags & MONEY_FLAG_OVERFLOW) ? "OVERFLOW " : "",
                    (money_op_flags & MONEY_FLAG_DIVZERO)  ? "DIVZERO "  : "");
                money_op_flags = 0;
            }

            if constexpr (BENCH) {
                uint64_t _bench_dt = (uint64_t)__rdtsc() - _bench_t0;
                LatencyHistogram_Accumulate(&g_engine_drainer_cycle_hist, _bench_dt);
            }

            if (total_drained == 0) std::this_thread::yield();
            if (producer_done.load(std::memory_order_acquire)) {
                for (int k = 0; k < 16; ++k) {
                    tt::EngineSharded_Async_DrainWithSubmit<F>(state, oms, ticks_produced, cfg);
                    EngineSharded_SlowPath_DrainManualCloses(state, oms, cfg, latest_tick, shared_ptr);
                    // v5.4.1 Bug B2: same partials-aware drain count as the
                    // main loop above. v5.15.5.C.4 Phase T1: per-iter dc
                    // recompute (state may have shifted across k iterations).
                    const tt::DrainerConstants dc_shutdown =
                        tt::DrainerConstants_Init(state.registered_count, cfg, oms);
                    OMS_DrainSubmit(&oms, dc_shutdown.drain_count);  // v4.7.37
                    // v5.15.5.C.4 Phase F — phase-separated drain (shutdown
                    // path mirrors main loop). drain_buckets is the drainer-
                    // thread-local bucket array declared at lambda entry.
                    tt::OrderManager_DrainIntoBuckets(&oms, &drain_buckets);
                    tt::OrderManager_ProcessBucket_Closes(&oms, &drain_buckets);
                    EngineCommon_DrainPostFill(state, oms, cfg);  // E.1.2.C leg 0 — the shared binder (was EngineSharded_SlowPath_DrainPostFill)
                    tt::OrderManager_ProcessBucket_Opens(&oms, &drain_buckets);
                    tt::OrderManager_ProcessBucket_Reconciles(&oms, &drain_buckets);
                }
                break;
            }
        }
    });

    // Spawn N per-core slow-path threads. Each runs OneCore helpers
    // (RebuildOneCore, TimeExitOneCore, TrailingSLRatchetOneCore) on
    // a fixed cadence. Producer continues doing GLOBAL work
    // (RollingStats pushes, depth state, snapshot save, GUI publish,
    // account-level KillSwitchEvaluate).
    //
    // v5.0.2: slow-path pinning. cfg.slow_path_pin_offset:
    //   < 0  → no pin (OS-scheduled, original v5.0 behavior)
    //   == 0 → auto: base = drainer_cpu + 1 = num_nodes + 2
    //   > 0  → explicit base
    // Slow-path c pins to (base + c) mod nproc. HT-sharing acceptable
    // (slow-path is jitter-tolerant). Best-effort: pin failure is logged
    // and execution continues unpinned.
    //
    // Reset Paper coordination: paper_reset_in_progress atomic declared
    // earlier (the InitArena boot block above) so fan_out lambda's reset handler can reference
    // it. Slow-paths park (yield) when set; producer's reset handler sets,
    // runs reset, clears.
    std::vector<std::thread> slow_paths;
    {
        // v5.1.5: smart pin selection. With cfg.slow_path_pin_offset == 0
        // (auto), use SmartSlowPathPins to AVOID landing on SMT siblings of
        // producer/hot-path/drainer CPUs. Pre-v5.1.5 the auto-derive was a
        // simple (num_nodes + 2 + c) % nproc, which on a 16-thread Intel
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
                /*num_hot=*/num_nodes,
                /*num_slow=*/num_nodes,
                sp_pins);
            if (smart_ok) {
                sp_pin_base = sp_pins[0];  // for log only
                fprintf(stderr, "[sharded] spawning %d "
                                "slow-path threads (smart pin: ", num_nodes);
                for (int c = 0; c < num_nodes; ++c) {
                    fprintf(stderr, "%s%d", c ? "," : "", sp_pins[c]);
                }
                fprintf(stderr, ", nproc %ld)\n", nproc);
            } else {
                // Fallback: simple (NodeSlowCpuBase(N) + c) % nproc = (num_nodes + 2 + c) % nproc
                sp_pin_base = EngineSharded_NodeSlowCpuBase(num_nodes);  // CPU N+2.. fallback base
                fprintf(stderr, "[sharded] spawning %d "
                                "slow-path threads (smart pin failed; fallback base CPU %d, nproc %ld)\n",
                        num_nodes, sp_pin_base, nproc);
            }
        } else if (cfg.slow_path_pin_offset > 0) {
            sp_pin_base = cfg.slow_path_pin_offset;
            fprintf(stderr, "[sharded] spawning %d "
                            "slow-path threads (explicit pin base CPU %d, nproc %ld)\n",
                    num_nodes, sp_pin_base, nproc);
        } else {
            // < 0: no pin
            fprintf(stderr, "[sharded] spawning %d "
                            "slow-path threads (UNPINNED, slow_path_pin_offset=%d)\n",
                    num_nodes, cfg.slow_path_pin_offset);
        }
        slow_paths.reserve(num_nodes);
        for (int c = 0; c < num_nodes; ++c) {
            int sp_cpu;
            if (smart_ok) {
                sp_cpu = sp_pins[c];
            } else if (sp_pin_base >= 0) {
                sp_cpu = (int)((sp_pin_base + c) % nproc);
            } else {
                sp_cpu = -1;
            }
            topo_slow_cpu[c] = sp_cpu;  // v5.0.2: capture for topology panel
            slow_paths.emplace_back([c, sp_cpu, &state, &oms, &nodes, &cfg,
                                      &ticks_produced, &latest_tick,
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
                    (unsigned)cfg.node_overrides[c].poll_interval,
                    (unsigned)cfg.poll_interval,
                    sp_cpu);
                // v4.7.42 (Phase E): enable per-core slow-path latency stats.
                // Sampled around the per-cycle work below (RebuildOneCore +
                // PushParameters + TimeExitOneCore + TrailingSL + permission).
                NodeLatencyStats_Enable(&state.display_meta[c].slow_path_latency);
                // v5.1.1: enable per-section breakdown stats.
                for (int s = 0; s < tt::SP_SECTION_COUNT; ++s) {
                    NodeLatencyStats_Enable(&state.display_meta[c].slow_path_breakdown[s]);
                }
                uint64_t last_seen_tick = 0;
                while (!g_engine_sharded_shutdown) {
                    // v5.0.3: user pause via paused_engines_mask bit c.
#ifdef USE_IMGUI_GUI
                    if (g_shared.paused_engines_mask & (uint16_t)(1u << c)) {
                        state.nodes[c].sp_telemetry.state.store(3, std::memory_order_relaxed);
                        state.nodes[c].sp_telemetry.yield_count.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                        continue;
                    }
#endif
                    // Reset Paper coordination — park while reset runs.
                    if (paper_reset_in_progress.load(std::memory_order_acquire)) {
                        state.nodes[c].sp_telemetry.state.store(1, std::memory_order_relaxed);
                        state.nodes[c].sp_telemetry.yield_count.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                        continue;
                    }
                    // Cadence — wake when enough ticks have passed.
                    uint64_t now_tick = ticks_produced.load(std::memory_order_acquire);
                    if (now_tick - last_seen_tick < (uint64_t)slow_path_interval) {
                        state.nodes[c].sp_telemetry.state.store(2, std::memory_order_relaxed);
                        state.nodes[c].sp_telemetry.yield_count.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                        continue;
                    }
                    last_seen_tick = now_tick;
                    state.nodes[c].sp_telemetry.state.store(0, std::memory_order_relaxed);  // running

                    // v5.15.5.F.4d.1.B.4 Step C.3 — slow-path latency telemetry
                    // (v4.7.42 outer bracket + v5.1.1 per-section breakdown) moved
                    // INSIDE EngineCommon_SlowPathCycleOneCore body per v1.7.3 HIGH-4
                    // (Telemetry Path A INTERNAL); helper computes its own _sp_t0 +
                    // 5 NodeLatencyStats_Sample calls.

                    // Skip cores with STRATEGY_NONE (caller responsibility per
                    // OneCore contract; OneCore would no-op on STRATEGY_NONE
                    // body because Strategy_BuildParameters dispatcher skips it,
                    // but the explicit check here is cheap and clarifying).
                    if (state.nodes[c].strategy_id == STRATEGY_NONE) continue;

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
                            // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
                            int partial_on = BITMAP_IS_SET(state.oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
                            uint16_t open_mask = BITMAP_NODE_SLOT_MASK(c, partial_on);
                            if ((state.oms->portfolio.active_bitmap & open_mask) == 0) {
                                // E.1.2.C leg 3 — ensemble-aware (was
                                // single-zoo-blind; shares the LiveReadiness
                                // node_has_serving_model predicate).
                                if (pending == STRATEGY_ML &&
                                    !tt::node_has_serving_model(state.nodes[c])) {
                                    fprintf(stderr,
                                        "[slow-path-%d] refusing swap to ML — "
                                        "no single-zoo model and no ready ensemble\n", c);
                                    __atomic_store_n(
                                        &g_shared.swap_strategy_requested[c],
                                        STRATEGY_NONE, __ATOMIC_RELEASE);
                                } else {
                                    uint8_t old_strat = state.nodes[c].strategy_id;
                                    state.nodes[c].strategy_id = pending;
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
                            // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
                            int partial_on = BITMAP_IS_SET(state.oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
                            uint16_t open_mask = BITMAP_NODE_SLOT_MASK(c, partial_on);
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
                                        "[hot_swap] node %d REFUSED: empty path\n", c);
                                    __atomic_store_n(
                                        &g_shared.swap_model_path_requested[c], 0,
                                        __ATOMIC_RELEASE);
                                } else {
                                    // Gate ORDER is the fix (third sibling of
                                    // the E.1.2.C leg-3 single-zoo-blind class;
                                    // the dispatch gate + strategy-swap gate got
                                    // it first): an ensemble-only node — the ONLY
                                    // layout the trainer produces since D-431 —
                                    // has ensemble_handle set while model_handle
                                    // stays NULL (boot 5c sets it only when the
                                    // single zoo loads a role). So: ensemble
                                    // first, single-zoo second, REFUSE only when
                                    // BOTH are null (node not ML at boot — no
                                    // arena of either kind to swap into).
                                    if (state.nodes[c].ensemble_handle != nullptr) {
                                        // v5.15.4 — ENSEMBLE SHADOW-LOAD HOT-SWAP.
                                        // Replaces v5.14.2's in-place Free+Init+Load
                                        // pattern (its EnsembleHotSwap.hpp was
                                        // DELETED at E.1.2.C — zero production
                                        // callers, H21). PARITY-023's broken
                                        // capture-pointer Revert design replaced by
                                        // shadow-load discipline per
                                        // DESIGN_SPECS/shadow-load-state-transition-pattern.md.
                                        //
                                        // Helper:
                                        //   1. Allocates NEW ezoo via aligned_alloc(64)
                                        //   2. Loads + PostLoadSetup into new_ezoo
                                        //   3. Atomically swaps state.nodes[c].ensemble_handle
                                        //   4. Free's old ezoo
                                        // Pre-swap untouched on any failure; caller
                                        // sees nonzero rc and continues serving from
                                        // pre-swap state.
                                        int swap_backend = cfg.ml_backend
                                            ? cfg.ml_backend
                                            : MODEL_BACKEND_XGBOOST;
                                        int rc = tt::HotSwap_ShadowLoad_Ensemble<F>(
                                            state, c, cfg, new_path, swap_backend);
                                        if (rc != 0) {
                                            // Pre-swap state preserved automatically;
                                            // helper already logged the specific
                                            // failure mode. Leave model_load_failed
                                            // at its pre-call value (likely 0 if
                                            // pre-swap was healthy).
                                            fprintf(stderr,
                                                "[hot_swap] ensemble node %d shadow-load "
                                                "FAILED (rc=%d); pre-swap state preserved\n",
                                                c, rc);
                                        } else {
                                            NODE_STATE_FLAG_CLR(state.nodes[c], MODEL_LOAD_FAILED);
                                            NODE_STATE_FLAG_CLR(state.nodes[c], MODEL_CORRUPT);  // v5.15.5.E.0.10 A6 (D-221) — new model starts clean
                                            // Re-fetch ezoo after swap to run post-load
                                            // validators on the NEW ezoo. v5.14.2.E.1
                                            // closes PARITY-009.F: ValidateAgainstCfg +
                                            // FeatureOverlay_PostLoadVerify still run
                                            // on hot-swap (was bypassed pre-v5.14.2.E.1).
                                            EnsembleModelZoo<F>* swap_ezoo =
                                                (EnsembleModelZoo<F>*)state.nodes[c].ensemble_handle;
                                            // D-h §1A — node-resolved view (see EngineCommon boot sister).
                                            ControllerConfig<F> vcfg_e = ControllerConfig_ResolveForCore(cfg, c);
                                            int validate_rc = NodeModelZoo_ValidateAgainstCfg<F>(
                                                /*zoo=*/nullptr,
                                                swap_ezoo,
                                                vcfg_e, /*node_id=*/c,
                                                vcfg_e.held_out_gate_strict,
                                                (int)BITMAP_IS_SET(vcfg_e.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_INFERENCE_CFG_DRIFT),
                                                (int)BITMAP_IS_SET(vcfg_e.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT),
                                                &state.display_meta[c], &state.nodes[c]);
                                            if (validate_rc < 0) {
                                                NODE_STATE_FLAG_SET(state.nodes[c], MODEL_LOAD_FAILED);
                                                fprintf(stderr,
                                                    "[hot_swap] ensemble node %d "
                                                    "REFUSED post-load validation "
                                                    "in strict mode; new model "
                                                    "loaded but flagged degraded. "
                                                    "Operator must reconcile cfg "
                                                    "vs stamp + restart.\n", c);
                                            }
                                            int overlay_rc = FeatureOverlay_PostLoadVerify<F>(
                                                /*zoo=*/nullptr, swap_ezoo,
                                                /*node_id=*/c, cfg.held_out_gate_strict);
                                            if (overlay_rc < 0) {
                                                NODE_STATE_FLAG_SET(state.nodes[c], MODEL_LOAD_FAILED);
                                            }
                                            // v5.15.5.E.0.10 A6 ingress (D-221) — post-swap corrupt finalize on
                                            // the NEW ezoo (mirror of the boot path; same slow-path thread, AFTER
                                            // the ACQ_REL ensemble_handle swap inside HotSwap_ShadowLoad_Ensemble).
                                            if (EnsembleZoo_FinalizeCorrupt<F>(swap_ezoo, FPN_ToDouble(cfg.model_corrupt_shalt_ratio))) {
                                                NODE_STATE_FLAG_SET(state.nodes[c], MODEL_CORRUPT);
                                                fprintf(stderr, "[hot_swap] node %d: ML barrier CORRUPT for the "
                                                                "majority of arms — node REFUSES new trades; RETRAIN (D-221)\n", c);
                                            }
                                        }
                                        __atomic_store_n(
                                            &g_shared.swap_model_path_requested[c], 0,
                                            __ATOMIC_RELEASE);
                                    } else if (state.nodes[c].model_handle != nullptr) {
                                        // v5.15.4 — SINGLE-ZOO SHADOW-LOAD HOT-SWAP.
                                        // Replaces in-place Free+Init+LoadFromDir
                                        // (v5.10.0c "log-and-leave" pattern; brief
                                        // empty-zoo window). Shadow-load discipline:
                                        // allocate NEW zoo + Load + PostLoadSetup,
                                        // atomically swap, Free old. Pre-swap
                                        // untouched on any failure → no empty-zoo
                                        // window; failed swaps preserve state.
                                        int swap_backend = cfg.ml_backend
                                            ? cfg.ml_backend
                                            : MODEL_BACKEND_XGBOOST;
                                        int rc = tt::HotSwap_ShadowLoad_SingleZoo<F>(
                                            state, c, cfg, new_path, swap_backend);
                                        if (rc != 0) {
                                            // Pre-swap state preserved by helper;
                                            // log + continue. Don't null the handle;
                                            // pre-swap zoo remains active.
                                            fprintf(stderr,
                                                "[hot_swap] single-zoo node %d shadow-load "
                                                "FAILED (rc=%d); pre-swap state preserved\n",
                                                c, rc);
                                        } else {
                                            NODE_STATE_FLAG_CLR(state.nodes[c], MODEL_LOAD_FAILED);
                                            // Re-fetch zoo after swap to run post-load
                                            // validators on the NEW zoo (parity-check
                                            // Finding #3 closure preserved).
                                            NodeModelZoo<F>* new_swap_zoo =
                                                (NodeModelZoo<F>*)state.nodes[c].model_handle;
                                            // D-h §1A — node-resolved view (see EngineCommon boot sister).
                                            ControllerConfig<F> vcfg_s = ControllerConfig_ResolveForCore(cfg, c);
                                            int validate_rc = NodeModelZoo_ValidateAgainstCfg<F>(
                                                new_swap_zoo,
                                                /*ezoo=*/nullptr,
                                                vcfg_s, /*node_id=*/c,
                                                vcfg_s.held_out_gate_strict,
                                                (int)BITMAP_IS_SET(vcfg_s.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_INFERENCE_CFG_DRIFT),
                                                (int)BITMAP_IS_SET(vcfg_s.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT),
                                                &state.display_meta[c], &state.nodes[c]);
                                            if (validate_rc < 0) {
                                                NODE_STATE_FLAG_SET(state.nodes[c], MODEL_LOAD_FAILED);
                                                fprintf(stderr,
                                                    "[hot_swap] node %d REFUSED post-load "
                                                    "validation in strict mode; new model "
                                                    "loaded but flagged degraded. Operator "
                                                    "must reconcile cfg vs stamp + restart.\n", c);
                                            }
                                            int overlay_rc = FeatureOverlay_PostLoadVerify<F>(
                                                new_swap_zoo, /*ezoo=*/nullptr,
                                                /*node_id=*/c, cfg.held_out_gate_strict);
                                            if (overlay_rc < 0) {
                                                NODE_STATE_FLAG_SET(state.nodes[c], MODEL_LOAD_FAILED);
                                            }
                                        }
                                        __atomic_store_n(
                                            &g_shared.swap_model_path_requested[c], 0,
                                            __ATOMIC_RELEASE);
                                    } else {
                                        // Neither handle exists — node was not
                                        // ML at boot. (Ensemble-only nodes no
                                        // longer land here; gate-order comment
                                        // above.)
                                        fprintf(stderr,
                                            "[hot_swap] node %d REFUSED: no "
                                            "single-zoo model and no ensemble "
                                            "(set node_%d_strategy=ml + restart "
                                            "to enable hot-swap)\n", c, c);
                                        __atomic_store_n(
                                            &g_shared.swap_model_path_requested[c], 0,
                                            __ATOMIC_RELEASE);
                                    }
                                }
                            }
                        }
                    }
#endif
                    // v5.15.5.F.4d.1.B.4 Step C.3 — extracted to
                    // EngineCommon_SlowPathCycleOneCore (closes PARITY-032 by-construction
                    // via D1-B BREAKEVEN_ON_PROFIT cached-gate dispatch sister to
                    // TrailingSLRatchet; BACKTEST sister at Step C.4 invokes AllCores
                    // wrapper). Helper body preserved verbatim from prior inline at LIVE
                    // the per_node_slow lambda body → EngineCommon_SlowPathCycleOneCore in CoreFrameworks/EngineCommon.hpp. Caller resolves
                    // per-cycle scalars per v1.6 O2 bytewise-identical math (price =
                    // mtm_price per v1.7.3 HIGH-1) + v1.7.3 N-6 9-arg with BookSnapshot<F>
                    // sister-canonical reuse. Telemetry Path A INTERNAL (helper computes
                    // own rdtsc bracket + 5 NodeLatencyStats_Sample calls inside body per
                    // v1.7.3 HIGH-4).

                    // Per-cycle scalar inputs (mtm_price discipline preserved; helper takes
                    // Money price = mtm_price, derives double internally via FPN_ToDouble
                    // for guard checks).
                    // PARITY-047 — ONE seqlock'd read yields (price, volume, side) from
                    // the SAME tick. Previously price and volume came from two independent
                    // relaxed lanes and the side bit had no lane at all, so
                    // EngineCommon_SlowPathCycleOneCore was handed a hardcoded
                    // /*is_buyer_maker=*/0 — which pinned volume_delta at exactly +1.0 and
                    // left cum-delta / the flow EWMAs one-sided on the LIVE path while the
                    // backtest driver fed them the real bit (a train-serve divergence on
                    // FEAT_CUMDELTA + FEAT_FLOW_10S/1M/5M, and a dead FEAT_VOLUME_DELTA).
                    //
                    // Money values are UNCHANGED: the producer built latest.price/.volume
                    // from the same doubles via the same money_from_double_payload call.
                    // The `> 0` guard is preserved exactly — it encodes "no tick seen yet",
                    // which ControllerEventLoop turns into a Money_IsZero early-return.
                    Tick<F> latest{};
                    ParameterSlot_Read(&latest_tick, &latest);
                    Money price  = Money_Gt(latest.price,  Money_Zero())
                                 ? latest.price  : Money_Zero();
                    Money volume = Money_Gt(latest.volume, Money_Zero())
                                 ? latest.volume : Money_Zero();
                    const int tick_is_buyer_maker = (int)latest.is_buyer_maker;
                    uint64_t ts_us =
                        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();

                    // Depth snapshot — existing BookSnapshot<F> ref from g_depth_shared
                    // static (LIVE; BACKTEST sister uses ShardedBacktestDriver<F> pointer
                    // fields at Step C.4). Helper checks MASK_GATE_CFG_DEPTH_ENABLED
                    // internally per v1.7.3 N-6 + canonical LIVE :3052-3058 pattern (no
                    // `enabled` field on BookSnapshot).
                    int dactive = __atomic_load_n(&g_depth_shared.active_idx, __ATOMIC_ACQUIRE);
                    const BookSnapshot<F>& depth = g_depth_shared.snapshots[dactive];

                    // Helper call (v1.7.3 N-6 9-arg signature)
                    EngineCommon_SlowPathCycleOneCore(cfg, c, state, oms,
                                                       price, volume, ts_us,
                                                       tick_is_buyer_maker,
                                                       now_tick, depth);

                    // NOTE: DrainPostFill stays on the drainer thread (single writer of
                    // last_*_mask is HandleFill on drainer; same thread reads + clears
                    // via DrainPostFill wrapper). NOTE: KillSwitchEvaluate is GLOBAL
                    // (account-level drawdown), runs on producer thread. Per-core kill
                    // switch state is mutated INSIDE RebuildOneCore (now inside helper).
                    // NOTE: Drag TP/SL pickup + manual close stay on drainer + producer
                    // threads respectively. They submit via OMS_PushSubmit (Phase B) —
                    // thread-safe.
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
        fprintf(stdout, SH_BOLD SH_PEACH "  /l、" SH_RESET "  " SH_BOLD SH_PEACH "FOXML TRADER" SH_RESET "  " SH_DIM "(per-node sharded)" SH_RESET "\033[K\n");
        fprintf(stdout, SH_DIM " ( °_ ° 7" SH_RESET "  " SH_DIM "engine v3.7.2" SH_RESET "  " SH_FG "%s" SH_RESET "\033[K\n",
                use_synthetic ? "synthetic ticks" : "real Binance feed");
        fprintf(stdout, SH_DIM "  ド  ヘ" SH_RESET "\033[K\n");
        fprintf(stdout, SH_DIM " じし_,)ノ" SH_RESET "\033[K\n");
        fprintf(stdout, "\033[K\n");

        // Top bar
        auto now = std::chrono::steady_clock::now();
        long uptime = std::chrono::duration_cast<std::chrono::seconds>(now - t_start).count();
        double bal = Money_ToDouble(state.oms->balance);
        double pnl = Money_ToDouble(state.oms->realized_pnl);
        int active = __builtin_popcount(state.oms->portfolio.active_bitmap);
        fprintf(stdout, " " SH_DIM "STATE: " SH_RESET SH_FG "ACTIVE" SH_RESET
                "  " SH_DIM "│" SH_RESET "  " SH_DIM "UPTIME: " SH_RESET SH_FG "%02ld:%02ld:%02ld" SH_RESET
                "  " SH_DIM "│" SH_RESET "  " SH_DIM "MODE: " SH_RESET SH_PEACH "SHARDED" SH_RESET "\033[K\n",
                uptime / 3600, (uptime / 60) % 60, uptime % 60);
        fprintf(stdout, "\033[K\n");

        // Market + account
        // PARITY-047 — display reads the same published sample as the slow path;
        // no separate price/volume lanes remain. Money_ToDouble is DISPLAY-ONLY
        // (H4-exempt) and is the correct bridge here.
        Tick<F> _disp{};
        ParameterSlot_Read(&latest_tick, &_disp);
        double price_d = Money_ToDouble(_disp.price);
        double vol_d   = Money_ToDouble(_disp.volume);
        // compute equity = balance + unrealized P&L across all open positions
        double unrealized = 0.0;
        {
            uint16_t bm = state.oms->portfolio.active_bitmap;
            while (bm) {
                int s = __builtin_ctz(bm);
                bm &= (uint16_t)(bm - 1);
                double entry = Money_ToDouble(state.oms->portfolio.positions[s].entry_price);
                double qty   = Money_ToDouble(state.oms->portfolio.positions[s].quantity);
                unrealized += (price_d - entry) * qty;
            }
        }
        double equity = bal + unrealized;
        fprintf(stdout, " " SH_DIM " PRICE " SH_RESET SH_BOLD SH_WHEAT "$%.2f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "VOL " SH_RESET SH_FG "%.4f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "BAL " SH_RESET SH_FG "$%.2f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "EQUITY " SH_RESET SH_BOLD "%s$%.2f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "POS " SH_RESET SH_FG "%d/%u" SH_RESET "\033[K\n",
                price_d, vol_d, bal, SH_PNL(equity - 10000.0), equity, active, (unsigned)num_nodes);
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
        fprintf(stdout, SH_BOLD SH_PEACH " PER-CORE LATENCY" SH_RESET SH_DIM "  (last 256 samples per node)" SH_RESET "\033[K\n");
        fprintf(stdout, "  " SH_DIM "core   samples       min        p50        p95        p99        max        avg" SH_RESET "\033[K\n");
        for (int i = 0; i < num_nodes; ++i) {
            NodeLatencySnapshot ls = NodeLatencyStats_Snapshot(&nodes[i].latency_stats, tsc_ghz);
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
                double entry_d = Money_ToDouble(state.oms->portfolio.positions[s].entry_price);
                double qty_d   = Money_ToDouble(state.oms->portfolio.positions[s].quantity);
                double tp_d    = Money_ToDouble(state.oms->portfolio.positions[s].take_profit_price);
                double sl_d    = Money_ToDouble(state.oms->portfolio.positions[s].stop_loss_price);
                double unreal_d = (price_d - entry_d) * qty_d;
                const char* strat = (s < state.registered_count && state.nodes[s].strategy_id < NUM_STRATEGIES)
                    ? STRATEGY_SHORT_NAMES[state.nodes[s].strategy_id] : "?";
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
        // v5.15.5.C.2 (S3a + S4): canonical mirror via bit-packed oms_state_flags.
        if (ShardedSnapshot_Save<F>(&state, "data/sharded_snapshot.dat",
                                      BITMAP_IS_SET(oms.oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED))) {
            fprintf(stderr, "[snapshot] final save: data/sharded_snapshot.dat\n");
        } else {
            fprintf(stderr, "[snapshot] final save FAILED — next restart starts fresh\n");
        }
    }

    // v5.10.0a.G.9 — final bandit state save. Each active ensemble core
    // flushes to its own <node_model_dir>/bandit_state.json. Survives
    // restart so weights resume rather than re-learn from uniform.
    // Live AND paper modes both save (live: deployed weights inform
    // next session; paper: same thing for backtest-style sessions).
    // Reaches the ezoo via ctx.ensemble_handle (registered at
    // the boot per-node loop above) since the static array's name is scope-limited
    // to the init for-loop.
    for (int i = 0; i < num_nodes; ++i) {
        auto* ezoo = static_cast<EnsembleModelZoo<F>*>(
            state.nodes[i].ensemble_handle);
        if (ezoo && BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE) && BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY) &&
            cfg.node_model_dir[i][0]) {
            // s5 BT-6/BT-7 — ONE call for all four families (the hand-written
            // 4-block that lived here broke twice in four days), and the
            // destination is DERIVED from the ezoo's live save path rather than
            // the BOOT cfg dir. After an "Apply (live)" swap those diverge, and
            // this site was writing the swapped family's learned weights into
            // the PREVIOUS family's directory — where the next boot of that
            // bundle loaded them as its own (the bundle-id guard that should
            // have caught it is vacuous, BT-8).
            char state_dir[sizeof(ezoo->bandit_save_path)];
            EnsembleModelZoo_DeriveStateDir(ezoo, cfg.node_model_dir[i],
                                             state_dir, sizeof(state_dir));
            EnsembleModelZoo_SaveAllBanditState(ezoo, state_dir, "sharded", i);
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
    fprintf(stderr, "[sharded]   joining executors (%d)...\n", num_nodes);
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
    // (the Strategy_FreePerCore loop below). Pre-fix ordering
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
            Money_ToDouble(state.oms->balance));

    EngineSharded_DumpLatency<F>(nodes, num_nodes, tsc_ghz);

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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// `g_engine_drainer_cycle_hist` is now declared in EngineSharded/Async.hpp (v5.15.5.F.4d.1.B.6
// Phase B Step B.2; co-located with drain_with_submit hoist that originated the per-cycle
// rdtsc bracket). Inline (C++17); same single-shared-storage semantics. References from this
// file (LatencyHistogram_Reset call below, drainer thread BENCH-accumulate, shutdown summary
// at end of EngineSharded_Run) all resolve via `tt::g_engine_drainer_cycle_hist`.
//======================================================================
// [END_FUNCTION]_[EngineSharded_Run]
//======================================================================

}  // namespace tt
