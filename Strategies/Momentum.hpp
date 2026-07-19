// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[Strategies/Momentum.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[trend-following — buys breakouts ABOVE avg + stddev*mult (gate_direction=1); tighter SL / wider TP; P&L regression adapts the breakout threshold]
// [CONTAINS]
//   - [STRUCT]_[MomentumState]
//   - [FUNCTION]_[Momentum_Adapt]        (Init shares the file head)
//   - [FUNCTION]_[Momentum_BuySignal]
//   - [FUNCTION]_[Momentum_ExitAdjust]   (+ ExitAdjustSharded ratchet twin)
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

#include <cstdint>  // v5.15.5.F.4d TECH_DEBT-083 close — explicit IWYU for uintN_t (was transitively pulled)
#include "StrategyInterface.hpp"
#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/OrderGates.hpp"
#include "../CoreFrameworks/Portfolio.hpp"
#include "../MemHeaders/OmsStateFlagRegistry.hpp"  // v5.15.5.C.2 (S3a) — MASK_OMS_STATE_*
#include "../ML_Headers/LinearRegression3X.hpp"
#include "../ML_Headers/ROR_regressor.hpp"
#include "../ML_Headers/RollingStats.hpp"

//======================================================================
// [STRUCT]_[MomentumState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[P&L + price regression feeders, adaptive breakout/volume multipliers, initial-conds anchor for the max_shift clamp]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F> struct MomentumState {
    RegressionFeederX<F> feeder;       // P&L regression for adaptive filters
    RORRegressor<F> ror;               // slope-of-slopes
    FPN_Binary<F> live_breakout_mult;         // adaptive breakout threshold (stddev multiplier)
    FPN_Binary<F> live_vol_mult;              // adaptive volume multiplier
    BuySideGateConditions<F> buy_conds_initial; // anchor for max_shift clamp
    LinearRegression3XResult<F> last_regression;
    int has_regression;
    RegressionFeederX<F> price_feeder; // for trailing R² computation
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[704B]
// [ALIGN]_[16]
// [CACHE_LINES]_[11]
// [STRADDLE]_[last_regression@496]
//======================================================================
// [END_STRUCT]_[MomentumState]
//======================================================================

//------------------------------------------------------------------------------
// [SECTION]_[INIT]
//------------------------------------------------------------------------------
// called at warmup completion. computes initial breakout buy conditions from rolling stats.
// buy_price = avg + (stddev * breakout_mult) — price must rise ABOVE this to trigger BuyGate
//------------------------------------------------------------------------------
template <unsigned F>
inline void Momentum_Init(MomentumState<F> *state,
                           const RollingStats<F> *rolling,
                           BuySideGateConditions<F> *buy_conds) {
    // breakout price: avg + stddev * breakout_mult
    FPN_Binary<F> breakout_offset = FPN_Mul(rolling->price_stddev, state->live_breakout_mult);
    FPN_Binary<F> buy_price = FPN_AddSat(rolling->price_avg, breakout_offset);

    // volume gate: same pattern as MR — require significant volume on breakout
    FPN_Binary<F> buy_vol = FPN_Mul(rolling->volume_avg, state->live_vol_mult);

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

//======================================================================
// [FUNCTION]_[Momentum_Adapt]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH]]
// [REFERENCE]_[INVARIANT]_[H20]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[idle squeeze (branchless mask) + P&L regression -> breakout_mult; positive slope lowers the threshold (enter earlier), negative raises it]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void Momentum_Adapt(MomentumState<F> *state,
                            FPN_Binary<F> current_price,
                            FPN_Binary<F> portfolio_delta,
                            uint16_t active_bitmap,
                            const BuySideGateConditions<F> *buy_conds,
                            const ControllerConfig<F> *cfg) {

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
        unsigned __int128 sq_mask = -(unsigned __int128)(unsigned)should_squeeze;

        // squeeze breakout_mult toward minimum (breakout_min stddevs)
        FPN_Binary<F> breakout_min = cfg->breakout_min;
        FPN_Binary<F> gap = FPN_Sub(state->live_breakout_mult, breakout_min);
        FPN_Binary<F> step = FPN_Mul(gap, cfg->squeeze_decay);
        FPN_Binary<F> masked_step { (__int128)((unsigned __int128)step.v & sq_mask) };
        state->live_breakout_mult = FPN_SubSat(state->live_breakout_mult, masked_step);
        state->live_breakout_mult = FPN_Max(state->live_breakout_mult, breakout_min);

        // squeeze volume multiplier toward 1.0 (same as MR)
        FPN_Binary<F> one = FPN_FromDouble<F>(1.0);
        FPN_Binary<F> vmult_gap = FPN_Sub(state->live_vol_mult, one);
        FPN_Binary<F> vmult_step = FPN_Mul(vmult_gap, cfg->squeeze_decay);
        FPN_Binary<F> masked_vmult { (__int128)((unsigned __int128)vmult_step.v & sq_mask) };
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
        FPN_Binary<F> shift = FPN_Mul(reg.model.slope, cfg->slope_scale_buy);
        // negate: positive P&L → lower breakout_mult (negative shift to subtract)
        FPN_Binary<F> neg_shift = FPN_Negate(shift);

        FPN_Binary<F> masked_shift { (__int128)((unsigned __int128)neg_shift.v & -(unsigned __int128)(unsigned)confident) };

        FPN_Binary<F> scaled_shift = FPN_Mul(masked_shift, cfg->stddev_adapt_scale);
        state->live_breakout_mult = FPN_AddSat(state->live_breakout_mult, scaled_shift);

        // clamp breakout_mult to reasonable range
        FPN_Binary<F> breakout_min = cfg->breakout_min;
        FPN_Binary<F> breakout_max = FPN_FromDouble<F>(5.0);
        state->live_breakout_mult = FPN_Max(state->live_breakout_mult, breakout_min);
        state->live_breakout_mult = FPN_Min(state->live_breakout_mult, breakout_max);
    }

    (void)current_price;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// same feedback loop as MR but tuning breakout_mult instead of offset:
