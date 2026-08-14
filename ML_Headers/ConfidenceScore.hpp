// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/ConfidenceScore.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SEAM]_[train-serve shared scorer — backtest suite (per-fold scoring) + live engine (ML threshold) consume the same confidence kernels]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[prediction-quality weighting — legacy 3-factor confidence + the v5.14.1.A 4-factor composite, the degradation-curve ladder, drift monitoring, and the snapshot persist registry]
// [DIAGRAM]_[formula]
//   legacy    (pre-v5.14.1): confidence = |IC| * freshness * stability        stability = 1 / (1 + RMSE)
//   composite (v5.14.1.A):   confidence = |IC| * freshness * capacity * (1 - clamp(RMSE / RMSE_baseline, 0, 1))
// [CONTAINS]
//   - [STRUCT]_[RollingWindow]           (generic ring template; + [FUNCTION]_[RollingWindow_Push] family: Init)
//   - [STRUCT]_[RollingIC]               (+ [FUNCTION]_[RollingIC_Push] family: Init; + [FUNCTION]_[RollingIC_Compute] with confidence_rank riding)
//   - [STRUCT]_[RollingRMSE]             (+ [FUNCTION]_[RollingRMSE_Push] family: Init/Compute — running-sum O(1))
//   - [FUNCTION]_[Confidence_Compute]    (+ Freshness/Stability helpers — the legacy 3-factor formula)
//   - [STRUCT]_[RollingFreshness]        (+ [FUNCTION]_[RollingFreshness_Compute] family: Init/Mark)
//   - [STRUCT]_[RollingCapacity]         (+ [FUNCTION]_[RollingCapacity_Compute] family: Init/UpdateADV)
//   - [STRUCT]_[ConfidenceScorer]        (+ [FUNCTION]_[ConfidenceScorer_Compute] API family: Init/ComputeICVariant/InitComposite/BindCompositeCfg/Update/UpdateAndMark)
//   - [FUNCTION]_[ConfidenceScorer_ComputeComposite]   (+ MarkPredict — the 4-factor opt-in path)
//   - [REGISTRY]_[FOREACH_DEGRADATION_CURVE]           (enum + dispatch table + string helpers + the 4 curve fns + bounds-checked wrapper share the block)
//   - [STRUCT]_[DriftSample] / [STRUCT]_[DriftHistory] (+ [FUNCTION]_[DriftHistory_CheckBreach] family: Init/Push)
//   - [REGISTRY]_[FOREACH_CONFIDENCE_PERSIST_FIELD]    (fieldwise write/read/commit + RecomputeRunningSums share the block)
//   - [STRUCT]_[RollingIC_LegacyV1] / [STRUCT]_[RollingRMSE_LegacyV1] / [STRUCT]_[RollingFreshness_LegacyV1] / [STRUCT]_[RollingCapacity_LegacyV1] / [STRUCT]_[ConfidenceScorerLegacyV1]   ([DEPRECATED] v11 wire-read targets — read-only, never written)
//   - [FUNCTION]_[ConfidenceScorer_ShadowLoadLegacyV1]   (reads the LegacyV1 targets; one-shot v11 migration)
// [REFERENCE]_[SOURCE]_[FoxML/private LIVE_TRADING/prediction/confidence.py]
//======================================================================================================
// port of FoxML/private LIVE_TRADING/prediction/confidence.py.
// weights predictions by quality: confidence = IC * freshness * stability.
//
// components:
//   IC        = rolling Spearman rank correlation (prediction vs actual)
//   freshness = exponential decay: e^(-dt / tau)
//   stability = 1 / (1 + rolling_RMSE)
//
// FoxML drops the capacity factor (kappa * ADV / planned_dollars) since we're
// single-symbol — the original port followed suit; since v5.14.1.A the
// composite path re-adds a capacity term (RollingCapacity below; inert at the
// target_dollars=0 default, so single-symbol behavior is unchanged).
//
// FoxML constants (from constants.py):
//   freshness_tau = 300.0 seconds (5 min)
//   MIN_IC_THRESHOLD = 0.01
//   ic_window = 20 predictions
//
// SHARED: used by both backtest suite (per-fold scoring) and live engine (ML threshold).
//
// FUTURE HOOKS:
//   multi-symbol: per-symbol IC buffers
//   multi-horizon: per-horizon tau values
//     → see ~/FoxML/private/LIVE_TRADING/common/constants.py FRESHNESS_TAU
//   capacity factor: LANDED v5.14.1.A (RollingCapacity — kappa * ADV / target_dollars)
//     → see ~/FoxML/private/LIVE_TRADING/prediction/confidence.py:calculate_capacity()
//======================================================================================================
#ifndef CONFIDENCE_SCORE_HPP
#define CONFIDENCE_SCORE_HPP

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>  // v5.14.9 — atoi for DegradationCurve_FromString numeric parse

#include <cstddef>   // v5.15.5.E.A — offsetof for layout-lock static_asserts
#include <math.h>
#include <string.h>
#include <strings.h>  // v5.14.9 — strcasecmp for DegradationCurve_FromString
#include "ICVariantRegistry.hpp"  // v5.14.1.F — FOREACH_IC_VARIANT X-macro
#include "../MemHeaders/BitmapMacros.hpp"  // v5.15.5.E.B — BITMAP_BIT_U8 + BITMAP_* accessors for DriftHistory flags

// default parameters (from FoxML constants.py + confidence.py)
#define CONFIDENCE_FRESHNESS_TAU_DEFAULT  300.0   // seconds (5 min decay)
#define CONFIDENCE_MIN_IC_DEFAULT         0.01    // floor for IC
#define CONFIDENCE_IC_WINDOW_DEFAULT      32      // rolling window size
#define CONFIDENCE_MIN_SAMPLES            5       // minimum for Spearman calc

//----------------------------------------------------------------------
// [SECTION]_[ROLLING IC — Spearman rank correlation]
//----------------------------------------------------------------------
// Spearman = Pearson correlation of ranks.
// simpler than scipy.stats.spearmanr, but same result for small windows.

#define ROLLING_IC_MAX_WINDOW 64

//======================================================================
// [STRUCT]_[RollingWindow]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[generic HOT-first ring-buffer template — RollingIC + RollingRMSE compose it (Class-18 mirror closed); BARE (no alignas) so the embedding consumer owns alignment]
// [INSTANTIATION]_[[double,64]]
// [REFERENCE]_[DESIGN_SPEC]_[generic-ring-buffer-template-pattern]
// [REFERENCE]_[CLASS]_[18]
//======================================================================
// [CODE]
//======================================================================
template <typename T, unsigned N>
struct RollingWindow {
    static constexpr unsigned CAPACITY = N;

    // HOT cluster (offset 0)
    int count;     // total items inserted (saturates at window)
    int head;      // ring buffer head
    int window;    // active window size [2, N]

    // 4B align pad (for T=double; 8-byte alignment) — samples at offset 16
    // COLD cluster
    T   samples[N];
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.E.C — Generic ring-buffer template. Variants compose this for
// type-specific math. Closes Class-18 mirror between RollingIC + RollingRMSE
// (both had identical count/head/window + samples ring-buffer skeleton).
// Pattern: DESIGN_SPECS/generic-ring-buffer-template-pattern.md.
//
// Design: BARE template (no internal alignas) so the embedding consumer
// owns alignment policy. RollingIC + RollingRMSE apply alignas(64) on
// THEIR struct definition; embedded RollingWindow<T, N> instances pack
// naturally inside.
//
// HOT-first: count + head + window at offset 0 (consumer's HOT cluster);
// samples[] follows after 4B alignment pad for double.
//
// Layout: 12B HOT + 4B pad + N×sizeof(T) samples; natural sizeof handles
// trailing alignment based on T's alignment.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[528B]
// [ALIGN]_[8]
// [CACHE_LINES]_[9]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[RollingWindow]
//======================================================================

//======================================================================
// [FUNCTION]_[RollingWindow_Push]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the generic ring API family (Init rides) — zero+validate init; write-head-advance-saturate push]
//======================================================================
// [CODE]
//======================================================================
// Generic Init: zero state, validate window range.
template <typename T, unsigned N>
static inline void RollingWindow_Init(RollingWindow<T, N>* w, int window) {
    memset(w, 0, sizeof(*w));
    if (window < 2)        window = 2;
    if (window > (int)N)   window = (int)N;
    w->window = window;
}

// Generic Push: write to head slot, advance, saturate count at window.
template <typename T, unsigned N>
static inline void RollingWindow_Push(RollingWindow<T, N>* w, T sample) {
    int idx = w->head % w->window;
    w->samples[idx] = sample;
    w->head++;
    if (w->count < w->window) w->count++;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RollingWindow_Push]
//======================================================================

// v5.15.5.E.C — Layout lock for the canonical RollingWindow<double, 64>
// instantiation. 12B HOT + 4B pad + 512B samples = 528 natural. NOT alignas
// (consumer owns alignment) → sizeof = 528.
using RollingWindowDoubleICT = RollingWindow<double, ROLLING_IC_MAX_WINDOW>;
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(RollingWindow<double,64>) == 528]
static_assert(sizeof(RollingWindowDoubleICT) == 528,
    "RollingWindow<double, 64> sizeof MUST be 528 B (12B HOT + 4B pad + 512B samples).");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(count) == 0]
static_assert(offsetof(RollingWindowDoubleICT, count) == 0,
    "RollingWindow HOT scalar `count` MUST sit at offset 0.");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(samples) == 16]
static_assert(offsetof(RollingWindowDoubleICT, samples) == 16,
    "RollingWindow COLD `samples` MUST sit at offset 16 (after HOT cluster + 4B pad).");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(RollingWindow<double,...>) == 8 — bare; consumer applies alignas(64)]
static_assert(alignof(RollingWindowDoubleICT) == 8,
    "RollingWindow<double, ...> bare alignof = 8 (consumer applies alignas(64)).");

