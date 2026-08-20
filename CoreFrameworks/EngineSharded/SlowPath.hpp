// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/EngineSharded/SlowPath.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[drainer slow-path hoisted helpers — post-fill bookkeeping + GUI manual-close funnel]
// [CONTAINS]
//   - (EngineSharded_SlowPath_DrainPostFill DELETED E.1.2.C leg 0 -> tt::EngineCommon_DrainPostFill)
//   - [FUNCTION]_[EngineSharded_SlowPath_DrainManualCloses]
//======================================================================================================
// Sub-file of CoreFrameworks/EngineSharded.hpp (split per file-size-split-discipline.md
// at v5.15.5.F.4d.1.B.6; subfolder pattern first canonical).
//
// **Decision B clarification:** Hoisted helpers use `template<unsigned F>` (NOT
// `template<F, BENCH>`). All Live/Backtest dispatch is cfg-flag-driven, NOT
// BENCH-driven; lambda bodies never referenced BENCH (verified at Phase A.2 CSV).
//
// **Decision H rationale:** Merging the two #ifdef-gated variants into ONE
// function with the gate moved INSIDE the body matches `.B.4` EngineCommon
// dual-cfg pattern (BootPerCore conditionalizes cfg-flag inside body). Single
// source of truth + consistent sister-pattern shape + future-extensible.
//
// **Sister to:** `.B.4` EngineCommon_SlowPathCycleAllCores GLOBAL-dispatch shape
// (called once per cycle; walks cores internally via bitmap iteration).
//======================================================================================================

#pragma once

#include <atomic>
#include <cstdio>

#include "../ControllerEventLoop.hpp"        // EventLoop_DrainPostFill
#include "../OrderManager.hpp"                // OrderManagerState
#include "../ExecutionCore.hpp"               // EventLoopState
#include "../ControllerConfig.hpp"            // ControllerConfig
#include "../../MemHeaders/OmsPushExitHelper.hpp"  // OMS_PushExitForSlot
#include "../../MemHeaders/BitmapMacros.hpp"  // BITMAP_IS_SET

// Forward decl for nullable TUISharedState* parameter; full type defined at
// GLOBAL scope in DataStream/EngineTUI.hpp:1287 (NOT in namespace tt).
// CRITICAL: declare forward-decl at global scope (NOT inside namespace tt)
// so unqualified `TUISharedState*` references inside `namespace tt { ... }`
// resolve to `::TUISharedState` via name lookup, NOT a new `tt::TUISharedState`
// that would shadow the real type with an incomplete forward decl.
struct TUISharedState;

// parent_index: CoreFrameworks/EngineSharded.hpp

