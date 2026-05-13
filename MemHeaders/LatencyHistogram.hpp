// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [LATENCY HISTOGRAM — v5.15.5.C.3 Phase 7.A]
//======================================================================================================
// 64-bucket logarithmic latency histogram. Buckets indexed by log2(cycles)
// via __builtin_clzll, covering ~1 cycle to ~2^63 cycles per bucket.
//
// At 3 GHz: bucket 0 = 1 cycle (~0.3 ns); bucket 10 = 1024 cycles (~341 ns);
// bucket 20 = 1M cycles (~333 µs); bucket 30 = 1G cycles (~333 ms).
//
// Lock-free single-writer / single-reader (writer = instrumented site;
// reader = TUI/snapshot publisher). Per CLAUDE.md item 25 cross-thread
// cluster isolation: 1 cache-line for hot bucket array, optional 2nd
// cache-line for cross-thread observability sub-cluster.
//
// PHASE 7.A SCOPE (this ship):
//   - Histogram primitive + accumulate + reset + percentile readout helpers
//   - cfg.oms_bench_enabled flag (substrate; no instrumented sites yet)
//   - Tests for histogram operations
//
// PHASE 7.B SCOPE (DEFERRED to focused follow-up ship; see TECH_DEBT entry):
//   - Template-parameterized bench wrappers per instrumented site:
//     template <bool BENCH> OrderManager_Tick<F, BENCH>(...) etc.
//   - Boot-time dispatch in EngineSharded_Run on cfg.oms_bench_enabled
//   - TUI surface: p50/p99/max readout per histogram per snapshot publish
//   - AUTOPOPULATE composition for N instrumented sites (per
//     DESIGN_SPECS/runtime-toggleable-bench-gate-pattern.md Composition 1)
//
// Cross-references:
//   DESIGN_SPECS/runtime-toggleable-bench-gate-pattern.md (full design;
//     this is the FIRST APPLICATION pattern — primitive shipped here;
//     integration deferred)
//   CLAUDE.md item 18 (compile-time elision via template <bool ENABLED>)
//   CLAUDE.md item 25 (cross-thread cluster isolation via alignas(64))
//   CLAUDE.md item 28 (latency-vs-cache decision framework)
//   DESIGN_SPECS/branchless-math-kernel-pattern.md (bucket index calc is
//     1 op via __builtin_clzll; no branch needed)
//   DESIGN_SPECS/cross-thread-snapshot-publish-cluster-isolation.md (ND1)
//======================================================================================================
#ifndef LATENCY_HISTOGRAM_HPP
#define LATENCY_HISTOGRAM_HPP

#include <stdint.h>
#include <atomic>

namespace tt {

//======================================================================================================
// [LatencyHistogram struct]
//======================================================================================================
// Layout (one cache line per concern; total ~640 bytes per histogram):
//   alignas(64) buckets[64]            — 512 B (8 cache lines; HOT writer access)
//   alignas(64) total_count + min_obs + max_obs — observability sub-cluster
//                                          (cross-thread read by publisher)
//
// HOT-side single-writer access: instrumented site bumps buckets[idx] +
// updates total_count + extremes.
//
// COLD-side single-reader access: snapshot publisher reads observability
// sub-cluster at 60 Hz (negligible cache impact). Atomic total_count
// uses __ATOMIC_RELAXED — publisher only needs monotonic-progress, not
// happens-before ordering with the bucket array.
//
// min_observed/max_observed are NOT atomic — slow-path writer single-thread
// (drainer cadence); publisher reads as eventually-consistent display state.
//
// IMPORTANT — bucket array NOT atomic: writes are single-thread per
// histogram (per CLAUDE.md item 25 cross-thread cluster isolation:
// SEPARATE histograms per writer-thread; no shared writes).
//======================================================================================================
struct LatencyHistogram {
    // ---- HOT writer-side cluster (1 cache line + 7 more for bucket array) ----
    alignas(64) uint64_t buckets[64];

