// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the MIT License. See LICENSE file for details.

//======================================================================================================
// [ML MODEL INFERENCE]
//======================================================================================================
// thin C-style abstraction over XGBoost and LightGBM C APIs for single-row inference.
// compiles to complete no-ops when neither backend is enabled (zero overhead).
// both APIs take float* row vectors — single-row inference is ~1-5μs for typical tree models.
//
// usage:
//   ModelHandle<F> model;
//   Model_Init(&model);
//   Model_Load(&model, "model.xgb", MODEL_BACKEND_XGBOOST);
//   float features[16]; int n = ModelFeatures_Pack(features, &signals, &rolling, rolling_long);
//   float prediction = Model_Predict(&model, features, n);
//   Model_Free(&model);
//======================================================================================================
#ifndef MODEL_INFERENCE_HPP
#define MODEL_INFERENCE_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include <stdio.h>
#include <string.h>

// backend IDs
#define MODEL_BACKEND_NONE     0
#define MODEL_BACKEND_XGBOOST  1
#define MODEL_BACKEND_LIGHTGBM 2

// feature indices — must match training pipeline exactly
// changing order here requires retraining models
#define FEAT_SHORT_SLOPE     0
#define FEAT_SHORT_R2        1
#define FEAT_SHORT_VARIANCE  2
#define FEAT_LONG_SLOPE      3
#define FEAT_LONG_R2         4
#define FEAT_LONG_VARIANCE   5
#define FEAT_VOL_RATIO       6
#define FEAT_ROR_SLOPE       7
#define FEAT_VOLUME_SLOPE    8
#define FEAT_VOLUME_DELTA    9
#define FEAT_EMA_SMA_SPREAD  10
#define FEAT_VWAP_DEV        11
#define FEAT_PRICE_STDDEV    12
#define FEAT_PRICE_AVG       13
#define FEAT_VOLUME_AVG      14
#define FEAT_EMA_ABOVE_SMA   15
#define MODEL_NUM_FEATURES   16

// max features buffer (room for future expansion)
#define MODEL_MAX_FEATURES   32

// model format version — increment when FEAT_* indices or count changes.
// embedded in trained models, checked at load time. old models with wrong
// version fail loudly instead of producing silent garbage predictions.
// FEAT_* constants are APPEND-ONLY — never reorder, never remove.
#define MODEL_FORMAT_VERSION 1

//======================================================================================================
// [FEATURE LOOKBACK REGISTRY]
//======================================================================================================
// per-feature metadata: how many ticks back each feature reads.
// used by:
//   - ValidationSplit (purge gap = max lookback across features + buffer)
//   - PortfolioController (warmup validation: warmup_ticks >= max lookback)
//
// when adding a new FEAT_* constant, add a matching entry here with its lookback.
// this is the single source of truth for feature temporal reach.
//
// FUTURE HOOKS:
//   multi-symbol: add symbol_id field when trading multiple pairs
//   feature growth: use 'enabled' field to toggle features without recompiling
//   feature selection: filter by enabled==1 before packing
//   stability tracking: save XGBoost importances per fold, compare across runs
//     → see ~/FoxML/private/TRAINING/stability/feature_importance/analysis.py
//     → thresholds: min_top_k_overlap=0.7, min_kendall_tau=0.6 (safety.yaml:157)
//======================================================================================================
struct FeatureLookback {
    int feat_idx;           // FEAT_* constant
    const char *name;       // human-readable name (for display/debugging)
    int lookback_ticks;     // how many ticks back this feature reads (from RollingStats window)
    int enabled;            // 1 = active, 0 = disabled (future: feature toggling)
};

