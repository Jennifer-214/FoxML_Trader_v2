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
#include "../MemHeaders/HmacSha256.hpp"  // v5.3.0 Phase B — in-process HMAC + SHA-256 (replaces popen paths)
#include <stdio.h>
#include <string.h>
#include <locale.h>                       // v5.3.0 Phase B — uselocale for canonical body LC_NUMERIC pinning
#include <unistd.h>                       // v5.3.0 Phase B — unlink/rename for atomic stamp writes

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
// v4.3 — medium-horizon feature expansion. All append-only (CLAUDE.md
// "Adding a new ML feature" — never reorder, never remove).
#define FEAT_MID_SLOPE       16  // slope on 256-tick window (between short and long)
#define FEAT_MID_R2          17  // R² of mid-window slope
#define FEAT_CUMDELTA        18  // cumulative trade-side delta (buyer - seller agg) over rolling window
#define FEAT_HOUR_SIN        19  // cyclical hour-of-day: sin(2π × hour/24)
#define FEAT_HOUR_COS        20  // cyclical hour-of-day: cos(2π × hour/24)
#define FEAT_VOL_REGIME_RAT  21  // current short_stddev / longer-baseline stddev (4096-tick)
#define FEAT_TICK_RATE_Z     22  // current ticks/sec z-score vs trailing baseline
#define FEAT_DIST_TO_HIGH    23  // (price_max_baseline - current_price) / price (% below recent high)
#define FEAT_DIST_TO_LOW     24  // (current_price - price_min_baseline) / price (% above recent low)
// v4.5 Wave 1 — D.1 (book imbalance over time), D.2 (flow asymmetry),
// D.4 (large-trade detection). All append-only; new features grow indices
// monotonically.
#define FEAT_BOOK_IMB_MEAN_SHORT 25  // mean of last 64 book_imbalance samples
#define FEAT_BOOK_IMB_MEAN_LONG  26  // mean over full BookImbalanceHistory window (~17m)
#define FEAT_BOOK_IMB_DRIFT      27  // current book_imbalance - mean_long
#define FEAT_FLOW_10S            28  // signed-volume EWMA, half-life 10s
#define FEAT_FLOW_1M             29  // signed-volume EWMA, half-life 60s
#define FEAT_FLOW_5M             30  // signed-volume EWMA, half-life 300s
#define FEAT_LARGE_TRADE_Z       31  // z-score of current trade size vs trailing window
// v4.6 Wave 2 — D.3 (spread dynamics). Reads from BookSnapshot.spread
// (live) / DepthReplayState.current.spread (backtest) — both produce
// identical values for identical input bid/ask streams.
#define FEAT_SPREAD_BPS          32  // current spread / mid_price × 10000 (basis points)
#define FEAT_SPREAD_ZSCORE       33  // z-score of current spread vs trailing window
#define MODEL_NUM_FEATURES       34

// max features buffer — bumped 32 → 64 to leave headroom for D.3 (Wave 2:
// spread_bps, spread_zscore) and any further expansion without retouching
// every fixed-size feature buffer in the codebase.
#define MODEL_MAX_FEATURES   64

