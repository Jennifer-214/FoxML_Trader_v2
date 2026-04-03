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

#include <math.h>
#include <string.h>

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
// [FULL CONFIDENCE SCORER — combines IC + RMSE buffers]
//======================================================================================================
struct ConfidenceScorer {
    RollingIC ic;
    RollingRMSE rmse;
    double freshness_tau;
    double last_confidence;
};

static inline void ConfidenceScorer_Init(ConfidenceScorer *cs, int window, double tau) {
    RollingIC_Init(&cs->ic, (window > 0) ? window : CONFIDENCE_IC_WINDOW_DEFAULT);
    RollingRMSE_Init(&cs->rmse, (window > 0) ? window : CONFIDENCE_IC_WINDOW_DEFAULT);
    cs->freshness_tau = (tau > 0.0) ? tau : CONFIDENCE_FRESHNESS_TAU_DEFAULT;
    cs->last_confidence = 0.0;
}

// feed a prediction + actual return pair (call after outcome is known)
static inline void ConfidenceScorer_Update(ConfidenceScorer *cs,
                                             double prediction, double actual) {
    RollingIC_Push(&cs->ic, prediction, actual);
    RollingRMSE_Push(&cs->rmse, prediction, actual);
}

// compute current confidence given data age
static inline double ConfidenceScorer_Compute(ConfidenceScorer *cs, double data_age_sec) {
    double ic = RollingIC_Compute(&cs->ic);
    double rmse = RollingRMSE_Compute(&cs->rmse);
    cs->last_confidence = Confidence_Compute(ic, data_age_sec, rmse, cs->freshness_tau);
    return cs->last_confidence;
}

#endif // CONFIDENCE_SCORE_HPP
