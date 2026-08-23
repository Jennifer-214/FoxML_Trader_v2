// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the MIT License. See LICENSE file for details.

//======================================================================================================
// [FILE]_[Strategies/MLStrategy.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[model-driven entries — feature pack + scaler + XGBoost inference (single-zoo or ensemble dispatch); NaN/Inf triple-guard; no model loaded = zero-cost no-op]
// [CONTAINS]
//   - [STRUCT]_[MLStrategyState]
//   - [FUNCTION]_[MLStrategy_BuySignal]   (Init / Adapt + canonical adapter unblocked)
//   - [FUNCTION]_[MLStrategy_ExitAdjust]  (+ ExitAdjustSharded ratchet twin)
//======================================================================================================
// model-driven buy signals using XGBoost or LightGBM inference.
// follows the same 4-function pattern as MR/Momentum/SimpleDip.
// model is loaded at startup, inference runs on slow path. COST LAW (measured
// 2026-08-22, 3-class): ~550ns per boosting round + ~10µs fixed, PER predict
// call, PER ensemble arm — 350 estimators × 3 classes = 1050 trees ≈ 217µs/call;
// size models to the H8 slow-path budget at TRAINING time (~100-150 rounds
// serves at ~67µs/call). The old "~1-5µs" figure was a small-binary-model era
// claim, stale by two orders of magnitude at current tree counts.
// when no model is loaded, all functions are no-ops (zero overhead).
//
// the model predicts a buy probability [0, 1]. if prediction > buy_threshold,
// a buy signal is emitted with gate price derived from rolling stats.
// TP/SL use the same volatility-based logic as other strategies.
//======================================================================================================
#ifndef ML_STRATEGY_HPP
#define ML_STRATEGY_HPP

#include "StrategyInterface.hpp"
#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../ML_Headers/ModelInference.hpp"
#include "../ML_Headers/NodeModelZoo.hpp"  // v5.10.0a.G.4: EnsembleModelZoo for multi-horizon
#include "../ML_Headers/FeatureRegistry.hpp"  // v5.8.1b: Features_PackAll replaces ModelFeatures_Pack
#include "../CoreFrameworks/OrderGates.hpp"
#include "../MemHeaders/OmsStateFlagRegistry.hpp"  // v5.15.5.C.2 (S3a) — MASK_OMS_STATE_*
#include <cmath>  // v5.9.0: std::isnan/isinf for prediction validation

//======================================================================
// [STRUCT]_[MLStrategyState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[buy model handle + feature scratch + last prediction; optional ensemble_zoo pointer (nullptr = single-model path)]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F> struct MLStrategyState {
    ModelHandle<F> buy_model;           // buy signal model (loaded from config path)
    float feature_buf[MODEL_MAX_FEATURES]; // scratch space for feature packing
    FPN_Binary<F> last_prediction;             // last model output (for display)
    BuySideGateConditions<F> buy_conds_initial; // anchor from warmup init
    int model_ready;                    // 1 if model loaded and features available
    // v5.10.0a.G.4 — optional ensemble dispatch. nullptr default = single-
    // model path (existing behavior). When engine sets this pointer +
    // ensemble->active=1, BuySignal dispatches via Model_Predict_Ensemble
    // over ensemble->buy_signal[0..buy_signal_count-1]. Plumbing is
    // engine-side: PortfolioController owns the EnsembleModelZoo and
    // assigns this pointer at MLStrategy_Init time.
    EnsembleModelZoo<F>* ensemble_zoo;
    int ensemble_last_selected_idx;     // for display: which horizon won
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-16]
// [SIZE]_[7040B]
// [ALIGN]_[64]
// [CACHE_LINES]_[110]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[MLStrategyState]
//======================================================================

