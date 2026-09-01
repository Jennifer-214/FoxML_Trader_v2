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

    // E.1.2.G — the 24h bucket ring (288 x 5min). Read by the extrema rows
    // (dist_to_high_24h / dist_to_low_24h / range_pos_24h) and the bar-frac-diff
    // rows. REQUIRED at FeatureComputeCtx_Build, like current_regime and for the
    // same reason: PARITY-053 was a field that three of five sites populated, and
    // a pointer defaulting to nullptr here would reproduce it exactly — the rows
    // would return zero at whichever seam forgot, silently.
    const BucketRingState*                bucket_ring;

    // E.1.2.G — the node's FlowState, for the ladder's ten long-half-life
    // accumulators. REQUIRED at FeatureComputeCtx_Build like its siblings: the
    // rows that read it are the ship's entire payload, and a nullable pointer here
    // would let one seam serve zeros while another trains on real values — which is
    // PARITY-053 restated on the very features this ship adds.
    const FlowState*                      flow_state;

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
        int                         current_regime,
        const BucketRingState*      bucket_ring,
        const FlowState*            flow_state) {
    FeatureComputeCtx<F> ctx{};
    ctx.signals        = signals;
    ctx.short_rolling  = short_rolling;
    ctx.current_regime = current_regime;
    ctx.bucket_ring    = bucket_ring;
    ctx.flow_state     = flow_state;
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
// [COLUMN]_[min_history_us]_[warm-up floor in MICROSECONDS; 0 = no gate. Compared ONCE in the Features_PackAll walker against the ctx's data span (D-467) — never inside a leaf, which receives no row identity and would have to hardcode its own FEATURE_* index. Hash-FOLDED: it changes computed values, so two builds with different floors must not claim one feature identity]
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
// [SECTION]_[E.1.2.G — the feature-horizon ladder leaves]
//----------------------------------------------------------------------
// The rows that let the model see past ~17 seconds. Every one reads state the
// slow path already maintains; none adds a feed.
//
// PRICE SOURCE: ctx->short_rolling->price_avg, the 128-tick window mean. Chosen
// over threading a raw last-price through the ctx because it is ALREADY in reach,
// deterministic, and identical on both seams — a new pointer would be one more
// thing two paths could populate differently (PARITY-053's shape).
//
// (A)(4) GUARD PLACEMENT — every divisive leaf tests its denominator BEFORE
// dividing and returns FPN_Zero, rather than letting FPN_DivNoAssert saturate to
// FPN_MAX. That is not defensive style, it is required: a saturated value trips
// FPN_IsValidFinite in Features_PackAll, and BOTH overloads then `return -1`,
// discarding ALL 60 FEATURES rather than the one bad row. A guard at the pack
// layer is too late by construction.
#define LADDER_PRICE(ctx) ((ctx)->short_rolling->price_avg)

// --- helper: (value - ref) / ref, guarded. The shape four rows share. -------
template <unsigned F>
inline FPN_Binary<F> LadderRelDev(FPN_Binary<F> value, FPN_Binary<F> ref) {
    if (FPN_IsZero(ref)) return FPN_Zero<F>();
    return FPN_DivNoAssert(FPN_Sub(value, ref), ref);
}

// --- T1: price momentum at four horizons ------------------------------------
// (price - EMA)/EMA. The EMA is an AVERAGE accumulator (D-465), so it has a fixed
// point at the price and this ratio is half-life-STABLE. Under a SUM it would pin
// near -1.0 and carry tick rate instead of momentum.
#define LADDER_RET_EMA(NAME, FIELD)                                              \
    template <unsigned F>                                                        \
    inline FPN_Binary<F> ML_Compute_##NAME(const FeatureComputeCtx<F>* ctx) {     \
        if (!ctx || !ctx->short_rolling || !ctx->flow_state) return FPN_Zero<F>(); \
        return LadderRelDev<F>(LADDER_PRICE(ctx), ctx->flow_state->FIELD.v);      \
    }
LADDER_RET_EMA(RetEma30m, ret_ema_30m)
LADDER_RET_EMA(RetEma2h,  ret_ema_2h)
LADDER_RET_EMA(RetEma8h,  ret_ema_8h)
LADDER_RET_EMA(RetEma24h, ret_ema_24h)
#undef LADDER_RET_EMA

