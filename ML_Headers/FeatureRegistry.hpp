// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FEATURE REGISTRY — v5.8.1b — X-macro driven]
//======================================================================================================
// Single source of truth for ML features. Adding a new feature:
//   1. Implement `ML_Compute_<Name>(ctx)` returning FPN<F>
//   2. Append one row to FOREACH_FEATURE(X)
//   3. Recompile — FEATURE_REGISTRY_HASH flips, old stamps reject at load
//
// Status (v5.8.1b): all 34 features registered (FEAT_SHORT_SLOPE through
// FEAT_SPREAD_ZSCORE). All 5 production callers (MLStrategy,
// StrategyParameters dispatcher, BacktestSharded, PortfolioController
// regime + barrier paths) use Features_PackAll. Legacy ModelFeatures_Pack
// in ModelInference.hpp is a deprecated frozen reference — kept only so
// the EXTENSIBILITY equivalence test can validate bytewise parity.
// Scheduled for full removal in v5.9.
//
// Auto-generated from FOREACH_FEATURE(X):
//   - enum FeatureId with stable IDs
//   - FEATURE_NAMES[] / FEATURE_VERSIONS[] arrays
//   - Features_PackAll(ctx, out) — packs all enabled features into a float array
//   - FEATURE_REGISTRY_HASH (FNV-1a over name+version) — contributes to model
//     fingerprint so train-time vs serve-time feature-set mismatch becomes a
//     load-time rejection
//
// Canonical signature (do not deviate; see DOCS/FEATURE_INTERFACE.md):
//   template <unsigned F>
//   inline FPN<F> ML_Compute_<Name>(const FeatureComputeCtx<F>* ctx);
//
// FPN_Zero<F>() is the safe "I don't have data yet" return for cold-start /
// warmup paths.
//======================================================================================================
#pragma once

#include <cstdint>
#include "../FixedPoint/FixedPointN.hpp"
#include "RollingStats.hpp"
#include "../Strategies/RegimeDetector.hpp"  // RegimeSignals<F>

//======================================================================================================
// [FEATURE COMPUTE CONTEXT]
//======================================================================================================
// The bundle of inputs the registered feature compute functions read.
// Each compute fn reads what it needs and ignores the rest.
//
// v5.9.0a — tightened from the v5.8.1a forward-compat shape. Removed
// 11 unused auxiliary fields (long_rolling, medium_rolling,
// baseline_rolling, ema_price, ror, flow, book_imb, spread,
// large_trade, cumdelta, tick_rate, timestamp_us). All 34 currently
// registered features read from `signals` (precomputed by
// Regime_ComputeSignals from the upstream raw state) or from
// `short_rolling` (indices 11-14: vwap_dev, price_stddev, price_avg,
// volume_avg). Aux state is upstream of `signals`, not duplicated
// into the ctx.
//
// When a v5.10+ feature needs raw state access not surfaced by
// RegimeSignals, re-add the specific field at that time. YAGNI
// applies — declared-but-unused fields confuse readers + audit
// tools (the v5.8 audit's false-CRITICAL came from misreading
// these fields as required).
//======================================================================================================
template <unsigned F>
struct FeatureComputeCtx {
    // Pre-computed regime signal bundle. Feature indices 0-10, 15-33
    // read here. Populated by Regime_ComputeSignals at slow-path
    // cadence on both live (MLBuildContext path) and backtest
    // (BacktestSharded driver) paths — train-serve parity.
    const RegimeSignals<F>*               signals;

    // Short-window rolling stats. Feature indices 11-14 read here:
    // vwap_dev, price_stddev, price_avg, volume_avg.
    const RollingStats<F, 128>*           short_rolling;
};

//======================================================================================================
// [FEATURE COMPUTE FUNCTIONS — v5.8.1a first 10 features]
//======================================================================================================
// All read from ctx->signals (the pre-computed RegimeSignals bundle).
// FPN_Zero return on null ctx or null signals — safe for cold-start.
//======================================================================================================

