// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [MOMENTUM / TREND-FOLLOWING STRATEGY]
//======================================================================================================
// the complement to mean reversion — buys breakouts instead of dips
// activates when regime detector identifies a strong directional trend
//
// entry: buy when price breaks ABOVE rolling average + offset (confirms trend strength)
//   - opposite of mean reversion which buys BELOW the average
//   - volume confirmation ensures breakout has participation, not just a thin spike
//   - stddev-scaled offset: wider breakout required in volatile markets
//
// exit: tighter SL (protect against false breakouts), wider TP (let trends run)
//   - SL trails aggressively — momentum failures reverse fast
//   - TP uses a larger stddev multiplier than mean reversion
//
// adaptation: P&L regression adjusts breakout threshold
//   - positive P&L → lower breakout threshold (enter earlier in the trend)
//   - negative P&L → raise threshold (wait for stronger confirmation)
//
// all adaptation is on the slow path. BuyGate just reads buy_conds with gate_direction=1.
//======================================================================================================
#ifndef MOMENTUM_HPP
#define MOMENTUM_HPP

#include "StrategyInterface.hpp"
#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/OrderGates.hpp"
#include "../CoreFrameworks/Portfolio.hpp"
#include "../ML_Headers/LinearRegression3X.hpp"
#include "../ML_Headers/ROR_regressor.hpp"
#include "../ML_Headers/RollingStats.hpp"

//======================================================================================================
// [STATE]
//======================================================================================================
template <unsigned F> struct MomentumState {
    RegressionFeederX<F> feeder;       // P&L regression for adaptive filters
    RORRegressor<F> ror;               // slope-of-slopes
    FPN<F> live_breakout_mult;         // adaptive breakout threshold (stddev multiplier)
    FPN<F> live_vol_mult;              // adaptive volume multiplier
    BuySideGateConditions<F> buy_conds_initial; // anchor for max_shift clamp
    LinearRegression3XResult<F> last_regression;
    int has_regression;
    RegressionFeederX<F> price_feeder; // for trailing R² computation
};

//======================================================================================================
// [INIT]
//======================================================================================================
// called at warmup completion. computes initial breakout buy conditions from rolling stats.
// buy_price = avg + (stddev * breakout_mult) — price must rise ABOVE this to trigger BuyGate
//======================================================================================================
template <unsigned F>
inline void Momentum_Init(MomentumState<F> *state,
                           const RollingStats<F> *rolling,
                           BuySideGateConditions<F> *buy_conds) {
    // breakout price: avg + stddev * breakout_mult
    FPN<F> breakout_offset = FPN_Mul(rolling->price_stddev, state->live_breakout_mult);
    FPN<F> buy_price = FPN_AddSat(rolling->price_avg, breakout_offset);

    // volume gate: same pattern as MR — require significant volume on breakout
    FPN<F> buy_vol = FPN_Mul(rolling->volume_avg, state->live_vol_mult);

    buy_conds->price = buy_price;
    buy_conds->volume = buy_vol;
    buy_conds->gate_direction = 1; // buy ABOVE price
    state->buy_conds_initial = *buy_conds;

    // reset feeders for P&L tracking
    state->feeder = RegressionFeederX_Init<F>();
    state->price_feeder = RegressionFeederX_Init<F>();
    state->ror = RORRegressor_Init<F>();
    state->has_regression = 0;
}

