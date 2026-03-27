// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [REGIME DETECTOR]
//======================================================================================================
// classifies market state and switches the active strategy using score-based detection
//
// regimes:
//   REGIME_RANGING    — low directional slope, no consistent trend → mean reversion
//   REGIME_TRENDING   — sustained directional move confirmed across timeframes → momentum
//   REGIME_VOLATILE   — volatility spike with no direction → pause buying
//
// detection uses signals from RollingStats (128-tick + 512-tick windows) and ROR regressor:
//   - price_slope + price_r_squared from both windows (multi-timeframe confirmation)
//   - variance ratio between windows (relative volatility, self-adapting)
//   - ROR slope-of-slopes (trend acceleration — catches new trends early)
//   - volume_slope (volume confirmation)
//
// each signal contributes to a trending_score or volatile_score. highest score wins.
// higher confidence (more signals agree) can reduce hysteresis for faster switching.
//
// extensibility: add a field to RegimeSignals + one comparison in Regime_Classify.
// future hooks: FoxML model output, order flow, microstructure signals.
//
// position handling on regime switch:
//   MR → momentum: widen TP (let trend run), tighten SL (cut if wrong)
//   momentum → MR: tighten TP (take profit), widen SL (allow chop)
//   volatile: no adjustment (panic-adjusting causes more harm)
//   adjustment is one-shot at transition. new positions get native TP/SL.
//======================================================================================================
#ifndef REGIME_DETECTOR_HPP
#define REGIME_DETECTOR_HPP

#include "StrategyInterface.hpp"
#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/Portfolio.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../ML_Headers/ROR_regressor.hpp"
#include <time.h>

#define REGIME_RANGING       0
#define REGIME_TRENDING      1  // uptrend — momentum (buy breakouts above)
#define REGIME_VOLATILE      2
#define REGIME_TRENDING_DOWN 3  // downtrend — pause buying (future: short strategy)

//======================================================================================================
// [REGIME SIGNALS]
//======================================================================================================
// collected once per slow-path cycle from rolling stats, rolling_long, and ROR
// this is the extensibility point: adding a new signal = adding a field here
// + one comparison in Regime_Classify
//======================================================================================================
template <unsigned F> struct RegimeSignals {
    // short window (128-tick)
    FPN<F> short_slope;       // relative price slope (slope / avg)
    FPN<F> short_r2;          // price regression R² (trend consistency)
    FPN<F> short_variance;    // price variance
    // long window (512-tick)
    FPN<F> long_slope;        // relative price slope
    FPN<F> long_r2;           // price regression R²
    FPN<F> long_variance;     // price variance
    // derived signals
    FPN<F> vol_ratio;         // short_variance / long_variance (volatility spike)
    FPN<F> ror_slope;         // slope-of-slopes (trend acceleration)
    FPN<F> volume_slope;      // volume trend (confirmation)
    // data sufficiency flags
    int short_count;
    int long_count;
    int ror_ready;            // 1 if ROR has enough data for meaningful output
    // future extensibility:
    // FPN<F> model_score;    // ← FoxML model output
    // FPN<F> order_flow;     // ← microstructure signal
};

