// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/WelfordStats.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[O(1) Welford mean/variance for unbounded streams (fixed-window sibling: RollingStats) — P&L distribution, drift detection, z-scores]
// [CONTAINS]
//   - [STRUCT]_[WelfordTracker]
//   - [FUNCTION]_[Welford_Push]   (Init/Variance/Stddev/ZScore/Reset read-outs share the block)
//======================================================================================================
// O(1) incremental mean/variance tracker — numerically stable for unbounded streams.
// complementary to RollingStats (which uses a fixed-window ring buffer).
// use cases: P&L distribution, signal drift detection, prediction normalization.
//======================================================================================================
#ifndef WELFORD_STATS_HPP
#define WELFORD_STATS_HPP

#include <cstdint>  // v5.15.5.F.4d TECH_DEBT-083 close — explicit IWYU for uintN_t (was transitively pulled)
#include "../FixedPoint/FixedPointN.hpp"

//======================================================================
// [STRUCT]_[WelfordTracker]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the O(1) accumulator state — count + running mean + m2 (sum of squared deviations) + running min/max]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct WelfordTracker {
    uint64_t count;
    FPN_Binary<F> mean;
    FPN_Binary<F> m2;       // sum of squared deviations from the mean
    FPN_Binary<F> min_val;
    FPN_Binary<F> max_val;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[80B]
// [ALIGN]_[16]
// [CACHE_LINES]_[2]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[WelfordTracker]
//======================================================================

//======================================================================
// [FUNCTION]_[Welford_Push]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the classic two-delta update (numerically stable) + min/max; Init + Variance/Stddev/ZScore/Reset read-outs share the block]
//======================================================================
// [CODE]
//======================================================================

template <unsigned F>
inline WelfordTracker<F> Welford_Init() {
    WelfordTracker<F> w;
    w.count = 0;
    w.mean = FPN_Zero<F>();
    w.m2 = FPN_Zero<F>();
    w.min_val = FPN_Zero<F>();
    w.max_val = FPN_Zero<F>();
    return w;
}

// O(1) push — Welford's online algorithm for stable variance
template <unsigned F>
inline void Welford_Push(WelfordTracker<F> *w, FPN_Binary<F> value) {
    w->count++;
    if (w->count == 1) {
        w->mean = value;
        w->m2 = FPN_Zero<F>();
        w->min_val = value;
        w->max_val = value;
        return;
    }
    // min/max tracking
    if (FPN_LessThan(value, w->min_val)) w->min_val = value;
    if (FPN_GreaterThan(value, w->max_val)) w->max_val = value;

    // Welford update: delta = value - mean_old
    FPN_Binary<F> delta = FPN_Sub(value, w->mean);
    // mean += delta / count
    FPN_Binary<F> count_fpn = FPN_FromDouble<F>((double)w->count);
    if (FPN_IsZero(count_fpn)) return; // guard
    FPN_Binary<F> delta_over_n = FPN_DivNoAssert(delta, count_fpn);
    w->mean = FPN_Add(w->mean, delta_over_n);
    // delta2 = value - mean_new
    FPN_Binary<F> delta2 = FPN_Sub(value, w->mean);
    // m2 += delta * delta2
    w->m2 = FPN_AddSat(w->m2, FPN_Mul(delta, delta2));
}

// population variance (m2 / count)
template <unsigned F>
inline FPN_Binary<F> Welford_Variance(const WelfordTracker<F> *w) {
    if (w->count < 2) return FPN_Zero<F>();
    FPN_Binary<F> count_fpn = FPN_FromDouble<F>((double)w->count);
    if (FPN_IsZero(count_fpn)) return FPN_Zero<F>();
    return FPN_DivNoAssert(w->m2, count_fpn);
}

// stddev via double conversion (FPN_Binary sqrt is expensive, slow-path only)
template <unsigned F>
inline FPN_Binary<F> Welford_Stddev(const WelfordTracker<F> *w) {
    double var = FPN_ToDouble(Welford_Variance(w));
    if (var <= 0.0) return FPN_Zero<F>();
    return FPN_FromDouble<F>(sqrt(var));
}

// z-score: (value - mean) / stddev
template <unsigned F>
inline double Welford_ZScore(const WelfordTracker<F> *w, FPN_Binary<F> value) {
    if (w->count < 2) return 0.0;
    double stddev = FPN_ToDouble(Welford_Stddev(w));
    if (stddev < 1e-15) return 0.0;
    double diff = FPN_ToDouble(FPN_Sub(value, w->mean));
    return diff / stddev;
}

template <unsigned F>
inline void Welford_Reset(WelfordTracker<F> *w) {
    *w = Welford_Init<F>();
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Welford_Push]
//======================================================================

#endif // WELFORD_STATS_HPP
