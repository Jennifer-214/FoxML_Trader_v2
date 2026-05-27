// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [EngineSharded/Run/Utilities.hpp — TSC calibration + thread pin + SMT topology helpers]
//======================================================================================================
// Sub-sub-file of CoreFrameworks/EngineSharded/Run.hpp (split per file-size-split-discipline.md
// at v5.15.5.F.4d.1.B.6 Phase B Step B.4.1; second-tier subfolder pattern to bring Run.hpp
// under the 1,500-line source-header threshold).
//
// Contains:
//   - EngineSharded_CalibrateTscGhz — TSC frequency calibration (~50ms busy work).
//   - EngineSharded_PinThread — best-effort core pinning via pthread_setaffinity_np.
//   - EngineSharded_GetSiblingCPU — SMT sibling read from /sys/devices/system/cpu.
//   - EngineSharded_SmartSlowPathPins — slow-path CPU pin assignment avoiding SMT
//     siblings of producer/hot/drainer threads.
//
// **Atomic extraction unit (Phase B Step B.4.1 Decision):** All four utilities live in
// the same translation unit because they're peers (boot-time topology + thread placement
// helpers) called only from EngineSharded_Run body. No call from anywhere else in the
// codebase. Pure helper functions; no captures, no globals, no template params.
//
// **Hot-path discipline:** NONE of these functions live on the hot path. They run at
// boot time only — pin assignment fires once per spawned thread, TSC calibrate fires
// once at engine entry. No latency impact from header location.
//======================================================================================================

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <unistd.h>  // sysconf(_SC_NPROCESSORS_ONLN) — POSIX, also available on macOS

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#include <x86intrin.h>  // __rdtsc / rdtscp / lfence / mfence

// parent_index: CoreFrameworks/EngineSharded/Run.hpp

namespace tt {

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

} // namespace tt
