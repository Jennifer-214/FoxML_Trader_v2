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
#include "../MemHeaders/FailureModeRegistry.hpp"  // v5.14.8.C — FAILURE_SET / FAILURE_IS_SET
#include "../MemHeaders/PerCoreStateFlagsRegistry.hpp"  // v5.14.9.B.2 — STATE_FLAG_SET / IS_SET
#include "SlowPathGateRegistry.hpp"  // v5.14.9.B.2 — MASK_LADDER_ACTIVE for ladder-bottom inference
#include "ControllerEventLoop.hpp"
#include "EventLoopAggregates.hpp"
#include "MetricCompute.hpp"  // v5.8.4c: shared metric helpers

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
    // v5.11.4.B — surface async log writer health (parity-check Section J).
    // Both counters are atomic on the writer-thread side; relaxed loads on
    // the publish thread are fine — these are advisory observability metrics,
    // not transactional state.
    snap->oms_log_ring_full_spins =
        state->oms->event_log.ring_full_spins.load(std::memory_order_relaxed);
    snap->oms_log_writer_realloc_failed =
        state->oms->event_log.writer_realloc_failed_count.load(std::memory_order_relaxed);
    // v5.11.5.D — closes parity-check J.1: log_full_drops was added in
    // v5.11.5.C but not surfaced. Sibling counter to ring_full_spins; both
    // signal async-log-writer distress (former = ring saturation, latter =
    // mmap capacity exhausted).
    snap->oms_log_full_drops =
        state->oms->event_log.log_full_drops.load(std::memory_order_relaxed);
    snap->return_pct   = (snap->starting > 0.0) ? (snap->total_pnl / snap->starting * 100.0) : 0.0;
    // active_count under partials: agg counts raw bitmap bits, but slot
    // 2c+0 + slot 2c+1 are ONE logical trade (both legs of one core's
    // pair). Collapse to "trades open" by counting cores with any leg
    // active. Standard "any-of-pair" trick: OR adjacent bits together,
    // mask to even positions, popcount. Handles half-paired states too
    // (e.g. leg A closed but leg B still open after TP1) — that core
    // counts as 1, not 0 (under-count) or 2 (raw bitmap).
    if (BITMAP_IS_SET(cfg->lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED)) {
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

    // v5.9.0c — capture cfg path for engine header panel
    {
        size_t n = strlen(cfg->source_cfg_path);
        if (n >= sizeof(snap->source_cfg_path)) n = sizeof(snap->source_cfg_path) - 1;
        memcpy(snap->source_cfg_path, cfg->source_cfg_path, n);
        snap->source_cfg_path[n] = '\0';
    }

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
        // v5.15.5.B.5 — branchless SESSION_BY_HOUR[24] table lookup
        // (FOREACH_SESSION_PHASE registry). Replaces 4-way if/else; one
        // memory load + one indirection vs ~4 branches pre-.B.5.
        const double session_mult_lookup[tt::SESSION_PHASE_COUNT] = {
#define X(NAME_U, name_l, START, END, MULT, DOC) FPN_ToDouble(cfg->session_##name_l##_mult),
            FOREACH_SESSION_PHASE(X)
#undef X
        };
        int h = utc.tm_hour;
        if (h < 0) h = 0;
        if (h > 23) h = 23;
        uint8_t phase = tt::SESSION_BY_HOUR[h];
        snap->current_session = (int)phase;
        snap->session_mult    = session_mult_lookup[phase];
    }

    // per-position details
    uint16_t bm = state->oms->portfolio.active_bitmap;
    double total_value = 0.0, total_qty = 0.0;
    // v4.7.6: wall-clock now in microseconds — used to compute
    // hold_minutes from the per-core last_entry_wall_us stamp.
    uint64_t now_wall_us = (uint64_t)
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    int partial_on = BITMAP_IS_SET(cfg->lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED) ? 1 : 0;
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
        int core_id_for_pos = (BITMAP_IS_SET(cfg->lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED) ? (idx >> 1) : idx);
        bool resolved_effective = false;
        if (core_id_for_pos >= 0 && core_id_for_pos < state->registered_count) {
            tt::ExecutionCore<F>* xc = state->cores[core_id_for_pos].core;
            if (xc) {
                tt::GateParameters<F> params;
                tt::ParameterSlot_Read(&xc->param_slot, &params);
                // Leg-aware live levels: leg B (slot odd under partials)
                // uses live_tp_b, leg A uses live_tp.
                bool is_leg_b = BITMAP_IS_SET(cfg->lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED) && (idx & 1);
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
        // v5.11.65 — prefer Position.entry_timestamp_us (per-slot, persisted
        // in snapshot via the Position struct dump → survives engine restart).
        // Fall back to CoreContext.last_entry_wall_us (per-core, NOT persisted)
        // for in-memory state that hasn't been re-entered yet under v5.11.65.
        // Pre-fix: hold display always read last_entry_wall_us, which reset
        // to 0 on every restart → Hold column showed "0m" forever for
        // positions loaded from snapshot.
        uint64_t entry_wall = state->oms->portfolio.positions[idx].entry_timestamp_us;
        if (entry_wall == 0) {
            int core_id = partial_on ? (idx >> 1) : idx;
            if (core_id >= 0 && core_id < state->registered_count) {
                entry_wall = state->cores[core_id].last_entry_wall_us;
            }
        }
        if (entry_wall > 0 && now_wall_us > entry_wall) {
            ps->hold_minutes = (double)(now_wall_us - entry_wall) / 60000000.0;
        } else {
            ps->hold_minutes = 0.0;
        }
        ps->entry_time = (time_t)(entry_wall / 1000000ULL);
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
    // portfolio bitmap. v5.15.5.B.8 — loop body merged into the unified
    // per-core loop below; state_flags reset + BITMAP_CONSISTENT bit set
    // at the top of the merged body before any other STATE_FLAG_SET.

    // counters
    snap->total_buys        = (uint32_t)agg.total_entries;
    snap->total_exits_fills = (uint32_t)agg.total_exits;  // per-fill heartbeat (leg fills)
    // v4.7.18: paper-reset sequence — caller fills this in (the engine
    // owns the TUISharedState that holds the live counter). Default 0
    // here so non-engine callers (tests) don't trip on uninit.
    snap->paper_reset_seq   = 0;

    // v5.15.5.B.8 — Per-core aggregates + headline_regime/regime_set flag +
    // ML headline state declared here so they're in scope for the unified
    // per-core loop below + the post-loop publishing block. Originally
    // declared between Loops 2/3/4; hoisted to make the loop-consolidation
    // boundary cleaner. Same defaults (zero / REGIME_RANGING / -1 / false)
    // as the pre-.B.8 declarations.
    //
    // wins/losses + gross accumulators (Bug fix 2026-04-27 + v4.7.25):
    //   aggregate per-core wins/losses + gross win/loss buckets into
    //   snap->wins / snap->losses / snap->avg_win / snap->avg_loss /
    //   snap->profit_factor / snap->expectancy. Per-trade pairing semantics
    //   (v4.7.21/26 partner pending + gross accumulators) carry through
    //   inside DrainPostFill — this loop just sums the per-core results.
    // headline_regime (v4.0.4):
    //   first AUTO core's regime_state.current_regime; falls back to
    //   REGIME_RANGING if no AUTO cores. Used by Market panel display.
    // ML headline state (Phase 6prep sharded c16):
    //   tracks highest-confidence ML core to populate snap->ml.* headline
    //   fields read by GUI_Panel_MLIntelligence.
    uint32_t total_wins   = 0;
    uint32_t total_losses = 0;
    FPN<F>   gross_wins   = FPN_Zero<F>();
    FPN<F>   gross_losses = FPN_Zero<F>();
    int      headline_regime     = REGIME_RANGING;
    bool     headline_regime_set = false;
    int      headline_ml_core    = -1;
    double   headline_conf       = -1.0;
    int      any_ml_active       = 0;
    int      any_model_loaded    = 0;

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
    snap->partial_exit_enabled = BITMAP_IS_SET(cfg->lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED) ? 1 : 0;
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
        // v5.15.5.B.8 — headline_regime AUTO-finder loop merged into the
        // unified per-core loop below. snap->current_regime assignment moved
        // to the post-loop publishing block (still guarded by registered_count > 0).
    }

    // ============================================================================
    // v5.15.5.B.8 — UNIFIED PER-CORE LOOP (T1 audit win)
    // ============================================================================
    // Consolidates 4 previously-separate walks over state->cores[]:
    //   (1) bitmap consistency  — state_flags reset + BITMAP_CONSISTENT bit
    //   (2) wins/losses + gross — total_wins/losses + gross_wins/losses accum
    //   (3) headline_regime     — first AUTO match wins (flag-driven, no break)
    //   (4) per_core publisher  — strategy_id_display / halt_reason / diag_* /
    //                              ML telemetry / drift / kill / etc.
    // Single walk over state->cores[i] per snapshot publish. Saves ~3 walks
    // × ~7 KB per CoreContext × 16 cores × 60 Hz = ~20 MB/s memory bandwidth
    // (audit synthesis line 96-101). PerCoreSnap output bytewise-identical
    // post-consolidation (verified via 3027-test regression).
    // ============================================================================
    for (int i = 0; i < state->registered_count && i < 16; ++i) {
        // ---- (was Loop 1) bitmap consistency — state_flags reset + bit ----
        // State_flags reset MUST happen here BEFORE any STATE_FLAG_SET below.
        // v5.6.1 / v5.7.7-fix invariant: hot any_active mask (core->active |
        // active_b) compared against snap->positions[] GUI view; mismatch →
        // DRIFT(bitmap). Under partials, core c owns slots {2c, 2c+1};
        // without partials, core c owns slot c.
        {
            tt::ExecutionCore<F>* xc = state->cores[i].core;
            bool hot_any_active = xc &&
                ((xc->active | xc->active_b) & 1) != 0;
            int slot_a = partial_on ? (i * 2)     : i;
            int slot_b = partial_on ? (i * 2 + 1) : -1;
            bool gui_any_pos = (slot_a >= 0 && slot_a < 16 && snap->positions[slot_a].idx >= 0)
                           || (slot_b >= 0 && slot_b < 16 && snap->positions[slot_b].idx >= 0);
            snap->per_core[i].state_flags = 0;
            if (hot_any_active == gui_any_pos) {
                STATE_FLAG_SET(snap->per_core[i], BITMAP_CONSISTENT);
            }
        }
        // ---- (was Loop 2) wins/losses + gross accumulator aggregation ----
        total_wins   += state->cores[i].core_wins;
        total_losses += state->cores[i].core_losses;
        gross_wins   = FPN_Add(gross_wins,   state->cores[i].core_gross_wins);
        gross_losses = FPN_Add(gross_losses, state->cores[i].core_gross_losses);
        // ---- (was Loop 3) headline regime — first AUTO match wins ----
        // Flag-driven; preserves the pre-.B.8 break-on-first-AUTO semantic
        // without an explicit break (which would prevent further loop work
        // for THIS i + skip subsequent cores' per-core publishing).
        if (!headline_regime_set && state->cores[i].strategy_id == STRATEGY_AUTO) {
            headline_regime = state->cores[i].regime_state.current_regime;
            headline_regime_set = true;
        }
        // ---- (was Loop 4) per_core publisher body (continues below) ----
        snap->per_core[i].strategy_id_display = state->cores[i].strategy_id;
        // v4.0.4: resolved strategy after AUTO regime classification. For
        // non-AUTO cores this equals strategy_id_display.
        snap->per_core[i].resolved_strategy_id = state->cores[i].resolved_strategy_id;
        // v5.9.0c — explicit-set bitmap (V5_9_AUDIT-#5). Drives tri-state
        // marker in Per-Core P&L panel: "i!" deliberate, "i?" defaulted,
        // "i" auto-regime. Read bit i from the cfg's bitmap.
        // v5.14.9.B.2 — strategy_was_explicit_set migrated to state_flags BIT_FLAG.
        if ((cfg->core_strategies_explicit_set >> i) & 0x1) {
            STATE_FLAG_SET(snap->per_core[i], STRATEGY_EXPLICITLY_SET);
        }
        // v5.9.1 — per-core warmup % (rolling_short.count vs min_warmup_samples).
        // Defensive bounds: if min_warmup_samples is 0/unset, the engine
        // defaults to 64 (matches the global-snap fallback at line 128).
        {
            int wmin = (int)cfg->min_warmup_samples;
            if (wmin <= 0) wmin = 64;
            int wnow = state->cores[i].slow_state ?
                       state->cores[i].slow_state->rolling_short.count : 0;
            int pct = (wnow >= wmin) ? 100 : ((wnow * 100) / wmin);
            if (pct > 100) pct = 100;
            if (pct < 0) pct = 0;
            snap->per_core[i].warmup_progress_pct = (uint8_t)pct;
        }
        // v5.9.5i — cfg drift summary mirror
        // v5.15.5.B.2 — tier counters extracted to CoreContextDisplayMeta.
        // v5.15.5.B.3 — strict_refused flag migrated to core_state_flags bitmap.
        snap->per_core[i].cfg_drift_tier1_count    = state->display_meta[i].cfg_drift_tier1_count;
        snap->per_core[i].cfg_drift_tier2_count    = state->display_meta[i].cfg_drift_tier2_count;
        snap->per_core[i].cfg_drift_strict_refused =
            CORE_STATE_FLAG_IS_SET(state->cores[i], CFG_DRIFT_STRICT_REFUSED) ? 1 : 0;
        // Per-core gate direction. Use RESOLVED strategy for AUTO so direction
        // tracks the active regime's strategy. MOMENTUM buys above; everything
        // else buys below.
        uint8_t dir_strat = (state->cores[i].resolved_strategy_id != STRATEGY_NONE)
                              ? state->cores[i].resolved_strategy_id
                              : state->cores[i].strategy_id;
        // v5.14.9.B.2 — gate_direction migrated to state_flags BIT_FLAG.
        // Bit set = buy ABOVE (MOM); bit clear = buy below (other strategies).
        if (dir_strat == STRATEGY_MOMENTUM) {
            STATE_FLAG_SET(snap->per_core[i], GATE_BUY_ABOVE);
        }
        // v4.0.4: per-core diagnostic state for Buy Gate panel
        snap->per_core[i].halt_reason            = state->cores[i].halt_reason;
        // v5.6.2: strategy-internal halt reason (SHALT_*). Distinct from
        // halt_reason — set by strategy _BuildParameters when zero-gating
        // for strategy-specific reasons (no uptrend, fee-floor BUY_BLOCKED,
        // ML below threshold, etc).
        snap->per_core[i].strategy_halt_reason   = state->cores[i].strategy_halt_reason;
        // v5.6.3 — gate diagnostic comparands → FPN_ToDouble.
        // v5.15.5.B.2 — registry-generated read block per
        // FOREACH_GATE_DIAG_PAIR (MemHeaders/DisplayMetaRegistry.hpp).
        // Adding a 7th gate diag = one row in the registry; this block
        // auto-flows + PerCoreSnap field decl + GUI render row similarly
        // pick up the new field with their own registry walks.
#define X(FAMILY, ACTUAL_FIELD, OTHER_FIELD, _DOC) \
        snap->per_core[i].diag_##ACTUAL_FIELD = FPN_ToDouble(state->display_meta[i].diag_##ACTUAL_FIELD); \
        snap->per_core[i].diag_##OTHER_FIELD  = FPN_ToDouble(state->display_meta[i].diag_##OTHER_FIELD);
        FOREACH_GATE_DIAG_PAIR(X)
#undef X
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
        // v5.15.1 — TECH_DEBT-028: core_kill_tripped + drift_breached +
        // drift_kill_tripped migrated to state_flags bitmap. ExecutionCore
        // side (state->cores[i].core_kill_tripped + drift_history.*) keeps
        // its bool/struct storage — only the snapshot side moves to bitmap.
        if (CORE_STATE_FLAG_IS_SET(state->cores[i], KILL_TRIPPED)) {
            STATE_FLAG_SET(snap->per_core[i], CORE_KILL_TRIPPED);
        }
        // v5.10.3.B — runtime IC drift observability (parity-check Finding #9).
        // v5.15.5.E.B — breached + kill_tripped now packed in drift_state_flags
        // bitmap (uint8_t); ic_samples + ts_us merged into AoS samples[].ic.
        // Per bitmap-flag-api.md + latency-vs-cache-decision-framework.md.
        if (BITMAP_IS_SET(state->cores[i].drift_history.drift_state_flags, MASK_DRIFT_BREACHED)) {
            STATE_FLAG_SET(snap->per_core[i], DRIFT_BREACHED);
        }
        if (BITMAP_IS_SET(state->cores[i].drift_history.drift_state_flags, MASK_DRIFT_KILL_TRIPPED)) {
            STATE_FLAG_SET(snap->per_core[i], DRIFT_KILL_TRIPPED);
        }
        snap->per_core[i].drift_n_samples    = (uint16_t)state->cores[i].drift_history.count;
        {
            double sum = 0.0;
            int cnt = state->cores[i].drift_history.count;
            // v5.15.5.E.B — AoS interleave: read samples[k].ic (vs prior parallel-
            // array ic_samples[k]). Each iteration touches 1 cache line containing
            // both .ic + .ts (vs prior 2 cache lines from arrays 2048B apart).
            for (int k = 0; k < cnt; ++k) sum += state->cores[i].drift_history.samples[k].ic;
            snap->per_core[i].drift_avg_ic = (cnt > 0) ? (sum / (double)cnt) : 0.0;
        }
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
            // v5.14.9.B.2 — permission migrated to state_flags BIT_FLAG.
            if (__atomic_load_n(&core->permission, __ATOMIC_ACQUIRE)) {
                STATE_FLAG_SET(snap->per_core[i], PERMISSION_ALLOWED);
            }
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
            // v5.14.9.B.2 — is_ml + ml_model_loaded migrated to state_flags BIT_FLAG.
            STATE_FLAG_SET(snap->per_core[i], IS_ML);
            any_ml_active = 1;
            CoreModelZoo<F>* zoo = (CoreModelZoo<F>*)state->cores[i].model_handle;
            int loaded = (zoo && CoreModelZoo_HasAny(zoo)) ? 1 : 0;
            if (loaded) {
                STATE_FLAG_SET(snap->per_core[i], ML_MODEL_LOADED);
                any_model_loaded = 1;
            }
            // staged_prediction is the freshest rebuild output; active_prediction
            // is the snapshot at last entry submit (0 if no open position).
            snap->per_core[i].ml_last_prediction   = state->cores[i].staged_prediction;
            snap->per_core[i].ml_last_confidence   = state->cores[i].last_confidence;
            snap->per_core[i].ml_active_prediction = state->cores[i].active_prediction;
            // v5.14.9.B — soft risk degradation ladder factor surface.
            snap->per_core[i].ml_confidence_factor = state->cores[i].last_confidence_factor;
            // v5.14.9.B.2 — ladder-bottom inference: ladder active for this core
            // (gate cache says LADDER_ACTIVE) AND factor written as exactly 0.0
            // by ML_BuildParameters → entry blocked + SHALT_LOW_CONFIDENCE fired.
            // STATE_FLAG_LADDER_BOTTOM_HIT surfaces this for ML Status panel +
            // entry log (operator sees per-cycle ladder behavior).
            if (BITMAP_IS_SET(state->cores[i].gate_state.flags, tt::MASK_LADDER_ACTIVE)
                && state->cores[i].last_confidence_factor == 0.0) {
                STATE_FLAG_SET(snap->per_core[i], LADDER_BOTTOM_HIT);
            }
            // v5.13.6.A — sell-side ML prediction surface (parity-check
            // Section J observability gap close). Operator sees per-cycle
            // exit_predictor blended prob + dominant horizon in dashboard.
            snap->per_core[i].ml_last_exit_prediction       = state->cores[i].last_exit_prediction;
            snap->per_core[i].ml_last_exit_dominant_horizon = state->cores[i].last_exit_dominant_horizon;
            // v5.15.5.A.6 — buy-side per-horizon barrier observability snap.
            snap->per_core[i].ml_last_buy_dominant_horizon   = state->cores[i].last_buy_dominant_horizon;
            snap->per_core[i].ml_last_barrier_mode_used      = state->cores[i].last_barrier_mode_used;
            snap->per_core[i].ml_barrier_shadow_event_count  = state->display_meta[i].barrier_shadow_event_count;
            // Direct reads of scorer internals — these are double-only and safe
            // to compute on the snapshot path (snapshot is slow-path itself).
            // v5.14.1.F — variant-aware IC (default 0=Spearman). cfg in scope
            // via Snapshot fn parameter. Future Pearson/Kendall variants slot
            // in via FOREACH_IC_VARIANT registry; today's behavior bytewise
            // unchanged (single-case switch inlines to direct call).
            snap->per_core[i].ml_confidence_ic   = ConfidenceScorer_ComputeICVariant(
                &state->cores[i].confidence, cfg ? cfg->confidence_ic_variant : 0);
            snap->per_core[i].ml_confidence_rmse = RollingRMSE_Compute(&state->cores[i].confidence.rmse);
            // v5.14.1.G — portfolio turnover. Reads per-core RollingTurnover
            // ring; ~500ns at window=100 (popcount-based; within slow-path
            // budget). HOT_PATH_CHANGELOG entry committed in this ship.
            snap->per_core[i].ml_portfolio_turnover =
                RollingTurnover_Compute(&state->cores[i].turnover);
            // v5.9.0b — ML observability extensions. Single-writer (slow path)
            // → snapshot read; no race. Counters are uint32 monotonic.
            // v5.14.8.C — failure_flags bitmap (FOREACH_FAILURE_MODE BIT_FLAG entries).
            // Reset all failure bits, then set per slow-path state.
            snap->per_core[i].failure_flags = 0;
            // v5.15.5.B.2 — model_load_failed + ML threshold/nan_* counters
            // moved to display_meta (.B.3 will bit-pack model_load_failed back
            // onto CoreContext as a core_state_flags bit).
            if (CORE_STATE_FLAG_IS_SET(state->cores[i], MODEL_LOAD_FAILED)) {
                FAILURE_SET(snap->per_core[i], ml_model_load_failed);
            }
            snap->per_core[i].ml_last_threshold          = state->display_meta[i].last_ml_threshold;
            snap->per_core[i].ml_last_effective_threshold= state->display_meta[i].last_ml_effective_threshold;
            snap->per_core[i].ml_nan_feature_events      = state->display_meta[i].nan_feature_events_total;
            snap->per_core[i].ml_nan_prediction_events   = state->display_meta[i].nan_prediction_events_total;
            // v5.9.3a — scaler observability (Gap H). Aggregate across all
            // 4 model roles in the zoo: scaler considered "present" if ANY
            // role's handle has has_scaler=1; "load_failed" if ANY role has
            // scaler_load_failed=1. (Per the v5.9.3a load contract, all
            // roles in a zoo share the same training pipeline so they
            // typically agree, but per-role granularity is preserved by
            // the underlying ModelHandle.scaler — this surface aggregates.)
            //
            // v5.14.9.H (TECH_DEBT-013 candidate 7): 2 bools collapsed into
            // uint8_t scaler_summary_flags bitmap. 2 bits used; 6 bits headroom
            // for future scaler observability (e.g., partial-load, version-
            // mismatch, calibration-stale). Transient local; not persisted to
            // wire format (downstream surfaces ml_scaler_present + failure_flags
            // bit are the canonical persisted state).
            static constexpr uint8_t MASK_SCALER_PRESENT = (uint8_t)(1u << 0);
            static constexpr uint8_t MASK_SCALER_FAILED  = (uint8_t)(1u << 1);
            uint8_t scaler_summary_flags = 0;
            if (zoo) {
                if (zoo->buy_signal.scaler.has_scaler)   scaler_summary_flags |= MASK_SCALER_PRESENT;
                if (zoo->barrier.scaler.has_scaler)      scaler_summary_flags |= MASK_SCALER_PRESENT;
                if (zoo->regime.scaler.has_scaler)       scaler_summary_flags |= MASK_SCALER_PRESENT;
                if (zoo->exit.scaler.has_scaler)         scaler_summary_flags |= MASK_SCALER_PRESENT;
                if (zoo->buy_signal.scaler_load_failed)  scaler_summary_flags |= MASK_SCALER_FAILED;
                if (zoo->barrier.scaler_load_failed)     scaler_summary_flags |= MASK_SCALER_FAILED;
                if (zoo->regime.scaler_load_failed)      scaler_summary_flags |= MASK_SCALER_FAILED;
                if (zoo->exit.scaler_load_failed)        scaler_summary_flags |= MASK_SCALER_FAILED;
            }
            // v5.15.1 — TECH_DEBT-028: ml_scaler_present migrated to state_flags bitmap.
            if (BITMAP_IS_SET(scaler_summary_flags, MASK_SCALER_PRESENT)) {
                STATE_FLAG_SET(snap->per_core[i], ML_SCALER_PRESENT);
            }
            if (BITMAP_IS_SET(scaler_summary_flags, MASK_SCALER_FAILED)) {
                FAILURE_SET(snap->per_core[i], ml_scaler_load_failed);
            }
            // v5.15.1 — Model Health drift aggregation. OR-combine each
            // role's handle->drift_flags_at_load (set at TryLoadRole
            // chokepoint) into snap.failure_flags. Single source of truth
            // per handle; aggregate to per-core snapshot for GUI rendering.
            // Operator sees "drift on this core" if ANY role has drift.
            // training_timestamp_us captured from buy_signal as the
            // representative role (single-zoo typical case).
            //
            // v5.15.5.F.3 — Shape B fix per DESIGN_SPECS/registry-bitmap-
            // set-discipline.md. PRE-FIX BUG: the aggregation walked ONLY
            // the single-zoo `zoo->{buy_signal,barrier,regime,exit}` handles.
            // Multi-horizon ENSEMBLE handles live in `ezoo->buy_signal[h]`,
            // `ezoo->barrier[h]`, etc. (N handles per role); their drift
            // bits never aggregated → for ensemble-using cores, GUI Model
            // Health showed "clean" even when engine log emitted held-out-
            // gate WARNs for actual stamp drift. Operator (Caramel) nearly
            // traded against stale ensemble models on first paper-test
            // session post-v5.12. Fix: walk ezoo's handle arrays too;
            // chokepoint now sees BOTH load paths (single-zoo + ensemble).
            if (zoo) {
                snap->per_core[i].failure_flags |= zoo->buy_signal.drift_flags_at_load;
                snap->per_core[i].failure_flags |= zoo->barrier.drift_flags_at_load;
                snap->per_core[i].failure_flags |= zoo->regime.drift_flags_at_load;
                snap->per_core[i].failure_flags |= zoo->exit.drift_flags_at_load;
                snap->per_core[i].handle_training_timestamp_us =
                    zoo->buy_signal.training_timestamp_us;
            } else {
                snap->per_core[i].handle_training_timestamp_us = 0;
            }
            // v5.15.5.F.3 — ensemble-path drift aggregation. Walk ezoo's
            // per-horizon handle arrays for ALL 4 roles + OR drift bits.
            // Representative training_timestamp_us: take from primary buy_signal
            // handle (arm 0) if zoo's value wasn't set above. Per registry-
            // bitmap-set-discipline.md Fix 2 (chokepoint extension covers
            // both load paths).
            {
                auto* ezoo_drift = static_cast<EnsembleModelZoo<F>*>(
                    state->cores[i].ensemble_handle);
                if (ezoo_drift && BITMAP_IS_SET(ezoo_drift->init_flags, MASK_EZOO_ACTIVE)) {
                    for (int h = 0; h < ezoo_drift->buy_signal_count; ++h) {
                        snap->per_core[i].failure_flags |=
                            ezoo_drift->buy_signal[h].drift_flags_at_load;
                    }
                    for (int h = 0; h < ezoo_drift->barrier_count; ++h) {
                        snap->per_core[i].failure_flags |=
                            ezoo_drift->barrier[h].drift_flags_at_load;
                    }
                    for (int h = 0; h < ezoo_drift->regime_count; ++h) {
                        snap->per_core[i].failure_flags |=
                            ezoo_drift->regime[h].drift_flags_at_load;
                    }
                    for (int h = 0; h < ezoo_drift->exit_predictor_count; ++h) {
                        snap->per_core[i].failure_flags |=
                            ezoo_drift->exit_predictor[h].drift_flags_at_load;
                    }
                    // Adopt representative training_timestamp_us from arm 0
                    // (buy_signal) if zoo didn't set one above.
                    if (snap->per_core[i].handle_training_timestamp_us == 0 &&
                        ezoo_drift->buy_signal_count > 0) {
                        snap->per_core[i].handle_training_timestamp_us =
                            ezoo_drift->buy_signal[0].training_timestamp_us;
                    }
                }
            }
            // v5.10.0a.G.10 — populate ensemble snapshot from ezoo (when active).
            // The cast through void* matches the dispatcher; ml_zoo_ensemble's
            // type isn't visible here without a forward decl. Empty when
            // ensemble inactive (single-zoo path) → GUI hides the section.
            auto* ezoo = static_cast<EnsembleModelZoo<F>*>(
                state->cores[i].ensemble_handle);
            if (ezoo && BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE)) {
                auto& es = snap->per_core[i];
                es.ensemble_active = 1;
                // v5.11.62 — n_horizons reflects primary handles (set at
                // load to whichever role was actually populated). Pre-fix
                // this read buy_signal_count which was 0 for barrier-only
                // multi-horizon deployments — Settings panel showed
                // "n_horizons: 0" even though 4 barrier models loaded.
                int n_h = ezoo->primary_count;
                if (n_h > 8) n_h = 8;
                es.ensemble_n_horizons = (uint8_t)n_h;
                for (int h = 0; h < n_h; ++h) {
                    es.ensemble_horizon_ticks[h] = ezoo->horizon_ticks_at_idx[h];
                }
                for (int h = n_h; h < 8; ++h) {
                    es.ensemble_horizon_ticks[h] = 0;
                }
                es.ensemble_last_predicted_regime = ezoo->last_predicted_regime_id;
                es.ensemble_last_predicted_horizon_idx = ezoo->last_predicted_horizon_idx;
                strncpy(es.ensemble_blend_mode, ezoo->blend_mode,
                        sizeof(es.ensemble_blend_mode) - 1);
                es.ensemble_blend_mode[sizeof(es.ensemble_blend_mode) - 1] = '\0';
                es.ensemble_disabled_horizon_mask = ezoo->disabled_horizon_mask;
                // Per-regime probability matrix (preferred over raw weights —
                // probabilities are normalized so the heatmap is interpretable).
                for (int r = 0; r < 5; ++r) {  // NUM_REGIMES = 5
                    if (BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY)) {
                        double probs[8];
                        Bandit_GetProbabilities(&ezoo->bandits[r], probs);
                        for (int h = 0; h < n_h; ++h) {
                            es.ensemble_weights[r][h] = probs[h];
                        }
                        for (int h = n_h; h < 8; ++h) {
                            es.ensemble_weights[r][h] = 0.0;
                        }
                        es.ensemble_n_updates_per_regime[r] = ezoo->bandits[r].total_steps;
                    } else {
                        // Pre-init: uniform 1/N for visualization
                        for (int h = 0; h < 8; ++h) {
                            es.ensemble_weights[r][h] = (h < n_h)
                                ? (1.0 / n_h) : 0.0;
                        }
                        es.ensemble_n_updates_per_regime[r] = 0;
                    }
                }
                // v5.14.10.D — Populate Thompson display fields when cfg.bandit_algorithm
                // != 0 + initialized_thompson_bandits=1. cfg=0 (EXP3 default) leaves
                // the cluster zeroed → ML Status panel skips Thompson render branch.
                // For cfg=1 / cfg=2: copy current regime's posterior to display arrays;
                // pack thompson_state byte (active flag + chosen_arm).
                if (cfg->bandit_algorithm != 0 && BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_THOMPSON_READY)) {
                    int regime_id = ezoo->last_predicted_regime_id;
                    if (regime_id < 0 || regime_id >= 5) regime_id = 0;
                    const ThompsonBanditState* tb = &ezoo->thompson_bandits[regime_id];
                    int n_arms = tb->n_arms;
                    if (n_arms > 8) n_arms = 8;
                    // Float-cast at copy time (display precision; saves 32B/array vs double)
                    for (int a = 0; a < n_arms; ++a) {
                        es.thompson_mu_post[a]        = (float)tb->mu_post[a];
                        es.thompson_precision_post[a] = (float)tb->precision_post[a];
                        es.thompson_total_pulls[a]    = tb->total_pulls[a];
                    }
                    for (int a = n_arms; a < 8; ++a) {
                        es.thompson_mu_post[a]        = 0.0f;
                        es.thompson_precision_post[a] = 0.0f;
                        es.thompson_total_pulls[a]    = 0;
                    }
                    // Pack state byte: bit 0 = active; bits 1-3 = chosen_arm
                    uint8_t arm_bits = (uint8_t)(ezoo->last_predicted_thompson_arm >= 0
                        ? (ezoo->last_predicted_thompson_arm & 0x07) : 0);
                    es.thompson_state = (uint8_t)(
                        (uint8_t)TUISnapshot::PerCoreSnap::MASK_THOMPSON_BANDIT_ACTIVE |
                        (arm_bits << TUISnapshot::PerCoreSnap::SHIFT_THOMPSON_CHOSEN_ARM));
                } else {
                    es.thompson_state = 0;
                    for (int a = 0; a < 8; ++a) {
                        es.thompson_mu_post[a]        = 0.0f;
                        es.thompson_precision_post[a] = 0.0f;
                        es.thompson_total_pulls[a]    = 0;
                    }
                }
            } else {
                snap->per_core[i].ensemble_active = 0;
                snap->per_core[i].ensemble_n_horizons = 0;
                // Thompson cluster also zeroed when ezoo is not active
                snap->per_core[i].thompson_state = 0;
                for (int a = 0; a < 8; ++a) {
                    snap->per_core[i].thompson_mu_post[a]        = 0.0f;
                    snap->per_core[i].thompson_precision_post[a] = 0.0f;
                    snap->per_core[i].thompson_total_pulls[a]    = 0;
                }
            }
            // Track the highest-confidence ML core for the headline summary.
            // Tie-break: prefer the lowest core index (deterministic).
            if (state->cores[i].last_confidence > headline_conf) {
                headline_conf = state->cores[i].last_confidence;
                headline_ml_core = i;
            }
        } else {
            snap->per_core[i].ensemble_active = 0;
            snap->per_core[i].ensemble_n_horizons = 0;
        }
    }

    // ============================================================================
    // v5.15.5.B.8 — POST-LOOP AGGREGATE PUBLISHING (was inlined between Loop 2
    // and Loop 4 pre-.B.8; moved here so the unified per-core loop above can
    // compute all aggregates before they're consumed.)
    // ============================================================================
    //
    // wins/losses + win_rate + avg_win/avg_loss + profit_factor + expectancy
    // (Bug fix 2026-04-27 + v4.7.25 + v5.8.4c canonical Compute_* helpers).
    snap->wins   = total_wins;
    snap->losses = total_losses;
    if (total_wins + total_losses > 0) {
        snap->win_rate = (double)total_wins / (double)(total_wins + total_losses) * 100.0;
    } else {
        snap->win_rate = 0.0;
    }
    {
        double g_wins_d   = FPN_ToDouble(gross_wins);
        double g_losses_d = FPN_ToDouble(gross_losses);
        snap->avg_win  = (total_wins   > 0) ? g_wins_d   / (double)total_wins   : 0.0;
        snap->avg_loss = (total_losses > 0) ? g_losses_d / (double)total_losses : 0.0;
        // v5.8.4c — routed through canonical Compute_* helpers (single source
        // of truth across backtest + live + sharded paths). profit_factor is
        // 0.0 when losses=0 (matches BacktestEngine); all_wins_run flag tells
        // the GUI render path to display "—" / "∞" distinctly.
        snap->profit_factor = Compute_ProfitFactor(g_wins_d, g_losses_d);
        snap->all_wins_run  = Compute_AllWinsRun(g_wins_d, g_losses_d);
        snap->expectancy    = Compute_Expectancy((uint32_t)(total_wins + total_losses),
                                                  (uint32_t)total_wins,
                                                  snap->avg_win, snap->avg_loss);
    }

    // headline regime (v4.0.4) — first AUTO core's regime_state.current_regime
    // captured during the unified loop above; assigned here under the same
    // registered_count > 0 guard the pre-.B.8 code used.
    if (state->registered_count > 0) {
        snap->current_regime = headline_regime;
    }

    // Phase 6prep sharded c16: populate s->ml.* from the headline ML core.
    // GUI_Panel_MLIntelligence renders this single-core view; per-core detail
    // goes through the new per-core section.
    snap->ml.confidence_enabled = BITMAP_IS_SET(cfg->ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_ENABLED) ? 1 : 0;
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
