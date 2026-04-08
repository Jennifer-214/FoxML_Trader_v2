// measurement.hpp — sample collection, percentile computation, table reporting
//
// Extracted from test_rdtsc.cpp so all four bench scenarios share the same
// measurement code. Same sample() template, same report() function, plus a
// Stats struct so the main bench can compute deltas across scenarios.
//
// Design:
//   - sample(N, fn) runs fn(i) N times with rdtsc fences around each call,
//     returns sorted cycle counts
//   - compute_stats() turns sorted samples into the percentile struct
//   - print_table_header / print_table_row produce the side-by-side format
//     spec'd in plans/per_core_sharding/03_benchmark_harness/plan.md
//
// Why a sorted-vector approach instead of histogramming: the bench runs at
// most a few million samples per scenario, fits in memory trivially, and a
// sort is ~50ms which is irrelevant compared to the actual work being measured.
// Histogramming would lose information we want for the report (raw min, max,
// exact p99.9). Keep it simple.

#pragma once

#include "rdtsc.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace experiment {

// Percentile bundle for one scenario. Cycles, not ns — conversion happens in
// the print step so we keep raw counts in case we want to recompute later.
struct Stats {
    uint64_t min;
    uint64_t p50;
    uint64_t p95;
    uint64_t p99;
    uint64_t p999;
    uint64_t max;
    uint64_t count;
};

// Sample N times. fn(i) runs once per iteration inside the rdtsc fences.
// Returns sorted samples. Reserves up front so the vector grow doesn't poison
// late samples with reallocation cost.
template <typename Fn>
static std::vector<uint64_t> sample(int n, Fn&& fn) {
    std::vector<uint64_t> samples;
    samples.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        uint64_t s = rdtsc_start();
        fn(i);
        uint64_t e = rdtsc_end();
        samples.push_back(e - s);
    }
    std::sort(samples.begin(), samples.end());
    return samples;
}

// Same as sample() but with a warmup pass that doesn't get measured. Use this
// for the bench scenarios so the I-cache and D-cache are hot before we start
// counting (per pitfall P3.2 — cold cache bias on first measurement).
template <typename Fn>
static std::vector<uint64_t> sample_with_warmup(int warmup_n, int measure_n, Fn&& fn) {
    for (int i = 0; i < warmup_n; ++i) {
        fn(i);
    }
    return sample(measure_n, std::forward<Fn>(fn));
}

// Turn sorted samples into a Stats bundle. Optionally drop the top fraction
// of samples before computing percentiles (per pitfall P3.4 — kernel
// preemption noise dominates p99/p99.9 on a non-isolated CPU and bleeds into
// the metrics we want to use for verdicts).
static inline Stats compute_stats(const std::vector<uint64_t>& sorted, double drop_top_frac = 0.0) {
    Stats s{};
    if (sorted.empty()) return s;
    size_t effective_size = sorted.size();
    if (drop_top_frac > 0.0 && drop_top_frac < 1.0) {
        size_t to_drop = (size_t)((double)sorted.size() * drop_top_frac);
        if (to_drop >= sorted.size()) to_drop = sorted.size() - 1;
        effective_size = sorted.size() - to_drop;
    }
    auto pct = [&](double p) -> uint64_t {
        size_t idx = (size_t)((double)effective_size * p);
        if (idx >= effective_size) idx = effective_size - 1;
        return sorted[idx];
    };
    s.min   = sorted.front();
    s.p50   = pct(0.50);
    s.p95   = pct(0.95);
    s.p99   = pct(0.99);
    s.p999  = pct(0.999);
    s.max   = sorted[effective_size - 1];
    s.count = effective_size;
    return s;
}

// Pretty-print one scenario row in cycles. Pair with print_table_header_cycles.
static inline void print_row_cycles(const char* name, const Stats& s) {
    printf("%-34s  %6lu %6lu %6lu %6lu %7lu %7lu  cycles\n",
           name, s.min, s.p50, s.p95, s.p99, s.p999, s.max);
}

// Same row, in nanoseconds.
static inline void print_row_ns(const char* name, const Stats& s, uint64_t tsc_hz) {
    printf("%-34s  %6.1f %6.1f %6.1f %6.1f %7.1f %7.1f  ns\n",
           name,
           cycles_to_ns(s.min,  tsc_hz),
           cycles_to_ns(s.p50,  tsc_hz),
           cycles_to_ns(s.p95,  tsc_hz),
           cycles_to_ns(s.p99,  tsc_hz),
           cycles_to_ns(s.p999, tsc_hz),
           cycles_to_ns(s.max,  tsc_hz));
}

static inline void print_table_header_cycles() {
    printf("%-34s  %6s %6s %6s %6s %7s %7s\n",
           "scenario", "min", "p50", "p95", "p99", "p99.9", "max");
    printf("%-34s  %6s %6s %6s %6s %7s %7s\n",
           "----------------------------------",
           "------", "------", "------", "------", "-------", "-------");
}

static inline void print_table_header_ns() {
    printf("%-34s  %6s %6s %6s %6s %7s %7s\n",
           "scenario  (ns)", "min", "p50", "p95", "p99", "p99.9", "max");
    printf("%-34s  %6s %6s %6s %6s %7s %7s\n",
           "----------------------------------",
           "------", "------", "------", "------", "-------", "-------");
}

// Quick sanity check: did the loop actually execute, or did the optimizer
// eliminate it? Returns true if min cycles is at least min_floor. Used at the
// end of each scenario per pitfall P3.1.
static inline bool sanity_check_floor(const Stats& s, uint64_t min_floor) {
    return s.min >= min_floor;
}

}  // namespace experiment
