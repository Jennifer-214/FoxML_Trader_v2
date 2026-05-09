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
#include <cmath>  // v5.9.0: isnan/isinf for NaN-fold validation in Features_PackAll
#include <array>  // v5.14.5.C: constexpr coefficient tables for fractional differentiation
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

    // v5.14.5.B — hysteresed regime classification (REGIME_RANGING /
    // _TRENDING / _VOLATILE / _TRENDING_DOWN / _MILD_TREND). Populated
    // at slow-path callers from EventLoopCoreState.regime_state.current_regime
    // (universalized in v5.14.5.B.0 to fire for ALL cores, not just AUTO).
    // Read by ML_Compute_RegimeClassOneHot.
    //
    // Default 0 (REGIME_RANGING) when caller doesn't populate; safe for
    // legacy/test paths that don't have regime_state in scope.
    int                                   current_regime;
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
// [v5.14.5.B — REGIME-CONDITIONAL FEATURES]
//======================================================================================================
// 3 features that expose regime context to ML models. Differs from
// existing SHORT_SLOPE / VOL_RATIO in normalization characteristics
// (saturating vs unbounded; sign-carrying z-score vs ratio).
//
// EMPIRICAL VERIFICATION DISCIPLINE (TECH_DEBT-007):
//   regime_trend_strength + regime_vol_zscore have semantic overlap
//   with existing SHORT_SLOPE / VOL_RATIO. The differing normalization
//   may produce complementary training signal OR may train identically
//   depending on operator's data + model architecture. Verify via
//   feature_importance scores post-first-retrain. If trend_strength
//   importance < 0.01 AND SHORT_SLOPE importance > 0.05 → likely
//   redundant; drop in v5.X+ ship via FEATURE_REGISTRY_HASH bump.
//
// regime_class_onehot reads ctx->current_regime directly (populated by
// slow-path callers from EventLoopCoreState.regime_state.current_regime;
// universalized in v5.14.5.B.0). Genuinely new info; not derivable
// from existing RegimeSignals fields.
//======================================================================================================

// regime_trend_strength: saturating clamp of short_slope to [-1, 1].
// Bounded; sign-preserving. Differs from SHORT_SLOPE (unbounded slope/avg
// ratio) in that extreme values saturate rather than dominating model
// gradient. Verify importance vs SHORT_SLOPE post-train (TECH_DEBT-007).
template <unsigned F>
inline FPN<F> ML_Compute_RegimeTrendStrength(const FeatureComputeCtx<F>* ctx) {
    if (!ctx || !ctx->signals) return FPN_Zero<F>();
    FPN<F> x = ctx->signals->short_slope;
    FPN<F> one = FPN_FromInt<F>(1);
    FPN<F> neg_one = FPN_Sub(FPN_Zero<F>(), one);
    if (FPN_GreaterThan(x, one)) return one;
    if (FPN_LessThan(x, neg_one)) return neg_one;
    return x;
}

// regime_vol_zscore: (short_var - long_var) / sqrt(long_var) z-score.
// Sign-carrying (positive = elevated vol; negative = depressed).
// Differs from VOL_RATIO (positive ratio) in sign + bounded behavior.
// Verify importance vs VOL_RATIO post-train (TECH_DEBT-007).
template <unsigned F>
inline FPN<F> ML_Compute_RegimeVolZscore(const FeatureComputeCtx<F>* ctx) {
    if (!ctx || !ctx->signals) return FPN_Zero<F>();
    FPN<F> short_var = ctx->signals->short_variance;
    FPN<F> long_var = ctx->signals->long_variance;
    if (FPN_IsZero(long_var)) return FPN_Zero<F>();
    FPN<F> diff = FPN_Sub(short_var, long_var);
    FPN<F> denom = FPN_Sqrt(long_var);
    if (FPN_IsZero(denom)) return FPN_Zero<F>();
    return FPN_DivNoAssert(diff, denom);
}

