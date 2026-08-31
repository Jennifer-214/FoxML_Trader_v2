// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/FeatureRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [FRAMEWORK_DISCIPLINE] [DETERMINISM]]
// [SEAM]_[train-serve feature-set identity — FEATURE_REGISTRY_HASH in every stamp; mismatch = load-time rejection]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ML feature SSoT — one FOREACH_FEATURE row per feature auto-flows enum + names/versions + enabled bitmap + staleness + registry hash + the Features_PackAll packers]
// [CONTAINS]
//   - [STRUCT]_[FeatureComputeCtx]
//   - [REGISTRY]_[FOREACH_FEATURE]   (the 40 ML_Compute_* leaves + registry hash + both Features_PackAll overloads share the block)
//======================================================================================================
// Single source of truth for ML features. Adding a new feature:
//   1. Implement `ML_Compute_<Name>(ctx)` returning FPN_Binary<F>
//   2. Append one row to FOREACH_FEATURE(X)
//   3. Recompile — FEATURE_REGISTRY_HASH flips, old stamps reject at load
//
// All 5 production callers (MLStrategy, StrategyParameters dispatcher,
// BacktestSharded, PortfolioController regime + barrier paths) use
// Features_PackAll. Legacy ModelFeatures_Pack in ModelInference.hpp is a
// deprecated frozen reference — kept only so the EXTENSIBILITY equivalence
// test can validate bytewise parity.
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
//   inline FPN_Binary<F> ML_Compute_<Name>(const FeatureComputeCtx<F>* ctx);
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