// default lookbacks for current features (from RollingStats 128-tick + 512-tick windows)
// table is append-only — matches FEAT_* ordering for direct indexing
static const FeatureLookback FEATURE_LOOKBACKS[] = {
    { FEAT_SHORT_SLOPE,    "short_slope",    128, 1 },  // 128-tick rolling window
    { FEAT_SHORT_R2,       "short_r2",       128, 1 },
    { FEAT_SHORT_VARIANCE, "short_variance", 128, 1 },
    { FEAT_LONG_SLOPE,     "long_slope",     512, 1 },  // 512-tick rolling window
    { FEAT_LONG_R2,        "long_r2",        512, 1 },
    { FEAT_LONG_VARIANCE,  "long_variance",  512, 1 },
    { FEAT_VOL_RATIO,      "vol_ratio",      512, 1 },  // uses both windows
    { FEAT_ROR_SLOPE,      "ror_slope",      512, 1 },  // ROR regressor lookback
    { FEAT_VOLUME_SLOPE,   "volume_slope",   128, 1 },
    { FEAT_VOLUME_DELTA,   "volume_delta",   128, 1 },
    { FEAT_EMA_SMA_SPREAD, "ema_sma_spread", 512, 1 },  // EMA + SMA comparison
    { FEAT_VWAP_DEV,       "vwap_dev",       128, 1 },
    { FEAT_PRICE_STDDEV,   "price_stddev",   128, 1 },
    { FEAT_PRICE_AVG,      "price_avg",      128, 1 },
    { FEAT_VOLUME_AVG,     "volume_avg",     128, 1 },
    { FEAT_EMA_ABOVE_SMA,  "ema_above_sma",  512, 1 },
};

static const int FEATURE_LOOKBACK_COUNT = sizeof(FEATURE_LOOKBACKS) / sizeof(FEATURE_LOOKBACKS[0]);

// compute max lookback across all enabled features
// used by: ValidationSplit (purge gap), PortfolioController (warmup check)
static inline int FeatureLookback_Max(void) {
    int max_lb = 0;
    for (int i = 0; i < FEATURE_LOOKBACK_COUNT; i++) {
        if (FEATURE_LOOKBACKS[i].enabled && FEATURE_LOOKBACKS[i].lookback_ticks > max_lb)
            max_lb = FEATURE_LOOKBACKS[i].lookback_ticks;
    }
    return max_lb;
}

// count enabled features (for validation)
static inline int FeatureLookback_CountEnabled(void) {
    int count = 0;
    for (int i = 0; i < FEATURE_LOOKBACK_COUNT; i++) {
        if (FEATURE_LOOKBACKS[i].enabled) count++;
    }
    return count;
}

//======================================================================================================
// conditional includes — only pull in headers when backend is enabled
//======================================================================================================
#ifdef USE_XGBOOST
#include <xgboost/c_api.h>
#endif

#ifdef USE_LIGHTGBM
#include <LightGBM/c_api.h>
#endif

//======================================================================================================
// [MODEL HANDLE]
//======================================================================================================
template <unsigned F>
struct ModelHandle {
    void *handle;           // opaque: BoosterHandle (XGB) or BoosterHandle (LGBM)
    int backend;            // MODEL_BACKEND_NONE / XGBOOST / LIGHTGBM
    int num_features;       // expected input dimension
    char model_path[256];   // path for display/logging
};

//======================================================================================================
template <unsigned F>
inline void Model_Init(ModelHandle<F> *m) {
    m->handle = NULL;
    m->backend = MODEL_BACKEND_NONE;
    m->num_features = 0;
    m->model_path[0] = '\0';
}

