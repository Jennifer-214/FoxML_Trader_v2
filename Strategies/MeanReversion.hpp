// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[Strategies/MeanReversion.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[dip buyer — buy below avg (percentage or stddev offset mode, branchless select); P&L regression adapts the filters; idle squeeze catches runaway markets]
// [CONTAINS]
//   - [STRUCT]_[MeanReversionState]
//   - [FUNCTION]_[MeanReversion_Adapt]      (Init unblocked)
//   - [FUNCTION]_[MeanReversion_BuySignal]
//   - [FUNCTION]_[MeanReversion_ExitAdjust] (+ ExitAdjustSharded ratchet twin)
//======================================================================================================
// buys dips below the rolling average, sells at fixed TP/SL per position
// adaptive filters (entry offset and volume multiplier) are adjusted by P&L
// regression:
//   positive P&L slope -> loosen filters (buy more aggressively)
//   negative P&L slope -> tighten filters (buy more defensively)
//
// two offset modes (selected by config, branchless mask-select):
//   percentage mode: buy_price = avg - (avg * offset_pct)        [default]
//   stddev mode:     buy_price = avg - (stddev * offset_mult)
//   [volatility-aware]
//
// idle squeeze: when portfolio is empty and price runs away from buy gate, the
// filters squeeze toward their minimum so the gate catches up to the market.
// prevents the strategy from sitting idle forever after a breakout.
//
// multi-timeframe gate: optionally requires the long-window (512 tick) price
// slope to be non-negative before allowing buys. prevents buying short-term
// dips inside broader crashes.
//
// all adaptation is branchless on the hot path. the strategy only runs on the
// slow path (every poll_interval ticks). BuyGate on the hot path just reads the
// conditions.
//======================================================================================================
#ifndef MEAN_REVERSION_HPP
#define MEAN_REVERSION_HPP

#include <cstdint>  // v5.15.5.F.4d TECH_DEBT-083 close — explicit IWYU for uintN_t (was transitively pulled)
#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/OrderGates.hpp"
#include "../CoreFrameworks/Portfolio.hpp"
#include "../MemHeaders/OmsStateFlagRegistry.hpp"  // v5.15.5.C.2 (S3a) — MASK_OMS_STATE_*
#include "../ML_Headers/LinearRegression3X.hpp"
#include "../ML_Headers/ROR_regressor.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "StrategyInterface.hpp"

//======================================================================
// [STRUCT]_[MeanReversionState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[P&L + price regression feeders, the three adaptive live_* filters, initial-conds anchor]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F> struct MeanReversionState {
  RegressionFeederX<F> feeder; // P&L regression ring buffer
  RORRegressor<F> ror;         // slope-of-slopes (second derivative of P&L)
  FPN_Binary<F>
      live_offset_pct; // adaptive entry offset (between offset_min..offset_max)
  FPN_Binary<F> live_vol_mult;    // adaptive volume multiplier (between
                           // vol_mult_min..vol_mult_max)
  FPN_Binary<F> live_stddev_mult; // adaptive stddev multiplier (between
                           // offset_stddev_min..max)
  BuySideGateConditions<F>
      buy_conds_initial; // anchor for max_shift clamp, tracks rolling avg
  LinearRegression3XResult<F>
      last_regression; // stored for BuySignal to apply gate shift
  int has_regression;  // 1 if last_regression is valid, 0 otherwise
  RegressionFeederX<F>
      price_feeder; // price regression for trailing TP R² computation
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
// [SIZE]_[720B]
// [ALIGN]_[16]
// [CACHE_LINES]_[12]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[MeanReversionState]
//======================================================================

