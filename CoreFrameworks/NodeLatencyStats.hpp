// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/NodeLatencyStats.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCOPE]_[CORE]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-core hot-path latency tracking — single-writer sampling + controller-side percentile snapshot]
// [CONTAINS]
//   - [STRUCT]_[NodeLatencyStats]
//   - [STRUCT]_[NodeLatencySnapshot]
//   - [FUNCTION]_[NodeLatencyStats_Init]
//   - [FUNCTION]_[NodeLatencyStats_Reset]
//   - [FUNCTION]_[NodeLatencyStats_Sample]
//   - [FUNCTION]_[NodeLatencyStats_Snapshot]
//======================================================================================================

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace tt {

//======================================================================
// [STRUCT]_[NodeLatencyStats]
//----------------------------------------------------------------------
// [TAG]_[[MONITORING_PLANE] [DATA_ORIENTED_DESIGN]]
// [SCOPE]_[CORE]
// [THREAD]_[[HOT_WRITER] [SLOW_READER]]
// [SYNC]_[ATOMIC]
// [REFERENCE]_[INVARIANT]_[H6]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-core latency accumulator — running scalars + 256-sample ring + lifetime log-bucket histogram]
//======================================================================
// [CODE]
//======================================================================
struct alignas(64) NodeLatencyStats {
    static constexpr int RING_SIZE = 256;  // power of 2 for cheap masking
    static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "RING_SIZE must be power of 2");

    // v4.7.36: lifetime log-bucket histogram for session-wide percentiles.
    // 64 buckets each covering [2^i, 2^(i+1)) cycles. Bucket lookup via
    // __builtin_clzll — ~2-3 cycles. Increment is a regular ++ (single-
    // writer; controller reads on slow path only). Cost on hot path:
    // ~3-5ns added vs the prior version. Memory: 512 bytes per core.
    static constexpr int HIST_BUCKETS = 64;

    // --- Cache line 0: hot fields the execution core writes every sample ---
    std::atomic<uint8_t> enabled;     // controller-flipped, hot path reads with relaxed
    uint8_t  _pad0[7];
    uint64_t total_count;             // monotonic count of samples
    uint64_t total_cycles;            // sum for avg computation
    uint64_t min_cycles;              // lifetime min
    uint64_t max_cycles;              // lifetime max
    uint32_t ring_head;               // next write index in recent[]
    uint32_t _pad1;
    uint64_t last_sample_tsc;         // rdtsc at the last sample (for "stale" detection)

    // --- Tail cache lines: ring buffer of recent samples ---
    // 32-bit truncated cycle counts. Even at 5GHz, 2^32 cycles = 858ms which
    // is way more than any healthy hot path should ever see. Anything larger
    // gets clamped to UINT32_MAX as a flag.
    uint32_t recent[RING_SIZE];
    // v4.7.36: lifetime histogram. bucket[i] = count of samples with
    // floor(log2(cycles)) == i. Used by snapshot to compute lifetime
    // p50/p95/p99 across all samples (not just the last 256).
    uint64_t lifetime_buckets[HIST_BUCKETS];
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]_[role + why per-core]
//----------------------------------------------------------------------
// Per-execution-core latency tracking. One NodeLatencyStats instance lives on
// each ExecutionCore. The execution core (single writer) samples its own tick
// cost when the enabled flag is set; the controller (single reader) snapshots
// the stats on its slow path for display in the TUI/GUI.
//
// 64-byte aligned to keep its cache line clean. The struct is single-writer
// (the execution core itself) so the only atomic is the enabled flag, which
// the controller flips from outside.
//
// Why per-core (not global):
//   - Each core has its own L1 cache, so the stats live next to the execution
//     state — zero false sharing across cores
//   - Single-writer means no atomics needed inside the struct except for the
//     enabled flag
//   - The controller can compare per-core histograms and spot the slow core,
//     not just see one aggregate "engine is slow" number
//======================================================================
// [COMMENT]_[hot-path overhead + storage + percentile accuracy]
//----------------------------------------------------------------------
// Hot path overhead:
//   - Disabled (default): 1 atomic load + 1 branch (predicted not-taken). ~1ns.
//   - Enabled: 2 rdtsc + 1 branch + 1 store + 1 ring write. ~25-30ns.
//   - On a 60ns hot path, enabled mode is a 50% overhead. Use it for
//     diagnostics, not always-on production monitoring (or accept the cost).
//
// Storage:
//   - Running scalars: count, sum, min, max
//   - Ring buffer of recent 256 samples for percentile estimation
//   - Last-sample timestamp for "stale" detection in the TUI
//
// Percentile accuracy:
//   - The ring buffer holds the most recent 256 samples (or all samples if
//     fewer have been collected). p50/p95/p99 are computed by sorting the
//     ring on demand. This is a sliding window, so the percentiles reflect
//     RECENT behavior, not lifetime behavior.
//   - Lifetime min/max/avg are tracked separately and don't decay.
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//----------------------------------------------------------------------
// [SIZE]_[1600B]
// [ALIGN]_[64]
// [CACHE_LINES]_[25]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[NodeLatencyStats]
//======================================================================

