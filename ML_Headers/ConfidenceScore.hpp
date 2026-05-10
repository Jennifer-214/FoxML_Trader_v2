// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [CONFIDENCE SCORE — PREDICTION QUALITY WEIGHTING]
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
// single-symbol — we follow suit.
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
//   capacity factor: kappa * ADV / order_size
//     → see ~/FoxML/private/LIVE_TRADING/prediction/confidence.py:calculate_capacity()
//======================================================================================================
#ifndef CONFIDENCE_SCORE_HPP
#define CONFIDENCE_SCORE_HPP

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>  // v5.14.9 — atoi for DegradationCurve_FromString numeric parse

#include <math.h>
#include <string.h>
#include <strings.h>  // v5.14.9 — strcasecmp for DegradationCurve_FromString
#include "ICVariantRegistry.hpp"  // v5.14.1.F — FOREACH_IC_VARIANT X-macro

// default parameters (from FoxML constants.py + confidence.py)
#define CONFIDENCE_FRESHNESS_TAU_DEFAULT  300.0   // seconds (5 min decay)
#define CONFIDENCE_MIN_IC_DEFAULT         0.01    // floor for IC
#define CONFIDENCE_IC_WINDOW_DEFAULT      32      // rolling window size
#define CONFIDENCE_MIN_SAMPLES            5       // minimum for Spearman calc

//======================================================================================================
// [ROLLING IC — Spearman rank correlation]
//======================================================================================================
// Spearman = Pearson correlation of ranks.
// simpler than scipy.stats.spearmanr, but same result for small windows.
//======================================================================================================

#define ROLLING_IC_MAX_WINDOW 64

struct RollingIC {
    double predictions[ROLLING_IC_MAX_WINDOW];
    double actuals[ROLLING_IC_MAX_WINDOW];
    int count;          // total items inserted (may exceed window)
    int head;           // ring buffer head
    int window;         // max window size
};

static inline void RollingIC_Init(RollingIC *ric, int window) {
    memset(ric, 0, sizeof(*ric));
    if (window < 2) window = 2;
    if (window > ROLLING_IC_MAX_WINDOW) window = ROLLING_IC_MAX_WINDOW;
    ric->window = window;
}

