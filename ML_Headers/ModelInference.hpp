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
#include "../Version.hpp"                 // v5.9.2b — ENGINE_VERSION_STRING for cross-major detection
#include "FeatureStandardizer.hpp"       // v5.9.3a — inline scaler struct on ModelHandle
#include "../CoreFrameworks/ParseFast.hpp"  // v5.11.4.C — std::from_chars wrapper (locale immunity)
#include "StampBoundCfgRegistry.hpp"     // v5.14.1.B.3 — FOREACH_STAMP_BOUND_CFG X-macro
#include <stdio.h>
#include <string.h>
#include <locale.h>                       // v5.3.0 Phase B — uselocale for canonical body LC_NUMERIC pinning
#include <unistd.h>                       // v5.3.0 Phase B — unlink/rename for atomic stamp writes

// backend IDs
#define MODEL_BACKEND_NONE     0
#define MODEL_BACKEND_XGBOOST  1
#define MODEL_BACKEND_LIGHTGBM 2
// v5.12.2.D — Treelite AOT backend slot (INFRASTRUCTURE-ONLY).
// Compiled C++ from XGBoost / LightGBM trees emits an inference function;
// Model_LoadAOT dlopen's the .so + resolves the predict symbol. Brings
// per-row inference from ~1-5us (C API) to <100ns. Operator workflow:
//   1. Train model (existing pipeline)
//   2. Run tools/aot_compile_model.sh <model.json> → emits <model>.aot.so +
//      stamp body extension (has_aot_compiled_sha256 + aot_compiled_path)
//   3. Re-stamp model with the new fields
//   4. Set cfg.use_aot_inference=1 to opt in
// This ship lands the BACKEND constant + cfg field + stamp body fields +
// a Model_LoadAOT stub that returns -1 (= "Treelite not vendored; engine
// falls back to XGBoost C API path"). Treelite vendoring + actual
// Predict_AOT impl land in a follow-up after operator tests on hardware.
#define MODEL_BACKEND_AOT      3

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
// v6 (v5.10.0b): bytewise-deterministic FPN-end-to-end slow path.
//           Multiple slow-path math primitives migrated from IEEE-754 to
//           pure-integer FPN: FlowFeatures EWMA decay (FPN_Exp), z-score
//           sqrt (FPN_Sqrt), RegimeDetector hour_sin/cos (FPN_Sin/Cos),
//           and FP64 divide (192-by-128 long division replaces long-double
//           FPU path). All slow-path features now produce bytewise-
//           identical output across compilers / -O levels / FMA support
//           given identical inputs. Bit-level shifts vs. v5 absorbed by
//           retraining; v5 stamps refuse to load with a "model trained
//           with pre-v5.10 IEEE-754 math; retrain required" message.
#define MODEL_FORMAT_VERSION 6

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
    // v5.9.3a — feature standardizer (mean-centering + unit-variance).
    // Inline (not heap) per audit decision: NUM_REGISTERED_FEATURES is
    // constexpr → struct size known at compile time. ~600 bytes per
    // handle; trivial vs the mmap'd XGBoost booster size.
    // has_scaler=0 = identity (legacy / not loaded); =1 = active.
    // Apply path early-returns when 0, so v5.9.3a "ships disabled"
    // is the natural state until v5.9.3b activates training-side
    // Compute + Persist + the 5 apply-site callers.
    tt::FeatureStandardizer scaler;
    // v5.9.3a — Gap H observability. Set by CoreModelZoo_TryLoadRole
    // when scaler load fails in non-strict mode (engine warns + applies
    // identity, distinct from model_load_failed which is the model itself).
    // Surfaces to PerCoreSnap.ml_scaler_load_failed for ML Status panel.
    int scaler_load_failed;
    // v5.9.4a — stamp-derived fields copied at CoreModelZoo_TryLoadRole
    // post-verify. Engine boot reads these to surface drift (Phase 6
    // poll_interval cadence) or refuse mismatched models (Phase 5
    // num_outputs). Set to 0 when stamp lacks the field (legacy stamps).
    uint32_t training_poll_interval;        // from stamp; 0 if absent
    uint8_t  has_training_poll_interval;    // 1 if stamp had it
    int      stamp_model_num_outputs;       // from stamp; 0 if absent
    uint8_t  has_stamp_num_outputs;         // 1 if stamp had model_num_outputs
    // v5.9.5h — XGBoost hyperparams from stamp, copied here at
    // CoreModelZoo._TryLoadRole load time. EngineSharded boot WARN
    // compares these vs cfg.xgb_* (mismatch logged; no refuse since
    // hyperparams don't affect inference).
    uint8_t  has_xgb_hyperparams;
    int      stamp_xgb_max_depth;
    double   stamp_xgb_learning_rate;
    int      stamp_xgb_n_estimators;
    double   stamp_xgb_subsample;
    double   stamp_xgb_colsample_bytree;
    int      stamp_xgb_min_child_weight;
    int      stamp_xgb_seed;
    char     stamp_xgb_tree_method[16];
    // v5.9.5h Phase 10 — build flags fingerprint
    uint8_t  has_build_flags_hash;
    uint64_t stamp_build_flags_hash;
    // v5.9.5i — stamp's recorded inference cfg fields. EngineSharded
    // boot-WARN/REFUSE compares these vs cfg.* (Tier 1: freshness_tau,
    // confidence_threshold_scale, barrier_gate_enabled — REFUSE strict;
    // Tier 2: hard_block, bandit, fees — WARN).
    uint8_t  has_stamp_inference_cfg;
    double   stamp_inf_confidence_threshold_scale;
    int      stamp_inf_barrier_gate_enabled;
    double   stamp_inf_confidence_hard_block_threshold;
    double   stamp_inf_freshness_tau;
    uint8_t  has_stamp_bandit;
    double   stamp_inf_bandit_blend_ratio;
    uint8_t  has_stamp_fees;
    double   stamp_inf_fee_rate_maker;
    double   stamp_inf_fee_rate_taker;
    // v5.11.42 D.1 — xgb_train_nthread recorded at training time. Stamp's
    // value is forensic — engine boot-WARN compares vs cfg.xgb_train_nthread
    // when stamp's nthread was 1 (indicates parallel multi-horizon mode)
    // but engine cfg has nthread > 1 (= reload would produce different
    // model bytes). Doesn't refuse — XGBoost output already trained,
    // just operator notification.
    uint8_t  has_stamp_xgb_train_nthread;
    int      stamp_xgb_train_nthread;
    // v5.11.42 D.2 — label_lookahead_ticks (aka horizon ticks) recorded
    // at training time. Engine ensemble auto-detect (CoreModelZoo_AutoDetectEnsemble)
    // parses horizon from dir name `_horizon_<N>` and compares vs this
    // field at load. Mismatch = REFUSE load with "stamp says horizon=X
    // but loaded from dir=horizon_Y" (catches dir rename / copy mistake).
    uint8_t  has_stamp_label_params;
    int      stamp_label_lookahead_ticks;
    double   stamp_label_tp_pct;
    double   stamp_label_sl_pct;
    // v5.11.42 D.3 — stamp's scaler_sha256 (forensic; for ensemble-sibling
    // consistency check). Per-horizon scalers in a Multi-Horizon ensemble
    // SHOULD be identical (scaler is derived from the shared feature matrix,
    // not from per-horizon labels). EnsembleModelZoo_LoadFromCfg post-loop
    // WARNs if siblings have different scaler_sha256 (= mixed training
    // sessions or accidental sidecar copy mistake).
    uint8_t  has_stamp_scaler_sha256;
    char     stamp_scaler_sha256[65];
    // v5.14.3.B — overlay-derived fields on runtime ModelHandle.
    // Mirrors ModelStampResult (parser side) — copied via TryLoadRole
    // post-verify_model_stamp. Read by FeatureOverlay_PostLoadVerify.
    uint8_t  has_overlay_hash;
    char     overlay_hash[65];
    uint8_t  has_effective_hash;
    char     effective_hash[65];
    // v5.11.62 — for multiclass models, which class index is the
    // "buy probability"? Default 0 (binary positive class) preserves
    // legacy semantics. CoreModelZoo loader sets:
    //   - 0 for buy_signal role (binary)
    //   - 1 for barrier role with num_outputs=3 (PEAK_VALLEY_STABLE class 1
    //     = peak = price expected to rise = entry signal)
    //   - 0 for regime role (operator chooses semantics via cfg)
    // Model_Predict returns out_result[buy_class_idx]. Out-of-range
    // index falls back to 0.
    int      buy_class_idx;
    // v5.12.3.B+E — prediction normalizer. Maps heterogeneous model
    // outputs to a [0,1] buy-probability space so ensemble blend can
    // average across mixed model types. Default NORM_IDENTITY = passthrough
    // (preserves existing single-output semantics bytewise). Loader sets
    // this from stamp body's label_kind at load time; never mutated post-
    // load (per-handle invariant). normalizer_param holds tp_pct for
    // NORM_REGRESSION; unused for other kinds.
    //
    // Elision-friendly design (per CLAUDE.md item 18): hot-path-equivalent
    // call site checks `if (m->normalizer == NORM_IDENTITY) return raw;`
    // FIRST. Default state → 1-line early return → optimizer treats as
    // ~1ns predicted-not-taken. Switch on enum is only entered when a
    // model is actually trained with non-IDENTITY normalizer. ~1ns
    // when off; ~5ns when on. Slow-path budget irrelevant either way
    // (Model_Predict's XGBoost C API call dominates at ~1-5us).
    enum prediction_normalizer_t {
        NORM_IDENTITY        = 0,    // passthrough (default; current behavior)
        NORM_REGRESSION      = 1,    // [-tp_pct, +tp_pct] → [0, 1] via clamp(0.5 + raw / (2*tp), 0, 1)
        NORM_BARRIER_CLASS_1 = 2,    // 3-class barrier; explicit class-1 extraction
        NORM_COMPOSITE       = 3,    // uses target_classes/class_weights from this struct (3.A)
    };
    uint8_t  normalizer;             // default NORM_IDENTITY
    float    normalizer_param;       // tp_pct for NORM_REGRESSION; unused otherwise
    // v5.12.3.A — composite-signal extractor. When num_classes_active > 1,
    // Model_Predict returns Σ class_weights[i] × out_result[target_classes[i]]
    // over the first num_classes_active entries. Default num_classes_active=1
    // + target_classes[0]=buy_class_idx + class_weights[0]=1.0 → preserves
    // existing single-class behavior bytewise. Useful for: 5-class up/down
    // models (target=[strong_up, strong_down], weights=[+1,-1] → directional
    // probability difference); P(strong_up) - P(strong_down) composites; any
    // soft-blended decision rule. Strategy code unchanged (per v5.11.62
    // invariant — composition lives in Model_Predict, not strategy).
    // Out-of-range target_classes[i] entries skip silently (defensive).
    uint8_t  num_classes_active;     // default 1
    int      target_classes[8];      // default [buy_class_idx, 0, 0, 0, 0, 0, 0, 0]
    float    class_weights[8];       // default [1.0, 0, 0, 0, 0, 0, 0, 0]
};

