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
//   - [FUNCTION]_[EventLoop_AggregatesFromPack]
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
// [FUNCTION]_[EventLoop_AggregatesFromPack]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the pack->display adapter (census #8) — money view from the composer-published MoneySnapshot; counts from single-word fields; NO live OMS money walk]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline EventLoopAggregates EventLoop_AggregatesFromPack(const MoneySnapshot<F>& pack,
                                                        const EventLoopState<F>* state) {
    EventLoopAggregates agg;

    // money view — from the composer-published coherent pack (census #8: the retired
    // builder walked live OMS money + positions[] cross-thread on the producer, under a
    // comment claiming "no concurrency hazard" that had rotted since the centralized era)
    agg.balance      = Money_ToDouble(pack.balance);
    agg.realized_pnl = Money_ToDouble(pack.realized);
    agg.peak_balance = Money_ToDouble(pack.ks_peak);

    // unrealized = the composer-owned per-node MtM rows (P2-a real since this ship).
    // Marked at the COMPOSE price — display now shows exactly what the kill eval saw
    // (≤1 drainer cycle stale), not a producer-side re-mark; zero rows when MtM is off,
    // degrading to equity == balance like the old have_mark=false path.
    Money unreal = Money_Zero();
    for (int n = 0; n < MAX_EXECUTION_NODES; ++n) {
        unreal = Money_Add(unreal, pack.rows[tt::NodeIdx{(int16_t)n}].unrealized);
    }
    agg.unrealized_pnl = Money_ToDouble(unreal);
    agg.equity         = agg.balance + agg.unrealized_pnl;

    // counts — single-word (≤8B) fields; the Class-63 M3 surface says these are fine
    // as plain reads (the class is MULTI-word values)
    agg.registered_nodes      = state->registered_count;
    agg.total_entries         = state->total_entries;
    agg.total_exits           = state->total_exits;
    agg.active_position_count = __builtin_popcount((unsigned)state->oms->portfolio.active_bitmap);

    // risk — kill authority from the pack's display copy of the 3-tier word (same
    // source the compose derives from the OMS flag; single coherent read)
    agg.kill_switch_tripped = (pack.kill_word_copy & KILLWORD_MASK_GLOBAL) != 0;
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
// Build the display aggregate from the composer-published MoneySnapshot. O(16 rows)
// per call. Cheap enough to call on every snapshot rebuild (~1Hz).
//
// HISTORY (census #8, E.1.3 P2-f): the predecessor (EventLoop_GetAggregates) walked
// live OMS money + positions[] from the PRODUCER thread under a "no concurrency
// hazard / no atomics required" comment — TRUE when the centralized controller was
// the sole caller, ROTTED the day the sharded producer started calling it (Class-63
// shape (b): the single-owner claim outlived its topology). The pack read closes it:
// one seqlock copy, composed by the single writer that owns the ledger. The caller
// obtains the pack via ParameterSlot_Read(&state->agg.publish, ...) — before the
// first compose it reads the boot pack (generation 0, zeros): equity == balance == 0
// for at most one frame.
//======================================================================
// [END_FUNCTION]_[EventLoop_AggregatesFromPack]
//======================================================================

}  // namespace tt