//------------------------------------------------------------------------------
// [SECTION]_[INIT]
//------------------------------------------------------------------------------
// called once after warmup completes. model should already be loaded by the controller.
// sets initial buy conditions from rolling stats (same pattern as other strategies).
//------------------------------------------------------------------------------
template <unsigned F>
inline void MLStrategy_Init(MLStrategyState<F> *state, const RollingStats<F> *rolling,
                             BuySideGateConditions<F> *buy_conds) {
    state->last_prediction = FPN_Zero<F>();
    state->buy_conds_initial = *buy_conds;
    state->model_ready = Model_IsLoaded(&state->buy_model);
    memset(state->feature_buf, 0, sizeof(state->feature_buf));
    // v5.10.0a.G.4 — ensemble pointer defaults to nullptr. Engine wiring
    // (PortfolioController boot) assigns it from cfg.horizon_list-loaded
    // EnsembleModelZoo when ensemble inference is active. nullptr = single-
    // model path; bytewise-equivalent to pre-v5.10.0a.G.4 behavior.
    state->ensemble_zoo = nullptr;
    state->ensemble_last_selected_idx = -1;

    if (state->model_ready)
        fprintf(stderr, "[ML] strategy initialized — single-zoo model ready, %d features\n",
                state->buy_model.num_features);
    else
        // v5.11.62 — phrasing reflects that ensemble may be wired later.
        // EngineSharded boot assigns state->ensemble_zoo AFTER MLStrategy_Init
        // runs. Look for the "[sharded] core N: ensemble active" line below
        // to confirm ensemble path is active.
        fprintf(stderr, "[ML] strategy initialized — single-zoo not loaded "
                        "(ensemble may attach below)\n");
}

//------------------------------------------------------------------------------
// [SECTION]_[ADAPT]
//------------------------------------------------------------------------------
// no-op for now — model is static (trained offline).
// future: online learning, feature drift detection, model hot-swap.
//------------------------------------------------------------------------------
template <unsigned F>
inline void MLStrategy_Adapt(MLStrategyState<F> *state, FPN_Binary<F> current_price,
                              FPN_Binary<F> portfolio_delta, uint16_t active_bitmap,
                              const BuySideGateConditions<F> *buy_conds,
                              const void *cfg) {
    // intentionally empty — model weights don't change at runtime
    (void)state; (void)current_price; (void)portfolio_delta;
    (void)active_bitmap; (void)buy_conds; (void)cfg;
}

// v5.8.0 — canonical-signature adapter. The X-macro registry expects
// every strategy's _Adapt to take `const ControllerConfig<F>*` as the
// last arg (per DOCS/EASY_ADDITIONS_INVARIANTS.md). MLStrategy_Adapt
// historically took `const void*` (likely an include-cycle workaround).
// This wrapper conforms to the canonical sig and forwards as void*.
// Real-function preserved for legacy callers; X-macro references this
// adapter.
template <unsigned F> struct ControllerConfig;
template <unsigned F>
inline void MLStrategy_Adapt_Canonical(
    MLStrategyState<F> *state, FPN_Binary<F> current_price,
    FPN_Binary<F> portfolio_delta, uint16_t active_bitmap,
    const BuySideGateConditions<F> *buy_conds,
    const ControllerConfig<F> *cfg) {
    MLStrategy_Adapt(state, current_price, portfolio_delta, active_bitmap,
                      buy_conds, (const void*)cfg);
}

//======================================================================
// [FUNCTION]_[MLStrategy_BuySignal]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [ML_INFERENCE]]
// [REFERENCE]_[INVARIANT]_[H5]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[pack -> scale -> predict (ensemble dispatch when wired) -> threshold gate; NaN/Inf guarded at pack, post-scaler, and prediction; model decides WHEN, gate decides WHERE]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F> struct RegimeSignals; // forward declaration
template <unsigned F> struct ControllerConfig; // forward declaration