// --- T1: signed-volume flow at two horizons ---------------------------------
// Read straight through: a decaying SUM's accumulated total IS the signal, so
// there is nothing to normalise against.
#define LADDER_FLOW(NAME, FIELD)                                                 \
    template <unsigned F>                                                        \
    inline FPN_Binary<F> ML_Compute_##NAME(const FeatureComputeCtx<F>* ctx) {     \
        if (!ctx || !ctx->flow_state) return FPN_Zero<F>();                      \
        return ctx->flow_state->FIELD.v;                                         \
    }
LADDER_FLOW(Flow30m, flow_30m)
LADDER_FLOW(Flow2h,  flow_2h)
LADDER_FLOW(Rvol1h,  rvol_1h)
LADDER_FLOW(Rvol8h,  rvol_8h)
#undef LADDER_FLOW

// --- T1: realized-vol squeeze/expansion -------------------------------------
// GUARDED DIVIDE (A)(4): rvol_8h is zero for the whole warm-up, so this is not a
// rare edge — it is the state every cold start passes through.
template <unsigned F>
inline FPN_Binary<F> ML_Compute_RvolRatio(const FeatureComputeCtx<F>* ctx) {
    if (!ctx || !ctx->flow_state) return FPN_Zero<F>();
    const FPN_Binary<F> num = ctx->flow_state->rvol_1h.v;
    const FPN_Binary<F> den = ctx->flow_state->rvol_8h.v;
    if (FPN_IsZero(den)) return FPN_Zero<F>();
    return FPN_DivNoAssert(num, den);
}

// --- T2: VWAP deviation ------------------------------------------------------
// vwap = EWMA(P*V)/EWMA(V), then (price - vwap)/vwap. TWO guarded divides. Both
// legs are AVERAGE at the SAME half-life; a MIX of forms would inflate the ratio
// ~432,000x, which is why the recurrence lives in the TYPE and not in a column.
template <unsigned F>
inline FPN_Binary<F> ML_Compute_VwapEma24hDev(const FeatureComputeCtx<F>* ctx) {
    if (!ctx || !ctx->short_rolling || !ctx->flow_state) return FPN_Zero<F>();
    const FPN_Binary<F> v = ctx->flow_state->vwap_v_24h.v;
    if (FPN_IsZero(v)) return FPN_Zero<F>();
    const FPN_Binary<F> vwap = FPN_DivNoAssert(ctx->flow_state->vwap_pv_24h.v, v);
    return LadderRelDev<F>(LADDER_PRICE(ctx), vwap);
}