//------------------------------------------------------------------------------
// INIT
//======================================================================================================
// called once at warmup completion. computes initial buy conditions from
// rolling stats and resets the regression feeder for P&L tracking in the active
// phase. handles both percentage and stddev offset modes via branchless select.
//======================================================================================================
template <unsigned F>
inline void MeanReversion_Init(MeanReversionState<F> *state,
                               const RollingStats<F> *rolling,
                               BuySideGateConditions<F> *buy_conds) {
  // compute initial buy price in both modes, select based on which is active
  int use_stddev = !FPN_IsZero(state->live_stddev_mult);

  FPN_Binary<F> pct_price = RollingStats_BuyPrice(rolling, state->live_offset_pct);
  FPN_Binary<F> stddev_offset =
      FPN_Mul(rolling->price_stddev, state->live_stddev_mult);
  FPN_Binary<F> stddev_price = FPN_Sub(rolling->price_avg, stddev_offset);

  // branchless select: buy_price = use_stddev ? stddev_price : pct_price (16B → blend the whole .v)
  unsigned __int128 sm = -(unsigned __int128)(unsigned)use_stddev;
  FPN_Binary<F> buy_price { (__int128)(((unsigned __int128)stddev_price.v & sm) | ((unsigned __int128)pct_price.v & ~sm)) };

  // volume gate uses rolling avg * multiplier so only significant trades pass
  FPN_Binary<F> buy_vol = FPN_Mul(rolling->volume_avg, state->live_vol_mult);

  buy_conds->price = buy_price;
  buy_conds->volume = buy_vol;
  state->buy_conds_initial = *buy_conds;

  // reset feeder/ROR for P&L tracking in active phase
  state->feeder = RegressionFeederX_Init<F>();
  state->ror = RORRegressor_Init<F>();
  state->has_regression = 0;
}