//======================================================================================================
// [COMPUTE SIGNALS]
//======================================================================================================
// fills RegimeSignals from current rolling stats, rolling_long, and ROR
// called once per slow-path cycle before Regime_Classify
//======================================================================================================
template <unsigned F>
inline void Regime_ComputeSignals(RegimeSignals<F> *sig,
                                   const RollingStats<F> *rolling,
                                   const RollingStats<F, 512> *rolling_long,
                                   const RORRegressor<F> *ror) {
    // short window signals
    sig->short_count    = rolling->count;
    sig->short_r2       = rolling->price_r_squared;
    sig->short_variance = rolling->price_variance;
    sig->volume_slope   = rolling->volume_slope;

    // relative slope: normalize by price so threshold is asset-independent
    if (!FPN_IsZero(rolling->price_avg))
        sig->short_slope = FPN_DivNoAssert(rolling->price_slope, rolling->price_avg);
    else
        sig->short_slope = FPN_Zero<F>();

    // long window signals
    sig->long_count    = rolling_long->count;
    sig->long_r2       = rolling_long->price_r_squared;
    sig->long_variance = rolling_long->price_variance;

    if (!FPN_IsZero(rolling_long->price_avg))
        sig->long_slope = FPN_DivNoAssert(rolling_long->price_slope, rolling_long->price_avg);
    else
        sig->long_slope = FPN_Zero<F>();

    // variance ratio: current volatility relative to baseline
    // > 1.0 means volatility is elevated vs longer-term average
    // self-adapting: $50 stddev in calm market (baseline $20) = 6.25x, same $50 in volatile ($45) = 1.23x
    if (rolling_long->count >= 64 && !FPN_IsZero(rolling_long->price_variance))
        sig->vol_ratio = FPN_DivNoAssert(rolling->price_variance, rolling_long->price_variance);
    else
        sig->vol_ratio = FPN_FromDouble<F>(1.0); // default: no spike detected

    // ROR: slope-of-slopes (trend acceleration)
    // positive = trend getting steeper, negative = trend flattening/reversing
    sig->ror_ready = (ror->count >= MAX_WINDOW);
    if (sig->ror_ready) {
        // compute ROR regression on slope samples
        LinearRegression3XResult<F> ror_result = RORRegressor_Compute(
            const_cast<RORRegressor<F>*>(ror));
        sig->ror_slope = ror_result.model.slope;
    } else {
        sig->ror_slope = FPN_Zero<F>();
    }
}

//======================================================================================================
// [STATE]
//======================================================================================================
template <unsigned F> struct RegimeState {
    int current_regime;          // REGIME_RANGING, REGIME_TRENDING, REGIME_VOLATILE
    int proposed_regime;         // what the classifier thinks (before hysteresis)
    int hysteresis_count;        // how many consecutive cycles the proposed regime has held
    int hysteresis_threshold;    // must hold for N cycles before switching (e.g. 5)
    int last_strategy_id;        // tracks which strategy was active before transition
    uint64_t regime_start_tick;  // tick at which current regime started
    time_t regime_start_time;    // wall clock time at regime start (for duration display)
};

//======================================================================================================
// [INIT]
//======================================================================================================
template <unsigned F>
inline void Regime_Init(RegimeState<F> *state, int hysteresis_threshold) {
    state->current_regime = REGIME_RANGING;  // start conservative
    state->proposed_regime = REGIME_RANGING;
    state->hysteresis_count = 0;
    state->hysteresis_threshold = hysteresis_threshold;
    state->last_strategy_id = STRATEGY_MEAN_REVERSION;
    state->regime_start_tick = 0;
    state->regime_start_time = time(NULL);
}