//======================================================================
// [STRUCT]_[RollingIC]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the Spearman input pair — 2 composed RollingWindow rings (predictions + actuals) advancing in lockstep; alignas(64) applied HERE (the composition owner)]
// [REFERENCE]_[DESIGN_SPEC]_[generic-ring-buffer-template-pattern]
// [REFERENCE]_[INVARIANT]_[H9]
//======================================================================
// [CODE]
//======================================================================
struct alignas(64) RollingIC {
    RollingWindow<double, ROLLING_IC_MAX_WINDOW> predictions;
    RollingWindow<double, ROLLING_IC_MAX_WINDOW> actuals;
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.E.A/C — RollingIC now COMPOSES 2× RollingWindow<double, 64>
// (predictions + actuals) via the generic template. Class-18 mirror between
// RollingIC + RollingRMSE CLOSED structurally. Spearman rank correlation
// math (in RollingIC_Compute) accesses .predictions.samples / .actuals.samples
// via the template's field paths.
//
// Outer alignas(64): cache-line aligned + each RollingWindow sub-struct
// starts at offset 0 / 528 respectively. 528 mod 64 = 16 → actuals starts
// in line 8 (offset 528 = 64*8 + 16). HOT scalars of actuals are NOT at a
// cache line boundary, but they're accessed together with predictions's
// HOT scalars (same cycle) so prefetching keeps them warm.
//
// Wire format byte-identical to pre-.E.C: predictions array @ offset 0
// stays at offset 0 of `ic`; actuals @ offset 528 = offset 16 + 512;
// matches the prior pre-.E.A `ic.actuals[0]` at offset 512 (within the
// pre-.E.C struct). The FOREACH_CONFIDENCE_PERSIST_FIELD paths change
// from `ic.predictions` to `ic.predictions.samples` etc.; wire bytes
// remain identical (same data values + offsets).
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[1088B]
// [ALIGN]_[64]
// [CACHE_LINES]_[17]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[RollingIC]
//======================================================================

// v5.15.5.E.A/C — Layout lock for RollingIC composing 2× RollingWindow.
// predictions @ 0 (528B) + actuals @ 528 (528B) = 1056 natural; alignas(64)
// → 1088 (same as pre-.E.C). +32B trailing pad.
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(RollingIC) == 1088]
static_assert(sizeof(RollingIC) == 1088,
    "RollingIC sizeof MUST be 1088 B (17 cache lines with 32B trailing pad).");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(predictions) == 0]
static_assert(offsetof(RollingIC, predictions) == 0,
    "RollingIC `predictions` (RollingWindow) at offset 0.");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(actuals) == 528]
static_assert(offsetof(RollingIC, actuals) == 528,
    "RollingIC `actuals` (RollingWindow) at offset 528 (after predictions).");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(RollingIC) == 64]
static_assert(alignof(RollingIC) == 64,
    "RollingIC MUST be cache-line aligned.");

//======================================================================
// [FUNCTION]_[RollingIC_Push]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the (prediction, actual) pair API family (Init rides) — both rings advance in lockstep via the generic helpers]
//======================================================================
// [CODE]
//======================================================================
// v5.15.5.E.C — Init via generic RollingWindow_Init on both rings.
static inline void RollingIC_Init(RollingIC *ric, int window) {
    RollingWindow_Init(&ric->predictions, window);
    RollingWindow_Init(&ric->actuals,     window);
}

// v5.15.5.E.C — Push via generic RollingWindow_Push on both rings. The two
// rings advance in lockstep (same count + head + window after every Push).
// Generic Push handles the count/head/window mechanics; this wrapper coordinates
// the parallel-array push semantics for the (prediction, actual) pair shape.
static inline void RollingIC_Push(RollingIC *ric, double prediction, double actual) {
    RollingWindow_Push(&ric->predictions, prediction);
    RollingWindow_Push(&ric->actuals,     actual);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RollingIC_Push]
//======================================================================

//======================================================================
// [FUNCTION]_[RollingIC_Compute]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[textbook Spearman — rank both rings (confidence_rank helper rides) then Pearson-correlate the ranks; returns IC in [-1,1], 0.0 on insufficient data]
//======================================================================
// [CODE]
//======================================================================
// compute ranks for an array (1-based, average ties)
// simple O(n^2) — fine for window <= 64
static inline void confidence_rank(const double *values, double *ranks, int n) {
    for (int i = 0; i < n; i++) {
        double rank = 1.0;
        int ties = 1;
        for (int j = 0; j < n; j++) {
            if (j == i) continue;
            if (values[j] < values[i]) rank += 1.0;
            else if (values[j] == values[i]) ties++;
        }
        // average rank for ties
        ranks[i] = rank + (ties - 1) * 0.5;
    }
}

// compute Spearman rank correlation from ring buffer
// returns IC in [-1, 1], or 0.0 if insufficient data
//
// v5.15.5.E.C — Updated for composed RollingWindow layout. predictions +
// actuals rings have IDENTICAL count + head + window (kept in sync by
// RollingIC_Push). Read either; using predictions's metadata canonically.
static inline double RollingIC_Compute(const RollingIC *ric) {
    if (ric->predictions.count < CONFIDENCE_MIN_SAMPLES) return 0.0;

    int n      = ric->predictions.count;
    int head   = ric->predictions.head;
    int window = ric->predictions.window;
    double preds[ROLLING_IC_MAX_WINDOW], acts[ROLLING_IC_MAX_WINDOW];
    double pred_ranks[ROLLING_IC_MAX_WINDOW], act_ranks[ROLLING_IC_MAX_WINDOW];

    // copy ring buffer to contiguous arrays
    for (int i = 0; i < n; i++) {
        int idx = (head - n + i);
        if (idx < 0) idx += window;
        else idx = idx % window;
        preds[i] = ric->predictions.samples[idx];
        acts[i]  = ric->actuals.samples[idx];
    }

    // rank both arrays
    confidence_rank(preds, pred_ranks, n);
    confidence_rank(acts, act_ranks, n);

    // Pearson correlation of ranks
    double sum_pr = 0.0, sum_ar = 0.0;
    double sum_pr2 = 0.0, sum_ar2 = 0.0, sum_prar = 0.0;
    for (int i = 0; i < n; i++) {
        sum_pr += pred_ranks[i];
        sum_ar += act_ranks[i];
        sum_pr2 += pred_ranks[i] * pred_ranks[i];
        sum_ar2 += act_ranks[i] * act_ranks[i];
        sum_prar += pred_ranks[i] * act_ranks[i];
    }

    double mean_pr = sum_pr / n;
    double mean_ar = sum_ar / n;
    double cov = (sum_prar / n) - (mean_pr * mean_ar);
    double var_pr = (sum_pr2 / n) - (mean_pr * mean_pr);
    double var_ar = (sum_ar2 / n) - (mean_ar * mean_ar);

    if (var_pr <= 0.0 || var_ar <= 0.0) return 0.0;

    double ic = cov / (sqrt(var_pr) * sqrt(var_ar));
    // clamp to valid range (numerical safety)
    if (ic > 1.0) ic = 1.0;
    if (ic < -1.0) ic = -1.0;
    return ic;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RollingIC_Compute]
//======================================================================

//----------------------------------------------------------------------
// [SECTION]_[ROLLING RMSE — prediction calibration stability]
//----------------------------------------------------------------------

//======================================================================
// [STRUCT]_[RollingRMSE]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[composed RollingWindow of squared errors + the v5.15.5.E.D running sum — O(1) Compute via subtract-then-add maintenance at Push]
// [REFERENCE]_[DESIGN_SPEC]_[generic-ring-buffer-template-pattern]
// [REFERENCE]_[DESIGN_SPEC]_[sliding-window-online-statistics-pattern]
//======================================================================
// [CODE]
//======================================================================
struct alignas(64) RollingRMSE {
    RollingWindow<double, ROLLING_IC_MAX_WINDOW> window;
    double sum_squared_errors;   // v5.15.5.E.D — running sum maintained at Push
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.E.A/C — RollingRMSE COMPOSES RollingWindow<double, 64>. Class-18
// mirror with RollingIC closed via generic template per DESIGN_SPECS/generic-
// ring-buffer-template-pattern.md.
//
// v5.15.5.E.D — `sum_squared_errors` running aggregate added per sliding-
// window-online-statistics-pattern.md Approach 3. 3rd canonical application
// of the sliding-window pattern (1st: v5.14.11.A Ridge correlation; 2nd:
// v5.15.5.D BookImbHistory). Compute O(N=32) loop → O(1) running-sum read.
// Push maintains running sum via subtract-then-add at eviction (per the
// pattern). Bytewise parity test in tests/controller_test.cpp locks the
// running-sum-vs-walked equivalence.
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
// [END_STRUCT]_[RollingRMSE]
//======================================================================

// v5.15.5.E.D — Layout lock. window (528B) + sum_squared_errors (8B) = 536
// natural; alignas(64) → 576 B (same as .E.A pre-.E.D; +40B trailing pad).
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(RollingRMSE) == 576]
static_assert(sizeof(RollingRMSE) == 576,
    "RollingRMSE sizeof MUST be 576 B (9 cache lines).");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(window) == 0]
static_assert(offsetof(RollingRMSE, window) == 0,
    "RollingRMSE `window` (RollingWindow) at offset 0.");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(sum_squared_errors) == 528]
static_assert(offsetof(RollingRMSE, sum_squared_errors) == 528,
    "RollingRMSE `sum_squared_errors` at offset 528 (immediately after window struct).");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(RollingRMSE) == 64]
static_assert(alignof(RollingRMSE) == 64,
    "RollingRMSE MUST be cache-line aligned.");

//======================================================================
// [FUNCTION]_[RollingRMSE_Push]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[squared-error API family (Init + O(1) Compute ride) — Push maintains the running sum via subtract-then-add at eviction]
// [REFERENCE]_[DESIGN_SPEC]_[sliding-window-online-statistics-pattern]
//======================================================================
// [CODE]
//======================================================================
static inline void RollingRMSE_Init(RollingRMSE *r, int window) {
    RollingWindow_Init(&r->window, window);
    r->sum_squared_errors = 0.0;   // v5.15.5.E.D — defensive explicit zero (memset covers it)
}