//======================================================================
// [STRUCT]_[FeatureComputeCtx]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the input bundle every registered compute fn reads — precomputed RegimeSignals + short rolling + hysteresed regime + the staleness-gate scaffold]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-15]
//======================================================================
// [CODE]
//======================================================================
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
    // _TRENDING / _VOLATILE / _TRENDING_DOWN / _MILD_TREND).
    // Read by ML_Compute_RegimeClassOneHot.
    //
    // CORRECTED 2026-08-30 (PARITY-053). This comment previously claimed the
    // field was "populated at slow-path callers ... for ALL cores" and that a
    // default 0 was "safe for legacy/test paths". BOTH halves were false, and
    // being false HERE is what stopped anyone looking: the LIVE sharded serve
    // seam never populated it, so the default was not a legacy convenience but
    // a live train-serve divergence on an ENABLED feature.
    //
    // There is no longer a "caller doesn't populate" case: construct via
    // FeatureComputeCtx_Build, where this is a REQUIRED parameter.
    int                                   current_regime;

    // v5.14.9.E — TECH_DEBT-015 close (infrastructure-only scaffold).
    // Per-feature staleness gate plumbing. Default values (0 / nullptr)
    // mean "no staleness checks fire" → preserves pre-v5.14.9.E behavior
    // bytewise. Operator opts-in by:
    //   1. Setting max_staleness_minutes > 0 in FOREACH_FEATURE registry
    //      (compile-time; bumps NO hash, preserves train-serve parity)
    //   2. Upstream wiring populates now_us + feature_last_update_us[]
    //      (deferred to operator-driven ship; today scaffolding only)
    //
    // When fully wired: features whose
    //   (now_us - feature_last_update_us[i]) / 60e6 > max_staleness_minutes[i]
    // get zeroed in Features_PackAll output + bump stale_feature_events_total.
    //
    // nullptr-safe: legacy callers can omit; staleness check trivially passes.
    uint64_t                              now_us;                        // wall-clock at slow-path entry; 0 = no check
    const uint64_t*                       feature_last_update_us;        // [NUM_REGISTERED_FEATURES]; nullptr = no check
    uint32_t*                             stale_feature_events_total;    // operator-readable counter; nullptr-safe
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// The bundle of inputs the registered feature compute functions read.
// Each compute fn reads what it needs and ignores the rest.
//
// v5.9.0a — tightened from the v5.8.1a forward-compat shape. Removed
// 11 unused auxiliary fields (long_rolling, medium_rolling,
// baseline_rolling, ema_price, ror, flow, book_imb, spread,
// large_trade, cumdelta, tick_rate, timestamp_us). All currently
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
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[48B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[FeatureComputeCtx]
//======================================================================

//======================================================================
// [FUNCTION]_[FeatureComputeCtx_Build]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONLY sanctioned FeatureComputeCtx construction — every field the compute leaves read is a REQUIRED parameter, so a caller cannot half-populate the bundle and a new field cannot be silently defaulted at one site]
// [REFERENCE]_[PARITY]_[PARITY-053]
// [REFERENCE]_[DESIGN_SPEC]_[[train-serve-execution-layer-parity] [structural-enforcement-when-memory-insufficient]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline FeatureComputeCtx<F> FeatureComputeCtx_Build(
        const RegimeSignals<F>*     signals,
        const RollingStats<F, 128>* short_rolling,
        int                         current_regime) {
    FeatureComputeCtx<F> ctx{};
    ctx.signals        = signals;
    ctx.short_rolling  = short_rolling;
    ctx.current_regime = current_regime;
    // The staleness trio (now_us / feature_last_update_us /
    // stale_feature_events_total) stays at its inert defaults DELIBERATELY:
    // it is a scaffold with zero writers tree-wide, and the gate exists only
    // in the no-mask Features_PackAll overload while live serve uses the mask
    // one. Wiring it here would fire at TRAIN and stay silent at SERVE.
    return ctx;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// WHY A BUILDER (PARITY-053, found by the 2026-08-30 re-gate). The five
// construction sites populated `signals` + `short_rolling` uniformly, but only
// THREE of five set `current_regime` — and the missing one was
// `StrategyParameters.hpp`, the LIVE sharded serve seam. Because
// `FeatureComputeCtx<F> ctx{}` value-initializes, the field read 0 there while
// the backtest collector set the real 0..4, so `regime_class_onehot` TRAINED on
// the regime and SERVED a constant. Nothing crashed and no test failed.
//
// It was invisible for a second reason worth recording: the feature had never
// reached a training matrix at all until the E.1.2.G pre-ship fixed the stride-34
// clobber, so the divergence was inert until the moment it became live.
//
// The class, not the instance: enumerating the SITES (M9) is what the amendment
// did, and it still missed a per-FIELD gap. Only making every read field a
// REQUIRED parameter removes the failure mode — a new ctx field becomes a
// compile error at every site instead of a silent default at some. That is the
// M7 escalation (codified memory proved insufficient at this exact surface).
// `tools/check_feature_ctx_build.py` keeps bare construction from returning.
//======================================================================
// [END_FUNCTION]_[FeatureComputeCtx_Build]
//======================================================================

//======================================================================
// [REGISTRY]_[FOREACH_FEATURE]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[40 feature rows — one row auto-flows FeatureId enum + NAMES/VERSIONS arrays + enabled bitmap + staleness table + FEATURE_REGISTRY_HASH + the Features_PackAll walkers]
// [COLUMN]_[id]_[UPPERCASE token -> FEATURE_<id> enum; IDs contiguous from 0, order LOCKED to historical indices (trained models address by position)]
// [COLUMN]_[name]_[string folded into FEATURE_REGISTRY_HASH + FEATURE_NAMES display table]
// [COLUMN]_[version]_[bump on formula change — flips the hash, forces retrain; never bump for renames/comments]
// [COLUMN]_[enabled]_[FEATURE_ENABLED | FEATURE_DISABLED compile-time gate — skipped by PackAll AND excluded from the hash]
// [COLUMN]_[fn]_[ML_Compute_<Name> leaf matching the canonical ctx signature]
// [COLUMN]_[note]_[human description]
// [COLUMN]_[max_staleness_minutes]_[0 = no check; >0 = zero-substitute when older (v5.14.9.E scaffold; NOT hash-folded)]
// [COLUMN]_[lookback_ticks]_[D-463 — FINITE reach in RAW TICKS (rolling window / frac-diff taps); 0 for time-based features. Hash-FOLDED: it moves the purge gap -> the train/validate split -> the model]
// [COLUMN]_[half_life_us]_[D-463 — EWMA half-life in MICROSECONDS (uint64; a 24h half-life overflows int32); 0 for window features. Stored as TIME because a tick figure needs a rate and engine.cfg:31 documents it varying ~10x. Hash-FOLDED for the same reason as lookback_ticks]
// [REFERENCE]_[INVARIANT]_[[H15] [H21]]
// [REFERENCE]_[TECH_DEBT]_[[TECH_DEBT-7] [TECH_DEBT-13] [TECH_DEBT-15]]
//======================================================================
// [CODE]
//======================================================================
//----------------------------------------------------------------------
// [SECTION]_[FEATURE COMPUTE FUNCTIONS — v5.8.1a first 10 features]
//----------------------------------------------------------------------
// All read from ctx->signals (the pre-computed RegimeSignals bundle).
// FPN_Zero return on null ctx or null signals — safe for cold-start.
//----------------------------------------------------------------------

template <unsigned F>
inline FPN_Binary<F> ML_Compute_ShortSlope(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->short_slope : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_ShortR2(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->short_r2 : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_ShortVariance(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->short_variance : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_LongSlope(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->long_slope : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_LongR2(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->long_r2 : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_LongVariance(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->long_variance : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_VolRatio(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->vol_ratio : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_RorSlope(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->ror_slope : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_VolumeSlope(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->volume_slope : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_VolumeDelta(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->volume_delta : FPN_Zero<F>();
}

//----------------------------------------------------------------------
// [SECTION]_[FEATURE COMPUTE FUNCTIONS — v5.8.1b features 10-33]
//----------------------------------------------------------------------
// Indices 10, 15-33 read from ctx->signals (precomputed RegimeSignals bundle).
// Indices 11-14 are the only ones reading directly from ctx->short_rolling
// (vwap_deviation, price_stddev, price_avg, volume_avg).
//
// double-typed signals (hour_sin/cos, tick_rate_z, flow_*, large_trade_z,
// spread_bps/zscore) round-trip through FPN_FromDouble<F> → FPN_ToDouble →
// float. With F=64 fractional bits the round-trip is lossless to within
// 1/2^64, far below float precision, so Features_PackAll produces the same
// float bits as the legacy direct (float)double cast in ModelFeatures_Pack.
//----------------------------------------------------------------------

template <unsigned F>
inline FPN_Binary<F> ML_Compute_EmaSmaSpread(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->ema_sma_spread : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_VwapDev(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->short_rolling) ? ctx->short_rolling->vwap_deviation : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_PriceStddev(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->short_rolling) ? ctx->short_rolling->price_stddev : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_PriceAvg(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->short_rolling) ? ctx->short_rolling->price_avg : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_VolumeAvg(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->short_rolling) ? ctx->short_rolling->volume_avg : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_EmaAboveSma(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals)
        ? FPN_FromDouble<F>((double)ctx->signals->ema_above_sma)
        : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_MidSlope(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->mid_slope : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_MidR2(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->mid_r2 : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_CumDelta(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->cumdelta : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_HourSin(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->hour_sin) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_HourCos(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->hour_cos) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_VolRegimeRatio(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->vol_regime_ratio : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_TickRateZ(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->tick_rate_z) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_DistToHigh(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->dist_to_high : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_DistToLow(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->dist_to_low : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_BookImbMeanShort(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->book_imb_mean_short : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_BookImbMeanLong(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->book_imb_mean_long : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_BookImbDrift(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? ctx->signals->book_imb_drift : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_Flow10s(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->flow_10s) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_Flow1m(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->flow_1m) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_Flow5m(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->flow_5m) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_LargeTradeZ(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->large_trade_z) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_SpreadBps(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->spread_bps) : FPN_Zero<F>();
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_SpreadZscore(const FeatureComputeCtx<F>* ctx) {
    return (ctx && ctx->signals) ? FPN_FromDouble<F>(ctx->signals->spread_zscore) : FPN_Zero<F>();
}

//----------------------------------------------------------------------
// [SECTION]_[v5.14.5.B — REGIME-CONDITIONAL FEATURES]
//----------------------------------------------------------------------
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
//----------------------------------------------------------------------

// regime_trend_strength: saturating clamp of short_slope to [-1, 1].
// Bounded; sign-preserving. Differs from SHORT_SLOPE (unbounded slope/avg
// ratio) in that extreme values saturate rather than dominating model
// gradient. Verify importance vs SHORT_SLOPE post-train (TECH_DEBT-007).
template <unsigned F>
inline FPN_Binary<F> ML_Compute_RegimeTrendStrength(const FeatureComputeCtx<F>* ctx) {
    if (!ctx || !ctx->signals) return FPN_Zero<F>();
    FPN_Binary<F> x = ctx->signals->short_slope;
    FPN_Binary<F> one = FPN_FromInt<F>(1);
    FPN_Binary<F> neg_one = FPN_Sub(FPN_Zero<F>(), one);
    if (FPN_GreaterThan(x, one)) return one;
    if (FPN_LessThan(x, neg_one)) return neg_one;
    return x;
}

// regime_vol_zscore: (short_var - long_var) / sqrt(long_var) z-score.
// Sign-carrying (positive = elevated vol; negative = depressed).
// Differs from VOL_RATIO (positive ratio) in sign + bounded behavior.
// Verify importance vs VOL_RATIO post-train (TECH_DEBT-007).
template <unsigned F>
inline FPN_Binary<F> ML_Compute_RegimeVolZscore(const FeatureComputeCtx<F>* ctx) {
    if (!ctx || !ctx->signals) return FPN_Zero<F>();
    FPN_Binary<F> short_var = ctx->signals->short_variance;
    FPN_Binary<F> long_var = ctx->signals->long_variance;
    if (FPN_IsZero(long_var)) return FPN_Zero<F>();
    FPN_Binary<F> diff = FPN_Sub(short_var, long_var);
    FPN_Binary<F> denom = FPN_Sqrt(long_var);
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
inline FPN_Binary<F> ML_Compute_RegimeClassOneHot(const FeatureComputeCtx<F>* ctx) {
    if (!ctx) return FPN_Zero<F>();
    return FPN_FromInt<F>(ctx->current_regime);
}

//----------------------------------------------------------------------
// [SECTION]_[v5.14.5.C — FRACTIONAL DIFFERENTIATION FEATURES]
//----------------------------------------------------------------------
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
// Coefficients computed at compile time (constexpr); converted to FPN_Binary
// per-call in Compute fns (50 FPN_FromDouble × 3 fns = ~150 conversions
// per slow-path cycle ≈ ~50ns total; SLOW-PATH only, well within budget).
//
// Train-serve parity: same Compute fn reads same RollingStats price_buf
// ring in both live + backtest paths. Bytewise identical by construction.
//
// FUTURE OPPORTUNITY (v5.16+): same pattern works for volume_buf[]; add
// ML_Compute_FracDiffVolume_d05 etc. as additional features if model
// finds price-frac-diff useful and we want volume-frac-diff isolation.
//----------------------------------------------------------------------

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

// E.1.2.G — the walk, generalized over ANY window size. The ladder's bar-frac-diff
// rows read the 288-slot BucketRingState, and 288 is not a power of two, so the
// `& (W-1)` form cannot serve both consumers.
//
// THE GUARD IS A REQUIRED PARAMETER, and that is the point of this signature.
// The obvious generalization — `(buf, head, W)` — SILENTLY DROPS the
// `count < FRAC_DIFF_K` early-return, because a raw buffer has no `count` and the
// bucket ring has none either (its ordinal replaced it). A pre-coding refute rated
// that the highest-probability silent corruption in this leg: the walk would read
// 50 taps of whatever the buffer happened to contain. Making `available` a
// non-defaulted parameter means a caller CANNOT omit it — the same structural move
// as FeatureComputeCtx_Build's required fields (PARITY-053).
//
// WRAP: the walk decrements by exactly 1 per iteration, so neither a modulo nor a
// power-of-two window is needed inside the loop — one masked conditional add
// suffices, and it is the house `-(uint64_t)pass` idiom. That lets the old
// `static_assert((W & (W-1)) == 0)` be DELETED rather than conditioned
// (structural-fix-over-belt-and-suspenders: remove the special case, do not add
// one). Nothing is lost — RollingStats carries the identical assert INSIDE the
// template, so a non-power-of-two RollingStats cannot be instantiated at all.
//
// SEED: `idx = head % W` before the loop, deliberately. The old `(head-1) & (W-1)`
// did TWO jobs — wrap, and truncate an out-of-range head — and the masked add only
// does the first. Rather than promote `head < W` from defensive to load-bearing,
// the modulo restores the truncation at ONE operation outside the loop (for a
// power-of-two W the compiler emits the same `&`; for 288 a multiply-shift).
template <unsigned F, int W>
inline FPN_Binary<F> FracDiffWalk(const FPN_Binary<F>* buf, int head, int available,
                                  const std::array<double, FRAC_DIFF_K>& coeffs) {
    static_assert(W > 0, "window must be positive");
    if (!buf || available < FRAC_DIFF_K) return FPN_Zero<F>();

    int idx = head % W;                       // truncate any out-of-range head
    idx -= 1;
    idx += W & -(int)(idx < 0);               // branchless wrap; ANY W, no div

    FPN_Binary<F> sum = FPN_Zero<F>();
    for (int k = 0; k < FRAC_DIFF_K; k++) {
        FPN_Binary<F> coeff_fpn = FPN_FromDouble<F>(coeffs[k]);
        FPN_Binary<F> term = FPN_Mul(coeff_fpn, buf[idx]);
        // Sign alternates: even k → add, odd k → subtract. The ternary is on the
        // LOOP COUNTER, so it unrolls at compile time — not a data-dependent branch.
        sum = ((k & 1) == 0) ? FPN_Add(sum, term) : FPN_Sub(sum, term);
        idx -= 1;
        idx += W & -(int)(idx < 0);
    }
    return sum;
}

// Adapter for the rolling-window consumer. W is DERIVED from the instantiation via
// RollingStats::WINDOW rather than re-typed — the previous `constexpr int W = 128`
// was a duplicated constant that indexed price_buf with its own idea of the width,
// so a RollingStats<F,256> would have silently walked the wrong slots.
template <unsigned F>
inline FPN_Binary<F> FracDiffPriceCompute(const FeatureComputeCtx<F>* ctx,
                                    const std::array<double, FRAC_DIFF_K>& coeffs) {
    if (!ctx || !ctx->short_rolling) return FPN_Zero<F>();
    const auto* rs = ctx->short_rolling;
    constexpr int W = (int)std::remove_pointer_t<decltype(rs)>::WINDOW;
    return FracDiffWalk<F, W>(rs->price_buf, rs->head, rs->count, coeffs);
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_FracDiffPrice_d04(const FeatureComputeCtx<F>* ctx) {
    return FracDiffPriceCompute<F>(ctx, kFracDiff_d04_Coeffs);
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_FracDiffPrice_d05(const FeatureComputeCtx<F>* ctx) {
    return FracDiffPriceCompute<F>(ctx, kFracDiff_d05_Coeffs);
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_FracDiffPrice_d06(const FeatureComputeCtx<F>* ctx) {
    return FracDiffPriceCompute<F>(ctx, kFracDiff_d06_Coeffs);
}

//----------------------------------------------------------------------
// [SECTION]_[FEATURE REGISTRY — the X-macro]
//----------------------------------------------------------------------
// (tuple column legend lives in the [COLUMN] lines of this registry's
//  orient block; per-column rationale preserved there)
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
//----------------------------------------------------------------------

#define FEATURE_ENABLED  1
#define FEATURE_DISABLED 0

// v5.14.9.E — extended to 7 columns. New 7th column: max_staleness_minutes
// (0 = disabled; >0 = max age in minutes before feature treated as stale +
// zero-substituted in Features_PackAll + bumps stale_feature_events_total).
// Initial values all 0 (preserves pre-v5.14.9.E behavior bytewise). Operator
// opts in per-feature by editing the registry. Per TECH_DEBT-015 close.
#define FOREACH_FEATURE(X) \
    X(SHORT_SLOPE,        "short_slope",        1, FEATURE_ENABLED, ML_Compute_ShortSlope,        "regression slope, 128-tick window", 0, 128, 0) \
    X(SHORT_R2,           "short_r2",           1, FEATURE_ENABLED, ML_Compute_ShortR2,           "R² of short-window regression", 0, 128, 0) \
    X(SHORT_VARIANCE,     "short_variance",     1, FEATURE_ENABLED, ML_Compute_ShortVariance,     "price variance over 128 ticks", 0, 128, 0) \
    X(LONG_SLOPE,         "long_slope",         1, FEATURE_ENABLED, ML_Compute_LongSlope,         "regression slope, 512-tick window", 0, 512, 0) \
    X(LONG_R2,            "long_r2",            1, FEATURE_ENABLED, ML_Compute_LongR2,            "R² of long-window regression", 0, 512, 0) \
    X(LONG_VARIANCE,      "long_variance",      1, FEATURE_ENABLED, ML_Compute_LongVariance,      "price variance over 512 ticks", 0, 512, 0) \
    X(VOL_RATIO,          "vol_ratio",          1, FEATURE_ENABLED, ML_Compute_VolRatio,          "short variance / long variance", 0, 512, 0) \
    X(ROR_SLOPE,          "ror_slope",          1, FEATURE_ENABLED, ML_Compute_RorSlope,          "regression-on-regression slope", 0, 512, 0) \
    X(VOLUME_SLOPE,       "volume_slope",       1, FEATURE_ENABLED, ML_Compute_VolumeSlope,       "regression slope of volume", 0, 128, 0) \
    X(VOLUME_DELTA,       "volume_delta",       1, FEATURE_ENABLED, ML_Compute_VolumeDelta,       "volume change last vs avg", 0, 128, 0) \
    X(EMA_SMA_SPREAD,     "ema_sma_spread",     1, FEATURE_ENABLED, ML_Compute_EmaSmaSpread,      "(ema - sma) / sma normalized", 0, 512, 0) \
    X(VWAP_DEV,           "vwap_dev",           1, FEATURE_ENABLED, ML_Compute_VwapDev,           "VWAP deviation from short rolling", 0, 128, 0) \
    X(PRICE_STDDEV,       "price_stddev",       1, FEATURE_ENABLED, ML_Compute_PriceStddev,       "stddev of price, short window", 0, 128, 0) \
    X(PRICE_AVG,          "price_avg",          1, FEATURE_ENABLED, ML_Compute_PriceAvg,          "mean price, short window", 0, 128, 0) \
    X(VOLUME_AVG,         "volume_avg",         1, FEATURE_ENABLED, ML_Compute_VolumeAvg,         "mean volume, short window", 0, 128, 0) \
    X(EMA_ABOVE_SMA,      "ema_above_sma",      1, FEATURE_ENABLED, ML_Compute_EmaAboveSma,       "1 if ema > short SMA (binary)", 0, 512, 0) \
    X(MID_SLOPE,          "mid_slope",          1, FEATURE_ENABLED, ML_Compute_MidSlope,          "regression slope, 256-tick window", 0, 256, 0) \
    X(MID_R2,             "mid_r2",             1, FEATURE_ENABLED, ML_Compute_MidR2,             "R² of mid-window regression", 0, 256, 0) \
    X(CUMDELTA,           "cumdelta",           1, FEATURE_ENABLED, ML_Compute_CumDelta,          "rolling cumulative buyer-vs-seller", 0, 1024, 0) \
    X(HOUR_SIN,           "hour_sin",           1, FEATURE_ENABLED, ML_Compute_HourSin,           "cyclical hour-of-day sin", 0, 1, 0) \
    X(HOUR_COS,           "hour_cos",           1, FEATURE_ENABLED, ML_Compute_HourCos,           "cyclical hour-of-day cos", 0, 1, 0) \
    X(VOL_REGIME_RAT,     "vol_regime_rat",     1, FEATURE_ENABLED, ML_Compute_VolRegimeRatio,    "short stddev / baseline stddev", 0, 1024, 0) \
    X(TICK_RATE_Z,        "tick_rate_z",        1, FEATURE_ENABLED, ML_Compute_TickRateZ,         "ticks/sec z-score vs trailing baseline", 0, 1024, 0) \
    X(DIST_TO_HIGH,       "dist_to_high",       1, FEATURE_ENABLED, ML_Compute_DistToHigh,        "(baseline_max - price) / price", 0, 1024, 0) \
    X(DIST_TO_LOW,        "dist_to_low",        1, FEATURE_ENABLED, ML_Compute_DistToLow,         "(price - baseline_min) / price", 0, 1024, 0) \
    X(BOOK_IMB_MEAN_SHORT, "book_imb_mean_short", 1, FEATURE_ENABLED, ML_Compute_BookImbMeanShort, "mean of last 64 book_imbalance samples", 0, 64, 0) \
    X(BOOK_IMB_MEAN_LONG,  "book_imb_mean_long",  1, FEATURE_ENABLED, ML_Compute_BookImbMeanLong,  "mean over full BookImbalanceHistory window", 0, 1024, 0) \
    X(BOOK_IMB_DRIFT,      "book_imb_drift",      1, FEATURE_ENABLED, ML_Compute_BookImbDrift,     "current book_imbalance - mean_long", 0, 1024, 0) \
    X(FLOW_10S,           "flow_10s",           1, FEATURE_ENABLED, ML_Compute_Flow10s,           "signed-volume EWMA, half-life 10s", 0, 0, 10000000) \
    X(FLOW_1M,            "flow_1m",            1, FEATURE_ENABLED, ML_Compute_Flow1m,            "signed-volume EWMA, half-life 60s", 0, 0, 60000000) \
    X(FLOW_5M,            "flow_5m",            1, FEATURE_ENABLED, ML_Compute_Flow5m,            "signed-volume EWMA, half-life 300s", 0, 0, 300000000) \
    X(LARGE_TRADE_Z,      "large_trade_z",      1, FEATURE_ENABLED, ML_Compute_LargeTradeZ,       "z-score of current trade size", 0, 1024, 0) \
    X(SPREAD_BPS,         "spread_bps",         1, FEATURE_ENABLED, ML_Compute_SpreadBps,         "spread / mid_price × 10000", 0, 1, 0) \
    X(SPREAD_ZSCORE,      "spread_zscore",      1, FEATURE_ENABLED, ML_Compute_SpreadZscore,      "z-score of current spread", 0, 1024, 0) \
    /* v5.14.5.B — regime-conditional features. Empirical-verification    */ \
    /*               discipline applies (TECH_DEBT-007): trend_strength + */ \
    /*               vol_zscore have semantic overlap with existing       */ \
    /*               SHORT_SLOPE / VOL_RATIO but differ in normalization. */ \
    /*               Verify post-first-retrain feature_importance gain.   */ \
    X(REGIME_TREND_STRENGTH, "regime_trend_strength", 1, FEATURE_ENABLED, ML_Compute_RegimeTrendStrength, "saturating tanh(short_slope) bounded [-1,1]", 0, 128, 0) \
    X(REGIME_VOL_ZSCORE,     "regime_vol_zscore",     1, FEATURE_ENABLED, ML_Compute_RegimeVolZscore,     "(short_var - long_var) / sqrt(long_var) z-score", 0, 512, 0) \
    X(REGIME_CLASS_ONEHOT,   "regime_class_onehot",   1, FEATURE_ENABLED, ML_Compute_RegimeClassOneHot,   "current_regime as int (0..NUM_REGIMES-1)", 0, 1024, 0) \
    /* v5.14.5.C — Marcos Lopez de Prado fractional differentiation       */ \
    /*               (FoxML_Core port). 3 integration orders bracket the  */ \
    /*               typical informative range for crypto tick prices.    */ \
    /*               Cold-start: returns 0 until rolling.count >= K=50.   */ \
    X(FRAC_DIFF_PRICE_D04, "frac_diff_price_d04", 1, FEATURE_ENABLED, ML_Compute_FracDiffPrice_d04, "fractional diff of price (d=0.4); long-memory removed", 0, 50, 0) \
    X(FRAC_DIFF_PRICE_D05, "frac_diff_price_d05", 1, FEATURE_ENABLED, ML_Compute_FracDiffPrice_d05, "fractional diff of price (d=0.5); often the sweet spot", 0, 50, 0) \
    X(FRAC_DIFF_PRICE_D06, "frac_diff_price_d06", 1, FEATURE_ENABLED, ML_Compute_FracDiffPrice_d06, "fractional diff of price (d=0.6); near-stationary residual", 0, 50, 0)

// Auto-generated FEATURE_<ID> enum constants. Order matches FOREACH_FEATURE.
enum FeatureId : uint16_t {
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us) FEATURE_##id,
    FOREACH_FEATURE(X)
#undef X
    NUM_REGISTERED_FEATURES
};

static_assert(NUM_REGISTERED_FEATURES <= 64,
              "v5.14.9.E: FEATURE_ENABLED_BITMAP is uint64_t; expand if >64 features");

// Names + versions arrays — auto-generated.
static const char* FEATURE_NAMES[] = {
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us) name,
    FOREACH_FEATURE(X)
#undef X
};

static const int FEATURE_VERSIONS[] = {
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us) version,
    FOREACH_FEATURE(X)
#undef X
};

// v5.14.9.E — TECH_DEBT-013 (4) close: byte-per-flag FEATURE_ENABLED_FLAGS[]
// (40 ints = 160 bytes) replaced by FEATURE_ENABLED_BITMAP uint64_t (8 bytes).
// 20× memory shrink + cache-friendly single-word load. Reads via
// IS_FEATURE_ENABLED(i) macro for ergonomics.
//
// Compile-time fold: each enabled feature contributes 1<<FEATURE_##id.
// Bit i set iff feature i has FEATURE_ENABLED in the registry tuple.
#define X_ACCUMULATE_ENABLED(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us) \
    | (((enabled) ? 1ULL : 0ULL) << FEATURE_##id)
static constexpr uint64_t FEATURE_ENABLED_BITMAP =
    0ULL FOREACH_FEATURE(X_ACCUMULATE_ENABLED);
#undef X_ACCUMULATE_ENABLED

#define IS_FEATURE_ENABLED(i) ((FEATURE_ENABLED_BITMAP >> (i)) & 1ULL)

// v5.14.9.E — TECH_DEBT-015 close: per-feature max_staleness_minutes
// (0 = disabled; >0 = max age before feature treated as stale +
// zero-substituted in Features_PackAll). Initial values all 0 (preserves
// pre-v5.14.9.E behavior bytewise). Operator opts in per-feature by
// editing the registry tuple.
static const uint16_t FEATURE_MAX_STALENESS_MINUTES[] = {
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us) (uint16_t)(staleness),
    FOREACH_FEATURE(X)
#undef X
};

static_assert(sizeof(FEATURE_NAMES) / sizeof(*FEATURE_NAMES) == NUM_REGISTERED_FEATURES,
              "FEATURE_NAMES out of sync with NUM_REGISTERED_FEATURES");
static_assert(sizeof(FEATURE_VERSIONS) / sizeof(*FEATURE_VERSIONS) == NUM_REGISTERED_FEATURES,
              "FEATURE_VERSIONS out of sync with NUM_REGISTERED_FEATURES");
static_assert(sizeof(FEATURE_MAX_STALENESS_MINUTES) / sizeof(*FEATURE_MAX_STALENESS_MINUTES)
              == NUM_REGISTERED_FEATURES,
              "FEATURE_MAX_STALENESS_MINUTES out of sync with NUM_REGISTERED_FEATURES");

//----------------------------------------------------------------------
// [SECTION]_[FEATURE REGISTRY HASH — FNV-1a over enabled feature names+versions]
//----------------------------------------------------------------------
// Compile-time hash that contributes to the model fingerprint. Stamp body
// embeds this value at training time; verifier rejects load when it
// differs from the build-time value (= train-serve feature-set mismatch).
//
// DISABLED features don't contribute, so flipping ENABLED → DISABLED also
// flips the hash, forcing retrain.
//----------------------------------------------------------------------

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
    // v5.14.9.E — staleness column added but NOT folded into hash
    // (operator policy, not training-time invariant). FEATURE_REGISTRY_HASH
    // remains stable across staleness changes; bytewise replay-determinism
    // preserved.
    //
    // D-463 — lookback_ticks AND half_life_us ARE folded, and the distinction from
    // staleness is the reason. Staleness only zero-SUBSTITUTES an already-computed
    // value (an operator policy). These two move `FeatureReach_MaxSamples` -> the
    // purge gap -> the train/validate split -> WHICH ROWS THE MODEL SAW. Two builds
    // that disagree about them produce different models while claiming the same
    // feature identity, so a stale artifact must REFUSE at load rather than serve.
    // They are registry columns, not producer-side values, so folding them needs no
    // mirror — that is exactly what made D-459's proposed fold a Class-18 hazard and
    // this one clean.
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us) \
    if ((enabled)) { \
        for (const char* p = name; *p; ++p) \
            h = (h ^ (uint64_t)(uint8_t)*p) * tt::FNV_PRIME_64; \
        const char* vstr = ":v" #version; \
        for (const char* p = vstr; *p; ++p) \
            h = (h ^ (uint64_t)(uint8_t)*p) * tt::FNV_PRIME_64; \
        h = (h ^ (uint64_t)(lookback_ticks)) * tt::FNV_PRIME_64; \
        h = (h ^ (uint64_t)(half_life_us))   * tt::FNV_PRIME_64; \
    }
    FOREACH_FEATURE(X)
#undef X
    return h;
}

inline uint64_t FEATURE_REGISTRY_HASH() {
    static const uint64_t h = feature_registry_hash_compute();
    return h;
}

//----------------------------------------------------------------------
// [SECTION]_[FEATURES_PACK_ALL — the packer]
//----------------------------------------------------------------------
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
//     space) AND any FPN_Binary value > 1e15 (no legitimate feature is this
//     large). FPN_Binary itself can't be NaN/Inf — it's an integer — so the
//     float-side check below misses "FPN_Binary garbage in normal-finite range."
//
// (2) `std::isnan(_v) || std::isinf(_v)` — float-side post-conversion
//     check. Catches the FPN_Binary→float saturation case (FPN_MAX → +Inf in
//     float because float max is 3.4e38, equal to FPN_Binary<64>'s post-conversion
//     value). Also catches `FPN_FromDouble(NaN)` paths that propagate
//     through to the output float.
//
// Both layers are mandatory — they cover orthogonal failure modes. Either
// firing returns -1 sentinel; caller MUST treat as validation failure
// (zero `out`, log, skip Model_Predict).
//
// See DOCS/CLAUDE_ML_INVARIANTS.md "Features_PackAll validates output".
//----------------------------------------------------------------------
template <unsigned F>
inline int Features_PackAll(const FeatureComputeCtx<F>* ctx, float* out) {
    int n = 0;
    // v5.14.9.E — per-feature staleness scaffold. No-op when ctx fields
    // are 0/nullptr (legacy/test/default path). Operator opts in by:
    //   1. Setting (staleness) > 0 in FOREACH_FEATURE registry tuple
    //   2. Caller populates now_us + feature_last_update_us[] + counter
    // do-while wrapper enables flag-skip pattern (X-macro inline sequence
    // doesn't permit `continue` since FOREACH_FEATURE expands to a sequence
    // of statements, not a loop body).
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us) \
    do { \
        if (!(enabled)) break; \
        bool _stale_skip = false; \
        if (ctx && (staleness) > 0 && ctx->now_us > 0 && \
            ctx->feature_last_update_us != nullptr && \
            ctx->feature_last_update_us[FEATURE_##id] > 0) { \
            uint64_t _age_us = ctx->now_us - ctx->feature_last_update_us[FEATURE_##id]; \
            if (_age_us / 60000000ULL > (uint64_t)(staleness)) { \
                out[FEATURE_##id] = 0.0f; \
                if (ctx->stale_feature_events_total) (*ctx->stale_feature_events_total)++; \
                ++n; \
                _stale_skip = true; \
            } \
        } \
        if (!_stale_skip) { \
            FPN_Binary<F> _fpn = fn(ctx); \
            if (!FPN_IsValidFinite(_fpn)) { return -1; } \
            float _v = (float)FPN_ToDouble(_fpn); \
            if (std::isnan(_v) || std::isinf(_v)) { return -1; } \
            out[FEATURE_##id] = _v; \
            ++n; \
        } \
    } while (0);
    FOREACH_FEATURE(X)
#undef X
    return n;
}

//----------------------------------------------------------------------
// [SECTION]_[Features_PackAll mask-aware variant — v5.11.18 main]
//----------------------------------------------------------------------
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
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us) \
    if ((enabled)) { \
        if (m & (1ULL << FEATURE_##id)) { \
            FPN_Binary<F> _fpn = fn(ctx); \
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_REGISTRY]_[FOREACH_FEATURE]
//======================================================================