//======================================================================
// [FUNCTION]_[MeanReversion_Adapt]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH]]
// [REFERENCE]_[INVARIANT]_[H20]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[idle squeeze + P&L regression -> the active offset mode's filter only (inactive mode frozen); branchless mask conditioning]
//======================================================================
// [CODE]
//======================================================================
// called every slow-path tick. three responsibilities:
//   1. idle squeeze - loosen filters when portfolio is empty and price is
//   running away
//   2. feeder push - feed P&L into regression buffer
//   3. regression + filter adjustment - adapt offset/vol_mult based on P&L
//   trend
//
// offset mode conditioning: only the ACTIVE mode's offset (percentage or
// stddev) is adapted. the inactive mode's value stays frozen at its init/reload
// value. this prevents idle squeeze and regression from drifting the inactive
// value (e.g. squeezing stddev_mult from 0 to offset_stddev_min while in
// percentage mode).
//======================================================================================================
template <unsigned F>
inline void MeanReversion_Adapt(MeanReversionState<F> *state,
                                FPN_Binary<F> current_price, FPN_Binary<F> portfolio_delta,
                                uint16_t active_bitmap,
                                const BuySideGateConditions<F> *buy_conds,
                                const ControllerConfig<F> *cfg) {
  // mode detection: stddev if offset_stddev_mult > 0 in config
  int use_stddev = !FPN_IsZero(cfg->offset_stddev_mult);

  //==================================================================================================
  // IDLE SQUEEZE: when portfolio is empty, use price slope to loosen/tighten
  // filters solves the chicken-and-egg problem: no positions -> no P&L -> no
  // adaptation if price is trending up and we have nothing, we're missing the
  // move -> squeeze offset down if price is trending down and we have nothing,
  // we're correctly staying out -> no change
  //
  // branchless: empty_mask is all 1s when portfolio empty, all 0s when holding
  // positive price slope -> squeeze offset toward offset_min
  // the squeeze rate is proportional to the slope magnitude
  //
  // mode-conditional: only squeeze the active mode's offset value
  //==================================================================================================
  {
    int is_empty = (active_bitmap == 0);
    int trailing = FPN_GreaterThan(current_price, buy_conds->price);

    // squeeze fires when: portfolio empty AND current price above buy gate
    int should_squeeze = is_empty & trailing;
    unsigned __int128 sq_mask = -(unsigned __int128)(unsigned)should_squeeze;

    // mode-conditional squeeze masks (16B → 128-bit)
    unsigned __int128 pct_sq_mask = sq_mask & -(unsigned __int128)(unsigned)(!use_stddev);
    unsigned __int128 stddev_sq_mask = sq_mask & -(unsigned __int128)(unsigned)use_stddev;

    // PERCENTAGE MODE: squeeze offset toward zero (not offset_min — go all the
    // way)
    FPN_Binary<F> zero = FPN_Zero<F>();
    FPN_Binary<F> squeeze_step = FPN_Mul(state->live_offset_pct, cfg->squeeze_decay);
    FPN_Binary<F> masked_squeeze { (__int128)((unsigned __int128)squeeze_step.v & pct_sq_mask) };  // pct-mode mask (16B .v)

    state->live_offset_pct = FPN_SubSat(state->live_offset_pct, masked_squeeze);
    state->live_offset_pct =
        FPN_Max(state->live_offset_pct, zero); // floor at zero, not offset_min

    // STDDEV MODE: squeeze stddev_mult toward offset_stddev_min
    // uses gap-based decay: step = (current - min) * 0.10
    FPN_Binary<F> stddev_gap =
        FPN_Sub(state->live_stddev_mult, cfg->offset_stddev_min);
    FPN_Binary<F> stddev_step = FPN_Mul(stddev_gap, cfg->squeeze_decay);
    FPN_Binary<F> masked_stddev { (__int128)((unsigned __int128)stddev_step.v & stddev_sq_mask) };  // stddev-mode mask (16B .v)

    state->live_stddev_mult =
        FPN_SubSat(state->live_stddev_mult, masked_stddev);
    state->live_stddev_mult =
        FPN_Max(state->live_stddev_mult, cfg->offset_stddev_min);

    // squeeze volume multiplier toward 1.0 (accept any trade size) — both modes
    FPN_Binary<F> one = FPN_FromDouble<F>(1.0);
    FPN_Binary<F> vmult_gap = FPN_Sub(state->live_vol_mult, one);
    FPN_Binary<F> vmult_step = FPN_Mul(vmult_gap, cfg->squeeze_decay);
    FPN_Binary<F> masked_vmult { (__int128)((unsigned __int128)vmult_step.v & sq_mask) };  // squeeze mask (16B .v)

    state->live_vol_mult = FPN_SubSat(state->live_vol_mult, masked_vmult);
    state->live_vol_mult = FPN_Max(state->live_vol_mult, one); // floor at 1.0x
  }

  //==================================================================================================
  // FEEDER PUSH: feed P&L and price into regression buffers
  // price feeder is used by ExitAdjust for trailing TP R² — pushed here so R²
  // uses the most recent market data when the trailing decision runs after
  // Adapt
  //==================================================================================================
  RegressionFeederX_Push(&state->feeder, portfolio_delta);
  RegressionFeederX_Push(&state->price_feeder, current_price);

  //==================================================================================================
  // REGRESSION + ADAPTIVE FILTER ADJUSTMENT
  // positive slope (making money) -> loosen filters (smaller offset, lower vol
  // mult) negative slope (losing money) -> tighten filters (larger offset,
  // higher vol mult)
  //
  // the shift direction is INVERTED from buy price adjustment:
  // - buy price: positive P&L -> shift price UP (buy more aggressively)
  // - offset: positive P&L -> shift offset DOWN (require less dip)
  // - vol mult: positive P&L -> shift mult DOWN (accept smaller trades)
  //
  // mode-conditional: offset_pct and stddev_mult are adapted independently,
  // only the active mode's value changes. prevents drift of inactive values.
  //
  // all branchless: same R^2 confidence mask, same clamp pattern
  //==================================================================================================
  state->has_regression = 0;
  if (state->feeder.count >= MAX_WINDOW) {
    LinearRegression3XResult<F> inner =
        RegressionFeederX_Compute(&state->feeder);
    RORRegressor_Push(&state->ror, inner);
    state->last_regression = inner;
    state->has_regression = 1;

    int confident = FPN_GreaterThanOrEqual(inner.r_squared, cfg->r2_threshold);

    // compute filter shift from slope: negate because positive P&L -> loosen
    // (decrease)
    FPN_Binary<F> filter_shift = FPN_Mul(inner.model.slope, cfg->filter_scale);
    FPN_Binary<F> neg_shift = FPN_Negate(filter_shift);

    // PERCENTAGE MODE: apply shift to offset_pct, scale 0.001, clamp
    // [offset_min, offset_max]
    {
      FPN_Binary<F> masked_pct_shift { (__int128)((unsigned __int128)neg_shift.v & -(unsigned __int128)(unsigned)(confident & !use_stddev)) };

      FPN_Binary<F> offset_scale = cfg->offset_adapt_scale;
      FPN_Binary<F> offset_shift = FPN_Mul(masked_pct_shift, offset_scale);
      state->live_offset_pct = FPN_AddSat(state->live_offset_pct, offset_shift);
      state->live_offset_pct = FPN_Max(state->live_offset_pct, Money_ToBinary(cfg->offset_min));
      state->live_offset_pct = FPN_Min(state->live_offset_pct, Money_ToBinary(cfg->offset_max));
    }

    // STDDEV MODE: apply shift to stddev_mult, scale 0.1, clamp [stddev_min,
    // stddev_max] scale is 100x larger than percentage mode because stddev_mult
    // ranges 0.5-4.0 while offset_pct ranges 0.0005-0.005 (~1000x difference in
    // magnitude)
    {
      FPN_Binary<F> masked_stddev_shift { (__int128)((unsigned __int128)neg_shift.v & -(unsigned __int128)(unsigned)(confident & use_stddev)) };

      FPN_Binary<F> stddev_adapt_scale = cfg->stddev_adapt_scale;
      FPN_Binary<F> stddev_shift = FPN_Mul(masked_stddev_shift, stddev_adapt_scale);
      state->live_stddev_mult =
          FPN_AddSat(state->live_stddev_mult, stddev_shift);
      state->live_stddev_mult =
          FPN_Max(state->live_stddev_mult, cfg->offset_stddev_min);
      state->live_stddev_mult =
          FPN_Min(state->live_stddev_mult, cfg->offset_stddev_max);
    }

    // apply shift to volume multiplier and clamp to [vol_mult_min,
    // vol_mult_max] — both modes
    FPN_Binary<F> masked_shift { (__int128)((unsigned __int128)neg_shift.v & -(unsigned __int128)(unsigned)confident) };

    FPN_Binary<F> vol_shift = FPN_Mul(masked_shift, cfg->vol_adapt_scale);
    state->live_vol_mult = FPN_AddSat(state->live_vol_mult, vol_shift);
    state->live_vol_mult = FPN_Max(state->live_vol_mult, cfg->vol_mult_min);
    state->live_vol_mult = FPN_Min(state->live_vol_mult, cfg->vol_mult_max);
  }
}