// v5.15.5.E.D — Push maintains running sum_squared_errors via subtract-then-
// add per sliding-window-online-statistics-pattern.md. At eviction (count >=
// window), the oldest squared_error at samples[head % window] is subtracted
// from sum_squared_errors BEFORE the new value overwrites it. Warm-up phase
// (count < window) just accumulates (no eviction; both walked + running paths
// produce identical sum until window saturates).
//
// Per-Push cost: 1 extra subtract + 1 extra read of evicted sample (typically
// L1-warm since the ring is small). Compute cost: O(N=32) loop → O(1) single
// load. Net win at slow-path Compute cadence (~1-2 Hz × Compute called many
// times per cycle in composite confidence): ~few hundred ns / cycle / core.
//
// Floating-point associativity caveat: chronological accumulation order vs
// the walked sum's iteration order COULD produce different bytes in theory.
// In practice for squared-error magnitudes (small positive doubles bounded
// by realistic prediction errors), all sums fit in mantissa without
// catastrophic cancellation → bytewise-identical via bytewise parity test
// in controller_test.cpp.
static inline void RollingRMSE_Push(RollingRMSE *r, double prediction, double actual) {
    double err = prediction - actual;
    double new_se = err * err;
    int idx = r->window.head % r->window.window;
    if (r->window.count >= r->window.window) {
        // Eviction: subtract oldest sample before overwrite
        r->sum_squared_errors -= r->window.samples[idx];
    }
    r->sum_squared_errors += new_se;
    // window write through the generic helper (count + head + samples)
    r->window.samples[idx] = new_se;
    r->window.head++;
    if (r->window.count < r->window.window) r->window.count++;
}

// v5.15.5.E.D — O(1) RollingRMSE_Compute via running sum_squared_errors.
// Pre-.E.D was O(N=32) loop. Bytewise parity vs the walked path is locked
// by tests/controller_test.cpp parity loop (200-push deterministic sequence
// covering warm-up + steady-state).
//
// Pattern: sliding-window-online-statistics-pattern.md Approach 3 (sliding-
// window incremental). 3rd canonical application of the sliding-window
// pattern (the spec tracks its production sites).
static inline double RollingRMSE_Compute(const RollingRMSE *r) {
    if (r->window.count < 2) return 1.0; // high RMSE = low confidence until enough data
    return sqrt(r->sum_squared_errors / (double)r->window.count);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RollingRMSE_Push]
//======================================================================

//----------------------------------------------------------------------
// [SECTION]_[CONFIDENCE COMPUTATION]
//----------------------------------------------------------------------

//======================================================================
// [FUNCTION]_[Confidence_Compute]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the legacy 3-factor formula (Freshness + Stability helpers ride) — abs(IC) floored at MIN_IC, freshness decay, 1/(1+RMSE) stability]
// [DIAGRAM]_[formula]
//   confidence = |IC| * freshness * stability
//     IC:        abs(Spearman rank correlation), floored at MIN_IC
//     freshness: e^(-data_age_sec / tau)
//     stability: 1 / (1 + RMSE)
//======================================================================
// [CODE]
//======================================================================
static inline double Confidence_Freshness(double data_age_sec, double tau) {
    if (tau <= 0.0) tau = CONFIDENCE_FRESHNESS_TAU_DEFAULT;
    if (data_age_sec <= 0.0) return 1.0;  // fresh data = max freshness
    return exp(-data_age_sec / tau);
}

static inline double Confidence_Stability(double rmse) {
    return 1.0 / (1.0 + rmse);
}

static inline double Confidence_Compute(double ic, double data_age_sec, double rmse,
                                          double freshness_tau) {
    // use absolute IC (direction handled elsewhere)
    double abs_ic = (ic >= 0.0) ? ic : -ic;
    if (abs_ic < CONFIDENCE_MIN_IC_DEFAULT) abs_ic = CONFIDENCE_MIN_IC_DEFAULT;

    double freshness = Confidence_Freshness(data_age_sec, freshness_tau);
    double stability = Confidence_Stability(rmse);

    return abs_ic * freshness * stability;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Confidence_Compute]
//======================================================================

//----------------------------------------------------------------------
// [SECTION]_[v5.14.1.A — COMPOSITE CONFIDENCE COMPONENTS]
//----------------------------------------------------------------------
// Composite formula: IC × Freshness × Capacity × Stability_normalized
// where Stability_normalized = 1 - clamp(rmse / rmse_baseline, 0, 1).
//
// Each component is independently observable + cfg-tunable, replacing the
// older 3-factor (IC * Freshness * 1/(1+RMSE)) with a 4-factor formulation
// that adds a Capacity term + normalizes Stability against a baseline RMSE
// pulled from training. Enables soft risk degradation (v5.14.9) by giving
// the sizing path a continuous [0, 1] confidence scalar instead of a
// binary kill-switch trip.

// Default kappa for capacity calc (proportionality constant on ADV).
#define CONFIDENCE_CAPACITY_KAPPA_DEFAULT  0.1
// Default ADV smoothing alpha (10-sample EWMA).
#define CONFIDENCE_CAPACITY_ALPHA_DEFAULT  0.1

//======================================================================
// [STRUCT]_[RollingFreshness]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[wall-clock freshness state — the scorer owns its own clock (Mark) instead of a caller-passed data_age_sec]
//======================================================================
// [CODE]
//======================================================================
struct RollingFreshness {
    uint64_t last_predict_us;   // wall-clock at last prediction (Mark)
    double   tau_secs;          // exponential decay time constant
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Wall-clock-driven freshness with cfg-tunable tau. Replaces the
// data_age_sec arg of the original Confidence_Freshness so the scorer
// owns its own clock state — operator + tests can manipulate via Mark.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[16B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[RollingFreshness]
//======================================================================

//======================================================================
// [FUNCTION]_[RollingFreshness_Compute]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[freshness in [0,1] (Init + Mark ride) — exp decay from last Mark; 0 when never marked (cold-start = stale); backward time clamps to 1]
//======================================================================
// [CODE]
//======================================================================
static inline void RollingFreshness_Init(RollingFreshness *f, double tau_secs) {
    f->last_predict_us = 0;
    f->tau_secs = (tau_secs > 0.0) ? tau_secs : CONFIDENCE_FRESHNESS_TAU_DEFAULT;
}

static inline void RollingFreshness_Mark(RollingFreshness *f, uint64_t now_us) {
    f->last_predict_us = now_us;
}

// freshness ∈ [0, 1]. Returns 0 when never marked (cold-start = stale).
// When time travels backward (now_us < last_predict_us, e.g. test fixture
// or replay determinism), clamp to 1.0.
static inline double RollingFreshness_Compute(const RollingFreshness *f, uint64_t now_us) {
    if (f->last_predict_us == 0) return 0.0;
    if (now_us <= f->last_predict_us) return 1.0;
    double tau = (f->tau_secs > 0.0) ? f->tau_secs : CONFIDENCE_FRESHNESS_TAU_DEFAULT;
    double age_sec = (double)(now_us - f->last_predict_us) / 1e6;
    return exp(-age_sec / tau);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RollingFreshness_Compute]
//======================================================================

//======================================================================
// [STRUCT]_[RollingCapacity]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[market-absorption factor state — EWMA-smoothed ADV vs position-size target; target_dollars=0 = unbounded (capacity always 1.0)]
//======================================================================
// [CODE]
//======================================================================
struct RollingCapacity {
    double current_adv;       // EWMA-smoothed average daily volume estimate
    double target_dollars;    // cfg-tunable position-size target; 0 = unbounded
    double kappa;             // proportionality constant
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Capacity factor: how much of the desired position size the market can
// absorb without slippage degradation. target_dollars=0 = unbounded
// (single-symbol small-account default; capacity always 1.0).
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[24B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[RollingCapacity]
//======================================================================

//======================================================================
// [FUNCTION]_[RollingCapacity_Compute]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[capacity in [0,1] (Init + UpdateADV ride) — kappa*ADV/target clamped; 1.0 when target_dollars=0 (unbounded default)]
//======================================================================
// [CODE]
//======================================================================
static inline void RollingCapacity_Init(RollingCapacity *c,
                                          double target_dollars, double kappa) {
    c->current_adv    = 0.0;
    c->target_dollars = (target_dollars >= 0.0) ? target_dollars : 0.0;
    c->kappa          = (kappa > 0.0) ? kappa : CONFIDENCE_CAPACITY_KAPPA_DEFAULT;
}

static inline void RollingCapacity_UpdateADV(RollingCapacity *c, double new_adv) {
    if (new_adv < 0.0) new_adv = 0.0;
    if (c->current_adv == 0.0) {
        c->current_adv = new_adv;
    } else {
        const double alpha = CONFIDENCE_CAPACITY_ALPHA_DEFAULT;
        c->current_adv = (1.0 - alpha) * c->current_adv + alpha * new_adv;
    }
}

static inline double RollingCapacity_Compute(const RollingCapacity *c) {
    if (c->target_dollars <= 0.0) return 1.0;
    double cap = (c->kappa * c->current_adv) / c->target_dollars;
    if (cap > 1.0) cap = 1.0;
    if (cap < 0.0) cap = 0.0;
    return cap;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RollingCapacity_Compute]
//======================================================================

//----------------------------------------------------------------------
// [SECTION]_[FULL CONFIDENCE SCORER — combines IC + RMSE buffers]
//----------------------------------------------------------------------

//======================================================================
// [STRUCT]_[ConfidenceScorer]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the full scorer — HOT scalars at line 0, WARM ic+rmse rings at cache-line boundaries, COLD composite-mode fields at the tail (decision-first ND3)]
// [DIAGRAM]
//   line 0:      [last_confidence:8][freshness_tau:8][pad:48]
//   lines 1-17:  ic   (RollingIC, 1088B)
//   lines 18-26: rmse (RollingRMSE, 576B)
//   lines 27:    freshness(16B) + capacity(24B) + rmse_baseline(8B) + pad(16B)
// [REFERENCE]_[DESIGN_SPEC]_[cache-layout-discipline-for-hot-side-structs]
// [REFERENCE]_[DESIGN_SPEC]_[[decision-first-cluster-layout-pattern] [cache-layout-discipline-for-hot-side-structs.md]]
//======================================================================
// [CODE]
//======================================================================
struct alignas(64) ConfidenceScorer {
    //---- [SECTION]_[HOT cluster (offset 0; touched every slow-path Compute call)] ----
    double last_confidence;       // cached result; written per Compute, read per snapshot
    double freshness_tau;         // read per Compute
    // 48B alignment pad → ic starts at offset 64 (alignas(64) on RollingIC)

    //---- [SECTION]_[WARM cluster (offsets 64+; ring buffers updated per Push 10-100Hz)] ----
    RollingIC ic;                 // 1088B = 17 cache lines
    RollingRMSE rmse;             // 576B = 9 cache lines

    //---- [SECTION]_[COLD cluster (composite-mode-only; touched once at boot + per Compute when enabled)] ----
    RollingFreshness freshness;   // v5.14.1.A — 16B
    RollingCapacity capacity;     // v5.14.1.A — 24B
    double rmse_baseline;         // v5.14.1.A — bound to training-time RMSE; default 1.0
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.14.1.A — added freshness + capacity + rmse_baseline for composite formula.
// Pre-v5.14.1 fields (ic, rmse, freshness_tau, last_confidence) preserved;
// existing ConfidenceScorer_Compute path bytewise unchanged when caller
// stays on the IC-only API. Composite opt-in via cfg flag (v5.14.1.B).
//
// IC SEMANTICS NOTE (v5.14.1.F doc-fix): the `ic` field's struct type
// `RollingIC` is generically named but its `RollingIC_Compute` body (above)
// ranks both predictions+actuals then
// computes Pearson correlation of the ranks = textbook **Spearman rank
// correlation**. Despite the generic struct name, this has been Spearman
// since v5.x.x. cfg.confidence_ic_variant=0 (default) selects this Spearman
// implementation via the FOREACH_IC_VARIANT registry; future variants
// (Pearson, Kendall, etc.) slot in without disturbing this field's wiring.
// v5.15.5.E.A — alignas(64) + HOT-first reorg + decision-first cluster
// ordering per cache-layout-discipline-for-hot-side-structs.md Rule 4 +
// decision-first-cluster-layout-pattern.md (ND3). HOT scalars
// (last_confidence + freshness_tau) at offset 0..15 in line 0; WARM rings
// (ic + rmse) follow at cache-line boundaries; COLD composite-mode-only
// fields at end.
//
// Wire format decoupled from runtime layout per .E.0 (FOREACH_CONFIDENCE_
// PERSIST_FIELD registry handles serialization independent of field offsets).
// Pre-.E.A frozen layout preserved in ConfidenceScorerLegacyV1 for shadow-
// load migration of v11 snapshots.
//
// Pre-.E.A design note about active variant + sizeof freeze is RESOLVED:
// runtime layout can now evolve (snapshot v12 = field-by-field). Future
// fields can be added via FOREACH_CONFIDENCE_PERSIST_FIELD registry.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[1792B]
// [ALIGN]_[64]
// [CACHE_LINES]_[28]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[ConfidenceScorer]
//======================================================================

// v5.15.5.E.A — Layout lock for ConfidenceScorer. last_confidence at offset 0
// (HOT scalar; per-Compute read + snapshot read; decision-first ordering).
// freshness_tau at offset 8 (HOT scalar; per-Compute read). Then 48B align
// pad to bring ic to next 64-aligned boundary. ic + rmse cache-line-aligned.
//
// Field offsets:
//   last_confidence @ 0
//   freshness_tau   @ 8
//   ic              @ 64 (after 48B pad)
//   rmse            @ 64 + 1088 = 1152
//   freshness       @ 1152 + 576 = 1728
//   capacity        @ 1728 + 16  = 1744
//   rmse_baseline   @ 1744 + 24  = 1768
//   end at 1776; alignas(64) outer → sizeof = 1792 (+16B trailing pad)
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(ConfidenceScorer) == 1792]
static_assert(sizeof(ConfidenceScorer) == 1792,
    "ConfidenceScorer sizeof MUST be 1792 B (28 cache lines exact).");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(last_confidence) == 0 — decision-first ND3]
static_assert(offsetof(ConfidenceScorer, last_confidence) == 0,
    "ConfidenceScorer HOT scalar `last_confidence` MUST sit at offset 0 "
    "(decision-first ordering per ND3).");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(freshness_tau) == 8]
static_assert(offsetof(ConfidenceScorer, freshness_tau) == 8,
    "ConfidenceScorer HOT scalar `freshness_tau` MUST sit at offset 8.");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(ic) == 64]