namespace tt {

// E.1.2.C leg 0 (2026-08-20) — EngineSharded_SlowPath_DrainPostFill DELETED.
// Its binding role moved to the SHARED tt::EngineCommon_DrainPostFill
// (EngineCommon.hpp; called by the live drainer AND both backtest driver
// sites — M5 execution-layer parity by construction). That binder's comment
// carries the fan-shift history: this wrapper's 8-arg call was landing the
// exit-bandit enable flag in OneCore's confidence_ic_variant slot after the
// v5.14.1.F mid-signature insert, so the flag never reached its gate.

//======================================================================
// [FUNCTION]_[EngineSharded_SlowPath_DrainManualCloses]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [GUI] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[GUI manual force-close funnel — synthetic SELL via OMS_PushExitForSlot; race-tolerant with hot-path SG]
//======================================================================
// [CODE]
//======================================================================
template<unsigned F>
inline void EngineSharded_SlowPath_DrainManualCloses(
    EventLoopState<F>& state,
    OrderManagerState<F>& oms,
    const ControllerConfig<F>& cfg,
    std::atomic<double>& last_price,
    TUISharedState* shared_ptr   // nullable; #ifdef USE_IMGUI_GUI gates body usage
) {
#ifdef USE_IMGUI_GUI
    // v5.15.5.C.4 Phase T1 — hoist partial_on out of slot loop.
    // Drainer-thread-stable predicate; one read per drain_manual_closes
    // call vs N slots × 1 read.
    const int partial_on = BITMAP_IS_SET(oms.oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
    for (int slot = 0; slot < MAX_PORTFOLIO_POSITIONS; ++slot) {
        if (!shared_ptr->manual_close_requested[slot]) continue;
        shared_ptr->manual_close_requested[slot] = 0;
        // Skip if no open position at this slot — defensive against
        // double-clicks or races with auto-close.
        if ((oms.portfolio.active_bitmap & (uint16_t)(1u << slot)) == 0) {
            std::fprintf(stderr,
                "[manual-close] slot %d: no active position, ignoring\n", slot);
            continue;
        }
        Money qty = oms.portfolio.positions[slot].quantity;
        if (Money_IsZero(qty)) continue;
        // Map slot → node_id for strategy_id + leg lookup
        // v5.15.5.C.2 (S3a + S4): canonical mirror via bit-packed oms_state_flags.
        // v5.15.5.C.4 Phase T1: partial_on hoisted to lambda-scope above.
        int node_id = Sharded_SlotNode(slot, partial_on);
        int leg     = partial_on ? (slot & 1)  : 0;
        if (node_id < 0 || node_id >= state.registered_count) continue;
        uint8_t strategy_id = state.nodes[node_id].strategy_id;
        // Use latest tick price as fill price for paper mode. Live
        // mode would route to a real adapter SELL — same Submit call.
        Money fill_px = Money{ money_from_double_payload(
            last_price.load(std::memory_order_relaxed)) };
        if (Money_IsZero(fill_px)) {
            fill_px = oms.portfolio.positions[slot].entry_price;  // safe fallback
        }
        // v4.7.37 (Phase B reordered): push through OMS_PushSubmit so
        // the drainer thread serializes Submit calls. Manual close is a
        // GUI-driven event; without funneling, this site races with
        // other producer-thread Submits when Phase C spawns multiple.
        // v5.15.5.C.4 Phase D5: routed via OMS_PushExitForSlot helper —
        // 8-arg market-sell-with-degenerate-TP/SL → 6-arg helper call.
        tt::OMS_PushExitForSlot(&oms,
            (int16_t)slot,
            qty,
            strategy_id,
            fill_px,
            (uint8_t)leg,
            &cfg.nodes[node_id]);  // v5.15.5.F.4c.3 WIP2d-1.B.1: per-node cfg for pre-resolve at submit
        // v4.7.19: counter bumps moved to EventLoop_DrainPostFill —
        // see the doctrine note there. Pre-v4.7.19 we bumped here
        // BEFORE Submit could fail (queue full, slot already closed,
        // etc.), causing 7-vs-5 counter-vs-CSV drift. Now bumps fire
        // exactly when HandleFill writes a CSV row.
        // Clear the matching ExecutionCore active flag so the hot
        // path doesn't re-emit on the next tick (race-tolerant —
        // worst case is one duplicate exit event that HandleFill
        // dedups via the empty-slot bitmap check).
        if (state.nodes[node_id].core) {
            if (leg == 0) state.nodes[node_id].core->active   = 0;
            else          state.nodes[node_id].core->active_b = 0;
        }
        std::fprintf(stderr,
            "[manual-close] slot %d (node %d leg %s): force-exit @ %.2f, qty %.6f\n",
            slot, node_id, leg == 0 ? "A" : "B",
            Money_ToDouble(fill_px), Money_ToDouble(qty));
    }
#else
    // ANSI / headless build: no GUI → no manual close requests possible.
    // Body preprocessor-elided; function still exists as no-op (called
    // unconditionally by drainer; shared_ptr=nullptr passed by caller).
    (void)state;
    (void)oms;
    (void)cfg;
    (void)last_price;
    (void)shared_ptr;
#endif
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v4.7.8: manual force-close requests from the GUI. User clicks a button on the
// Positions panel → GUI sets manual_close_requested[slot]=1 → drainer reads +
// emits a synthetic SELL via OrderManager_Submit bypassing the hot-path SG.
// Race with the hot path (ExecutionCore could fire a real SL on the same slot
// in the same window) is tolerated: HandleFill checks the bitmap before
// CloseSlot, so a double-close becomes a no-op on the second attempt.
// Slow-path only.
//
// **Decision H merge (v5.15.5.F.4d.1.B.6):** Originally TWO lambda variants —
// LIVE (under #ifdef USE_IMGUI_GUI) + NO-OP (under #else). Merged into ONE
// function with the #ifdef moved INSIDE the body. Single source of truth +
// matches `.B.4 EngineCommon_BootPerCore` dual-cfg pattern (cfg-flag
// conditionalization inside helper). Under non-GUI build the body is
// preprocessor-elided (function still exists but does nothing).
//
// shared_ptr is nullable: pass `&g_shared` under USE_IMGUI_GUI; nullptr otherwise.
// Body's #ifdef gate guarantees shared_ptr is only dereferenced when valid.
//======================================================================
// [END_FUNCTION]_[EngineSharded_SlowPath_DrainManualCloses]
//======================================================================

} // namespace tt
