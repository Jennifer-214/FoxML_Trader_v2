// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [PORTFOLIO CONTROLLER]
//======================================================================================================
// this just tracks the portfolio delta and tracks performance over time, uses
// linear regression and the GCN to edit and update the gate conditions for
// buying and selling based on portoflio performance, probably gonna add
// configrurebale parameters for polling rates and stuff, this should be a
// seperate module from the actual order engine, as it shouldnt interfere with
// the order execution, this simply pipes conditions to the gates
//======================================================================================================
// [INCLUDE]
//======================================================================================================
#ifndef PORTFOLIO_CONTROLLER_HPP
#define PORTFOLIO_CONTROLLER_HPP

#include "ControllerConfig.hpp"
#include "OrderGates.hpp"
#include "Portfolio.hpp"
#include "../DataStream/TradeLog.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../Strategies/MeanReversion.hpp"
#include "../Strategies/Momentum.hpp"
#include "../Strategies/RegimeDetector.hpp"
#include <stdio.h>
//======================================================================================================
// [CONTROLLER STRUCT]
//======================================================================================================
#define CONTROLLER_WARMUP 0
#define CONTROLLER_ACTIVE 1
//======================================================================================================
template <unsigned F> struct PortfolioController {
  //================================================================================================
  // HOT — touched every tick, grouped for L1 cache locality
  // target: all hot fields within first ~3KB so they share cache lines
  //================================================================================================
  Portfolio<F> portfolio;
  uint64_t prev_bitmap;   // fill detection: pool->bitmap & ~prev_bitmap
  uint64_t tick_count;    // slow-path gate: tick_count < config.poll_interval
  uint64_t last_slow_time; // wall-time floor: run slow path if this many seconds elapsed
  uint64_t total_ticks;
  BuySideGateConditions<F> buy_conds;
  ExitBuffer<F> exit_buf;

  //================================================================================================
  // WARM — accessed on fills or slow path, but not every tick
  //================================================================================================
  ControllerConfig<F> config;
  FPN<F> portfolio_delta; // unrealized P&L (current open positions)
  FPN<F> realized_pnl;    // cumulative realized P&L (closed positions)
  FPN<F> balance;    // paper trading balance (deducted on buy, added on sell)
  FPN<F> total_fees; // cumulative fees paid (tracked for display)

  uint32_t wins;       // TP exits
  uint32_t losses;     // SL exits
  uint32_t total_buys; // total entries
  FPN<F> gross_wins;   // cumulative dollar gains from TP exits
  FPN<F>
      gross_losses; // cumulative dollar losses from SL exits (positive number)
  uint64_t
      total_hold_ticks;     // cumulative ticks held across all closed positions
  uint64_t entry_ticks[16]; // tick at which each position was entered (indexed
                            // by slot)
  time_t entry_time[16];    // wall clock time at entry (for hold duration display)
  uint8_t entry_strategy[16]; // which strategy_id entered each position (for regime adjustment)

  int state;
  FPN<F> price_sum;
  FPN<F> volume_sum;
  uint64_t warmup_count;

  int strategy_id;
  MeanReversionState<F> mean_rev;
  MomentumState<F> momentum;
  RegimeState<F> regime;

  RORRegressor<F> regime_ror;  // slope-of-slopes for trend acceleration detection
  FPN<F> volume_spike_ratio;   // current_volume / rolling.volume_max (spike detection)
  uint32_t sl_cooldown_counter; // remaining slow-path cycles before buy gate re-enables
  double session_high;         // highest price since startup
  double session_low;          // lowest price since startup
  uint32_t fills_rejected;     // total fills rejected since startup
  int last_reject_reason;      // 0=none, 1=spacing, 2=balance, 3=exposure, 4=breaker, 5=full, 6=dup
  TradeLogBuffer trade_buf;    // buffered trade log — hot path pushes, slow path drains

  //================================================================================================
  // COLD — slow path only, kept at end to avoid polluting hot cache lines
  //================================================================================================
  RollingStats<F> rolling;
  RollingStats<F, 512> *rolling_long;  // heap-allocated (24KB), slow path only
};
//======================================================================================================
// [INIT]
//======================================================================================================
template <unsigned F>
inline void PortfolioController_Init(PortfolioController<F> *ctrl,
                                     ControllerConfig<F> config) {
  Portfolio_Init(&ctrl->portfolio);
  ctrl->portfolio_delta = FPN_Zero<F>();
  ctrl->realized_pnl = FPN_Zero<F>();
  ctrl->balance = config.starting_balance;
  ctrl->total_fees = FPN_Zero<F>();
  ctrl->wins = 0;
  ctrl->losses = 0;
  ctrl->total_buys = 0;
  ctrl->gross_wins = FPN_Zero<F>();
  ctrl->gross_losses = FPN_Zero<F>();
  ctrl->total_hold_ticks = 0;
  for (int i = 0; i < 16; i++) {
    ctrl->entry_ticks[i] = 0;
    ctrl->entry_time[i] = 0;
    ctrl->entry_strategy[i] = STRATEGY_MEAN_REVERSION;
  }

  ctrl->rolling = RollingStats_Init<F>();

  // during warmup, buy gate is disabled - price 0 means nothing passes
  // LessThanOrEqual
  ctrl->buy_conds.price = FPN_Zero<F>();
  ctrl->buy_conds.volume = FPN_Zero<F>();
  ctrl->buy_conds.gate_direction = 0;  // default: buy below (mean reversion)

  // init strategy states — both ready at startup so regime switch is instant
  ctrl->strategy_id = STRATEGY_MEAN_REVERSION;
  // mean reversion
  ctrl->mean_rev.feeder = RegressionFeederX_Init<F>();
  ctrl->mean_rev.price_feeder = RegressionFeederX_Init<F>();
  ctrl->mean_rev.ror = RORRegressor_Init<F>();
  ctrl->mean_rev.live_offset_pct = config.entry_offset_pct;
  ctrl->mean_rev.live_vol_mult = config.volume_multiplier;
  ctrl->mean_rev.live_stddev_mult = config.offset_stddev_mult;
  ctrl->mean_rev.buy_conds_initial = ctrl->buy_conds;
  ctrl->mean_rev.has_regression = 0;
  // momentum
  ctrl->momentum.feeder = RegressionFeederX_Init<F>();
  ctrl->momentum.price_feeder = RegressionFeederX_Init<F>();
  ctrl->momentum.ror = RORRegressor_Init<F>();
  ctrl->momentum.live_breakout_mult = config.momentum_breakout_mult;
  ctrl->momentum.live_vol_mult = config.volume_multiplier;
  ctrl->momentum.buy_conds_initial = ctrl->buy_conds;
  ctrl->momentum.has_regression = 0;
  // regime detector
  Regime_Init(&ctrl->regime, config.regime_hysteresis);
  ctrl->regime_ror = RORRegressor_Init<F>();
  ctrl->volume_spike_ratio = FPN_Zero<F>();
  ctrl->sl_cooldown_counter = 0;
  ctrl->session_high = 0.0;
  ctrl->session_low = 0.0;
  ctrl->fills_rejected = 0;
  ctrl->last_reject_reason = 0;

  ExitBuffer_Init(&ctrl->exit_buf);
  TradeLogBuffer_Init(&ctrl->trade_buf);

  ctrl->prev_bitmap = 0;
  ctrl->tick_count = 0;
  ctrl->last_slow_time = (uint64_t)time(NULL);
  ctrl->total_ticks = 0;

  ctrl->state = CONTROLLER_WARMUP;
  ctrl->price_sum = FPN_Zero<F>();
  ctrl->volume_sum = FPN_Zero<F>();
  ctrl->warmup_count = 0;

  ctrl->config = config;

  // heap-allocate rolling_long (24KB) — keeps it out of the hot struct
  if (ctrl->rolling_long) free(ctrl->rolling_long);  // safe on reinit (24h reconnect)
  ctrl->rolling_long = (RollingStats<F, 512>*)malloc(sizeof(RollingStats<F, 512>));
  if (!ctrl->rolling_long) {
    fprintf(stderr, "[FATAL] malloc failed for rolling_long (24KB)\n");
    return;
  }
  *ctrl->rolling_long = RollingStats_Init<F, 512>();
}
//======================================================================================================
// [EXIT BUFFER DRAIN]
//======================================================================================================
// processes TP/SL exits: books P&L, updates balance, logs trades, triggers cooldown
// called from both warmup (loaded positions can exit) and slow path
//======================================================================================================
template <unsigned F>
inline void PortfolioController_DrainExits(PortfolioController<F> *ctrl) {
  for (uint32_t i = 0; i < ctrl->exit_buf.count; i++) {
    ExitRecord<F> *rec = &ctrl->exit_buf.records[i];
    if (rec->position_index >= 16) continue; // bounds guard
    Position<F> *pos = &ctrl->portfolio.positions[rec->position_index];

    // slippage: simulate worse fill on exit (sell at lower price than market)
    FPN<F> exit_price = rec->exit_price;
    if (!FPN_IsZero(ctrl->config.slippage_pct)) {
        FPN<F> slip = FPN_Mul(exit_price, ctrl->config.slippage_pct);
        exit_price = FPN_SubSat(exit_price, slip);
    }

    double entry_d = FPN_ToDouble(pos->entry_price);
    double exit_d = FPN_ToDouble(exit_price);
    double qty_d = FPN_ToDouble(pos->quantity);
    double delta_pct = 0.0;
    if (entry_d != 0.0)
      delta_pct = ((exit_d - entry_d) / entry_d) * 100.0;

    FPN<F> gross_proceeds = FPN_Mul(exit_price, pos->quantity);
    FPN<F> exit_fee = FPN_Mul(gross_proceeds, ctrl->config.fee_rate);
    FPN<F> net_proceeds = FPN_SubSat(gross_proceeds, exit_fee);

    FPN<F> entry_cost = FPN_Mul(pos->entry_price, pos->quantity);
    FPN<F> entry_fee_recon = FPN_Mul(entry_cost, ctrl->config.fee_rate);
    FPN<F> total_entry_cost = FPN_AddSat(entry_cost, entry_fee_recon);
    FPN<F> pos_pnl = FPN_Sub(net_proceeds, total_entry_cost);
    ctrl->realized_pnl = FPN_AddSat(ctrl->realized_pnl, pos_pnl);

    ctrl->balance = FPN_AddSat(ctrl->balance, net_proceeds);
    ctrl->total_fees = FPN_AddSat(ctrl->total_fees, exit_fee);

    const char *reason = (rec->reason == 0) ? "TP" : "SL";
    ctrl->wins += (rec->reason == 0);
    ctrl->losses += (rec->reason == 1);
    if (rec->reason == 1 && ctrl->config.sl_cooldown_cycles > 0)
        ctrl->sl_cooldown_counter = ctrl->config.sl_cooldown_cycles;

    int is_win = !pos_pnl.sign & !FPN_IsZero(pos_pnl);
    int is_loss = pos_pnl.sign;
    {
      constexpr unsigned N2 = FPN<F>::N;
      uint64_t win_mask = -(uint64_t)is_win;
      uint64_t loss_mask = -(uint64_t)is_loss;
      FPN<F> neg_pnl = FPN_Negate(pos_pnl);
      FPN<F> win_add, loss_add;
      for (unsigned w = 0; w < N2; w++) {
        win_add.w[w] = pos_pnl.w[w] & win_mask;
        loss_add.w[w] = neg_pnl.w[w] & loss_mask;
      }
      win_add.sign = 0;
      loss_add.sign = 0;
      ctrl->gross_wins = FPN_AddSat(ctrl->gross_wins, win_add);
      ctrl->gross_losses = FPN_AddSat(ctrl->gross_losses, loss_add);
    }

    uint64_t entry_tick = ctrl->entry_ticks[rec->position_index];
    ctrl->total_hold_ticks +=
        (rec->tick > entry_tick) ? (rec->tick - entry_tick) : 0;
    TradeLogBuffer_PushSell(&ctrl->trade_buf, rec->tick, exit_d, qty_d, entry_d,
                            delta_pct, reason,
                            FPN_ToDouble(ctrl->balance), FPN_ToDouble(exit_fee),
                            ctrl->entry_strategy[rec->position_index],
                            ctrl->regime.current_regime);
  }
  ExitBuffer_Clear(&ctrl->exit_buf);
}
//======================================================================================================
// [TICK - MAIN CONTROLLER FUNCTION]
//======================================================================================================
// called every tick. fill consumption runs every tick (zero unprotected
// exposure). regression/adjustment runs every poll_interval ticks (slow path).
//======================================================================================================
template <unsigned F>
inline void PortfolioController_Tick(PortfolioController<F> *ctrl,
                                     OrderPool<F> *pool, FPN<F> current_price,
                                     FPN<F> current_volume,
                                     TradeLog *trade_log) {
  // always increment tick counter (branchless, single add)
  ctrl->total_ticks++;
  ctrl->tick_count++;

  //==================================================================================================
  // WARMUP PHASE
  //==================================================================================================
  if (__builtin_expect(ctrl->state == CONTROLLER_WARMUP, 0)) {
    ctrl->warmup_count++; // every tick — needed for warmup completion gate below

    // feed rolling stats + session tracking at slow-path rate during warmup
    // each push is one slow-path sample — spacing them out ensures each sample
    // represents a different point in time (real price diversity, not 128 copies
    // of the same second). tests use poll_interval=1 so every tick IS a sample.
    // time floor: also push if 3+ seconds have passed (low-volume periods)
    uint64_t warmup_now = (uint64_t)time(NULL);
    int warmup_time_floor = (warmup_now - ctrl->last_slow_time >= ctrl->config.slow_path_max_secs);
    if ((ctrl->warmup_count % ctrl->config.poll_interval == 0) || warmup_time_floor) {
      ctrl->last_slow_time = warmup_now;
      // session high/low tracking (display-only, slow-path granularity is sufficient)
      double pd = FPN_ToDouble(current_price);
      if (ctrl->session_high == 0.0) { ctrl->session_high = pd; ctrl->session_low = pd; }
      if (pd > ctrl->session_high) ctrl->session_high = pd;
      if (pd < ctrl->session_low) ctrl->session_low = pd;
      ctrl->price_sum = FPN_AddSat(ctrl->price_sum, current_price);
      ctrl->volume_sum = FPN_AddSat(ctrl->volume_sum, current_volume);
      RollingStats_Push(&ctrl->rolling, current_price, current_volume);
      RollingStats_Push(ctrl->rolling_long, current_price, current_volume);
      ctrl->tick_count = 0; // Reset for main.cpp slow-path detection
    }

    // gate on rolling stats having real data, not just raw tick count
    // requires warmup_ticks raw ticks AND rolling window having enough slow-path
    // samples for meaningful regression (stddev/R²/slope)
    // min_warmup_samples=0 skips the check (tests, backward compat)
    if (ctrl->warmup_count >= ctrl->config.warmup_ticks
        && ctrl->rolling.count >= (int)ctrl->config.min_warmup_samples) {
      MeanReversion_Init(&ctrl->mean_rev, &ctrl->rolling, &ctrl->buy_conds);
      Momentum_Init(&ctrl->momentum, &ctrl->rolling, &ctrl->buy_conds);
      ctrl->buy_conds = MeanReversion_BuySignal(&ctrl->mean_rev, &ctrl->rolling,
                                                 ctrl->rolling_long, &ctrl->config);
      ctrl->buy_conds.gate_direction = 0;
      ctrl->state = CONTROLLER_ACTIVE;
    }

    // drain exits during warmup — loaded positions need TP/SL processed
    // (exit gate runs every tick regardless of state, but drain is slow-path)
    if (ctrl->exit_buf.count > 0)
      PortfolioController_DrainExits(ctrl);

    return;
  }

  //==================================================================================================
  // ACTIVE PHASE - EVERY TICK: consume new fills immediately
  //==================================================================================================
  // branchless: mask new_fills to zero if portfolio is full, then the while
  // loop body count is 0 the loop itself is unavoidable (variable fill count)
  // but the outer gate is eliminated
  //==================================================================================================
  uint64_t new_fills = pool->bitmap & ~ctrl->prev_bitmap;
  uint64_t can_fill = -(uint64_t)(Portfolio_CountActive(
      &ctrl->portfolio) < (int)ctrl->config.max_positions); // all 1s if room, all 0s if at cap
  uint64_t fills = new_fills & can_fill;
  uint64_t consumed = fills; // track what we actually process for pool clearing

  while (fills) {
    uint32_t idx = __builtin_ctzll(fills);

    FPN<F> fill_price = pool->slots[idx].price;
    // slippage: simulate worse fill on entry (buy at higher price than market)
    if (!FPN_IsZero(ctrl->config.slippage_pct)) {
        FPN<F> slip = FPN_Mul(fill_price, ctrl->config.slippage_pct);
        fill_price = FPN_AddSat(fill_price, slip);
    }
    // ignore stream quantity - we use position sizing based on balance
    // fill_price is the signal, not fill_qty

    // consolidation: find existing position at same price
    // with position sizing, consolidation is DISABLED - each fill at the same
    // price would just be a duplicate signal. we only want one position per
    // price level. the entry spacing check below handles preventing duplicate
    // entries at nearby prices.
    int existing = Portfolio_FindByPrice(&ctrl->portfolio, fill_price);
    int found = (existing >= 0);

    // entry spacing check: reject fills that are too close to existing
    // positions walks the portfolio bitmap to find the minimum distance from
    // fill_price to any entry this spreads the 16 slots across actual price
    // levels instead of clustering
    FPN<F> min_spacing = RollingStats_EntrySpacing(
        &ctrl->rolling, ctrl->config.spacing_multiplier);

    // volume spike: reduce spacing requirement for high-conviction entries
    // a 5x+ volume spike on a dip is a stronger signal — allow tighter clustering
    // compute fresh ratio from current tick volume (not stale slow-path value)
    FPN<F> live_spike_ratio = (!FPN_IsZero(ctrl->rolling.volume_max))
        ? FPN_DivNoAssert(current_volume, ctrl->rolling.volume_max)
        : FPN_Zero<F>();
    ctrl->volume_spike_ratio = live_spike_ratio;  // update for TUI display
    int is_spike = FPN_GreaterThanOrEqual(live_spike_ratio,
                                           ctrl->config.spike_threshold);
    FPN<F> spike_spacing = FPN_Mul(min_spacing, ctrl->config.spike_spacing_reduction);
    uint64_t spike_mask = -(uint64_t)is_spike;
    for (unsigned w = 0; w < FPN<F>::N; w++) {
        min_spacing.w[w] = (spike_spacing.w[w] & spike_mask) |
                           (min_spacing.w[w] & ~spike_mask);
    }
    min_spacing.sign = (spike_spacing.sign & is_spike) |
                       (min_spacing.sign & !is_spike);

    int too_close = 0;
    {
      uint16_t active_pos = ctrl->portfolio.active_bitmap;
      while (active_pos) {
        int pidx = __builtin_ctz(active_pos);
        FPN<F> dist =
            FPN_Sub(fill_price, ctrl->portfolio.positions[pidx].entry_price);
        // branchless absolute value: mask-select between dist and negated dist
        // neg_mask is all 1s if negative, all 0s if positive
        FPN<F> neg_dist = FPN_Negate(dist);
        uint64_t neg_mask = -(uint64_t)(dist.sign);
        FPN<F> abs_dist;
        for (unsigned w = 0; w < FPN<F>::N; w++) {
          abs_dist.w[w] = (neg_dist.w[w] & neg_mask) | (dist.w[w] & ~neg_mask);
        }
        abs_dist.sign = 0; // absolute value is always positive
        too_close |= FPN_LessThan(abs_dist, min_spacing);
        active_pos &= active_pos - 1;
      }
    }

    // POSITION SIZING: compute quantity from balance and risk percentage
    // qty = (balance * risk_pct) / price
    // this replaces the stream quantity - we decide how much to buy, not the
    // stream
    if (FPN_IsZero(fill_price)) return; // guard: no fill at price zero
    FPN<F> risk_amount = FPN_Mul(ctrl->balance, ctrl->config.risk_pct);
    FPN<F> sized_qty = FPN_DivNoAssert(risk_amount, fill_price);

    // balance check: can we afford this position + entry fee? (branchless)
    FPN<F> cost = FPN_Mul(fill_price, sized_qty);
    FPN<F> entry_fee = FPN_Mul(cost, ctrl->config.fee_rate);
    FPN<F> total_cost = FPN_AddSat(cost, entry_fee);
    int can_afford = FPN_GreaterThanOrEqual(ctrl->balance, total_cost);

    // CIRCUIT BREAKER: halt if total P&L has dropped below max_drawdown_pct of
    // starting balance total_pnl = realized + unrealized, drawdown_limit =
    // -(starting_balance * max_drawdown_pct)
    FPN<F> total_pnl_check =
        FPN_AddSat(ctrl->realized_pnl, ctrl->portfolio_delta);
    FPN<F> drawdown_limit = FPN_Negate(
        FPN_Mul(ctrl->config.starting_balance, ctrl->config.max_drawdown_pct));
    int not_blown = FPN_GreaterThan(total_pnl_check, drawdown_limit);

    // EXPOSURE LIMIT: cap total deployed capital at max_exposure_pct of
    // starting balance deployed = starting_balance - current_balance (how much
    // is in positions)
    FPN<F> deployed = FPN_Sub(ctrl->config.starting_balance, ctrl->balance);
    FPN<F> max_deployed =
        FPN_Mul(ctrl->config.starting_balance, ctrl->config.max_exposure_pct);
    int under_limit =
        FPN_LessThan(FPN_AddSat(deployed, total_cost), max_deployed);

    // new position: room AND spacing AND balance AND not blown AND under
    // exposure limit
    int is_new = !found & (Portfolio_CountActive(&ctrl->portfolio) < (int)ctrl->config.max_positions) & !too_close &
                 can_afford & not_blown & under_limit;
    if (is_new) {
      // volatility-based TP/SL with fee floor
      FPN<F> stddev = ctrl->rolling.price_stddev;
      int has_stats = !FPN_IsZero(stddev);

      // volatility path: TP = entry + stddev * tp_mult, SL = entry - stddev * sl_mult
      // strategy-aware: momentum uses wider TP / tighter SL than mean reversion
      FPN<F> hundred = FPN_FromDouble<F>(100.0);
      FPN<F> tp_mult, sl_mult;
      if (ctrl->strategy_id == STRATEGY_MOMENTUM) {
          // adaptive momentum TP/SL — scale by R² from rolling stats
          // R² ∈ [0,1]: high R² = consistent trend → widen TP, tighten SL
          //              low R²  = choppy           → tighten TP, widen SL
          FPN<F> r2 = ctrl->rolling.price_r_squared;

          // TP: base * (tp_r2_min + R²) → range [tp_r2_min, 1+tp_r2_min] of config value
          // strong trend lets winners run, weak trend takes profits early
          FPN<F> tp_scale = FPN_AddSat(ctrl->config.momentum_tp_r2_min, r2);
          tp_mult = FPN_Mul(ctrl->config.momentum_tp_mult, tp_scale);

          // SL: base * (sl_r2_max - R²*tp_r2_min) → range [1.0x, sl_r2_max] of config value
          // choppy = wider SL (avoid whipsaw stops), consistent = tighter SL
          FPN<F> sl_scale = FPN_SubSat(ctrl->config.momentum_sl_r2_max,
                                        FPN_Mul(r2, ctrl->config.momentum_tp_r2_min));
          sl_mult = FPN_Mul(ctrl->config.momentum_sl_mult, sl_scale);

          // ROR bonus: accelerating trend → wider TP (ror_tp_bonus, default 1.2 = 20%)
          // uses direction only (not magnitude) to avoid calibration issues
          if (ctrl->regime_ror.count >= MAX_WINDOW) {
              LinearRegression3XResult<F> ror_r = RORRegressor_Compute(
                  const_cast<RORRegressor<F>*>(&ctrl->regime_ror));
              if (FPN_GreaterThan(ror_r.model.slope, FPN_Zero<F>())) {
                  tp_mult = FPN_Mul(tp_mult, ctrl->config.ror_tp_bonus);
              }
          }
      } else {
          tp_mult = FPN_Mul(ctrl->config.take_profit_pct, hundred);
          sl_mult = FPN_Mul(ctrl->config.stop_loss_pct, hundred);
      }
      FPN<F> tp_offset = FPN_Mul(stddev, tp_mult);
      FPN<F> sl_offset = FPN_Mul(stddev, sl_mult);

      // percentage fallback when rolling stats arent ready
      FPN<F> one = FPN_FromDouble<F>(1.0);
      FPN<F> tp_pct_up =
          FPN_Mul(fill_price, FPN_AddSat(one, ctrl->config.take_profit_pct));
      FPN<F> sl_pct_dn =
          FPN_Mul(fill_price, FPN_SubSat(one, ctrl->config.stop_loss_pct));

      // branchless select: volatility-based if stats ready, percentage-based
      // otherwise
      FPN<F> vol_tp = FPN_AddSat(fill_price, tp_offset);
      FPN<F> vol_sl = FPN_SubSat(fill_price, sl_offset);

      uint64_t stats_mask = -(uint64_t)has_stats;
      FPN<F> tp_price, sl_price;
      for (unsigned w = 0; w < FPN<F>::N; w++) {
        tp_price.w[w] =
            (vol_tp.w[w] & stats_mask) | (tp_pct_up.w[w] & ~stats_mask);
        sl_price.w[w] =
            (vol_sl.w[w] & stats_mask) | (sl_pct_dn.w[w] & ~stats_mask);
      }
      tp_price.sign = (vol_tp.sign & has_stats) | (tp_pct_up.sign & !has_stats);
      sl_price.sign = (vol_sl.sign & has_stats) | (sl_pct_dn.sign & !has_stats);

      // TP FLOOR: ensure TP is above the round-trip fee breakeven point
      // min_tp = entry + entry * fee_rate * fee_floor_mult
      // default 3.0 = 2x round-trip fees + 1x safety margin
      FPN<F> fee_floor_offset =
          FPN_Mul(fill_price, FPN_Mul(ctrl->config.fee_rate, ctrl->config.fee_floor_mult));
      FPN<F> tp_floor = FPN_AddSat(fill_price, fee_floor_offset);
      tp_price = FPN_Max(tp_price, tp_floor);

      // SL FLOOR: ensure SL distance is at least half the TP distance
      // prevents SL from being so tight that normal price fluctuations trigger
      // it with TP at +$209 (fee floor), SL should be at least -$104.50 this
      // gives a minimum 2:1 reward-to-risk ratio
      FPN<F> tp_dist =
          FPN_Sub(tp_price, fill_price); // how far TP is from entry
      FPN<F> min_sl_dist =
          FPN_Mul(tp_dist, ctrl->config.min_sl_tp_ratio); // SL must be at least this fraction of TP dist
      FPN<F> sl_floor = FPN_SubSat(fill_price, min_sl_dist);
      sl_price = FPN_Min(
          sl_price, sl_floor); // Min because SL is below entry (lower = wider)

      int slot = Portfolio_AddPositionWithExits(&ctrl->portfolio, sized_qty,
                                                fill_price, tp_price, sl_price);
      ctrl->total_buys++;
      if (slot >= 0) {
        ctrl->entry_ticks[slot] = ctrl->total_ticks;
        ctrl->entry_time[slot] = time(NULL);
        ctrl->entry_strategy[slot] = (uint8_t)ctrl->strategy_id;
        ctrl->portfolio.positions[slot].original_tp = tp_price;
        ctrl->portfolio.positions[slot].original_sl = sl_price;
      }

      // deduct cost + entry fee from balance
      ctrl->balance = FPN_SubSat(ctrl->balance, total_cost);
      ctrl->total_fees = FPN_AddSat(ctrl->total_fees, entry_fee);

      // buffer buy record (no file I/O on hot path)
      { double _avg = FPN_ToDouble(ctrl->rolling.price_avg);
        double _stddev = FPN_ToDouble(ctrl->rolling.price_stddev);
        double _spacing = FPN_ToDouble(RollingStats_EntrySpacing(&ctrl->rolling, ctrl->config.spacing_multiplier));
        double _bp = FPN_ToDouble(ctrl->buy_conds.price);
        double _gdist = (_avg != 0.0) ? ((FPN_ToDouble(fill_price) - _bp) / _avg) * 100.0 : 0.0;
        TradeLogBuffer_PushBuy(&ctrl->trade_buf, ctrl->total_ticks,
                              FPN_ToDouble(fill_price), FPN_ToDouble(sized_qty),
                              FPN_ToDouble(tp_price), FPN_ToDouble(sl_price),
                              _bp, FPN_ToDouble(ctrl->buy_conds.volume),
                              _stddev, _avg, FPN_ToDouble(ctrl->balance),
                              FPN_ToDouble(entry_fee), _spacing, _gdist,
                              ctrl->strategy_id, ctrl->regime.current_regime); }
    } else {
      // fill rejected — track reason for TUI diagnostics
      ctrl->fills_rejected++;
      if (!not_blown) ctrl->last_reject_reason = 4;       // breaker
      else if (!can_afford) ctrl->last_reject_reason = 2;  // balance
      else if (!under_limit) ctrl->last_reject_reason = 3; // exposure
      else if (found) ctrl->last_reject_reason = 6;        // duplicate
      else if (too_close) ctrl->last_reject_reason = 1;    // spacing
      else ctrl->last_reject_reason = 5;                    // full
    }

    fills &= fills - 1;
  }

  // clear consumed fills from pool - free slots for BuyGate
  pool->bitmap &= ~consumed;
  ctrl->prev_bitmap = pool->bitmap;

  //==================================================================================================
  // ACTIVE PHASE - EVERY N TICKS OR 3 SECONDS: slow-path operations
  // tick gate handles normal/high volume; time floor handles low-volume periods
  // where 100 ticks could take 60+ seconds (crypto off-hours, weekends)
  //==================================================================================================
  uint64_t now = (uint64_t)time(NULL);
  int time_floor_hit = (now - ctrl->last_slow_time >= ctrl->config.slow_path_max_secs);
  if (ctrl->tick_count < ctrl->config.poll_interval && !time_floor_hit)
    return;
  ctrl->tick_count = 0;
  ctrl->last_slow_time = now;

  // drain exit buffer — books P&L, updates balance, logs trades
  PortfolioController_DrainExits(ctrl);

  // TIME-BASED EXIT: close positions held too long with insufficient gain
  // frees capital trapped in positions where TP became unreachable (e.g. volatility
  // dropped after entry, making the stddev-based TP too far away)
  if (ctrl->config.max_hold_ticks > 0) {
    uint16_t active_check = ctrl->portfolio.active_bitmap;
    while (active_check) {
      int idx = __builtin_ctz(active_check);
      Position<F> *pos = &ctrl->portfolio.positions[idx];
      uint64_t entry_tick = ctrl->entry_ticks[idx];
      uint64_t held = (ctrl->total_ticks > entry_tick) ? (ctrl->total_ticks - entry_tick) : 0;

      if (held >= ctrl->config.max_hold_ticks) {
        // check if gain is below threshold — don't time-exit winners
        if (FPN_IsZero(pos->entry_price)) { active_check &= active_check - 1; continue; }
        FPN<F> gain = FPN_Sub(current_price, pos->entry_price);
        FPN<F> gain_pct = FPN_DivNoAssert(gain, pos->entry_price);
        int low_gain = FPN_LessThan(gain_pct, ctrl->config.min_hold_gain_pct);

        if (low_gain) {
          // slippage: simulate worse fill on time-based exit (sell at lower price)
          FPN<F> sell_price = current_price;
          if (!FPN_IsZero(ctrl->config.slippage_pct)) {
              FPN<F> slip = FPN_Mul(sell_price, ctrl->config.slippage_pct);
              sell_price = FPN_SubSat(sell_price, slip);
          }
          double entry_d = FPN_ToDouble(pos->entry_price);
          double exit_d  = FPN_ToDouble(sell_price);
          double qty_d   = FPN_ToDouble(pos->quantity);
          double delta_pct = 0.0;
          if (entry_d != 0.0) delta_pct = ((exit_d - entry_d) / entry_d) * 100.0;

          // compute realized P&L with fees (same as exit buffer drain)
          FPN<F> gross_proceeds = FPN_Mul(sell_price, pos->quantity);
          FPN<F> exit_fee = FPN_Mul(gross_proceeds, ctrl->config.fee_rate);
          FPN<F> net_proceeds = FPN_SubSat(gross_proceeds, exit_fee);
          FPN<F> entry_cost = FPN_Mul(pos->entry_price, pos->quantity);
          FPN<F> entry_fee_recon = FPN_Mul(entry_cost, ctrl->config.fee_rate);
          FPN<F> total_entry_cost = FPN_AddSat(entry_cost, entry_fee_recon);
          FPN<F> pos_pnl = FPN_Sub(net_proceeds, total_entry_cost);
          ctrl->realized_pnl = FPN_AddSat(ctrl->realized_pnl, pos_pnl);

          ctrl->balance = FPN_AddSat(ctrl->balance, net_proceeds);
          ctrl->total_fees = FPN_AddSat(ctrl->total_fees, exit_fee);
          ctrl->losses++;
          ctrl->gross_losses = FPN_AddSat(ctrl->gross_losses, FPN_Negate(pos_pnl));
          ctrl->total_hold_ticks += held;

          TradeLogBuffer_PushSell(&ctrl->trade_buf, ctrl->total_ticks, exit_d, qty_d,
                                  entry_d, delta_pct, "TIME",
                                  FPN_ToDouble(ctrl->balance), FPN_ToDouble(exit_fee),
                                  ctrl->entry_strategy[idx], ctrl->regime.current_regime);
          Portfolio_RemovePosition(&ctrl->portfolio, idx);
        }
      }
      active_check &= active_check - 1;
    }
  }

  // drain buffered trade records to CSV (file I/O moved off hot path)
  TradeLogBuffer_Drain(&ctrl->trade_buf, trade_log);

  // update rolling market stats - tracks price/volume trends for dynamic gate
  // adjustment
  RollingStats_Push(&ctrl->rolling, current_price, current_volume);
  RollingStats_Push(ctrl->rolling_long, current_price, current_volume);

  // compute unrealized P&L and estimate exit fees on open positions
  // gross P&L is what Portfolio_ComputePnL returns (price delta * qty)
  // net P&L subtracts estimated exit fees so the regression optimizes on real
  // profitability
  FPN<F> gross_pnl = Portfolio_ComputePnL(&ctrl->portfolio, current_price);
  FPN<F> portfolio_value =
      Portfolio_ComputeValue(&ctrl->portfolio, current_price);
  FPN<F> estimated_exit_fees = FPN_Mul(portfolio_value, ctrl->config.fee_rate);
  ctrl->portfolio_delta = FPN_Sub(gross_pnl, estimated_exit_fees);

  // feed rolling price slope to ROR for trend acceleration detection
  // ROR gives us slope-of-slopes: is the trend getting steeper or flattening?
  // fills after 8 slow-path cycles (~4 min), much faster than the old feeder chain
  {
    // construct a minimal regression result to push to ROR (it stores slope + r2)
    LinearRegression3XResult<F> slope_sample;
    slope_sample.model.slope = ctrl->rolling.price_slope;
    slope_sample.model.intercept = FPN_Zero<F>();
    slope_sample.r_squared = ctrl->rolling.price_r_squared;
    RORRegressor_Push(&ctrl->regime_ror, slope_sample);
  }

  // regime detection: compute signals from rolling stats + ROR, then classify
  {
    RegimeSignals<F> signals;
    Regime_ComputeSignals(&signals, &ctrl->rolling, ctrl->rolling_long, &ctrl->regime_ror);

    int old_regime = ctrl->regime.current_regime;
    Regime_Classify(&ctrl->regime, &signals, &ctrl->config);
    int new_regime = ctrl->regime.current_regime;
    if (new_regime != old_regime) {
      int old_strategy = ctrl->strategy_id;
      ctrl->strategy_id = Regime_ToStrategy(new_regime);
      ctrl->regime.regime_start_tick = ctrl->total_ticks;
      ctrl->regime.regime_start_time = time(NULL);
      if (ctrl->strategy_id != old_strategy)
        Regime_AdjustPositions(&ctrl->portfolio, &ctrl->rolling,
                                old_regime, new_regime, ctrl->entry_strategy, &ctrl->config);
    }
  }

  // strategy dispatch: adapt filters + compute buy gate based on active strategy
  switch (ctrl->strategy_id) {
  case STRATEGY_MEAN_REVERSION:
    MeanReversion_Adapt(&ctrl->mean_rev, current_price, ctrl->portfolio_delta,
                         ctrl->portfolio.active_bitmap, &ctrl->buy_conds,
                         &ctrl->config);
    // no ExitAdjust for MR — fixed TP/SL, no trailing. MR is buy-the-dip, take-the-profit.
    // trailing gives back gains in ranging markets where MR operates.
    ctrl->buy_conds = MeanReversion_BuySignal(&ctrl->mean_rev, &ctrl->rolling,
                                               ctrl->rolling_long, &ctrl->config);
    ctrl->buy_conds.gate_direction = 0;  // buy below (dips)
    break;
  case STRATEGY_MOMENTUM:
    Momentum_Adapt(&ctrl->momentum, current_price, ctrl->portfolio_delta,
                    ctrl->portfolio.active_bitmap, &ctrl->buy_conds,
                    &ctrl->config);
    Momentum_ExitAdjust(&ctrl->portfolio, current_price, &ctrl->rolling,
                         &ctrl->momentum, &ctrl->config);
    ctrl->buy_conds = Momentum_BuySignal(&ctrl->momentum, &ctrl->rolling,
                                          ctrl->rolling_long, &ctrl->config);
    ctrl->buy_conds.gate_direction = 1;  // buy above (breakouts)
    break;
  }

  // volatile / downtrend: pause buying entirely (existing positions keep running)
  // TRENDING_DOWN: momentum only trades long, so no entries in downtrends
  // future: replace with short strategy dispatch
  if (ctrl->regime.current_regime == REGIME_VOLATILE ||
      ctrl->regime.current_regime == REGIME_TRENDING_DOWN) {
    ctrl->buy_conds.price = FPN_Zero<F>();
    ctrl->buy_conds.volume = FPN_Zero<F>();
  }

  // post-SL cooldown: pause buying after stop loss to let market settle
  // during cooldown, RollingStats keep updating so regression adapts to new price level
  if (ctrl->sl_cooldown_counter > 0) {
    ctrl->sl_cooldown_counter--;
    ctrl->buy_conds.price = FPN_Zero<F>();
    ctrl->buy_conds.volume = FPN_Zero<F>();
  }
}
//======================================================================================================
// [SHARED FUNCTIONS — used by both multicore and single-threaded TUI paths]
//======================================================================================================
// these eliminate duplication between main.cpp and EngineTUI.hpp so adding a new
// strategy only requires updating these functions, not both TUI paths.
//======================================================================================================

