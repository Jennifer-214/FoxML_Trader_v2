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
#include "StampBoundModelConstRegistry.hpp"  // v5.14.8.0+ — FOREACH_STAMP_BOUND_MODEL_CONST X-macro (registry, MASK constants, STAMP_HAS aliases, AUTOPOPULATE)
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
    // v5.14.9.D — DELETED stamp_inf_freshness_tau (TECH_DEBT-004 close).
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
    // v5.14.8.E — stale-model gate fields. Copied from ModelStampResult
    // at TryLoadRole post-verify (v5.14.8.D adds them to stamp body via
    // FOREACH_STAMP_BOUND_MODEL_CONST_POST_CFG). Read by
    // CoreModelZoo_CheckStaleModel at boot.
    // Manual fields on ModelHandle (NOT registry-driven) — ModelHandle
    // X-macro migration deferred (TECH_DEBT-014); these are added directly
    // to keep the v5.14.8 ship bounded.
    uint8_t  has_training_timestamp_us;
    uint64_t training_timestamp_us;     // wall-clock at training time (μs since epoch)
    uint8_t  has_run_name;
    char     run_name[64];              // operator-set training run identifier
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
    // v5.14.9.D — DELETED stamp_inf_freshness_tau zero-init (TECH_DEBT-004 close).
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
    // v5.14.8.E — stale-model gate fields zero-init.
    m->has_training_timestamp_us = 0;
    m->training_timestamp_us     = 0;
    m->has_run_name              = 0;
    m->run_name[0]               = '\0';
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

// v5.14.8.A.merged — ModelStampResult migrated to X-macro generation
// from FOREACH_STAMP_BOUND_MODEL_CONST registry. has_* flags bit-packed
// into uint64_t has_flags (v5.14.8.A.merged.1 bit allocation enum).
// All 26 architectural fields auto-flow from the registry; future field
// addition = 1 row in registry → struct field + parser + emitter +
// AUTOPOPULATE wiring all auto-derived. Closes TECH_DEBT-006.
//
// Caller migration: `r.has_<X>` → `STAMP_HAS(r, <group_or_entry>)`;
// field reads continue as `r.<canonical_name>`.
struct ModelStampResult {
    // === Runtime-only fields (NOT in registry) ===
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
    int      inference_cfg_drift_count;  // v5.9.2b runtime: 0 if all match; >0 = mismatched fields
    uint8_t  cross_major_engine;     // v5.9.2b runtime: stamp's engine_version differs by major

    // === Bit-packed has_* flags (v5.14.8.A.merged.1; 13 bits used today) ===
    // 6 group bits (inference_cfg, scaler, fees, xgb_hyperparams, grid_member, label_params)
    // + 7 standalone bits (inference_cfg_bandit_blend_ratio, training_poll_interval,
    // model_num_outputs, build_flags_hash, label_registry_hash, feature_mask, xgb_train_nthread).
    // 51 bits headroom in uint64_t for future fields.
    uint64_t has_flags;

    // === Architectural value fields — auto-generated from FOREACH_STAMP_BOUND_MODEL_CONST ===
    // 26 entries (v5.14.8.A.0.b restored 3 dropped fields; v5.14.8.A.merged added xgb_tree_method).
    #define X(name, group, presence, type, fmt, default_val, get_value, emit_when, doc) \
        type name;
    FOREACH_STAMP_BOUND_MODEL_CONST(X)
    #undef X

    // === FOREACH_STAMP_BOUND_CFG fields (sister registry; v5.14.1.B.3 X-macro driven) ===
    // Each field has a uint8_t has_<name> Surface G forward-compat flag
    // (per-entry semantics; sister registry uses per-entry has_*, not group-bit).
    // v5.14.9.F.2 — 7-arg X macro signature (emit_source col added; unused for struct gen)
    #define X(name, type, fmt, default_val, get_cfg_expr, emit_when, emit_source) \
        uint8_t has_##name;                                                       \
        type name;
    FOREACH_STAMP_BOUND_CFG(X)
    #undef X

