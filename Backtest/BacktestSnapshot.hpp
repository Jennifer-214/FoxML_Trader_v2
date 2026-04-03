// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BACKTEST SNAPSHOT]
//======================================================================================================
// populates TUISnapshot from backtest PortfolioController state.
// mirrors TUI_CopySnapshot (EngineTUI.hpp:777) — same field mappings,
// but takes explicit price/volume instead of DataStream, and uses
// memset zero as safety net for any missed fields.
//
// if you add a field to TUISnapshot, add it here too.
// missing fields show as 0 in the dashboard (visually obvious).
//======================================================================================================
#ifndef BACKTEST_SNAPSHOT_HPP
#define BACKTEST_SNAPSHOT_HPP

#include "../DataStream/EngineTUI.hpp"
#include "../CoreFrameworks/PortfolioController.hpp"
#include "../Strategies/RegimeDetector.hpp"

template <unsigned F>
static inline void BacktestSnapshot_Copy(TUISnapshot *snap,
                                          const PortfolioController<F> *ctrl,
                                          double price, double volume) {
    // zero first — any field we miss shows as 0 (visually obvious, never silently wrong)
    memset(snap, 0, sizeof(*snap));

    // market
    snap->price  = price;
    snap->volume = volume;

    // state
    snap->state_warmup = (ctrl->state == CONTROLLER_WARMUP);
    snap->is_paused = FPN_IsZero(ctrl->buy_conds.price) && !snap->state_warmup;
    snap->engine_state = ctrl->state;

    // rolling stats
    double avg = FPN_ToDouble(ctrl->rolling.price_avg);
    double slope = FPN_ToDouble(ctrl->rolling.price_slope);
    snap->roll_price_avg = avg;
    snap->roll_stddev    = FPN_ToDouble(ctrl->rolling.price_stddev);
    snap->roll_p_min     = FPN_ToDouble(ctrl->rolling.price_min);
    snap->roll_p_max     = FPN_ToDouble(ctrl->rolling.price_max);
    snap->roll_vol_avg   = FPN_ToDouble(ctrl->rolling.volume_avg);
    snap->roll_vol_slope = FPN_ToDouble(ctrl->rolling.volume_slope);
    snap->slope_pct      = (avg > 1e-15) ? (slope / avg) * 100.0 : 0.0;
    snap->roll_count     = ctrl->rolling.count;

    // long window
    if (ctrl->rolling_long) {
        double long_slope = FPN_ToDouble(ctrl->rolling_long->price_slope);
        double long_avg   = FPN_ToDouble(ctrl->rolling_long->price_avg);
        snap->long_slope_pct = (long_avg > 1e-15) ? (long_slope / long_avg) * 100.0 : 0.0;
        snap->long_count     = ctrl->rolling_long->count;
        snap->long_r2    = FPN_ToDouble(ctrl->rolling_long->price_r_squared);
    }

    // buy gate
    double buy_p = FPN_ToDouble(ctrl->buy_conds.price);
    snap->buy_p = buy_p;
    snap->buy_v = FPN_ToDouble(ctrl->buy_conds.volume);
    snap->gate_dist     = price - buy_p;
    snap->gate_dist_pct = (avg > 1e-15) ? (snap->gate_dist / avg) * 100.0 : 0.0;
    double spacing_val = FPN_ToDouble(RollingStats_EntrySpacing(&ctrl->rolling, ctrl->config.spacing_multiplier));
    snap->spacing     = spacing_val;
    snap->spacing_pct = (avg > 1e-15) ? (spacing_val / avg) * 100.0 : 0.0;
    snap->stddev_mode = !FPN_IsZero(ctrl->config.offset_stddev_mult);
    snap->gate_direction = ctrl->buy_conds.gate_direction;
    snap->live_offset = FPN_ToDouble(ctrl->mean_rev.live_offset_pct) * 100.0;
    snap->live_vmult  = FPN_ToDouble(ctrl->mean_rev.live_vol_mult);
    snap->live_sm     = FPN_ToDouble(ctrl->mean_rev.live_stddev_mult);
    snap->long_gate_enabled = !FPN_IsZero(ctrl->config.min_long_slope);
    double min_ls = FPN_ToDouble(ctrl->config.min_long_slope);
    snap->long_min_ls = min_ls;
    double long_avg_val = ctrl->rolling_long ? FPN_ToDouble(ctrl->rolling_long->price_avg) : 0.0;
    double long_slope_val = ctrl->rolling_long ? FPN_ToDouble(ctrl->rolling_long->price_slope) : 0.0;
    snap->long_rel_slope = (long_avg_val > 1e-15) ? long_slope_val / long_avg_val : 0.0;
    snap->long_gate_ok = !snap->long_gate_enabled || (snap->long_rel_slope >= min_ls);
    snap->buying_halted = ctrl->buying_halted;
    snap->halt_reason = ctrl->halt_reason;
    snap->gate_reason = ctrl->gate_reason;
    snap->fills_rejected = ctrl->fills_rejected;
    snap->last_reject_reason = ctrl->last_reject_reason;

    // portfolio + positions
    double fee_r = FPN_ToDouble(ctrl->config.fee_rate);
    snap->active_count = Portfolio_CountActive(&ctrl->portfolio);
    snap->max_positions = (int)ctrl->config.max_positions;
    snap->total_value = 0.0;
    snap->total_qty   = 0.0;

    uint16_t active = ctrl->portfolio.active_bitmap;
    for (int i = 0; i < 16; i++) snap->positions[i].idx = -1;
    while (active) {
        int idx = __builtin_ctz(active);
        const Position<F> *pos = &ctrl->portfolio.positions[idx];
        TUIPositionSnap *ps = &snap->positions[idx];
        ps->idx      = idx;
        ps->entry    = FPN_ToDouble(pos->entry_price);
        ps->qty      = FPN_ToDouble(pos->quantity);
        ps->tp       = FPN_ToDouble(pos->take_profit_price);
        ps->sl       = FPN_ToDouble(pos->stop_loss_price);
        ps->orig_tp  = FPN_ToDouble(pos->original_tp);
        ps->value    = price * ps->qty;
        ps->gross_pnl = (ps->entry > 1e-15) ? ((price - ps->entry) / ps->entry) * 100.0 : 0.0;
        ps->net_pnl   = ps->gross_pnl - (fee_r * 200.0);
        ps->is_trailing  = !FPN_Equal(pos->take_profit_price, pos->original_tp);
        ps->above_orig_tp = (price > ps->orig_tp) && (ps->entry > 1e-15);
        ps->ticks_held   = ctrl->total_ticks - ctrl->entry_ticks[idx];
        snap->total_value += ps->value;
        snap->total_qty   += ps->qty;
        active &= active - 1;
    }

    // financials
    double starting = FPN_ToDouble(ctrl->config.starting_balance);
    double balance  = FPN_ToDouble(ctrl->balance);
    snap->balance    = balance;
    snap->starting   = starting;
    snap->realized   = FPN_ToDouble(ctrl->realized_pnl);
    snap->unrealized = FPN_ToDouble(ctrl->portfolio_delta);
    snap->equity     = balance + snap->total_value;
    snap->total_pnl  = snap->equity - starting;
    snap->return_pct = (starting > 1e-15) ? (snap->total_pnl / starting) * 100.0 : 0.0;
    snap->exposure_pct = (starting > 1e-15) ? (snap->total_value / starting) * 100.0 : 0.0;
    snap->fees       = FPN_ToDouble(ctrl->total_fees);
    snap->fee_rate_pct = fee_r * 100.0;

    // regime
    snap->current_regime = ctrl->regime.current_regime;
    snap->strategy_id    = ctrl->strategy_id;
    snap->regime_auto    = (ctrl->config.default_strategy < 0);
    snap->regime_duration_min = 0.0; // no wall-clock in backtest
    snap->short_r2   = FPN_ToDouble(ctrl->rolling.price_r_squared);
    snap->ema_price  = FPN_ToDouble(ctrl->ema_price);
    // ROR slope
    snap->ror_slope  = 0.0;
    if (ctrl->regime_ror.count >= MAX_WINDOW) {
        LinearRegression3XResult<F> ror_r = RORRegressor_Compute(
            const_cast<RORRegressor<F>*>(&ctrl->regime_ror));
        snap->ror_slope = FPN_ToDouble(ror_r.model.slope);
    }
    // volume spike
    snap->volume_spike_ratio = FPN_ToDouble(ctrl->volume_spike_ratio);
    snap->spike_active = FPN_GreaterThanOrEqual(ctrl->volume_spike_ratio,
                                                 ctrl->config.spike_threshold);
    snap->vwap       = FPN_ToDouble(ctrl->rolling.vwap);
    snap->vwap_dev   = FPN_ToDouble(ctrl->rolling.vwap_deviation);
    snap->book_imbalance = FPN_ToDouble(ctrl->book_imbalance);
    snap->danger_score = FPN_ToDouble(ctrl->danger_score);
    snap->current_session = ctrl->current_session;
    snap->session_mult = FPN_ToDouble(ctrl->session_mult);
    snap->sl_cooldown = (int)ctrl->sl_cooldown_counter;
    snap->min_warmup_samples = (int)ctrl->config.min_warmup_samples;
    snap->kill_switch_active = ctrl->kill_switch_active;
    snap->kill_reason = ctrl->kill_reason;
    snap->kill_recovery = (int)ctrl->kill_recovery_counter;

    // EMA/SMA spread
    {
        double ema = FPN_ToDouble(ctrl->ema_price);
        double sma = FPN_ToDouble(ctrl->rolling.price_avg);
        snap->ema_sma_spread = (sma > 1e-15) ? (ema - sma) / sma : 0.0;
    }

    // variance ratio
    {
        double sv = FPN_ToDouble(ctrl->rolling.price_variance);
        double lv = ctrl->rolling_long ? FPN_ToDouble(ctrl->rolling_long->price_variance) : 1.0;
        snap->vol_ratio = (lv > 1e-15) ? sv / lv : 1.0;
    }

    // vol-scaled sizing
    snap->vol_scale = ctrl->last_vol_scale;

    // FoxML integration (Phase 6C) — single populate function
    MLSnapshot_Populate(&snap->ml, ctrl);

    // no-trade band
    {
        double bavg = FPN_ToDouble(ctrl->rolling.price_avg);
        double bprice = FPN_ToDouble(ctrl->buy_conds.price);
        snap->signal_strength = (bavg > 1e-15) ? fabs(bprice - bavg) / bavg * 100.0 : 0.0;
        double min_signal = FPN_ToDouble(ctrl->config.fee_rate) * FPN_ToDouble(ctrl->config.no_trade_band_mult) * 100.0;
        snap->no_trade_band_blocked = ctrl->config.no_trade_band_enabled &&
            (snap->signal_strength < min_signal) && !snap->state_warmup;
    }

    // per-strategy reward attribution
    for (int i = 0; i < 5; i++) {
        snap->strat_stats[i].pnl   = FPN_ToDouble(ctrl->strategy_stats[i].realized_pnl);
        snap->strat_stats[i].wins  = ctrl->strategy_stats[i].wins;
        snap->strat_stats[i].losses = ctrl->strategy_stats[i].losses;
        snap->strat_stats[i].total = ctrl->strategy_stats[i].total_trades;
    }

    // session stats
    snap->session_high = ctrl->session_high;
    snap->session_low = ctrl->session_low;

    // config display
    snap->cfg_tp  = FPN_ToDouble(ctrl->config.take_profit_pct) * 100.0;
    snap->cfg_sl  = FPN_ToDouble(ctrl->config.stop_loss_pct) * 100.0;
    snap->cfg_fee = fee_r * 100.0;
    snap->cfg_slippage = FPN_ToDouble(ctrl->config.slippage_pct) * 100.0;
    snap->live_trading = 0; // always paper in backtest
    snap->trailing_enabled = !FPN_IsZero(ctrl->config.tp_hold_score);
    snap->cfg_hold_score   = FPN_ToDouble(ctrl->config.tp_hold_score);
    snap->cfg_trail_mult   = FPN_ToDouble(ctrl->config.tp_trail_mult);
    snap->cfg_sl_trail_mult = FPN_ToDouble(ctrl->config.sl_trail_mult);
    snap->cfg_offset_val = snap->stddev_mode
        ? FPN_ToDouble(ctrl->config.offset_stddev_mult)
        : FPN_ToDouble(ctrl->config.entry_offset_pct) * 100.0;
    snap->risk_amt   = FPN_ToDouble(ctrl->config.risk_pct) * 100.0;
    snap->max_dd     = FPN_ToDouble(ctrl->config.max_drawdown_pct) * 100.0;
    snap->breaker_tripped = (snap->total_pnl < -(starting * FPN_ToDouble(ctrl->config.max_drawdown_pct)));

    // stats
    snap->total_buys = ctrl->total_buys;
    snap->wins       = ctrl->wins;
    snap->losses     = ctrl->losses;
    uint32_t total_exits = ctrl->wins + ctrl->losses;
    snap->win_rate      = (total_exits > 0) ? ((double)ctrl->wins / total_exits) * 100.0 : 0.0;
    double g_wins  = FPN_ToDouble(ctrl->gross_wins);
    double g_losses = FPN_ToDouble(ctrl->gross_losses);
    snap->profit_factor = (g_losses > 0.001) ? g_wins / g_losses : 0.0;
    snap->avg_win  = (ctrl->wins > 0)  ? g_wins / ctrl->wins : 0.0;
    snap->avg_loss = (ctrl->losses > 0) ? g_losses / ctrl->losses : 0.0;
    {
        double fee_per_exit = (total_exits > 0) ? FPN_ToDouble(ctrl->total_fees) / total_exits : 0.0;
        snap->avg_loss_market = (ctrl->losses > 0) ? snap->avg_loss - fee_per_exit : 0.0;
        if (snap->avg_loss_market < 0.0) snap->avg_loss_market = 0.0;
    }
    snap->avg_hold = (total_exits > 0)  ? (double)ctrl->total_hold_ticks / total_exits : 0.0;
    if (total_exits > 0) {
        double wr = (double)ctrl->wins / total_exits;
        double lr = (double)ctrl->losses / total_exits;
        snap->expectancy = (wr * snap->avg_win) - (lr * snap->avg_loss);
    }
    snap->max_drawdown = FPN_ToDouble(ctrl->max_drawdown);
    double pe = FPN_ToDouble(ctrl->peak_equity);
    snap->max_drawdown_pct = (pe > 0.0) ? (snap->max_drawdown / pe) * 100.0 : 0.0;
    snap->fee_ratio = (g_wins > 0.001) ?
        (FPN_ToDouble(ctrl->total_fees) / g_wins) * 100.0 : 0.0;
}

#endif // BACKTEST_SNAPSHOT_HPP