// unpause: dispatch to active strategy's BuySignal
template <unsigned F>
inline void PortfolioController_Unpause(PortfolioController<F> *ctrl) {
    switch (ctrl->strategy_id) {
    case STRATEGY_MOMENTUM:
        ctrl->buy_conds = Momentum_BuySignal(&ctrl->momentum, &ctrl->rolling,
                                               ctrl->rolling_long, &ctrl->config);
        break;
    default:
        ctrl->buy_conds = MeanReversion_BuySignal(&ctrl->mean_rev, &ctrl->rolling,
                                                    ctrl->rolling_long, &ctrl->config);
        break;
    }
}

// manual regime cycle: RANGING → TRENDING → TRENDING_DOWN → VOLATILE → RANGING
template <unsigned F>
inline void PortfolioController_CycleRegime(PortfolioController<F> *ctrl) {
    int old = ctrl->regime.current_regime;
    int next = (old + 1) % 4;
    ctrl->regime.current_regime = next;
    ctrl->regime.proposed_regime = next;
    ctrl->regime.regime_start_tick = ctrl->total_ticks;
    int old_strategy = ctrl->strategy_id;
    ctrl->strategy_id = Regime_ToStrategy(next);
    if (ctrl->strategy_id != old_strategy)
        Regime_AdjustPositions(&ctrl->portfolio, &ctrl->rolling,
                                old, next, ctrl->entry_strategy, &ctrl->config);
    // volatile / downtrend: pause buying
    if (next == REGIME_VOLATILE || next == REGIME_TRENDING_DOWN) {
        ctrl->buy_conds.price = FPN_Zero<F>();
        ctrl->buy_conds.volume = FPN_Zero<F>();
    }
    const char *names[] = {"RANGING", "TRENDING", "VOLATILE", "TRENDING_DOWN"};
    fprintf(stderr, "[ENGINE] regime manually set to %s\n", names[next]);
}

