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

// regime constants, RegimeInfo, and REGIME_STRATEGY_TABLE are in StrategyInterface.hpp

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
    FPN<F> volume_delta;      // net buy/sell pressure [-1.0, +1.0] (from Binance "m" field)
    // EMA/SMA crossover signals — primary trending/ranging indicator
    // EMA reacts every tick (~333-tick window), SMA lags (128/512 samples at slow-path rate)
    // spread = (ema - sma) / sma: positive = EMA above SMA = bullish, magnitude = strength
    FPN<F> ema_sma_spread;    // normalized spread vs 128-sample SMA
    FPN<F> ema_sma_spread_long; // normalized spread vs 512-sample SMA (multi-timeframe)
    int    ema_above_sma;     // 1 if ema > short SMA (bullish crossover state)
    // data sufficiency flags
    int short_count;
    int long_count;
    int ror_ready;            // 1 if ROR has enough data for meaningful output
    // ML model output (populated by ModelInference if model loaded, zero otherwise)
    FPN<F> model_score;       // raw model prediction [0, 1] — higher = more likely trending
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
                                   const RORRegressor<F> *ror,
                                   FPN<F> ema_price) {
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

    // volume delta: net buy/sell pressure from short window
    sig->volume_delta = rolling->volume_delta;

    // model score: initialized to zero, populated externally by PortfolioController
    // if a regime enrichment model is loaded
    sig->model_score = FPN_Zero<F>();

    // EMA/SMA crossover: (ema - sma) / sma
    // normalized so threshold is asset-independent (same value works for BTC and ETH)
    if (!FPN_IsZero(rolling->price_avg) && !FPN_IsZero(ema_price)) {
        sig->ema_sma_spread = FPN_DivNoAssert(
            FPN_Sub(ema_price, rolling->price_avg), rolling->price_avg);
        sig->ema_above_sma = !sig->ema_sma_spread.sign & !FPN_IsZero(sig->ema_sma_spread);
    } else {
        sig->ema_sma_spread = FPN_Zero<F>();
        sig->ema_above_sma = 0;
    }

    if (!FPN_IsZero(rolling_long->price_avg) && !FPN_IsZero(ema_price)) {
        sig->ema_sma_spread_long = FPN_DivNoAssert(
            FPN_Sub(ema_price, rolling_long->price_avg), rolling_long->price_avg);
    } else {
        sig->ema_sma_spread_long = FPN_Zero<F>();
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

    // --- trending score (EMA/SMA crossover-based) ---
    // EMA reacts every tick, SMA lags — crossover detects trends as they start
    // spread = (ema - sma) / sma: magnitude = trend strength, sign = direction
    int trending_score = 0;
    int up_signals = 0;
    int down_signals = 0;

    // signal 1: short crossover — EMA vs 128-sample SMA
    FPN<F> abs_spread = FPN_Abs(sig->ema_sma_spread);
    int crossover_strong = FPN_GreaterThan(abs_spread, cfg->regime_crossover_threshold);
    trending_score += crossover_strong;
    up_signals += crossover_strong & sig->ema_above_sma;
    down_signals += crossover_strong & !sig->ema_above_sma;

    // signal 2: long crossover — EMA vs 512-sample SMA (multi-timeframe confirmation)
    FPN<F> abs_spread_long = FPN_Abs(sig->ema_sma_spread_long);
    int long_has_data = (sig->long_count >= 64);
    int long_ema_above = !sig->ema_sma_spread_long.sign & !FPN_IsZero(sig->ema_sma_spread_long);
    int long_crossover_strong = long_has_data &
        FPN_GreaterThan(abs_spread_long, cfg->regime_crossover_threshold);
    trending_score += long_crossover_strong;
    up_signals += long_crossover_strong & long_ema_above;
    down_signals += long_crossover_strong & !long_ema_above;

    // hidden downtrend: EMA far below long SMA + short crossover neutral
    // catches macro downtrends where short window mean-reverts inside the trend
    int long_down_only = long_has_data
        & FPN_GreaterThan(abs_spread_long, FPN_Mul(cfg->regime_crossover_threshold, FPN_FromDouble<F>(2.0)))
        & !long_ema_above & !crossover_strong;
    down_signals += long_down_only;
    trending_score += long_down_only;

    // signal 4: price movement is consistent (high R² — orthogonal to crossover)
    int consistent = FPN_GreaterThan(sig->short_r2, cfg->regime_r2_threshold);
    trending_score += consistent;

    // signal 5: trend is accelerating (ROR — orthogonal, catches steepening trends)
    int ror_positive = sig->ror_ready & FPN_GreaterThan(sig->ror_slope, FPN_Zero<F>());
    int ror_negative = sig->ror_ready & FPN_LessThan(sig->ror_slope, FPN_Zero<F>());
    trending_score += (ror_positive | ror_negative);
    up_signals += ror_positive;
    down_signals += ror_negative;

    // signal 6: volume rising in direction of crossover (confirmation)
    int vol_confirms = FPN_GreaterThan(sig->volume_slope, FPN_Zero<F>()) & crossover_strong;
    trending_score += vol_confirms;

    // signal 7: ML model regime enrichment (Mode A)
    // model_score > 0.5 = model predicts trending, weighted by regime_model_weight
    if (!FPN_IsZero(sig->model_score)) {
        int model_trending = FPN_GreaterThan(sig->model_score, FPN_FromDouble<F>(0.5));
        int weight = (int)FPN_ToDouble(cfg->regime_model_weight);
        trending_score += model_trending * weight;
    }

    // --- volatile score (unchanged — vol_ratio based) ---
    int volatile_score = 0;

    // variance spike relative to longer-term baseline (self-adapting)
    int vol_spike = FPN_GreaterThan(sig->vol_ratio, cfg->regime_vol_spike_ratio);
    volatile_score += vol_spike;

    // high variance but no consistent direction (choppy)
    int inconsistent = !consistent;
    volatile_score += vol_spike & inconsistent;

    // --- classify ---
    // trending needs at least 2 signals AND at least one crossover signal
    // volatile needs at least 2 signals (spike + no direction)
    // direction: more down signals = TRENDING_DOWN, otherwise TRENDING (up)
    // uptrend split: strong crossover = TRENDING (momentum), mild = MILD_TREND (EMA Cross)
    int has_crossover = crossover_strong | long_crossover_strong;
    int detected;
    if (trending_score >= 2 && has_crossover && consistent && trending_score > volatile_score) {
        if (down_signals > up_signals) {
            detected = REGIME_TRENDING_DOWN;
        } else {
            // split uptrend: use max of short/long spread for strength assessment
            FPN<F> max_spread = FPN_Max(abs_spread, abs_spread_long);
            int strong = FPN_GreaterThan(max_spread, cfg->regime_strong_crossover);
            detected = strong ? REGIME_TRENDING : REGIME_MILD_TREND;
        }
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
    return (regime >= 0 && regime < NUM_REGIMES)
        ? REGIME_STRATEGY_TABLE[regime] : STRATEGY_MEAN_REVERSION;
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
            // RANGING → MILD_TREND: both buy dips, widen TP slightly (uptrend confirmed)
            else if (old_regime == REGIME_RANGING && new_regime == REGIME_MILD_TREND) {
                FPN<F> mr_tp_offset = FPN_Mul(stddev, FPN_Mul(cfg->take_profit_pct, hundred));
                FPN<F> mild_widen = FPN_Mul(mr_tp_offset, FPN_FromDouble<F>(1.3));
                FPN<F> wider_tp = FPN_AddSat(pos->entry_price, mild_widen);
                pos->take_profit_price = FPN_Max(pos->take_profit_price, wider_tp);

                FPN<F> tp_dist = FPN_Sub(pos->take_profit_price, pos->entry_price);
                FPN<F> min_sl_dist = FPN_Mul(tp_dist, half);
                FPN<F> sl_floor = FPN_SubSat(pos->entry_price, min_sl_dist);
                pos->stop_loss_price = FPN_Min(pos->stop_loss_price, sl_floor);
            }
            // MILD_TREND → TRENDING: uptrend strengthening, widen TP to momentum levels
            else if (old_regime == REGIME_MILD_TREND && new_regime == REGIME_TRENDING) {
                FPN<F> wide_tp_offset = FPN_Mul(stddev, cfg->momentum_tp_mult);
                FPN<F> wide_tp = FPN_AddSat(pos->entry_price, wide_tp_offset);
                pos->take_profit_price = FPN_Max(pos->take_profit_price, wide_tp);

                FPN<F> tight_sl_offset = FPN_Mul(stddev, cfg->momentum_sl_mult);
                FPN<F> tight_sl = FPN_SubSat(pos->entry_price, tight_sl_offset);
                pos->stop_loss_price = FPN_Max(pos->stop_loss_price, tight_sl);

                FPN<F> tp_dist = FPN_Sub(pos->take_profit_price, pos->entry_price);
                FPN<F> min_sl_dist = FPN_Mul(tp_dist, half);
                FPN<F> sl_floor = FPN_SubSat(pos->entry_price, min_sl_dist);
                pos->stop_loss_price = FPN_Min(pos->stop_loss_price, sl_floor);
            }
            // TRENDING/TRENDING_DOWN/MILD_TREND → RANGING: tighten TP, widen SL for chop
            else if ((old_regime == REGIME_TRENDING || old_regime == REGIME_TRENDING_DOWN
                      || old_regime == REGIME_MILD_TREND)
                     && new_regime == REGIME_RANGING) {
                FPN<F> tight_tp_offset = FPN_Mul(stddev, FPN_Mul(cfg->take_profit_pct, hundred));
                FPN<F> tight_tp = FPN_AddSat(pos->entry_price, tight_tp_offset);
                pos->take_profit_price = FPN_Min(pos->take_profit_price, tight_tp);

                // fee floor: TP must cover round-trip fees even after tightening
                FPN<F> fee_floor = FPN_AddSat(pos->entry_price,
                    FPN_Mul(FPN_Mul(pos->entry_price, cfg->fee_rate), cfg->fee_floor_mult));
                pos->take_profit_price = FPN_Max(pos->take_profit_price, fee_floor);

                FPN<F> wide_sl_offset = FPN_Mul(stddev, FPN_Mul(cfg->stop_loss_pct, hundred));
                FPN<F> wide_sl = FPN_SubSat(pos->entry_price, wide_sl_offset);
                pos->stop_loss_price = FPN_Min(pos->stop_loss_price, wide_sl);

                // SL ceiling: if TP was tightened (vol dropped since fill), don't let SL
                // stay at the old high-vol width. SL distance must not exceed TP distance.
                // without this, a fill at high σ followed by regime switch at low σ
                // produces SL > TP (inverted risk/reward)
                FPN<F> tp_dist = FPN_Sub(pos->take_profit_price, pos->entry_price);
                FPN<F> sl_ceiling = FPN_SubSat(pos->entry_price, tp_dist);
                pos->stop_loss_price = FPN_Max(pos->stop_loss_price, sl_ceiling);

                // SL floor: ensure SL distance >= 0.5 × TP distance (2:1 min reward/risk)
                FPN<F> min_sl_dist = FPN_Mul(tp_dist, half);
                FPN<F> sl_floor = FPN_SubSat(pos->entry_price, min_sl_dist);
                pos->stop_loss_price = FPN_Min(pos->stop_loss_price, sl_floor);
            }
            // TRENDING → MILD_TREND: trend weakening, tighten TP moderately
            else if (old_regime == REGIME_TRENDING && new_regime == REGIME_MILD_TREND) {
                FPN<F> mr_tp_offset = FPN_Mul(stddev, FPN_Mul(cfg->take_profit_pct, hundred));
                FPN<F> mild_tp = FPN_Mul(mr_tp_offset, FPN_FromDouble<F>(1.3));
                FPN<F> tighter_tp = FPN_AddSat(pos->entry_price, mild_tp);
                pos->take_profit_price = FPN_Min(pos->take_profit_price, tighter_tp);

                // fee floor: TP must cover round-trip fees even after tightening
                FPN<F> fee_floor = FPN_AddSat(pos->entry_price,
                    FPN_Mul(FPN_Mul(pos->entry_price, cfg->fee_rate), cfg->fee_floor_mult));
                pos->take_profit_price = FPN_Max(pos->take_profit_price, fee_floor);

                // SL ceiling + floor
                FPN<F> tp_dist = FPN_Sub(pos->take_profit_price, pos->entry_price);
                FPN<F> sl_ceiling = FPN_SubSat(pos->entry_price, tp_dist);
                pos->stop_loss_price = FPN_Max(pos->stop_loss_price, sl_ceiling);
                FPN<F> min_sl_dist = FPN_Mul(tp_dist, half);
                FPN<F> sl_floor = FPN_SubSat(pos->entry_price, min_sl_dist);
                pos->stop_loss_price = FPN_Min(pos->stop_loss_price, sl_floor);
            }
            // entering downtrend: tighten TP (take profits), tighten SL (cut losses)
            else if (new_regime == REGIME_TRENDING_DOWN) {
                FPN<F> tight_tp_offset = FPN_Mul(stddev, cfg->momentum_tp_mult);
                FPN<F> tight_tp = FPN_AddSat(pos->entry_price, tight_tp_offset);
                pos->take_profit_price = FPN_Min(pos->take_profit_price, tight_tp);

                // fee floor: TP must cover round-trip fees even after tightening
                FPN<F> fee_floor = FPN_AddSat(pos->entry_price,
                    FPN_Mul(FPN_Mul(pos->entry_price, cfg->fee_rate), cfg->fee_floor_mult));
                pos->take_profit_price = FPN_Max(pos->take_profit_price, fee_floor);

                FPN<F> tight_sl_offset = FPN_Mul(stddev, cfg->momentum_sl_mult);
                FPN<F> tight_sl = FPN_SubSat(pos->entry_price, tight_sl_offset);
                pos->stop_loss_price = FPN_Max(pos->stop_loss_price, tight_sl);

                // SL ceiling: SL distance must not exceed TP distance (no inverted risk/reward)
                FPN<F> tp_dist = FPN_Sub(pos->take_profit_price, pos->entry_price);
                FPN<F> sl_ceiling = FPN_SubSat(pos->entry_price, tp_dist);
                pos->stop_loss_price = FPN_Max(pos->stop_loss_price, sl_ceiling);

                // SL floor: ensure SL distance >= 0.5 × TP distance (2:1 min reward/risk)
                FPN<F> min_sl_dist = FPN_Mul(tp_dist, half);
                FPN<F> sl_floor = FPN_SubSat(pos->entry_price, min_sl_dist);
                pos->stop_loss_price = FPN_Min(pos->stop_loss_price, sl_floor);
            }
        }

        active &= active - 1;
    }
}

#endif // REGIME_DETECTOR_HPP