// model format version — increment when FEAT_* indices or count changes.
// embedded in trained models, checked at load time. old models with wrong
// version fail loudly instead of producing silent garbage predictions.
// FEAT_* constants are APPEND-ONLY — never reorder, never remove.
// v1: initial 16-feature pack
// v2 (v4.3): added 9 medium-horizon features (FEAT_MID_*, FEAT_CUMDELTA,
//           FEAT_HOUR_SIN/COS, FEAT_VOL_REGIME_RAT, FEAT_TICK_RATE_Z,
//           FEAT_DIST_TO_HIGH/LOW). Old v1 models will fail load.
// v3 (v4.5 Wave 1): added 7 microstructure features (FEAT_BOOK_IMB_*,
//           FEAT_FLOW_*, FEAT_LARGE_TRADE_Z). Old v2 models will fail load.
// v4 (v4.6 Wave 2): added 2 spread features (FEAT_SPREAD_BPS,
//           FEAT_SPREAD_ZSCORE). Old v3 models will fail load.
// v5 (v5.8.1a): introduce feature_registry_hash field in stamp body.
//           Hash is FNV-1a over FOREACH_FEATURE(X) enabled-row names +
//           versions (see FeatureRegistry.hpp). Stamps signed under one
//           registry refuse to load under a different registry. v4 stamps
//           lack the field and fail format-version check.
#define MODEL_FORMAT_VERSION 5

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
    // v4.3 features
    { FEAT_MID_SLOPE,      "mid_slope",      256, 1 },  // 256-tick rolling window
    { FEAT_MID_R2,         "mid_r2",         256, 1 },
    { FEAT_CUMDELTA,       "cumdelta",       1024, 1 },  // rolling buyer-seller agg delta
    { FEAT_HOUR_SIN,       "hour_sin",       1, 1 },     // pure time-of-day, no lookback
    { FEAT_HOUR_COS,       "hour_cos",       1, 1 },
    { FEAT_VOL_REGIME_RAT, "vol_regime_rat", 1024, 1 },  // current vs longer baseline
    { FEAT_TICK_RATE_Z,    "tick_rate_z",    1024, 1 },
    { FEAT_DIST_TO_HIGH,   "dist_to_high",   1024, 1 },
    { FEAT_DIST_TO_LOW,    "dist_to_low",    1024, 1 },
    // v4.5 Wave 1 features
    { FEAT_BOOK_IMB_MEAN_SHORT, "book_imb_mean_short", 64,   1 },  // last 64 slow-path samples
    { FEAT_BOOK_IMB_MEAN_LONG,  "book_imb_mean_long",  1024, 1 },  // BookImbalanceHistory window
    { FEAT_BOOK_IMB_DRIFT,      "book_imb_drift",      1024, 1 },
    { FEAT_FLOW_10S,            "flow_10s",            10,   1 },  // ~10s wallclock
    { FEAT_FLOW_1M,             "flow_1m",             60,   1 },  // ~60s wallclock
    { FEAT_FLOW_5M,             "flow_5m",             300,  1 },  // ~300s wallclock
    { FEAT_LARGE_TRADE_Z,       "large_trade_z",       1024, 1 },
    // v4.6 Wave 2 features
    { FEAT_SPREAD_BPS,          "spread_bps",          1,    1 },  // current value, no lookback
    { FEAT_SPREAD_ZSCORE,       "spread_zscore",       1024, 1 },
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
    int num_outputs;        // 1 = binary/regression, ≥2 = multiclass softmax.
                            // detected at load time. enables stupid-proof check
                            // "model is 3-class but barrier_gate_enabled=0".
    char model_path[256];   // path for display/logging
    char training_fingerprint[65]; // SHA256 of config+data used to train this model (empty if unknown)
};

//======================================================================================================
template <unsigned F>
inline void Model_Init(ModelHandle<F> *m) {
    m->handle = NULL;
    m->backend = MODEL_BACKEND_NONE;
    m->num_features = 0;
    m->num_outputs = 0;
    m->model_path[0] = '\0';
}