//   1. idle squeeze: lower breakout threshold when no positions and price is running
//   2. P&L regression: adjust breakout_mult based on profitability
//      positive P&L slope → lower threshold (enter breakouts earlier)
//      negative P&L slope → raise threshold (wait for stronger confirmation)
//======================================================================
// [END_FUNCTION]_[Momentum_Adapt]
//======================================================================

//======================================================================
// [FUNCTION]_[Momentum_BuySignal]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH]]
// [REFERENCE]_[INVARIANT]_[H20]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[breakout gate = base_avg + stddev*mult (buy ABOVE); regression shift (max_shift-clamped, branchless confidence mask) + long-trend and R2-floor vetoes]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline BuySideGateConditions<F> Momentum_BuySignal(MomentumState<F> *state,
                                                     const RollingStats<F> *rolling,
                                                     const RollingStats<F, 512> *rolling_long,
                                                     const ControllerConfig<F> *cfg,
                                                     FPN_Binary<F> ema_price = FPN_Zero<F>()) {
    BuySideGateConditions<F> conds;

    // base average: EMA when provided (nonzero), rolling avg otherwise
    FPN_Binary<F> base_avg = FPN_IsZero(ema_price) ? rolling->price_avg : ema_price;

    // breakout price: base + stddev * live_breakout_mult
    FPN_Binary<F> breakout_offset = FPN_Mul(rolling->price_stddev, state->live_breakout_mult);
    conds.price = FPN_AddSat(base_avg, breakout_offset);

    // volume: same pattern as MR
    conds.volume = FPN_Mul(rolling->volume_avg, state->live_vol_mult);
    conds.gate_direction = 1; // buy ABOVE price

    // update initial conditions for shift clamp tracking
    state->buy_conds_initial = conds;

    // apply regression-based gate shift if available (same pattern as MR)
    if (state->has_regression) {
        int confident = FPN_GreaterThanOrEqual(state->last_regression.r_squared,
                                                cfg->r2_threshold);
        FPN_Binary<F> shift = FPN_Mul(state->last_regression.model.slope, cfg->slope_scale_buy);

        // clamp to max_shift
        FPN_Binary<F> max_shift_abs = FPN_Mul(rolling->price_avg, cfg->max_shift);
        shift = FPN_Min(shift, max_shift_abs);
        shift = FPN_Max(shift, FPN_Negate(max_shift_abs));

        // branchless: masked_shift = confident ? shift : 0 (16B → mask the whole .v)
        unsigned __int128 conf_mask = -(unsigned __int128)(unsigned)confident;
        FPN_Binary<F> masked_shift { (__int128)((unsigned __int128)shift.v & conf_mask) };

        conds.price = FPN_AddSat(conds.price, masked_shift);

        // clamp to initial +/- max_shift
        FPN_Binary<F> upper = FPN_AddSat(state->buy_conds_initial.price, max_shift_abs);
        FPN_Binary<F> lower = FPN_SubSat(state->buy_conds_initial.price, max_shift_abs);
        conds.price = FPN_Max(conds.price, lower);
        conds.price = FPN_Min(conds.price, upper);
    }

    // multi-timeframe gate: same check as MR — block buys when long trend is negative
    // for momentum this is especially important — don't buy breakouts in a downtrend
    {
        int long_enabled = !FPN_IsZero(cfg->min_long_slope);
        FPN_Binary<F> relative_long_slope = FPN_IsZero(rolling_long->price_avg)
            ? FPN_Zero<F>()
            : FPN_DivNoAssert(rolling_long->price_slope, rolling_long->price_avg);
        int long_pass = FPN_GreaterThanOrEqual(relative_long_slope, cfg->min_long_slope);

        int long_ok = long_pass | !long_enabled;
        Gate_ZeroAll(&conds, long_ok);
    }

    // R² floor: don't enter momentum trades in choppy markets
    // low R² means the trend is inconsistent — breakout entries have inverted R:R
    {
        int r2_enabled = !FPN_IsZero(cfg->momentum_r2_min);
        int r2_pass = FPN_GreaterThanOrEqual(rolling->price_r_squared, cfg->momentum_r2_min);
        int r2_ok = r2_pass | !r2_enabled;
        Gate_ZeroAll(&conds, r2_ok);
    }

    return conds;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// computes breakout buy conditions: buy_price = avg + (stddev * breakout_mult)
// gate_direction = 1 means BuyGate checks price >= buy_price (buy above)
//======================================================================
// [END_FUNCTION]_[Momentum_BuySignal]
//======================================================================

//======================================================================
// [FUNCTION]_[Momentum_ExitAdjust]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[hold_score = SNR*R2 gates the trail — tighter SL (momentum_sl_mult) + wider TP + 2:1 floor; ExitAdjustSharded (ratchet_sl twin, fee-floor capped) shares the section]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void Momentum_ExitAdjust(Portfolio<F> *portfolio, Money current_price,
                                  const RollingStats<F> *rolling,
                                  MomentumState<F> *state,
                                  const ControllerConfig<F> *cfg) {
    // skip if trailing disabled
    if (FPN_IsZero(cfg->tp_hold_score)) return;
    if (FPN_IsZero(rolling->price_stddev)) return;

    // compute R² from price regression (same pattern as MR)
    FPN_Binary<F> r_squared = FPN_Zero<F>();
    FPN_Binary<F> reg_slope = FPN_Zero<F>();
    if (state->price_feeder.count >= MAX_WINDOW) {
        LinearRegression3XResult<F> price_reg = RegressionFeederX_Compute(&state->price_feeder);
        r_squared = price_reg.r_squared;
        reg_slope = price_reg.model.slope;
    }

    FPN_Binary<F> snr = FPN_DivNoAssert(reg_slope, rolling->price_stddev);
    FPN_Binary<F> hold_score = FPN_Mul(snr, r_squared);
    int should_trail = FPN_GreaterThanOrEqual(hold_score, cfg->tp_hold_score);

    uint16_t active = portfolio->active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);
        Position<F> *pos = &portfolio->positions[idx];

        int above_tp = Money_Gt(current_price, pos->original_tp);

        if (above_tp & should_trail) {
            // momentum trailing: use tp_trail_mult for TP (same as MR)
            // but use momentum_sl_mult for tighter SL (cut losers faster in trends)
            FPN_Binary<F> tp_offset = FPN_Mul(rolling->price_stddev, cfg->tp_trail_mult);
            Money trailing_tp = Money_Sub(current_price, Money_FromBinary(tp_offset));
            pos->take_profit_price = Money_Max(pos->take_profit_price, trailing_tp);

            // tighter SL: momentum_sl_mult is typically smaller than sl_trail_mult
            FPN_Binary<F> sl_offset = FPN_Mul(rolling->price_stddev, cfg->momentum_sl_mult);
            Money trailing_sl = Money_Sub(current_price, Money_FromBinary(sl_offset));
            pos->stop_loss_price = Money_Max(pos->stop_loss_price, trailing_sl);

            // SL floor: enforce 2:1 min reward/risk after trailing adjustments
            // only applies when SL is still below entry (at-risk position).
            // once SL trails above entry, the position is a guaranteed win — no floor needed
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
// v5.4.0 Phase 2.3: sharded equivalent of Momentum_ExitAdjust above.
// Same shape as MeanReversion_ExitAdjustSharded but uses momentum_sl_mult
// (the tighter momentum-specific SL trail) instead of sl_trail_mult.
//
// Falling-knife guard: Strategy_WriteRatchetSL applies the v5.1.7 fee-floor
// cap (entry × (1 - 3 × fee_rate_taker)). The momentum trailing is tighter
// than MR's by design ("cut losers fast in trends"), but the cap prevents
// the ratchet from inverting above the safe-fee zone — which would close
// the position at near-breakeven gross (net-negative after fees) on the
// first tiny pullback. This is exactly the symptom the user reported on
// fast drops; the cap turns it from a bug into a guaranteed-non-negative
// exit.
//
// TP trailing deferred to Phase 3 (parallel TP-ratchet channel).
//======================================================================================================
namespace tt {
template <unsigned F> struct EventLoopState;
template <unsigned F>
bool Strategy_WriteRatchetSL(EventLoopState<F>* state, int slot,
                              FPN_Binary<F> proposed_sl, FPN_Binary<F> entry_price,
                              const ControllerConfig<F>* cfg);

template <unsigned F, unsigned W>
inline void Momentum_ExitAdjustSharded(
    EventLoopState<F>* state,
    int slot,
    MomentumState<F>* mom,
    Money current_price,
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* cfg
) {
    if (FPN_IsZero(cfg->tp_hold_score))    return;
    if (FPN_IsZero(rolling->price_stddev)) return;

    FPN_Binary<F> r_squared = FPN_Zero<F>();
    FPN_Binary<F> reg_slope = FPN_Zero<F>();
    if (mom->price_feeder.count >= MAX_WINDOW) {
        LinearRegression3XResult<F> price_reg =
            RegressionFeederX_Compute(&mom->price_feeder);
        r_squared = price_reg.r_squared;
        reg_slope = price_reg.model.slope;
    }
    FPN_Binary<F> snr        = FPN_DivNoAssert(reg_slope, rolling->price_stddev);
    FPN_Binary<F> hold_score = FPN_Mul(snr, r_squared);
    if (!FPN_GreaterThanOrEqual(hold_score, cfg->tp_hold_score)) return;

    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    int partial_on = BITMAP_IS_SET(state->oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
    uint16_t my_mask = partial_on
        ? (uint16_t)((1u << (slot * 2)) | (1u << (slot * 2 + 1)))
        : (uint16_t)(1u << slot);
    uint16_t bm = (uint16_t)(state->oms->portfolio.active_bitmap & my_mask);

    // Momentum: tighter trail (momentum_sl_mult) — but Strategy_WriteRatchetSL's
    // fee-floor cap prevents inversion. Fall back to sl_trail_mult if the
    // momentum-specific cfg field is zero.
    FPN_Binary<F> trail_mult = !FPN_IsZero(cfg->momentum_sl_mult)
        ? cfg->momentum_sl_mult : cfg->sl_trail_mult;
    FPN_Binary<F> sl_offset = FPN_Mul(rolling->price_stddev, trail_mult);
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
// momentum trailing: tighter SL trail (failures reverse fast), wider TP trail (let runs extend)
// uses the same hold_score = SNR * R² pattern as MR but with different multipliers
// the key difference: momentum uses cfg->momentum_sl_mult (tighter) instead of cfg->sl_trail_mult
//======================================================================
// [END_FUNCTION]_[Momentum_ExitAdjust]
//======================================================================

#endif // MOMENTUM_HPP