// --- T2: 24h extrema, from the bucket ring ----------------------------------
// A slot counts only when its 1-BASED stamp names a bucket within the last 288.
// Zero-stamped slots are NEVER-WRITTEN and are skipped — without that, a cold or
// gapped ring would fold zero-priced buckets into the extrema as if measured.
template <unsigned F>
inline bool LadderRingExtrema(const BucketRingState* r,
                              FPN_Binary<F>* out_min, FPN_Binary<F>* out_max) {
    if (!r) return false;
    bool any = false;
    FPN_Binary<F> lo = FPN_Zero<F>(), hi = FPN_Zero<F>();
    const uint32_t cur1 = r->cur_ordinal + 1u;
    for (int i = 0; i < BUCKET_SLOTS; i++) {
        const uint32_t stamp = r->slot_bucket[i];
        const uint32_t age   = cur1 - stamp;
        if (stamp == 0u || age >= (uint32_t)BUCKET_SLOTS) continue;
        if (!any) { lo = r->min[i]; hi = r->max[i]; any = true; continue; }
        if (FPN_LessThan(r->min[i], lo)) lo = r->min[i];
        if (FPN_LessThan(hi, r->max[i])) hi = r->max[i];
    }
    *out_min = lo; *out_max = hi;
    return any;
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_DistToHigh24h(const FeatureComputeCtx<F>* ctx) {
    if (!ctx || !ctx->short_rolling) return FPN_Zero<F>();
    FPN_Binary<F> lo, hi;
    if (!LadderRingExtrema<F>(ctx->bucket_ring, &lo, &hi)) return FPN_Zero<F>();
    return LadderRelDev<F>(hi, LADDER_PRICE(ctx));
}

template <unsigned F>
inline FPN_Binary<F> ML_Compute_DistToLow24h(const FeatureComputeCtx<F>* ctx) {
    if (!ctx || !ctx->short_rolling) return FPN_Zero<F>();
    FPN_Binary<F> lo, hi;
    if (!LadderRingExtrema<F>(ctx->bucket_ring, &lo, &hi)) return FPN_Zero<F>();
    return LadderRelDev<F>(LADDER_PRICE(ctx), lo);
}

// GUARDED DIVIDE (A)(4): (max - min) is EXACTLY zero on a single-bucket ring,
// which every cold start and every quiet 5-minute window produces.
template <unsigned F>
inline FPN_Binary<F> ML_Compute_RangePos24h(const FeatureComputeCtx<F>* ctx) {
    if (!ctx || !ctx->short_rolling) return FPN_Zero<F>();
    FPN_Binary<F> lo, hi;
    if (!LadderRingExtrema<F>(ctx->bucket_ring, &lo, &hi)) return FPN_Zero<F>();
    const FPN_Binary<F> span = FPN_Sub(hi, lo);
    if (FPN_IsZero(span)) return FPN_Zero<F>();
    return FPN_DivNoAssert(FPN_Sub(LADDER_PRICE(ctx), lo), span);
}

// --- T3: fractional differencing over BARS ----------------------------------
// FracDiffWalk over the ring's close[] at W=288. `available` is the count of
// valid slots — a REQUIRED argument, which is what stops the guard vanishing at
// exactly the seam where the ring has no `count` of its own.
template <unsigned F>
inline int LadderRingValidCount(const BucketRingState* r) {
    if (!r) return 0;
    int n = 0;
    const uint32_t cur1 = r->cur_ordinal + 1u;
    for (int i = 0; i < BUCKET_SLOTS; i++) {
        const uint32_t stamp = r->slot_bucket[i];
        if (stamp != 0u && (uint32_t)(cur1 - stamp) < (uint32_t)BUCKET_SLOTS) n++;
    }
    return n;
}

#define LADDER_FRACDIFF_BARS(NAME, COEFFS)                                        \
    template <unsigned F>                                                         \
    inline FPN_Binary<F> ML_Compute_##NAME(const FeatureComputeCtx<F>* ctx) {      \
        if (!ctx || !ctx->bucket_ring) return FPN_Zero<F>();                      \
        const int head = (int)((ctx->bucket_ring->cur_ordinal + 1u)               \
                               % (uint32_t)BUCKET_SLOTS);                         \
        return FracDiffWalk<F, BUCKET_SLOTS>(ctx->bucket_ring->close, head,       \
                                             LadderRingValidCount<F>(ctx->bucket_ring), \
                                             COEFFS);                             \
    }
LADDER_FRACDIFF_BARS(FracDiffBars_d04, kFracDiff_d04_Coeffs)
LADDER_FRACDIFF_BARS(FracDiffBars_d05, kFracDiff_d05_Coeffs)
LADDER_FRACDIFF_BARS(FracDiffBars_d06, kFracDiff_d06_Coeffs)
#undef LADDER_FRACDIFF_BARS

// --- T2/T3: cyclical phase rows ---------------------------------------------
// Pure functions of the DATA timestamp — no feed, deterministic, and
// backtest-reproducible by construction. FPN_Sin/FPN_Cos, never libm (H15).
// The 8h row is the perp funding cycle's 3rd harmonic, which hour_sin/cos cannot
// give a tree cheaply.
#define LADDER_PHASE(NAME, PERIOD_US, FN)                                         \
    template <unsigned F>                                                         \
    inline FPN_Binary<F> ML_Compute_##NAME(const FeatureComputeCtx<F>* ctx) {      \
        if (!ctx || !ctx->flow_state) return FPN_Zero<F>();                       \
        const uint64_t phase = ctx->flow_state->last_us % (uint64_t)(PERIOD_US);   \
        const FPN_Binary<F> frac =                                                \
            FPN_DivNoAssert(FPN_FromInt<F>((int64_t)phase),                        \
                            FPN_FromInt<F>((int64_t)(PERIOD_US)));                 \
        return FN(FPN_Mul(FPN_FromDouble<F>(6.283185307179586), frac));            \
    }