static_assert(offsetof(ConfidenceScorer, ic) == 64,
    "ConfidenceScorer WARM cluster `ic` MUST sit at offset 64 (cache-line aligned).");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(rmse) == 1152]
static_assert(offsetof(ConfidenceScorer, rmse) == 1152,
    "ConfidenceScorer WARM cluster `rmse` MUST sit at offset 1152 (after ic).");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(ConfidenceScorer) == 64]
static_assert(alignof(ConfidenceScorer) == 64,
    "ConfidenceScorer MUST be cache-line aligned.");

//======================================================================
// [FUNCTION]_[ConfidenceScorer_Compute]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the scorer API family (Init / ComputeICVariant / InitComposite / BindCompositeCfg / Update / UpdateAndMark ride) — legacy 3-factor Compute is the terminus; composite path is the next block]
// [REFERENCE]_[CLASS]_[4]
// [REFERENCE]_[PARITY]_[PARITY-3]
//======================================================================
// [CODE]
//======================================================================
static inline void ConfidenceScorer_Init(ConfidenceScorer *cs, int window, double tau) {
    RollingIC_Init(&cs->ic, (window > 0) ? window : CONFIDENCE_IC_WINDOW_DEFAULT);
    RollingRMSE_Init(&cs->rmse, (window > 0) ? window : CONFIDENCE_IC_WINDOW_DEFAULT);
    // v5.9.1 (V5_9_AUDIT-#13) — surface silent default fallback. Cfg parser
    // now refuses tau<=0, but defensive code-path callers (older tests,
    // direct embeds) still hit this branch. WARN once at boot so the
    // operator knows why their cfg value isn't taking effect.
    if (tau <= 0.0) {
        fprintf(stderr, "[WARN] ConfidenceScorer_Init: tau=%.3f invalid, using default %.1f\n",
                tau, (double)CONFIDENCE_FRESHNESS_TAU_DEFAULT);
        cs->freshness_tau = CONFIDENCE_FRESHNESS_TAU_DEFAULT;
    } else {
        cs->freshness_tau = tau;
    }
    cs->last_confidence = 0.0;
    // v5.14.1.A — composite components default to "no-op" so legacy
    // ConfidenceScorer_Compute path is bytewise unchanged. Operator
    // tunes via cfg in v5.14.1.B; ComputeComposite is opt-in.
    RollingFreshness_Init(&cs->freshness, cs->freshness_tau);
    RollingCapacity_Init(&cs->capacity, /*target_dollars=*/0.0,
                          /*kappa=*/CONFIDENCE_CAPACITY_KAPPA_DEFAULT);
    cs->rmse_baseline = 1.0;  // safe default; bound to training-time RMSE in v5.14.1.B
}

// v5.14.1.F — variant-aware IC dispatcher. Routes to the registered
// variant's compute fn via FOREACH_IC_VARIANT X-macro. Caller passes
// `variant` explicitly (typically `cfg.confidence_ic_variant` from
// ControllerConfig) — NOT cached on ConfidenceScorer to avoid changing
// sizeof and breaking the PortfolioController snapshot save/load pair
// (Class 4 — snapshot save/load asymmetry).
//
// Out-of-range variant → falls through to default case in the dispatch
// switch (returns 0.0; safe). Existing sites that DON'T need variant
// awareness (e.g., legacy ConfidenceScorer_Compute internal path; kept
// bytewise unchanged) can keep direct RollingIC_Compute(&cs->ic) calls.
// New code + sites being refactored for variant choice use this dispatcher.
static inline double ConfidenceScorer_ComputeICVariant(const ConfidenceScorer *cs,
                                                         int variant) {
    if (!cs) return 0.0;
    return IC_VARIANT_COMPUTE(cs, variant);
}

// v5.14.1.A — extended init for composite path. Equivalent to base Init +
// explicit composite parameters. Useful for tests + v5.14.1.B cfg wiring.
static inline void ConfidenceScorer_InitComposite(ConfidenceScorer *cs,
                                                    int window, double tau,
                                                    double freshness_tau_secs,
                                                    double capacity_target_dollars,
                                                    double capacity_kappa,
                                                    double rmse_baseline) {
    ConfidenceScorer_Init(cs, window, tau);
    RollingFreshness_Init(&cs->freshness, freshness_tau_secs);
    RollingCapacity_Init(&cs->capacity, capacity_target_dollars, capacity_kappa);
    cs->rmse_baseline = (rmse_baseline > 0.0) ? rmse_baseline : 1.0;
}

// v5.14.1.B.1 (PARITY-003 fix) — push composite cfg fields into a scorer
// AFTER ConfidenceScorer_Init has run. Designed to be called from boot sites
// alongside the existing Init call so legacy callers stay compatible
// (composite_enabled=0 → leaves Init's safe defaults in place).
//
// Avoids circular include risk by taking primitive doubles instead of
// ControllerConfig<F>* (ConfidenceScore.hpp lives in ML_Headers/, cfg lives
// in CoreFrameworks/ — keeping the dependency direction one-way).
//
// Caller pattern (the engine bind sites: EngineCommon boot, StrategyParameters,
// ShardedSnapshotPersist post-load, PortfolioController legacy):
//   ConfidenceScorer_Init(&cs, window, base_tau);
//   ConfidenceScorer_BindCompositeCfg(&cs,
//       BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED),
//       FPN_ToDouble(cfg.confidence_freshness_tau_secs),
//       FPN_ToDouble(cfg.confidence_capacity_target_dollars),
//       FPN_ToDouble(cfg.confidence_capacity_kappa),
//       FPN_ToDouble(cfg.confidence_rmse_baseline));
static inline void ConfidenceScorer_BindCompositeCfg(ConfidenceScorer *cs,
                                                       int composite_enabled,
                                                       double freshness_tau_secs,
                                                       double capacity_target_dollars,
                                                       double capacity_kappa,
                                                       double rmse_baseline) {
    if (!composite_enabled) return;  // legacy path; leave Init defaults
    RollingFreshness_Init(&cs->freshness, freshness_tau_secs);
    RollingCapacity_Init(&cs->capacity, capacity_target_dollars, capacity_kappa);
    cs->rmse_baseline = (rmse_baseline > 0.0) ? rmse_baseline : 1.0;
}