//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[MeanReversion_Adapt]
//======================================================================

//======================================================================
// [FUNCTION]_[MeanReversion_BuySignal]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH]]
// [REFERENCE]_[INVARIANT]_[H20]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[dip gate from adaptive filters + regression shift (max_shift clamped) + multi-timeframe long-trend veto]
//======================================================================
// [CODE]
//======================================================================
// called every slow-path tick after Adapt. computes buy gate conditions from
// rolling stats using the current adaptive filter values, then applies
// regression-based gate shift if available. optionally gates on long-window
// trend for multi-timeframe confirmation. returns the conditions for the engine
// to write to ctrl->buy_conds.
//======================================================================================================
template <unsigned F>
inline BuySideGateConditions<F> MeanReversion_BuySignal(
    MeanReversionState<F> *state, const RollingStats<F> *rolling,
    const RollingStats<F, 512> *rolling_long, const ControllerConfig<F> *cfg,
    FPN_Binary<F> ema_price = FPN_Zero<F>()) {
  BuySideGateConditions<F> conds;

  // base average: EMA when provided (nonzero), rolling avg otherwise
  // no branch — caller passes rolling_avg when EMA is disabled
  FPN_Binary<F> base_avg = FPN_IsZero(ema_price) ? rolling->price_avg : ema_price;

  // compute base buy price in both modes, branchless select the active one
  // percentage mode: buy_price = base - (base * offset_pct)
  // stddev mode:     buy_price = base - (stddev * offset_mult) — scales with
  // volatility
  int use_stddev = !FPN_IsZero(cfg->offset_stddev_mult);

  FPN_Binary<F> pct_offset = FPN_Mul(base_avg, state->live_offset_pct);
  FPN_Binary<F> pct_price = FPN_Sub(base_avg, pct_offset);
  FPN_Binary<F> stddev_offset =
      FPN_Mul(rolling->price_stddev, state->live_stddev_mult);
  FPN_Binary<F> stddev_price = FPN_Sub(base_avg, stddev_offset);

  // branchless select: use_stddev ? stddev_price : pct_price (16B → blend the whole .v)
  unsigned __int128 sm = -(unsigned __int128)(unsigned)use_stddev;
  conds.price = { (__int128)(((unsigned __int128)stddev_price.v & sm) | ((unsigned __int128)pct_price.v & ~sm)) };

  conds.volume = FPN_Mul(rolling->volume_avg, state->live_vol_mult);

  // update initial conditions to track the market - prevents the gate shift
  // clamp from anchoring to stale warmup prices. the clamp window (max_shift)
  // moves with the rolling average so the gate stays in the current price
  // neighborhood
  state->buy_conds_initial = conds;

  // apply regression-based gate shift if available
  // shifts buy price condition based on regression slope, masked by R^2
  // confidence, clamped to max_shift from initial conditions - all branchless
  // works identically in both offset modes — it's an absolute price shift
  if (state->has_regression) {
    int confident = FPN_GreaterThanOrEqual(state->last_regression.r_squared,
                                           cfg->r2_threshold);

    FPN_Binary<F> shift =
        FPN_Mul(state->last_regression.model.slope, cfg->slope_scale_buy);

    // clamp shift magnitude to max_shift (percentage of rolling avg,
    // price-independent)
    FPN_Binary<F> max_shift_abs = FPN_Mul(rolling->price_avg, cfg->max_shift);
    shift = FPN_Min(shift, max_shift_abs);
    shift = FPN_Max(shift, FPN_Negate(max_shift_abs));

    // mask shift to zero if not confident - word-level branchless mask
    // branchless: masked_shift = confident ? shift : 0 (16B → mask the whole .v)
    unsigned __int128 conf_mask = -(unsigned __int128)(unsigned)confident;
    FPN_Binary<F> masked_shift { (__int128)((unsigned __int128)shift.v & conf_mask) };

    // apply shift
    FPN_Binary<F> new_price = FPN_AddSat(conds.price, masked_shift);

    // clamp to initial +/- max_shift (percentage-based, scales with price)
    FPN_Binary<F> upper = FPN_AddSat(state->buy_conds_initial.price, max_shift_abs);
    FPN_Binary<F> lower = FPN_SubSat(state->buy_conds_initial.price, max_shift_abs);
    new_price = FPN_Max(new_price, lower);
    new_price = FPN_Min(new_price, upper);

    conds.price = new_price;
  }

  //==================================================================================================
  // MULTI-TIMEFRAME GATE: require long-window trend to be flat or rising
  // uses relative slope (slope / price_avg) so the threshold is
  // price-independent: the same config value works whether the asset is $0.50
  // or $70,000 when disabled (min_long_slope = 0), gate always passes
  //==================================================================================================
  {
    int long_enabled = !FPN_IsZero(cfg->min_long_slope);
    // normalize slope by price: relative_slope = slope / price_avg
    // (dimensionless fraction)
    FPN_Binary<F> relative_long_slope =
        FPN_IsZero(rolling_long->price_avg)
            ? FPN_Zero<F>()
            : FPN_DivNoAssert(rolling_long->price_slope,
                              rolling_long->price_avg);
    int long_pass =
        FPN_GreaterThanOrEqual(relative_long_slope, cfg->min_long_slope);
    int long_ok = long_pass | !long_enabled;
    Gate_Zero(&conds, long_ok);
  }

  //==================================================================================================
  // VOLUME DELTA GATE: block MR buys when heavy selling pressure (falling
  // knife) volume_delta = (buy_vol - sell_vol) / total_vol, range [-1.0, +1.0]
  // when delta is deeply negative (heavy selling), MR dip buys are catching a
  // falling knife when disabled (min_buy_delta = 0), gate always passes
  //==================================================================================================
  {
    int delta_enabled = !FPN_IsZero(cfg->min_buy_delta);
    int delta_pass =
        FPN_GreaterThanOrEqual(rolling->volume_delta, cfg->min_buy_delta);
    int delta_ok = delta_pass | !delta_enabled;
    Gate_Zero(&conds, delta_ok);
  }

  //==================================================================================================
  // VWAP GATE: block buys when price is above VWAP (buying at a premium)
  // only buy at or below VWAP - vwap_offset (discount to volume-weighted avg)
  // when disabled (vwap_offset = 0), gate always passes
  //==================================================================================================
  {
    int vwap_enabled = !FPN_IsZero(cfg->vwap_offset);
    // vwap_deviation is (price - vwap) / vwap — negative means below VWAP
    // pass when deviation <= -vwap_offset (price is sufficiently below VWAP)
    FPN_Binary<F> neg_offset = FPN_Negate(cfg->vwap_offset);
    int vwap_pass =
        FPN_LessThanOrEqual(rolling->vwap_deviation, neg_offset);
    int vwap_ok = vwap_pass | !vwap_enabled;
    Gate_Zero(&conds, vwap_ok);
  }

  return conds;
}

