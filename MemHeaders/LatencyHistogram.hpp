// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/LatencyHistogram.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[64-bucket log2(cycles) latency histogram — clzll bucket index, lock-free single-writer/single-reader, cache-line-isolated hot buckets vs cold observability cluster]
// [CONTAINS]
//   - [STRUCT]_[LatencyHistogram]
//   - [FUNCTION]_[latency_bucket_index]
//   - [FUNCTION]_[LatencyHistogram_Reset]
//   - [FUNCTION]_[LatencyHistogram_Accumulate]
//   - [FUNCTION]_[LatencyHistogram_Percentile]
// [REFERENCE]_[DESIGN_SPEC]_[runtime-toggleable-bench-gate-pattern]
//======================================================================================================
// 64-bucket logarithmic latency histogram. Buckets indexed by log2(cycles)
// via __builtin_clzll, covering ~1 cycle to ~2^63 cycles per bucket.
//
// At 3 GHz: bucket 0 = 1 cycle (~0.3 ns); bucket 10 = 1024 cycles (~341 ns);
// bucket 20 = 1M cycles (~333 µs); bucket 30 = 1G cycles (~333 ms).
//
// Lock-free single-writer / single-reader (writer = instrumented site;
// reader = TUI/snapshot publisher). Per the cross-thread cluster-isolation
// discipline (ND1): 1 cache-line for hot bucket array, optional 2nd
// cache-line for cross-thread observability sub-cluster.
//
// PHASE 7.A SCOPE (shipped v5.15.5.C.3):
//   - Histogram primitive + accumulate + reset + percentile readout helpers
//   - cfg.oms_bench_enabled flag + tests for histogram operations
//
// PHASE 7.B STATUS (partially landed; remainder tracked at TECH_DEBT-045):
//   - LANDED: boot-time dispatch in main.cpp on cfg.oms_bench_enabled
//     (EngineSharded_Run<F, BENCH> two-instantiation split) + ONE coarse
//     instrumented site (whole drainer cycle, EngineSharded/Run.hpp) +
//     stderr p50/p99/max summary at shutdown.
//   - STILL DEFERRED: per-site wrappers (OrderManager_Tick<F, BENCH> etc.),
//     TUI/snapshot readout per publish, AUTOPOPULATE composition for N
//     instrumented sites (per
//     DESIGN_SPECS/runtime-toggleable-bench-gate-pattern.md Composition 1)
//
// Cross-references (compile-time elision via template <bool ENABLED> +
// cross-thread cluster isolation via alignas(64) + the latency-vs-cache
// decision framework):
//   DESIGN_SPECS/runtime-toggleable-bench-gate-pattern.md (full design;
//     this is the FIRST APPLICATION pattern — primitive shipped here)
//   DESIGN_SPECS/branchless-math-kernel-pattern.md (bucket index calc is
//     1 op via __builtin_clzll; no branch needed)
//   DESIGN_SPECS/cross-thread-snapshot-publish-cluster-isolation.md (ND1)
//======================================================================================================
#ifndef LATENCY_HISTOGRAM_HPP
#define LATENCY_HISTOGRAM_HPP

#include <stdint.h>
#include <atomic>

namespace tt {

//======================================================================
// [STRUCT]_[LatencyHistogram]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[576B / 9 cache lines — 8 hot writer-side bucket lines + 1 cold cross-thread observability line (relaxed-atomic total_count, non-atomic extremes)]
//======================================================================
// [CODE]
//======================================================================
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Layout (one cache line per concern; total 576 bytes per histogram):
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
// histogram (per the cross-thread cluster-isolation discipline:
// SEPARATE histograms per writer-thread; no shared writes).
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[576B]
// [ALIGN]_[64]
// [CACHE_LINES]_[9]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[LatencyHistogram]
//======================================================================

// [ASSERT]_[LAYOUT_LOCK]_[sizeof(LatencyHistogram) % 64 == 0]
static_assert(sizeof(LatencyHistogram) % 64 == 0,
              "LatencyHistogram should be a multiple of 64 bytes for clean cache-line packing");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(LatencyHistogram) == 64]