//======================================================================================================
template <unsigned F>
inline void Model_Init(ModelHandle<F> *m) {
    m->handle = NULL;
    m->backend = MODEL_BACKEND_NONE;
    m->num_features = 0;
    m->num_outputs = 0;
    m->model_path[0] = '\0';
    // v5.9.3a — scaler init. has_scaler=0 means identity-applied;
    // CoreModelZoo_TryLoadRole calls FeatureStandardizer_Load post-Model_Load
    // to populate.
    tt::FeatureStandardizer_Init(&m->scaler);
    m->scaler_load_failed = 0;
    // v5.9.4a — stamp-derived fields zero-init. CoreModelZoo_TryLoadRole
    // copies values post-verify; absent flags stay 0 (legacy stamp path).
    m->training_poll_interval = 0;
    m->has_training_poll_interval = 0;
    m->stamp_model_num_outputs = 0;
    m->has_stamp_num_outputs = 0;
    // v5.9.5h — XGBoost hyperparam fields zero-init
    m->has_xgb_hyperparams = 0;
    m->stamp_xgb_max_depth = 0;
    m->stamp_xgb_learning_rate = 0.0;
    m->stamp_xgb_n_estimators = 0;
    m->stamp_xgb_subsample = 0.0;
    m->stamp_xgb_colsample_bytree = 0.0;
    m->stamp_xgb_min_child_weight = 0;
    m->stamp_xgb_seed = 0;
    m->stamp_xgb_tree_method[0] = '\0';
    m->has_build_flags_hash = 0;
    m->stamp_build_flags_hash = 0;
    // v5.9.5i — stamp inference cfg fields zero-init
    m->has_stamp_inference_cfg = 0;
    m->stamp_inf_confidence_threshold_scale = 0.0;
    m->stamp_inf_barrier_gate_enabled = 0;
    m->stamp_inf_confidence_hard_block_threshold = 0.0;
    m->stamp_inf_freshness_tau = 0.0;
    m->has_stamp_bandit = 0;
    m->stamp_inf_bandit_blend_ratio = 0.0;
    m->has_stamp_fees = 0;
    m->stamp_inf_fee_rate_maker = 0.0;
    m->stamp_inf_fee_rate_taker = 0.0;
    // v5.11.42 — stamp xgb_train_nthread + label params zero-init
    m->has_stamp_xgb_train_nthread = 0;
    m->stamp_xgb_train_nthread = 0;
    m->has_stamp_label_params = 0;
    m->stamp_label_lookahead_ticks = 0;
    m->stamp_label_tp_pct = 0.0;
    m->stamp_label_sl_pct = 0.0;
    m->has_stamp_scaler_sha256 = 0;
    m->stamp_scaler_sha256[0] = '\0';
    // v5.14.3.B — overlay-derived fields zero-init
    m->has_overlay_hash = 0;
    m->overlay_hash[0] = '\0';
    m->has_effective_hash = 0;
    m->effective_hash[0] = '\0';
    // v5.11.62 — buy class default = 0 (binary positive class).
    m->buy_class_idx = 0;
    // v5.12.3.A — composite-signal defaults: single-class extraction equivalent
    // to pre-v5.12.3.A behavior. Loader sets num_classes_active>1 + target_classes/
    // class_weights from stamp body (Surface G) when operator trains a model
    // with composite-signal config.
    m->num_classes_active = 1;
    m->target_classes[0] = m->buy_class_idx;
    for (int i = 1; i < 8; ++i) m->target_classes[i] = 0;
    m->class_weights[0] = 1.0f;
    for (int i = 1; i < 8; ++i) m->class_weights[i] = 0.0f;
    // v5.12.3.B+E — normalizer defaults: NORM_IDENTITY = passthrough.
    // Preserves existing single-output behavior bytewise. Loader picks
    // non-IDENTITY when stamp body's label_kind needs it (e.g.,
    // REGRESSION model uses NORM_REGRESSION).
    m->normalizer = ModelHandle<F>::NORM_IDENTITY;
    m->normalizer_param = 0.0f;
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
        const char *names[] = {"none", "xgboost", "lightgbm", "aot"};
        const char *name = (backend >= 1 && backend <= 3) ? names[backend] : "unknown";
        fprintf(stderr, "[ML] backend '%s' requested but not compiled in (need -DUSE_%s=ON)\n",
                name, backend == 1 ? "XGBOOST" : (backend == 2 ? "LIGHTGBM" : "TREELITE_AOT"));
    }
    return 0;
}

