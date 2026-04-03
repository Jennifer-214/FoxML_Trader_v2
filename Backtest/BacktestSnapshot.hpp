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
    snap->gate_direction = ctrl->buy_conds.gate_direction;
    snap->buying_halted = ctrl->buying_halted;
    snap->halt_reason = ctrl->halt_reason;
    snap->gate_reason = ctrl->gate_reason;

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
    snap->short_r2   = FPN_ToDouble(ctrl->rolling.price_r_squared);
    snap->ema_price  = FPN_ToDouble(ctrl->ema_price);
    snap->vwap       = FPN_ToDouble(ctrl->rolling.vwap);
    snap->vwap_dev   = FPN_ToDouble(ctrl->rolling.vwap_deviation);
    snap->danger_score = FPN_ToDouble(ctrl->danger_score);
    snap->sl_cooldown = (int)ctrl->sl_cooldown_counter;

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

    // FoxML integration (Phase 6C)
    snap->cost_bps = ctrl->last_cost_bps;
    snap->foxml_vol_scale = ctrl->foxml_vol_scale;
    snap->confidence = ctrl->last_confidence;
    snap->cost_gate_enabled = ctrl->config.cost_gate_enabled;
    snap->foxml_vol_scaling_enabled = ctrl->config.foxml_vol_scaling_enabled;
    snap->confidence_enabled = ctrl->config.confidence_enabled;
    snap->bandit_enabled = ctrl->config.bandit_enabled;
    if (ctrl->config.bandit_enabled) {
        snap->bandit_blend = Bandit_EffectiveBlend(&ctrl->bandit);
        snap->bandit_active = (ctrl->bandit.total_steps >= ctrl->bandit.min_samples) ? 1 : 0;
        double bw[BANDIT_MAX_ARMS];
        Bandit_GetWeights(&ctrl->bandit, bw);
        for (int i = 0; i < 5; i++) snap->bandit_weights[i] = bw[i];
    }

    // config display
    snap->cfg_tp  = FPN_ToDouble(ctrl->config.take_profit_pct) * 100.0;
    snap->cfg_sl  = FPN_ToDouble(ctrl->config.stop_loss_pct) * 100.0;
    snap->cfg_fee = fee_r * 100.0;
    snap->cfg_slippage = FPN_ToDouble(ctrl->config.slippage_pct) * 100.0;
    snap->live_trading = 0; // always paper in backtest
    snap->trailing_enabled = !FPN_IsZero(ctrl->config.tp_hold_score);

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
    snap->avg_hold = (total_exits > 0)  ? (double)ctrl->total_hold_ticks / total_exits : 0.0;
    if (total_exits > 0) {
        double wr = (double)ctrl->wins / total_exits;
        double lr = (double)ctrl->losses / total_exits;
        snap->expectancy = (wr * snap->avg_win) - (lr * snap->avg_loss);
    }
    snap->fee_ratio = (g_wins > 0.001) ?
        (FPN_ToDouble(ctrl->total_fees) / g_wins) * 100.0 : 0.0;
}

#endif // BACKTEST_SNAPSHOT_HPP