static_assert(alignof(LatencyHistogram) == 64,
              "LatencyHistogram MUST be 64-byte aligned (alignas(64) on first member)");

//======================================================================
// [FUNCTION]_[latency_bucket_index]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[log2(cycles) clamped to [0,63] via one __builtin_clzll — ~3-5 cycles; the per-call overhead bound for bench-ON instrumentation]
//======================================================================
// [CODE]
//======================================================================
inline int latency_bucket_index(uint64_t cycles) {
    if (cycles == 0) return 0;
    // 64 - __builtin_clzll(cycles) = bits needed to represent cycles = ceil(log2(cycles+1))
    // For cycles=1 → 1; cycles=2 → 2; cycles=1023 → 10; cycles=1024 → 11.
    int idx = 64 - __builtin_clzll(cycles);
    if (idx > 63) idx = 63;
    return idx;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Returns log2(cycles) clamped to [0, 63]. Single-op via __builtin_clzll
// (count-leading-zeros). For cycles==0, returns 0 (special case).
//
// __builtin_clzll: 1 instruction on modern x86 (lzcnt or bsr — both
// ~3-cycle latency, fully pipelined). Branchless in codegen: the
// cycles==0 guard + the idx clamp both lower to cmov.
//
// Cost: ~3-5 cycles total (1 lzcnt + 1 subtract + 1 cmov). Per-call
// instrumentation overhead bound for bench gate ON-state.
//======================================================================
// [END_FUNCTION]_[latency_bucket_index]
//======================================================================

//======================================================================
// [FUNCTION]_[LatencyHistogram_Reset]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[zero all state — engine boot OR paper-reset boundary; single-thread by contract (writer paused or not yet started)]
//======================================================================
// [CODE]
//======================================================================
inline void LatencyHistogram_Reset(LatencyHistogram* h) {
    if (!h) return;
    for (int i = 0; i < 64; ++i) h->buckets[i] = 0;
    h->total_count.store(0, std::memory_order_relaxed);
    h->min_observed = UINT64_MAX;
    h->max_observed = 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[LatencyHistogram_Reset]
//======================================================================

//======================================================================
// [FUNCTION]_[LatencyHistogram_Accumulate]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[single-sample bump (bucket + relaxed total_count + extremes) — ~10-15 cycles bench-ON; bench-OFF is zero via if-constexpr elision at the caller]
//======================================================================
// [CODE]
//======================================================================
inline void LatencyHistogram_Accumulate(LatencyHistogram* h, uint64_t cycles) {
    if (!h) return;
    int idx = latency_bucket_index(cycles);
    h->buckets[idx]++;
    h->total_count.fetch_add(1, std::memory_order_relaxed);
    if (cycles < h->min_observed) h->min_observed = cycles;
    if (cycles > h->max_observed) h->max_observed = cycles;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Cost per call: ~5 cycles (bucket index calc) + 1 store (bucket bump)
// + 1 atomic fetch_add (total_count; lock-free) + 1-2 compare+store
// (min/max). Total: ~10-15 cycles per accumulate call in bench-ON mode.
// Production bench-OFF: zero (if constexpr (BENCH=false) discards the
// entire instrumentation block — compile-time elision discipline).
//======================================================================
// [END_FUNCTION]_[LatencyHistogram_Accumulate]
//======================================================================

//======================================================================
// [FUNCTION]_[LatencyHistogram_Percentile]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[p-th percentile readout (0.5 median / 0.99 p99 / 1.0 max) — O(64) cumulative walk; returns the bucket LOWER bound ("at least N cycles")]
//======================================================================
// [CODE]
//======================================================================
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
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
//======================================================================
// [END_FUNCTION]_[LatencyHistogram_Percentile]
//======================================================================

}  // namespace tt

#endif  // LATENCY_HISTOGRAM_HPP
