// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [STRATEGY PARAMETERS — pure functions that build GateParameters packs]
//======================================================================================================
//
// Phase 06 of the per-core sharding port. Each strategy provides a pure
// `_BuildParameters` function that takes the current slow-path state
// (RollingStats, ControllerConfig, allocated balance) and produces a
// GateParameters<F> pack the execution core can read on its hot path.
//
// Architecture:
//
//   slow path (controller core, every ~3 sim seconds):
//     1. RollingStats_Push absorbs the latest tick into the rolling window
//     2. for each registered execution core, dispatch to the strategy's
//        _BuildParameters via Strategy_BuildParameters(strategy_id, ...)
//     3. write the resulting pack into CoreContext::pending_params, mark dirty
//     4. EventLoop_PushParameters atomically swaps the pack into the core's
//        ParameterSlot via the seqlock from phase 05
//
//   hot path (execution core, every tick):
//     1. ParameterSlot_Read snapshots the active pack into stack-local storage
//     2. BG_Evaluate / SG_Evaluate consume the pack
//     3. branchless decision, optional event push
//
// Pure invariant: each _BuildParameters function MUST be a pure function of
// its inputs (RollingStats, config, allocated_balance). No globals, no
// statics, no Portfolio reads. The controller is the source of truth for
// per-core state and passes the parameters explicitly. Pitfall P6.3 (hidden
// statics) is the failure mode this invariant protects against.
//
// Phase 06 minimum viable scope:
//   - SimpleDip_BuildParameters: full port. SimpleDip is the simplest strategy
//     and the current default in production, so it's the priority.
//   - MeanReversion / Momentum / EMACross: minimal stubs that produce safe
//     defaults. Full ports happen in followup work — they have complex
//     adaptive feedback loops that need separate attention.
//   - Strategy_BuildParameters dispatcher routes by strategy_id.
//
//======================================================================================================

#pragma once

#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/GateParameters.hpp"
#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/BarrierGate.hpp"
#include "../ML_Headers/ConfidenceScore.hpp"
#include "../ML_Headers/CoreModelZoo.hpp"
#include "../ML_Headers/ModelInference.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../Strategies/RegimeDetector.hpp"
#include "StrategyInterface.hpp"

#include <cstdint>

