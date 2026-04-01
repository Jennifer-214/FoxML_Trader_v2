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
#include "../ML_Headers/WelfordStats.hpp"
#include "../Strategies/MeanReversion.hpp"
#include "../Strategies/Momentum.hpp"
#include "../Strategies/SimpleDip.hpp"
#include "../Strategies/MLStrategy.hpp"
#include "../Strategies/RegimeDetector.hpp"
#if __has_include("../Strategies/private/EmaCross.hpp")
#include "../Strategies/private/EmaCross.hpp"
#endif
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
  uint64_t entry_ticks[MAX_PORTFOLIO_POSITIONS]; // tick at which each position was entered
  time_t entry_time[MAX_PORTFOLIO_POSITIONS];    // wall clock time at entry
  uint8_t entry_strategy[MAX_PORTFOLIO_POSITIONS]; // which strategy_id entered each position

  // kill switch state (sticky — survives until session reset or manual 'k')
  int kill_switch_active;           // 1 = all buying halted
  int kill_reason;                  // 0=none, 1=daily_loss, 2=drawdown
  FPN<F> daily_realized_pnl;        // accumulated realized P&L this session (resets on 24h boundary)
  uint32_t kill_recovery_counter;   // slow-path cycles remaining after kill reset before trading resumes
  double last_vol_scale;            // most recent vol scale factor applied (for TUI display)

  // per-strategy reward attribution
  struct StrategyStats {
    FPN<F> realized_pnl;
    uint32_t wins;
    uint32_t losses;
    uint32_t total_trades;
  };
  StrategyStats strategy_stats[5]; // 0=MR, 1=Momentum, 2=SimpleDip, 3=ML, 4=EmaCross

  int state;
  FPN<F> price_sum;
  FPN<F> volume_sum;
  uint64_t warmup_count;

  int strategy_id;
  MeanReversionState<F> mean_rev;
  MomentumState<F> momentum;
  SimpleDipState<F> simple_dip;
  MLStrategyState<F> ml_strategy;
#ifdef STRATEGY_EMA_CROSS
  EmaCrossState<F> ema_cross;