static inline void RollingIC_Push(RollingIC *ric, double prediction, double actual) {
    int idx = ric->head % ric->window;
    ric->predictions[idx] = prediction;
    ric->actuals[idx] = actual;
    ric->head++;
    if (ric->count < ric->window) ric->count++;
}

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
static inline double RollingIC_Compute(const RollingIC *ric) {
    if (ric->count < CONFIDENCE_MIN_SAMPLES) return 0.0;

    int n = ric->count;
    double preds[ROLLING_IC_MAX_WINDOW], acts[ROLLING_IC_MAX_WINDOW];
    double pred_ranks[ROLLING_IC_MAX_WINDOW], act_ranks[ROLLING_IC_MAX_WINDOW];

    // copy ring buffer to contiguous arrays
    for (int i = 0; i < n; i++) {
        int idx = (ric->head - n + i);
        if (idx < 0) idx += ric->window;
        else idx = idx % ric->window;
        preds[i] = ric->predictions[idx];
        acts[i] = ric->actuals[idx];
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

//======================================================================================================
// [ROLLING RMSE — prediction calibration stability]
//======================================================================================================
struct RollingRMSE {
    double squared_errors[ROLLING_IC_MAX_WINDOW];
    int count;
    int head;
    int window;
};

static inline void RollingRMSE_Init(RollingRMSE *r, int window) {
    memset(r, 0, sizeof(*r));
    if (window < 2) window = 2;
    if (window > ROLLING_IC_MAX_WINDOW) window = ROLLING_IC_MAX_WINDOW;
    r->window = window;
}

static inline void RollingRMSE_Push(RollingRMSE *r, double prediction, double actual) {
    double err = prediction - actual;
    int idx = r->head % r->window;
    r->squared_errors[idx] = err * err;
    r->head++;
    if (r->count < r->window) r->count++;
}

static inline double RollingRMSE_Compute(const RollingRMSE *r) {
    if (r->count < 2) return 1.0; // high RMSE = low confidence until enough data
    double sum = 0.0;
    for (int i = 0; i < r->count; i++)
        sum += r->squared_errors[i];
    return sqrt(sum / r->count);
}

//======================================================================================================
// [CONFIDENCE COMPUTATION]
//======================================================================================================
// confidence = IC * freshness * stability
//   IC:        abs(Spearman rank correlation), floored at MIN_IC
//   freshness: e^(-data_age_sec / tau)
//   stability: 1 / (1 + RMSE)
//======================================================================================================

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

//======================================================================================================
// [v5.14.1.A — COMPOSITE CONFIDENCE COMPONENTS]
//======================================================================================================
// Composite formula: IC × Freshness × Capacity × Stability_normalized
// where Stability_normalized = 1 - clamp(rmse / rmse_baseline, 0, 1).
//
// Each component is independently observable + cfg-tunable, replacing the
// older 3-factor (IC * Freshness * 1/(1+RMSE)) with a 4-factor formulation
// that adds a Capacity term + normalizes Stability against a baseline RMSE
// pulled from training. Enables soft risk degradation (v5.14.9) by giving
// the sizing path a continuous [0, 1] confidence scalar instead of a
// binary kill-switch trip.
//======================================================================================================

// Default kappa for capacity calc (proportionality constant on ADV).
#define CONFIDENCE_CAPACITY_KAPPA_DEFAULT  0.1
// Default ADV smoothing alpha (10-sample EWMA).
#define CONFIDENCE_CAPACITY_ALPHA_DEFAULT  0.1

// Wall-clock-driven freshness with cfg-tunable tau. Replaces the
// data_age_sec arg of the original Confidence_Freshness so the scorer
// owns its own clock state — operator + tests can manipulate via Mark.
struct RollingFreshness {
    uint64_t last_predict_us;   // wall-clock at last prediction (Mark)
    double   tau_secs;          // exponential decay time constant
};

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

// Capacity factor: how much of the desired position size the market can
// absorb without slippage degradation. target_dollars=0 = unbounded
// (single-symbol small-account default; capacity always 1.0).
struct RollingCapacity {
    double current_adv;       // EWMA-smoothed average daily volume estimate
    double target_dollars;    // cfg-tunable position-size target; 0 = unbounded
    double kappa;             // proportionality constant
};

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

//======================================================================================================
// [FULL CONFIDENCE SCORER — combines IC + RMSE buffers]
//======================================================================================================
// v5.14.1.A — added freshness + capacity + rmse_baseline for composite formula.
// Pre-v5.14.1 fields (ic, rmse, freshness_tau, last_confidence) preserved;
// existing ConfidenceScorer_Compute path bytewise unchanged when caller
// stays on the IC-only API. Composite opt-in via cfg flag (v5.14.1.B).
//
// IC SEMANTICS NOTE (v5.14.1.F doc-fix): the `ic` field's struct type
// `RollingIC` is generically named but its `RollingIC_Compute` body at
// `ML_Headers/ConfidenceScore.hpp:96+` ranks both predictions+actuals then
// computes Pearson correlation of the ranks = textbook **Spearman rank
// correlation**. Despite the generic struct name, this has been Spearman
// since v5.x.x. cfg.confidence_ic_variant=0 (default) selects this Spearman
// implementation via the FOREACH_IC_VARIANT registry; future variants
// (Pearson, Kendall, etc.) slot in without disturbing this field's wiring.
struct ConfidenceScorer {
    RollingIC ic;
    RollingRMSE rmse;
    double freshness_tau;
    double last_confidence;
    RollingFreshness freshness;   // v5.14.1.A
    RollingCapacity capacity;     // v5.14.1.A
    double rmse_baseline;         // v5.14.1.A — bound to training-time RMSE; default 1.0
};
// v5.14.1.F design note: NO struct field for active variant (would change
// sizeof(ConfidenceScorer) and break snapshot load at PortfolioController.hpp:2094+2210
// per Class 4 — snapshot save/load asymmetry). Variant is passed explicitly
// to ConfidenceScorer_ComputeICVariant; caller reads from cfg at call site.

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
// sizeof and breaking snapshot save/load at PortfolioController.hpp:2094+2210
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
// Caller pattern (3 boot sites: EngineSharded, ControllerEventLoop, PortfolioController):
//   ConfidenceScorer_Init(&cs, window, base_tau);
//   ConfidenceScorer_BindCompositeCfg(&cs,
//       cfg.confidence_composite_enabled,
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

//======================================================================================================
// [v5.14.1.A — COMPOSITE CONFIDENCE COMPUTE]
//======================================================================================================
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
//======================================================================================================
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

//======================================================================================================
// [v5.14.9 — SOFT RISK DEGRADATION LADDER]
//======================================================================================================
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
//======================================================================================================

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

//======================================================================================================
// [CURVE COMPUTE FNS — v5.14.9.A]
//======================================================================================================
// All branchless. fmin/fmax → cmov; fma → 1 cycle on modern x86.
// Defensive: if full <= min (operator misconfig), return min_pct unconditionally
// (avoids div-by-zero; operator gets predictable degraded behavior).
//======================================================================================================

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

//======================================================================================================
// [DRIFT HISTORY — v5.10.0e runtime IC monitoring]
//======================================================================================================
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
//======================================================================================================
#define DRIFT_HISTORY_CAPACITY 256

struct DriftHistory {
    double   ic_samples[DRIFT_HISTORY_CAPACITY];
    uint64_t ts_us[DRIFT_HISTORY_CAPACITY];
    int      count;             // monotonic insert count (saturates at CAPACITY)
    int      head;              // next write index modulo CAPACITY
    int      breached;          // 1 = sustained breach currently active
    uint64_t breach_first_us;   // wall-clock when breach was first detected
    int      kill_tripped;      // 1 = kill_switch was tripped due to drift
};

static inline void DriftHistory_Init(DriftHistory *dh) {
    memset(dh, 0, sizeof(*dh));
}

static inline void DriftHistory_Push(DriftHistory *dh, double ic, uint64_t now_us) {
    int idx = dh->head % DRIFT_HISTORY_CAPACITY;
    dh->ic_samples[idx] = ic;
    dh->ts_us[idx]      = now_us;
    dh->head++;
    if (dh->count < DRIFT_HISTORY_CAPACITY) dh->count++;
}

// Returns 1 if sustained breach: average IC across samples whose
// timestamps fall within (now_us - window_us, now_us] is below `floor`,
// AND at least 5 such samples exist (avoid noise-triggered false alarm).
// out_avg_ic / out_samples are optional diagnostic outputs.
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
        if (dh->ts_us[idx] <= cutoff) break;  // outside window
        sum += dh->ic_samples[idx];
        n++;
    }
    if (n < 5) return 0;
    double avg = sum / (double)n;
    if (out_avg_ic)  *out_avg_ic  = avg;
    if (out_samples) *out_samples = n;
    return (avg < floor) ? 1 : 0;
}

#endif // CONFIDENCE_SCORE_HPP