//======================================================================================================
// [LOAD]
//======================================================================================================
template <unsigned F>
inline int Model_Load(ModelHandle<F> *m, const char *path, int backend) {
    Model_Init(m);
    m->training_fingerprint[0] = '\0';

    if (!path || path[0] == '\0') return 0; // no path = disabled

    // compute simple file checksum for logging (FNV-1a, fast and dependency-free)
    // full SHA256 available via Fingerprint.hpp but would create circular include
    {
        FILE *cf = fopen(path, "rb");
        if (cf) {
            uint64_t hash = 14695981039346656037ULL; // FNV offset basis
            uint8_t buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), cf)) > 0)
                for (size_t i = 0; i < n; i++)
                    hash = (hash ^ buf[i]) * 1099511628211ULL;
            fclose(cf);
            fprintf(stderr, "[ML] model checksum: %016lx (%s)\n", (unsigned long)hash, path);
        }
    }

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
        // read training fingerprint (if embedded)
        const char *fp = NULL;
        int got_fp = XGBoosterGetAttr(booster, "foxml_fingerprint", &fp, (int[]){0});
        if (got_fp == 0 && fp) {
            strncpy(m->training_fingerprint, fp, 64);
            m->training_fingerprint[64] = '\0';
        }
        m->handle = (void*)booster;
        m->backend = MODEL_BACKEND_XGBOOST;
        m->num_features = MODEL_NUM_FEATURES;
        // detect num_outputs by running a single-row prediction with zeros.
        // for binary models out_len = 1; for multi:softprob out_len = num_class.
        // this is the stupid-proof check: lets the engine warn when a 3-class
        // model is loaded into a binary-config core (or vice versa).
        m->num_outputs = 1;
        {
            float zero_row[MODEL_MAX_FEATURES] = {0};
            DMatrixHandle probe;
            if (XGDMatrixCreateFromMat(zero_row, 1, MODEL_NUM_FEATURES, -1.0f, &probe) == 0) {
                bst_ulong out_len = 0;
                const float *out_result = NULL;
                if (XGBoosterPredict(booster, probe, 0, 0, 0, &out_len, &out_result) == 0) {
                    if (out_len > 0) m->num_outputs = (int)out_len;
                }
                XGDMatrixFree(probe);
            }
        }
        fprintf(stderr, "[ML] XGBoost model loaded: %s (%d features, %d output%s, format v%d%s%s)\n",
                path, m->num_features, m->num_outputs, m->num_outputs == 1 ? "" : "s",
                MODEL_FORMAT_VERSION,
                m->training_fingerprint[0] ? ", fingerprint: " : "",
                m->training_fingerprint[0] ? m->training_fingerprint : "");
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
        m->num_outputs = 1;
        LGBM_BoosterGetNumClasses(booster, &m->num_outputs);
        fprintf(stderr, "[ML] LightGBM model loaded: %s (%d features, %d output%s, %d iterations)\n",
                path, m->num_features, m->num_outputs, m->num_outputs == 1 ? "" : "s", num_iterations);
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
// [PREDICT MULTI — multi-class softmax output]
//======================================================================================================
// fills `out_buf` with up to max_outputs class probabilities. returns the number
// of class outputs actually written (== num_class for the loaded model). on
// failure or no model loaded, returns 0 and leaves buf undisturbed.
//
// for binary classifiers, prefer Model_Predict — this works for them too but
// returns 1 output. the function is intended for models trained with
// objective=multi:softprob (XGBoost) or objective=multiclass (LightGBM).
//======================================================================================================
template <unsigned F>
inline int Model_PredictMulti(ModelHandle<F> *m, const float *features, int num_features,
                               float *out_buf, int max_outputs) {
    if (!m->handle || max_outputs <= 0) return 0;

#ifdef USE_XGBOOST
    if (m->backend == MODEL_BACKEND_XGBOOST) {
        BoosterHandle booster = (BoosterHandle)m->handle;
        DMatrixHandle dmat;
        int ret = XGDMatrixCreateFromMat(features, 1, num_features, -1.0f, &dmat);
        if (ret != 0) return 0;

        bst_ulong out_len;
        const float *out_result;
        // XGBoost returns N×K floats for multi:softprob (N=1 row, K=num_class)
        // for binary objective, returns N floats (same as Model_Predict)
        ret = XGBoosterPredict(booster, dmat, 0, 0, 0, &out_len, &out_result);
        XGDMatrixFree(dmat);

        if (ret != 0 || out_len == 0) return 0;
        int n = (int)out_len < max_outputs ? (int)out_len : max_outputs;
        for (int i = 0; i < n; i++) out_buf[i] = out_result[i];
        return n;
    }
#endif

#ifdef USE_LIGHTGBM
    if (m->backend == MODEL_BACKEND_LIGHTGBM) {
        BoosterHandle booster = (BoosterHandle)m->handle;
        // need to know num_class first — query the booster
        int num_class = 1;
        LGBM_BoosterGetNumClasses(booster, &num_class);
        int n = num_class < max_outputs ? num_class : max_outputs;
        // LightGBM returns doubles, need a temp buffer
        double tmp[32];
        if (n > 32) n = 32; // safety clamp
        int64_t out_len;
        int ret = LGBM_BoosterPredictForMatSingleRow(
            booster, features, C_API_DTYPE_FLOAT32,
            num_features, 1,
            C_API_PREDICT_NORMAL, 0, -1, "",
            &out_len, tmp);
        if (ret != 0) return 0;
        int written = (int)out_len < n ? (int)out_len : n;
        for (int i = 0; i < written; i++) out_buf[i] = (float)tmp[i];
        return written;
    }
#endif

    return 0;
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
// [FEATURE PACKING — DEPRECATED]
//======================================================================================================
// Replaced by Features_PackAll in ML_Headers/FeatureRegistry.hpp (v5.8.1b).
// All 5 production callers (MLStrategy, StrategyParameters dispatcher,
// BacktestSharded, PortfolioController regime/barrier paths) flipped at
// v5.8.1b ship time.
//
// This function is now a frozen historical reference, kept ONLY so the
// EXTENSIBILITY equivalence test in controller_test.cpp can validate that
// Features_PackAll produces bytewise-identical output. Treat any change
// to this body as breaking the regression contract — change Features_PackAll
// instead, then re-pin the FEATURE_REGISTRY_HASH snapshot.
//
// Scheduled for full removal in v5.9 once the registry has a few months
// of paper-validation behind it. At that point the equivalence test gets
// retired alongside.
//
// Feature order is defined by FEAT_* constants — must match training pipeline.
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
    // v4.3 — medium-horizon features
    buf[FEAT_MID_SLOPE]      = (float)FPN_ToDouble(sig->mid_slope);
    buf[FEAT_MID_R2]         = (float)FPN_ToDouble(sig->mid_r2);
    buf[FEAT_CUMDELTA]       = (float)FPN_ToDouble(sig->cumdelta);
    buf[FEAT_HOUR_SIN]       = (float)sig->hour_sin;
    buf[FEAT_HOUR_COS]       = (float)sig->hour_cos;
    buf[FEAT_VOL_REGIME_RAT] = (float)FPN_ToDouble(sig->vol_regime_ratio);
    buf[FEAT_TICK_RATE_Z]    = (float)sig->tick_rate_z;
    buf[FEAT_DIST_TO_HIGH]   = (float)FPN_ToDouble(sig->dist_to_high);
    buf[FEAT_DIST_TO_LOW]    = (float)FPN_ToDouble(sig->dist_to_low);
    // v4.5 Wave 1 — microstructure features (D.1, D.2, D.4)
    buf[FEAT_BOOK_IMB_MEAN_SHORT] = (float)FPN_ToDouble(sig->book_imb_mean_short);
    buf[FEAT_BOOK_IMB_MEAN_LONG]  = (float)FPN_ToDouble(sig->book_imb_mean_long);
    buf[FEAT_BOOK_IMB_DRIFT]      = (float)FPN_ToDouble(sig->book_imb_drift);
    buf[FEAT_FLOW_10S]            = (float)sig->flow_10s;
    buf[FEAT_FLOW_1M]             = (float)sig->flow_1m;
    buf[FEAT_FLOW_5M]             = (float)sig->flow_5m;
    buf[FEAT_LARGE_TRADE_Z]       = (float)sig->large_trade_z;
    // v4.6 Wave 2 — spread features (D.3)
    buf[FEAT_SPREAD_BPS]          = (float)sig->spread_bps;
    buf[FEAT_SPREAD_ZSCORE]       = (float)sig->spread_zscore;
    return MODEL_NUM_FEATURES;
}

