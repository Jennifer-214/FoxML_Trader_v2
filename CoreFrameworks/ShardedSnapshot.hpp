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
    // v5.4.1 Bug B1: was never populated in sharded path → Account header
    // always showed fees: $0.00 even when OMS had accumulated fees.
    // Legacy EngineTUI.hpp path set this from ctrl->total_fees; the
    // sharded equivalent is oms->total_fees, populated by HandleFill on
    // entry+exit fills.
    snap->fees             = FPN_ToDouble(state->oms->total_fees);
    // v5.4.2 — same B1-class fix for the maker/taker breakdown
    // (used by the fees tooltip). OMS HandleFill bumps these counters
    // on every fill (BUY entry + SELL exit). Pre-fix, sharded mode
    // showed all-zeros in the maker/taker tooltip even after dozens of
    // fills.
    snap->maker_fills_count = state->oms->maker_fills_count;
    snap->taker_fills_count = state->oms->taker_fills_count;
    snap->total_maker_fees  = FPN_ToDouble(state->oms->total_maker_fees);
    snap->total_taker_fees  = FPN_ToDouble(state->oms->total_taker_fees);
    snap->return_pct   = (snap->starting > 0.0) ? (snap->total_pnl / snap->starting * 100.0) : 0.0;
    // active_count under partials: agg counts raw bitmap bits, but slot
    // 2c+0 + slot 2c+1 are ONE logical trade (both legs of one core's
    // pair). Collapse to "trades open" by counting cores with any leg
    // active. Standard "any-of-pair" trick: OR adjacent bits together,
    // mask to even positions, popcount. Handles half-paired states too
    // (e.g. leg A closed but leg B still open after TP1) — that core
    // counts as 1, not 0 (under-count) or 2 (raw bitmap).
    if (cfg->partial_exit_enabled) {
        uint16_t bm = state->oms->portfolio.active_bitmap;
        uint16_t any_pair = (uint16_t)(((bm | (bm >> 1)) & 0x5555u));
        snap->active_count = __builtin_popcount(any_pair);
    } else {
        snap->active_count = agg.active_position_count;
    }
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

    // v5.7.9: session indicator. The legacy single-core
    // PortfolioController computes current_session + session_mult
    // from UTC hour at the top of every slow-path cycle
    // (PortfolioController.hpp:1440-1443). The sharded port never
    // mirrored this — `snap->current_session` stayed at 0 (ASIA)
    // and `snap->session_mult` at 0.0 forever, hence the GUI's
    // stuck "ASIA (0.0x)" indicator. Computing here in the snapshot
    // copy is cheap (one time() syscall per GUI frame, ~60Hz) and
    // matches the legacy formula bit-for-bit. Mirror Class 2c —
    // sharding-port-orphan with display↔execution divergence.
    {
        time_t now = time(nullptr);
        struct tm utc;
        gmtime_r(&now, &utc);
        int h = utc.tm_hour;
        if (h < 7) {
            snap->current_session = 0;  // ASIA
            snap->session_mult    = FPN_ToDouble(cfg->session_asian_mult);
        } else if (h < 13) {
            snap->current_session = 1;  // EU
            snap->session_mult    = FPN_ToDouble(cfg->session_european_mult);
        } else if (h < 20) {
            snap->current_session = 2;  // US
            snap->session_mult    = FPN_ToDouble(cfg->session_us_mult);
        } else {
            snap->current_session = 3;  // OVERNIGHT
            snap->session_mult    = FPN_ToDouble(cfg->session_overnight_mult);
        }
    }

    // per-position details
    uint16_t bm = state->oms->portfolio.active_bitmap;
    double total_value = 0.0, total_qty = 0.0;
    // v4.7.6: wall-clock now in microseconds — used to compute
    // hold_minutes from the per-core last_entry_wall_us stamp.
    uint64_t now_wall_us = (uint64_t)
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    int partial_on = cfg->partial_exit_enabled ? 1 : 0;
    while (bm) {
        int idx = __builtin_ctz(bm);
        bm &= (uint16_t)(bm - 1);
        const Position<F>* pos = &state->oms->portfolio.positions[idx];
        TUIPositionSnap* ps = &snap->positions[idx];
        ps->idx      = idx;
        ps->entry    = FPN_ToDouble(pos->entry_price);
        ps->qty      = FPN_ToDouble(pos->quantity);

        // v5.4.0 Phase 4: GUI shows the SAME effective TP/SL the hot path
        // will exit at. Pre-Phase 4 the snapshot read pos->take_profit_price
        // / pos->stop_loss_price (postmortem F2 — dead writes, hot path
        // uses core->live_tp + cached_params.ratchet_tp instead). Now we
        // mirror SG_Evaluate's formula: effective = max(active, ratchet).
        // Falls back to pos->* when this slot's core isn't yet registered
        // (cold start) or when reading param_slot fails.
        int core_id_for_pos = (cfg->partial_exit_enabled ? (idx >> 1) : idx);
        bool resolved_effective = false;
        if (core_id_for_pos >= 0 && core_id_for_pos < state->registered_count) {
            tt::ExecutionCore<F>* xc = state->cores[core_id_for_pos].core;
            if (xc) {
                tt::GateParameters<F> params;
                tt::ParameterSlot_Read(&xc->param_slot, &params);
                // Leg-aware live levels: leg B (slot odd under partials)
                // uses live_tp_b, leg A uses live_tp.
                bool is_leg_b = cfg->partial_exit_enabled && (idx & 1);
                FPN<F> live_tp = is_leg_b ? xc->live_tp_b : xc->live_tp;
                FPN<F> live_sl = is_leg_b ? xc->live_sl_b : xc->live_sl;
                // active_tp/sl is the per-fill price when set, else the
                // cached params absolute. Same shape as ExecutionCore_Tick.
                FPN<F> active_tp = !FPN_IsZero(live_tp) ? live_tp : params.sg_take_profit_price;
                FPN<F> active_sl = !FPN_IsZero(live_sl) ? live_sl : params.sg_stop_loss_price;
                FPN<F> effective_tp = FPN_Max(active_tp, params.ratchet_tp);
                FPN<F> effective_sl = FPN_Max(active_sl, params.ratchet_sl);
                ps->tp = FPN_ToDouble(effective_tp);
                ps->sl = FPN_ToDouble(effective_sl);
                resolved_effective = true;
            }
        }
        if (!resolved_effective) {
            // Fallback for cold-start / no-core-registered: legacy display
            // matches what it used to show.
            ps->tp = FPN_ToDouble(pos->take_profit_price);
            ps->sl = FPN_ToDouble(pos->stop_loss_price);
        }
        ps->orig_tp  = FPN_ToDouble(pos->original_tp);
        ps->value    = price_d * ps->qty;
        if (ps->entry > 0.0) {
            ps->gross_pnl = ((price_d - ps->entry) / ps->entry) * 100.0;
            double fee_r = FPN_ToDouble(cfg->fee_rate);
            ps->net_pnl   = ps->gross_pnl - (fee_r * 200.0);
        }
        ps->is_trailing = (ps->tp != ps->orig_tp) ? 1 : 0;
        // v4.7.6: hold_minutes from per-core last_entry_wall_us. Both
        // legs of a paired trade share the same core's stamp (only leg A
        // stamps it on entry), so map slot → core_id and read from there.
        int core_id = partial_on ? (idx >> 1) : idx;
        if (core_id >= 0 && core_id < state->registered_count) {
            uint64_t entry_wall = state->cores[core_id].last_entry_wall_us;
            if (entry_wall > 0 && now_wall_us > entry_wall) {
                ps->hold_minutes = (double)(now_wall_us - entry_wall) / 60000000.0;
            } else {
                ps->hold_minutes = 0.0;
            }
            ps->entry_time = (time_t)(entry_wall / 1000000ULL);
        }
        total_value += ps->value;
        total_qty   += ps->qty;
    }
    snap->total_value = total_value;
    snap->total_qty   = total_qty;
    if (snap->starting > 0.0) {
        snap->exposure_pct = (total_value / snap->starting) * 100.0;
    }

    // v5.6.1 / v5.7.7-fix: bitmap consistency post-pass.
    // Compares the hot-path's any_active mask
    // (core->active | core->active_b) against the GUI's view from the
    // portfolio bitmap. The per-position loop above populated
    // snap->positions[].idx; now read core->active directly so we don't
    // depend on per-core-loop ordering (the v5.6.1 ship had a bug here
    // where the tentative byte was read BEFORE the per-core loop wrote
    // it, causing false DRIFT positives on cores 1/2/3 when permission
    // was off on core 0 — observed 2026-04-30 paper run).
    //
    // Under partials, core c owns slots {2c, 2c+1}. Without partials,
    // core c owns slot c. If masks disagree → Class 2c display↔execution
    // divergence; GUI surfaces as "DRIFT(bitmap)".
    //
    // Final value: 1 = consistent, 0 = drift detected.
    for (int c = 0; c < state->registered_count && c < 16; ++c) {
        tt::ExecutionCore<F>* xc = state->cores[c].core;
        bool hot_any_active = xc &&
            ((xc->active | xc->active_b) & 1) != 0;
        int slot_a = partial_on ? (c * 2)     : c;
        int slot_b = partial_on ? (c * 2 + 1) : -1;
        bool gui_any_pos = (slot_a >= 0 && slot_a < 16 && snap->positions[slot_a].idx >= 0)
                       || (slot_b >= 0 && slot_b < 16 && snap->positions[slot_b].idx >= 0);
        snap->per_core[c].bitmap_consistency =
            (hot_any_active == gui_any_pos) ? 1 : 0;
    }

    // counters
    snap->total_buys        = (uint32_t)agg.total_entries;
    snap->total_exits_fills = (uint32_t)agg.total_exits;  // per-fill heartbeat (leg fills)
    // v4.7.18: paper-reset sequence — caller fills this in (the engine
    // owns the TUISharedState that holds the live counter). Default 0
    // here so non-engine callers (tests) don't trip on uninit.
    snap->paper_reset_seq   = 0;

    // Bug fix (2026-04-27): aggregate per-core core_wins / core_losses
    // into snap->wins / snap->losses so the global Stats panel
    // (GUI_Panel_Stats reads s->wins + s->losses for total_exits) sees
    // real numbers in sharded mode. Pre-fix, snap->wins / losses stayed
    // at zero (only per_core[i].core_wins was populated downstream),
    // so the Stats panel showed buys=N exits=0 W=0 L=0 even when cores
    // had visibly racked up wins/losses in their per-core W/L column.
    //
    // With partial exits, each LEG exit counts as a separate win/loss
    // (HandleFill increments per-leg via OnEvent's mode-0 path).
    // A paired trade where leg A hits TP1 (win) and leg B hits SL (loss)
    // shows as 1 win + 1 loss = 50% win-rate. UX semantics for "trades
    // vs leg events" is a separate display question.
    uint32_t total_wins   = 0;
    uint32_t total_losses = 0;
    // v4.7.25: aggregate gross_wins / gross_losses across cores so the
    // sharded Stats panel can compute avg_win / avg_loss / profit_factor /
    // expectancy. Pre-v4.7.25 these fields stayed at zero in sharded mode
    // (only the legacy single_core path populated them) — visible in the
    // GUI as "avg W: $0.00 L: $0.00 E[trade]: $+0.00" even after dozens
    // of trades. Per-trade pairing semantics (v4.7.21) carry through:
    // a TP1+SL paired exit's NET is summed once and routed into the
    // matching gross bucket inside DrainPostFill.
    FPN<F> gross_wins   = FPN_Zero<F>();
    FPN<F> gross_losses = FPN_Zero<F>();
    for (int i = 0; i < state->registered_count && i < 16; ++i) {
        total_wins   += state->cores[i].core_wins;
        total_losses += state->cores[i].core_losses;
        gross_wins   = FPN_Add(gross_wins,   state->cores[i].core_gross_wins);
        gross_losses = FPN_Add(gross_losses, state->cores[i].core_gross_losses);
    }
    snap->wins   = total_wins;
    snap->losses = total_losses;
    if (total_wins + total_losses > 0) {
        snap->win_rate = (double)total_wins / (double)(total_wins + total_losses) * 100.0;
    } else {
        snap->win_rate = 0.0;
    }
    // v4.7.25: populate avg_win / avg_loss / profit_factor / expectancy
    // from the per-core gross accumulators. Mirrors TUI_CopySnapshot's
    // legacy formulas (line ~1233) so the Stats panel renders correctly
    // in BOTH modes.
    double g_wins_d   = FPN_ToDouble(gross_wins);
    double g_losses_d = FPN_ToDouble(gross_losses);
    snap->avg_win  = (total_wins   > 0) ? g_wins_d   / (double)total_wins   : 0.0;
    snap->avg_loss = (total_losses > 0) ? g_losses_d / (double)total_losses : 0.0;
    // v5.3.1 (Phase D): when no losses, profit_factor is mathematically
    // undefined (∞). Pre-fix code returned 0.0 which renders as "pf: 0.00"
    // — confusing for a strategy with all wins. Use -1.0 as a "no losses"
    // sentinel; Stats panel renders "—" for negative values.
    if (g_losses_d > 0.001) {
        snap->profit_factor = g_wins_d / g_losses_d;
    } else if (g_wins_d > 0.001) {
        snap->profit_factor = -1.0;  // sentinel: ∞ (all wins, no losses)
    } else {
        snap->profit_factor = 0.0;   // no trades at all
    }
    if (total_wins + total_losses > 0) {
        double tot = (double)(total_wins + total_losses);
        double wr = (double)total_wins   / tot;
        double lr = (double)total_losses / tot;
        snap->expectancy = (wr * snap->avg_win) - (lr * snap->avg_loss);
    } else {
        snap->expectancy = 0.0;
    }

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
    snap->partial_exit_enabled = cfg->partial_exit_enabled ? 1 : 0;
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
        // v5.6.2: strategy-internal halt reason (SHALT_*). Distinct from
        // halt_reason — set by strategy _BuildParameters when zero-gating
        // for strategy-specific reasons (no uptrend, fee-floor BUY_BLOCKED,
        // ML below threshold, etc).
        snap->per_core[i].strategy_halt_reason   = state->cores[i].strategy_halt_reason;
        // v5.6.3: copy gate diagnostic comparands. Captured by the
        // controller's gate checks; converted FPN<F> → double here.
        snap->per_core[i].diag_spacing_actual    = FPN_ToDouble(state->cores[i].diag_spacing_actual);
        snap->per_core[i].diag_spacing_floor     = FPN_ToDouble(state->cores[i].diag_spacing_floor);
        snap->per_core[i].diag_vwap_actual       = FPN_ToDouble(state->cores[i].diag_vwap_actual);
        snap->per_core[i].diag_vwap_threshold    = FPN_ToDouble(state->cores[i].diag_vwap_threshold);
        snap->per_core[i].diag_long_slope        = FPN_ToDouble(state->cores[i].diag_long_slope);
        snap->per_core[i].diag_long_slope_min    = FPN_ToDouble(state->cores[i].diag_long_slope_min);
        snap->per_core[i].diag_volume_delta      = FPN_ToDouble(state->cores[i].diag_volume_delta);
        snap->per_core[i].diag_volume_delta_min  = FPN_ToDouble(state->cores[i].diag_volume_delta_min);
        snap->per_core[i].diag_stddev_pct        = FPN_ToDouble(state->cores[i].diag_stddev_pct);
        snap->per_core[i].diag_stddev_pct_min    = FPN_ToDouble(state->cores[i].diag_stddev_pct_min);
        snap->per_core[i].diag_tp_pct_actual     = FPN_ToDouble(state->cores[i].diag_tp_pct_actual);
        snap->per_core[i].diag_tp_pct_floor      = FPN_ToDouble(state->cores[i].diag_tp_pct_floor);
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
        // Phase 2.1: per-core open notional + budget-used %. The % is
        // computed defensively — if allocated is zero or near-zero (rare
        // misconfiguration), report 0% rather than a divide-by-zero blowup.
        double open_n  = FPN_ToDouble(state->cores[i].core_open_notional);
        double alloc_d = FPN_ToDouble(state->cores[i].allocated_balance);
        snap->per_core[i].core_open_notional = open_n;
        snap->per_core[i].core_budget_used_pct = (alloc_d > 0.01) ? (open_n / alloc_d * 100.0) : 0.0;
        // Phase 3: per-core kill switch state for the Risk panel
        snap->per_core[i].core_peak_balance    = FPN_ToDouble(state->cores[i].core_peak_balance);
        snap->per_core[i].core_dd_pct          = FPN_ToDouble(state->cores[i].core_dd_pct);
        snap->per_core[i].core_ks_trips_total  = state->cores[i].core_ks_trips_total;
        snap->per_core[i].core_kill_tripped    = state->cores[i].core_kill_tripped;
        tt::ExecutionCore<F>* core = state->cores[i].core;
        if (core) {
            tt::GateParameters<F> params;
            tt::ParameterSlot_Read(&core->param_slot, &params);
            snap->per_core[i].buy_gate_price = FPN_ToDouble(params.bg_price_threshold);
            // v5.6.0: snapshot the flags byte so GUI can render BUY_BLOCKED /
            // VOLUME_REQUIRED / TP/SL ENABLED / BUY_ABOVE / PAIR_ACTIVE without
            // needing access to GateParameters internals. ParameterSlot_Read
            // is seqlock-published so flags + thresholds are consistent.
            snap->per_core[i].gate_flags = params.flags;
            // v5.6.1: bg_volume_threshold for collapsing-header readout. Only
            // meaningful when GATE_FLAG_VOLUME_REQUIRED is set, but copying
            // unconditionally is cheaper than branching.
            snap->per_core[i].bg_volume_threshold =
                FPN_ToDouble(params.bg_volume_threshold);
            // v5.6.1: permission atomic snapshot. ACQUIRE load matches the
            // hot-path read in ExecutionCore.hpp:356, so we see the same
            // state the next tick would see. 0 = entries forbidden.
            snap->per_core[i].permission = (uint8_t)__atomic_load_n(
                &core->permission, __ATOMIC_ACQUIRE);
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