// regime_class_onehot: current_regime as int (0..NUM_REGIMES-1).
// Read from ctx->current_regime (populated by slow-path callers from
// EventLoopCoreState.regime_state.current_regime; universalized in
// v5.14.5.B.0 to fire for ALL cores). Default 0 (REGIME_RANGING) if
// caller doesn't populate (legacy/test paths).
//
// "Onehot" name preserved for future v5.X+ expansion to actual one-hot
// vector encoding (NUM_REGIMES separate features); single-int form
// today is the bandwidth-conscious version. Tree-based models (XGBoost)
// handle integer-valued categorical features natively.
template <unsigned F>
inline FPN<F> ML_Compute_RegimeClassOneHot(const FeatureComputeCtx<F>* ctx) {
    if (!ctx) return FPN_Zero<F>();
    return FPN_FromInt<F>(ctx->current_regime);
}

//======================================================================================================
// [v5.14.5.C — FRACTIONAL DIFFERENTIATION FEATURES]
//======================================================================================================
// Marcos Lopez de Prado fractional differentiation. Removes the long-
// memory component of price series while preserving stationarity in
// the residual. Three integration orders d ∈ {0.4, 0.5, 0.6} bracket
// the range typically informative for crypto tick prices (per
// FoxML_Core research; d=0.5 is often the sweet spot but
// near-neighbors give complementary signal).
//
// Math:
//   Δ^d x_t = Σ_{k=0..K-1} (-1)^k * C(d, k) * x_{t-k}
//   C(d, k) = d*(d-1)*...*(d-k+1) / k!
//   Recurrence: C(d, k) = C(d, k-1) * (d-k+1) / k
//
// K=50 captures > 99.999% of the infinite-sum weight for d ∈ [0.4, 0.6].
// Coefficients computed at compile time (constexpr); converted to FPN
// per-call in Compute fns (50 FPN_FromDouble × 3 fns = ~150 conversions
// per slow-path cycle ≈ ~50ns total; SLOW-PATH only, well within budget).
//
// Train-serve parity: same Compute fn reads same RollingStats price_buf
// ring in both live + backtest paths. Bytewise identical by construction.
//
// FUTURE OPPORTUNITY (v5.16+): same pattern works for volume_buf[]; add
// ML_Compute_FracDiffVolume_d05 etc. as additional features if model
// finds price-frac-diff useful and we want volume-frac-diff isolation.
//======================================================================================================

constexpr int FRAC_DIFF_K = 50;

constexpr std::array<double, FRAC_DIFF_K> ComputeFracDiffCoeffs(double d) {
    std::array<double, FRAC_DIFF_K> c{};
    c[0] = 1.0;
    for (int k = 1; k < FRAC_DIFF_K; k++) {
        c[k] = c[k-1] * (d - (double)k + 1.0) / (double)k;
    }
    return c;
}

constexpr auto kFracDiff_d04_Coeffs = ComputeFracDiffCoeffs(0.4);
constexpr auto kFracDiff_d05_Coeffs = ComputeFracDiffCoeffs(0.5);
constexpr auto kFracDiff_d06_Coeffs = ComputeFracDiffCoeffs(0.6);

// Generic Compute helper: walk K=50 most-recent prices, accumulate
// alternating sum. Branchless wrap relies on W=128 being a power of 2.
template <unsigned F>
inline FPN<F> FracDiffPriceCompute(const FeatureComputeCtx<F>* ctx,
                                    const std::array<double, FRAC_DIFF_K>& coeffs) {
    if (!ctx || !ctx->short_rolling) return FPN_Zero<F>();
    const auto* rs = ctx->short_rolling;
    if (rs->count < FRAC_DIFF_K) return FPN_Zero<F>();
    constexpr int W = 128;
    static_assert((W & (W - 1)) == 0, "W must be power of 2 for branchless wrap");
    FPN<F> sum = FPN_Zero<F>();
    int idx = (rs->head - 1) & (W - 1);
    for (int k = 0; k < FRAC_DIFF_K; k++) {
        FPN<F> coeff_fpn = FPN_FromDouble<F>(coeffs[k]);
        FPN<F> term = FPN_Mul(coeff_fpn, rs->price_buf[idx]);
        // Sign alternates: even k → add, odd k → subtract.
        sum = ((k & 1) == 0) ? FPN_Add(sum, term) : FPN_Sub(sum, term);
        idx = (idx - 1) & (W - 1);
    }
    return sum;
}

template <unsigned F>
inline FPN<F> ML_Compute_FracDiffPrice_d04(const FeatureComputeCtx<F>* ctx) {
    return FracDiffPriceCompute<F>(ctx, kFracDiff_d04_Coeffs);
}