    // ---- COLD observability sub-cluster (separate cache line; cross-thread read by publisher) ----
    alignas(64) std::atomic<uint64_t> total_count;
    uint64_t min_observed;   // single-thread writer; publisher reads as eventually-consistent
    uint64_t max_observed;   // same
    // padding to 64-byte alignment for the observability cluster
    uint64_t _pad_obs[5];
};

static_assert(sizeof(LatencyHistogram) % 64 == 0,
              "LatencyHistogram should be a multiple of 64 bytes for clean cache-line packing");
static_assert(alignof(LatencyHistogram) == 64,
              "LatencyHistogram MUST be 64-byte aligned (alignas(64) on first member)");

//======================================================================================================
// [latency_bucket_index — branchless log2 bucket index]
//======================================================================================================
// Returns log2(cycles) clamped to [0, 63]. Single-op via __builtin_clzll
// (count-leading-zeros). For cycles==0, returns 0 (special case).
//
// __builtin_clzll: 1 instruction on modern x86 (lzcnt or bsr — both
// ~3-cycle latency, fully pipelined). Branchless via the ternary on
// cycles==0; compiler emits cmov.
//
// Cost: ~3-5 cycles total (1 lzcnt + 1 subtract + 1 cmov). Per-call
// instrumentation overhead bound for bench gate ON-state.
//======================================================================================================
inline int latency_bucket_index(uint64_t cycles) {
    if (cycles == 0) return 0;
    // 64 - __builtin_clzll(cycles) = bits needed to represent cycles = ceil(log2(cycles+1))
    // For cycles=1 → 1; cycles=2 → 2; cycles=1023 → 10; cycles=1024 → 11.
    int idx = 64 - __builtin_clzll(cycles);
    if (idx > 63) idx = 63;
    return idx;
}

//======================================================================================================
// [LatencyHistogram_Reset — zero all state]
//======================================================================================================
// Call at engine boot OR at paper-reset boundary. Single-thread (no
// cross-thread races by contract — writer thread paused or not yet started).
//======================================================================================================
inline void LatencyHistogram_Reset(LatencyHistogram* h) {
    if (!h) return;
    for (int i = 0; i < 64; ++i) h->buckets[i] = 0;
    h->total_count.store(0, std::memory_order_relaxed);
    h->min_observed = UINT64_MAX;
    h->max_observed = 0;
}

//======================================================================================================
// [LatencyHistogram_Accumulate — single-sample bump]
//======================================================================================================
// Bumps the bucket corresponding to `cycles`, increments total_count,
// updates min/max. Single-thread writer; safe to call from the
// instrumented site without locks.
//
// Cost per call: ~5 cycles (bucket index calc) + 1 store (bucket bump)
// + 1 atomic fetch_add (total_count; lock-free) + 1-2 compare+store
// (min/max). Total: ~10-15 cycles per accumulate call in bench-ON mode.
// Production bench-OFF: zero (if constexpr (BENCH=false) discards the
// entire instrumentation block per CLAUDE.md item 18).
//======================================================================================================
inline void LatencyHistogram_Accumulate(LatencyHistogram* h, uint64_t cycles) {
    if (!h) return;
    int idx = latency_bucket_index(cycles);
    h->buckets[idx]++;
    h->total_count.fetch_add(1, std::memory_order_relaxed);
    if (cycles < h->min_observed) h->min_observed = cycles;
    if (cycles > h->max_observed) h->max_observed = cycles;
}

//======================================================================================================
// [LatencyHistogram_Percentile — readout helper for snapshot publisher]
//======================================================================================================
// Returns the cycle count at the p-th percentile [0.0, 1.0]. e.g.,
// percentile=0.5 → median; 0.99 → p99; 1.0 → max.
//
// Single-thread reader (snapshot publisher). Walks bucket array
// cumulative; returns the bucket boundary that contains the target rank.
//
// Bucket boundary is log2-spaced: bucket i covers cycles in
// [2^(i-1), 2^i) for i > 0; bucket 0 is the cycles==0 special case.
// The returned cycle count is the LOWER bound of the bucket — useful for
// "p99 latency was AT LEAST N cycles". For tighter percentile, increase
// bucket count (currently 64; doubling to 128 → ~25% more precision but
// 2× memory).
//
// Cost: O(64) cumulative walk + 1 multiply for the percentile threshold.
// Called at 60 Hz publish cadence; negligible.
//======================================================================================================
inline uint64_t LatencyHistogram_Percentile(const LatencyHistogram* h, double percentile) {
    if (!h) return 0;
    uint64_t total = h->total_count.load(std::memory_order_relaxed);
    if (total == 0) return 0;
    if (percentile <= 0.0) return h->min_observed;
    if (percentile >= 1.0) return h->max_observed;
    uint64_t target_rank = (uint64_t)((double)total * percentile);
    uint64_t cumulative = 0;
    for (int i = 0; i < 64; ++i) {
        cumulative += h->buckets[i];
        if (cumulative >= target_rank) {
            // Bucket i covers cycles [2^(i-1), 2^i); return lower bound (or 0 for i=0).
            return (i > 0) ? ((uint64_t)1 << (i - 1)) : 0;
        }
    }
    return h->max_observed;
}

}  // namespace tt

#endif  // LATENCY_HISTOGRAM_HPP