//======================================================================================================
// [CLASSIFY — SCORE-BASED]
//======================================================================================================
// each signal contributes +1 to a regime score. highest score wins.
// higher confidence = more signals agreeing = faster hysteresis passage.
//
// trending signals:
//   1. short window slope above threshold
//   2. long window slope above threshold (multi-timeframe confirmation)
//   3. short window R² above threshold (consistent direction)
//   4. ROR slope positive (trend accelerating — catches NEW trends earlier)
//   5. volume rising while price trending (volume confirmation)
//
// volatile signals:
//   1. variance ratio spiking vs baseline (relative, self-adapting)
//   2. low R² despite high variance (big moves, no direction)
//
// RANGING is the default when neither trending nor volatile has enough evidence.
//======================================================================================================
template <unsigned F>
inline int Regime_Classify(RegimeState<F> *state,
                            const RegimeSignals<F> *sig,
                            const ControllerConfig<F> *cfg) {
    // cold start: stay in RANGING until short window has enough data
    if (sig->short_count < 64)
        return state->current_regime;

    FPN<F> abs_short_slope = FPN_Abs(sig->short_slope);
    FPN<F> abs_long_slope  = FPN_Abs(sig->long_slope);

    // slope direction: 0 = positive (up), 1 = negative (down)
    int short_up = !sig->short_slope.sign;
    int short_down = sig->short_slope.sign;
    int long_up = !sig->long_slope.sign;
    int long_down = sig->long_slope.sign;

    // --- trending score (direction-aware) ---
    // magnitude checks use abs, direction splits into up/down scores
    int trending_score = 0;
    int up_signals = 0;
    int down_signals = 0;

    // short window slope strong enough
    int short_slope_strong = FPN_GreaterThan(abs_short_slope, cfg->regime_slope_threshold);
    trending_score += short_slope_strong;
    up_signals += short_slope_strong & short_up;
    down_signals += short_slope_strong & short_down;

    // long window confirms (multi-timeframe agreement)
    int long_has_data = (sig->long_count >= 64);
    int long_slope_strong = long_has_data & FPN_GreaterThan(abs_long_slope, cfg->regime_slope_threshold);
    trending_score += long_slope_strong;
    up_signals += long_slope_strong & long_up;
    down_signals += long_slope_strong & long_down;

    // price movement is consistent (high R²)
    int consistent = FPN_GreaterThan(sig->short_r2, cfg->regime_r2_threshold);
    trending_score += consistent;

    // trend is accelerating (ROR positive = upward acceleration, negative = downward)
    int ror_positive = sig->ror_ready & FPN_GreaterThan(sig->ror_slope, FPN_Zero<F>());
    int ror_negative = sig->ror_ready & FPN_LessThan(sig->ror_slope, FPN_Zero<F>());
    trending_score += (ror_positive | ror_negative); // either direction counts for trending
    up_signals += ror_positive;
    down_signals += ror_negative;

    // volume rising in direction of trend (confirmation)
    int vol_confirms = FPN_GreaterThan(sig->volume_slope, FPN_Zero<F>()) & short_slope_strong;
    trending_score += vol_confirms;

    // --- volatile score ---
    int volatile_score = 0;

    // variance spike relative to longer-term baseline (self-adapting)
    int vol_spike = FPN_GreaterThan(sig->vol_ratio, cfg->regime_vol_spike_ratio);
    volatile_score += vol_spike;

    // high variance but no consistent direction (choppy)
    int inconsistent = !consistent;
    volatile_score += vol_spike & inconsistent;

    // --- classify ---
    // trending needs at least 2 signals AND at least one slope signal (short or long)
    // volatile needs at least 2 signals (spike + no direction)
    // direction: more down signals = TRENDING_DOWN, otherwise TRENDING (up)
    int has_slope = short_slope_strong | long_slope_strong;
    int detected;
    if (trending_score >= 2 && has_slope && consistent && trending_score > volatile_score) {
        detected = (down_signals > up_signals) ? REGIME_TRENDING_DOWN : REGIME_TRENDING;
    } else if (volatile_score >= 2 && volatile_score > trending_score)
        detected = REGIME_VOLATILE;
    else
        detected = REGIME_RANGING;

    // hysteresis: proposed regime must hold for N consecutive cycles before switching
    if (detected == state->proposed_regime) {
        state->hysteresis_count++;
    } else {
        state->proposed_regime = detected;
        state->hysteresis_count = 1;
    }

    if (state->hysteresis_count >= state->hysteresis_threshold
        && state->proposed_regime != state->current_regime) {
        state->current_regime = state->proposed_regime;
    }

    return state->current_regime;
}

//======================================================================================================
// [STRATEGY MAPPING]
//======================================================================================================
static inline int Regime_ToStrategy(int regime) {
    switch (regime) {
        case REGIME_TRENDING:      return STRATEGY_MOMENTUM;
        case REGIME_TRENDING_DOWN: return STRATEGY_MEAN_REVERSION; // pause buying, future: short
        case REGIME_VOLATILE:      return STRATEGY_MEAN_REVERSION;
        case REGIME_RANGING:
        default:                   return STRATEGY_MEAN_REVERSION;
    }
}