#endif
  RegimeState<F> regime;
  ModelHandle<F> regime_model;       // Mode A: regime signal enrichment model
  RegimeSignals<F> last_signals;     // cached for ML strategy BuySignal access

  RORRegressor<F> regime_ror;  // slope-of-slopes for trend acceleration detection
  FPN<F> volume_spike_ratio;   // current_volume / rolling.volume_max (spike detection)
  uint32_t sl_cooldown_counter; // remaining slow-path cycles before buy gate re-enables
  double session_high;         // highest price since startup
  double session_low;          // lowest price since startup
  FPN<F> peak_equity;          // highest equity seen (for max drawdown tracking)
  FPN<F> max_drawdown;         // largest peak-to-trough equity drop ($)
  FPN<F> session_start_equity; // equity at engine startup (for session P&L)
  int current_session;          // 0=asian, 1=european, 2=us, 3=overnight
  FPN<F> session_mult;          // current session gate multiplier
  FPN<F> book_imbalance;        // bid/ask imbalance from depth stream [-1, +1] (updated externally)
  // EMA price tracker — updates every tick on hot path for responsive gate
  FPN<F> ema_price;             // exponential moving average of price (hot path, ~2ns per tick)
  int ema_initialized;          // 0 until first tick sets it to current price

  // hot-path fast-react layer — precomputed on slow path, applied every tick
  FPN<F> gate_offset;           // distance from EMA to gate price (set on slow path)
  FPN<F> danger_warn;           // price threshold where gradient starts (avg - warn_stddevs * σ)
  FPN<F> danger_crash;          // price threshold where gate zeroes (avg - crash_stddevs * σ)
  FPN<F> danger_range_inv;      // 1 / (warn - crash), precomputed for hot-path multiply
  FPN<F> danger_score;          // current danger score [0, 1] — computed every tick
  int buying_halted;            // centralized halt — checked every tick after gate tracking
  int halt_reason;              // 0=none, 1=kill, 2=recovery, 3=volatile, 4=cooldown, 5=wind_down, 6=paused

  uint32_t idle_cycles;         // slow-path cycles since last fill (gate death spiral recovery)
  uint32_t fills_rejected;     // total fills rejected since startup
  int last_reject_reason;      // 0=none, 1=spacing, 2=balance, 3=exposure, 4=breaker, 5=full, 6=dup
  TradeLogBuffer trade_buf;    // buffered trade log — hot path pushes, slow path drains

  //================================================================================================
  // COLD — slow path only, kept at end to avoid polluting hot cache lines
  //================================================================================================
  RollingStats<F> rolling;
  RollingStats<F, 512> *rolling_long;  // heap-allocated (24KB), slow path only

  // Welford online trackers (unbounded stream, O(1) push)
  WelfordTracker<F> pnl_tracker;      // per-exit P&L distribution
  WelfordTracker<F> signal_tracker;   // buy signal strength distribution
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
  // ML strategy
  Model_Init(&ctrl->ml_strategy.buy_model);
  ctrl->ml_strategy.model_ready = 0;
  ctrl->ml_strategy.last_prediction = FPN_Zero<F>();
  memset(ctrl->ml_strategy.feature_buf, 0, sizeof(ctrl->ml_strategy.feature_buf));
  // load ML models if configured
  if (config.ml_backend != 0)
    Model_Load(&ctrl->ml_strategy.buy_model, config.ml_model_path, config.ml_backend);
  Model_Init(&ctrl->regime_model);
  if (config.regime_model_backend != 0)
    Model_Load(&ctrl->regime_model, config.regime_model_path, config.regime_model_backend);
  // regime detector
  Regime_Init(&ctrl->regime, config.regime_hysteresis);
  ctrl->regime_ror = RORRegressor_Init<F>();
  ctrl->volume_spike_ratio = FPN_Zero<F>();
  ctrl->sl_cooldown_counter = 0;
  ctrl->session_high = 0.0;
  ctrl->session_low = 0.0;
  ctrl->peak_equity = config.starting_balance;
  ctrl->max_drawdown = FPN_Zero<F>();
  ctrl->session_start_equity = FPN_Zero<F>();  // set on first slow-path equity calc
  ctrl->current_session = -1;  // unset until first slow path
  ctrl->session_mult = FPN_FromDouble<F>(1.0);
  ctrl->book_imbalance = FPN_Zero<F>();
  ctrl->idle_cycles = 0;
  ctrl->fills_rejected = 0;
  ctrl->last_reject_reason = 0;
  ctrl->kill_switch_active = 0;
  ctrl->kill_reason = 0;
  ctrl->daily_realized_pnl = FPN_Zero<F>();
  ctrl->kill_recovery_counter = 0;
  ctrl->buying_halted = 0;
  ctrl->halt_reason = 0;
  ctrl->last_vol_scale = 1.0;
  for (int i = 0; i < 5; i++) {
    ctrl->strategy_stats[i].realized_pnl = FPN_Zero<F>();
    ctrl->strategy_stats[i].wins = 0;
    ctrl->strategy_stats[i].losses = 0;
    ctrl->strategy_stats[i].total_trades = 0;
  }

  ctrl->pnl_tracker = Welford_Init<F>();
  ctrl->signal_tracker = Welford_Init<F>();

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
    // use actual entry fee stored at fill time (not reconstructed from current fee_rate)
    FPN<F> total_entry_cost = FPN_AddSat(entry_cost, pos->entry_fee);
    FPN<F> pos_pnl = FPN_Sub(net_proceeds, total_entry_cost);
    ctrl->realized_pnl = FPN_AddSat(ctrl->realized_pnl, pos_pnl);
    ctrl->daily_realized_pnl = FPN_AddSat(ctrl->daily_realized_pnl, pos_pnl);
    Welford_Push(&ctrl->pnl_tracker, pos_pnl);

    // per-strategy reward attribution
    {
      int strat = ctrl->entry_strategy[rec->position_index];
      if (strat >= 0 && strat < NUM_STRATEGIES) {
        ctrl->strategy_stats[strat].total_trades++;
        ctrl->strategy_stats[strat].realized_pnl = FPN_AddSat(
            ctrl->strategy_stats[strat].realized_pnl, pos_pnl);
      }
    }

    ctrl->balance = FPN_AddSat(ctrl->balance, net_proceeds);
    ctrl->total_fees = FPN_AddSat(ctrl->total_fees, exit_fee);

    // balance reconciliation: check that balance tracks realized_pnl
    // if these diverge, there's a balance leak
    if (Portfolio_CountActive(&ctrl->portfolio) == 0) {
      double bal = FPN_ToDouble(ctrl->balance);
      double expected = FPN_ToDouble(ctrl->config.starting_balance) + FPN_ToDouble(ctrl->realized_pnl);
      double drift = bal - expected;
      if (drift < -0.10 || drift > 0.10) {
        fprintf(stderr, "[BALANCE DRIFT] bal=%.2f expected=%.2f drift=%.2f realized=%.4f fees=%.4f\n",
                bal, expected, drift, FPN_ToDouble(ctrl->realized_pnl), FPN_ToDouble(ctrl->total_fees));
      }
    }

    const char *reason = (rec->reason == 0) ? "TP" : "SL";
    ctrl->wins += (rec->reason == 0);
    ctrl->losses += (rec->reason == 1);
    if (rec->reason == 1) {
        if (ctrl->config.sl_cooldown_adaptive) {
            // adaptive: scale by trend confidence (R² * negative slope direction)
            // high R² downtrend = long cooldown, low R² spike = short cooldown
            double r2 = FPN_ToDouble(ctrl->rolling.price_r_squared);
            double slope = FPN_ToDouble(ctrl->rolling.price_slope);
            double confidence = r2 * (slope < 0.0 ? 1.0 : 0.0);
            ctrl->sl_cooldown_counter = ctrl->config.sl_cooldown_base +
                (uint32_t)(ctrl->config.sl_cooldown_extra * confidence);
        } else if (ctrl->config.sl_cooldown_cycles > 0) {
            ctrl->sl_cooldown_counter = ctrl->config.sl_cooldown_cycles;
        }
    }

    // partial exit: if this was a TP exit and the position has a paired leg,
    // ratchet the pair's SL to breakeven (entry price) — makes it a "free trade"
    int8_t pair_idx = ctrl->portfolio.positions[rec->position_index].pair_index;
    if (rec->reason == 0 && pair_idx >= 0 && ctrl->config.breakeven_on_partial &&
        (ctrl->portfolio.active_bitmap & (1 << pair_idx))) {
        ctrl->portfolio.positions[pair_idx].stop_loss_price =
            FPN_Max(ctrl->portfolio.positions[pair_idx].stop_loss_price,
                    ctrl->portfolio.positions[pair_idx].entry_price);
        ctrl->portfolio.positions[pair_idx].pair_index = -1; // unpair
    }
    ctrl->portfolio.positions[rec->position_index].pair_index = -1; // clear exited slot

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
      // per-strategy win/loss
      int strat = ctrl->entry_strategy[rec->position_index];
      if (strat >= 0 && strat < NUM_STRATEGIES) {
        ctrl->strategy_stats[strat].wins += (1 & (uint32_t)is_win);
        ctrl->strategy_stats[strat].losses += (1 & (uint32_t)is_loss);
      }
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
//======================================================================================================
// [STRATEGY DISPATCH]
//======================================================================================================
// two entry points:
//   _StrategyDispatch: full slow-path cycle (adapt regression + compute buy signal)
//   _StrategyBuySignal: signal-only (unpause, warmup init — no regression feed)
// adding a new strategy: add one case to EACH function.
//======================================================================================================