//======================================================================================================
// [v5.2.0 — held-out gate: model stamp verification]
//======================================================================================================
// A `.stamp` file lives alongside each `.bin` model:
//
//   models/aggressive/buy_signal.bin
//   models/aggressive/buy_signal.stamp
//
// Stamp format (text, key=value lines, last line is signature):
//
//   model_format_version=12
//   model_sha256=<hex of binary>
//   trained_on=2026-04-28
//   wf_mean_val=0.55
//   held_out_metric=0.53
//   gap=0.02
//   gap_threshold=0.05
//   signature=<HMAC-SHA256(secret, all-prior-lines-concatenated)>
//
// Verifier returns:
//   1 = stamp present, signature valid, gap below threshold → safe to load
//   0 = stamp present but FAILED (sig mismatch, gap too wide, format-version
//       drift, or model_sha256 mismatch) → REJECT, log reason
//  -1 = stamp file missing entirely → caller decides via held_out_gate_strict
//
// Empty `secret` = "accept any signature" mode (dev convenience).
// Real production: set a non-empty secret + flip held_out_gate_strict=1.
//
// Safe from path traversal: caller passes the .bin path; we append ".stamp".
// File reads are bounded; stamp file > 4KB is treated as malformed.
//======================================================================================================

struct ModelStampResult {
    int      valid;             // 1 / 0 / -1 per above
    char     reason[256];       // human-readable failure reason
    int      model_format_version;
    double   generalization_gap;
    double   gap_threshold;
    uint64_t feature_registry_hash;  // v5.8.1a: 0 if absent (old stamps)
};

