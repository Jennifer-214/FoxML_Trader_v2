// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/EventLoopAggregates.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [FLOAT_DISPLAY_ONLY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the TUI money-view adapter — flat aggregate doubles built once from EventLoopState per snapshot rebuild]
// [CONTAINS]
//   - [STRUCT]_[EventLoopAggregates]
//   - [FUNCTION]_[EventLoop_GetAggregates]
//======================================================================================================
// Phase 10 of the per-core sharded engine. Adapter struct + builder that pulls
// the "money view" out of an EventLoopState so the existing TUI/GUI snapshot
// fields can be populated without rewriting any panels.
//
// Why an adapter exists at all:
//   Per-core sharding moves position state from a single PortfolioController
//   into N execution cores plus a controller-side EventLoopState. The TUI was
//   built for the single-controller world where balance and equity are direct
//   fields on the controller. After sharding, those numbers are computed from
//   per-core state — balance is still a global wallet but equity has to walk
//   the active positions and add unrealized P&L from the latest mark price.
//
//   Rather than redesign the TUISnapshot struct (which has 100+ fields and
//   touches every panel), we expose a small adapter that produces flat aggregate
//   doubles. Phase 13 migration writes a 30-line shim that maps these into the
//   existing TUISnapshot fields:
//
//     snap->balance      = agg.balance
//     snap->equity       = agg.equity
//     snap->realized     = agg.realized_pnl
//     snap->unrealized   = agg.unrealized_pnl
//     snap->total_pnl    = agg.realized_pnl + agg.unrealized_pnl
//     snap->active_count = agg.active_position_count
//     snap->max_drawdown = agg.max_drawdown
//     snap->max_drawdown_pct      = agg.max_drawdown_pct
//     snap->kill_switch_active    = agg.kill_switch_tripped
//
//   Existing panels keep working unchanged. Per-core debug views are out of
//   scope for this phase — can be added later as a new optional panel.
//
// Mark price:
//   Equity needs a mark price for unrealized P&L. The execution cores see ticks
//   directly but the controller core does not — it processes events. The engine
//   main publishes the latest tick price into the controller via a separate
//   path (a "current price" memory cell or a 1-Hz price update event), and
//   passes it as the mark_price argument to GetAggregates. Pass FPN_Zero to
//   skip unrealized computation entirely (unrealized stays 0).
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"
#include "ControllerEventLoop.hpp"

#include <cstdint>

namespace tt {

//======================================================================
// [STRUCT]_[EventLoopAggregates]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [FLOAT_DISPLAY_ONLY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the flat money view — doubles for the TUI; one Money->double crossing here instead of scattered per panel]
//======================================================================
// [CODE]
//======================================================================
struct EventLoopAggregates {
    // realized side — reflects executed trades
    double balance;              // global wallet (starting + sum of realized net P&L)
    double realized_pnl;         // total realized P&L this session
    double peak_balance;         // running max of balance, used for drawdown

    // unrealized side — reflects open positions at the latest mark price
    double unrealized_pnl;       // sum of (qty * (mark_price - entry_price)) over active slots
    double equity;               // balance + unrealized_pnl

    // counts
    int registered_nodes;        // == EventLoopState::registered_count
    int active_position_count;   // popcount of portfolio.active_bitmap
    uint64_t total_entries;
    uint64_t total_exits;

    // risk
    int    kill_switch_tripped;
    double max_drawdown;         // max(0, peak_balance - equity)
    double max_drawdown_pct;     // max_drawdown / peak_balance (0 when peak == 0)
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// All fields are doubles because the TUI consumes doubles. The conversion from
// FPN_Binary happens once here, in the adapter, instead of being scattered across
// every panel. realized + unrealized = total_pnl. balance + unrealized = equity.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[88B]
// [ALIGN]_[8]
// [CACHE_LINES]_[2]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[EventLoopAggregates]
//======================================================================

//======================================================================
// [FUNCTION]_[EventLoop_GetAggregates]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one EventLoopState walk -> the aggregate money view; unrealized marks active slots at mark_price (skip on zero)]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline EventLoopAggregates EventLoop_GetAggregates(const EventLoopState<F>* state,
                                                    Money mark_price) {
    EventLoopAggregates agg;

    // realized side from OMS (financial state lives there since chunk 1B)
    agg.balance      = Money_ToDouble(state->oms->balance);
    agg.realized_pnl = Money_ToDouble(state->oms->realized_pnl);
    agg.peak_balance = Money_ToDouble(state->oms->ks_peak_balance);

    // counts
    agg.registered_nodes = state->registered_count;
    agg.total_entries    = state->total_entries;
    agg.total_exits      = state->total_exits;
    // v5.15.5.C.2 (S3a) — kill_switch_tripped now lives in oms_state_flags bitmap.
    agg.kill_switch_tripped = BITMAP_IS_SET(state->oms->oms_state_flags, tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED);

    // walk the active bitmap once for unrealized P&L and active count
    Money unreal = Money_Zero();
    int active = 0;
    uint16_t bm = state->oms->portfolio.active_bitmap;
    bool have_mark = !Money_IsZero(mark_price);
    for (int slot = 0; slot < MAX_PORTFOLIO_POSITIONS; ++slot) {
        if (((bm >> slot) & 1) == 0) continue;
        ++active;
        if (have_mark) {
            const Position<F>* pos = &state->oms->portfolio.positions[slot];
            // unrealized = qty * (mark - entry). Long-only for now; if quantity
            // were ever negative this still produces the correct sign.
            Money diff = Money_Sub(mark_price, pos->entry_price);
            Money pnl  = Money_Mul(pos->quantity, diff);
            unreal = Money_Add(unreal, pnl);
        }
    }
    agg.active_position_count = active;
    agg.unrealized_pnl = Money_ToDouble(unreal);
    agg.equity         = agg.balance + agg.unrealized_pnl;

    // drawdown — computed against equity (not balance) so it reflects mark to
    // market loss in real time. matches the existing TUI semantics.
    if (agg.peak_balance > 0.0 && agg.equity < agg.peak_balance) {
        agg.max_drawdown     = agg.peak_balance - agg.equity;
        agg.max_drawdown_pct = agg.max_drawdown / agg.peak_balance;
    } else {
        agg.max_drawdown     = 0.0;
        agg.max_drawdown_pct = 0.0;
    }

    return agg;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Walk the EventLoopState once and emit the aggregate struct. O(MAX_PORTFOLIO_POSITIONS)
// per call but only the bits set in active_bitmap are read for unrealized P&L.
// Cheap enough to call on every snapshot rebuild (slow path, ~1Hz).
//
// mark_price: latest known market price. Pass FPN_Zero to skip unrealized P&L
// (equity will == balance, useful when there's no current price available
// such as during warmup or replay seek).
//
// pitfall P10.1 caveat: the portfolio bitmap and per-position fields read here
// are mutated by the controller core (the same core calling this), so there is
// no concurrency hazard. The execution cores never write to EventLoopState's
// portfolio — only the controller's _OnEvent does. Snapshot reads from one
// core, writes from one core, no atomics required.
//======================================================================
// [END_FUNCTION]_[EventLoop_GetAggregates]
//======================================================================

}  // namespace tt