template <unsigned F>
inline BuySideGateConditions<F> MLStrategy_BuySignal(MLStrategyState<F> *state,
                                                      const RollingStats<F> *rolling,
                                                      const RollingStats<F, 512> *rolling_long,
                                                      const void *cfg_void,
                                                      const RegimeSignals<F> *signals) {
    BuySideGateConditions<F> conds;
    conds.price = FPN_Zero<F>();
    conds.volume = FPN_Zero<F>();
    conds.gate_direction = 0;

    if (!state->model_ready) return conds;

    // pack features from regime signals + rolling stats (v5.8.1b: registry-driven)
    FeatureComputeCtx<F> ctx{};
    ctx.signals       = signals;
    ctx.short_rolling = rolling;
    int n = Features_PackAll(&ctx, state->feature_buf);
    // v5.9.0 — NaN/Inf in feature pack → no entry (Features_PackAll returns
    // -1 sentinel; never pass garbage to XGBoost, never silently entry).
    if (n < 0) {
        fprintf(stderr, "[ML] BuySignal: NaN/Inf in feature pack — skipping prediction\n");
        return conds;  // already zero-initialized above; no buy signal
    }

    // v5.9.3b — apply feature standardizer (mean-centering + unit-variance).
    // Identity no-op when state->buy_model.scaler.has_scaler=0 (legacy v5.x
    // models or v5.9.3+ models without a sidecar). Post-apply finite check
    // catches any NaN introduced by the scaler math itself (rare-edge:
    // stddev floored + feature at saturation).
    if (tt::FeatureStandardizer_Apply(&state->buy_model.scaler,
                                       state->feature_buf, n) < 0) {
        fprintf(stderr, "[ML] BuySignal: NaN/Inf post-scaler-apply — skipping prediction\n");
        return conds;
    }

    // v5.10.0a.G.4 — ensemble dispatch when active. Operator opts in via
    // cfg.horizon_list non-empty; engine boot wires state->ensemble_zoo
    // to the loaded EnsembleModelZoo. If ensemble inactive (nullptr or
    // active=0), falls through to single-model Model_Predict — bytewise-
    // equivalent to pre-v5.10.0a.G.4 behavior.
    // v5.11.62 — read from ezoo->primary_handles (set at load time to
    // whichever role file was actually present: buy_signal > barrier >
    // regime). Per-handle buy_class_idx is also set then, so
    // Model_Predict returns the right class probability transparently.
    float prediction;
    int   selected_horizon_idx = -1;
    if (state->ensemble_zoo && BITMAP_IS_SET(state->ensemble_zoo->init_flags, MASK_EZOO_ACTIVE) &&
        state->ensemble_zoo->primary_count > 0 &&
        state->ensemble_zoo->primary_handles) {
        prediction = Model_Predict_Ensemble(state->ensemble_zoo->primary_handles,
                                              state->ensemble_zoo->primary_count,
                                              state->feature_buf, n,
                                              &selected_horizon_idx);
    } else {
        prediction = Model_Predict(&state->buy_model, state->feature_buf, n);
    }
    state->ensemble_last_selected_idx = selected_horizon_idx;

    // v5.9.0 — NaN/Inf in prediction → no entry. XGBoost can return NaN on
    // pathological inputs; `prediction > threshold` evaluates false on NaN
    // (silent miss). Guard explicitly + log.
    if (std::isnan(prediction) || std::isinf(prediction)) {
        fprintf(stderr, "[ML] BuySignal: prediction NaN/Inf — skipping entry\n");
        return conds;
    }
    state->last_prediction = FPN_FromDouble<F>((double)prediction);

    // cast config to access threshold
    const ControllerConfig<F> *cfg = (const ControllerConfig<F>*)cfg_void;
    float threshold = (float)FPN_ToDouble(cfg->ml_buy_threshold);

    if (prediction > threshold) {
        // buy signal: use rolling avg as gate price (model says WHEN, gate says WHERE)
        // offset below avg by 1 stddev to catch dips (like SimpleDip)
        FPN_Binary<F> offset = rolling->price_stddev;
        conds.price = FPN_Sub(rolling->price_avg, offset);
        conds.volume = rolling->volume_avg; // minimum volume = average
        conds.gate_direction = 0; // buy below
    }

    return conds;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// packs features from rolling stats + regime signals, runs model inference.
// if prediction > threshold, returns buy conditions; otherwise returns zero-gate.
// gate_direction = 0 (buy below avg, like MR) — model decides WHEN, not WHERE.
//======================================================================
// [END_FUNCTION]_[MLStrategy_BuySignal]
//======================================================================

//======================================================================
// [FUNCTION]_[MLStrategy_ExitAdjust]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[R2-gated trailing TP/SL (rolling R2 > 0.5) + 2:1 floor; ExitAdjustSharded (ratchet_sl twin, fee-floor capped) shares the section]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void MLStrategy_ExitAdjust(Portfolio<F> *portfolio, Money current_price,
                                   const RollingStats<F> *rolling,
                                   MLStrategyState<F> *state,
                                   const ControllerConfig<F> *cfg) {
    if (FPN_IsZero(cfg->tp_hold_score)) return;
    if (FPN_IsZero(rolling->price_stddev)) return;

    // R² from rolling stats (no separate feeder needed — rolling already has it)
    FPN_Binary<F> r_squared = rolling->price_r_squared;
    int r2_ok = FPN_GreaterThan(r_squared, FPN_FromDouble<F>(0.5));

    uint16_t active = portfolio->active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);
        Position<F> *pos = &portfolio->positions[idx];

        int above_tp = Money_Gt(current_price, pos->original_tp);

        if (above_tp & r2_ok) {
            FPN_Binary<F> tp_offset = FPN_Mul(rolling->price_stddev, cfg->tp_trail_mult);
            Money trailing_tp = Money_Sub(current_price, Money_FromBinary(tp_offset));
            pos->take_profit_price = Money_Max(pos->take_profit_price, trailing_tp);

            FPN_Binary<F> sl_offset = FPN_Mul(rolling->price_stddev, cfg->sl_trail_mult);
            Money trailing_sl = Money_Sub(current_price, Money_FromBinary(sl_offset));
            pos->stop_loss_price = Money_Max(pos->stop_loss_price, trailing_sl);

            // SL floor: 2:1 min reward/risk (only when SL below entry)
            if (Money_Lt(pos->stop_loss_price, pos->entry_price)) {
                Money tp_dist = Money_Sub(pos->take_profit_price, pos->entry_price);
                Money min_sl_dist = Money_Mul(tp_dist, Money{ 50000000 });  // exact 0.5
                Money sl_floor = Money_Sub(pos->entry_price, min_sl_dist);
                pos->stop_loss_price = Money_Min(pos->stop_loss_price, sl_floor);
            }
        }

        active &= active - 1;
    }
}