// Compute SHA-256 of a file. Reads in 64K chunks, safe for any size.
// Out parameter `hex` must be at least 65 bytes (64 hex digits + NUL).
//
// v5.3.0 Phase B: now an in-process EVP wrapper. Was a popen("sha256sum ...")
// shell-out in v5.2.0 — replaced for speed, shell-injection safety, and
// removing the dependency on /usr/bin/sha256sum being installed.
inline int sha256_file_hex(const char* path, char* hex_out, size_t hex_cap) {
    return tt::sha256_file_hex_inproc(path, hex_out, hex_cap);
}

// Parse a "key=value" line into key + value pointers. Returns 1 on success.
// Modifies `line` in place (NUL-terminates the key at '=').
inline int stamp_parse_line(char* line, const char** key_out, const char** val_out) {
    char* eq = strchr(line, '=');
    if (!eq) return 0;
    *eq = '\0';
    *key_out = line;
    *val_out = eq + 1;
    // Trim trailing newline from value
    char* nl = strchr((char*)*val_out, '\n');
    if (nl) *nl = '\0';
    return 1;
}

// Verify a model stamp. See header for return values + format.
//
// model_path: path to the .bin file. `.stamp` is implied by appending.
// secret: HMAC secret. Empty string ("") = accept-any signature (dev mode).
// gap_threshold: max acceptable generalization gap. Stamp gap must be ≤ this.
// expected_format_version: caller passes MODEL_FORMAT_VERSION; mismatch fails.
inline ModelStampResult verify_model_stamp(const char* model_path,
                                            const char* secret,
                                            double gap_threshold,
                                            int expected_format_version,
                                            uint64_t expected_feature_registry_hash = 0) {
    ModelStampResult r;
    r.valid = -1;
    r.reason[0] = '\0';
    r.model_format_version = 0;
    r.generalization_gap = 0.0;
    r.gap_threshold = gap_threshold;
    r.feature_registry_hash = 0;

    char stamp_path[512];
    snprintf(stamp_path, sizeof(stamp_path), "%s.stamp", model_path);
    FILE* f = fopen(stamp_path, "r");
    if (!f) {
        snprintf(r.reason, sizeof(r.reason), "stamp file missing: %s", stamp_path);
        return r;
    }

    // Read the whole stamp into a buffer (cap 4KB)
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason), "empty stamp file");
        return r;
    }
    buf[n] = '\0';

    // Parse line-by-line. Capture each key/value into r and detect the
    // signature line. Build the canonical "signed body" (everything before
    // signature= line) for HMAC verify.
    char canonical[4096] = {0};
    size_t canonical_len = 0;
    char model_sha[80] = {0};
    char stamp_sig[128] = {0};

    char* save = nullptr;
    char* line = strtok_r(buf, "\n", &save);
    while (line) {
        char line_copy[512] = {0};
        size_t lc = strlen(line);
        if (lc >= sizeof(line_copy)) lc = sizeof(line_copy) - 1;
        memcpy(line_copy, line, lc);
        line_copy[lc] = '\0';

        const char* key;
        const char* val;
        if (stamp_parse_line(line_copy, &key, &val)) {
            if (strcmp(key, "signature") == 0) {
                size_t vl = strlen(val);
                if (vl >= sizeof(stamp_sig)) vl = sizeof(stamp_sig) - 1;
                memcpy(stamp_sig, val, vl);
                stamp_sig[vl] = '\0';
                break;  // signature is the last line; stop accumulating canonical
            }
            // Add this line to canonical (in original "key=val\n" form)
            int wrote = snprintf(canonical + canonical_len,
                                  sizeof(canonical) - canonical_len,
                                  "%s=%s\n", key, val);
            if (wrote > 0 && (size_t)wrote < sizeof(canonical) - canonical_len) {
                canonical_len += wrote;
            }
            // Capture fields we care about
            if (strcmp(key, "model_format_version") == 0) {
                r.model_format_version = atoi(val);
            } else if (strcmp(key, "model_sha256") == 0) {
                size_t vl = strlen(val);
                if (vl >= sizeof(model_sha)) vl = sizeof(model_sha) - 1;
                memcpy(model_sha, val, vl);
                model_sha[vl] = '\0';
            } else if (strcmp(key, "gap") == 0) {
                r.generalization_gap = atof(val);
            } else if (strcmp(key, "gap_threshold") == 0) {
                r.gap_threshold = atof(val);
            } else if (strcmp(key, "feature_registry_hash") == 0) {
                // v5.8.1a: parse hex-encoded 64-bit hash. strtoull accepts
                // 0x-prefix or bare hex. Stamp emits %016lx (no prefix).
                r.feature_registry_hash = (uint64_t)strtoull(val, nullptr, 16);
            }
        }
        line = strtok_r(nullptr, "\n", &save);
    }

    // 1. Format version match
    if (r.model_format_version != expected_format_version) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason),
            "format-version mismatch: stamp=%d engine=%d",
            r.model_format_version, expected_format_version);
        return r;
    }

    // 1b. v5.8.1a — feature registry hash match. When caller passes
    // expected_feature_registry_hash != 0, stamp must contain matching
    // hash. Default 0 = "skip check" (backward-compat for callers that
    // haven't migrated to the new signature; v5.8.1b flips them all).
    if (expected_feature_registry_hash != 0 &&
        r.feature_registry_hash != expected_feature_registry_hash) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason),
            "feature-registry-hash mismatch: stamp=%016lx engine=%016lx "
            "(retrain required)",
            (unsigned long)r.feature_registry_hash,
            (unsigned long)expected_feature_registry_hash);
        return r;
    }

    // 2. Gap acceptable
    if (r.generalization_gap > r.gap_threshold) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason),
            "generalization gap %.4f exceeds threshold %.4f",
            r.generalization_gap, r.gap_threshold);
        return r;
    }

    // 3. Model file hasn't been swapped post-stamp
    char actual_sha[80] = {0};
    if (!sha256_file_hex(model_path, actual_sha, sizeof(actual_sha))) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason),
            "could not compute sha256 of %s", model_path);
        return r;
    }
    if (model_sha[0] && strcmp(model_sha, actual_sha) != 0) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason),
            "model file hash differs from stamp: actual=%.16s... stamp=%.16s...",
            actual_sha, model_sha);
        return r;
    }

    // 4. Signature verify (HMAC-SHA256 over canonical body, base64 or hex sig)
    //    Empty secret = accept-any (dev mode). Production should set secret.
    if (secret == nullptr || secret[0] == '\0') {
        // Dev mode — accept without sig check. Log a warning so this is
        // visible in stderr.
        fprintf(stderr,
            "[stamp] WARN: held_out_stamp_secret is empty — signature NOT verified for %s\n",
            stamp_path);
        r.valid = 1;
        snprintf(r.reason, sizeof(r.reason), "ok (dev mode, sig unchecked)");
        return r;
    }
    // v5.3.0 Phase B: in-process HMAC. Was a popen("openssl dgst -sha256 -hmac")
    // shell-out in v5.2.0 — replaced for shell-injection safety (canonical
    // body contained user-controlled fields like trained_on, secret was
    // single-quoted). RFC 4231 vectors and bash-compat regression test in
    // controller_test.cpp guard sig parity with the bash script's openssl
    // calls.
    char computed[80];
    if (!tt::hmac_sha256_hex(secret, canonical, computed)) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason), "HMAC-SHA256 computation failed");
        return r;
    }
    if (strcmp(computed, stamp_sig) == 0) {
        r.valid = 1;
        snprintf(r.reason, sizeof(r.reason), "ok (signature verified, gap %.4f ≤ %.4f)",
            r.generalization_gap, r.gap_threshold);
    } else {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason),
            "signature mismatch: stamp=%.16s... computed=%.16s...",
            stamp_sig, computed);
    }
    return r;
}