//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[MeanReversion_BuySignal]
//======================================================================

//======================================================================
// [FUNCTION]_[MeanReversion_ExitAdjust]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[hold_score = SNR x R2 (needs BOTH magnitude and consistency) gates the TP/SL trail; ExitAdjustSharded (ratchet_sl twin — the F4 dead-write fix) shares the section]
//======================================================================
// [CODE]
//======================================================================
// called every slow-path tick after Adapt. adjusts TP/SL for positions that are
// "running" (price above original TP) based on trend strength and consistency.
//
// hold_score = SNR * R² where:
//   SNR = price_slope / price_stddev (signal-to-noise: is the trend big
//   relative to noise?) R² = price regression R² (consistency: is the trend
//   reliable?)
// the product requires BOTH magnitude AND consistency — choppy whipsaws (high
// SNR, low R²) and slow bleeds (low SNR, high R²) both produce low scores.
//
// when hold_score >= threshold: ratchet TP and SL upward (trailing)
// when hold_score drops below: stop ratcheting, exit gate catches pullback at
// last raised TP
//======================================================================================================
template <unsigned F>
inline void MeanReversion_ExitAdjust(Portfolio<F> *portfolio,
                                     Money current_price,
                                     const RollingStats<F> *rolling,
                                     MeanReversionState<F> *state,
                                     const ControllerConfig<F> *cfg) {
  // skip if trailing disabled
  if (FPN_IsZero(cfg->tp_hold_score))
    return;

  // guard: flat market (stddev = 0) would make SNR = MAX via DivNoAssert
  // saturation which falsely activates trailing. skip when no volatility data.
  if (FPN_IsZero(rolling->price_stddev))
    return;

  // compute R² and slope from price regression (8-sample window, ~4 min)
  // uses the SHORT regression window, not the 128-tick rolling window (~70
  // min), so trailing reacts to what's happening NOW, not historical trend cold
  // start: R² = 0 for first 8 slow-path cycles after warmup. trailing disabled
  // until then.
  FPN_Binary<F> r_squared = FPN_Zero<F>();
  FPN_Binary<F> reg_slope = FPN_Zero<F>();
  if (state->price_feeder.count >= MAX_WINDOW) {
    LinearRegression3XResult<F> price_reg =
        RegressionFeederX_Compute(&state->price_feeder);
    r_squared = price_reg.r_squared;
    reg_slope = price_reg.model.slope;
  }

  // compute SNR using regression slope (not rolling slope)
  // regression slope = recent ~4 min trend. rolling slope = ~70 min average.
  // for trailing decisions, recent trend is what matters.
  FPN_Binary<F> snr = FPN_DivNoAssert(reg_slope, rolling->price_stddev);

  // hold_score = SNR * R²
  // negative slope → negative SNR → negative score → never exceeds positive
  // threshold → correct
  FPN_Binary<F> hold_score = FPN_Mul(snr, r_squared);

  int should_trail = FPN_GreaterThanOrEqual(hold_score, cfg->tp_hold_score);

  uint16_t active = portfolio->active_bitmap;
  while (active) {
    int idx = __builtin_ctz(active);
    Position<F> *pos = &portfolio->positions[idx];

    // only trail positions that are above their original TP (position is
    // "running")
    int above_tp = Money_Gt(current_price, pos->original_tp);

    if (above_tp & should_trail) {
      // trailing TP: current_price - (stddev * trail_mult)
      // ratchet up only — FPN_Max ensures TP never decreases, locking in gains
      FPN_Binary<F> tp_offset = FPN_Mul(rolling->price_stddev, cfg->tp_trail_mult);
      FPN_Binary<F> trailing_tp = FPN_Sub(current_price, tp_offset);
      pos->take_profit_price = FPN_Max(pos->take_profit_price, trailing_tp);

      // trailing SL: lock in gains alongside TP
      // ratchet up only — prevents "let winners turn into losers"
      FPN_Binary<F> sl_offset = FPN_Mul(rolling->price_stddev, cfg->sl_trail_mult);
      FPN_Binary<F> trailing_sl = FPN_Sub(current_price, sl_offset);
      pos->stop_loss_price = FPN_Max(pos->stop_loss_price, trailing_sl);

      // SL floor: enforce 2:1 min reward/risk after trailing adjustments
      // only applies when SL is still below entry (at-risk position).
      // once SL trails above entry, the position is a guaranteed win — no floor needed
      if (FPN_LessThan(pos->stop_loss_price, pos->entry_price)) {
        FPN_Binary<F> tp_dist = FPN_Sub(pos->take_profit_price, pos->entry_price);
        FPN_Binary<F> min_sl_dist = FPN_Mul(tp_dist, FPN_FromDouble<F>(0.5));
        FPN_Binary<F> sl_floor = FPN_SubSat(pos->entry_price, min_sl_dist);
        pos->stop_loss_price = FPN_Min(pos->stop_loss_price, sl_floor);
      }
    }

    active &= active - 1;
  }
}