//======================================================================================================
// [ADAPT]
//======================================================================================================
// same feedback loop as MR but tuning breakout_mult instead of offset:
//   1. idle squeeze: lower breakout threshold when no positions and price is running
//   2. P&L regression: adjust breakout_mult based on profitability
//      positive P&L slope → lower threshold (enter breakouts earlier)
//      negative P&L slope → raise threshold (wait for stronger confirmation)
//======================================================================================================
template <unsigned F>
inline void Momentum_Adapt(MomentumState<F> *state,
                            FPN<F> current_price,
                            FPN<F> portfolio_delta,
                            uint16_t active_bitmap,
                            const BuySideGateConditions<F> *buy_conds,
                            const ControllerConfig<F> *cfg) {
    constexpr unsigned N = FPN<F>::N;

    //==================================================================================================
    // IDLE SQUEEZE: lower breakout threshold when no positions and price above buy gate
    // for momentum, "trailing" means price is above our breakout level but we haven't bought
    //==================================================================================================
    {
        int is_empty = (active_bitmap == 0);
        // momentum: squeeze when price is BELOW buy gate (we're waiting for a breakout
        // that already happened while we were too defensive)
        int trailing = FPN_LessThan(current_price, buy_conds->price);
        int should_squeeze = is_empty & trailing;
        uint64_t sq_mask = -(uint64_t)should_squeeze;

        // squeeze breakout_mult toward minimum (breakout_min stddevs)
        FPN<F> breakout_min = cfg->breakout_min;
        FPN<F> gap = FPN_Sub(state->live_breakout_mult, breakout_min);
        FPN<F> step = FPN_Mul(gap, cfg->squeeze_decay);
        FPN<F> masked_step;
        for (unsigned i = 0; i < N; i++) {
            masked_step.w[i] = step.w[i] & sq_mask;
        }
        masked_step.sign = step.sign & should_squeeze;
        state->live_breakout_mult = FPN_SubSat(state->live_breakout_mult, masked_step);
        state->live_breakout_mult = FPN_Max(state->live_breakout_mult, breakout_min);

        // squeeze volume multiplier toward 1.0 (same as MR)
        FPN<F> one = FPN_FromDouble<F>(1.0);
        FPN<F> vmult_gap = FPN_Sub(state->live_vol_mult, one);
        FPN<F> vmult_step = FPN_Mul(vmult_gap, cfg->squeeze_decay);
        FPN<F> masked_vmult;
        for (unsigned i = 0; i < N; i++) {
            masked_vmult.w[i] = vmult_step.w[i] & sq_mask;
        }
        masked_vmult.sign = vmult_step.sign & should_squeeze;
        state->live_vol_mult = FPN_SubSat(state->live_vol_mult, masked_vmult);
        state->live_vol_mult = FPN_Max(state->live_vol_mult, one);
    }

    //==================================================================================================
    // P&L REGRESSION: push portfolio delta, compute regression, adjust breakout_mult
    //==================================================================================================
    RegressionFeederX_Push(&state->feeder, portfolio_delta);
    RegressionFeederX_Push(&state->price_feeder, current_price);

    if (state->feeder.count >= MAX_WINDOW) {
        LinearRegression3XResult<F> reg = RegressionFeederX_Compute(&state->feeder);
        state->last_regression = reg;
        state->has_regression = 1;

        int confident = FPN_GreaterThanOrEqual(reg.r_squared, cfg->r2_threshold);

        // for momentum: NEGATIVE slope means we should raise the breakout threshold
        // (be more selective about which breakouts to enter)
        // POSITIVE slope means lower threshold (enter breakouts earlier)
        // this is OPPOSITE to MR where negative slope tightens (raises) the offset
        FPN<F> shift = FPN_Mul(reg.model.slope, cfg->slope_scale_buy);
        // negate: positive P&L → lower breakout_mult (negative shift to subtract)
        FPN<F> neg_shift = FPN_Negate(shift);

        FPN<F> masked_shift;
        uint64_t conf_mask = -(uint64_t)confident;
        for (unsigned i = 0; i < N; i++) {
            masked_shift.w[i] = neg_shift.w[i] & conf_mask;
        }
        masked_shift.sign = neg_shift.sign & confident;

        FPN<F> scaled_shift = FPN_Mul(masked_shift, cfg->stddev_adapt_scale);
        state->live_breakout_mult = FPN_AddSat(state->live_breakout_mult, scaled_shift);

        // clamp breakout_mult to reasonable range
        FPN<F> breakout_min = cfg->breakout_min;
        FPN<F> breakout_max = FPN_FromDouble<F>(5.0);
        state->live_breakout_mult = FPN_Max(state->live_breakout_mult, breakout_min);
        state->live_breakout_mult = FPN_Min(state->live_breakout_mult, breakout_max);
    }

    (void)current_price;
}