// feed a prediction + actual return pair (call after outcome is known)
static inline void ConfidenceScorer_Update(ConfidenceScorer *cs,
                                             double prediction, double actual) {
    RollingIC_Push(&cs->ic, prediction, actual);
    RollingRMSE_Push(&cs->rmse, prediction, actual);
}

// v5.14.1.B — Update + Mark in one call. Use this from production sites
// when composite confidence is enabled so freshness reflects "how recently
// we observed a calibration data point". Wall-clock now_us drives the
// freshness decay (Compute reads it via ComputeComposite's now_us arg).
//
// Backwards-compat: existing _Update sites can stay on the 3-arg form
// when composite is disabled (composite path is opt-in via cfg).
static inline void ConfidenceScorer_UpdateAndMark(ConfidenceScorer *cs,
                                                    double prediction,
                                                    double actual,
                                                    uint64_t now_us) {
    ConfidenceScorer_Update(cs, prediction, actual);
    RollingFreshness_Mark(&cs->freshness, now_us);
}

// compute current confidence given data age
static inline double ConfidenceScorer_Compute(ConfidenceScorer *cs, double data_age_sec) {
    double ic = RollingIC_Compute(&cs->ic);
    double rmse = RollingRMSE_Compute(&cs->rmse);
    cs->last_confidence = Confidence_Compute(ic, data_age_sec, rmse, cs->freshness_tau);
    return cs->last_confidence;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ConfidenceScorer_Compute]
//======================================================================

//======================================================================
// [FUNCTION]_[ConfidenceScorer_ComputeComposite]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the v5.14.1.A 4-factor opt-in path (MarkPredict rides) — returns [0,1]; caller-provided now_us pins time for replay determinism + tests]
// [DIAGRAM]_[formula]
//   composite = |IC| * freshness * capacity * stability_normalized
//   stability_normalized = 1 - clamp(rmse / rmse_baseline, 0, 1)
//======================================================================
// [CODE]
//======================================================================
static inline double ConfidenceScorer_ComputeComposite(ConfidenceScorer *cs,
                                                          uint64_t now_us) {
    double ic   = RollingIC_Compute(&cs->ic);
    double rmse = RollingRMSE_Compute(&cs->rmse);
    double abs_ic = (ic >= 0.0) ? ic : -ic;
    if (abs_ic < CONFIDENCE_MIN_IC_DEFAULT) abs_ic = CONFIDENCE_MIN_IC_DEFAULT;

    double fresh = RollingFreshness_Compute(&cs->freshness, now_us);
    double capac = RollingCapacity_Compute(&cs->capacity);

    // Stability normalized: 1 when rmse=0 (perfect cal); 0 when rmse>=baseline
    // (no edge vs training). Clamp protects against rmse_baseline misconfig
    // (e.g. operator forgot to bind from training).
    double baseline = (cs->rmse_baseline > 0.0) ? cs->rmse_baseline : 1.0;
    double stab_ratio = rmse / baseline;
    if (stab_ratio > 1.0) stab_ratio = 1.0;
    if (stab_ratio < 0.0) stab_ratio = 0.0;
    double stability = 1.0 - stab_ratio;

    double composite = abs_ic * fresh * capac * stability;
    cs->last_confidence = composite;
    return composite;
}