// signal-only: compute buy gate without feeding regression (for unpause/init)
template <unsigned F>
inline void PortfolioController_StrategyBuySignal(PortfolioController<F> *ctrl) {
  FPN<F> gate_avg = ctrl->config.gate_ema_enabled ? ctrl->ema_price : FPN_Zero<F>();
  switch (ctrl->strategy_id) {
  case STRATEGY_MEAN_REVERSION:
    ctrl->buy_conds = MeanReversion_BuySignal(&ctrl->mean_rev, &ctrl->rolling,
                                               ctrl->rolling_long, &ctrl->config,
                                               gate_avg);
    ctrl->buy_conds.gate_direction = 0;
    break;
  case STRATEGY_MOMENTUM:
    ctrl->buy_conds = Momentum_BuySignal(&ctrl->momentum, &ctrl->rolling,
                                          ctrl->rolling_long, &ctrl->config,
                                          gate_avg);
    ctrl->buy_conds.gate_direction = 1;
    break;
  case STRATEGY_SIMPLE_DIP:
    ctrl->buy_conds = SimpleDip_BuySignal(&ctrl->simple_dip, &ctrl->rolling,
                                           ctrl->rolling_long, &ctrl->config);
    ctrl->buy_conds.gate_direction = 0;
    break;
#ifdef STRATEGY_EMA_CROSS
  case STRATEGY_EMA_CROSS:
    ctrl->buy_conds = EmaCross_BuySignal(&ctrl->ema_cross, &ctrl->rolling,
                                          ctrl->rolling_long, &ctrl->config,
                                          gate_avg);
    ctrl->buy_conds.gate_direction = 0;
    break;
#endif
  case STRATEGY_ML:
    ctrl->buy_conds = MLStrategy_BuySignal(&ctrl->ml_strategy, &ctrl->rolling,
                                            ctrl->rolling_long, (const void*)&ctrl->config,
                                            &ctrl->last_signals);
    ctrl->buy_conds.gate_direction = 0;
    break;
  }

  // capture gate offset for hot-path EMA tracking
  // offset = distance from EMA to gate price (strategy-specific, reapplied every tick to live EMA)
  if (!FPN_IsZero(ctrl->buy_conds.price) && !FPN_IsZero(ctrl->ema_price)) {
    if (ctrl->buy_conds.gate_direction == 0)
      ctrl->gate_offset = FPN_SubSat(ctrl->ema_price, ctrl->buy_conds.price);
    else
      ctrl->gate_offset = FPN_SubSat(ctrl->buy_conds.price, ctrl->ema_price);
  }

  // feed signal strength to Welford tracker (before no-trade band may zero it)
  if (!FPN_IsZero(ctrl->buy_conds.price) && !FPN_IsZero(ctrl->rolling.price_avg)) {
    FPN<F> sd = FPN_Sub(ctrl->buy_conds.price, ctrl->rolling.price_avg);
    FPN<F> neg = FPN_Negate(sd);
    uint64_t m = -(uint64_t)(sd.sign);
    FPN<F> abs_s;
    for (unsigned w = 0; w < FPN<F>::N; w++)
      abs_s.w[w] = (neg.w[w] & m) | (sd.w[w] & ~m);
    abs_s.sign = 0;
    Welford_Push(&ctrl->signal_tracker, FPN_DivNoAssert(abs_s, ctrl->rolling.price_avg));
  }

  // NO-TRADE BAND: suppress entries when signal strength < fee breakeven
  // cost-aware: signal must exceed fee_rate × no_trade_band_mult to justify trade
  if (ctrl->config.no_trade_band_enabled && !FPN_IsZero(ctrl->rolling.price_avg)) {
    FPN<F> min_signal = FPN_Mul(ctrl->config.fee_rate, ctrl->config.no_trade_band_mult);
    FPN<F> signal_dist = FPN_Sub(ctrl->buy_conds.price, ctrl->rolling.price_avg);
    // absolute value
    FPN<F> neg_sd = FPN_Negate(signal_dist);
    uint64_t neg_m = -(uint64_t)(signal_dist.sign);
    FPN<F> abs_sd;
    for (unsigned w = 0; w < FPN<F>::N; w++)
      abs_sd.w[w] = (neg_sd.w[w] & neg_m) | (signal_dist.w[w] & ~neg_m);
    abs_sd.sign = 0;
    FPN<F> signal_pct = FPN_DivNoAssert(abs_sd, ctrl->rolling.price_avg);
    if (FPN_LessThan(signal_pct, min_signal)) {
      ctrl->buy_conds.price = FPN_Zero<F>();
      ctrl->buy_conds.volume = FPN_Zero<F>();
    }
  }
}