//------------------------------------------------------------------------------
// [SECTION]_[EXIT ADJUST — sharded, ratchet_sl path]
//------------------------------------------------------------------------------
// v5.4.0 Phase 2.5: sharded equivalent of MLStrategy_ExitAdjust above.
// Uses rolling->price_r_squared > 0.5 as the trail gate (same as legacy
// MLStrategy_ExitAdjust). Routes through Strategy_WriteRatchetSL so the
// v5.1.7 fee-floor cap applies. Per-core scope.
//
// State is unused here — MLStrategyState's only meaningful field
// (model_handle / last_prediction) lives on the inference path
// (ML_BuildParameters via MLBuildContext). The R² test reads rolling
// stats directly. The state pointer is taken for dispatcher symmetry
// with the other ExitAdjustSharded variants and so future ML state
// (online learning hooks, prediction-quality decay) has a place to land.
//
// TP trailing deferred to Phase 3.
//======================================================================================================
namespace tt {
template <unsigned F> struct EventLoopState;
template <unsigned F>
bool Strategy_WriteRatchetSL(EventLoopState<F>* state, int slot,
                              FPN_Binary<F> proposed_sl, FPN_Binary<F> entry_price,
                              const ControllerConfig<F>* cfg);

template <unsigned F, unsigned W>
inline void MLStrategy_ExitAdjustSharded(
    EventLoopState<F>* state,
    int slot,
    MLStrategyState<F>* /*ml*/,            // reserved — see comment above
    Money current_price,
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* cfg
) {
    if (FPN_IsZero(rolling->price_stddev)) return;
    // R² gate: only trail in confirmed-trend conditions. Mirrors the
    // legacy MLStrategy_ExitAdjust's `r2_ok = R² > 0.5` threshold.
    if (!FPN_GreaterThan(rolling->price_r_squared, FPN_FromDouble<F>(0.5))) return;

    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    int partial_on = BITMAP_IS_SET(state->oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
    uint16_t my_mask = BITMAP_NODE_SLOT_MASK(slot, partial_on);
    uint16_t bm = (uint16_t)(state->oms->portfolio.active_bitmap & my_mask);

    FPN_Binary<F> sl_offset   = FPN_Mul(rolling->price_stddev, cfg->sl_trail_mult);
    Money trailing_sl = Money_Sub(current_price, Money_FromBinary(sl_offset));

    while (bm) {
        int pidx = __builtin_ctz(bm);
        bm &= (uint16_t)(bm - 1);
        Money entry = state->oms->portfolio.positions[pidx].entry_price;
        if (Money_IsZero(entry)) continue;
        Money orig_tp = state->oms->portfolio.positions[pidx].original_tp;
        if (!Money_IsZero(orig_tp) && !Money_Gt(current_price, orig_tp)) continue;
        Strategy_WriteRatchetSL(state, slot, trailing_sl, entry, cfg);
    }
}
} // namespace tt
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// uses fixed TP/SL from config (ml_tp_pct / ml_sl_pct).
// no trailing — keep it simple until we have exit models.
// TP/SL are set at fill time by the controller, not here.
//
// R²-scaled trailing TP/SL (same pattern as Momentum_ExitAdjust).
// ratchets TP/SL upward when price runs past original TP in a strong trend.
// uses tp_trail_mult / sl_trail_mult from config with R² gating.
//======================================================================
// [END_FUNCTION]_[MLStrategy_ExitAdjust]
//======================================================================

#endif // ML_STRATEGY_HPP