//======================================================================================================
// [v5.3.0 Phase B — stamp_write_for_model: sign + write a model stamp in-process]
//======================================================================================================
// Inverse of verify_model_stamp. Computes SHA-256 of the model file,
// builds the same canonical body the verifier reads, signs with
// HMAC-SHA256, writes <model>.stamp atomically (write to .tmp, then
// rename — POSIX atomic within a filesystem). Refuses to write when
// |wf - held_out| > gap_threshold unless `force` is set.
//
// Field order in the canonical body MUST match tools/stamp_model.sh
// byte-for-byte; bash-compat regression test in controller_test.cpp
// catches drift.
//
// Locale pinning: %g/%f honor LC_NUMERIC. A stamp signed under
// LC_NUMERIC=C wouldn't verify under LC_NUMERIC=de_DE because
// 0.55 → "0,55" in some locales. We pin LC_NUMERIC=C for the canonical
// body construction (per-thread via uselocale).
//======================================================================================================

struct StampWriteResult {
    int  ok;             // 1 = stamp written; 0 = refused (gap too wide, i/o, etc.)
    char error[256];     // human-readable failure reason
    char stamp_path[512]; // where it was written (or would have been)
};

inline StampWriteResult stamp_write_for_model(const char* model_path,
                                                const char* secret,
                                                int   format_version,
                                                const char* trained_on_iso,  // YYYY-MM-DD
                                                double wf_mean_val,
                                                double held_out_metric,
                                                double gap_threshold,
                                                int   force,
                                                uint64_t feature_registry_hash = 0) {
    StampWriteResult r;
    r.ok = 0;
    r.error[0] = '\0';
    r.stamp_path[0] = '\0';

    if (!model_path || !trained_on_iso) {
        snprintf(r.error, sizeof(r.error), "NULL inputs (model_path/trained_on_iso)");
        return r;
    }

    // 1. SHA-256 of the model file (in-process)
    char model_sha[80] = {0};
    if (!tt::sha256_file_hex_inproc(model_path, model_sha, sizeof(model_sha))) {
        snprintf(r.error, sizeof(r.error), "could not sha256 %s", model_path);
        return r;
    }

    // 2. Compute |wf - held_out|
    double gap = wf_mean_val - held_out_metric;
    if (gap < 0) gap = -gap;

    // 3. Refuse on gap > threshold (unless force)
    if (gap > gap_threshold && !force) {
        snprintf(r.error, sizeof(r.error),
            "REFUSE: gap %.4f > threshold %.4f (use force=1 to override)",
            gap, gap_threshold);
        return r;
    }

    // 4. Pin LC_NUMERIC=C for canonical body construction. uselocale() is
    //    per-thread — doesn't disturb the rest of the process.
    locale_t pinned = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    locale_t prev = (locale_t)0;
    if (pinned) prev = uselocale(pinned);

    // 5. Canonical body — must match bash script + verifier byte-for-byte.
    //    Field order: format-version, sha256, trained_on, wf_mean_val,
    //    held_out_metric, gap, gap_threshold, [feature_registry_hash].
    //    feature_registry_hash is appended ONLY when format_version >= 5
    //    AND a non-zero hash was supplied. v4 stamps and dev-mode invocations
    //    omit the field — verifier handles missing field as "skip check"
    //    when expected_feature_registry_hash == 0.
    //    Each line ends with \n.
    char canonical[2048];
    int n;
    if (format_version >= 5 && feature_registry_hash != 0) {
        n = snprintf(canonical, sizeof(canonical),
            "model_format_version=%d\n"
            "model_sha256=%s\n"
            "trained_on=%s\n"
            "wf_mean_val=%g\n"
            "held_out_metric=%g\n"
            "gap=%.6f\n"
            "gap_threshold=%g\n"
            "feature_registry_hash=%016lx\n",
            format_version, model_sha, trained_on_iso,
            wf_mean_val, held_out_metric, gap, gap_threshold,
            (unsigned long)feature_registry_hash);
    } else {
        n = snprintf(canonical, sizeof(canonical),
            "model_format_version=%d\n"
            "model_sha256=%s\n"
            "trained_on=%s\n"
            "wf_mean_val=%g\n"
            "held_out_metric=%g\n"
            "gap=%.6f\n"
            "gap_threshold=%g\n",
            format_version, model_sha, trained_on_iso,
            wf_mean_val, held_out_metric, gap, gap_threshold);
    }

    // Restore prior locale ASAP — every subsequent return must NOT undo this twice
    if (pinned) {
        uselocale(prev);
        freelocale(pinned);
    }

    if (n <= 0 || (size_t)n >= sizeof(canonical)) {
        snprintf(r.error, sizeof(r.error), "canonical body overflow (n=%d)", n);
        return r;
    }

    // 6. HMAC-SHA256(secret, canonical). Empty secret = dev-mode placeholder
    //    so the file is well-formed but the engine knows to skip sig check.
    char sig[80];
    const char* effective_secret = (secret && secret[0]) ? secret : "";
    if (effective_secret[0] == '\0') {
        memcpy(sig, "devmode-no-secret-no-signature", 31);
        sig[31] = '\0';
    } else {
        if (!tt::hmac_sha256_hex(effective_secret, canonical, sig)) {
            snprintf(r.error, sizeof(r.error), "HMAC-SHA256 computation failed");
            return r;
        }
    }

    // 7. Atomic write: write to <stamp>.tmp, then rename. POSIX rename()
    //    is atomic within the same filesystem, so a reader can never
    //    observe a partially-written stamp.
    snprintf(r.stamp_path, sizeof(r.stamp_path), "%s.stamp", model_path);
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", r.stamp_path);

    FILE* f = fopen(tmp_path, "w");
    if (!f) {
        snprintf(r.error, sizeof(r.error), "fopen failed: %s", tmp_path);
        return r;
    }
    fputs(canonical, f);
    fprintf(f, "signature=%s\n", sig);
    if (fclose(f) != 0) {
        unlink(tmp_path);
        snprintf(r.error, sizeof(r.error), "fclose failed: %s", tmp_path);
        return r;
    }

    if (rename(tmp_path, r.stamp_path) != 0) {
        unlink(tmp_path);
        snprintf(r.error, sizeof(r.error), "rename failed: %s -> %s",
                 tmp_path, r.stamp_path);
        return r;
    }

    r.ok = 1;
    return r;
}

#endif // MODEL_INFERENCE_HPP