// v5.14.1.A — convenience wrapper for callers that don't track now_us
// directly (e.g. simple tests, single-shot manual eval). Production
// path should pass now_us explicitly for replay-determinism + test
// fixture control.
static inline void ConfidenceScorer_MarkPredict(ConfidenceScorer *cs, uint64_t now_us) {
    RollingFreshness_Mark(&cs->freshness, now_us);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// 4-factor composite: IC × Freshness × Capacity × Stability_normalized.
//
// Differs from ConfidenceScorer_Compute (3-factor IC × Freshness ×
// 1/(1+RMSE)) in three ways:
//   1. Adds Capacity term (silently 1.0 when target_dollars=0; default).
//   2. Stability is normalized vs rmse_baseline (training-time RMSE),
//      so "stability" means "how close are we to training-time
//      calibration" rather than "absolute RMSE magnitude".
//   3. Freshness uses wall-clock now_us against last Mark, not a
//      caller-passed data_age_sec — the scorer owns its own clock state.
//
// Returns scalar in [0, 1]. Caller-provided now_us so tests + replay-
// determinism can pin time.
//
// Mark must be called when a prediction is generated (typically inside
// the slow-path predict loop, between Features_PackAll + Model_Predict).
// Update is called when the outcome is known (post-fill, same as legacy
// ConfidenceScorer_Update).
//======================================================================
// [END_FUNCTION]_[ConfidenceScorer_ComputeComposite]
//======================================================================

//======================================================================
// [REGISTRY]_[FOREACH_DEGRADATION_CURVE]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[v5.14.9 soft risk degradation ladder — composite confidence -> sizing-multiplier curve; enum + dispatch table + string helpers + the 4 BRANCHLESS curve fns + bounds-checked wrapper all auto-flow from the rows]
// [COLUMN]_[name]_[curve identifier -> CURVE_<name> enum + Confidence_DegradationScale_<Name> fn naming]
// [COLUMN]_[enum_value]_[dense 0..N-1 — indexes the fn-ptr dispatch table]
// [COLUMN]_[compute_fn]_[the curve implementation wired into degradation_curve_fns[]]
// [COLUMN]_[doc_string]_[operator-facing curve description]
// [REFERENCE]_[DESIGN_SPEC]_[curve-registry-pattern]
// [REFERENCE]_[DESIGN_SPEC]_[[x-macro-registry-with-presence-dispatch] [curve-registry-pattern.md]]
//======================================================================
// [CODE]
//======================================================================
// Composite confidence → sizing-multiplier scaling. Replaces the broken-for-
// composite v5.12.1.D math (which compared conf_now ∈ [0.001, 0.3] against
// ml_buy_threshold ∈ [0.5, 0.7] → factor=0 silently).
//
// FOREACH_DEGRADATION_CURVE(X) registry — adding a new curve is 1 line:
//   1. Append X(NAME, val, fn, "doc") below
//   2. Implement Confidence_DegradationScale_<NAME>
//   Auto-generated: enum DegradationCurve, dispatch table curve_fns[],
//   FOREACH_DEGRADATION_CURVE_COUNT, ToString helper.
//
// Curve signature: f(conf, full, min, min_pct) → factor ∈ [0, 1].
//   conf      — composite confidence ∈ [0, 1] (or legacy IC scale)
//   full      — threshold above which factor=1.0 (full size)
//   min       — threshold below which factor=0.0 (block; ladder bottom)
//   min_pct   — factor at min (typical 0.10 = 10% of base) so the curve
//               doesn't drop to zero immediately above min; ladder bottom
//               only fires strictly below min
//
// All compute fns are BRANCHLESS (mask compute via fmin/fmax/cmov + fma).
// SIMD-friendly if curve becomes vectorized (multi-core fan-out scenario).
//
// Pattern documented in DESIGN_SPECS/curve-registry-pattern.md.
// Slow-path-only; hot path UNTOUCHED.

//----------------------------------------------------------------------
// [SECTION]_[forward decls + the registry rows]
//----------------------------------------------------------------------
// Forward-declare curve compute fns so the dispatch table can reference them.
inline double Confidence_DegradationScale_Off    (double, double, double, double);
inline double Confidence_DegradationScale_Linear (double, double, double, double);
inline double Confidence_DegradationScale_Exp    (double, double, double, double);
inline double Confidence_DegradationScale_Step   (double, double, double, double);

// Tuple: X(name, enum_value, compute_fn, doc_string)
#define FOREACH_DEGRADATION_CURVE(X)                                                                  \
    X(OFF,    0, Confidence_DegradationScale_Off,    "disabled — factor=1.0; preserves pre-v5.14.9") \
    X(LINEAR, 1, Confidence_DegradationScale_Linear, "linear interp between (min, min_pct) and (full, 1.0)") \
    X(EXP,    2, Confidence_DegradationScale_Exp,    "quadratic falloff; preserves more size in middle") \
    X(STEP,   3, Confidence_DegradationScale_Step,   "binary 1.0 above midpoint else min_pct (debug)")

//----------------------------------------------------------------------
// [SECTION]_[auto-generated consumers — enum + count + dispatch table + string helpers]
//----------------------------------------------------------------------
// Auto-generated enum (CURVE_OFF / CURVE_LINEAR / CURVE_EXP / CURVE_STEP).
#define X_GEN_ENUM(name, val, fn, doc) CURVE_##name = val,
enum DegradationCurve {
    FOREACH_DEGRADATION_CURVE(X_GEN_ENUM)
};
#undef X_GEN_ENUM

// Auto-generated count. NOTE: X_GEN_DEGRADATION_COUNT_ONE stays defined
// (the COUNT macro defers expansion to use sites; undef'ing the helper
// would break later expansions). Same pattern as FOREACH_STAMP_BOUND_CFG_COUNT.
#define X_GEN_DEGRADATION_COUNT_ONE(name, val, fn, doc) +1
#define FOREACH_DEGRADATION_CURVE_COUNT (0 FOREACH_DEGRADATION_CURVE(X_GEN_DEGRADATION_COUNT_ONE))

// Function-pointer dispatch table. Indexed by curve enum value.
// Slow-path: 1 indirect call (~1-2ns); branch predictor handles cfg-stable curves.
typedef double (*DegradationCurveFn)(double conf, double full, double min, double min_pct);

#define X_GEN_FN_PTR(name, val, fn, doc) fn,
static const DegradationCurveFn degradation_curve_fns[] = {
    FOREACH_DEGRADATION_CURVE(X_GEN_FN_PTR)
};
#undef X_GEN_FN_PTR

// Auto-generated ToString — for cfg parser + GUI display.
static inline const char* DegradationCurve_ToString(int curve) {
    switch (curve) {
        #define X_GEN_TOSTRING(name, val, fn, doc) case val: return #name;
        FOREACH_DEGRADATION_CURVE(X_GEN_TOSTRING)
        #undef X_GEN_TOSTRING
        default: return "INVALID";
    }
}

// Auto-generated FromString — for cfg parser. Accepts string ("LINEAR") or
// numeric ("1") forms; case-insensitive on string form. Returns -1 on miss.
static inline int DegradationCurve_FromString(const char* s) {
    if (!s || !*s) return -1;
    // Try numeric first
    if (s[0] >= '0' && s[0] <= '9') {
        int v = atoi(s);
        if (v >= 0 && v < FOREACH_DEGRADATION_CURVE_COUNT) return v;
        return -1;
    }
    // Case-insensitive string match
    #define X_GEN_FROMSTRING(name, val, fn, doc)                          \
        if (strcasecmp(s, #name) == 0) return val;
    FOREACH_DEGRADATION_CURVE(X_GEN_FROMSTRING)
    #undef X_GEN_FROMSTRING
    return -1;
}

//----------------------------------------------------------------------
// [SECTION]_[CURVE COMPUTE FNS — v5.14.9.A]
//----------------------------------------------------------------------
// All branchless. fmin/fmax → cmov; fma → 1 cycle on modern x86.
// Defensive: if full <= min (operator misconfig), return min_pct unconditionally
// (avoids div-by-zero; operator gets predictable degraded behavior).

// OFF — factor=1.0 unconditionally. Preserves pre-v5.14.9 behavior bytewise
// when cfg.risk_degradation_curve=0 (default).
inline double Confidence_DegradationScale_Off(double conf, double full, double min, double min_pct) {
    (void)conf; (void)full; (void)min; (void)min_pct;
    return 1.0;
}

// LINEAR — interp between (min, min_pct) and (full, 1.0). Below min, returns
// min_pct (caller treats factor==0 as ladder-bottom hit; here min_pct ≥ 0).
// To get ladder-bottom (factor=0), operator sets min_pct=0.0.
inline double Confidence_DegradationScale_Linear(double conf, double full, double min, double min_pct) {
    if (full <= min) return min_pct;  // misconfig guard
    double clamped = fmin(fmax(conf, min), full);
    double t = (clamped - min) / (full - min);  // ∈ [0, 1]
    return fma(t, 1.0 - min_pct, min_pct);      // min_pct + t*(1-min_pct)
}

// EXP — quadratic falloff: factor = min_pct + t² * (1-min_pct). Steeper drop
// near min; preserves more size in the middle of the range. Same endpoints
// as LINEAR.
inline double Confidence_DegradationScale_Exp(double conf, double full, double min, double min_pct) {
    if (full <= min) return min_pct;  // misconfig guard
    double clamped = fmin(fmax(conf, min), full);
    double t = (clamped - min) / (full - min);
    double t_sq = t * t;
    return fma(t_sq, 1.0 - min_pct, min_pct);
}

// STEP — binary above/below midpoint. factor = 1.0 if conf >= (full+min)/2
// else min_pct. Useful for debugging/paper-test "did the ladder fire?"
// without continuous-curve noise.
inline double Confidence_DegradationScale_Step(double conf, double full, double min, double min_pct) {
    double mid = (full + min) * 0.5;
    double mask = (conf >= mid) ? 1.0 : 0.0;  // cmov; branchless
    return fma(mask, 1.0 - min_pct, min_pct);
}

// Dispatch wrapper — bounds-checked. Caller passes any int curve value;
// out-of-range returns 1.0 (degrades safely to OFF behavior).
static inline double Confidence_DegradationScale(int curve, double conf,
                                                  double full, double min, double min_pct) {
    if (curve < 0 || curve >= FOREACH_DEGRADATION_CURVE_COUNT) return 1.0;
    return degradation_curve_fns[curve](conf, full, min, min_pct);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_REGISTRY]_[FOREACH_DEGRADATION_CURVE]
//======================================================================

//----------------------------------------------------------------------
// [SECTION]_[DRIFT HISTORY — v5.10.0e runtime IC monitoring]
//----------------------------------------------------------------------
// Time-series ring buffer of (IC, timestamp) pairs sampled at slow-path
// cadence (typically post-fill drain when ConfidenceScorer_Update fires).
// Sustained-breach detection: average IC over the last `window_us` is
// below `floor` AND we have at least 5 samples in that window. Engine
// emits CRITICAL log on first breach + optionally trips kill_switch.
//
// Capacity 256 covers a wide range of cadences. At 1 sample/sec that's
// ~4 minutes of history; at 1 sample/30s that's ~2 hours; the breach
// window is operator-tunable via cfg.confidence_ic_floor_window so
// fast-cadence operators get longer effective coverage.

#define DRIFT_HISTORY_CAPACITY 256

// v5.15.5.E.B — DriftHistory state flags bitmap. Replaces int breached +
// int kill_tripped (8 bytes; 2 booleans) with uint8_t drift_state_flags
// (1 byte; 2 bits used, 6 free for future flags). Per bitmap-flag-api.md.
// Single-thread access (per-core slow-path); no atomic
// variant needed.
constexpr uint8_t MASK_DRIFT_BREACHED     = BITMAP_BIT_U8(0);
constexpr uint8_t MASK_DRIFT_KILL_TRIPPED = BITMAP_BIT_U8(1);
// bits 2-7 reserved for future drift-state flags

//======================================================================
// [STRUCT]_[DriftSample]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the AoS (ic, ts) pair — one CheckBreach iteration touches ONE cache line instead of the pre-.E.B two parallel arrays 2048B apart]
// [REFERENCE]_[DESIGN_SPEC]_[latency-vs-cache-decision-framework]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-49]
//======================================================================
// [CODE]
//======================================================================
struct DriftSample {
    double   ic;   // IC value at this sample point (read by CheckBreach + snapshot aggregate)
    uint64_t ts;   // wall-clock timestamp at sample (read by CheckBreach window filter)
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.E.B — AoS sample interleave per latency-vs-cache-decision-framework.md
// + DESIGN_SPECS/aos-time-series-pattern (codification deferred to 2nd app per
// TECH_DEBT-049 trigger). Pre-.E.B layout was parallel arrays (ic_samples[256] +
// ts_us[256]) at 2048B offset apart → CheckBreach loop touched 2 cache lines
// per iteration. Post-.E.B: each iteration touches 1 cache line (DriftSample
// is 16B; samples[k] read pulls both .ic + .ts in one cache fill).
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[16B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[DriftSample]
//======================================================================
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(DriftSample) == 16]
static_assert(sizeof(DriftSample) == 16,
    "DriftSample MUST be 16 B (2 × 8B; AoS cache locality for CheckBreach).");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(ic) == 0]
static_assert(offsetof(DriftSample, ic) == 0, "DriftSample.ic at offset 0");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(ts) == 8]
static_assert(offsetof(DriftSample, ts) == 8, "DriftSample.ts at offset 8");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(DriftSample) == 8]
static_assert(alignof(DriftSample) == 8, "DriftSample 8B aligned");

//======================================================================
// [STRUCT]_[DriftHistory]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DATA_ORIENTED_DESIGN] [BITMAP_PACKED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[IC drift ring — HOT count/head/flags at line 0, COLD 4096B AoS sample ring at offset 16; bit-packed state flags (MASK_DRIFT_*)]
// [REFERENCE]_[DESIGN_SPEC]_[cache-layout-discipline-for-hot-side-structs]
// [REFERENCE]_[DESIGN_SPEC]_[[bitmap-flag-api] [cache-layout-discipline-for-hot-side-structs.md] [latency-vs-cache-decision-framework.md]]
//======================================================================
// [CODE]
//======================================================================
struct alignas(64) DriftHistory {
    // HOT cluster (offset 0)
    int     count;              // monotonic insert count (saturates at CAPACITY)
    int     head;               // next write index modulo CAPACITY
    uint8_t drift_state_flags;  // MASK_DRIFT_* bits; replaces int breached + int kill_tripped
    // 7B implicit pad → samples at offset 16 (DriftSample needs 8B alignment;
    // memset(0) at Init clears pad bytes; DriftHistory is NOT in byte-
    // equivalence context per audit so explicit pad fields not required)
    // COLD cluster (offset 16; 4096B AoS ring buffer)
    DriftSample samples[DRIFT_HISTORY_CAPACITY];
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.E.B — DriftHistory full discipline: alignas(64) + HOT-first reorg +
// AoS interleave + bit-packed flags + display-only field extracted. Pattern:
// cache-layout-discipline-for-hot-side-structs.md Rules 1 + 4 + 5 + bitmap-
// flag-api.md + latency-vs-cache-decision-framework.md.
//
// v5.15.5.E.B — breach_first_us EXTRACTED to existing NodeContextDisplayMeta
// sibling via FOREACH_DISPLAY_META_FIELD registry row (drift_breach_first_us).
// Per cache-layout-discipline-for-hot-side-structs.md Rule 1 (display-only
// field extraction) + closes Class-18 mirror with other display-only fields
// on NodeContext (avoids creating yet another DisplayMeta sister struct).
// Audit verified 2026-05-13: breach_first_us is WRITE-ONLY in current code
// (set at first-breach detection; never read; not in snapshot). Preserved
// for future GUI consumption via DisplayMeta access.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[4160B]
// [ALIGN]_[64]
// [CACHE_LINES]_[65]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[DriftHistory]
//======================================================================
// v5.15.5.E.B — Layout lock for DriftHistory.
// 9 byte HOT + 7B pad + 4096B samples = 4112 natural; alignas(64) → 4160
// (+48B trailing pad; 65 cache lines exact).
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(DriftHistory) == 4160]
static_assert(sizeof(DriftHistory) == 4160,
    "DriftHistory sizeof MUST be 4160 B (65 cache lines with 48B trailing pad).");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(count) == 0]