//======================================================================================================
// [BUY SIGNAL]
//======================================================================================================
// computes breakout buy conditions: buy_price = avg + (stddev * breakout_mult)
// gate_direction = 1 means BuyGate checks price >= buy_price (buy above)
//======================================================================================================
template <unsigned F>
inline BuySideGateConditions<F> Momentum_BuySignal(MomentumState<F> *state,
                                                     const RollingStats<F> *rolling,
                                                     const RollingStats<F, 512> *rolling_long,
                                                     const ControllerConfig<F> *cfg) {
    BuySideGateConditions<F> conds;

    // breakout price: avg + stddev * live_breakout_mult
    FPN<F> breakout_offset = FPN_Mul(rolling->price_stddev, state->live_breakout_mult);
    conds.price = FPN_AddSat(rolling->price_avg, breakout_offset);

    // volume: same pattern as MR
    conds.volume = FPN_Mul(rolling->volume_avg, state->live_vol_mult);
    conds.gate_direction = 1; // buy ABOVE price

    // update initial conditions for shift clamp tracking
    state->buy_conds_initial = conds;

    // apply regression-based gate shift if available (same pattern as MR)
    if (state->has_regression) {
        int confident = FPN_GreaterThanOrEqual(state->last_regression.r_squared,
                                                cfg->r2_threshold);
        FPN<F> shift = FPN_Mul(state->last_regression.model.slope, cfg->slope_scale_buy);

        // clamp to max_shift
        FPN<F> max_shift_abs = FPN_Mul(rolling->price_avg, cfg->max_shift);
        shift = FPN_Min(shift, max_shift_abs);
        shift = FPN_Max(shift, FPN_Negate(max_shift_abs));

        FPN<F> masked_shift;
        uint64_t conf_mask = -(uint64_t)confident;
        constexpr unsigned N = FPN<F>::N;
        for (unsigned i = 0; i < N; i++) {
            masked_shift.w[i] = shift.w[i] & conf_mask;
        }
        masked_shift.sign = shift.sign & confident;

        conds.price = FPN_AddSat(conds.price, masked_shift);

        // clamp to initial +/- max_shift
        FPN<F> upper = FPN_AddSat(state->buy_conds_initial.price, max_shift_abs);
        FPN<F> lower = FPN_SubSat(state->buy_conds_initial.price, max_shift_abs);
        conds.price = FPN_Max(conds.price, lower);
        conds.price = FPN_Min(conds.price, upper);
    }

    // multi-timeframe gate: same check as MR — block buys when long trend is negative
    // for momentum this is especially important — don't buy breakouts in a downtrend
    {
        int long_enabled = !FPN_IsZero(cfg->min_long_slope);
        FPN<F> relative_long_slope = FPN_IsZero(rolling_long->price_avg)
            ? FPN_Zero<F>()
            : FPN_DivNoAssert(rolling_long->price_slope, rolling_long->price_avg);
        int long_pass = FPN_GreaterThanOrEqual(relative_long_slope, cfg->min_long_slope);

        int long_blocked = long_enabled & !long_pass;
        uint64_t block_mask = -(uint64_t)long_blocked;
        constexpr unsigned N2 = FPN<F>::N;
        for (unsigned i = 0; i < N2; i++) {
            conds.price.w[i]  &= ~block_mask;
            conds.volume.w[i] &= ~block_mask;
        }
        conds.price.sign  &= !long_blocked;
        conds.volume.sign &= !long_blocked;
    }

    return conds;
}

//======================================================================================================
// [EXIT ADJUST]
//======================================================================================================
// momentum trailing: tighter SL trail (failures reverse fast), wider TP trail (let runs extend)
// uses the same hold_score = SNR * R² pattern as MR but with different multipliers
// the key difference: momentum uses cfg->momentum_sl_mult (tighter) instead of cfg->sl_trail_mult
//======================================================================================================
template <unsigned F>
inline void Momentum_ExitAdjust(Portfolio<F> *portfolio, FPN<F> current_price,
                                  const RollingStats<F> *rolling,
                                  MomentumState<F> *state,
                                  const ControllerConfig<F> *cfg) {
    // skip if trailing disabled
    if (FPN_IsZero(cfg->tp_hold_score)) return;
    if (FPN_IsZero(rolling->price_stddev)) return;

    // compute R² from price regression (same pattern as MR)
    FPN<F> r_squared = FPN_Zero<F>();
    FPN<F> reg_slope = FPN_Zero<F>();
    if (state->price_feeder.count >= MAX_WINDOW) {
        LinearRegression3XResult<F> price_reg = RegressionFeederX_Compute(&state->price_feeder);
        r_squared = price_reg.r_squared;
        reg_slope = price_reg.model.slope;
    }

    FPN<F> snr = FPN_DivNoAssert(reg_slope, rolling->price_stddev);
    FPN<F> hold_score = FPN_Mul(snr, r_squared);
    int should_trail = FPN_GreaterThanOrEqual(hold_score, cfg->tp_hold_score);

    uint16_t active = portfolio->active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);
        Position<F> *pos = &portfolio->positions[idx];

        int above_tp = FPN_GreaterThan(current_price, pos->original_tp);

        if (above_tp & should_trail) {
            // momentum trailing: use tp_trail_mult for TP (same as MR)
            // but use momentum_sl_mult for tighter SL (cut losers faster in trends)
            FPN<F> tp_offset = FPN_Mul(rolling->price_stddev, cfg->tp_trail_mult);
            FPN<F> trailing_tp = FPN_Sub(current_price, tp_offset);
            pos->take_profit_price = FPN_Max(pos->take_profit_price, trailing_tp);

            // tighter SL: momentum_sl_mult is typically smaller than sl_trail_mult
            FPN<F> sl_offset = FPN_Mul(rolling->price_stddev, cfg->momentum_sl_mult);
            FPN<F> trailing_sl = FPN_Sub(current_price, sl_offset);
            pos->stop_loss_price = FPN_Max(pos->stop_loss_price, trailing_sl);
        }

        active &= active - 1;
    }
}

#endif // MOMENTUM_HPP