    // === Late-emit architectural fields auto-generated from FOREACH_STAMP_BOUND_MODEL_CONST_POST_CFG ===
    // (v5.14.8.A.merged.4 — TECH_DEBT-006 full closure). 6 entries:
    // expected_num_classes, expected_role, expected_num_features,
    // expected_feature_format_version, overlay_hash, effective_hash.
    // has_* flags bit-packed in has_flags (above); typed value fields
    // declared via X-macro below. Entries emit AFTER FOREACH_STAMP_BOUND_CFG
    // to preserve canonical wire format.
    //
    // Note: ModelStampResult includes ALL POST_CFG entries (parser side
    // sees everything). FOREACH_STAMP_BOUND_MODEL_CONST already walked
    // both halves above (union expansion).
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
    // v5.14.8.A.merged — Brace-init zero-fills the entire struct in one
    // statement. Replaces ~90 lines of explicit field zero-inits.
    // - has_flags = 0 (all bits clear; legacy stamps load with all
    //   group/standalone bits 0 → drift checks skip silently)
    // - All char arrays = "" (zero-filled)
    // - All numeric fields = 0
    // - FOREACH_STAMP_BOUND_CFG fields' has_* + value = 0
    // - Late-emit manual fields (expected_*, overlay_hash, effective_hash) = 0
    //
    // After brace-init, set the runtime-only fields that need non-zero
    // initial values (valid + gap_threshold from caller param).
    ModelStampResult r{};
    r.valid = -1;
    r.gap_threshold = gap_threshold;

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
                STAMP_SET(r, inference_cfg);
            } else if (strcmp(key, "inference_cfg_barrier_gate_enabled") == 0) {
                r.inference_cfg_barrier_gate_enabled = atoi(val);
                STAMP_SET(r, inference_cfg);
            } else if (strcmp(key, "inference_cfg_confidence_hard_block_threshold") == 0) {
                r.inference_cfg_confidence_hard_block_threshold = tt::parse_double_fast(val);
                STAMP_SET(r, inference_cfg);
            } else if (strcmp(key, "inference_cfg_held_out_fraction") == 0) {
                r.inference_cfg_held_out_fraction = tt::parse_double_fast(val);
                STAMP_SET(r, inference_cfg);
            // v5.14.9.D — DELETED inference_cfg_freshness_tau parser branch
            // (TECH_DEBT-004 close). Legacy stamps with this key parse it as
            // unknown (silently ignored by parser); HMAC chain unbroken.
            } else if (strcmp(key, "inference_cfg_bandit_blend_ratio") == 0) {
                r.inference_cfg_bandit_blend_ratio = tt::parse_double_fast(val);
                STAMP_SET(r, inference_cfg_bandit_blend_ratio);
            } else if (strcmp(key, "inference_cfg_fee_rate_maker") == 0) {
                r.inference_cfg_fee_rate_maker = tt::parse_double_fast(val);
                STAMP_SET(r, fees);
            } else if (strcmp(key, "inference_cfg_fee_rate_taker") == 0) {
                r.inference_cfg_fee_rate_taker = tt::parse_double_fast(val);
                STAMP_SET(r, fees);
            } else if (strcmp(key, "training_poll_interval") == 0) {
                r.training_poll_interval = (uint32_t)strtoul(val, nullptr, 10);
                STAMP_SET(r, training_poll_interval);
            }
            // v5.9.3a — scaler fields
            else if (strcmp(key, "feature_scaler_present") == 0) {
                r.feature_scaler_present = (atoi(val) != 0) ? 1 : 0;
                STAMP_SET(r, scaler);
            } else if (strcmp(key, "scaler_sha256") == 0) {
                size_t vl = strlen(val);
                if (vl >= sizeof(r.scaler_sha256)) vl = sizeof(r.scaler_sha256) - 1;
                memcpy(r.scaler_sha256, val, vl);
                r.scaler_sha256[vl] = '\0';
                STAMP_SET(r, scaler);
            }
            // v5.9.4a — model_num_outputs (output dimension binding)
            else if (strcmp(key, "model_num_outputs") == 0) {
                r.model_num_outputs = atoi(val);
                STAMP_SET(r, model_num_outputs);
            }
            // v5.9.5h — XGBoost hyperparam parsing. Macro-expanded
            // to keep the 8-field dispatch tight (vs an if/else
            // chain). Each macro expands to one `else if` clause
            // continuing the existing chain.
            #define PARSE_XGB_INT(field) \
                else if (strcmp(key, "xgb_" #field) == 0) { \
                    r.xgb_##field = atoi(val); \
                    STAMP_SET(r, xgb_hyperparams); \
                }
            #define PARSE_XGB_DOUBLE(field) \
                else if (strcmp(key, "xgb_" #field) == 0) { \
                    r.xgb_##field = tt::parse_double_fast(val); \
                    STAMP_SET(r, xgb_hyperparams); \
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
                STAMP_SET(r, xgb_hyperparams);
            }
            #undef PARSE_XGB_INT
            #undef PARSE_XGB_DOUBLE
            // v5.9.5h Phase 10 — build flags hash (hex parse)
            else if (strcmp(key, "build_flags_hash") == 0) {
                r.build_flags_hash = (uint64_t)strtoull(val, nullptr, 16);
                STAMP_SET(r, build_flags_hash);
            }
            // v5.10.0a.G.2 — multi-horizon ensemble member count (position 19)
            else if (strcmp(key, "grid_member_count") == 0) {
                r.grid_member_count = atoi(val);
                STAMP_SET(r, grid_member);
            }
            else if (strcmp(key, "grid_member_idx") == 0) {
                r.grid_member_idx = atoi(val);
                STAMP_SET(r, grid_member);
            }
            // v5.10.0d — label registry hash (position 20)
            else if (strcmp(key, "label_registry_hash") == 0) {
                r.label_registry_hash = (uint64_t)strtoull(val, nullptr, 16);
                STAMP_SET(r, label_registry_hash);
            }
            // v5.11.18a — feature mask (position 21). Hex-encoded uint64
            // bitmap of features active during training. Stamp emits
            // %016lx (no prefix). Verifier compares against runtime cfg
            // mask in 3-tier strict-mode. Legacy stamps (no field) load
            // with has_feature_mask=0 → check skipped.
            else if (strcmp(key, "feature_mask") == 0) {
                r.feature_mask = (uint64_t)strtoull(val, nullptr, 16);
                STAMP_SET(r, feature_mask);
            }
            // v5.11.41 — per-horizon label params (canonical position 22).
            else if (strcmp(key, "label_lookahead_ticks") == 0) {
                r.label_lookahead_ticks = atoi(val);
                STAMP_SET(r, label_params);
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
                STAMP_SET(r, xgb_train_nthread);
            }
            // v5.14.1.B.3 — X-macro-driven parser branches (positions 24+).
            // Each X expands to one `else if (strcmp(key, "<name>") == 0)`
            // branch that uses the type-dispatched STAMP_CFG_PARSE macro.
            // v5.14.9.F.2 — 7-arg X macro signature (emit_source col added; unused for parser)
            #define X(name, type, fmt, default_val, get_cfg_expr, emit_when, emit_source) \
                else if (strcmp(key, #name) == 0) {                                       \
                    r.name = (type)(STAMP_CFG_PARSE(type, val));                          \
                    r.has_##name = 1;                                                     \
                }
            FOREACH_STAMP_BOUND_CFG(X)
            #undef X
            // v5.14.8.A.merged.4 — POST_CFG section parser branches (registry-driven).
            // Replaces manual if-else branches for expected_*, overlay_hash,
            // effective_hash (6 fields total). All POST_CFG entries are
            // standalone (group="_") so STAMP_SET(r, name) sets the entry's
            // own bit. tt::stamp_parse_field<T> templated helper handles
            // type-dispatch (char[N] strncpy / scalar cast); avoids the
            // non-template `if constexpr` cast-syntax issue (templated
            // function instantiates per-T, properly discarding branches).
            #define X(name, group, presence, type, fmt, default_val, get_value, emit_when, doc) \
                else if (strcmp(key, #name) == 0) { \
                    tt::stamp_parse_field(r.name, val); \
                    STAMP_PARSER_SET_HAS_##group(name); \
                }
            FOREACH_STAMP_BOUND_MODEL_CONST_POST_CFG(X)
            #undef X
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
        if (!STAMP_HAS(r, feature_mask)) {
            fprintf(stderr,
                "[stamp] WARN: %s stamp lacks feature_mask "
                "(pre-v5.11.18a) — feature-mask drift NOT verified\n",
                stamp_path);
        } else if (r.feature_mask != expected_feature_mask) {
            r.valid = 0;
            snprintf(r.reason, sizeof(r.reason),
                "feature_mask mismatch: stamp=%016lx engine=%016lx "
                "(per-core feature subset drift; retrain or restore "
                "feature_mask cfg to training-time value)",
                (unsigned long)r.feature_mask,
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
// v5.14.8.A.merged.3 — StampInferenceCfgInputs migrated to X-macro
// generation from FOREACH_STAMP_BOUND_MODEL_CONST registry (matches
// ModelStampResult; Option 1 unification). Both structs now use canonical
// wire-key field names + uint64_t has_flags bit-packed via STAMP_HAS aliases.
//
// Production caller migration: replace manual population blocks with
// `STAMP_MODEL_CONST_AUTOPOPULATE(inf, src)` macro call. Future field
// additions auto-flow via the registry (extinguishes v5.9.5b production-
// caller class for stamp body fields).
struct StampInferenceCfgInputs {
    // === Bit-packed has_* flags (matches ModelStampResult) ===
    uint64_t has_flags;

    // === Architectural value fields — auto-generated from FOREACH_STAMP_BOUND_MODEL_CONST ===
    // 26 entries; canonical wire-key names. Same expansion as ModelStampResult side.
    #define X(name, group, presence, type, fmt, default_val, get_value, emit_when, doc) \
        type name;
    FOREACH_STAMP_BOUND_MODEL_CONST(X)
    #undef X

    // === FOREACH_STAMP_BOUND_CFG fields (sister registry; per-entry has_*) ===
    // v5.14.9.F.2 — 7-arg X macro signature (emit_source col added; unused for struct gen)
    #define X(name, type, fmt, default_val, get_cfg_expr, emit_when, emit_source) \
        int  has_##name;                                                          \
        type name;
    FOREACH_STAMP_BOUND_CFG(X)
    #undef X

    // === Late-emit architectural fields — auto-generated via FOREACH_STAMP_BOUND_MODEL_CONST union (v5.14.8.A.merged.4) ===
    // POST_CFG section's entries declared above via X-macro walk over
    // FOREACH_STAMP_BOUND_MODEL_CONST = PRE_CFG + POST_CFG. has_* bits
    // bit-packed in has_flags; STAMP_HAS(inf, expected_num_classes) etc.
    // for accessor.
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
    // v5.14.8.A.merged.3 — Registry-driven emit walk. Replaces ~152 lines
    // of manual `if (inf->has_X) snprintf(...)` blocks with single X-macro
    // walk over FOREACH_STAMP_BOUND_MODEL_CONST. Per-entry has_* dispatch
    // via STAMP_EMIT_CHECK_HAS_<group> token paste. Wire format byte-for-byte
    // preserved (same registry order as canonical emit sequence v5.14.7).
    //
    // Type handling: printf format string in registry's `fmt` column matches
    // C type promotion rules on x86_64:
    //   double + "%g" / "%.6g" — direct
    //   int + "%d" — direct
    //   uint8_t + "%d" — integer promotion to int
    //   uint32_t + "%u" — same as unsigned int on x86_64
    //   uint64_t + "%016lx" — same as unsigned long on x86_64
    //   tt::stamp_str_N (char[N]) + "%s" — array decay to char*
    // Compiler may emit -Wformat warnings for non-x86_64; if porting to
    // 32-bit, add explicit casts via a templated stamp_emit_field<T> helper.
    #define X(name, group, presence, type, fmt, default_val, get_value, emit_when, doc) \
        if (inf && STAMP_EMIT_CHECK_HAS_##group(name) && n > 0 && (size_t)n < sizeof(canonical)) { \
            int wrote = snprintf(canonical + n, sizeof(canonical) - n, \
                #name "=" fmt "\n", inf->name); \
            if (wrote > 0) n += wrote; \
        }
    FOREACH_STAMP_BOUND_MODEL_CONST_PRE_CFG(X)
    #undef X

    // v5.14.1.B.3 — X-macro-driven emit for stamp-bound cfg fields.
    // Each X expands to one `if (inf->has_<name>) snprintf("<name>=...")`
    // emit block. Surface G discipline: legacy callers (which leave
    // inf->has_<name>=0) emit nothing → canonical body stays
    // bytewise-identical to pre-v5.14.1.B.3 stamps.
    // v5.14.9.F.2 — 7-arg X macro signature (emit_source col added; unused for emit — wire bytes
    // are read from inf->name struct field, populated by AUTOPOPULATE which dispatched by emit_source)
    #define X(name, type, fmt, default_val, get_cfg_expr, emit_when, emit_source) \
        if (inf && inf->has_##name && n > 0 && (size_t)n < sizeof(canonical)) {   \
            int wrote = snprintf(canonical + n, sizeof(canonical) - n,            \
                #name "=" fmt "\n", inf->name);                                   \
            if (wrote > 0) n += wrote;                                            \
        }
    FOREACH_STAMP_BOUND_CFG(X)
    #undef X

    // v5.14.8.A.merged.4 — POST_CFG section emit walk (registry-driven).
    // Closes TECH_DEBT-006: 6 late-emit fields (expected_*, overlay_hash,
    // effective_hash) auto-flow via the same X-macro pattern as PRE_CFG.
    // Wire format byte-for-byte preserved (canonical order: PRE_CFG → cfg
    // → POST_CFG; same as v5.14.7 manual emit).
    #define X(name, group, presence, type, fmt, default_val, get_value, emit_when, doc) \
        if (inf && STAMP_EMIT_CHECK_HAS_##group(name) && n > 0 && (size_t)n < sizeof(canonical)) { \
            int wrote = snprintf(canonical + n, sizeof(canonical) - n, \
                #name "=" fmt "\n", inf->name); \
            if (wrote > 0) n += wrote; \
        }
    FOREACH_STAMP_BOUND_MODEL_CONST_POST_CFG(X)
    #undef X

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