static_assert(offsetof(DriftHistory, count) == 0,
    "DriftHistory HOT scalar `count` at offset 0.");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(drift_state_flags) == 8]
static_assert(offsetof(DriftHistory, drift_state_flags) == 8,
    "DriftHistory HOT bitmap `drift_state_flags` at offset 8.");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(samples) == 16]
static_assert(offsetof(DriftHistory, samples) == 16,
    "DriftHistory COLD `samples` at offset 16 (cache-aligned with 7B pad after flags).");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(DriftHistory) == 64]
static_assert(alignof(DriftHistory) == 64,
    "DriftHistory MUST be cache-line aligned.");

//======================================================================
// [FUNCTION]_[DriftHistory_CheckBreach]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[sustained-breach detector (Init + Push ride) — avg IC over the trailing window below floor with >= 5 samples; AoS ring walked backward from head]
//======================================================================
// [CODE]
//======================================================================
static inline void DriftHistory_Init(DriftHistory *dh) {
    memset(dh, 0, sizeof(*dh));
    // count = head = drift_state_flags = 0; samples[] = {0}; pad bytes cleared.
}

// v5.15.5.E.B — Write to AoS samples[idx].{ic, ts}. Each Push touches ONE
// cache line for the sample write (vs pre-.E.B which wrote to ic_samples[idx]
// + ts_us[idx], 2 separate cache lines 2048B apart).
static inline void DriftHistory_Push(DriftHistory *dh, double ic, uint64_t now_us) {
    int idx = dh->head % DRIFT_HISTORY_CAPACITY;
    dh->samples[idx].ic = ic;
    dh->samples[idx].ts = now_us;
    dh->head++;
    if (dh->count < DRIFT_HISTORY_CAPACITY) dh->count++;
}

