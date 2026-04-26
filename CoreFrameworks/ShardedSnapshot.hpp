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
#include "../ML_Headers/ConfidenceScore.hpp"
#include "../ML_Headers/CoreModelZoo.hpp"
#include "ControllerEventLoop.hpp"
#include "EventLoopAggregates.hpp"

#include <cmath>

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

    // v4.0.4: warmup progress display. min_warmup_samples is what the
    // warmup gate at EngineSharded.hpp checks; warmup_samples_now is
    // the current rolling count. state_warmup = (now < target).
    snap->min_warmup_samples = (int)cfg->min_warmup_samples;
    if (snap->min_warmup_samples <= 0) snap->min_warmup_samples = 64;  // engine default
    snap->warmup_samples_now = rolling->count;
    snap->state_warmup = (snap->warmup_samples_now < snap->min_warmup_samples) ? 1 : 0;

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
    snap->cfg_slippage = FPN_ToDouble(cfg->slippage_pct) * 100.0;
    snap->live_trading = cfg->use_real_money;

    // per-core details (strategy assignment + buy gate levels).
    // use core 0's strategy as the "headline" strategy for the Market panel,
    // and core 0's gate parameters for the Buy Gate panel.
    snap->sharded_mode_active = 1;
    snap->per_core_count = state->registered_count;
    if (state->registered_count > 0) {
        // v4.0.4: use core 0's RESOLVED strategy as headline. For AUTO core 0
        // this gives the regime-resolved concrete strategy; for static cores
        // it equals strategy_id. Avoids showing "AUTO" raw which isn't a
        // real strategy the Market panel can label.
        uint8_t headline_sid = (state->cores[0].resolved_strategy_id != STRATEGY_NONE)
                                ? state->cores[0].resolved_strategy_id
                                : state->cores[0].strategy_id;
        snap->strategy_id = headline_sid;
        // v4.0.4: regime headline. Pick the regime from the first AUTO
        // core if any exist (its hysteresis state IS the regime). Otherwise
        // compute fresh from rolling stats so the panel isn't stuck on
        // RANGING. Each AUTO core has its own state — they may differ
        // briefly under hysteresis, but core 0 (or first AUTO) is the
        // "headline" for display purposes.
        int headline_regime = REGIME_RANGING;  // default
        for (int i = 0; i < state->registered_count && i < 16; ++i) {
            if (state->cores[i].strategy_id == STRATEGY_AUTO) {
                headline_regime = state->cores[i].regime_state.current_regime;
                break;
            }
        }
        snap->current_regime = headline_regime;
    }
    // Phase 6prep sharded c16: ML aggregation across per-core scorers. We
    // pick the highest-confidence ML core to populate the headline `s->ml.*`
    // fields (which the existing GUI_Panel_MLIntelligence already reads).
    // Per-core detail goes into per_core[i].ml_* for the per-core panel.
    int    headline_ml_core = -1;
    double headline_conf    = -1.0;
    int    any_ml_active    = 0;
    int    any_model_loaded = 0;

    for (int i = 0; i < state->registered_count && i < 16; ++i) {
        snap->per_core[i].strategy_id_display = state->cores[i].strategy_id;
        // v4.0.4: resolved strategy after AUTO regime classification. For
        // non-AUTO cores this equals strategy_id_display.
        snap->per_core[i].resolved_strategy_id = state->cores[i].resolved_strategy_id;
        // Per-core gate direction. Use RESOLVED strategy for AUTO so direction
        // tracks the active regime's strategy. MOMENTUM buys above; everything
        // else buys below.
        uint8_t dir_strat = (state->cores[i].resolved_strategy_id != STRATEGY_NONE)
                              ? state->cores[i].resolved_strategy_id
                              : state->cores[i].strategy_id;
        snap->per_core[i].gate_direction = (dir_strat == STRATEGY_MOMENTUM) ? 1 : 0;
        // v4.0.4: per-core diagnostic state for Buy Gate panel
        snap->per_core[i].halt_reason            = state->cores[i].halt_reason;
        snap->per_core[i].sl_cooldown_remaining  = state->cores[i].sl_cooldown_remaining;
        // v4.0.4: per-core P&L for Account panel breakdown
        snap->per_core[i].core_realized      = FPN_ToDouble(state->cores[i].core_realized);
        snap->per_core[i].core_fees          = FPN_ToDouble(state->cores[i].core_fees);
        snap->per_core[i].core_allocated     = FPN_ToDouble(state->cores[i].allocated_balance);
        snap->per_core[i].core_wins          = state->cores[i].core_wins;
        snap->per_core[i].core_losses        = state->cores[i].core_losses;
        // open positions = entries minus exits (single-position-per-core invariant
        // means this is 0 or 1 today, but kept generic for future multi-position).
        uint64_t entries = state->cores[i].entries_processed;
        uint64_t exits   = state->cores[i].exits_processed;
        snap->per_core[i].core_open_positions = (uint32_t)(entries - exits);
        tt::ExecutionCore<F>* core = state->cores[i].core;
        if (core) {
            tt::GateParameters<F> params;
            tt::ParameterSlot_Read(&core->param_slot, &params);
            snap->per_core[i].buy_gate_price = FPN_ToDouble(params.bg_price_threshold);
            // populate headline buy gate from core 0
            if (i == 0) {
                snap->buy_p = FPN_ToDouble(params.bg_price_threshold);
                snap->buy_v = FPN_ToDouble(params.bg_volume_threshold);
                if (snap->buy_p > 0.01 && price_d > 0.01) {
                    snap->gate_dist = price_d - snap->buy_p;
                    snap->gate_dist_pct = (snap->gate_dist / price_d) * 100.0;
                }
            }
        }

        // Phase 6prep sharded c16: per-core ML observability
        if (state->cores[i].strategy_id == STRATEGY_ML) {
            snap->per_core[i].is_ml = 1;
            any_ml_active = 1;
            CoreModelZoo<F>* zoo = (CoreModelZoo<F>*)state->cores[i].model_handle;
            int loaded = (zoo && CoreModelZoo_HasAny(zoo)) ? 1 : 0;
            snap->per_core[i].ml_model_loaded = (uint8_t)loaded;
            if (loaded) any_model_loaded = 1;
            // staged_prediction is the freshest rebuild output; active_prediction
            // is the snapshot at last entry submit (0 if no open position).
            snap->per_core[i].ml_last_prediction   = state->cores[i].staged_prediction;
            snap->per_core[i].ml_last_confidence   = state->cores[i].last_confidence;
            snap->per_core[i].ml_active_prediction = state->cores[i].active_prediction;
            // Direct reads of scorer internals — these are double-only and safe
            // to compute on the snapshot path (snapshot is slow-path itself).
            snap->per_core[i].ml_confidence_ic   = RollingIC_Compute(&state->cores[i].confidence.ic);
            snap->per_core[i].ml_confidence_rmse = RollingRMSE_Compute(&state->cores[i].confidence.rmse);
            // Track the highest-confidence ML core for the headline summary.
            // Tie-break: prefer the lowest core index (deterministic).
            if (state->cores[i].last_confidence > headline_conf) {
                headline_conf = state->cores[i].last_confidence;
                headline_ml_core = i;
            }
        }
    }

    // Phase 6prep sharded c16: populate s->ml.* from the headline ML core.
    // GUI_Panel_MLIntelligence renders this single-core view; per-core detail
    // goes through the new per-core section.
    snap->ml.confidence_enabled = cfg->confidence_enabled ? 1 : 0;
    snap->ml.ml_model_loaded    = any_model_loaded;
    if (headline_ml_core >= 0) {
        const auto& core_pc = snap->per_core[headline_ml_core];
        snap->ml.confidence            = core_pc.ml_last_confidence;
        snap->ml.confidence_ic         = core_pc.ml_confidence_ic;
        snap->ml.confidence_rmse       = core_pc.ml_confidence_rmse;
        // Stability ≈ exp(-rmse) per Confidence_Stability; the GUI labels
        // this as "Stability" not "Freshness" despite the field name (legacy).
        snap->ml.confidence_freshness  = (core_pc.ml_confidence_rmse > 0.0)
                                          ? std::exp(-core_pc.ml_confidence_rmse)
                                          : 1.0;
        snap->ml.ml_last_prediction    = core_pc.ml_last_prediction;
    }
    (void)any_ml_active;
}