namespace tt {

//======================================================================================================
// [ML BUILD CONTEXT — Phase 6prep sharded c13/c15]
//======================================================================================================
// Bundle of ML-only extras that the dispatcher passes through to ML_BuildParameters
// as a single void*. Keeps the dispatcher signature stable when more ML inputs
// are added (cost model, vol scaler) — just add a field here.
//
// Non-ML strategies don't see this; the dispatcher's void* ml_ctx is opaque to
// them.
//
// Field semantics:
//   model_handle    — &CoreModelZoo<F>, populated by EngineSharded per core
//   confidence      — &CoreContext::confidence, fed (pred, return) at exit fill
//   out_prediction  — written by ML_BuildParameters with the prediction value
//                     used in the gate decision; drainer snapshots this into
//                     active_prediction at entry-submit time
//   out_confidence  — written with the conf used (so snapshot reads match the
//                     value that drove the gate decision; no recomputation)
//======================================================================================================
struct MLBuildContext {
    void*               model_handle;
    ConfidenceScorer*   confidence;
    double*             out_prediction;
    double*             out_confidence;
    // v4.0 train-serve parity: pass through the RORRegressor + EMA price
    // that the engine's slow path maintains. ML_BuildParameters uses these
    // with Regime_ComputeSignals so ALL features ModelFeatures_Pack reads
    // (ror_slope, ema_sma_spread, ema_above_sma) match what the legacy
    // backtest path produces during training.
    void*               ror_regressor;   // const RORRegressor<F>*
    void*               ema_price;       // const FPN<F>*
};

//======================================================================================================
// [SIMPLEDIP — full port]
//======================================================================================================
//
// SimpleDip buys when price drops `entry_offset_pct` below the recent high
// in the rolling window. TP and SL are fixed percentage offsets from the
// expected entry price (config: take_profit_pct, stop_loss_pct).
//
// inputs read from rolling stats:
//   - price_max:  recent high used as the dip reference
//   - volume_avg: rolling mean volume, used for the volume gate threshold
//
// inputs read from config:
//   - entry_offset_pct: dip depth (e.g. 0.0015 = 0.15% below high)
//   - volume_multiplier: volume gate (tick volume must be > avg * multiplier)
//   - take_profit_pct: TP offset (e.g. 0.03 = 3%)
//   - stop_loss_pct: SL offset (e.g. 0.015 = 1.5%)
//
// allocated_balance is the per-core capital share. trade_size is computed as
// allocated_balance / expected_entry_price (i.e. spend the full allocation
// on a single position). this matches the legacy single-slot mode.
//
// Note on absolute vs relative TP/SL:
//   The execution core's SG_Evaluate uses absolute TP and SL prices. We don't
//   know the actual entry price ahead of time — we only know the threshold
//   that BG will fire on. So we compute TP/SL relative to the threshold price
//   (the expected entry). If the actual entry is slightly different, TP/SL
//   are off by the same amount. The controller's slow-path rebuild (every
//   ~3 sim seconds) refreshes them as the rolling stats move, so the drift
//   is bounded.
//======================================================================================================
template <unsigned F, unsigned W = 128, unsigned WL = 512>
inline void SimpleDip_BuildParameters(
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out,
    const RollingStats<F, WL>* rolling_long = nullptr
) {
    // Match production SimpleDip_BuySignal: use MAX(short_max, long_max) when
    // a long-window stats is provided, otherwise fall back to the short max.
    // Without this the sharded path produces a tighter entry threshold than
    // the legacy path and misses trades that legacy takes.
    FPN<F> recent_high = rolling->price_max;
    if (rolling_long && FPN_GreaterThan(rolling_long->price_max, recent_high)) {
        recent_high = rolling_long->price_max;
    }

    // expected_entry = recent_high * (1 - entry_offset_pct)
    FPN<F> dip_offset = FPN_Mul(recent_high, config->entry_offset_pct);
    FPN<F> expected_entry = FPN_Sub(recent_high, dip_offset);

    // Per-strategy TP/SL: use simpledip-specific override if set, else shared
    FPN<F> tp_pct = !FPN_IsZero(config->simpledip_tp_pct) ? config->simpledip_tp_pct : config->take_profit_pct;
    FPN<F> sl_pct = !FPN_IsZero(config->simpledip_sl_pct) ? config->simpledip_sl_pct : config->stop_loss_pct;

    // tp = expected_entry * (1 + tp_pct)
    FPN<F> tp_amount = FPN_Mul(expected_entry, tp_pct);
    FPN<F> take_profit_price = FPN_Add(expected_entry, tp_amount);

    // sl = expected_entry * (1 - sl_pct)
    FPN<F> sl_amount = FPN_Mul(expected_entry, sl_pct);
    FPN<F> stop_loss_price = FPN_Sub(expected_entry, sl_amount);

    // volume gate: tick volume must exceed rolling avg * multiplier
    FPN<F> volume_threshold = FPN_Mul(rolling->volume_avg, config->volume_multiplier);

    // sizing: allocated_balance / expected_entry_price (notional ÷ price = qty)
    // guard against zero entry to avoid divide-by-zero (rolling stats uninit)
    FPN<F> trade_size = FPN_Zero<F>();
    if (!FPN_IsZero(expected_entry)) {
        trade_size = FPN_DivNoAssert(allocated_balance, expected_entry);
    }

    out->bg_price_threshold   = expected_entry;
    out->bg_volume_threshold  = volume_threshold;
    // Legacy absolute prices kept for back-compat with tests that don't
    // exercise the per-fill path.
    out->sg_take_profit_price = take_profit_price;
    out->sg_stop_loss_price   = stop_loss_price;
    // Phase 14 per-fill: the execution core will compute live TP/SL from
    // the actual fill price using these percentages, overriding the legacy
    // absolute prices above. Removes the structural loss bias.
    out->tp_pct               = tp_pct;
    out->sl_pct               = sl_pct;
    out->trade_size           = trade_size;
    out->strategy_id          = STRATEGY_SIMPLE_DIP;
    out->flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    for (int i = 0; i < 6; ++i) out->_pad[i] = 0;
}

//======================================================================================================
// [MEAN REVERSION — STUB, full port deferred]
//======================================================================================================
// Real MeanReversion uses a regression feedback loop to adapt entry_offset_pct
// and volume_multiplier based on recent P&L. The state lives in
// `MeanReversionState<F>` and is mutated by `MeanReversion_Adapt`. Porting it
// to a pure function requires moving the regression state out of the strategy
// and into the controller (or accepting that the controller calls _Adapt
// before each _BuildParameters cycle).
//
// For phase 06, this stub produces a safe deterministic pack: same entry
// price as SimpleDip but with the MR strategy_id so dispatcher routing works
// for tests. Full port is followup work.
//======================================================================================================
template <unsigned F, unsigned W = 128>
inline void MeanReversion_BuildParameters(
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out
) {
    // TODO(phase06-followup): full port of the regression feedback loop.
    // For now, use the rolling mean ± stddev as the base entry/exit prices.
    FPN<F> entry_price = rolling->price_avg;
    if (FPN_IsZero(entry_price)) entry_price = rolling->price_max;

    FPN<F> tp_pct = !FPN_IsZero(config->mr_tp_pct) ? config->mr_tp_pct : config->take_profit_pct;
    FPN<F> sl_pct = !FPN_IsZero(config->mr_sl_pct) ? config->mr_sl_pct : config->stop_loss_pct;
    FPN<F> tp_amount = FPN_Mul(entry_price, tp_pct);
    FPN<F> sl_amount = FPN_Mul(entry_price, sl_pct);
    FPN<F> volume_threshold = FPN_Mul(rolling->volume_avg, config->volume_multiplier);

    FPN<F> trade_size = FPN_Zero<F>();
    if (!FPN_IsZero(entry_price)) {
        trade_size = FPN_DivNoAssert(allocated_balance, entry_price);
    }

    out->bg_price_threshold   = entry_price;
    out->bg_volume_threshold  = volume_threshold;
    out->sg_take_profit_price = FPN_Add(entry_price, tp_amount);
    out->sg_stop_loss_price   = FPN_Sub(entry_price, sl_amount);
    out->tp_pct               = tp_pct;
    out->sl_pct               = sl_pct;
    out->trade_size           = trade_size;
    out->strategy_id          = STRATEGY_MEAN_REVERSION;
    out->flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    for (int i = 0; i < 6; ++i) out->_pad[i] = 0;
}

//======================================================================================================
// [MOMENTUM — STUB, full port deferred]
//======================================================================================================
// Real Momentum uses ROR (rate-of-rate, slope-of-slope) and R² gates from
// the rolling regression. It buys ABOVE the price (gate_direction = 1) when
// momentum is rising. Like MeanReversion, the adaptive feedback loop needs
// to be relocated to controller-side state before this becomes a pure
// function.
//======================================================================================================
template <unsigned F, unsigned W = 128>
inline void Momentum_BuildParameters(
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out
) {
    // TODO(phase06-followup): full port with ROR and R² gates.
    // For phase 06 stub: buy above the rolling avg.
    // Momentum uses stddev multipliers (momentum_tp_mult / momentum_sl_mult)
    // rather than percentage TP/SL. Falls back to shared percentage if not set.
    FPN<F> entry_price = rolling->price_avg;
    if (FPN_IsZero(entry_price)) entry_price = rolling->price_max;

    FPN<F> tp_amount, sl_amount;
    if (!FPN_IsZero(config->momentum_tp_mult) && !FPN_IsZero(rolling->price_stddev)) {
        tp_amount = FPN_Mul(rolling->price_stddev, config->momentum_tp_mult);
        sl_amount = FPN_Mul(rolling->price_stddev, config->momentum_sl_mult);
    } else {
        tp_amount = FPN_Mul(entry_price, config->take_profit_pct);
        sl_amount = FPN_Mul(entry_price, config->stop_loss_pct);
    }
    FPN<F> volume_threshold = FPN_Mul(rolling->volume_avg, config->volume_multiplier);

    FPN<F> trade_size = FPN_Zero<F>();
    if (!FPN_IsZero(entry_price)) {
        trade_size = FPN_DivNoAssert(allocated_balance, entry_price);
    }

    out->bg_price_threshold   = entry_price;
    out->bg_volume_threshold  = volume_threshold;
    out->sg_take_profit_price = FPN_Add(entry_price, tp_amount);
    out->sg_stop_loss_price   = FPN_Sub(entry_price, sl_amount);
    out->tp_pct               = config->take_profit_pct;  // fallback for per-fill
    out->sl_pct               = config->stop_loss_pct;
    out->trade_size           = trade_size;
    out->strategy_id          = STRATEGY_MOMENTUM;
    out->flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    for (int i = 0; i < 6; ++i) out->_pad[i] = 0;
}

//======================================================================================================
// [EMA CROSS — STUB, full port deferred]
//======================================================================================================
// Real EMA Cross uses an EMA computed by the controller on the slow path,
// then buys dips when fast EMA is above slow EMA. The EMA state lives in
// the controller, not the strategy. Once the controller exposes a clean
// "current EMA" output, the port becomes straightforward.
//======================================================================================================
template <unsigned F, unsigned W = 128>
inline void EmaCross_BuildParameters(
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out
) {
    // TODO(phase06-followup): full port with EMA crossover trigger.
    // For phase 06 stub: same entry logic as SimpleDip but uses EMA Cross
    // specific TP/SL overrides if set.
    SimpleDip_BuildParameters(rolling, config, allocated_balance, out);
    out->strategy_id = STRATEGY_EMA_CROSS;
    // Override TP/SL with EMA Cross specific values if set
    if (!FPN_IsZero(config->emacross_tp_pct)) {
        out->tp_pct = config->emacross_tp_pct;
        FPN<F> entry = out->bg_price_threshold;
        out->sg_take_profit_price = FPN_Add(entry, FPN_Mul(entry, config->emacross_tp_pct));
    }
    if (!FPN_IsZero(config->emacross_sl_pct)) {
        out->sl_pct = config->emacross_sl_pct;
        FPN<F> entry = out->bg_price_threshold;
        out->sg_stop_loss_price = FPN_Sub(entry, FPN_Mul(entry, config->emacross_sl_pct));
    }
}

//======================================================================================================
// [ML — model-driven buy signals via CoreModelZoo]
//======================================================================================================
// Packs features from rolling stats, runs inference on whichever role models
// are loaded in the per-core CoreModelZoo, computes BarrierGate modulation,
// and produces gate parameters.
//
// Model resolution priority (first match wins):
//   1. CORE_MODEL_BARRIER (3-class softmax: stable/peak/valley) — primary path
//   2. CORE_MODEL_BUY_SIGNAL (single-binary, complementary interpretation) — legacy
//   3. no models → fall back to SimpleDip behavior
//
// BarrierGate modulation (when config->barrier_gate_enabled):
//   - hard block when bg.blocked (p_peak > BARRIER_HARD_BLOCK)
//   - hard block when prediction < ml_buy_threshold (signal too cold)
//   - else soft modulation: scale trade_size by bg.gate ∈ [g_min, 1.0]
//
// ml_ctx_ptr is &MLBuildContext (Phase 6prep sharded c13/c15). Legacy callers
// (LegacyReferenceDriver, experiment tests) pass nullptr — no model, no
// confidence damping, fall back to SimpleDip. Sharded production path always
// passes a real MLBuildContext.
//======================================================================================================
template <unsigned F, unsigned W = 128, unsigned WL = 512>
inline void ML_BuildParameters(
    const RollingStats<F, W>* rolling,
    const RollingStats<F, WL>* rolling_long,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out,
    void* ml_ctx_ptr
) {
    MLBuildContext* mctx = (MLBuildContext*)ml_ctx_ptr;
    CoreModelZoo<F>* zoo = nullptr;
    ConfidenceScorer* conf_scorer = nullptr;
    double* out_prediction = nullptr;
    double* out_confidence = nullptr;
    const RORRegressor<F>* ror_in = nullptr;
    const FPN<F>* ema_in = nullptr;
    if (mctx) {
        zoo = (CoreModelZoo<F>*)mctx->model_handle;
        conf_scorer = mctx->confidence;
        out_prediction = mctx->out_prediction;
        out_confidence = mctx->out_confidence;
        ror_in = (const RORRegressor<F>*)mctx->ror_regressor;
        ema_in = (const FPN<F>*)mctx->ema_price;
    }

    // if no zoo or no models loaded, fall back to SimpleDip
    if (!zoo || !CoreModelZoo_HasAny(zoo)) {
        SimpleDip_BuildParameters(rolling, config, allocated_balance, out, rolling_long);
        out->strategy_id = STRATEGY_ML;
        return;
    }

    // compute entry price (same as SimpleDip for sizing)
    FPN<F> recent_high = rolling->price_max;
    if (rolling_long && FPN_GreaterThan(rolling_long->price_max, recent_high)) {
        recent_high = rolling_long->price_max;
    }
    FPN<F> entry_price = rolling->price_avg;
    if (FPN_IsZero(entry_price)) entry_price = recent_high;

    // v4.0 train-serve parity: when the engine provides a RORRegressor + EMA
    // pointer (sharded production path), call Regime_ComputeSignals — same
    // path the legacy backtest uses. Otherwise (legacy/test callers without
    // mctx, or mctx without these pointers), fall back to the inline minimal
    // population (preserves prior behavior for those callers; ror_slope +
    // ema_sma_spread + ema_above_sma stay zero, matching pre-v4.0).
    RegimeSignals<F> sig;
    memset(&sig, 0, sizeof(sig));
    if (ror_in && ema_in && rolling_long) {
        Regime_ComputeSignals(&sig, rolling, rolling_long, ror_in, *ema_in);
    } else {
        sig.short_slope    = FPN_IsZero(rolling->price_avg) ? FPN_Zero<F>()
                             : FPN_DivNoAssert(rolling->price_slope, rolling->price_avg);
        sig.short_r2       = rolling->price_r_squared;
        sig.short_variance = rolling->price_variance;
        sig.volume_slope   = rolling->volume_slope;
        sig.volume_delta   = rolling->volume_delta;
        if (rolling_long && rolling_long->count > 0) {
            sig.long_slope    = FPN_IsZero(rolling_long->price_avg) ? FPN_Zero<F>()
                                : FPN_DivNoAssert(rolling_long->price_slope, rolling_long->price_avg);
            sig.long_r2       = rolling_long->price_r_squared;
            sig.long_variance = rolling_long->price_variance;
            if (!FPN_IsZero(rolling_long->price_variance))
                sig.vol_ratio = FPN_DivNoAssert(rolling->price_variance, rolling_long->price_variance);
            else
                sig.vol_ratio = FPN_FromDouble<F>(1.0);
        }
    }

    // pack features once — used by whichever model role is loaded
    float features[MODEL_MAX_FEATURES];
    int n = ModelFeatures_Pack(features, &sig, rolling, rolling_long);

    // run inference, prefer 3-class barrier model when available
    double prediction = 0.5;   // neutral fallback
    double p_peak     = 0.5;
    double p_valley   = 0.5;
    int have_signal   = 0;

    if (zoo->loaded_mask & CORE_MODEL_BARRIER) {
        // 3-class softmax: [0]=stable, [1]=peak, [2]=valley
        float multi[3] = {0.0f, 0.0f, 0.0f};
        int got = Model_PredictMulti(&zoo->barrier, features, n, multi, 3);
        if (got >= 3) {
            p_peak     = multi[1];
            p_valley   = multi[2];
            // entry signal = "valley imminent" — primary trade trigger
            prediction = p_valley;
            have_signal = 1;
        } else if (got == 1) {
            // model was actually binary (mis-labeled as barrier role) — still usable
            prediction = multi[0];
            p_peak     = 1.0 - prediction;
            p_valley   = prediction;
            have_signal = 1;
        }
    } else if (zoo->loaded_mask & CORE_MODEL_BUY_SIGNAL) {
        // legacy single-binary: complementary interpretation
        prediction = Model_Predict(&zoo->buy_signal, features, n);
        p_peak     = 1.0 - prediction;
        p_valley   = prediction;
        have_signal = 1;
    }

    // if inference failed, fall back to SimpleDip
    if (!have_signal) {
        SimpleDip_BuildParameters(rolling, config, allocated_balance, out, rolling_long);
        out->strategy_id = STRATEGY_ML;
        return;
    }

    // TP/SL from ML-specific config
    FPN<F> tp_pct = config->ml_tp_pct;
    FPN<F> sl_pct = config->ml_sl_pct;
    FPN<F> tp_amount = FPN_Mul(entry_price, tp_pct);
    FPN<F> sl_amount = FPN_Mul(entry_price, sl_pct);

    // volume gate
    FPN<F> volume_threshold = FPN_Mul(rolling->volume_avg, config->volume_multiplier);

    // sizing — base trade size before barrier modulation
    FPN<F> trade_size = FPN_Zero<F>();
    if (!FPN_IsZero(entry_price)) {
        trade_size = FPN_DivNoAssert(allocated_balance, entry_price);
    }

    // Phase 6prep sharded c15: confidence-damped threshold. When confidence_enabled,
    // the effective entry threshold is base * (scale - conf), clamped to <= 1.0.
    // A noise-floor model (conf ≈ CONFIDENCE_MIN_IC_DEFAULT) gives effective ≈
    // base * (2.0 - 0.01) ≈ 2*base — gate stays cold. Real signal pushes conf
    // toward 1.0 → effective approaches 0 → gate fires more readily. The
    // unconditional clamp to 1.0 prevents a perverse "always blocked" state if
    // base+scale combine to >1.0 with low conf. Mirrors the legacy formula at
    // PortfolioController.hpp:~1614.
    double base_threshold = FPN_ToDouble(config->ml_buy_threshold);
    double conf_now = 0.0;
    double threshold = base_threshold;
    if (config->confidence_enabled && conf_scorer) {
        conf_now = ConfidenceScorer_Compute(conf_scorer, 0.0);  // data_age=0 (live)
        double scale = FPN_ToDouble(config->confidence_threshold_scale);
        double effective = base_threshold * (scale - conf_now);
        if (effective > 1.0) effective = 1.0;
        threshold = effective;
    }

    // gate decision: BarrierGate (continuous modulation) OR binary threshold
    FPN<F> gate_price = FPN_Zero<F>();  // default: zero-gate (no entry)

    if (config->barrier_gate_enabled) {
        BarrierGateResult bg = BarrierGate_Compute(p_peak, p_valley);
        // hard block if either: barrier says "imminent peak" OR prediction below threshold
        if (!bg.blocked && prediction >= threshold) {
            gate_price = entry_price;
            // soft modulation: scale position by gate strength [g_min, 1.0]
            trade_size = FPN_Mul(trade_size, FPN_FromDouble<F>(bg.gate));
        }
    } else {
        // legacy binary threshold path (backward compat when barrier_gate_enabled=0)
        if (prediction >= threshold) {
            gate_price = entry_price;
        }
    }

    // Phase 6prep sharded c13/c15: outparams for snapshot + drainer.
    // out_prediction is snapshotted to ctx->active_prediction at entry submit.
    // out_confidence is read by ShardedSnapshot for ML observability.
    if (out_prediction)  *out_prediction  = prediction;
    if (out_confidence)  *out_confidence  = conf_now;

    out->bg_price_threshold   = gate_price;
    out->bg_volume_threshold  = volume_threshold;
    out->sg_take_profit_price = FPN_Add(entry_price, tp_amount);
    out->sg_stop_loss_price   = FPN_Sub(entry_price, sl_amount);
    out->tp_pct               = tp_pct;
    out->sl_pct               = sl_pct;
    out->trade_size           = trade_size;
    out->strategy_id          = STRATEGY_ML;
    out->flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    for (int i = 0; i < 6; ++i) out->_pad[i] = 0;
}

//======================================================================================================
// [STRATEGY DISPATCHER]
//======================================================================================================
// the slow path calls this once per registered execution core. dispatches to
// the strategy's _BuildParameters by strategy_id. STRATEGY_NONE produces a
// zero-init pack so the core has safe defaults but won't trade (combined
// with permission=0 enforced by the run loop, this is bulletproof).
//
// the dispatcher is the integration point between the per-core sharded
// architecture and the strategy library. adding a new strategy:
//   1. define STRATEGY_FOO constant in StrategyInterface.hpp (or wherever)
//   2. write Foo_BuildParameters following the pattern above
//   3. add a case to this switch
//   4. done — Strategy_BuildParameters dispatches to it automatically
//======================================================================================================
template <unsigned F, unsigned W = 128, unsigned WL = 512>
inline void Strategy_BuildParameters(
    uint8_t strategy_id,
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out,
    const RollingStats<F, WL>* rolling_long = nullptr,
    void* model_ctx = nullptr
) {
    switch (strategy_id) {
        case STRATEGY_SIMPLE_DIP:
            SimpleDip_BuildParameters(rolling, config, allocated_balance, out, rolling_long);
            return;
        case STRATEGY_MEAN_REVERSION:
            MeanReversion_BuildParameters(rolling, config, allocated_balance, out);
            return;
        case STRATEGY_MOMENTUM:
            Momentum_BuildParameters(rolling, config, allocated_balance, out);
            return;
        case STRATEGY_EMA_CROSS:
            EmaCross_BuildParameters(rolling, config, allocated_balance, out);
            return;
        case STRATEGY_ML:
            ML_BuildParameters(rolling, rolling_long, config, allocated_balance, out, model_ctx);
            return;
        default:
            GateParameters_Init(out);
            return;
    }
}

}  // namespace tt