LADDER_PHASE(DowSin,          604800000000ULL, FPN_Sin)   // 7d week
LADDER_PHASE(DowCos,          604800000000ULL, FPN_Cos)
LADDER_PHASE(FundingPhaseSin,  28800000000ULL, FPN_Sin)   // 8h funding cycle
LADDER_PHASE(FundingPhaseCos,  28800000000ULL, FPN_Cos)
#undef LADDER_PHASE

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
    X(SHORT_SLOPE,        "short_slope",        1, FEATURE_ENABLED, ML_Compute_ShortSlope,        "regression slope, 128-tick window", 0, 128, 0, 0) \
    X(SHORT_R2,           "short_r2",           1, FEATURE_ENABLED, ML_Compute_ShortR2,           "R² of short-window regression", 0, 128, 0, 0) \
    X(SHORT_VARIANCE,     "short_variance",     1, FEATURE_ENABLED, ML_Compute_ShortVariance,     "price variance over 128 ticks", 0, 128, 0, 0) \
    X(LONG_SLOPE,         "long_slope",         1, FEATURE_ENABLED, ML_Compute_LongSlope,         "regression slope, 512-tick window", 0, 512, 0, 0) \
    X(LONG_R2,            "long_r2",            1, FEATURE_ENABLED, ML_Compute_LongR2,            "R² of long-window regression", 0, 512, 0, 0) \
    X(LONG_VARIANCE,      "long_variance",      1, FEATURE_ENABLED, ML_Compute_LongVariance,      "price variance over 512 ticks", 0, 512, 0, 0) \
    X(VOL_RATIO,          "vol_ratio",          1, FEATURE_ENABLED, ML_Compute_VolRatio,          "short variance / long variance", 0, 512, 0, 0) \
    X(ROR_SLOPE,          "ror_slope",          1, FEATURE_ENABLED, ML_Compute_RorSlope,          "regression-on-regression slope", 0, 512, 0, 0) \
    X(VOLUME_SLOPE,       "volume_slope",       1, FEATURE_ENABLED, ML_Compute_VolumeSlope,       "regression slope of volume", 0, 128, 0, 0) \
    X(VOLUME_DELTA,       "volume_delta",       1, FEATURE_ENABLED, ML_Compute_VolumeDelta,       "volume change last vs avg", 0, 128, 0, 0) \
    X(EMA_SMA_SPREAD,     "ema_sma_spread",     1, FEATURE_ENABLED, ML_Compute_EmaSmaSpread,      "(ema - sma) / sma normalized", 0, 512, 0, 0) \
    X(VWAP_DEV,           "vwap_dev",           1, FEATURE_ENABLED, ML_Compute_VwapDev,           "VWAP deviation from short rolling", 0, 128, 0, 0) \
    X(PRICE_STDDEV,       "price_stddev",       1, FEATURE_ENABLED, ML_Compute_PriceStddev,       "stddev of price, short window", 0, 128, 0, 0) \
    X(PRICE_AVG,          "price_avg",          1, FEATURE_ENABLED, ML_Compute_PriceAvg,          "mean price, short window", 0, 128, 0, 0) \
    X(VOLUME_AVG,         "volume_avg",         1, FEATURE_ENABLED, ML_Compute_VolumeAvg,         "mean volume, short window", 0, 128, 0, 0) \
    X(EMA_ABOVE_SMA,      "ema_above_sma",      1, FEATURE_ENABLED, ML_Compute_EmaAboveSma,       "1 if ema > short SMA (binary)", 0, 512, 0, 0) \
    X(MID_SLOPE,          "mid_slope",          1, FEATURE_ENABLED, ML_Compute_MidSlope,          "regression slope, 256-tick window", 0, 256, 0, 0) \
    X(MID_R2,             "mid_r2",             1, FEATURE_ENABLED, ML_Compute_MidR2,             "R² of mid-window regression", 0, 256, 0, 0) \
    X(CUMDELTA,           "cumdelta",           1, FEATURE_ENABLED, ML_Compute_CumDelta,          "rolling cumulative buyer-vs-seller", 0, 1024, 0, 0) \
    X(HOUR_SIN,           "hour_sin",           1, FEATURE_ENABLED, ML_Compute_HourSin,           "cyclical hour-of-day sin", 0, 1, 0, 0) \
    X(HOUR_COS,           "hour_cos",           1, FEATURE_ENABLED, ML_Compute_HourCos,           "cyclical hour-of-day cos", 0, 1, 0, 0) \
    X(VOL_REGIME_RAT,     "vol_regime_rat",     1, FEATURE_ENABLED, ML_Compute_VolRegimeRatio,    "short stddev / baseline stddev", 0, 1024, 0, 0) \
    X(TICK_RATE_Z,        "tick_rate_z",        1, FEATURE_ENABLED, ML_Compute_TickRateZ,         "ticks/sec z-score vs trailing baseline", 0, 1024, 0, 0) \
    X(DIST_TO_HIGH,       "dist_to_high",       1, FEATURE_ENABLED, ML_Compute_DistToHigh,        "(baseline_max - price) / price", 0, 1024, 0, 0) \
    X(DIST_TO_LOW,        "dist_to_low",        1, FEATURE_ENABLED, ML_Compute_DistToLow,         "(price - baseline_min) / price", 0, 1024, 0, 0) \
    X(BOOK_IMB_MEAN_SHORT, "book_imb_mean_short", 1, FEATURE_ENABLED, ML_Compute_BookImbMeanShort, "mean of last 64 book_imbalance samples", 0, 64, 0, 0) \
    X(BOOK_IMB_MEAN_LONG,  "book_imb_mean_long",  1, FEATURE_ENABLED, ML_Compute_BookImbMeanLong,  "mean over full BookImbalanceHistory window", 0, 1024, 0, 0) \
    X(BOOK_IMB_DRIFT,      "book_imb_drift",      1, FEATURE_ENABLED, ML_Compute_BookImbDrift,     "current book_imbalance - mean_long", 0, 1024, 0, 0) \
    X(FLOW_10S,           "flow_10s",           1, FEATURE_ENABLED, ML_Compute_Flow10s,           "signed-volume EWMA, half-life 10s", 0, 0, 10000000, 0) \
    X(FLOW_1M,            "flow_1m",            1, FEATURE_ENABLED, ML_Compute_Flow1m,            "signed-volume EWMA, half-life 60s", 0, 0, 60000000, 0) \
    X(FLOW_5M,            "flow_5m",            1, FEATURE_ENABLED, ML_Compute_Flow5m,            "signed-volume EWMA, half-life 300s", 0, 0, 300000000, 0) \
    X(LARGE_TRADE_Z,      "large_trade_z",      1, FEATURE_ENABLED, ML_Compute_LargeTradeZ,       "z-score of current trade size", 0, 1024, 0, 0) \
    X(SPREAD_BPS,         "spread_bps",         1, FEATURE_ENABLED, ML_Compute_SpreadBps,         "spread / mid_price × 10000", 0, 1, 0, 0) \
    X(SPREAD_ZSCORE,      "spread_zscore",      1, FEATURE_ENABLED, ML_Compute_SpreadZscore,      "z-score of current spread", 0, 1024, 0, 0) \
    /* v5.14.5.B — regime-conditional features. Empirical-verification    */ \
    /*               discipline applies (TECH_DEBT-007): trend_strength + */ \
    /*               vol_zscore have semantic overlap with existing       */ \
    /*               SHORT_SLOPE / VOL_RATIO but differ in normalization. */ \
    /*               Verify post-first-retrain feature_importance gain.   */ \
    X(REGIME_TREND_STRENGTH, "regime_trend_strength", 1, FEATURE_ENABLED, ML_Compute_RegimeTrendStrength, "saturating tanh(short_slope) bounded [-1,1]", 0, 128, 0, 0) \
    X(REGIME_VOL_ZSCORE,     "regime_vol_zscore",     1, FEATURE_ENABLED, ML_Compute_RegimeVolZscore,     "(short_var - long_var) / sqrt(long_var) z-score", 0, 512, 0, 0) \
    X(REGIME_CLASS_ONEHOT,   "regime_class_onehot",   1, FEATURE_ENABLED, ML_Compute_RegimeClassOneHot,   "current_regime as int (0..NUM_REGIMES-1)", 0, 1024, 0, 0) \
    /* v5.14.5.C — Marcos Lopez de Prado fractional differentiation       */ \
    /*               (FoxML_Core port). 3 integration orders bracket the  */ \
    /*               typical informative range for crypto tick prices.    */ \
    /*               Cold-start: returns 0 until rolling.count >= K=50.   */ \
    X(FRAC_DIFF_PRICE_D04, "frac_diff_price_d04", 1, FEATURE_ENABLED, ML_Compute_FracDiffPrice_d04, "fractional diff of price (d=0.4); long-memory removed", 0, 50, 0, 0) \
    X(FRAC_DIFF_PRICE_D05, "frac_diff_price_d05", 1, FEATURE_ENABLED, ML_Compute_FracDiffPrice_d05, "fractional diff of price (d=0.5); often the sweet spot", 0, 50, 0, 0) \
    X(FRAC_DIFF_PRICE_D06, "frac_diff_price_d06", 1, FEATURE_ENABLED, ML_Compute_FracDiffPrice_d06, "fractional diff of price (d=0.6); near-stationary residual", 0, 50, 0, 0) \
    /* ── E.1.2.G: the feature-horizon ladder (rows 40-59) ────────────────── */ \
    /* min_history_us is ~3 half-lives per row — where an EWMA is within ~12% */ \
    /* of its asymptote. lookback_ticks stays 0: an EWMA has no finite TICK   */ \
    /* reach, which is precisely the distinction D-463's column split made.   */ \
    X(RET_EMA_30M,        "ret_ema_30m",        1, FEATURE_ENABLED, ML_Compute_RetEma30m,        "(price-EMA)/EMA, HL 30m", 0, 0, 1800000000, 5400000000) \
    X(RET_EMA_2H,         "ret_ema_2h",         1, FEATURE_ENABLED, ML_Compute_RetEma2h,         "(price-EMA)/EMA, HL 2h", 0, 0, 7200000000, 21600000000) \
    X(RET_EMA_8H,         "ret_ema_8h",         1, FEATURE_ENABLED, ML_Compute_RetEma8h,         "(price-EMA)/EMA, HL 8h", 0, 0, 28800000000, 86400000000) \
    X(RET_EMA_24H,        "ret_ema_24h",        1, FEATURE_ENABLED, ML_Compute_RetEma24h,        "(price-EMA)/EMA, HL 24h (T3)", 0, 0, 86400000000, 259200000000) \
    X(FLOW_30M,           "flow_30m",           1, FEATURE_ENABLED, ML_Compute_Flow30m,          "signed-volume EWMA, HL 30m", 0, 0, 1800000000, 5400000000) \
    X(FLOW_2H,            "flow_2h",            1, FEATURE_ENABLED, ML_Compute_Flow2h,           "signed-volume EWMA, HL 2h", 0, 0, 7200000000, 21600000000) \
    X(RVOL_1H,            "rvol_1h",            1, FEATURE_ENABLED, ML_Compute_Rvol1h,           "EWMA of squared simple returns, HL 1h", 0, 0, 3600000000, 10800000000) \
    X(RVOL_8H,            "rvol_8h",            1, FEATURE_ENABLED, ML_Compute_Rvol8h,           "EWMA of squared simple returns, HL 8h", 0, 0, 28800000000, 86400000000) \
    X(RVOL_RATIO,         "rvol_ratio",         1, FEATURE_ENABLED, ML_Compute_RvolRatio,        "rvol_1h/rvol_8h — vol squeeze vs expansion", 0, 0, 28800000000, 86400000000) \
    X(VWAP_EMA_24H_DEV,   "vwap_ema_24h_dev",   1, FEATURE_ENABLED, ML_Compute_VwapEma24hDev,    "(price-vwap)/vwap, EWMA legs at HL 24h", 0, 0, 86400000000, 259200000000) \
    X(DIST_TO_HIGH_24H,   "dist_to_high_24h",   1, FEATURE_ENABLED, ML_Compute_DistToHigh24h,    "(max24h-price)/price, from the bucket ring", 0, 0, 0, 86400000000) \
    X(DIST_TO_LOW_24H,    "dist_to_low_24h",    1, FEATURE_ENABLED, ML_Compute_DistToLow24h,     "(price-min24h)/price, from the bucket ring", 0, 0, 0, 86400000000) \
    X(RANGE_POS_24H,      "range_pos_24h",      1, FEATURE_ENABLED, ML_Compute_RangePos24h,      "(price-min)/(max-min) — position in the 24h range", 0, 0, 0, 86400000000) \
    X(FRAC_DIFF_BARS_D04, "frac_diff_bars_d04", 1, FEATURE_ENABLED, ML_Compute_FracDiffBars_d04, "frac diff (d=0.4) over 5min bar closes; 50 taps = 4.2h", 0, 0, 0, 15000000000) \
    X(FRAC_DIFF_BARS_D05, "frac_diff_bars_d05", 1, FEATURE_ENABLED, ML_Compute_FracDiffBars_d05, "frac diff (d=0.5) over 5min bar closes", 0, 0, 0, 15000000000) \
    X(FRAC_DIFF_BARS_D06, "frac_diff_bars_d06", 1, FEATURE_ENABLED, ML_Compute_FracDiffBars_d06, "frac diff (d=0.6) over 5min bar closes", 0, 0, 0, 15000000000) \
    X(DOW_SIN,            "dow_sin",            1, FEATURE_ENABLED, ML_Compute_DowSin,           "day-of-week cyclical (sin); pure fn of the data timestamp", 0, 0, 0, 0) \
    X(DOW_COS,            "dow_cos",            1, FEATURE_ENABLED, ML_Compute_DowCos,           "day-of-week cyclical (cos)", 0, 0, 0, 0) \
    X(FUNDING_PHASE_SIN,  "funding_phase_sin",  1, FEATURE_ENABLED, ML_Compute_FundingPhaseSin,  "8h perp funding-cycle phase (sin); 3rd harmonic of the 24h cycle", 0, 0, 0, 0) \
    X(FUNDING_PHASE_COS,  "funding_phase_cos",  1, FEATURE_ENABLED, ML_Compute_FundingPhaseCos,  "8h perp funding-cycle phase (cos)", 0, 0, 0, 0)