//======================================================================================================
// [LOAD]
//======================================================================================================
template <unsigned F>
inline int Model_Load(ModelHandle<F> *m, const char *path, int backend) {
    Model_Init(m);

    if (!path || path[0] == '\0') return 0; // no path = disabled

    // stash path for logging
    strncpy(m->model_path, path, sizeof(m->model_path) - 1);
    m->model_path[sizeof(m->model_path) - 1] = '\0';

#ifdef USE_XGBOOST
    if (backend == MODEL_BACKEND_XGBOOST) {
        BoosterHandle booster;
        int ret = XGBoosterCreate(NULL, 0, &booster);
        if (ret != 0) {
            fprintf(stderr, "[ML] XGBoost: failed to create booster: %s\n", XGBGetLastError());
            return 0;
        }
        ret = XGBoosterLoadModel(booster, path);
        if (ret != 0) {
            fprintf(stderr, "[ML] XGBoost: failed to load %s: %s\n", path, XGBGetLastError());
            XGBoosterFree(booster);
            return 0;
        }
        // set single-threaded for deterministic latency
        XGBoosterSetParam(booster, "nthread", "1");
        // version check — reject models trained with a different feature set
        const char *ver = NULL;
        int got_ver = XGBoosterGetAttr(booster, "foxml_version", &ver, (int[]){0});
        if (got_ver == 0 && ver) {
            int model_ver = atoi(ver);
            if (model_ver != MODEL_FORMAT_VERSION) {
                fprintf(stderr, "[ML] XGBoost: model %s was trained with format v%d, engine expects v%d — retrain required\n",
                        path, model_ver, MODEL_FORMAT_VERSION);
                XGBoosterFree(booster);
                return 0;
            }
        }
        m->handle = (void*)booster;
        m->backend = MODEL_BACKEND_XGBOOST;
        m->num_features = MODEL_NUM_FEATURES;
        fprintf(stderr, "[ML] XGBoost model loaded: %s (%d features, format v%d)\n",
                path, m->num_features, MODEL_FORMAT_VERSION);
        return 1;
    }
#endif

#ifdef USE_LIGHTGBM
    if (backend == MODEL_BACKEND_LIGHTGBM) {
        int num_iterations;
        BoosterHandle booster;
        int ret = LGBM_BoosterCreateFromModelfile(path, &num_iterations, &booster);
        if (ret != 0) {
            fprintf(stderr, "[ML] LightGBM: failed to load %s\n", path);
            return 0;
        }
        m->handle = (void*)booster;
        m->backend = MODEL_BACKEND_LIGHTGBM;
        m->num_features = MODEL_NUM_FEATURES;
        fprintf(stderr, "[ML] LightGBM model loaded: %s (%d features, %d iterations)\n",
                path, m->num_features, num_iterations);
        return 1;
    }
#endif

    // backend requested but not compiled in
    if (backend != MODEL_BACKEND_NONE) {
        const char *names[] = {"none", "xgboost", "lightgbm"};
        const char *name = (backend >= 1 && backend <= 2) ? names[backend] : "unknown";
        fprintf(stderr, "[ML] backend '%s' requested but not compiled in (need -DUSE_%s=ON)\n",
                name, backend == 1 ? "XGBOOST" : "LIGHTGBM");
    }
    return 0;
}

//======================================================================================================
// [PREDICT]
//======================================================================================================
// returns raw model output (probability for classifiers, value for regressors)
// returns 0.0f if no model loaded — caller should check Model_IsLoaded first
//======================================================================================================
template <unsigned F>
inline float Model_Predict(ModelHandle<F> *m, const float *features, int num_features) {
    if (!m->handle) return 0.0f;

#ifdef USE_XGBOOST
    if (m->backend == MODEL_BACKEND_XGBOOST) {
        BoosterHandle booster = (BoosterHandle)m->handle;
        DMatrixHandle dmat;
        // create single-row DMatrix from float array
        int ret = XGDMatrixCreateFromMat(features, 1, num_features, -1.0f, &dmat);
        if (ret != 0) return 0.0f;

        bst_ulong out_len;
        const float *out_result;
        ret = XGBoosterPredict(booster, dmat, 0, 0, 0, &out_len, &out_result);
        XGDMatrixFree(dmat);

        if (ret != 0 || out_len == 0) return 0.0f;
        return out_result[0];
    }
#endif

#ifdef USE_LIGHTGBM
    if (m->backend == MODEL_BACKEND_LIGHTGBM) {
        BoosterHandle booster = (BoosterHandle)m->handle;
        double out_result;
        int64_t out_len;
        // single-row prediction — fastest LGBM path
        int ret = LGBM_BoosterPredictForMatSingleRow(
            booster, features, C_API_DTYPE_FLOAT32,
            num_features, 1, // is_row_major
            C_API_PREDICT_NORMAL, 0, -1, "", // predict type, start iteration, num iteration, parameters
            &out_len, &out_result);
        if (ret != 0) return 0.0f;
        return (float)out_result;
    }
#endif

    return 0.0f;
}

