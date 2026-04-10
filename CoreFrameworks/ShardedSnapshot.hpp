// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [SHARDED SNAPSHOT COPY]
//
// Populates TUISnapshot from the sharded engine's EventLoopState + OMS.
// Called from the producer thread's slow-path cadence in EngineSharded.hpp.
// Maps the same TUISnapshot fields the GUI panels read, just from different
// source structs (EventLoopState instead of PortfolioController).
//
// Lives in its own header because it depends on both EngineTUI.hpp (for
// TUISnapshot) and ControllerEventLoop.hpp (for EventLoopState). Included
// by EngineSharded.hpp after both are available.
//
// Adding a new display field for sharded mode:
//   1. populate it here from the appropriate source
//   2. the GUI panel already reads it from snap->field_name
//   done — one site to update, panels work unchanged
//======================================================================================================

#pragma once

#include "../DataStream/EngineTUI.hpp"
#include "ControllerEventLoop.hpp"
#include "EventLoopAggregates.hpp"

// TUISnapshot, TUIPositionSnap, RollingStats, ControllerConfig, Position
// are all in the global namespace. EventLoopState, EventLoopAggregates,
// Portfolio are in namespace tt.

template <unsigned F, unsigned W = 128, unsigned WL = 512>
static inline void TUI_CopySnapshotSharded(
    TUISnapshot *snap,
    const tt::EventLoopState<F>* state,
    const RollingStats<F, W>* rolling,
    const RollingStats<F, WL>* rolling_long,
    const ControllerConfig<F>* cfg,
    double price_d, double volume_d)
{
    memset(snap, 0, sizeof(*snap));
    // mark all position slots as empty so the GUI doesn't render them
    for (int i = 0; i < 16; ++i) snap->positions[i].idx = -1;

    // market data
    snap->price  = price_d;
    snap->volume = volume_d;

    // rolling stats — short window
    if (rolling->count > 0) {
        snap->roll_price_avg = FPN_ToDouble(rolling->price_avg);
        snap->roll_stddev    = FPN_ToDouble(rolling->price_stddev);
        snap->roll_p_min     = FPN_ToDouble(rolling->price_min);
        snap->roll_p_max     = FPN_ToDouble(rolling->price_max);
        snap->roll_vol_avg   = FPN_ToDouble(rolling->volume_avg);
        snap->roll_count     = rolling->count;
        double avg = snap->roll_price_avg;
        if (avg > 0.0) {
            snap->slope_pct = FPN_ToDouble(rolling->price_slope) / avg * 100.0;
        }
        snap->short_r2 = FPN_ToDouble(rolling->price_r_squared);
    }

    // rolling stats — long window
    if (rolling_long && rolling_long->count > 0) {
        snap->long_count = rolling_long->count;
        double avg_l = FPN_ToDouble(rolling_long->price_avg);
        if (avg_l > 0.0) {
            snap->long_slope_pct = FPN_ToDouble(rolling_long->price_slope) / avg_l * 100.0;
        }
        snap->long_r2 = FPN_ToDouble(rolling_long->price_r_squared);
    }

    // account — from OMS via EventLoopAggregates
    tt::EventLoopAggregates agg = tt::EventLoop_GetAggregates(state, FPN_FromDouble<F>(price_d));
    snap->balance      = agg.balance;
    snap->equity       = agg.equity;
    snap->starting     = FPN_ToDouble(cfg->starting_balance);
    snap->realized     = agg.realized_pnl;
    snap->unrealized   = agg.unrealized_pnl;
    snap->total_pnl    = agg.realized_pnl + agg.unrealized_pnl;
    snap->return_pct   = (snap->starting > 0.0) ? (snap->total_pnl / snap->starting * 100.0) : 0.0;
    snap->active_count = agg.active_position_count;
    snap->max_positions = (int)cfg->num_execution_cores;
    snap->max_dd       = agg.max_drawdown;
    snap->max_drawdown = agg.max_drawdown;
    snap->max_drawdown_pct = agg.max_drawdown_pct * 100.0;
    snap->fee_rate_pct = FPN_ToDouble(cfg->fee_rate) * 100.0;

    // kill switch
    snap->kill_switch_active = agg.kill_switch_tripped;
    snap->breaker_tripped    = agg.kill_switch_tripped;

    // per-position details
    uint16_t bm = state->oms->portfolio.active_bitmap;
    double total_value = 0.0, total_qty = 0.0;
    while (bm) {
        int idx = __builtin_ctz(bm);
        bm &= (uint16_t)(bm - 1);
        const Position<F>* pos = &state->oms->portfolio.positions[idx];
        TUIPositionSnap* ps = &snap->positions[idx];
        ps->idx      = idx;
        ps->entry    = FPN_ToDouble(pos->entry_price);
        ps->qty      = FPN_ToDouble(pos->quantity);
        ps->tp       = FPN_ToDouble(pos->take_profit_price);
        ps->sl       = FPN_ToDouble(pos->stop_loss_price);
        ps->orig_tp  = FPN_ToDouble(pos->original_tp);
        ps->value    = price_d * ps->qty;
        if (ps->entry > 0.0) {
            ps->gross_pnl = ((price_d - ps->entry) / ps->entry) * 100.0;
            double fee_r = FPN_ToDouble(cfg->fee_rate);
            ps->net_pnl   = ps->gross_pnl - (fee_r * 200.0);
        }
        ps->is_trailing = (ps->tp != ps->orig_tp) ? 1 : 0;
        total_value += ps->value;
        total_qty   += ps->qty;
    }
    snap->total_value = total_value;
    snap->total_qty   = total_qty;
    if (snap->starting > 0.0) {
        snap->exposure_pct = (total_value / snap->starting) * 100.0;
    }

    // counters
    snap->total_buys = (uint32_t)agg.total_entries;

    // config display
    snap->cfg_tp  = FPN_ToDouble(cfg->take_profit_pct) * 100.0;
    snap->cfg_sl  = FPN_ToDouble(cfg->stop_loss_pct) * 100.0;
    snap->cfg_fee = FPN_ToDouble(cfg->fee_rate) * 100.0;
    snap->live_trading = cfg->use_real_money;

    // per-core details (strategy assignment + buy gate levels)
    snap->sharded_mode_active = 1;
    snap->per_core_count = state->registered_count;
    for (int i = 0; i < state->registered_count && i < 16; ++i) {
        snap->per_core[i].strategy_id_display = state->cores[i].strategy_id;
        // read buy gate price from the core's parameter slot
        tt::ExecutionCore<F>* core = state->cores[i].core;
        if (core) {
            tt::GateParameters<F> params;
            tt::ParameterSlot_Read(&core->param_slot, &params);
            snap->per_core[i].buy_gate_price = FPN_ToDouble(params.bg_price_threshold);
        }
    }
}