// full dispatch: adapt regression + exit adjust + buy signal (slow path only)
template <unsigned F>
inline void PortfolioController_StrategyDispatch(PortfolioController<F> *ctrl,
                                                  FPN<F> current_price) {
  switch (ctrl->strategy_id) {
  case STRATEGY_MEAN_REVERSION:
    MeanReversion_Adapt(&ctrl->mean_rev, current_price, ctrl->portfolio_delta,
                         ctrl->portfolio.active_bitmap, &ctrl->buy_conds,
                         &ctrl->config);
    break;
  case STRATEGY_MOMENTUM:
    Momentum_Adapt(&ctrl->momentum, current_price, ctrl->portfolio_delta,
                    ctrl->portfolio.active_bitmap, &ctrl->buy_conds,
                    &ctrl->config);
    Momentum_ExitAdjust(&ctrl->portfolio, current_price, &ctrl->rolling,
                         &ctrl->momentum, &ctrl->config);
    break;
  case STRATEGY_SIMPLE_DIP:
    SimpleDip_Adapt(&ctrl->simple_dip, current_price, ctrl->portfolio_delta,
                     ctrl->portfolio.active_bitmap, &ctrl->buy_conds,
                     &ctrl->config);
    break;
#ifdef STRATEGY_EMA_CROSS
  case STRATEGY_EMA_CROSS:
    EmaCross_Adapt(&ctrl->ema_cross, current_price, ctrl->portfolio_delta,
                    ctrl->portfolio.active_bitmap, &ctrl->buy_conds, &ctrl->config);
    EmaCross_ExitAdjust(&ctrl->portfolio, current_price, &ctrl->rolling,
                         &ctrl->ema_cross, &ctrl->config);
    break;
#endif
  case STRATEGY_ML:
    MLStrategy_Adapt(&ctrl->ml_strategy, current_price, ctrl->portfolio_delta,
                      ctrl->portfolio.active_bitmap, &ctrl->buy_conds,
                      (const void*)&ctrl->config);
    break;
  }
  PortfolioController_StrategyBuySignal(ctrl);
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
                                     TradeLog *trade_log,
                                     int is_buyer_maker = 0) {
  // always increment tick counter (branchless, single add)
  ctrl->total_ticks++;
  ctrl->tick_count++;

  // EMA price update — every tick, fully branchless (~2-3ns)
  // ema = ema * alpha + price * (1 - alpha)
  // when ema is zero (first tick), alpha*0 + (1-alpha)*price ≈ price * 0.003
  // so we also add price * alpha when ema was zero, giving exactly price
  // subsequent ticks: ema is nonzero so the correction term is masked out
  {
    FPN<F> ema_new = FPN_Add(
      FPN_Mul(ctrl->ema_price, ctrl->config.gate_ema_alpha),
      FPN_Mul(current_price, ctrl->config.gate_ema_one_minus_alpha));
    // mask: all-ones if ema is zero (first tick), all-zeros otherwise
    uint64_t first_tick = -(uint64_t)(FPN_IsZero(ctrl->ema_price));
    // branchless select: first tick → current_price, otherwise → ema_new
    FPN<F> selected;
    for (unsigned i = 0; i < FPN<F>::N; i++)
      selected.w[i] = (current_price.w[i] & first_tick) | (ema_new.w[i] & ~first_tick);
    selected.sign = (current_price.sign & (int)first_tick) | (ema_new.sign & (int)~first_tick);
    ctrl->ema_price = selected;
  }

  // hot-path kill: catches equity drops between slow-path cycles
  // Portfolio_ComputeValue is O(popcount) — 1 multiply for single-slot mode
  if (ctrl->config.kill_switch_enabled && !ctrl->buying_halted) {
    FPN<F> pv = Portfolio_ComputeValue(&ctrl->portfolio, current_price);
    FPN<F> equity = FPN_AddSat(ctrl->balance, pv);
    int tripped = 0;
    // daily loss: (equity - start) / start < -threshold
    if (!FPN_IsZero(ctrl->session_start_equity)) {
      FPN<F> loss = FPN_Sub(ctrl->session_start_equity, equity); // positive when equity dropped
      FPN<F> limit = FPN_Mul(ctrl->session_start_equity, ctrl->config.kill_switch_daily_loss_pct);
      if (FPN_GreaterThan(loss, limit)) {
        ctrl->kill_reason = 1;
        tripped = 1;
      }
    }
    // drawdown: (peak - equity) / peak > threshold
    if (!tripped && !FPN_IsZero(ctrl->peak_equity)) {
      FPN<F> dd = FPN_SubSat(ctrl->peak_equity, equity);
      FPN<F> limit = FPN_Mul(ctrl->peak_equity, ctrl->config.kill_switch_drawdown_pct);
      if (FPN_GreaterThan(dd, limit)) {
        ctrl->kill_reason = 2;
        tripped = 1;
      }
    }
    if (tripped) {
      ctrl->kill_switch_active = 1;
      ctrl->buying_halted = 1;
      ctrl->halt_reason = 1;
      ctrl->buy_conds.price = FPN_Zero<F>();
      ctrl->buy_conds.volume = FPN_Zero<F>();
      ctrl->gate_offset = FPN_Zero<F>();
      fprintf(stderr, "[KILL] hot-path: equity %.2f — trading halted (reason %d)\n",
              equity, ctrl->kill_reason);
    }
  }

  // hot-path gate tracking + danger gradient — SKIPPED when halted
  // no ordering dependency: halted = nothing modifies buy_conds, gate_offset stays zero
  if (!ctrl->buying_halted) {
    // gate tracking: recompute gate price from live EMA + stored offset
    // gate_offset is set on slow path by strategy BuySignal; EMA updates every tick above
    if (!FPN_IsZero(ctrl->gate_offset)) {
      if (ctrl->buy_conds.gate_direction == 0)
        ctrl->buy_conds.price = FPN_SubSat(ctrl->ema_price, ctrl->gate_offset);
      else
        ctrl->buy_conds.price = FPN_AddSat(ctrl->ema_price, ctrl->gate_offset);
    }

    // danger gradient: proportional crash protection between slow-path cycles
    // danger_score ∈ [0, 1]: 0 = safe, 1 = crash. scales gate price toward zero.
    if (ctrl->config.danger_enabled && !FPN_IsZero(ctrl->danger_range_inv)) {
      FPN<F> depth = FPN_SubSat(ctrl->danger_warn, current_price);
      FPN<F> raw = FPN_Mul(depth, ctrl->danger_range_inv);
      FPN<F> one = FPN_FromDouble<F>(1.0);
      FPN<F> zero = FPN_Zero<F>();
      ctrl->danger_score = FPN_Min(FPN_Max(raw, zero), one);
      FPN<F> gate_scale = FPN_SubSat(one, ctrl->danger_score);
      ctrl->buy_conds.price = FPN_Mul(ctrl->buy_conds.price, gate_scale);
    }
  }

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
      RollingStats_Push(&ctrl->rolling, current_price, current_volume, is_buyer_maker);
      RollingStats_Push(ctrl->rolling_long, current_price, current_volume, is_buyer_maker);
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
      SimpleDip_Init(&ctrl->simple_dip, &ctrl->rolling, &ctrl->buy_conds);
      MLStrategy_Init(&ctrl->ml_strategy, &ctrl->rolling, &ctrl->buy_conds);
#ifdef STRATEGY_EMA_CROSS
      EmaCross_Init(&ctrl->ema_cross, &ctrl->rolling, &ctrl->buy_conds);
#endif
      // use configured default strategy (0=MR, 1=Momentum, 2=SimpleDip)
      if (ctrl->config.default_strategy >= 0)
          ctrl->strategy_id = ctrl->config.default_strategy;
      PortfolioController_StrategyDispatch(ctrl, current_price);
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

    // VOL-SCALED SIZING: scale qty inversely with volatility
    // high vol → smaller position, low vol → larger (consistent risk per trade)
    // uses long-window stddev as baseline so it self-calibrates
    ctrl->last_vol_scale = 1.0;
    if (ctrl->config.vol_sizing_enabled
        && !FPN_IsZero(ctrl->rolling.price_stddev)
        && !FPN_IsZero(ctrl->rolling_long->price_stddev)) {
      FPN<F> vol_ratio = FPN_DivNoAssert(ctrl->rolling_long->price_stddev,
                                          ctrl->rolling.price_stddev);
      vol_ratio = FPN_Max(vol_ratio, ctrl->config.vol_scale_min);
      vol_ratio = FPN_Min(vol_ratio, ctrl->config.vol_scale_max);
      sized_qty = FPN_Mul(sized_qty, vol_ratio);
      ctrl->last_vol_scale = FPN_ToDouble(vol_ratio);
    }

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
    // starting balance. deployed = market value of open positions at fill price
    // (NOT starting - balance, which includes realized losses/fees as phantom exposure)
    FPN<F> deployed = Portfolio_ComputeValue(&ctrl->portfolio, fill_price);
    FPN<F> max_deployed =
        FPN_Mul(ctrl->config.starting_balance, ctrl->config.max_exposure_pct);
    int under_limit =
        FPN_LessThan(FPN_AddSat(deployed, total_cost), max_deployed);

    // min volatility filter: skip trades when stddev is too small relative to price
    // prevents entries where fee-floored TP is unreachable by stddev-based SL
    FPN<F> stddev_ratio = FPN_IsZero(fill_price) ? FPN_Zero<F>()
        : FPN_DivNoAssert(ctrl->rolling.price_stddev, fill_price);
    int vol_sufficient = FPN_IsZero(ctrl->config.min_stddev_pct) |
                         FPN_GreaterThanOrEqual(stddev_ratio, ctrl->config.min_stddev_pct);

    // new position: room AND spacing AND balance AND not blown AND under
    // exposure limit AND sufficient volatility
    int is_new = !found & (Portfolio_CountActive(&ctrl->portfolio) < (int)ctrl->config.max_positions) & !too_close &
                 can_afford & not_blown & under_limit & vol_sufficient;
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

      // partial exits: split into two legs with different TP levels
      // leg A exits at TP1 (conservative), leg B rides to TP2 (extended)
      // when disabled or not enough room for 2 slots, falls back to single position
      int do_split = ctrl->config.partial_exit_enabled &&
          (Portfolio_CountActive(&ctrl->portfolio) + 2 <= (int)ctrl->config.max_positions);

      int fill_ok = 0; // track whether any position was actually created

      if (do_split) {
        FPN<F> qty_a = FPN_Mul(sized_qty, ctrl->config.partial_exit_pct);
        FPN<F> qty_b = FPN_Sub(sized_qty, qty_a);

        // split entry fee proportionally between legs
        FPN<F> fee_a = FPN_Mul(entry_fee, ctrl->config.partial_exit_pct);
        FPN<F> fee_b = FPN_Sub(entry_fee, fee_a);

        // TP2 = entry + (tp_dist * tp2_mult)
        FPN<F> tp_dist_2 = FPN_Mul(FPN_Sub(tp_price, fill_price), ctrl->config.tp2_mult);
        FPN<F> tp2_price = FPN_AddSat(fill_price, tp_dist_2);

        int slot_a = Portfolio_AddPositionWithExits(&ctrl->portfolio, qty_a,
                                                    fill_price, tp_price, sl_price, fee_a);
        int slot_b = Portfolio_AddPositionWithExits(&ctrl->portfolio, qty_b,
                                                    fill_price, tp2_price, sl_price, fee_b);
        if (slot_a >= 0 && slot_b >= 0) {
          // link them as a pair
          ctrl->portfolio.positions[slot_a].pair_index = (int8_t)slot_b;
          ctrl->portfolio.positions[slot_b].pair_index = (int8_t)slot_a;
          // metadata for both slots
          time_t now = time(NULL);
          ctrl->entry_ticks[slot_a] = ctrl->total_ticks;
          ctrl->entry_ticks[slot_b] = ctrl->total_ticks;
          ctrl->entry_time[slot_a] = now;
          ctrl->entry_time[slot_b] = now;
          ctrl->entry_strategy[slot_a] = (uint8_t)ctrl->strategy_id;
          ctrl->entry_strategy[slot_b] = (uint8_t)ctrl->strategy_id;
          ctrl->portfolio.positions[slot_a].original_tp = tp_price;
          ctrl->portfolio.positions[slot_a].original_sl = sl_price;
          ctrl->portfolio.positions[slot_b].original_tp = tp2_price;
          ctrl->portfolio.positions[slot_b].original_sl = sl_price;
          fill_ok = 1;
        } else {
          // rollback: if one slot succeeded but the other failed, remove it
          if (slot_a >= 0) {
            ctrl->portfolio.active_bitmap &= ~(1 << slot_a);
          }
          if (slot_b >= 0) {
            ctrl->portfolio.active_bitmap &= ~(1 << slot_b);
          }
        }
      } else {
        // single position (original behavior)
        int slot = Portfolio_AddPositionWithExits(&ctrl->portfolio, sized_qty,
                                                  fill_price, tp_price, sl_price, entry_fee);
        if (slot >= 0) {
          ctrl->entry_ticks[slot] = ctrl->total_ticks;
          ctrl->entry_time[slot] = time(NULL);
          ctrl->entry_strategy[slot] = (uint8_t)ctrl->strategy_id;
          ctrl->portfolio.positions[slot].original_tp = tp_price;
          ctrl->portfolio.positions[slot].original_sl = sl_price;
          fill_ok = 1;
        }
      }

      // only deduct balance and count the buy if a position was actually created
      if (fill_ok) {
        ctrl->total_buys++;
        ctrl->idle_cycles = 0;  // reset gate death spiral counter
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
      }
    } else {
      // fill rejected — track reason for TUI diagnostics
      ctrl->fills_rejected++;
      if (!not_blown) ctrl->last_reject_reason = 4;       // breaker
      else if (!can_afford) ctrl->last_reject_reason = 2;  // balance
      else if (!under_limit) ctrl->last_reject_reason = 3; // exposure
      else if (found) ctrl->last_reject_reason = 6;        // duplicate
      else if (too_close) ctrl->last_reject_reason = 1;    // spacing
      else if (!vol_sufficient) ctrl->last_reject_reason = 7; // min volatility
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
  // tick gate: most ticks return here with zero syscalls
  if (ctrl->tick_count < ctrl->config.poll_interval) {
    // time floor: check every 16 ticks to catch low-volume stalls (~0.3ns bitmask vs ~500ns syscall)
    if (ctrl->tick_count & 0xF) return;
    uint64_t now = (uint64_t)time(NULL);
    if (now - ctrl->last_slow_time < ctrl->config.slow_path_max_secs) return;
    // time floor hit — fall through to slow path
  }
  ctrl->tick_count = 0;
  ctrl->last_slow_time = (uint64_t)time(NULL);

  // session awareness: classify UTC hour and set gate multiplier
  // reuses last_slow_time — no extra syscall
  if (ctrl->config.session_filter_enabled) {
    time_t st = (time_t)ctrl->last_slow_time;
    struct tm *utc = gmtime(&st);
    int h = utc->tm_hour;
    if (h < 7)       { ctrl->current_session = 0; ctrl->session_mult = ctrl->config.session_asian_mult; }
    else if (h < 13)  { ctrl->current_session = 1; ctrl->session_mult = ctrl->config.session_european_mult; }
    else if (h < 20)  { ctrl->current_session = 2; ctrl->session_mult = ctrl->config.session_us_mult; }
    else              { ctrl->current_session = 3; ctrl->session_mult = ctrl->config.session_overnight_mult; }
  }

  // drain exit buffer — books P&L, updates balance, logs trades
  if (ctrl->exit_buf.count > 0)
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
          // use actual entry fee stored at fill time (not reconstructed)
          FPN<F> total_entry_cost = FPN_AddSat(entry_cost, pos->entry_fee);
          FPN<F> pos_pnl = FPN_Sub(net_proceeds, total_entry_cost);
          ctrl->realized_pnl = FPN_AddSat(ctrl->realized_pnl, pos_pnl);

          ctrl->balance = FPN_AddSat(ctrl->balance, net_proceeds);
          ctrl->total_fees = FPN_AddSat(ctrl->total_fees, exit_fee);

          // balance reconciliation (same check as DrainExits)
          if (Portfolio_CountActive(&ctrl->portfolio) <= 1) { // this position is about to be removed
            double bal = FPN_ToDouble(ctrl->balance);
            double expected = FPN_ToDouble(ctrl->config.starting_balance) + FPN_ToDouble(ctrl->realized_pnl);
            double drift = bal - expected;
            if (drift < -0.10 || drift > 0.10) {
              fprintf(stderr, "[BALANCE DRIFT TIME] bal=%.2f expected=%.2f drift=%.2f\n",
                      bal, expected, drift);
            }
          }

          ctrl->losses++;
          // branchless win/loss accounting (same pattern as DrainExits)
          {
            int is_loss = pos_pnl.sign;
            constexpr unsigned N2 = FPN<F>::N;
            uint64_t loss_mask = -(uint64_t)is_loss;
            FPN<F> neg_pnl = FPN_Negate(pos_pnl);
            FPN<F> loss_add;
            for (unsigned w = 0; w < N2; w++)
              loss_add.w[w] = neg_pnl.w[w] & loss_mask;
            loss_add.sign = 0;
            ctrl->gross_losses = FPN_AddSat(ctrl->gross_losses, loss_add);
          }
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
  RollingStats_Push(&ctrl->rolling, current_price, current_volume, is_buyer_maker);
  RollingStats_Push(ctrl->rolling_long, current_price, current_volume, is_buyer_maker);

  // precompute danger gradient thresholds for hot-path use
  // warn = avg - warn_stddevs * σ, crash = avg - crash_stddevs * σ
  if (ctrl->config.danger_enabled && !FPN_IsZero(ctrl->rolling.price_stddev)) {
    FPN<F> avg = ctrl->rolling.price_avg;
    FPN<F> sd = ctrl->rolling.price_stddev;
    ctrl->danger_warn = FPN_SubSat(avg, FPN_Mul(sd, ctrl->config.danger_warn_stddevs));
    ctrl->danger_crash = FPN_SubSat(avg, FPN_Mul(sd, ctrl->config.danger_crash_stddevs));
    FPN<F> range = FPN_SubSat(ctrl->danger_warn, ctrl->danger_crash);
    if (!FPN_IsZero(range))
      ctrl->danger_range_inv = FPN_DivNoAssert(FPN_FromDouble<F>(1.0), range);
  }

  // compute unrealized P&L and estimate exit fees on open positions
  // gross P&L is what Portfolio_ComputePnL returns (price delta * qty)
  // net P&L subtracts estimated exit fees so the regression optimizes on real
  // profitability
  FPN<F> gross_pnl = Portfolio_ComputePnL(&ctrl->portfolio, current_price);
  FPN<F> portfolio_value =
      Portfolio_ComputeValue(&ctrl->portfolio, current_price);
  FPN<F> estimated_exit_fees = FPN_Mul(portfolio_value, ctrl->config.fee_rate);
  ctrl->portfolio_delta = FPN_Sub(gross_pnl, estimated_exit_fees);

  // track peak equity and max drawdown (branchless, all FPN)
  {
    FPN<F> equity = FPN_AddSat(ctrl->balance, portfolio_value);
    // branchless first-tick select: zero → set to equity, nonzero → keep
    uint64_t first = -(uint64_t)FPN_IsZero(ctrl->session_start_equity);
    FPN<F> sse;
    for (unsigned w = 0; w < FPN<F>::N; w++)
      sse.w[w] = (equity.w[w] & first) | (ctrl->session_start_equity.w[w] & ~first);
    sse.sign = (equity.sign & (int)first) | (ctrl->session_start_equity.sign & (int)~first);
    ctrl->session_start_equity = sse;
    ctrl->peak_equity = FPN_Max(ctrl->peak_equity, equity);
    FPN<F> dd = FPN_SubSat(ctrl->peak_equity, equity);
    ctrl->max_drawdown = FPN_Max(ctrl->max_drawdown, dd);
  }

  // KILL SWITCH: check daily loss and drawdown limits (all FPN)
  // sticky — once triggered, stays active until session reset or manual 'k'
  if (ctrl->config.kill_switch_enabled && !ctrl->kill_switch_active) {
    FPN<F> equity = FPN_AddSat(ctrl->balance, portfolio_value);
    // daily loss: loss = start - equity, limit = start * threshold
    if (!FPN_IsZero(ctrl->session_start_equity)) {
      FPN<F> loss = FPN_Sub(ctrl->session_start_equity, equity);
      FPN<F> limit = FPN_Mul(ctrl->session_start_equity, ctrl->config.kill_switch_daily_loss_pct);
      if (FPN_GreaterThan(loss, limit)) {
        ctrl->kill_switch_active = 1;
        ctrl->kill_reason = 1;
        double pct = (FPN_ToDouble(loss) / FPN_ToDouble(ctrl->session_start_equity)) * 100.0;
        fprintf(stderr, "[KILL] daily loss %.2f%% exceeded limit — trading halted\n", pct);
      }
    }
    // drawdown: dd = peak - equity, limit = peak * threshold
    if (!ctrl->kill_switch_active && !FPN_IsZero(ctrl->peak_equity)) {
      FPN<F> dd = FPN_SubSat(ctrl->peak_equity, equity);
      FPN<F> limit = FPN_Mul(ctrl->peak_equity, ctrl->config.kill_switch_drawdown_pct);
      if (FPN_GreaterThan(dd, limit)) {
        ctrl->kill_switch_active = 1;
        ctrl->kill_reason = 2;
        double pct = (FPN_ToDouble(dd) / FPN_ToDouble(ctrl->peak_equity)) * 100.0;
        fprintf(stderr, "[KILL] drawdown %.2f%% exceeded limit — trading halted\n", pct);
      }
    }
  }

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
    Regime_ComputeSignals(&signals, &ctrl->rolling, ctrl->rolling_long, &ctrl->regime_ror, ctrl->ema_price);

    // Mode A: regime model enrichment — add model_score to classification
    if (Model_IsLoaded(&ctrl->regime_model)) {
      float feat_buf[MODEL_MAX_FEATURES];
      int n = ModelFeatures_Pack(feat_buf, &signals, &ctrl->rolling, ctrl->rolling_long);
      signals.model_score = FPN_FromDouble<F>(
          (double)Model_Predict(&ctrl->regime_model, feat_buf, n));
    }

    ctrl->last_signals = signals; // cache for MLStrategy_BuySignal

    int old_regime = ctrl->regime.current_regime;
    Regime_Classify(&ctrl->regime, &signals, &ctrl->config);
    int new_regime = ctrl->regime.current_regime;
    if (new_regime != old_regime) {
      ctrl->regime.regime_start_tick = ctrl->total_ticks;
      ctrl->regime.regime_start_time = time(NULL);
      // only auto-switch strategy when default_strategy=-1 (regime auto mode)
      // when a specific strategy is selected, regime detection still runs
      // (for display/signals) but doesn't override the strategy
      if (ctrl->config.default_strategy == -1) {
        // legacy 2-strategy auto: MR + Momentum only
        int old_strategy = ctrl->strategy_id;
        ctrl->strategy_id = (new_regime == REGIME_TRENDING)
            ? STRATEGY_MOMENTUM : STRATEGY_MEAN_REVERSION;
        if (ctrl->strategy_id != old_strategy)
          Regime_AdjustPositions(&ctrl->portfolio, &ctrl->rolling,
                                  old_regime, new_regime, ctrl->entry_strategy, &ctrl->config);
      } else if (ctrl->config.default_strategy <= -2) {
        // full 4-strategy auto: MR + EMA Cross + Momentum + SimpleDip
        int old_strategy = ctrl->strategy_id;
        ctrl->strategy_id = Regime_ToStrategy(new_regime);
        if (ctrl->strategy_id != old_strategy)
          Regime_AdjustPositions(&ctrl->portfolio, &ctrl->rolling,
                                  old_regime, new_regime, ctrl->entry_strategy, &ctrl->config);
      }
    }
  }

  // strategy dispatch — single function, called from both slow path and unpause
  PortfolioController_StrategyDispatch(ctrl, current_price);

  // gate death spiral recovery: if no fills for idle_reset_cycles, decay gates
  // back toward initial config values to prevent permanent lockout after losses
  ctrl->idle_cycles++;
  if (ctrl->config.idle_reset_cycles > 0 && ctrl->idle_cycles >= ctrl->config.idle_reset_cycles) {
    FPN<F> decay = ctrl->config.squeeze_decay; // 10% of gap per cycle
    // MR: decay offset and volume mult toward initial config
    FPN<F> off_gap = FPN_Sub(ctrl->mean_rev.live_offset_pct, ctrl->config.entry_offset_pct);
    ctrl->mean_rev.live_offset_pct = FPN_Sub(ctrl->mean_rev.live_offset_pct, FPN_Mul(off_gap, decay));
    FPN<F> vol_gap = FPN_Sub(ctrl->mean_rev.live_vol_mult, ctrl->config.volume_multiplier);
    ctrl->mean_rev.live_vol_mult = FPN_Sub(ctrl->mean_rev.live_vol_mult, FPN_Mul(vol_gap, decay));
    FPN<F> sm_gap = FPN_Sub(ctrl->mean_rev.live_stddev_mult, ctrl->config.offset_stddev_mult);
    ctrl->mean_rev.live_stddev_mult = FPN_Sub(ctrl->mean_rev.live_stddev_mult, FPN_Mul(sm_gap, decay));
    // Momentum: decay breakout mult toward initial config
    FPN<F> bk_gap = FPN_Sub(ctrl->momentum.live_breakout_mult, ctrl->config.momentum_breakout_mult);
    ctrl->momentum.live_breakout_mult = FPN_Sub(ctrl->momentum.live_breakout_mult, FPN_Mul(bk_gap, decay));
    // clear stale regression so it doesn't re-tighten immediately
    ctrl->mean_rev.has_regression = 0;
    ctrl->momentum.has_regression = 0;
  }

  // session awareness: scale volume gate by session multiplier
  // wider mult = more volume required = fewer entries during low-liquidity sessions
  if (ctrl->config.session_filter_enabled) {
    ctrl->buy_conds.volume = FPN_Mul(ctrl->buy_conds.volume, ctrl->session_mult);
  }

  // book imbalance gate: require bid excess before buying
  // book_imbalance is updated externally from depth thread (zero if no depth data)
  if (!FPN_IsZero(ctrl->config.min_book_imbalance)) {
    int book_ok = FPN_GreaterThanOrEqual(ctrl->book_imbalance, ctrl->config.min_book_imbalance);
    Gate_Zero(&ctrl->buy_conds, book_ok);
  }

  // CENTRALIZED HALT: single location for all halt conditions
  // counters decrement unconditionally — only the flag/reason uses priority
  {
    if (ctrl->sl_cooldown_counter > 0) ctrl->sl_cooldown_counter--;
    if (ctrl->kill_recovery_counter > 0) ctrl->kill_recovery_counter--;

    int halted = 0, reason = 0;
    if (ctrl->kill_switch_active)                            { halted = 1; reason = 1; }
    else if (ctrl->kill_recovery_counter > 0)                { halted = 1; reason = 2; }
    else if (ctrl->regime.current_regime == REGIME_VOLATILE) { halted = 1; reason = 3; }
    else if (ctrl->sl_cooldown_counter > 0)                  { halted = 1; reason = 4; }
    ctrl->buying_halted = halted;
    ctrl->halt_reason = reason;
    if (halted) {
      ctrl->buy_conds.price = FPN_Zero<F>();
      ctrl->buy_conds.volume = FPN_Zero<F>();
      ctrl->gate_offset = FPN_Zero<F>();
    }
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
    // don't unpause if a higher-priority halt is active
    if (ctrl->kill_switch_active || ctrl->kill_recovery_counter > 0) return;
    ctrl->buying_halted = 0;
    ctrl->halt_reason = 0;
    // signal-only — don't feed regression on unpause
    PortfolioController_StrategyBuySignal(ctrl);
}