//======================================================================================================
// [TREELITE AOT — INFRASTRUCTURE STUBS (v5.12.2.D)]
//======================================================================================================
// Stubs for compiled-tree inference. Returns -1 = "AOT not vendored;
// caller falls back to MODEL_BACKEND_XGBOOST path." The real
// implementation lands when Treelite is vendored to vendor/treelite/
// (gitignored, ~hundreds of MB) + the operator runs the compile script
// on their hardware. Ship plan:
//   1. (this ship) — slot in the dispatch chain + stamp body fields +
//      cfg flag + dlopen scaffolding stubs
//   2. (follow-up) — vendor Treelite, wire actual dlopen + symbol resolve
//      + Predict_AOT FFI shim
//   3. (validation) — 1000-feature parity test: AOT == C API within 1e-6
//
// Failure-mode contract: the engine never fires Predict_AOT in this ship
// because Model_LoadAOT always returns -1. Caller (CoreModelZoo) sees the
// failure, logs a single INFO line, and proceeds with MODEL_BACKEND_XGBOOST.
// Operator behavior is bytewise identical to pre-.D when use_aot_inference=0
// or when AOT load fails — the cfg flag is opt-in and load failure is
// transparent fallback.
//======================================================================================================
// [PREDICT NORMALIZED — mixed-output ensemble support (v5.12.3.B+E)]
//======================================================================================================
// Wraps Model_Predict + applies the per-handle normalizer to map any
// model's raw output to a [0, 1] buy-probability space. Bandit ensemble
// blend can average normalized values across mixed model types
// (binary, regression, 3-class barrier).
//
// Elision-friendly: default NORM_IDENTITY → 1-line early return.
// Branch is heavily predicted (default state); ~1ns runtime cost when
// no model uses non-IDENTITY normalizer (= every existing operator
// model today). Switch on enum is entered only after a model is trained
// with a non-default label_kind that needs scale alignment.
//
// Strategy code unchanged per v5.11.62 invariant — strategy reads the
// (already-normalized) float and acts on it. Composition lives in
// Model_Predict + Model_Predict_Normalized; not in strategy.
//======================================================================================================
template <unsigned F>
inline float Model_Predict_Normalized(ModelHandle<F>* m,
                                        const float* features,
                                        int num_features) {
    float raw = Model_Predict(m, features, num_features);
    // Elision-friendly fast path: NORM_IDENTITY (default) → return raw.
    // Optimizer + branch predictor reduce this to ~1ns when default.
    if (m->normalizer == ModelHandle<F>::NORM_IDENTITY) return raw;

    switch (m->normalizer) {
        case ModelHandle<F>::NORM_REGRESSION: {
            // [-tp_pct, +tp_pct] → [0, 1]. tp from normalizer_param at load.
            float tp = m->normalizer_param;
            if (tp <= 0.0f) return 0.5f;  // defensive: invalid tp → neutral
            float v = 0.5f + raw / (2.0f * tp);
            return (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
        case ModelHandle<F>::NORM_BARRIER_CLASS_1:
            // Loader sets buy_class_idx=1 for these handles, so
            // Model_Predict already returned out_result[1]. Just
            // passthrough; documented for clarity + future where the
            // loader-side aliasing is dropped (v5.11.62 cleanup).
            return raw;
        case ModelHandle<F>::NORM_COMPOSITE:
            // Phase 3.A's composite extraction already ran inside
            // Model_Predict. Just clamp to [0, 1].
            return (raw < 0.0f) ? 0.0f : (raw > 1.0f ? 1.0f : raw);
        default:
            return raw;  // unrecognized → passthrough
    }
}

//======================================================================================================
// [PREDICT AT CLASS — class-explicit predict (v5.12.3.E foundation)]
//======================================================================================================
// Decouples the class-extraction concern from the role-aliasing concern.
// Strategy code (or ensemble blend) can ask for "this model's class N"
// without knowing role-name semantics (buy_signal vs barrier vs regime).
//
// Foundation for the v5.11.62 architectural cleanup: future loader
// refactor populates ezoo->primary_handles directly + sets per-handle
// buy_class_idx; consumers call Model_Predict_AtClass with the
// configured class index. Removes the tactical memcpy alias (which
// requires the borrowed flag bookkeeping).
//
// In this ship: just the helper. Loader integration + alias removal
// deferred to follow-up (when operator trains a 4th label kind that
// breaks the current tactical patch's assumptions).
//
// Behavior: identical to Model_Predict but uses caller-supplied
// class_idx instead of m->buy_class_idx. Default Model_Predict() ==
// Model_Predict_AtClass(m, features, n, m->buy_class_idx).
//======================================================================================================
template <unsigned F>
inline float Model_Predict_AtClass(ModelHandle<F>* m,
                                     const float* features,
                                     int num_features,
                                     int class_idx) {
    if (!m->handle) return 0.0f;
#ifdef USE_XGBOOST
    if (m->backend == MODEL_BACKEND_XGBOOST) {
        BoosterHandle booster = (BoosterHandle)m->handle;
        DMatrixHandle dmat;
        int ret = XGDMatrixCreateFromMat(features, 1, num_features, -1.0f, &dmat);
        if (ret != 0) return 0.0f;
        bst_ulong out_len;
        const float *out_result;
        ret = XGBoosterPredict(booster, dmat, 0, 0, 0, &out_len, &out_result);
        XGDMatrixFree(dmat);
        if (ret != 0 || out_len == 0) return 0.0f;
        int idx = class_idx;
        if (idx < 0 || (unsigned long)idx >= out_len) idx = 0;
        return out_result[idx];
    }
#endif
    // Other backends fall back to the standard Model_Predict (which uses
    // m->buy_class_idx). For LIGHTGBM this is fine since LGBM single-row
    // returns a single scalar.
    (void)class_idx;
    return Model_Predict(m, features, num_features);
}

template <unsigned F>
inline int Model_LoadAOT(ModelHandle<F>* m, const char* path) {
    // INFRASTRUCTURE-ONLY in v5.12.2.D. Treelite vendor lib not present;
    // returns -1 to signal "fall back to C API". Future ship dlopen's
    // path + resolves the predict symbol via dlsym; populates
    // m->aot_handle (new field on ModelHandle, added in follow-up) +
    // sets m->backend = MODEL_BACKEND_AOT.
    (void)m; (void)path;
    fprintf(stderr,
        "[ML] Model_LoadAOT: Treelite not vendored in this build; "
        "engine will fall back to MODEL_BACKEND_XGBOOST C API path.\n");
    return -1;
}

template <unsigned F>
inline float Model_Predict_AOT(ModelHandle<F>* m, const float* features,
                                 int num_features) {
    // INFRASTRUCTURE-ONLY. Same fallback semantics as LoadAOT — never
    // called in this ship because LoadAOT returns -1 → backend stays at
    // XGBOOST → Model_Predict's existing dispatch routes to C API.
    (void)m; (void)features; (void)num_features;
    return 0.0f;
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
        // v5.12.3.A — composite-signal extraction. When num_classes_active > 1,
        // returns linear combination over target_classes[]. Out-of-range
        // class indices contribute 0 (defensive). When num_classes_active == 1
        // (default), falls through to the single-class path which is bytewise-
        // equivalent to v5.11.62 behavior.
        if (m->num_classes_active > 1) {
            float composite = 0.0f;
            uint8_t n = m->num_classes_active;
            if (n > 8) n = 8;
            for (uint8_t i = 0; i < n; ++i) {
                int cls = m->target_classes[i];
                if (cls < 0 || (unsigned long)cls >= out_len) continue;
                composite += m->class_weights[i] * out_result[cls];
            }
            return composite;
        }
        // v5.11.62 — for multiclass models (out_len > 1), return the
        // configured "buy class" probability instead of out_result[0].
        // Default buy_class_idx=0 preserves binary semantics; loader sets
        // buy_class_idx=1 when aliasing PEAK_VALLEY_STABLE 3-class barrier
        // handles (class 1 = peak = price expected to rise = buy signal).
        // Out-of-range index falls back to 0 (defensive).
        int idx = m->buy_class_idx;
        if (idx < 0 || (unsigned long)idx >= out_len) idx = 0;
        return out_result[idx];
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
// [v5.10.0a.G.4 — ENSEMBLE PREDICT (multi-horizon)]
//======================================================================================================
// Predict via N independent ModelHandles trained as a multi-horizon
// ensemble. For each loaded model, compute prediction; return the
// HIGHEST-ABSOLUTE-DEVIATION-FROM-NEUTRAL prediction (the most-confident
// signal). Neutral = 0.5 for binary; this rule generalizes to "the
// model that's surest about its prediction wins."
//
// Selection logic:
//   - Binary models: distance from 0.5 = confidence; argmax(|p - 0.5|)
//   - Regression: |p| as confidence proxy (zero = no edge)
//
// Returns:
//   - The selected member's raw prediction value
//   - *out_selected_idx (optional): which member won (0..count-1)
//
// Operator-side workflow:
//   1. Train N horizons via Train Multi-Horizon (G.1)
//   2. Engine boot loads N models per role into EnsembleModelZoo (G.3)
//   3. Per slow-path predict, ensemble dispatch picks highest-confidence
//      model's output (this function)
//
// Latency: linear in N (each member predicts independently). Operator
// can measure via Item A timer; default cfg.horizon_count=0 keeps
// single-model path with zero overhead.
//
// Single-model fallback: if count <= 1, returns Model_Predict on
// models[0]. Safe to call from MLStrategy regardless of ensemble state.
template <unsigned F>
inline float Model_Predict_Ensemble(ModelHandle<F> *models,
                                      int count,
                                      const float *features,
                                      int num_features,
                                      int *out_selected_idx = nullptr) {
    if (count <= 0) {
        if (out_selected_idx) *out_selected_idx = -1;
        return 0.0f;
    }
    if (count == 1) {
        if (out_selected_idx) *out_selected_idx = 0;
        return Model_Predict(&models[0], features, num_features);
    }

    float best_pred = 0.0f;
    float best_conf = -1.0f;  // sentinel: "no valid prediction yet"
    int   best_idx = 0;
    for (int i = 0; i < count; ++i) {
        if (!Model_IsLoaded(&models[i])) continue;
        float p = Model_Predict(&models[i], features, num_features);
        if (std::isnan(p) || std::isinf(p)) continue;
        // Confidence = |p - 0.5| for binary (centered at neutral), or
        // |p| for regression (zero = no edge). Both metrics: higher
        // value = more-confident model.
        float conf = (p > 1.0f || p < -1.0f) ? std::fabs(p)
                                              : std::fabs(p - 0.5f);
        if (conf > best_conf) {
            best_conf = conf;
            best_pred = p;
            best_idx  = i;
        }
    }
    if (best_conf < 0.0f) {
        // No member produced a valid prediction; fall back to model[0]
        // raw output (matches single-model failure mode).
        if (out_selected_idx) *out_selected_idx = 0;
        return Model_Predict(&models[0], features, num_features);
    }
    if (out_selected_idx) *out_selected_idx = best_idx;
    return best_pred;
}

//======================================================================================================
// [v5.10.0a.G.7 — WEIGHTED ENSEMBLE PREDICT (Bandit-Exp3 blend)]
//======================================================================================================
// Run prediction across N independent boosters; combine via weighted
// blend: final = Σ weight_i × pred_i / Σ weight_i (NaN-skipped + agreement-
// gated). Replaces G.4's argmax-confidence selection when operator sets
// ensemble_blend_mode=weighted (default).
//
// Inputs:
//   models: array of N independent ModelHandles (caller's responsibility
//           to ensure they share the same scaler — true by G.3 LoadFromCfg
//           invariant)
//   count: how many models populated (1..ENSEMBLE_HORIZON_MAX)
//   weights: per-arm weights from BanditState (already-normalized
//            probabilities, OR raw weights — function renormalizes)
//   disabled_mask: bit i set = skip horizon i (operator kill-switch via
//                  cfg.core_N_disabled_horizons, parsed by
//                  EnsembleModelZoo_SetDisabledHorizons)
//   min_agreement_pct: ≥X fraction of non-disabled horizons must predict
//                       same direction OR return 0.5 (no-edge sentinel,
//                       MLStrategy treats as no-entry). 0.0 = disabled.
//
// Outputs:
//   *out_dominant_idx: which arm contributed most to signal direction
//                       (argmax weight × |p − 0.5|); -1 if no entry
//   Returns: blended prediction, OR 0.5 if agreement check failed.
//
// Latency: linear in N (each model predicts independently). G.7 perf
// optimization #1: features are pre-standardized once before this call;
// each Model_Predict skips its own scaler.
template <unsigned F>
inline float Model_Predict_Ensemble_Weighted(
    ModelHandle<F>* models,
    int count,
    const float* features,
    int num_features,
    const double* weights,
    uint32_t disabled_mask,
    double min_agreement_pct,
    int* out_dominant_idx,
    float* out_per_arm_predictions = nullptr) {  // v5.10.0a.G.8: optional buffer for reward record
    if (count <= 0) {
        if (out_dominant_idx) *out_dominant_idx = -1;
        return 0.0f;
    }
    if (count == 1) {
        if (disabled_mask & 1u) {
            if (out_dominant_idx) *out_dominant_idx = -1;
            return 0.5f;
        }
        if (out_dominant_idx) *out_dominant_idx = 0;
        return Model_Predict(&models[0], features, num_features);
    }

    // Local buffers sized to match ENSEMBLE_HORIZON_MAX (CoreModelZoo.hpp);
    // hardcoded 8 here to avoid circular include (this header is included
    // by CoreModelZoo.hpp).
    float predictions[8];
    int   valid[8];
    if (count > 8) count = 8;  // bound safety
    int   n_active = 0;
    int   n_long = 0, n_short = 0;
    for (int i = 0; i < count; ++i) {
        if (disabled_mask & (1u << i)) {
            valid[i] = 0;
            predictions[i] = 0.5f;
            continue;
        }
        if (!Model_IsLoaded(&models[i])) {
            valid[i] = 0;
            predictions[i] = 0.5f;
            continue;
        }
        float p = Model_Predict(&models[i], features, num_features);
        if (std::isnan(p) || std::isinf(p)) {
            valid[i] = 0;
            predictions[i] = 0.5f;
            continue;
        }
        valid[i] = 1;
        predictions[i] = p;
        n_active++;
        if (p > 0.5f) n_long++;
        else if (p < 0.5f) n_short++;
    }

    // Agreement filter (G.7 #5b safety check). Skips entry when ensemble
    // is internally split — high-conviction entries only.
    if (n_active > 0 && min_agreement_pct > 0.0) {
        double frac_long  = (double)n_long  / n_active;
        double frac_short = (double)n_short / n_active;
        double agreement  = (frac_long > frac_short) ? frac_long : frac_short;
        if (agreement < min_agreement_pct) {
            if (out_dominant_idx) *out_dominant_idx = -1;
            return 0.5f;  // no-edge sentinel; MLStrategy → no entry
        }
    }

    // Weighted blend across valid arms.
    double sum_w = 0.0, sum_wp = 0.0;
    double best_contrib = 0.0;
    int    best_idx = -1;
    for (int i = 0; i < count; ++i) {
        if (!valid[i]) continue;
        double w = weights[i];
        if (w <= 0.0) w = 1e-9;  // avoid zero-weight degenerate
        sum_w  += w;
        sum_wp += w * (double)predictions[i];
        // Track dominant arm: largest weight × |p - 0.5| contribution
        double contrib = w * std::fabs((double)predictions[i] - 0.5);
        if (contrib > best_contrib) {
            best_contrib = contrib;
            best_idx = i;
        }
    }
    if (sum_w <= 0.0 || n_active == 0) {
        // All-NaN or all-disabled: no signal. Fall back to first-loaded
        // model's raw predict for robustness (matches single-model failure
        // mode); if even that fails caller sees 0.0 / NaN.
        if (out_dominant_idx) *out_dominant_idx = -1;
        return 0.5f;
    }
    if (out_dominant_idx) *out_dominant_idx = best_idx;
    // v5.10.0a.G.8 — expose per-arm predictions for reward record write
    if (out_per_arm_predictions) {
        for (int i = 0; i < count; ++i)
            out_per_arm_predictions[i] = predictions[i];
    }
    return (float)(sum_wp / sum_w);
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
    char     engine_version[16];     // v5.8.6: SemVer string at training time, "" if absent
    int      stamp_format_version;   // v5.9.0: schema version of the stamp body itself.
                                     //         0 if absent (v5.8.x and older).
                                     //         1 = current (v5.9.0+).
                                     //         Verifier rejects unknown versions in strict mode.
    // v5.9.2b — inference-affecting cfg fields stamped at training time.
    // Verifier compares against current cfg; mismatch triggers
    // 3-tier strict-mode behavior (refuse / warn / surfaced).
    // Field flags = 1 when stamp had the field; 0 = absent (skip check).
    uint8_t  has_inference_cfg;                    // 1 if any inference_cfg_* present
    double   inference_cfg_confidence_threshold_scale;
    int      inference_cfg_barrier_gate_enabled;
    double   inference_cfg_confidence_hard_block_threshold;
    double   inference_cfg_held_out_fraction;
    double   inference_cfg_freshness_tau;
    uint8_t  has_inference_cfg_bandit;             // 1 if bandit_blend_ratio present
    double   inference_cfg_bandit_blend_ratio;
    uint8_t  has_inference_cfg_fees;               // 1 if fee_rate_* fields present
    double   inference_cfg_fee_rate_maker;
    double   inference_cfg_fee_rate_taker;
    uint8_t  has_training_poll_interval;
    uint32_t training_poll_interval;
    int      inference_cfg_drift_count;            // 0 if all match; >0 = mismatched fields
    // v5.9.2b — cross-major engine version detection. Set to 1 when stamp's
    // engine_version differs by major number from current build's
    // ENGINE_VERSION_STRING (e.g. stamp says 5.9.0 but engine is 6.0.0).
    // Caller (CoreModelZoo) refuses load when:
    //   cross_major_engine==1 AND !cfg.allow_cross_major_engine
    // Within-major (5.7 → 5.9) always allowed.
    uint8_t  cross_major_engine;
    // v5.9.3a — scaler sidecar binding. has_scaler_fields=1 when stamp
    // contains feature_scaler_present + scaler_sha256 lines (v5.9.3+
    // stamps); =0 means legacy stamp (no scaler claimed). Distinct from
    // feature_scaler_present (=1 only when scaler is actually present
    // for the model). Backward compat: legacy stamps load with
    // has_scaler_fields=0 + feature_scaler_present=0.
    uint8_t  has_scaler_fields;
    uint8_t  feature_scaler_present;        // 1 = sidecar exists at <model>.scaler
    char     scaler_sha256[65];             // SHA-256 hex of full sidecar file
    // v5.9.4a — model num_outputs (output dimension) stamp binding.
    // has_model_num_outputs=1 when stamp had the field; legacy stamps
    // load with 0 (no check fired). Verifier compares against
    // ModelHandle.num_outputs at CoreModelZoo load time.
    uint8_t  has_model_num_outputs;
    int      model_num_outputs;
    // v5.9.5h — XGBoost training hyperparams (parsed from stamp body
    // position 17). has_xgb_hyperparams=0 for legacy stamps; loader
    // skips comparison.
    uint8_t  has_xgb_hyperparams;
    int      xgb_max_depth;
    double   xgb_learning_rate;
    int      xgb_n_estimators;
    double   xgb_subsample;
    double   xgb_colsample_bytree;
    int      xgb_min_child_weight;
    int      xgb_seed;
    char     xgb_tree_method[16];
    // v5.9.5h Phase 10 — build flags fingerprint
    uint8_t  has_build_flags_hash;
    uint64_t build_flags_hash;
    // v5.10.0a.G.2 — multi-horizon ensemble member count (parsed from stamp).
    // See StampInferenceCfgInputs above for canonical position 19 anchor.
    uint8_t  has_grid_member_count;
    int      grid_member_count;
    int      grid_member_idx;
    // v5.10.0d — label registry hash (canonical position 20). 0 if absent
    // (stamps from before v5.10.0d). Verifier compares against current
    // build's LABEL_REGISTRY_HASH() — mismatch refuses load with
    // "label set drift; retrain required" message. Mirrors v5.8.6
    // feature_registry_hash refusal flow.
    uint8_t  has_label_registry_hash;
    uint64_t label_registry_hash;
    // v5.11.18a — per-core feature_mask binding (stamp-side anchor for the
    // runtime cfg field at ControllerConfig::core_feature_mask[16]). The
    // stamp persists ONE mask (the training-time mask, which must equal
    // the cfg mask of the core that produced this model). v5.11.18a only
    // emits + verifies this field when the mask differs from the all-on
    // default — legacy stamps + default-cfg-trained models load with
    // has_feature_mask=0 (skip check), preserving backward compat.
    //
    // Verifier compares stamp's feature_mask_train against the runtime
    // cfg's per-core mask (caller passes which core is loading) and
    // refuses on mismatch in strict mode. v5.11.18a writes infrastructure
    // only; v5.11.18 wires Features_PackAll to actually act on the mask.
    uint8_t  has_feature_mask;
    uint64_t feature_mask_train;
    // v5.11.41 — per-horizon label parameters (parsed from stamp body
    // canonical position 22). Forensic record; no load-time refusal.
    // Operator can read these via stamp_inspect.sh + grep to identify
    // which horizon a stamped model belongs to.
    uint8_t  has_label_params;
    int      label_lookahead_ticks;
    double   label_tp_pct;
    double   label_sl_pct;
    // v5.11.41 — XGBoost training thread count (canonical position 23).
    // Forensic; lets operator post-hoc identify which mode (serial
    // vs parallel multi-horizon) produced a given stamp.
    uint8_t  has_xgb_train_nthread;
    int      xgb_train_nthread;

    // v5.14.1.B.3 — X-macro-driven stamp-bound cfg fields (canonical
    // positions 24+). Auto-generated from FOREACH_STAMP_BOUND_CFG.
    // Each field has a uint8_t has_<name> Surface G forward-compat
    // flag + the typed value field. Adding the next field is ONE line
    // in StampBoundCfgRegistry.hpp.
    #define X(name, type, fmt, default_val, get_cfg_expr, emit_when)  \
        uint8_t has_##name;                                             \
        type name;
    FOREACH_STAMP_BOUND_CFG(X)
    #undef X

    // v5.14.2.E.2.B — model-architectural fields migrated from expected.cfg
    // sidecar to stamp body. NOT cfg-bound (these come from training-time
    // model-architectural context: label_kind → num_classes; operator's
    // role choice; build-time MODEL_NUM_FEATURES + MODEL_FORMAT_VERSION
    // constants). Manual emit + parse pattern (don't fit FOREACH_STAMP_BOUND_CFG
    // which is for cfg-only fields). When count grows to 5+, refactor to
    // FOREACH_STAMP_BOUND_MODEL_CONST registry (TECH_DEBT.md entry).
    //
    // **If you add a NEW architectural field here, ALSO update:**
    //   - StampInferenceCfgInputs (~line 1940; emit-side struct)
    //   - verify_model_stamp init block (zero has_*)
    //   - verify_model_stamp parser (add `if (strcmp(key, "...") == 0)` branch)
    //   - stamp_write_for_model emit (add `if (inf->has_*)` block)
    //   - BacktestEngine.hpp populator (add `inf.has_*=1; inf.*=value;`)
    //   - tools/stamp_model.sh CLI emit (TECH_DEBT-tracked catch-up)
    //   - CoreModelZoo_VerifyExpected dual-path read (use stamp if has_*, else expected.cfg)
    uint8_t  has_expected_num_classes;
    int      expected_num_classes;             // 0=binary, 1=regression, ≥2=multiclass
    uint8_t  has_expected_role;
    char     expected_role[16];                 // "buy_signal" | "barrier" | "regime" | "exit"
    uint8_t  has_expected_num_features;
    int      expected_num_features;             // = MODEL_NUM_FEATURES at training time
    uint8_t  has_expected_feature_format_version;
    int      expected_feature_format_version;   // = MODEL_FORMAT_VERSION at training time

    // v5.14.3.B — overlay-derived fields (3-layer fingerprinting).
    // SIDECAR-DERIVED (FeatureOverlay JSON → SHA256). NOT cfg-bound.
    // See StampInferenceCfgInputs (below) for the discipline list of sites
    // that must stay in sync when adding a new sidecar-derived field.
    uint8_t  has_overlay_hash;
    char     overlay_hash[65];                  // layer-2: SHA256 of canonical overlay JSON
    uint8_t  has_effective_hash;
    char     effective_hash[65];                // layer-3: SHA256(layer1 || layer2)
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
                                            uint64_t expected_feature_registry_hash = 0,
                                            uint64_t expected_label_registry_hash = 0,
                                            // v5.11.18a — feature_mask of the core
                                            // loading the model. 0 = skip check
                                            // (legacy callers + default mask).
                                            uint64_t expected_feature_mask = 0) {
    ModelStampResult r;
    r.valid = -1;
    r.reason[0] = '\0';
    r.model_format_version = 0;
    r.generalization_gap = 0.0;
    r.gap_threshold = gap_threshold;
    r.feature_registry_hash = 0;
    r.engine_version[0] = '\0';
    r.stamp_format_version = 0;  // v5.9.0: 0 = absent (legacy stamp)
    // v5.9.2b — inference cfg fields. has_* flags = 0 until parser sets them.
    r.has_inference_cfg = 0;
    r.inference_cfg_confidence_threshold_scale = 0.0;
    r.inference_cfg_barrier_gate_enabled = 0;
    r.inference_cfg_confidence_hard_block_threshold = 0.0;
    r.inference_cfg_held_out_fraction = 0.0;
    r.inference_cfg_freshness_tau = 0.0;
    r.has_inference_cfg_bandit = 0;
    r.inference_cfg_bandit_blend_ratio = 0.0;
    r.has_inference_cfg_fees = 0;
    r.inference_cfg_fee_rate_maker = 0.0;
    r.inference_cfg_fee_rate_taker = 0.0;
    r.has_training_poll_interval = 0;
    r.training_poll_interval = 0;
    r.inference_cfg_drift_count = 0;
    r.cross_major_engine = 0;
    // v5.9.3a — scaler fields. has_scaler_fields = 1 if stamp had any
    // scaler key; feature_scaler_present = 1 only if stamp claims the
    // sidecar exists. Legacy stamps load with both = 0 (forward-compat).
    r.has_scaler_fields = 0;
    r.feature_scaler_present = 0;
    r.scaler_sha256[0] = '\0';
    // v5.9.4a — model num_outputs init.
    r.has_model_num_outputs = 0;
    r.model_num_outputs = 0;
    // v5.9.5h — XGBoost hyperparam fields zero-init
    r.has_xgb_hyperparams = 0;
    r.xgb_max_depth = 0;
    r.xgb_learning_rate = 0.0;
    r.xgb_n_estimators = 0;
    r.xgb_subsample = 0.0;
    r.xgb_colsample_bytree = 0.0;
    r.xgb_min_child_weight = 0;
    r.xgb_seed = 0;
    r.xgb_tree_method[0] = '\0';
    r.has_build_flags_hash = 0;
    r.build_flags_hash = 0;
    // v5.10.0a.G.2 — grid_member_count fields zero-init
    r.has_grid_member_count = 0;
    r.grid_member_count = 0;
    r.grid_member_idx = 0;
    // v5.10.0d — label_registry_hash zero-init (absent in legacy stamps)
    r.has_label_registry_hash = 0;
    r.label_registry_hash = 0;
    // v5.11.18a — feature_mask zero-init (absent in legacy stamps + in
    // v5.11.18a stamps trained with the all-on default mask). Caller's
    // load-time check skips when has_feature_mask=0.
    r.has_feature_mask = 0;
    r.feature_mask_train = 0;
    // v5.11.41 — per-horizon label params + xgb_train_nthread zero-init.
    // Legacy stamps (pre-v5.11.41) load with has_*=0 → fields skipped.
    r.has_label_params = 0;
    r.label_lookahead_ticks = 0;
    r.label_tp_pct = 0.0;
    r.label_sl_pct = 0.0;
    r.has_xgb_train_nthread = 0;
    r.xgb_train_nthread = 0;

    // v5.14.1.B.3 — X-macro-driven zero-init for stamp-bound cfg fields.
    // Legacy stamps (pre-v5.14.1.B.3) load with has_<name>=0 → drift
    // check at caller site skips silently. New stamps populate via the
    // parser branch below.
    #define X(name, type, fmt, default_val, get_cfg_expr, emit_when)  \
        r.has_##name = 0;                                               \
        r.name = (type)(default_val);
    FOREACH_STAMP_BOUND_CFG(X)
    #undef X

    // v5.14.2.E.2.B — model-architectural fields zero-init. Legacy stamps
    // (pre-v5.14.2.E.2) load with has_*=0 → caller (CoreModelZoo_VerifyExpected)
    // falls back to expected.cfg sidecar lookup.
    r.has_expected_num_classes = 0;
    r.expected_num_classes = 0;
    r.has_expected_role = 0;
    r.expected_role[0] = '\0';
    r.has_expected_num_features = 0;
    r.expected_num_features = 0;
    r.has_expected_feature_format_version = 0;
    r.expected_feature_format_version = 0;

    // v5.14.3.B — overlay-derived fields zero-init. Legacy stamps
    // (pre-v5.14.3) load with has_*=0 → FeatureOverlay_PostLoadVerify
    // skips silently (no overlay verification claimed).
    r.has_overlay_hash = 0;
    r.overlay_hash[0] = '\0';
    r.has_effective_hash = 0;
    r.effective_hash[0] = '\0';

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
                r.generalization_gap = tt::parse_double_fast(val);
            } else if (strcmp(key, "gap_threshold") == 0) {
                r.gap_threshold = tt::parse_double_fast(val);
            } else if (strcmp(key, "feature_registry_hash") == 0) {
                // v5.8.1a: parse hex-encoded 64-bit hash. strtoull accepts
                // 0x-prefix or bare hex. Stamp emits %016lx (no prefix).
                r.feature_registry_hash = (uint64_t)strtoull(val, nullptr, 16);
            } else if (strcmp(key, "engine_version") == 0) {
                // v5.8.6: SemVer string captured at training time (e.g. "5.8.5").
                // Empty / missing for stamps written by pre-v5.8.6 callers.
                size_t vl = strlen(val);
                if (vl >= sizeof(r.engine_version)) vl = sizeof(r.engine_version) - 1;
                memcpy(r.engine_version, val, vl);
                r.engine_version[vl] = '\0';
            } else if (strcmp(key, "stamp_format_version") == 0) {
                // v5.9.0: stamp body schema version. 0 means absent (legacy);
                // current = 1. Future schema changes bump this. Verifier
                // could reject unknown versions in strict mode (deferred to
                // a future ship; for now we just record the value).
                r.stamp_format_version = atoi(val);
            }
            // v5.9.2b — inference-affecting cfg fields. Each present field
            // sets the relevant has_* flag. Verifier compares against
            // current cfg later (caller-side).
            else if (strcmp(key, "inference_cfg_confidence_threshold_scale") == 0) {
                r.inference_cfg_confidence_threshold_scale = tt::parse_double_fast(val);
                r.has_inference_cfg = 1;
            } else if (strcmp(key, "inference_cfg_barrier_gate_enabled") == 0) {
                r.inference_cfg_barrier_gate_enabled = atoi(val);
                r.has_inference_cfg = 1;
            } else if (strcmp(key, "inference_cfg_confidence_hard_block_threshold") == 0) {
                r.inference_cfg_confidence_hard_block_threshold = tt::parse_double_fast(val);
                r.has_inference_cfg = 1;
            } else if (strcmp(key, "inference_cfg_held_out_fraction") == 0) {
                r.inference_cfg_held_out_fraction = tt::parse_double_fast(val);
                r.has_inference_cfg = 1;
            } else if (strcmp(key, "inference_cfg_freshness_tau") == 0) {
                r.inference_cfg_freshness_tau = tt::parse_double_fast(val);
                r.has_inference_cfg = 1;
            } else if (strcmp(key, "inference_cfg_bandit_blend_ratio") == 0) {
                r.inference_cfg_bandit_blend_ratio = tt::parse_double_fast(val);
                r.has_inference_cfg_bandit = 1;
            } else if (strcmp(key, "inference_cfg_fee_rate_maker") == 0) {
                r.inference_cfg_fee_rate_maker = tt::parse_double_fast(val);
                r.has_inference_cfg_fees = 1;
            } else if (strcmp(key, "inference_cfg_fee_rate_taker") == 0) {
                r.inference_cfg_fee_rate_taker = tt::parse_double_fast(val);
                r.has_inference_cfg_fees = 1;
            } else if (strcmp(key, "training_poll_interval") == 0) {
                r.training_poll_interval = (uint32_t)strtoul(val, nullptr, 10);
                r.has_training_poll_interval = 1;
            }
            // v5.9.3a — scaler fields
            else if (strcmp(key, "feature_scaler_present") == 0) {
                r.feature_scaler_present = (atoi(val) != 0) ? 1 : 0;
                r.has_scaler_fields = 1;
            } else if (strcmp(key, "scaler_sha256") == 0) {
                size_t vl = strlen(val);
                if (vl >= sizeof(r.scaler_sha256)) vl = sizeof(r.scaler_sha256) - 1;
                memcpy(r.scaler_sha256, val, vl);
                r.scaler_sha256[vl] = '\0';
                r.has_scaler_fields = 1;
            }
            // v5.9.4a — model_num_outputs (output dimension binding)
            else if (strcmp(key, "model_num_outputs") == 0) {
                r.model_num_outputs = atoi(val);
                r.has_model_num_outputs = 1;
            }
            // v5.9.5h — XGBoost hyperparam parsing. Macro-expanded
            // to keep the 8-field dispatch tight (vs an if/else
            // chain). Each macro expands to one `else if` clause
            // continuing the existing chain.
            #define PARSE_XGB_INT(field) \
                else if (strcmp(key, "xgb_" #field) == 0) { \
                    r.xgb_##field = atoi(val); \
                    r.has_xgb_hyperparams = 1; \
                }
            #define PARSE_XGB_DOUBLE(field) \
                else if (strcmp(key, "xgb_" #field) == 0) { \
                    r.xgb_##field = tt::parse_double_fast(val); \
                    r.has_xgb_hyperparams = 1; \
                }
            PARSE_XGB_INT(max_depth)
            PARSE_XGB_DOUBLE(learning_rate)
            PARSE_XGB_INT(n_estimators)
            PARSE_XGB_DOUBLE(subsample)
            PARSE_XGB_DOUBLE(colsample_bytree)
            PARSE_XGB_INT(min_child_weight)
            PARSE_XGB_INT(seed)
            // tree_method is a fixed-size string; inline (no macro).
            else if (strcmp(key, "xgb_tree_method") == 0) {
                size_t vl = strlen(val);
                if (vl >= sizeof(r.xgb_tree_method)) vl = sizeof(r.xgb_tree_method) - 1;
                memcpy(r.xgb_tree_method, val, vl);
                r.xgb_tree_method[vl] = '\0';
                r.has_xgb_hyperparams = 1;
            }
            #undef PARSE_XGB_INT
            #undef PARSE_XGB_DOUBLE
            // v5.9.5h Phase 10 — build flags hash (hex parse)
            else if (strcmp(key, "build_flags_hash") == 0) {
                r.build_flags_hash = (uint64_t)strtoull(val, nullptr, 16);
                r.has_build_flags_hash = 1;
            }
            // v5.10.0a.G.2 — multi-horizon ensemble member count (position 19)
            else if (strcmp(key, "grid_member_count") == 0) {
                r.grid_member_count = atoi(val);
                r.has_grid_member_count = 1;
            }
            else if (strcmp(key, "grid_member_idx") == 0) {
                r.grid_member_idx = atoi(val);
                r.has_grid_member_count = 1;
            }
            // v5.10.0d — label registry hash (position 20)
            else if (strcmp(key, "label_registry_hash") == 0) {
                r.label_registry_hash = (uint64_t)strtoull(val, nullptr, 16);
                r.has_label_registry_hash = 1;
            }
            // v5.11.18a — feature mask (position 21). Hex-encoded uint64
            // bitmap of features active during training. Stamp emits
            // %016lx (no prefix). Verifier compares against runtime cfg
            // mask in 3-tier strict-mode. Legacy stamps (no field) load
            // with has_feature_mask=0 → check skipped.
            else if (strcmp(key, "feature_mask") == 0) {
                r.feature_mask_train = (uint64_t)strtoull(val, nullptr, 16);
                r.has_feature_mask = 1;
            }
            // v5.11.41 — per-horizon label params (canonical position 22).
            else if (strcmp(key, "label_lookahead_ticks") == 0) {
                r.label_lookahead_ticks = atoi(val);
                r.has_label_params = 1;
            }
            else if (strcmp(key, "label_tp_pct") == 0) {
                r.label_tp_pct = tt::parse_double_fast(val);
            }
            else if (strcmp(key, "label_sl_pct") == 0) {
                r.label_sl_pct = tt::parse_double_fast(val);
            }
            // v5.11.41 — XGBoost train-time thread count (position 23).
            else if (strcmp(key, "xgb_train_nthread") == 0) {
                r.xgb_train_nthread = atoi(val);
                r.has_xgb_train_nthread = 1;
            }
            // v5.14.1.B.3 — X-macro-driven parser branches (positions 24+).
            // Each X expands to one `else if (strcmp(key, "<name>") == 0)`
            // branch that uses the type-dispatched STAMP_CFG_PARSE macro.
            #define X(name, type, fmt, default_val, get_cfg_expr, emit_when)  \
                else if (strcmp(key, #name) == 0) {                            \
                    r.name = (type)(STAMP_CFG_PARSE(type, val));               \
                    r.has_##name = 1;                                          \
                }
            FOREACH_STAMP_BOUND_CFG(X)
            #undef X
            // v5.14.2.E.2.B — model-architectural fields (manual parser branches;
            // not in FOREACH_STAMP_BOUND_CFG since they're not cfg-bound).
            else if (strcmp(key, "expected_num_classes") == 0) {
                r.expected_num_classes = atoi(val);
                r.has_expected_num_classes = 1;
            }
            else if (strcmp(key, "expected_role") == 0) {
                strncpy(r.expected_role, val, sizeof(r.expected_role) - 1);
                r.expected_role[sizeof(r.expected_role) - 1] = '\0';
                r.has_expected_role = 1;
            }
            else if (strcmp(key, "expected_num_features") == 0) {
                r.expected_num_features = atoi(val);
                r.has_expected_num_features = 1;
            }
            else if (strcmp(key, "expected_feature_format_version") == 0) {
                r.expected_feature_format_version = atoi(val);
                r.has_expected_feature_format_version = 1;
            }
            // v5.14.3.B — overlay-derived fields parser branches.
            else if (strcmp(key, "overlay_hash") == 0) {
                strncpy(r.overlay_hash, val, sizeof(r.overlay_hash) - 1);
                r.overlay_hash[sizeof(r.overlay_hash) - 1] = '\0';
                r.has_overlay_hash = 1;
            }
            else if (strcmp(key, "effective_hash") == 0) {
                strncpy(r.effective_hash, val, sizeof(r.effective_hash) - 1);
                r.effective_hash[sizeof(r.effective_hash) - 1] = '\0';
                r.has_effective_hash = 1;
            }
        }
        line = strtok_r(nullptr, "\n", &save);
    }

    // v5.9.2b — cross-major engine version detection. Compare stamp's
    // engine_version major against current build's ENGINE_VERSION_STRING
    // major. Empty stamp engine_version (pre-v5.8.6) → skip (allow).
    // Major = atoi() of the prefix before first '.' — works for "5.9.2",
    // "5.9.2a", "v5.9.2", or any leading-int form.
    r.cross_major_engine = 0;
    if (r.engine_version[0] != '\0') {
        const char* sv = r.engine_version;
        if (sv[0] == 'v' || sv[0] == 'V') sv++;  // accept v-prefix
        int stamp_major = atoi(sv);
        const char* cur = ENGINE_VERSION_STRING;
        if (cur[0] == 'v' || cur[0] == 'V') cur++;
        int cur_major = atoi(cur);
        if (stamp_major != cur_major && stamp_major > 0 && cur_major > 0) {
            r.cross_major_engine = 1;
        }
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
    // expected_feature_registry_hash != 0, the stamp's hash must match
    // (catches train-serve drift). Default 0 = "skip check" (caller
    // explicitly opts out). v5.8.6: when stamp has NO hash field
    // (pre-v5.8.1a stamps parse as 0), accept with stderr WARN rather
    // than reject — preserves back-compat with legacy models. Drift catch
    // fires only when BOTH sides have the data and they disagree.
    if (expected_feature_registry_hash != 0) {
        if (r.feature_registry_hash == 0) {
            fprintf(stderr,
                "[stamp] WARN: %s stamp lacks feature_registry_hash "
                "(pre-v5.8.1a) — drift NOT verified\n",
                stamp_path);
        } else if (r.feature_registry_hash != expected_feature_registry_hash) {
            r.valid = 0;
            snprintf(r.reason, sizeof(r.reason),
                "feature-registry-hash mismatch: stamp=%016lx engine=%016lx "
                "(retrain required)",
                (unsigned long)r.feature_registry_hash,
                (unsigned long)expected_feature_registry_hash);
            return r;
        }
    }

    // 1c. v5.10.0d — label registry hash match. Same shape as 1b but for
    // the LABEL_REGISTRY_HASH (FOREACH_TARGET X-macro). Caller passes
    // expected_label_registry_hash from LABEL_REGISTRY_HASH() at engine
    // boot. Default 0 = "skip check" (legacy callers + non-ML cores).
    // Pre-v5.10.0d stamps lack the field (parses as 0) → WARN, accept.
    // Drift catch fires only when both sides have the data and disagree.
    if (expected_label_registry_hash != 0) {
        if (r.label_registry_hash == 0) {
            fprintf(stderr,
                "[stamp] WARN: %s stamp lacks label_registry_hash "
                "(pre-v5.10.0d) — label drift NOT verified\n",
                stamp_path);
        } else if (r.label_registry_hash != expected_label_registry_hash) {
            r.valid = 0;
            snprintf(r.reason, sizeof(r.reason),
                "label-registry-hash mismatch: stamp=%016lx engine=%016lx "
                "(label set drift; retrain required)",
                (unsigned long)r.label_registry_hash,
                (unsigned long)expected_label_registry_hash);
            return r;
        }
    }

    // 1d. v5.11.18a — feature_mask match. Caller passes the runtime cfg's
    // per-core mask for the core loading this model. Default 0 = skip
    // check. Pre-v5.11.18a stamps lack the field (parses as 0) → caller
    // can decide WARN vs accept based on operator strictness; here we
    // WARN-and-accept by default (informational; behavior change is
    // v5.11.18 territory). When both sides have the data and disagree,
    // refuse — masked-feature drift is a parity-critical failure mode
    // (CRITICAL gap from /parity-check 2026-05-07).
    if (expected_feature_mask != 0) {
        if (!r.has_feature_mask) {
            fprintf(stderr,
                "[stamp] WARN: %s stamp lacks feature_mask "
                "(pre-v5.11.18a) — feature-mask drift NOT verified\n",
                stamp_path);
        } else if (r.feature_mask_train != expected_feature_mask) {
            r.valid = 0;
            snprintf(r.reason, sizeof(r.reason),
                "feature_mask mismatch: stamp=%016lx engine=%016lx "
                "(per-core feature subset drift; retrain or restore "
                "feature_mask cfg to training-time value)",
                (unsigned long)r.feature_mask_train,
                (unsigned long)expected_feature_mask);
            return r;
        }
    }

    // 2. Gap acceptable. v5.9.5j sentinel: gap_threshold == 0.0 + held_out
    // == 0.0 means "training-only stamp" (Train Model auto-stamp without
    // held-out). Skip the gap check for these stamps; they're info-grade
    // not deploy-grade. Operator wanting deploy validation runs Run Full
    // Validation which produces a full stamp.
    bool training_only_stamp = (r.gap_threshold == 0.0);
    if (!training_only_stamp && r.generalization_gap > r.gap_threshold) {
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

// v5.9.2b — inference cfg fields bound to the stamp at training time.
// Caller fills only the fields it has; has_* flags gate emit. Nullptr
// passed for legacy callers means none of these fields emit (forward-
// compat with v5.9.0/.1/.2 stamps).
struct StampInferenceCfgInputs {
    int      has_inference_cfg;                     // 1 = emit the 5 always-present cfg fields below
    double   confidence_threshold_scale;
    int      barrier_gate_enabled;
    double   confidence_hard_block_threshold;
    double   held_out_fraction;
    double   freshness_tau;                          // bound to stamp at training time
    int      has_bandit;                             // 1 = emit bandit_blend_ratio (when bandit_enabled cfg=1)
    double   bandit_blend_ratio;
    int      has_fees;                               // 1 = emit fee_rate_maker/taker (when cost_gate_enabled=1)
    double   fee_rate_maker;
    double   fee_rate_taker;
    int      has_training_poll_interval;             // 1 = emit training_poll_interval
    uint32_t training_poll_interval;
    // v5.9.3a — scaler sidecar binding fields. has_scaler=1 → emit
    // both feature_scaler_present + scaler_sha256 lines. scaler_sha256
    // is the SHA-256 of the FULL sidecar file (computed by trainer
    // post-Persist via sha256_file_hex_inproc).
    int      has_scaler;
    int      feature_scaler_present;                 // 0 = no sidecar; 1 = sidecar exists
    const char* scaler_sha256_hex;                   // null-terminated 64-char hex (or empty)
    // v5.9.4a — model num_outputs (output dimension) stamp binding.
    // Stamp records what the trainer SAW; verifier compares against
    // ModelHandle.num_outputs at load time. Binary/regression = 1,
    // multiclass = num_classes (e.g. 3 for PEAK_VALLEY_STABLE).
    // Catches "stamp claims 3-class but binary model loaded" bug.
    int      has_num_outputs;
    int      model_num_outputs;
    // v5.9.5h — XGBoost training hyperparams. Stamp body position 17
    // (canonical-order locked: appended after model_num_outputs at
    // position 16). Surface G has_*=0 forward-compat for legacy stamps.
    // Engine load-WARN compares these vs cfg.xgb_* at boot; mismatch
    // logs (no refuse — hyperparams don't affect inference, only
    // forensics + reproducibility). max_depth/lr/n_est are the
    // operator-tunable ones; subsample/colsample/min_child_weight/
    // seed/tree_method are the cfg-tunable ones (v5.9.5h Phase 2).
    int      has_xgb_hyperparams;
    int      xgb_max_depth;
    double   xgb_learning_rate;
    int      xgb_n_estimators;
    double   xgb_subsample;
    double   xgb_colsample_bytree;
    int      xgb_min_child_weight;
    int      xgb_seed;
    char     xgb_tree_method[16];
    int      has_build_flags_hash;
    uint64_t build_flags_hash;
    // v5.10.0a.G.2 — multi-horizon ensemble member count.
    //
    // CANONICAL STAMP BODY POSITION ASSIGNMENT (v5.10.0a):
    // Position 19: grid_member_count (this ship; locks order via Sprint B B2 ships first)
    // Position 20: label_registry_hash (v5.10.0d / Sprint B B5; ships after this)
    // New fields after v5.10.0a + v5.10.0d MUST take position 21+ per
    // master plan canonical-order rule (only append at end). Sprint
    // order is locked by master plan; do NOT reassign positions.
    //
    // grid_member_count = N where this stamp belongs to an ensemble of
    // N models trained as a horizon set (cfg.horizon_list non-empty at
    // train time). Single-horizon stamps have has_grid_member_count=0
    // (forward-compat; legacy stamps load fine).
    //
    // Engine load: when has_grid_member_count=1, load proceeds normally
    // BUT logs "[ensemble] this model is member <member_idx>/<count> of a
    // multi-horizon ensemble; consider loading siblings via cfg.horizon_list
    // for full ensemble inference." Operator-side hint only; no refuse.
    int      has_grid_member_count;
    int      grid_member_count;        // total members in the ensemble (N)
    int      grid_member_idx;          // this model's index within (0..N-1)
    // v5.10.0d — label registry hash (canonical position 20). Set
    // has_label_registry_hash=1 to emit; verifier compares stamp's value
    // vs current build's LABEL_REGISTRY_HASH() — mismatch refuses load.
    // Mirrors v5.8.6 feature_registry_hash refusal flow.
    int      has_label_registry_hash;
    uint64_t label_registry_hash;
    // v5.11.18a — feature_mask (canonical position 21). Per-core uint64
    // bitmap of which features were active at training time. Ship is
    // infrastructure-only — Features_PackAll still consumes ALL features
    // until v5.11.18 wires the mask through MLBuildContext. Surface G
    // has_*=0 forward-compat for legacy stamps + default-cfg trains
    // (where the mask is the all-on default 0xFFFFFFFFFFFFFFFF).
    //
    // Convention: emit only when caller explicitly passes a non-default
    // mask. This keeps stamps trained against the all-on default
    // bytewise-identical to pre-v5.11.18a stamps (the field is absent
    // from the canonical body, so the HMAC signature is unchanged).
    int      has_feature_mask;
    uint64_t feature_mask_train;
    // v5.11.41 — per-horizon label parameters (canonical position 22).
    // Forensic-only: verifier records into ModelStampResult so operator
    // can grep stamp file to identify which horizon produced this model.
    // No load-time refusal — engine has no "expected horizon" cfg-side
    // to compare against (multi-horizon ensemble auto-detects via dir
    // naming `_horizon_<H>` per v5.10.0a-final). Recording closes a
    // pre-existing schema gap surfaced by /parity-check 2026-05-07-stamp.
    int      has_label_params;
    int      label_lookahead_ticks;        // aka label_forward_ticks
    double   label_tp_pct;                  // 0.05 means 0.05% (BacktestRunConfig convention)
    double   label_sl_pct;
    // v5.11.41 — XGBoost train-time thread count (canonical position 23).
    // Forensic-only: lets operator post-hoc detect mode divergence
    // (serial mode = cfg.xgb_train_nthread; parallel multi-horizon
    // mode = pinned to 1 for bytewise determinism vs serial-with-1).
    // Recording closes /parity-check 2026-05-07-stamp CRITICAL-2.
    int      has_xgb_train_nthread;
    int      xgb_train_nthread;

    // v5.14.1.B.3 — X-macro-driven stamp-bound cfg fields (canonical
    // positions 24+). Auto-generated from FOREACH_STAMP_BOUND_CFG;
    // mirrors ModelStampResult (parser side). Production caller (e.g.
    // BacktestPanels' Train Model worker) populates has_<name>=1 +
    // value when cfg-side flag is enabled. Default 0 = legacy stamp
    // (emit nothing → byte-identical to pre-v5.14.1.B.3 stamps).
    #define X(name, type, fmt, default_val, get_cfg_expr, emit_when)  \
        int  has_##name;                                                \
        type name;
    FOREACH_STAMP_BOUND_CFG(X)
    #undef X

    // v5.14.2.E.2.B — model-architectural fields (mirror of ModelStampResult
    // additions; see ModelStampResult for the discipline list). NOT in
    // FOREACH_STAMP_BOUND_CFG (architectural, not cfg-bound).
    int      has_expected_num_classes;
    int      expected_num_classes;
    int      has_expected_role;
    char     expected_role[16];
    int      has_expected_num_features;
    int      expected_num_features;
    int      has_expected_feature_format_version;
    int      expected_feature_format_version;

    // v5.14.3.B — overlay-derived fields (3-layer fingerprinting).
    // SIDECAR-DERIVED (computed from FeatureOverlay JSON), NOT cfg-bound.
    // Don't fit FOREACH_STAMP_BOUND_CFG. Manual emit + parse pattern
    // (mirror v5.14.2.E.2.B precedent above).
    //
    // **TECH_DEBT trigger:** when sidecar-derived field count grows to 5+,
    // refactor to parallel `FOREACH_STAMP_BOUND_SIDECAR(X)` registry
    // (sister to TECH_DEBT-006's FOREACH_STAMP_BOUND_MODEL_CONST).
    //
    // **If you add a NEW sidecar-derived field here, ALSO update:**
    //   - ModelStampResult (above; parser-side struct)
    //   - verify_model_stamp init block (zero has_*)
    //   - verify_model_stamp parser (add `if (strcmp(key, "...") == 0)` branch)
    //   - stamp_write_for_model emit (add `if (inf->has_*)` block)
    //   - BacktestEngine.hpp populator (add `inf.has_*=1; inf.*=value;`)
    //   - tools/feature_overlay.py (Python emit-side mirror)
    //   - FeatureOverlay_PostLoadVerify helper (verification check)
    int      has_overlay_hash;
    char     overlay_hash[65];                  // SHA256 of canonical overlay JSON; 64 hex + null
    int      has_effective_hash;
    char     effective_hash[65];                // SHA256(layer1 || layer2); 64 hex + null
};

inline StampWriteResult stamp_write_for_model(const char* model_path,
                                                const char* secret,
                                                int   format_version,
                                                const char* trained_on_iso,  // YYYY-MM-DD
                                                double wf_mean_val,
                                                double held_out_metric,
                                                double gap_threshold,
                                                int   force,
                                                uint64_t feature_registry_hash = 0,
                                                const char* engine_version = nullptr,
                                                // v5.9.2b — inference cfg binding.
                                                // Optional; nullptr = skip emit (legacy callers).
                                                const StampInferenceCfgInputs* inf = nullptr) {
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
    //    held_out_metric, gap, gap_threshold, [feature_registry_hash],
    //    [engine_version].
    //    feature_registry_hash is appended ONLY when format_version >= 5
    //    AND a non-zero hash was supplied. engine_version is appended
    //    ONLY when format_version >= 5 AND a non-empty string was supplied.
    //    v4 stamps and dev-mode invocations omit both fields — verifier
    //    handles missing fields as "skip check" / empty.
    //    Each line ends with \n.
    int has_hash    = (format_version >= 5 && feature_registry_hash != 0);
    int has_engver  = (format_version >= 5 && engine_version && engine_version[0] != '\0');
    // v5.9.0: stamp_format_version=1 emitted whenever format_version >= 5
    // (the v5.8.1a+ wire-format era — the era that has feature_registry_hash
    // and engine_version). Schema version of the stamp body itself,
    // distinct from MODEL_FORMAT_VERSION (which versions the model file
    // shape, not the stamp). Bumped on future stamp body schema changes.
    int has_stamp_ver = (format_version >= 5);
    // v5.9.2b — bumped from 2048 → 4096. Original ~700 bytes; 9 new
    // inference_cfg_* + training_poll_interval fields × ~50 bytes each
    // = +450 bytes worst-case, well under the new ceiling. Leaves
    // headroom for v5.9.3 scaler fields too.
    char canonical[4096];
    int n = snprintf(canonical, sizeof(canonical),
        "model_format_version=%d\n"
        "model_sha256=%s\n"
        "trained_on=%s\n"
        "wf_mean_val=%g\n"
        "held_out_metric=%g\n"
        "gap=%.6f\n"
        "gap_threshold=%g\n",
        format_version, model_sha, trained_on_iso,
        wf_mean_val, held_out_metric, gap, gap_threshold);
    if (has_hash && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "feature_registry_hash=%016lx\n",
            (unsigned long)feature_registry_hash);
        if (wrote > 0) n += wrote;
    }
    if (has_engver && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "engine_version=%s\n", engine_version);
        if (wrote > 0) n += wrote;
    }
    if (has_stamp_ver && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "stamp_format_version=1\n");
        if (wrote > 0) n += wrote;
    }
    // v5.9.2b — inference cfg binding. Emitted only when caller passed
    // non-null `inf` pointer + respective has_* flag set. Verifier
    // parser tolerates absent fields (legacy stamps + no-bind callers).
    if (inf && inf->has_inference_cfg && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "inference_cfg_confidence_threshold_scale=%g\n"
            "inference_cfg_barrier_gate_enabled=%d\n"
            "inference_cfg_confidence_hard_block_threshold=%g\n"
            "inference_cfg_held_out_fraction=%g\n"
            "inference_cfg_freshness_tau=%g\n",
            inf->confidence_threshold_scale,
            inf->barrier_gate_enabled,
            inf->confidence_hard_block_threshold,
            inf->held_out_fraction,
            inf->freshness_tau);
        if (wrote > 0) n += wrote;
    }
    if (inf && inf->has_bandit && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "inference_cfg_bandit_blend_ratio=%g\n",
            inf->bandit_blend_ratio);
        if (wrote > 0) n += wrote;
    }
    if (inf && inf->has_fees && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "inference_cfg_fee_rate_maker=%g\n"
            "inference_cfg_fee_rate_taker=%g\n",
            inf->fee_rate_maker,
            inf->fee_rate_taker);
        if (wrote > 0) n += wrote;
    }
    if (inf && inf->has_training_poll_interval && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "training_poll_interval=%u\n",
            (unsigned)inf->training_poll_interval);
        if (wrote > 0) n += wrote;
    }
    // v5.9.3a — scaler sidecar binding. Both lines emit together; SHA
    // empty when feature_scaler_present=0 (no sidecar).
    if (inf && inf->has_scaler && n > 0 && (size_t)n < sizeof(canonical)) {
        const char* sha = (inf->scaler_sha256_hex && inf->scaler_sha256_hex[0])
                        ? inf->scaler_sha256_hex : "";
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "feature_scaler_present=%d\n"
            "scaler_sha256=%s\n",
            inf->feature_scaler_present ? 1 : 0, sha);
        if (wrote > 0) n += wrote;
    }
    // v5.9.4a — model num_outputs (output dimension). Stamp records
    // what trainer SAW; engine load compares vs ModelHandle.num_outputs.
    // Mismatch caught by CoreModelZoo_TryLoadRole's strict-mode gate.
    if (inf && inf->has_num_outputs && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "model_num_outputs=%d\n", inf->model_num_outputs);
        if (wrote > 0) n += wrote;
    }
    // v5.9.5h — XGBoost training hyperparams (stamp body position 17).
    // 8 fields emit together as a block. Operator-tunable max_depth/lr/
    // n_est come from Train Model panel; cfg-tunable subsample/colsample/
    // min_child_weight/seed/tree_method come from cfg.xgb_*. Engine
    // load-WARN compares to cfg at boot; mismatch logged (no refuse —
    // hyperparams don't affect inference, only forensics + reproducibility).
    if (inf && inf->has_xgb_hyperparams && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "xgb_max_depth=%d\n"
            "xgb_learning_rate=%g\n"
            "xgb_n_estimators=%d\n"
            "xgb_subsample=%g\n"
            "xgb_colsample_bytree=%g\n"
            "xgb_min_child_weight=%d\n"
            "xgb_seed=%d\n"
            "xgb_tree_method=%s\n",
            inf->xgb_max_depth, inf->xgb_learning_rate, inf->xgb_n_estimators,
            inf->xgb_subsample, inf->xgb_colsample_bytree,
            inf->xgb_min_child_weight, inf->xgb_seed, inf->xgb_tree_method);
        if (wrote > 0) n += wrote;
    }
    // v5.9.5h Phase 10 — build flags fingerprint (position 18). Emit
    // when has_build_flags_hash=1; engine load-WARN compares stamp's
    // hash vs current build's BUILD_FLAGS_HASH() (mismatch logged).
    if (inf && inf->has_build_flags_hash && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "build_flags_hash=%016lx\n",
            (unsigned long)inf->build_flags_hash);
        if (wrote > 0) n += wrote;
    }
    // v5.10.0a.G.2 — multi-horizon ensemble metadata (position 19).
    // Emitted when this model was trained as part of a horizon set
    // (cfg.horizon_list non-empty at train time). Forward-compat:
    // single-horizon stamps have has_grid_member_count=0 and skip
    // this block; legacy verifiers (pre-v5.10.0a.G.2) tolerate missing
    // fields. Engine load-time: when present, hint operator that
    // siblings exist (informational; does not enforce ensemble load).
    if (inf && inf->has_grid_member_count && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "grid_member_count=%d\n"
            "grid_member_idx=%d\n",
            inf->grid_member_count, inf->grid_member_idx);
        if (wrote > 0) n += wrote;
    }

    // v5.10.0d — label_registry_hash (canonical position 20). Set
    // inf->has_label_registry_hash=1 to emit; verifier compares against
    // engine's LABEL_REGISTRY_HASH() and refuses on mismatch (mirrors
    // feature_registry_hash refusal flow).
    if (inf && inf->has_label_registry_hash && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "label_registry_hash=%016lx\n",
            (unsigned long)inf->label_registry_hash);
        if (wrote > 0) n += wrote;
    }

    // v5.11.18a — feature_mask (canonical position 21). Set
    // inf->has_feature_mask=1 to emit; verifier compares against the
    // runtime cfg's per-core feature_mask at load time. Convention:
    // emit only when caller explicitly passes a non-default mask
    // (training-time mask differs from 0xFFFF..F all-on default). This
    // keeps stamps trained against the default mask bytewise-identical
    // to pre-v5.11.18a stamps + their HMAC signatures unchanged.
    if (inf && inf->has_feature_mask && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "feature_mask=%016lx\n",
            (unsigned long)inf->feature_mask_train);
        if (wrote > 0) n += wrote;
    }

    // v5.11.41 — per-horizon label params (canonical position 22). Set
    // inf->has_label_params=1 to emit. Forensic-only: verifier records
    // into ModelStampResult; no load-time refusal because engine has no
    // expected horizon cfg-side. Multi-horizon ensemble auto-detects
    // siblings via dir naming `_horizon_<H>` (v5.10.0a-final). Recording
    // closes /parity-check 2026-05-07-stamp CRITICAL-1 + lets operator
    // grep stamp file to identify which horizon a model belongs to.
    if (inf && inf->has_label_params && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "label_lookahead_ticks=%d\n"
            "label_tp_pct=%.6g\n"
            "label_sl_pct=%.6g\n",
            inf->label_lookahead_ticks,
            inf->label_tp_pct,
            inf->label_sl_pct);
        if (wrote > 0) n += wrote;
    }

    // v5.11.41 — XGBoost training thread count (canonical position 23).
    // Set inf->has_xgb_train_nthread=1 to emit. Forensic-only: lets
    // operator post-hoc detect whether a stamp came from serial mode
    // (cfg.xgb_train_nthread default 4) or parallel multi-horizon mode
    // (pinned to 1). Recording closes /parity-check 2026-05-07-stamp
    // CRITICAL-2.
    if (inf && inf->has_xgb_train_nthread && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "xgb_train_nthread=%d\n",
            inf->xgb_train_nthread);
        if (wrote > 0) n += wrote;
    }

    // v5.14.1.B.3 — X-macro-driven emit for stamp-bound cfg fields.
    // Each X expands to one `if (inf->has_<name>) snprintf("<name>=...")`
    // emit block. Surface G discipline: legacy callers (which leave
    // inf->has_<name>=0) emit nothing → canonical body stays
    // bytewise-identical to pre-v5.14.1.B.3 stamps.
    #define X(name, type, fmt, default_val, get_cfg_expr, emit_when)            \
        if (inf && inf->has_##name && n > 0 && (size_t)n < sizeof(canonical)) { \
            int wrote = snprintf(canonical + n, sizeof(canonical) - n,          \
                #name "=" fmt "\n", inf->name);                                 \
            if (wrote > 0) n += wrote;                                          \
        }
    FOREACH_STAMP_BOUND_CFG(X)
    #undef X

    // v5.14.2.E.2.B — model-architectural fields emit (manual, not in
    // FOREACH_STAMP_BOUND_CFG). Surface G discipline: legacy callers
    // (has_*=0) emit nothing → canonical body bytewise-identical.
    if (inf && inf->has_expected_num_classes && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "expected_num_classes=%d\n", inf->expected_num_classes);
        if (wrote > 0) n += wrote;
    }
    if (inf && inf->has_expected_role && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "expected_role=%s\n", inf->expected_role);
        if (wrote > 0) n += wrote;
    }
    if (inf && inf->has_expected_num_features && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "expected_num_features=%d\n", inf->expected_num_features);
        if (wrote > 0) n += wrote;
    }
    if (inf && inf->has_expected_feature_format_version && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "expected_feature_format_version=%d\n", inf->expected_feature_format_version);
        if (wrote > 0) n += wrote;
    }

    // v5.14.3.B — overlay-derived fields emit (manual; not in FOREACH_STAMP_BOUND_CFG).
    // Surface G discipline: legacy callers (has_*=0) emit nothing.
    if (inf && inf->has_overlay_hash && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "overlay_hash=%s\n", inf->overlay_hash);
        if (wrote > 0) n += wrote;
    }
    if (inf && inf->has_effective_hash && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "effective_hash=%s\n", inf->effective_hash);
        if (wrote > 0) n += wrote;
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