// Auto-generated FEATURE_<ID> enum constants. Order matches FOREACH_FEATURE.
enum FeatureId : uint16_t {
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us, min_history_us) FEATURE_##id,
    FOREACH_FEATURE(X)
#undef X
    NUM_REGISTERED_FEATURES
};

static_assert(NUM_REGISTERED_FEATURES <= 64,
              "v5.14.9.E: FEATURE_ENABLED_BITMAP is uint64_t; expand if >64 features");

// Names + versions arrays — auto-generated.
static const char* FEATURE_NAMES[] = {
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us, min_history_us) name,
    FOREACH_FEATURE(X)
#undef X
};

static const int FEATURE_VERSIONS[] = {
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us, min_history_us) version,
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
#define X_ACCUMULATE_ENABLED(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us, min_history_us) \
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
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us, min_history_us) (uint16_t)(staleness),
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
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us, min_history_us) \
    if ((enabled)) { \
        for (const char* p = name; *p; ++p) \
            h = (h ^ (uint64_t)(uint8_t)*p) * tt::FNV_PRIME_64; \
        const char* vstr = ":v" #version; \
        for (const char* p = vstr; *p; ++p) \
            h = (h ^ (uint64_t)(uint8_t)*p) * tt::FNV_PRIME_64; \
        h = (h ^ (uint64_t)(lookback_ticks)) * tt::FNV_PRIME_64; \
        h = (h ^ (uint64_t)(half_life_us))   * tt::FNV_PRIME_64; \
        h = (h ^ (uint64_t)(min_history_us)) * tt::FNV_PRIME_64; \
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
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us, min_history_us) \
    do { \
        if (!(enabled)) break; \
        bool _stale_skip = false; \
        /* E.1.2.G (D-467) — MIN-HISTORY GATE. A row whose warm-up floor is unmet is \
           NOT MEASURED and must not be served as if it were: a 24h EWMA seeded at \
           first price reads a near-zero deviation for a full day — a plausible \
           number carrying no information. Compared HERE because the walker is the \
           ONLY site holding both the row's COLUMN and its FEATURE_##id INDEX; a leaf \
           gets no row identity and would hardcode its own index, so a copy-pasted \
           leaf citing a sibling's index would gate on the wrong threshold silently. */ \
        if (ctx && (min_history_us) > 0 && \
            FlowState_DataSpanUs(ctx->flow_state) < (uint64_t)(min_history_us)) { \
            out[FEATURE_##id] = 0.0f; \
            ++n; \
            _stale_skip = true; \
        } \
        if (!_stale_skip && ctx && (staleness) > 0 && ctx->now_us > 0 && \
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
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us, min_history_us) \
    if ((enabled)) { \
        /* E.1.2.G (D-467) — the SAME min-history gate, in the MASK overload too. \
           This is not duplication for its own sake: the pre-existing `staleness` \
           gate lives ONLY in the no-mask overload, and LIVE SERVE uses THIS one — \
           so a gate modelled on staleness would fire at TRAIN and stay silent at \
           SERVE, inverting the very M5 contract this ship declares BINDING. A \
           masked-off row and an unwarmed row both write 0.0f, so one condition \
           covers both reasons. */ \
        const bool _mh_ok = !(ctx && (min_history_us) > 0 && \
            FlowState_DataSpanUs(ctx->flow_state) < (uint64_t)(min_history_us)); \
        if ((m & (1ULL << FEATURE_##id)) && _mh_ok) { \
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