// manual regime cycle: RANGING → TRENDING → TRENDING_DOWN → VOLATILE → RANGING
template <unsigned F>
inline void PortfolioController_CycleRegime(PortfolioController<F> *ctrl) {
    int old = ctrl->regime.current_regime;
    int next = (old + 1) % NUM_REGIMES;
    ctrl->regime.current_regime = next;
    ctrl->regime.proposed_regime = next;
    ctrl->regime.regime_start_tick = ctrl->total_ticks;
    int old_strategy = ctrl->strategy_id;
    ctrl->strategy_id = Regime_ToStrategy(next);
    if (ctrl->strategy_id != old_strategy)
        Regime_AdjustPositions(&ctrl->portfolio, &ctrl->rolling,
                                old, next, ctrl->entry_strategy, &ctrl->config);
    // volatile / downtrend: pause buying
    if (next == REGIME_VOLATILE) {
        ctrl->buy_conds.price = FPN_Zero<F>();
        ctrl->buy_conds.volume = FPN_Zero<F>();
    }
    const char *names[] = {"RANGING", "TRENDING", "VOLATILE", "TRENDING_DOWN", "MILD_TREND"};
    fprintf(stderr, "[ENGINE] regime manually set to %s\n", names[next]);
}

// config hot-reload: bulk copy all fields, then restore protected startup-only fields
// new config fields automatically hot-reload without touching this function
template <unsigned F>
inline void PortfolioController_HotReload(PortfolioController<F> *ctrl,
                                           const ControllerConfig<F> &new_cfg) {
    // save startup-only fields that should survive hot-reload
    FPN<F> saved_starting_balance = ctrl->config.starting_balance;
    uint32_t saved_warmup_ticks = ctrl->config.warmup_ticks;
    uint32_t saved_min_warmup = ctrl->config.min_warmup_samples;
    int saved_use_real_money = ctrl->config.use_real_money;

    // bulk copy — every field updates automatically
    ctrl->config = new_cfg;

    // restore protected fields
    ctrl->config.starting_balance = saved_starting_balance;
    ctrl->config.warmup_ticks = saved_warmup_ticks;
    ctrl->config.min_warmup_samples = saved_min_warmup;
    ctrl->config.use_real_money = saved_use_real_money;

    // reset adaptive filters to new values
    ctrl->mean_rev.live_offset_pct    = new_cfg.entry_offset_pct;
    ctrl->mean_rev.live_vol_mult      = new_cfg.volume_multiplier;
    ctrl->mean_rev.live_stddev_mult   = new_cfg.offset_stddev_mult;
    ctrl->momentum.live_breakout_mult = new_cfg.momentum_breakout_mult;
    ctrl->momentum.live_vol_mult      = new_cfg.volume_multiplier;
    ctrl->regime.hysteresis_threshold = new_cfg.regime_hysteresis;

    // live strategy switch
    // default_strategy >= 0: explicit strategy selection (0=MR, 1=Momentum, 2=SimpleDip)
    // default_strategy == -1: regime auto mode (regime detector picks the strategy)
    if (new_cfg.default_strategy >= 0 && new_cfg.default_strategy != ctrl->strategy_id) {
        ctrl->strategy_id = new_cfg.default_strategy;
        PortfolioController_StrategyBuySignal(ctrl);
        fprintf(stderr, "[ENGINE] strategy switched to %d via hot-reload\n", ctrl->strategy_id);
    } else if (new_cfg.default_strategy < 0) {
        // regime auto: let Regime_ToStrategy pick on next slow path
        // force a re-evaluation now so it takes effect immediately
        ctrl->strategy_id = Regime_ToStrategy(ctrl->regime.current_regime);
        PortfolioController_StrategyBuySignal(ctrl);
        fprintf(stderr, "[ENGINE] regime auto enabled — strategy=%d (from regime %d)\n",
                ctrl->strategy_id, ctrl->regime.current_regime);
    }
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
#define CONTROLLER_SNAPSHOT_VERSION 10

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

  // v8: starting_balance (survives config changes between sessions)
  fwrite(&ctrl->config.starting_balance, sizeof(FPN<F>), 1, f);

  // v9: kill switch state + per-strategy stats + session equity baseline
  fwrite(&ctrl->kill_switch_active, sizeof(int), 1, f);
  fwrite(&ctrl->kill_reason, sizeof(int), 1, f);
  fwrite(&ctrl->daily_realized_pnl, sizeof(FPN<F>), 1, f);
  fwrite(&ctrl->session_start_equity, sizeof(FPN<F>), 1, f);
  fwrite(&ctrl->peak_equity, sizeof(FPN<F>), 1, f);
  for (int i = 0; i < 5; i++) {
    fwrite(&ctrl->strategy_stats[i].realized_pnl, sizeof(FPN<F>), 1, f);
    fwrite(&ctrl->strategy_stats[i].wins, sizeof(uint32_t), 1, f);
    fwrite(&ctrl->strategy_stats[i].losses, sizeof(uint32_t), 1, f);
    fwrite(&ctrl->strategy_stats[i].total_trades, sizeof(uint32_t), 1, f);
  }

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

    // v8: starting_balance persisted (immune to config edits between sessions)
    if (version >= 8) {
      if (fread(&ctrl->config.starting_balance, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    }

    // v9: kill switch state + per-strategy stats + session equity baseline
    if (version >= 9) {
      if (fread(&ctrl->kill_switch_active, sizeof(int), 1, f) != 1) { fclose(f); return 0; }
      if (fread(&ctrl->kill_reason, sizeof(int), 1, f) != 1) { fclose(f); return 0; }
      if (fread(&ctrl->daily_realized_pnl, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
      if (version == 9) {
        // v9 stored equity fields as double (8 bytes) — convert to FPN
        double sse_d, pe_d;
        if (fread(&sse_d, sizeof(double), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&pe_d, sizeof(double), 1, f) != 1) { fclose(f); return 0; }
        ctrl->session_start_equity = FPN_FromDouble<F>(sse_d);
        ctrl->peak_equity = FPN_FromDouble<F>(pe_d);
      } else {
        // v10+: FPN fields
        if (fread(&ctrl->session_start_equity, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&ctrl->peak_equity, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
      }
      int n_strats = (version == 9) ? 4 : 5;
      for (int i = 0; i < n_strats; i++) {
        if (fread(&ctrl->strategy_stats[i].realized_pnl, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&ctrl->strategy_stats[i].wins, sizeof(uint32_t), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&ctrl->strategy_stats[i].losses, sizeof(uint32_t), 1, f) != 1) { fclose(f); return 0; }
        if (fread(&ctrl->strategy_stats[i].total_trades, sizeof(uint32_t), 1, f) != 1) { fclose(f); return 0; }
      }
      if (ctrl->kill_switch_active)
        fprintf(stderr, "[SNAPSHOT] kill switch ACTIVE (reason=%d) — trading halted. press 'k' to reset\n",
                ctrl->kill_reason);
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