// Returns 1 if sustained breach: average IC across samples whose timestamps
// fall within (now_us - window_us, now_us] is below `floor`, AND at least 5
// such samples exist (avoid noise-triggered false alarm). out_avg_ic /
// out_samples are optional diagnostic outputs.
//
// v5.15.5.E.B — AoS interleave: each loop iteration reads samples[idx].ic +
// samples[idx].ts from the SAME cache line (vs pre-.E.B which read from 2
// separate arrays 2048B apart). At cap=256 samples / cycle, savings of
// ~128-256 cache-line fills per call (cold cache) → ~12-25 µs/CheckBreach
// at typical 10-100Hz cadence.
static inline int DriftHistory_CheckBreach(const DriftHistory *dh, uint64_t now_us,
                                            uint64_t window_us, double floor,
                                            double *out_avg_ic, int *out_samples) {
    if (out_avg_ic) *out_avg_ic = 0.0;
    if (out_samples) *out_samples = 0;
    if (dh->count < 5) return 0;

    // Walk backward from head until we run out of samples or fall outside the window
    double sum = 0.0;
    int    n   = 0;
    int    cap = (dh->count < DRIFT_HISTORY_CAPACITY) ? dh->count : DRIFT_HISTORY_CAPACITY;
    uint64_t cutoff = (now_us > window_us) ? (now_us - window_us) : 0ULL;
    for (int i = 0; i < cap; i++) {
        int idx = (dh->head - 1 - i + DRIFT_HISTORY_CAPACITY) % DRIFT_HISTORY_CAPACITY;
        if (dh->samples[idx].ts <= cutoff) break;  // outside window — 1 line touch
        sum += dh->samples[idx].ic;                 // SAME line as .ts (AoS)
        n++;
    }
    if (n < 5) return 0;
    double avg = sum / (double)n;
    if (out_avg_ic)  *out_avg_ic  = avg;
    if (out_samples) *out_samples = n;
    return (avg < floor) ? 1 : 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[DriftHistory_CheckBreach]
//======================================================================

//======================================================================
// [REGISTRY]_[FOREACH_CONFIDENCE_PERSIST_FIELD]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ConfidenceScorer wire-format spec — one row per persisted field; fwrite + fread + commit bodies auto-generate; RecomputeRunningSums restores the derived aggregate post-load]
// [COLUMN]_[member_path]_[ConfidenceScorer member access path — the macro expands inside cs->{name}]
// [COLUMN]_[type]_[element C type for sizeof in the fwrite/fread/memcpy expansion]
// [COLUMN]_[count]_[element count — 1 for scalar, N for array]
// [REFERENCE]_[DESIGN_SPEC]_[registry-tuple-as-single-source-of-truth]
// [REFERENCE]_[DESIGN_SPEC]_[[autopopulate-pattern-for-production-caller-class] [postloadsetup-registry-pattern.md] [registry-tuple-as-single-source-of-truth.md] [sliding-window-online-statistics-pattern.md] [structural-fix-preferred-decision-framework.md]]
// [REFERENCE]_[INVARIANT]_[H9]
// [REFERENCE]_[CLASS]_[18]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_CONFIDENCE_PERSIST_FIELD(X)                      \
    X(ic.predictions.samples,    double, ROLLING_IC_MAX_WINDOW)  \
    X(ic.actuals.samples,        double, ROLLING_IC_MAX_WINDOW)  \
    X(ic.predictions.count,      int,    1)                      \
    X(ic.predictions.head,       int,    1)                      \
    X(rmse.window.samples,       double, ROLLING_IC_MAX_WINDOW)  \
    X(rmse.window.count,         int,    1)                      \
    X(rmse.window.head,          int,    1)

// AUTOPOPULATE fwrite — field-by-field write. Returns -1 on any fwrite failure.
// Wire byte sequence: exactly the same as pre-.E sharded path (lines 247-256
// of ShardedSnapshotPersist.hpp). PortfolioController.hpp gains this format
// via the .E.0 version bump (CONTROLLER_SNAPSHOT_VERSION 11 → 12).
#define CONFIDENCE_FWRITE_FIELD_(name, type, n)             \
    if (fwrite(&cs->name, sizeof(type), (size_t)(n), f) != (size_t)(n)) return -1;

static inline int ConfidenceScorer_FieldwiseWrite(const ConfidenceScorer* cs, FILE* f) {
    FOREACH_CONFIDENCE_PERSIST_FIELD(CONFIDENCE_FWRITE_FIELD_)
    return 0;
}

// AUTOPOPULATE fread — field-by-field read. Same wire format as FieldwiseWrite.
#define CONFIDENCE_FREAD_FIELD_(name, type, n)              \
    if (fread(&cs->name, sizeof(type), (size_t)(n), f) != (size_t)(n)) return -1;

static inline int ConfidenceScorer_FieldwiseRead(ConfidenceScorer* cs, FILE* f) {
    FOREACH_CONFIDENCE_PERSIST_FIELD(CONFIDENCE_FREAD_FIELD_)
    return 0;
}

// AUTOPOPULATE commit — copy persisted-subset from src to dst. Used by
// ShardedSnapshotPersist load (commit-after-read-validation pattern preserves
// atomicity; staging instance receives fread; commit copies to runtime only
// after all reads succeed).
#define CONFIDENCE_COMMIT_FIELD_(name, type, n)             \
    memcpy(&dst->name, &src->name, sizeof(type) * (size_t)(n));

static inline void ConfidenceScorer_CommitPersistedFields(ConfidenceScorer* dst,
                                                            const ConfidenceScorer* src) {
    FOREACH_CONFIDENCE_PERSIST_FIELD(CONFIDENCE_COMMIT_FIELD_)
}

#undef CONFIDENCE_FWRITE_FIELD_
#undef CONFIDENCE_FREAD_FIELD_
#undef CONFIDENCE_COMMIT_FIELD_

// Count-lock (E.1.2 D-305 — the missing sibling of the regime ==7 / feeder ==3
// pins; primary forcing function per D-302 Option B): EXACTLY 7 persisted fields.
// A row add/drop trips this at compile time → forces a SHARDED_SNAPSHOT_VERSION
// bump (H21). The parent-level lock is FOREACH_NODE_PERSIST_FIELD_COUNT == 29
// (MemHeaders/NodeCtxPersistRegistry.hpp) — this local tripwire catches a
// delegate-INTERNAL drop the parent row count cannot see.
#define CONFIDENCE_PERSIST_COUNT_ONE_(name, type, n) +1
constexpr int FOREACH_CONFIDENCE_PERSIST_FIELD_COUNT =
    0 FOREACH_CONFIDENCE_PERSIST_FIELD(CONFIDENCE_PERSIST_COUNT_ONE_);
#undef CONFIDENCE_PERSIST_COUNT_ONE_
static_assert(FOREACH_CONFIDENCE_PERSIST_FIELD_COUNT == 7,
    "ConfidenceScorer wire format = EXACTLY 7 persisted fields (ic predictions/actuals "
    "sample arrays + count + head, rmse window samples + count + head; 1552B/node); a "
    "change requires a SHARDED_SNAPSHOT_VERSION bump + loader migration (H21).");

// v5.15.5.E.D — RollingRMSE.sum_squared_errors is intentionally NOT in
// FOREACH_CONFIDENCE_PERSIST_FIELD: adding it would grow the wire format +
// require a SHARDED_SNAPSHOT_VERSION bump for a derivable field. Instead,
// recompute from samples[] after load. Cost: O(N=32) once per load — cheap.
// Idempotent + safe to call on a freshly-loaded ConfidenceScorer.
//
// Pattern: postloadsetup-registry-pattern.md (composing with FOREACH-based
// load) + sliding-window-online-statistics-pattern.md (the running-aggregate
// invariant restored post-load).
//
// Call site contract: after any load path that writes rmse.window.samples
// (PortfolioController_LoadSnapshot, ShardedSnapshot_Load post-commit,
// ConfidenceScorer_ShadowLoadLegacyV1). Built into ConfidenceScorer_
// CommitPersistedFields tail; PortfolioController calls it explicitly.
static inline void ConfidenceScorer_RecomputeRunningSums(ConfidenceScorer* cs) {
    double sum = 0.0;
    for (int i = 0; i < cs->rmse.window.count; i++) {
        sum += cs->rmse.window.samples[i];
    }
    cs->rmse.sum_squared_errors = sum;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.E.0 — STRUCTURAL UNBLOCK: FOREACH_CONFIDENCE_PERSIST_FIELD + AUTOPOPULATE + shadow-load.
// Closes Class-18 mirror between PortfolioController.hpp:2117 (raw fwrite)
// and ShardedSnapshotPersist.hpp:247-256 (field-by-field) per
// structural-fix-preferred-decision-framework.md.
//
// FOREACH registry IS the wire format spec. Both persistence sites use the
// same fieldwise helpers — adding a new persisted field = ONE row in the
// registry; both fwrite + fread bodies + commit path auto-generated via
// macro expansion (autopopulate-pattern-for-production-caller-class.md).
//
// Subset matches sharded path's pre-.E behavior: only ic + rmse internals
// persist. Composite-mode fields (freshness/capacity/rmse_baseline) are
// EXCLUDED — wall-clock + EWMA state stale on reload; re-init from cfg
// at boot is the correct behavior (legacy raw-fwrite was over-persisting).
// last_confidence / freshness_tau / ic.window / rmse.window similarly omitted
// (re-compute on next Compute / re-init from cfg / template constant).
//
// FIELD signature: X(member_path, type, count) where count=1 for scalar,
// N for array. The macro expands inside `cs->{name}` access.
//
// Pattern: DESIGN_SPECS/registry-tuple-as-single-source-of-truth.md +
// DESIGN_SPECS/autopopulate-pattern-for-production-caller-class.md.
//
// v5.15.5.E.C — Field paths updated for RollingWindow<T,N> composition:
//   ic.predictions → ic.predictions.samples (the array within RollingWindow)
//   ic.actuals     → ic.actuals.samples
//   ic.count       → ic.predictions.count (predictions + actuals stay in lockstep
//                    via RollingIC_Push; canonically read from predictions)
//   ic.head        → ic.predictions.head
//   rmse.squared_errors → rmse.window.samples
//   rmse.count     → rmse.window.count
//   rmse.head      → rmse.window.head
//
// Wire bytes IDENTICAL to pre-.E.C: data values + byte offsets within
// `ic` / `rmse` unchanged. predictions.samples starts at offset 16 of
// RollingWindow (matches predictions array offset 16 of pre-.E.C RollingIC).
//======================================================================
// [END_REGISTRY]_[FOREACH_CONFIDENCE_PERSIST_FIELD]
//======================================================================

//----------------------------------------------------------------------
// [SECTION]_[v5.15.5.E.0 — LEGACY V1 WIRE STRUCT + SHADOW-LOAD MIGRATION]
//----------------------------------------------------------------------
// ConfidenceScorerLegacyV1 + sub-structs: FROZEN byte-format matching pre-.E
// PortfolioController raw fwrite (CONTROLLER_SNAPSHOT_VERSION=11). Used ONLY
// by ConfidenceScorer_ShadowLoadLegacyV1 during one-shot migration of v11
// snapshots → v12 runtime layout. Operator data preserved across the wire-
// format break (no re-warm required).
//
// THESE STRUCTS ARE NEVER WRITTEN. Pure read-side wire-format target. After
// TECH_DEBT-002 closes (legacy PortfolioController removal), they can be
// deleted entirely + the shadow-load helper goes with them.
//
// Layout cloned from pre-.E.A ConfidenceScorer + sub-structs. Field order +
// types match exactly. Compiler-generated padding matches because the struct
// definitions are identical (same alignof, same field types).
//
// Pattern: DESIGN_SPECS/shadow-load-state-transition-pattern.md +
// DESIGN_SPECS/wire-format-byte-preservation-discipline.md +
// DESIGN_SPECS/struct-padding-determinism-pattern.md.

//======================================================================
// [STRUCT]_[RollingIC_LegacyV1]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DEPRECATED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[FROZEN v11 wire-read target — the pre-.E.A RollingIC layout (predictions/actuals rings + count/head/window); read-only, deletable when TECH_DEBT-002 closes]
//======================================================================
// [CODE]
//======================================================================
struct RollingIC_LegacyV1 {
    double predictions[ROLLING_IC_MAX_WINDOW];
    double actuals[ROLLING_IC_MAX_WINDOW];
    int count;
    int head;
    int window;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[1040B]
// [ALIGN]_[8]
// [CACHE_LINES]_[17]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[RollingIC_LegacyV1]
//======================================================================

//======================================================================
// [STRUCT]_[RollingRMSE_LegacyV1]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DEPRECATED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[FROZEN v11 wire-read target — the pre-.E.A RollingRMSE layout (squared-error ring + count/head/window); read-only, deletable when TECH_DEBT-002 closes]
//======================================================================
// [CODE]
//======================================================================
struct RollingRMSE_LegacyV1 {
    double squared_errors[ROLLING_IC_MAX_WINDOW];
    int count;
    int head;
    int window;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[528B]
// [ALIGN]_[8]
// [CACHE_LINES]_[9]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[RollingRMSE_LegacyV1]
//======================================================================

//======================================================================
// [STRUCT]_[RollingFreshness_LegacyV1]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DEPRECATED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[FROZEN v11 wire-read target — the pre-.E.A RollingFreshness layout (last-predict-us + tau-secs); read-only, deletable when TECH_DEBT-002 closes]
//======================================================================
// [CODE]
//======================================================================
struct RollingFreshness_LegacyV1 {
    uint64_t last_predict_us;
    double   tau_secs;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[16B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[RollingFreshness_LegacyV1]
//======================================================================

//======================================================================
// [STRUCT]_[RollingCapacity_LegacyV1]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DEPRECATED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[FROZEN v11 wire-read target — the pre-.E.A RollingCapacity layout (current-adv / target-dollars / kappa); read-only, deletable when TECH_DEBT-002 closes]
//======================================================================
// [CODE]
//======================================================================
struct RollingCapacity_LegacyV1 {
    double current_adv;
    double target_dollars;
    double kappa;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[24B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[RollingCapacity_LegacyV1]
//======================================================================

//======================================================================
// [STRUCT]_[ConfidenceScorerLegacyV1]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DEPRECATED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[FROZEN v11 wire-read target — the composite pre-.E.A ConfidenceScorer (ic/rmse/freshness/capacity sub-structs + tau/confidence/baseline); the shadow-load reader's fread target; read-only, deletable when TECH_DEBT-002 closes]
//======================================================================
// [CODE]
//======================================================================
struct ConfidenceScorerLegacyV1 {
    RollingIC_LegacyV1   ic;
    RollingRMSE_LegacyV1 rmse;
    double               freshness_tau;
    double               last_confidence;
    RollingFreshness_LegacyV1 freshness;
    RollingCapacity_LegacyV1  capacity;
    double               rmse_baseline;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[1632B]
// [ALIGN]_[8]
// [CACHE_LINES]_[26]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[ConfidenceScorerLegacyV1]
//======================================================================

//======================================================================
// [FUNCTION]_[ConfidenceScorer_ShadowLoadLegacyV1]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one-shot v11 -> v12 migration read — raw LegacyV1 bytes into the runtime layout; stale wall-clock/EWMA/cfg-owned fields INTENTIONALLY DROPPED (re-init from cfg)]
// [REFERENCE]_[DESIGN_SPEC]_[shadow-load-state-transition-pattern]
//======================================================================
// [CODE]
//======================================================================
static inline int ConfidenceScorer_ShadowLoadLegacyV1(ConfidenceScorer* cs, FILE* f) {
    ConfidenceScorerLegacyV1 wire;
    if (fread(&wire, sizeof(wire), 1, f) != 1) return -1;

    // Re-init runtime struct to defaults (clean slate; preserves window
    // semantics from current cfg). Window restored from legacy if it's
    // a sensible value; else default.
    int restore_window = wire.ic.window;
    double restore_tau = wire.freshness_tau;
    ConfidenceScorer_Init(cs, restore_window, restore_tau);

    // Copy persisted IC + RMSE history from wire to runtime. v5.15.5.E.C
    // updated for RollingWindow composition (paths gained .samples / .count
    // / .head intermediate via the embedded RollingWindow). Wire format
    // bytes unchanged (frozen LegacyV1 still uses flat arrays + scalars).
    memcpy(cs->ic.predictions.samples, wire.ic.predictions,
           sizeof(wire.ic.predictions));
    memcpy(cs->ic.actuals.samples,     wire.ic.actuals,
           sizeof(wire.ic.actuals));
    cs->ic.predictions.count = wire.ic.count;
    cs->ic.predictions.head  = wire.ic.head;
    cs->ic.actuals.count     = wire.ic.count;   // keep parallel ring in sync
    cs->ic.actuals.head      = wire.ic.head;
    memcpy(cs->rmse.window.samples, wire.rmse.squared_errors,
           sizeof(wire.rmse.squared_errors));
    cs->rmse.window.count = wire.rmse.count;
    cs->rmse.window.head  = wire.rmse.head;

    // Restore last_confidence cache (next Compute will overwrite anyway).
    cs->last_confidence = wire.last_confidence;
    // INTENTIONALLY DROPPED: wire.freshness.last_predict_us (wall-clock; stale)
    // INTENTIONALLY DROPPED: wire.freshness.tau_secs (re-init from cfg)
    // INTENTIONALLY DROPPED: wire.capacity.* (EWMA state stale; re-warm)
    // INTENTIONALLY DROPPED: wire.rmse_baseline (re-init from cfg)
    // INTENTIONALLY DROPPED: wire.ic.window / rmse.window (template constant)

    return 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Shadow-load: read v11 raw-fwrite bytes, populate runtime ConfidenceScorer
// via field-by-field copy. Composite-mode fields RE-INIT from cfg via
// ConfidenceScorer_Init (wall-clock + EWMA stale on reload; re-warm correct).
// Returns 0 on success; -1 on fread failure.
//======================================================================
// [END_FUNCTION]_[ConfidenceScorer_ShadowLoadLegacyV1]
//======================================================================

#endif // CONFIDENCE_SCORE_HPP