template <unsigned F>
inline FPN<F> ML_Compute_FracDiffPrice_d05(const FeatureComputeCtx<F>* ctx) {
    return FracDiffPriceCompute<F>(ctx, kFracDiff_d05_Coeffs);
}

template <unsigned F>
inline FPN<F> ML_Compute_FracDiffPrice_d06(const FeatureComputeCtx<F>* ctx) {
    return FracDiffPriceCompute<F>(ctx, kFracDiff_d06_Coeffs);
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
    X(SPREAD_ZSCORE,      "spread_zscore",      1, FEATURE_ENABLED, ML_Compute_SpreadZscore,      "z-score of current spread") \
    /* v5.14.5.B — regime-conditional features. Empirical-verification    */ \
    /*               discipline applies (TECH_DEBT-007): trend_strength + */ \
    /*               vol_zscore have semantic overlap with existing       */ \
    /*               SHORT_SLOPE / VOL_RATIO but differ in normalization. */ \
    /*               Verify post-first-retrain feature_importance gain.   */ \
    X(REGIME_TREND_STRENGTH, "regime_trend_strength", 1, FEATURE_ENABLED, ML_Compute_RegimeTrendStrength, "saturating tanh(short_slope) bounded [-1,1]") \
    X(REGIME_VOL_ZSCORE,     "regime_vol_zscore",     1, FEATURE_ENABLED, ML_Compute_RegimeVolZscore,     "(short_var - long_var) / sqrt(long_var) z-score") \
    X(REGIME_CLASS_ONEHOT,   "regime_class_onehot",   1, FEATURE_ENABLED, ML_Compute_RegimeClassOneHot,   "current_regime as int (0..NUM_REGIMES-1)") \
    /* v5.14.5.C — Marcos Lopez de Prado fractional differentiation       */ \
    /*               (FoxML_Core port). 3 integration orders bracket the  */ \
    /*               typical informative range for crypto tick prices.    */ \
    /*               Cold-start: returns 0 until rolling.count >= K=50.   */ \
    X(FRAC_DIFF_PRICE_D04, "frac_diff_price_d04", 1, FEATURE_ENABLED, ML_Compute_FracDiffPrice_d04, "fractional diff of price (d=0.4); long-memory removed") \
    X(FRAC_DIFF_PRICE_D05, "frac_diff_price_d05", 1, FEATURE_ENABLED, ML_Compute_FracDiffPrice_d05, "fractional diff of price (d=0.5); often the sweet spot") \
    X(FRAC_DIFF_PRICE_D06, "frac_diff_price_d06", 1, FEATURE_ENABLED, ML_Compute_FracDiffPrice_d06, "fractional diff of price (d=0.6); near-stationary residual")

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
//
// Returns:
//   N (>= 0) — number of features written, all valid floats
//   -1       — NaN/Inf detected at index N. Caller MUST treat this as
//              validation failure: zero `out`, log, skip Model_Predict.
//              Single source of truth for feature validation; do NOT
//              re-validate at call sites.
//
// Equivalence test in controller_test.cpp pins out[i] == legacy
// ModelFeatures_Pack buf[i] for ALL 34 indices — load-bearing regression
// guard against future divergence in either path.
//
// v5.9.0 — NaN/Inf validation FOLDED into the packer. Prevents silent
// passthrough of garbage to XGBoost (which can produce NaN output → gate
// decision evaluates false silently → no entry → operator-blind miss).
//
// Two-layer check:
//
// (1) `FPN_IsValidFinite<F>(val_fpn)` — branchless integer magnitude check.
//     Catches FPN_DivNoAssert(x, 0) saturation (FPN_MAX, ~3.4e38 in float
//     space) AND any FPN value > 1e15 (no legitimate feature is this
//     large). FPN itself can't be NaN/Inf — it's an integer — so the
//     float-side check below misses "FPN garbage in normal-finite range."
//
// (2) `std::isnan(_v) || std::isinf(_v)` — float-side post-conversion
//     check. Catches the FPN→float saturation case (FPN_MAX → +Inf in
//     float because float max is 3.4e38, equal to FPN<64>'s post-conversion
//     value). Also catches `FPN_FromDouble(NaN)` paths that propagate
//     through to the output float.
//
// Both layers are mandatory — they cover orthogonal failure modes. Either
// firing returns -1 sentinel; caller MUST treat as validation failure
// (zero `out`, log, skip Model_Predict).
//
// See DOCS/CLAUDE_ML_INVARIANTS.md "Features_PackAll validates output".
//======================================================================================================
template <unsigned F>
inline int Features_PackAll(const FeatureComputeCtx<F>* ctx, float* out) {
    int n = 0;
#define X(id, name, version, enabled, fn, note) \
    if ((enabled)) { \
        FPN<F> _fpn = fn(ctx); \
        if (!FPN_IsValidFinite(_fpn)) { return -1; } \
        float _v = (float)FPN_ToDouble(_fpn); \
        if (std::isnan(_v) || std::isinf(_v)) { return -1; } \
        out[FEATURE_##id] = _v; \
        ++n; \
    }
    FOREACH_FEATURE(X)
#undef X
    return n;
}

//======================================================================================================
// [Features_PackAll mask-aware variant — v5.11.18 main]
//======================================================================================================
// Per /parity-check 2026-05-07 (CRITICAL gap on scaler binding + HIGH gap
// on Features_PackAll index contract). Operator-controlled per-core
// feature subsetting via the cfg field core_<N>_feature_mask
// (uint64_t hex bitmap; 1=feature enabled at runtime).
//
// Sparse-zero contract — IMPORTANT for caller correctness:
//   - Caller passes a buffer of size NUM_REGISTERED_FEATURES floats.
//   - Returned value `n` = NUM_REGISTERED_FEATURES (count of OUTPUT
//     slots written, including the zeroed ones), NOT the count of
//     unmasked features. Caller's downstream code (Scaler_Apply,
//     Model_Predict) expects exactly NUM_REGISTERED_FEATURES inputs.
//   - When mask bit i is unset, out[i] = 0.0f (write the zero
//     explicitly; don't skip the slot). Compute fn() is NOT called
//     for masked features — saves the slow-path math for them.
//   - When mask bit i is set, identical semantics to the no-mask
//     overload above (compute, FPN_IsValidFinite + isnan/isinf
//     guard, return -1 sentinel on any failure).
//
// Why sparse-zero (not dense compression):
//   - Scaler sidecar's mean[] / stddev[] are indexed by
//     FEATURE_<ID>, computed against training data with the SAME
//     mask. A zero-input through standardization at index i
//     produces (0 - mean[i]) / stddev[i] — a deterministic
//     post-scale value the model has already seen during training.
//     Dense compression would shift indices and break this contract.
//   - XGBoost expects fixed-shape input across train + serve. n
//     must always equal NUM_REGISTERED_FEATURES.
//
// Stamp parity:
//   - When mask is non-null AND non-default (i.e., differs from
//     0xFFFF..F all-on), the trained model's stamp MUST have
//     has_feature_mask=1 + feature_mask_train matching the runtime
//     cfg. v5.11.18a's verify_model_stamp pipeline checks this and
//     refuses load on mismatch.
//   - mask=nullptr OR mask=0xFFFF..F → bytewise-identical to
//     pre-v5.11.18 path. Stamp check skipped (legacy stamps load).
//
// Backwards compat: the no-mask overload above stays. Existing
// callers (5 production sites, 4 test sites) compile unchanged
// until they explicitly opt in to mask-aware behavior.
template <unsigned F>
inline int Features_PackAll(const FeatureComputeCtx<F>* ctx, float* out,
                              const uint64_t* mask) {
    if (mask == nullptr) {
        // null mask → no-op; delegate to the no-mask overload for
        // bytewise-identical behavior.
        return Features_PackAll(ctx, out);
    }
    uint64_t m = *mask;
    int n = 0;
#define X(id, name, version, enabled, fn, note) \
    if ((enabled)) { \
        if (m & (1ULL << FEATURE_##id)) { \
            FPN<F> _fpn = fn(ctx); \
            if (!FPN_IsValidFinite(_fpn)) { return -1; } \
            float _v = (float)FPN_ToDouble(_fpn); \
            if (std::isnan(_v) || std::isinf(_v)) { return -1; } \
            out[FEATURE_##id] = _v; \
        } else { \
            out[FEATURE_##id] = 0.0f; \
        } \
        ++n; \
    }
    FOREACH_FEATURE(X)
#undef X
    return n;
}