//------------------------------------------------------------------------------
// EXIT ADJUST — sharded, ratchet_sl path
//------------------------------------------------------------------------------
// v5.4.0 Phase 2.2: sharded equivalent of MeanReversion_ExitAdjust above.
// The legacy version writes pos->stop_loss_price + pos->take_profit_price,
// neither of which the sharded hot path reads (postmortem F4 — dead writes,
// hot path reads core->live_sl + cached_params.ratchet_sl).
//
// Differences vs legacy:
//   - Writes ratchet_sl (max-only) via Strategy_WriteRatchetSL helper which
//     applies the v5.1.7 fee-floor cap (entry × (1 - 3 × fee_rate_taker)).
//   - Per-core scope: iterates this core's slot(s) only, not the full
//     portfolio bitmap.
//   - TP trailing deferred to Phase 3 (parallel TP-ratchet channel — sharded
//     hot path uses cached_params.tp_pct via per-fill compute, no TP ratchet
//     field exists yet).
//   - Same regression-based hold_score = (reg_slope / stddev) × R²:
//     trail only when confidence is high.
//
// Forward declaration of Strategy_WriteRatchetSL — defined in
// StrategyLifecycle.hpp which can't be included here (cycle).
//======================================================================================================
namespace tt {
template <unsigned F> struct EventLoopState;
template <unsigned F>
bool Strategy_WriteRatchetSL(EventLoopState<F>* state, int slot,
                              FPN_Binary<F> proposed_sl, FPN_Binary<F> entry_price,
                              const ControllerConfig<F>* cfg);

template <unsigned F, unsigned W>
inline void MeanReversion_ExitAdjustSharded(
    EventLoopState<F>* state,
    int slot,
    MeanReversionState<F>* mr,
    Money current_price,
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* cfg
) {
    if (FPN_IsZero(cfg->tp_hold_score))   return;
    if (FPN_IsZero(rolling->price_stddev)) return;

    // Compute regression-based hold_score from MR state's price feeder.
    // MR_Adapt pushes price_feeder this same cycle; Adapt runs before this
    // function, so we read fresh data.
    FPN_Binary<F> r_squared = FPN_Zero<F>();
    FPN_Binary<F> reg_slope = FPN_Zero<F>();
    if (mr->price_feeder.count >= MAX_WINDOW) {
        LinearRegression3XResult<F> price_reg =
            RegressionFeederX_Compute(&mr->price_feeder);
        r_squared = price_reg.r_squared;
        reg_slope = price_reg.model.slope;
    }
    FPN_Binary<F> snr        = FPN_DivNoAssert(reg_slope, rolling->price_stddev);
    FPN_Binary<F> hold_score = FPN_Mul(snr, r_squared);
    if (!FPN_GreaterThanOrEqual(hold_score, cfg->tp_hold_score)) return;

    // For each of this core's active slots, propose a trailing SL and
    // route through Strategy_WriteRatchetSL (fee-floor capped, max-only).
    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    int partial_on = BITMAP_IS_SET(state->oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
    uint16_t my_mask = partial_on
        ? (uint16_t)((1u << (slot * 2)) | (1u << (slot * 2 + 1)))
        : (uint16_t)(1u << slot);
    uint16_t bm = (uint16_t)(state->oms->portfolio.active_bitmap & my_mask);

    FPN_Binary<F> sl_offset = FPN_Mul(rolling->price_stddev, cfg->sl_trail_mult);
    Money trailing_sl = Money_Sub(current_price, Money_FromBinary(sl_offset));

    while (bm) {
        int pidx = __builtin_ctz(bm);
        bm &= (uint16_t)(bm - 1);
        Money entry = state->oms->portfolio.positions[pidx].entry_price;
        // Only trail when position is "running" (price above original_tp)
        // and entry is set (idle slot guard).
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
// [END_FUNCTION]_[MeanReversion_ExitAdjust]
//======================================================================

#endif // MEAN_REVERSION_HPP