//======================================================================================================
// [FREE]
//======================================================================================================
template <unsigned F>
inline void Model_Free(ModelHandle<F> *m) {
    if (!m->handle) return;

#ifdef USE_XGBOOST
    if (m->backend == MODEL_BACKEND_XGBOOST) {
        XGBoosterFree((BoosterHandle)m->handle);
    }
#endif

#ifdef USE_LIGHTGBM
    if (m->backend == MODEL_BACKEND_LIGHTGBM) {
        LGBM_BoosterFree((BoosterHandle)m->handle);
    }
#endif

    m->handle = NULL;
    m->backend = MODEL_BACKEND_NONE;
}

//======================================================================================================
template <unsigned F>
inline int Model_IsLoaded(const ModelHandle<F> *m) {
    return m->backend != MODEL_BACKEND_NONE && m->handle != NULL;
}

//======================================================================================================
// [FEATURE PACKING]
//======================================================================================================
// packs RegimeSignals + RollingStats into a float array for model inference.
// feature order is defined by FEAT_* constants — must match training pipeline.
// forward-declare RegimeSignals to avoid circular include.
//======================================================================================================
template <unsigned F> struct RegimeSignals; // forward declaration

template <unsigned F>
inline int ModelFeatures_Pack(float *buf, const RegimeSignals<F> *sig,
                               const RollingStats<F> *r,
                               const RollingStats<F, 512> *r_long) {
    buf[FEAT_SHORT_SLOPE]    = (float)FPN_ToDouble(sig->short_slope);
    buf[FEAT_SHORT_R2]       = (float)FPN_ToDouble(sig->short_r2);
    buf[FEAT_SHORT_VARIANCE] = (float)FPN_ToDouble(sig->short_variance);
    buf[FEAT_LONG_SLOPE]     = (float)FPN_ToDouble(sig->long_slope);
    buf[FEAT_LONG_R2]        = (float)FPN_ToDouble(sig->long_r2);
    buf[FEAT_LONG_VARIANCE]  = (float)FPN_ToDouble(sig->long_variance);
    buf[FEAT_VOL_RATIO]      = (float)FPN_ToDouble(sig->vol_ratio);
    buf[FEAT_ROR_SLOPE]      = (float)FPN_ToDouble(sig->ror_slope);
    buf[FEAT_VOLUME_SLOPE]   = (float)FPN_ToDouble(sig->volume_slope);
    buf[FEAT_VOLUME_DELTA]   = (float)FPN_ToDouble(sig->volume_delta);
    buf[FEAT_EMA_SMA_SPREAD] = (float)FPN_ToDouble(sig->ema_sma_spread);
    buf[FEAT_VWAP_DEV]       = (float)FPN_ToDouble(r->vwap_deviation);
    buf[FEAT_PRICE_STDDEV]   = (float)FPN_ToDouble(r->price_stddev);
    buf[FEAT_PRICE_AVG]      = (float)FPN_ToDouble(r->price_avg);
    buf[FEAT_VOLUME_AVG]     = (float)FPN_ToDouble(r->volume_avg);
    buf[FEAT_EMA_ABOVE_SMA]  = (float)sig->ema_above_sma;
    return MODEL_NUM_FEATURES;
}

#endif // MODEL_INFERENCE_HPP