//======================================================================
// [STRUCT]_[NodeLatencySnapshot]
//----------------------------------------------------------------------
// [TAG]_[[MONITORING_PLANE] [FLOAT_DISPLAY_ONLY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[read-only percentile view returned by Snapshot — ns conversions for TUI/GUI display]
//======================================================================
// [CODE]
//======================================================================
struct NodeLatencySnapshot {
    int      enabled;            // 1 if stats are currently being collected
    uint64_t total_count;        // lifetime sample count
    double   avg_cycles;
    double   avg_ns;
    uint64_t min_cycles;
    double   min_ns;
    uint64_t max_cycles;
    double   max_ns;
    // Sliding-window percentiles from the most recent 256 samples
    uint64_t p50_cycles;
    double   p50_ns;
    uint64_t p95_cycles;
    double   p95_ns;
    uint64_t p99_cycles;
    double   p99_ns;
    // Sample window depth used for percentile calc (min(total_count, 256))
    int      window_size;
    // v4.7.36: lifetime percentiles from log-bucket histogram (all samples,
    // not just last 256). Bucket precision is power-of-2, so values are
    // approximate — reported as the upper bound of the bucket.
    double   lifetime_p50_ns;
    double   lifetime_p95_ns;
    double   lifetime_p99_ns;
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Read-only view returned by NodeLatencyStats_Snapshot. Computed values are
// converted to nanoseconds at the call site (the snapshot function takes the
// TSC frequency in GHz so it can do the conversion in one place).
//======================================================================
// [END_STRUCT]_[NodeLatencySnapshot]
//======================================================================

//======================================================================
// [FUNCTION]_[NodeLatencyStats_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[zero everything, stats DISABLED]
//======================================================================
// [CODE]
//======================================================================
inline void NodeLatencyStats_Init(NodeLatencyStats* s) {
    s->enabled.store(0, std::memory_order_relaxed);
    s->total_count = 0;
    s->total_cycles = 0;
    s->min_cycles = UINT64_MAX;
    s->max_cycles = 0;
    s->ring_head = 0;
    s->last_sample_tsc = 0;
    for (int i = 0; i < NodeLatencyStats::RING_SIZE; ++i) s->recent[i] = 0;
    for (int i = 0; i < NodeLatencyStats::HIST_BUCKETS; ++i) s->lifetime_buckets[i] = 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]_[init + reset semantics — shared with Reset below]
//----------------------------------------------------------------------
// Init zeros everything and leaves stats DISABLED. Reset clears the running
// scalars + ring but does NOT change the enabled state — the caller can reset
// stats mid-run without disabling them.
//======================================================================
// [END_FUNCTION]_[NodeLatencyStats_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[NodeLatencyStats_Reset]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[clear scalars + ring, PRESERVE the enabled state — mid-run reset without disabling]
//======================================================================
// [CODE]
//======================================================================
inline void NodeLatencyStats_Reset(NodeLatencyStats* s) {
    s->total_count = 0;
    s->total_cycles = 0;
    s->min_cycles = UINT64_MAX;
    s->max_cycles = 0;
    s->ring_head = 0;
    s->last_sample_tsc = 0;
    for (int i = 0; i < NodeLatencyStats::RING_SIZE; ++i) s->recent[i] = 0;
    for (int i = 0; i < NodeLatencyStats::HIST_BUCKETS; ++i) s->lifetime_buckets[i] = 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[NodeLatencyStats_Reset]
//======================================================================

inline void NodeLatencyStats_Enable(NodeLatencyStats* s) {
    s->enabled.store(1, std::memory_order_release);
}

inline void NodeLatencyStats_Disable(NodeLatencyStats* s) {
    s->enabled.store(0, std::memory_order_release);
}

//======================================================================
// [FUNCTION]_[NodeLatencyStats_Sample]
//----------------------------------------------------------------------
// [TAG]_[[HOT_PATH] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[hot-path sample record — scalars + ring write + log-bucket increment; single writer, no atomics]
//======================================================================
// [CODE]
//======================================================================
__attribute__((always_inline))
static inline void NodeLatencyStats_Sample(NodeLatencyStats* s, uint64_t cycles, uint64_t now_tsc) {
    s->total_count++;
    s->total_cycles += cycles;
    if (cycles < s->min_cycles) s->min_cycles = cycles;
    if (cycles > s->max_cycles) s->max_cycles = cycles;
    uint32_t clamped = (cycles > UINT32_MAX) ? UINT32_MAX : (uint32_t)cycles;
    s->recent[s->ring_head] = clamped;
    s->ring_head = (s->ring_head + 1) & (NodeLatencyStats::RING_SIZE - 1);
    s->last_sample_tsc = now_tsc;
    // v4.7.36: lifetime histogram bucket increment. floor(log2(cycles))
    // gives bucket index. clzll counts leading zeros — 63 minus that is
    // the highest set bit position, i.e. floor(log2). Bucket 0 reserved
    // for cycles==0 (shouldn't happen but defensive).
    int bucket = (cycles == 0) ? 0 : (63 - __builtin_clzll(cycles));
    if (bucket >= NodeLatencyStats::HIST_BUCKETS) bucket = NodeLatencyStats::HIST_BUCKETS - 1;
    s->lifetime_buckets[bucket]++;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Called from inside ExecutionCore_Tick when enabled. Single writer = no
// atomics. The controller never writes to these fields, only reads them
// during snapshot computation, so the relaxed reads are safe.
//
// `cycles` is the rdtsc delta of the work being measured. `now_tsc` is the
// rdtsc reading at sample time, used for "last seen" tracking in the TUI.
//======================================================================
// [END_FUNCTION]_[NodeLatencyStats_Sample]
//======================================================================

//======================================================================
// [FUNCTION]_[NodeLatencyStats_Snapshot]
//----------------------------------------------------------------------
// [TAG]_[[SLOW_PATH] [MONITORING_PLANE] [FLOAT_DISPLAY_ONLY]]
// [COMPLEXITY]_[O(N log N), N=256 — ~2us per call; slow-path budget]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[controller-side percentile snapshot — sort the recent ring + walk the lifetime histogram]
//======================================================================
// [CODE]
//======================================================================
inline NodeLatencySnapshot NodeLatencyStats_Snapshot(const NodeLatencyStats* s, double tsc_ghz) {
    NodeLatencySnapshot out{};
    out.enabled = s->enabled.load(std::memory_order_acquire);
    out.total_count = s->total_count;

    auto cyc_to_ns = [tsc_ghz](double c) -> double {
        return tsc_ghz > 0.0 ? c / tsc_ghz : 0.0;
    };

    if (s->total_count == 0) {
        // No samples yet — leave everything zeroed
        return out;
    }

    out.avg_cycles = (double)s->total_cycles / (double)s->total_count;
    out.avg_ns = cyc_to_ns(out.avg_cycles);
    out.min_cycles = s->min_cycles;
    out.min_ns = cyc_to_ns((double)s->min_cycles);
    out.max_cycles = s->max_cycles;
    out.max_ns = cyc_to_ns((double)s->max_cycles);

    // Copy the ring into a local sortable buffer. The window is min(count, RING_SIZE).
    uint32_t local[NodeLatencyStats::RING_SIZE];
    int n = (s->total_count < (uint64_t)NodeLatencyStats::RING_SIZE)
        ? (int)s->total_count
        : NodeLatencyStats::RING_SIZE;
    for (int i = 0; i < n; ++i) local[i] = s->recent[i];
    std::sort(local, local + n);

    out.window_size = n;
    out.p50_cycles = local[n * 50 / 100];
    out.p95_cycles = local[n * 95 / 100];
    out.p99_cycles = local[n * 99 / 100];
    out.p50_ns = cyc_to_ns((double)out.p50_cycles);
    out.p95_ns = cyc_to_ns((double)out.p95_cycles);
    out.p99_ns = cyc_to_ns((double)out.p99_cycles);

    // v4.7.36: lifetime percentiles from log-bucket histogram. Walk
    // buckets accumulating count until we cross each percentile threshold.
    // Bucket i covers [2^i, 2^(i+1)) cycles — report upper bound as the
    // percentile estimate (conservative). Power-of-2 precision means
    // reported values are within ~2x of true percentile.
    auto lifetime_pct = [&](double pct) -> double {
        uint64_t target = (uint64_t)((double)s->total_count * pct);
        if (target == 0) target = 1;
        uint64_t accum = 0;
        for (int b = 0; b < NodeLatencyStats::HIST_BUCKETS; ++b) {
            accum += s->lifetime_buckets[b];
            if (accum >= target) {
                // Upper bound of bucket b is 2^(b+1) cycles.
                uint64_t cycles_upper = (b >= 63) ? UINT64_MAX : (1ULL << (b + 1));
                return cyc_to_ns((double)cycles_upper);
            }
        }
        return cyc_to_ns((double)s->max_cycles);
    };
    out.lifetime_p50_ns = lifetime_pct(0.50);
    out.lifetime_p95_ns = lifetime_pct(0.95);
    out.lifetime_p99_ns = lifetime_pct(0.99);
    return out;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Copies the ring into a local array, sorts it, and computes percentiles.
// O(N log N) where N=256, so ~2µs per call on a typical CPU. Cheap enough to
// run on the controller's slow path every snapshot rebuild.
//
// tsc_ghz is the calibrated TSC frequency for cycle→ns conversion. Pass 0 to
// skip the conversion (cycle counts only).
//======================================================================
// [END_FUNCTION]_[NodeLatencyStats_Snapshot]
//======================================================================

}  // namespace tt
