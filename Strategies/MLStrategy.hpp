// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the MIT License. See LICENSE file for details.

//======================================================================================================
// [ML STRATEGY]
//======================================================================================================
// model-driven buy signals using XGBoost or LightGBM inference.
// follows the same 4-function pattern as MR/Momentum/SimpleDip.
// model is loaded at startup, inference runs on slow path (~1-5μs per prediction).
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
#include "../CoreFrameworks/OrderGates.hpp"

//======================================================================================================
// [STATE]
//======================================================================================================
template <unsigned F> struct MLStrategyState {
    ModelHandle<F> buy_model;           // buy signal model (loaded from config path)
    float feature_buf[MODEL_MAX_FEATURES]; // scratch space for feature packing
    FPN<F> last_prediction;             // last model output (for display)
    BuySideGateConditions<F> buy_conds_initial; // anchor from warmup init
    int model_ready;                    // 1 if model loaded and features available
};

//======================================================================================================
// [INIT]
//======================================================================================================
// called once after warmup completes. model should already be loaded by the controller.
// sets initial buy conditions from rolling stats (same pattern as other strategies).
//======================================================================================================
template <unsigned F>
inline void MLStrategy_Init(MLStrategyState<F> *state, const RollingStats<F> *rolling,
                             BuySideGateConditions<F> *buy_conds) {
    state->last_prediction = FPN_Zero<F>();
    state->buy_conds_initial = *buy_conds;
    state->model_ready = Model_IsLoaded(&state->buy_model);
    memset(state->feature_buf, 0, sizeof(state->feature_buf));

    if (state->model_ready)
        fprintf(stderr, "[ML] strategy initialized — model ready, %d features\n",
                state->buy_model.num_features);
    else
        fprintf(stderr, "[ML] strategy initialized — no model loaded (predictions disabled)\n");
}

//======================================================================================================
// [ADAPT]
//======================================================================================================
// no-op for now — model is static (trained offline).
// future: online learning, feature drift detection, model hot-swap.
//======================================================================================================
template <unsigned F>
inline void MLStrategy_Adapt(MLStrategyState<F> *state, FPN<F> current_price,
                              FPN<F> portfolio_delta, uint16_t active_bitmap,
                              const BuySideGateConditions<F> *buy_conds,
                              const void *cfg) {
    // intentionally empty — model weights don't change at runtime
    (void)state; (void)current_price; (void)portfolio_delta;
    (void)active_bitmap; (void)buy_conds; (void)cfg;
}

//======================================================================================================
// [BUY SIGNAL]
//======================================================================================================
// packs features from rolling stats + regime signals, runs model inference.
// if prediction > threshold, returns buy conditions; otherwise returns zero-gate.
// gate_direction = 0 (buy below avg, like MR) — model decides WHEN, not WHERE.
//======================================================================================================
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

    // pack features from regime signals + rolling stats
    int n = ModelFeatures_Pack(state->feature_buf, signals, rolling, rolling_long);

    // run inference
    float prediction = Model_Predict(&state->buy_model, state->feature_buf, n);
    state->last_prediction = FPN_FromDouble<F>((double)prediction);

    // cast config to access threshold
    const ControllerConfig<F> *cfg = (const ControllerConfig<F>*)cfg_void;
    float threshold = (float)FPN_ToDouble(cfg->ml_buy_threshold);

    if (prediction > threshold) {
        // buy signal: use rolling avg as gate price (model says WHEN, gate says WHERE)
        // offset below avg by 1 stddev to catch dips (like SimpleDip)
        FPN<F> offset = rolling->price_stddev;
        conds.price = FPN_Sub(rolling->price_avg, offset);
        conds.volume = rolling->volume_avg; // minimum volume = average
        conds.gate_direction = 0; // buy below
    }

    return conds;
}

//======================================================================================================
// [EXIT ADJUST]
//======================================================================================================
// uses fixed TP/SL from config (ml_tp_pct / ml_sl_pct).
// no trailing — keep it simple until we have exit models.
// TP/SL are set at fill time by the controller, not here.
//======================================================================================================
template <unsigned F>
inline void MLStrategy_ExitAdjust(void *portfolio, FPN<F> current_price,
                                   const RollingStats<F> *rolling,
                                   MLStrategyState<F> *state, const void *cfg) {
    // no per-position trailing for ML strategy (fixed TP/SL from config)
    // future: run exit model per position, adjust TP/SL based on prediction
    (void)portfolio; (void)current_price; (void)rolling; (void)state; (void)cfg;
}

#endif // ML_STRATEGY_HPP