// config hot-reload: one function for all fields, called from both TUI paths
template <unsigned F>
inline void PortfolioController_HotReload(PortfolioController<F> *ctrl,
                                           const ControllerConfig<F> &new_cfg) {
    ctrl->config.poll_interval       = new_cfg.poll_interval;
    ctrl->config.r2_threshold        = new_cfg.r2_threshold;
    ctrl->config.slope_scale_buy     = new_cfg.slope_scale_buy;
    ctrl->config.max_shift           = new_cfg.max_shift;
    ctrl->config.take_profit_pct     = new_cfg.take_profit_pct;
    ctrl->config.stop_loss_pct       = new_cfg.stop_loss_pct;
    ctrl->config.fee_rate            = new_cfg.fee_rate;
    ctrl->config.risk_pct            = new_cfg.risk_pct;
    ctrl->config.volume_multiplier   = new_cfg.volume_multiplier;
    ctrl->config.entry_offset_pct    = new_cfg.entry_offset_pct;
    ctrl->config.spacing_multiplier  = new_cfg.spacing_multiplier;
    ctrl->config.offset_min          = new_cfg.offset_min;
    ctrl->config.offset_max          = new_cfg.offset_max;
    ctrl->config.vol_mult_min        = new_cfg.vol_mult_min;
    ctrl->config.vol_mult_max        = new_cfg.vol_mult_max;
    ctrl->config.filter_scale        = new_cfg.filter_scale;
    ctrl->config.max_drawdown_pct    = new_cfg.max_drawdown_pct;
    ctrl->config.max_exposure_pct    = new_cfg.max_exposure_pct;
    ctrl->config.max_positions       = new_cfg.max_positions;
    ctrl->config.offset_stddev_mult  = new_cfg.offset_stddev_mult;
    ctrl->config.offset_stddev_min   = new_cfg.offset_stddev_min;
    ctrl->config.offset_stddev_max   = new_cfg.offset_stddev_max;
    ctrl->config.min_long_slope      = new_cfg.min_long_slope;
    ctrl->config.tp_hold_score       = new_cfg.tp_hold_score;
    ctrl->config.tp_trail_mult       = new_cfg.tp_trail_mult;
    ctrl->config.sl_trail_mult       = new_cfg.sl_trail_mult;
    ctrl->config.fee_floor_mult      = new_cfg.fee_floor_mult;
    ctrl->config.min_sl_tp_ratio     = new_cfg.min_sl_tp_ratio;
    ctrl->config.ror_tp_bonus        = new_cfg.ror_tp_bonus;
    ctrl->config.momentum_tp_r2_min  = new_cfg.momentum_tp_r2_min;
    ctrl->config.momentum_sl_r2_max  = new_cfg.momentum_sl_r2_max;
    ctrl->config.squeeze_decay       = new_cfg.squeeze_decay;
    ctrl->config.offset_adapt_scale  = new_cfg.offset_adapt_scale;
    ctrl->config.stddev_adapt_scale  = new_cfg.stddev_adapt_scale;
    ctrl->config.vol_adapt_scale     = new_cfg.vol_adapt_scale;
    ctrl->config.breakout_min        = new_cfg.breakout_min;
    ctrl->config.slow_path_max_secs  = new_cfg.slow_path_max_secs;
    ctrl->config.max_hold_ticks      = new_cfg.max_hold_ticks;
    ctrl->config.min_hold_gain_pct   = new_cfg.min_hold_gain_pct;
    // regime + momentum
    ctrl->config.regime_slope_threshold = new_cfg.regime_slope_threshold;
    ctrl->config.regime_r2_threshold    = new_cfg.regime_r2_threshold;
    ctrl->config.regime_volatile_stddev = new_cfg.regime_volatile_stddev;
    ctrl->config.regime_vol_spike_ratio = new_cfg.regime_vol_spike_ratio;
    ctrl->config.regime_hysteresis      = new_cfg.regime_hysteresis;
    ctrl->config.momentum_breakout_mult = new_cfg.momentum_breakout_mult;
    ctrl->config.momentum_tp_mult       = new_cfg.momentum_tp_mult;
    ctrl->config.momentum_sl_mult       = new_cfg.momentum_sl_mult;
    ctrl->config.spike_threshold         = new_cfg.spike_threshold;
    ctrl->config.spike_spacing_reduction = new_cfg.spike_spacing_reduction;
    ctrl->config.sl_cooldown_cycles      = new_cfg.sl_cooldown_cycles;
    ctrl->config.slippage_pct            = new_cfg.slippage_pct;
    // reset adaptive filters to new values
    ctrl->mean_rev.live_offset_pct    = new_cfg.entry_offset_pct;
    ctrl->mean_rev.live_vol_mult      = new_cfg.volume_multiplier;
    ctrl->mean_rev.live_stddev_mult   = new_cfg.offset_stddev_mult;
    ctrl->momentum.live_breakout_mult = new_cfg.momentum_breakout_mult;
    ctrl->momentum.live_vol_mult      = new_cfg.volume_multiplier;
    ctrl->regime.hysteresis_threshold = new_cfg.regime_hysteresis;
}