//======================================================================================================
// [ADJUST POSITIONS ON REGIME SWITCH]
//======================================================================================================
// called once when current_regime changes. walks active positions and adjusts TP/SL
// to match the new regime's risk profile.
//
// MR → momentum: widen TP (let trend run), tighten SL (cut if wrong)
// momentum → MR: tighten TP (take profit before reversal), widen SL (allow chop)
// volatile: no adjustment (panic-adjusting in volatility causes more harm)
//
// only adjusts positions entered under the PREVIOUS strategy.
//======================================================================================================
template <unsigned F>
inline void Regime_AdjustPositions(Portfolio<F> *portfolio,
                                     const RollingStats<F> *rolling,
                                     int old_regime, int new_regime,
                                     const uint8_t *entry_strategy,
                                     const ControllerConfig<F> *cfg) {
    int old_strategy = Regime_ToStrategy(old_regime);
    FPN<F> stddev = rolling->price_stddev;

    // guard: flat market (stddev=0) would produce zero offsets → TP=SL=entry → immediate exit
    if (FPN_IsZero(stddev)) return;

    FPN<F> hundred = FPN_FromDouble<F>(100.0);
    FPN<F> half = cfg->min_sl_tp_ratio;

    uint16_t active = portfolio->active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);

        if (entry_strategy[idx] == old_strategy) {
            Position<F> *pos = &portfolio->positions[idx];

            if (old_regime == REGIME_RANGING && new_regime == REGIME_TRENDING) {
                // momentum_tp/sl_mult are direct stddev multipliers — no ×100
                FPN<F> wide_tp_offset = FPN_Mul(stddev, cfg->momentum_tp_mult);
                FPN<F> wide_tp = FPN_AddSat(pos->entry_price, wide_tp_offset);
                pos->take_profit_price = FPN_Max(pos->take_profit_price, wide_tp);

                FPN<F> tight_sl_offset = FPN_Mul(stddev, cfg->momentum_sl_mult);
                FPN<F> tight_sl = FPN_SubSat(pos->entry_price, tight_sl_offset);
                pos->stop_loss_price = FPN_Max(pos->stop_loss_price, tight_sl);

                // SL floor: ensure SL distance >= 0.5 × TP distance (2:1 min reward/risk)
                FPN<F> tp_dist = FPN_Sub(pos->take_profit_price, pos->entry_price);
                FPN<F> min_sl_dist = FPN_Mul(tp_dist, half);
                FPN<F> sl_floor = FPN_SubSat(pos->entry_price, min_sl_dist);
                pos->stop_loss_price = FPN_Min(pos->stop_loss_price, sl_floor);
            }
            else if ((old_regime == REGIME_TRENDING || old_regime == REGIME_TRENDING_DOWN)
                     && new_regime == REGIME_RANGING) {
                FPN<F> tight_tp_offset = FPN_Mul(stddev, FPN_Mul(cfg->take_profit_pct, hundred));
                FPN<F> tight_tp = FPN_AddSat(pos->entry_price, tight_tp_offset);
                pos->take_profit_price = FPN_Min(pos->take_profit_price, tight_tp);

                FPN<F> wide_sl_offset = FPN_Mul(stddev, FPN_Mul(cfg->stop_loss_pct, hundred));
                FPN<F> wide_sl = FPN_SubSat(pos->entry_price, wide_sl_offset);
                pos->stop_loss_price = FPN_Min(pos->stop_loss_price, wide_sl);

                // SL floor: ensure SL distance >= 0.5 × TP distance (2:1 min reward/risk)
                FPN<F> tp_dist = FPN_Sub(pos->take_profit_price, pos->entry_price);
                FPN<F> min_sl_dist = FPN_Mul(tp_dist, half);
                FPN<F> sl_floor = FPN_SubSat(pos->entry_price, min_sl_dist);
                pos->stop_loss_price = FPN_Min(pos->stop_loss_price, sl_floor);
            }
            // entering downtrend: tighten TP (take profits), tighten SL (cut losses)
            // uses momentum_tp_mult (not take_profit_pct×100) — these are momentum positions
            else if (new_regime == REGIME_TRENDING_DOWN) {
                FPN<F> tight_tp_offset = FPN_Mul(stddev, cfg->momentum_tp_mult);
                FPN<F> tight_tp = FPN_AddSat(pos->entry_price, tight_tp_offset);
                pos->take_profit_price = FPN_Min(pos->take_profit_price, tight_tp);

                FPN<F> tight_sl_offset = FPN_Mul(stddev, cfg->momentum_sl_mult);
                FPN<F> tight_sl = FPN_SubSat(pos->entry_price, tight_sl_offset);
                pos->stop_loss_price = FPN_Max(pos->stop_loss_price, tight_sl);

                // SL floor: ensure SL distance >= 0.5 × TP distance (2:1 min reward/risk)
                FPN<F> tp_dist = FPN_Sub(pos->take_profit_price, pos->entry_price);
                FPN<F> min_sl_dist = FPN_Mul(tp_dist, half);
                FPN<F> sl_floor = FPN_SubSat(pos->entry_price, min_sl_dist);
                pos->stop_loss_price = FPN_Min(pos->stop_loss_price, sl_floor);
            }
        }

        active &= active - 1;
    }
}

#endif // REGIME_DETECTOR_HPP