template <unsigned F>
inline FPN<F> ML_Compute_ShortSlope(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->short_slope : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_ShortR2(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->short_r2 : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_ShortVariance(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->short_variance : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_LongSlope(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->long_slope : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_LongR2(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->long_r2 : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_LongVariance(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->long_variance : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_VolRatio(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->vol_ratio : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_RorSlope(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->ror_slope : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_VolumeSlope(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->volume_slope : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_VolumeDelta(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->volume_delta : FPN_Zero<F>();
}

//======================================================================================================
// [FEATURE COMPUTE FUNCTIONS — v5.8.1b features 10-33]
//======================================================================================================
// Indices 10, 15-33 read from ctx->signals (precomputed RegimeSignals bundle).
// Indices 11-14 are the only ones reading directly from ctx->short_rolling
// (vwap_deviation, price_stddev, price_avg, volume_avg).
//
// double-typed signals (hour_sin/cos, tick_rate_z, flow_*, large_trade_z,
// spread_bps/zscore) round-trip through FPN_FromDouble<F> → FPN_ToDouble →
// float. With F=64 fractional bits the round-trip is lossless to within
// 1/2^64, far below float precision, so Features_PackAll produces the same
// float bits as the legacy direct (float)double cast in ModelFeatures_Pack.
//======================================================================================================

template <unsigned F>
inline FPN<F> ML_Compute_EmaSmaSpread(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->ema_sma_spread : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_VwapDev(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->short_rolling) ? ctx->short_rolling->vwap_deviation : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_PriceStddev(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->short_rolling) ? ctx->short_rolling->price_stddev : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_PriceAvg(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->short_rolling) ? ctx->short_rolling->price_avg : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_VolumeAvg(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->short_rolling) ? ctx->short_rolling->volume_avg : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_EmaAboveSma(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals)
        ? FPN_FromDouble<F>((double)ctx->signals->ema_above_sma)
        : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_MidSlope(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->mid_slope : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_MidR2(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->mid_r2 : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_CumDelta(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->cumdelta : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_HourSin(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->hour_sin) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_HourCos(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->hour_cos) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_VolRegimeRatio(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->vol_regime_ratio : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_TickRateZ(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->tick_rate_z) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_DistToHigh(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->dist_to_high : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_DistToLow(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->dist_to_low : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_BookImbMeanShort(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->book_imb_mean_short : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_BookImbMeanLong(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->book_imb_mean_long : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_BookImbDrift(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->book_imb_drift : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_Flow10s(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->flow_10s) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_Flow1m(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->flow_1m) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_Flow5m(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->flow_5m) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_LargeTradeZ(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->large_trade_z) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_SpreadBps(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->spread_bps) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN<F> ML_Compute_SpreadZscore(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->spread_zscore) : FPN_Zero<F>();
}

//======================================================================================================
// [FEATURE REGISTRY — X-macro]
//======================================================================================================
// Row format:
//   X(<ID>, <name>, <version>, <ENABLED|DISABLED>, <compute_fn>, <note>)
//
// ENABLED/DISABLED is a compile-time gate — DISABLED features are skipped
// by Features_PackAll AND don't contribute to FEATURE_REGISTRY_HASH (so
// disabling a feature flips the hash, forcing retrain).
//
// Version: bump on formula change. Bumping flips the hash → retrain.
// Don't bump for renames or comment-only edits.
//
// IDs: contiguous from 0. Order MUST match historical FEAT_* indices so
// trained models continue to find each feature at the expected position.
// EXTENSIBILITY tests pin specific FEATURE_<NAME> == <legacy index> values
// to catch reorderings.
//======================================================================================================

#define FEATURE_ENABLED  1
#define FEATURE_DISABLED 0

#define FOREACH_FEATURE(X) \
    X(SHORT_SLOPE,        "short_slope",        1, FEATURE_ENABLED, ML_Compute_ShortSlope,        "regression slope, 128-tick window") \
    X(SHORT_R2,           "short_r2",           1, FEATURE_ENABLED, ML_Compute_ShortR2,           "R² of short-window regression") \
    X(SHORT_VARIANCE,     "short_variance",     1, FEATURE_ENABLED, ML_Compute_ShortVariance,     "price variance over 128 ticks") \
    X(LONG_SLOPE,         "long_slope",         1, FEATURE_ENABLED, ML_Compute_LongSlope,         "regression slope, 512-tick window") \
    X(LONG_R2,            "long_r2",            1, FEATURE_ENABLED, ML_Compute_LongR2,            "R² of long-window regression") \
    X(LONG_VARIANCE,      "long_variance",      1, FEATURE_ENABLED, ML_Compute_LongVariance,      "price variance over 512 ticks") \
    X(VOL_RATIO,          "vol_ratio",          1, FEATURE_ENABLED, ML_Compute_VolRatio,          "short variance / long variance") \
    X(ROR_SLOPE,          "ror_slope",          1, FEATURE_ENABLED, ML_Compute_RorSlope,          "regression-on-regression slope") \
    X(VOLUME_SLOPE,       "volume_slope",       1, FEATURE_ENABLED, ML_Compute_VolumeSlope,       "regression slope of volume") \
    X(VOLUME_DELTA,       "volume_delta",       1, FEATURE_ENABLED, ML_Compute_VolumeDelta,       "volume change last vs avg") \
    X(EMA_SMA_SPREAD,     "ema_sma_spread",     1, FEATURE_ENABLED, ML_Compute_EmaSmaSpread,      "(ema - sma) / sma normalized") \
    X(VWAP_DEV,           "vwap_dev",           1, FEATURE_ENABLED, ML_Compute_VwapDev,           "VWAP deviation from short rolling") \
    X(PRICE_STDDEV,       "price_stddev",       1, FEATURE_ENABLED, ML_Compute_PriceStddev,       "stddev of price, short window") \
    X(PRICE_AVG,          "price_avg",          1, FEATURE_ENABLED, ML_Compute_PriceAvg,          "mean price, short window") \
    X(VOLUME_AVG,         "volume_avg",         1, FEATURE_ENABLED, ML_Compute_VolumeAvg,         "mean volume, short window") \
    X(EMA_ABOVE_SMA,      "ema_above_sma",      1, FEATURE_ENABLED, ML_Compute_EmaAboveSma,       "1 if ema > short SMA (binary)") \
    X(MID_SLOPE,          "mid_slope",          1, FEATURE_ENABLED, ML_Compute_MidSlope,          "regression slope, 256-tick window") \
    X(MID_R2,             "mid_r2",             1, FEATURE_ENABLED, ML_Compute_MidR2,             "R² of mid-window regression") \
    X(CUMDELTA,           "cumdelta",           1, FEATURE_ENABLED, ML_Compute_CumDelta,          "rolling cumulative buyer-vs-seller") \
    X(HOUR_SIN,           "hour_sin",           1, FEATURE_ENABLED, ML_Compute_HourSin,           "cyclical hour-of-day sin") \
    X(HOUR_COS,           "hour_cos",           1, FEATURE_ENABLED, ML_Compute_HourCos,           "cyclical hour-of-day cos") \
    X(VOL_REGIME_RAT,     "vol_regime_rat",     1, FEATURE_ENABLED, ML_Compute_VolRegimeRatio,    "short stddev / baseline stddev") \
    X(TICK_RATE_Z,        "tick_rate_z",        1, FEATURE_ENABLED, ML_Compute_TickRateZ,         "ticks/sec z-score vs trailing baseline") \
    X(DIST_TO_HIGH,       "dist_to_high",       1, FEATURE_ENABLED, ML_Compute_DistToHigh,        "(baseline_max - price) / price") \
    X(DIST_TO_LOW,        "dist_to_low",        1, FEATURE_ENABLED, ML_Compute_DistToLow,         "(price - baseline_min) / price") \
    X(BOOK_IMB_MEAN_SHORT, "book_imb_mean_short", 1, FEATURE_ENABLED, ML_Compute_BookImbMeanShort, "mean of last 64 book_imbalance samples") \
    X(BOOK_IMB_MEAN_LONG,  "book_imb_mean_long",  1, FEATURE_ENABLED, ML_Compute_BookImbMeanLong,  "mean over full BookImbalanceHistory window") \
    X(BOOK_IMB_DRIFT,      "book_imb_drift",      1, FEATURE_ENABLED, ML_Compute_BookImbDrift,     "current book_imbalance - mean_long") \
    X(FLOW_10S,           "flow_10s",           1, FEATURE_ENABLED, ML_Compute_Flow10s,           "signed-volume EWMA, half-life 10s") \
    X(FLOW_1M,            "flow_1m",            1, FEATURE_ENABLED, ML_Compute_Flow1m,            "signed-volume EWMA, half-life 60s") \
    X(FLOW_5M,            "flow_5m",            1, FEATURE_ENABLED, ML_Compute_Flow5m,            "signed-volume EWMA, half-life 300s") \
    X(LARGE_TRADE_Z,      "large_trade_z",      1, FEATURE_ENABLED, ML_Compute_LargeTradeZ,       "z-score of current trade size") \
    X(SPREAD_BPS,         "spread_bps",         1, FEATURE_ENABLED, ML_Compute_SpreadBps,         "spread / mid_price × 10000") \
    X(SPREAD_ZSCORE,      "spread_zscore",      1, FEATURE_ENABLED, ML_Compute_SpreadZscore,      "z-score of current spread")

// Auto-generated FEATURE_<ID> enum constants. Order matches FOREACH_FEATURE.
enum FeatureId : uint16_t {
#define X(id, name, version, enabled, fn, note) FEATURE_##id,
    FOREACH_FEATURE(X)
#undef X
    NUM_REGISTERED_FEATURES
};

// Names + versions arrays — auto-generated.
static const char* FEATURE_NAMES[] = {
#define X(id, name, version, enabled, fn, note) name,
    FOREACH_FEATURE(X)
#undef X
};

static const int FEATURE_VERSIONS[] = {
#define X(id, name, version, enabled, fn, note) version,
    FOREACH_FEATURE(X)
#undef X
};

static const int FEATURE_ENABLED_FLAGS[] = {
#define X(id, name, version, enabled, fn, note) enabled,
    FOREACH_FEATURE(X)
#undef X
};

static_assert(sizeof(FEATURE_NAMES) / sizeof(*FEATURE_NAMES) == NUM_REGISTERED_FEATURES,
              "FEATURE_NAMES out of sync with NUM_REGISTERED_FEATURES");
static_assert(sizeof(FEATURE_VERSIONS) / sizeof(*FEATURE_VERSIONS) == NUM_REGISTERED_FEATURES,
              "FEATURE_VERSIONS out of sync with NUM_REGISTERED_FEATURES");

//======================================================================================================
// [FEATURE REGISTRY HASH — FNV-1a over enabled feature names+versions]
//======================================================================================================
// Compile-time hash that contributes to the model fingerprint. Stamp body
// embeds this value at training time; verifier rejects load when it
// differs from the build-time value (= train-serve feature-set mismatch).
//
// DISABLED features don't contribute, so flipping ENABLED → DISABLED also
// flips the hash, forcing retrain.
//======================================================================================================

namespace tt {
constexpr uint64_t FNV_OFFSET_64 = 0xcbf29ce484222325ULL;
constexpr uint64_t FNV_PRIME_64  = 0x100000001b3ULL;

// FNV-1a hash of a null-terminated C string. Recursive constexpr for C++17.
constexpr uint64_t fnv1a(const char* s, uint64_t h = FNV_OFFSET_64) {
    return *s ? fnv1a(s + 1, (h ^ (uint64_t)(uint8_t)*s) * FNV_PRIME_64) : h;
}
} // namespace tt

// Compile-time fold over the registry. Each enabled row contributes
// hash(name) chained with hash(":v" + version).
inline uint64_t feature_registry_hash_compute() {
    uint64_t h = tt::FNV_OFFSET_64;
#define X(id, name, version, enabled, fn, note) \
    if ((enabled)) { \
        for (const char* p = name; *p; ++p) \
            h = (h ^ (uint64_t)(uint8_t)*p) * tt::FNV_PRIME_64; \
        const char* vstr = ":v" #version; \
        for (const char* p = vstr; *p; ++p) \
            h = (h ^ (uint64_t)(uint8_t)*p) * tt::FNV_PRIME_64; \
    }
    FOREACH_FEATURE(X)
#undef X
    return h;
}

inline uint64_t FEATURE_REGISTRY_HASH() {
    static const uint64_t h = feature_registry_hash_compute();
    return h;
}

//======================================================================================================
// [FEATURES_PACK_ALL — the new packer]
//======================================================================================================
// Replaced ModelFeatures_Pack at v5.8.1b. Loops the X-macro registry,
// invokes each enabled compute fn, writes float result into out[i].
//
// `out` must have capacity >= NUM_REGISTERED_FEATURES.
// Returns the number of features written.
//
// Equivalence test in controller_test.cpp pins out[i] == legacy
// ModelFeatures_Pack buf[i] for ALL 34 indices — load-bearing regression
// guard against future divergence in either path.
//======================================================================================================
template <unsigned F>
inline int Features_PackAll(const FeatureComputeCtx<F>* ctx, float* out) {
    int n = 0;
#define X(id, name, version, enabled, fn, note) \
    if ((enabled)) { \
        out[FEATURE_##id] = (float)FPN_ToDouble(fn(ctx)); \
        ++n; \
    }
    FOREACH_FEATURE(X)
#undef X
    return n;
}