//======================================================================================================
// [SNAPSHOT SAVE/LOAD - v5]
//======================================================================================================
// persists full controller state for crash recovery and session resume
// v7 adds: session stats (buys, wins, losses, gross_wins/losses, hold_ticks, fees)
// v6 adds: entry_time (wall clock) for hold duration across restarts
// v5 adds: entry_ticks, entry_strategy, strategy_id, regime, momentum state
// backward compatible: v4/v5/v6 load gracefully (missing fields get defaults)
//======================================================================================================
#define CONTROLLER_SNAPSHOT_VERSION 7

template <unsigned F>
inline void PortfolioController_SaveSnapshot(const PortfolioController<F> *ctrl,
                                             const char *filepath) {
  FILE *f = fopen(filepath, "wb");
  if (!f) { fprintf(stderr, "[SNAPSHOT] failed to open %s for writing\n", filepath); return; }

  uint32_t magic = PORTFOLIO_SNAPSHOT_MAGIC;
  uint32_t version = CONTROLLER_SNAPSHOT_VERSION;
  fwrite(&magic, 4, 1, f);
  fwrite(&version, 4, 1, f);

  // portfolio (same as v4)
  fwrite(&ctrl->portfolio.active_bitmap, 2, 1, f);
  uint16_t pad = 0;
  fwrite(&pad, 2, 1, f);
  fwrite(ctrl->portfolio.positions, sizeof(Position<F>), 16, f);

  // realized P&L + balance (same as v4)
  fwrite(&ctrl->realized_pnl, sizeof(FPN<F>), 1, f);
  fwrite(&ctrl->balance, sizeof(FPN<F>), 1, f);

  // MR state (same as v4 but reordered for clarity)
  fwrite(&ctrl->mean_rev.live_offset_pct, sizeof(FPN<F>), 1, f);
  fwrite(&ctrl->mean_rev.live_vol_mult, sizeof(FPN<F>), 1, f);
  fwrite(&ctrl->mean_rev.live_stddev_mult, sizeof(FPN<F>), 1, f);

  // v5 fields
  fwrite(ctrl->entry_ticks, sizeof(uint64_t), 16, f);
  fwrite(ctrl->entry_strategy, sizeof(uint8_t), 16, f);
  fwrite(&ctrl->strategy_id, sizeof(int), 1, f);
  fwrite(&ctrl->regime.current_regime, sizeof(int), 1, f);
  fwrite(&ctrl->momentum.live_breakout_mult, sizeof(FPN<F>), 1, f);
  fwrite(&ctrl->momentum.live_vol_mult, sizeof(FPN<F>), 1, f);

  // v6: wall clock entry times for hold duration display
  fwrite(ctrl->entry_time, sizeof(time_t), 16, f);

  // v7: session stats
  fwrite(&ctrl->total_buys, sizeof(uint32_t), 1, f);
  fwrite(&ctrl->wins, sizeof(uint32_t), 1, f);
  fwrite(&ctrl->losses, sizeof(uint32_t), 1, f);
  fwrite(&ctrl->gross_wins, sizeof(FPN<F>), 1, f);
  fwrite(&ctrl->gross_losses, sizeof(FPN<F>), 1, f);
  fwrite(&ctrl->total_hold_ticks, sizeof(uint64_t), 1, f);
  fwrite(&ctrl->total_fees, sizeof(FPN<F>), 1, f);

  fflush(f);
  fclose(f);
}

template <unsigned F>
inline int PortfolioController_LoadSnapshot(PortfolioController<F> *ctrl,
                                            const char *filepath) {
  FILE *f = fopen(filepath, "rb");
  if (!f) return 0;

  uint32_t magic, version;
  if (fread(&magic, 4, 1, f) != 1 || magic != PORTFOLIO_SNAPSHOT_MAGIC) {
    fprintf(stderr, "[SNAPSHOT] bad magic in %s - ignoring\n", filepath);
    fclose(f); return 0;
  }
  if (fread(&version, 4, 1, f) != 1) { fclose(f); return 0; }

  // reject anything older than v4
  if (version < 4) {
    fprintf(stderr, "[SNAPSHOT] version %u too old in %s - ignoring\n", version, filepath);
    fclose(f); return 0;
  }

  // portfolio (v4+)
  uint16_t bitmap, pad;
  if (fread(&bitmap, 2, 1, f) != 1) { fclose(f); return 0; }
  if (fread(&pad, 2, 1, f) != 1) { fclose(f); return 0; }
  if (fread(ctrl->portfolio.positions, sizeof(Position<F>), 16, f) != 16) { fclose(f); return 0; }
  ctrl->portfolio.active_bitmap = bitmap;

  if (version == 4) {
    // v4 format: realized_pnl, live_offset_pct, live_vol_mult, live_stddev_mult, balance
    FPN<F> realized, offset, vmult, stdmult, bal;
    if (fread(&realized, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(&offset, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(&vmult, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(&stdmult, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(&bal, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    ctrl->realized_pnl = realized;
    ctrl->mean_rev.live_offset_pct = offset;
    ctrl->mean_rev.live_vol_mult = vmult;
    ctrl->mean_rev.live_stddev_mult = stdmult;
    ctrl->balance = bal;
    // v4 defaults for new fields
    ctrl->strategy_id = STRATEGY_MEAN_REVERSION;
    ctrl->regime.current_regime = REGIME_RANGING;
    fprintf(stderr, "[SNAPSHOT] loaded v4 snapshot — defaulting to RANGING/MR\n");
  } else {
    // v5+ format
    if (fread(&ctrl->realized_pnl, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(&ctrl->balance, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(&ctrl->mean_rev.live_offset_pct, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(&ctrl->mean_rev.live_vol_mult, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(&ctrl->mean_rev.live_stddev_mult, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(ctrl->entry_ticks, sizeof(uint64_t), 16, f) != 16) { fclose(f); return 0; }
    if (fread(ctrl->entry_strategy, sizeof(uint8_t), 16, f) != 16) { fclose(f); return 0; }
    if (fread(&ctrl->strategy_id, sizeof(int), 1, f) != 1) { fclose(f); return 0; }
    if (fread(&ctrl->regime.current_regime, sizeof(int), 1, f) != 1) { fclose(f); return 0; }
    if (fread(&ctrl->momentum.live_breakout_mult, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(&ctrl->momentum.live_vol_mult, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }

    // v6: wall clock entry times
    if (version >= 6) {
      if (fread(ctrl->entry_time, sizeof(time_t), 16, f) != 16) { fclose(f); return 0; }
    }

    // v7: session stats
    if (version >= 7) {
      if (fread(&ctrl->total_buys, sizeof(uint32_t), 1, f) != 1) { fclose(f); return 0; }
      if (fread(&ctrl->wins, sizeof(uint32_t), 1, f) != 1) { fclose(f); return 0; }
      if (fread(&ctrl->losses, sizeof(uint32_t), 1, f) != 1) { fclose(f); return 0; }
      if (fread(&ctrl->gross_wins, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
      if (fread(&ctrl->gross_losses, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
      if (fread(&ctrl->total_hold_ticks, sizeof(uint64_t), 1, f) != 1) { fclose(f); return 0; }
      if (fread(&ctrl->total_fees, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    }
  }

  // loaded positions keep their TP/SL exits (exit gate runs during warmup)
  // but do NOT skip warmup — rolling stats need real data before new buys
  // the buy gate stays disabled until warmup completes normally

  // v5 backward compat: entry_time wasn't saved, approximate from now
  if (version < 6) {
    uint16_t active = ctrl->portfolio.active_bitmap;
    time_t now = time(NULL);
    while (active) {
      int idx = __builtin_ctz(active);
      ctrl->entry_time[idx] = now;
      active &= active - 1;
    }
    if (ctrl->portfolio.active_bitmap)
      fprintf(stderr, "[SNAPSHOT] v%u: entry_time not saved, hold times start from now\n", version);
  }

  int count = __builtin_popcount(bitmap);
  fprintf(stderr, "[SNAPSHOT] loaded %d positions from %s (v%u)\n", count, filepath, version);
  return 1;
}
//======================================================================================================
//======================================================================================================
//======================================================================================================
#endif // PORTFOLIO_CONTROLLER_HPP
