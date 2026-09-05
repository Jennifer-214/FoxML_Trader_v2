// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/EngineSharded/SlowPath.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[drainer slow-path hoisted helpers — the GUI manual-close funnel + the drainer/composer BOOK pass and its UNCONDITIONAL shutdown tail (E.1.3 3b(ii) commit 3)]
// [CONTAINS]
//   - (EngineSharded_SlowPath_DrainPostFill DELETED E.1.2.C leg 0 -> tt::EngineCommon_DrainPostFill)
//   - [FUNCTION]_[EngineSharded_SlowPath_DrainManualCloses]
//   - [FUNCTION]_[EngineSharded_Drainer_BookPass]
//   - [FUNCTION]_[EngineSharded_Drainer_ShutdownTail]
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
#include "../Tick.hpp"                        // Tick<F>
#include "../EngineCommon.hpp"                // E.1.3 c3 — EngineCommon_DrainEventsAndSubmit / _DrainPostFill / _ComposeAndKillEval (the book pass)
#include "../../MemHeaders/OmsPhasedDrain.hpp"   // E.1.3 c3 — OmsDrainBuckets + OrderManager_DrainIntoBuckets / ProcessBucket_*
#include "../../MemHeaders/DrainerConstants.hpp" // E.1.3 c3 — DrainerConstants_Init (per-pass drain count)
#include "../../MemHeaders/HealthLog.hpp"        // E.1.3 c3 — the shutdown tail's durable summary line
#include "../ParameterSlot.hpp"               // ParameterSlot<Tick<F>> — latest-tick seqlock (PARITY-047)
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
    // PARITY-047 — the latest tick arrives as ONE seqlock'd sample (was a bare
    // price lane). Same value; the producer built .price from the same double
    // via the same money_from_double_payload call.
    ParameterSlot<Tick<F>>& latest_tick,
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
        int node_id = (int)Sharded_SlotNode(tt::SlotIdx{(int16_t)slot}, partial_on);
        int leg     = partial_on ? (slot & 1)  : 0;
        if (node_id < 0 || node_id >= state.registered_count) continue;
        // D-470 (cascade C3) — RESOLVED, not configured. The entry submit binds
        // resolved_strategy_id (EngineCommon), so an exit binding the configured id
        // puts two different strategy labels on the two halves of ONE trade.
        uint8_t strategy_id = state.nodes[tt::NodeIdx{(int16_t)node_id}].resolved_strategy_id;
        // Use latest tick price as fill price for paper mode. Live
        // mode would route to a real adapter SELL — same Submit call.
        Tick<F> _latest{};
        ParameterSlot_Read(&latest_tick, &_latest);
        Money fill_px = _latest.price;
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
            &cfg.nodes[tt::NodeIdx{(int16_t)node_id}]);  // v5.15.5.F.4c.3 WIP2d-1.B.1: per-node cfg for pre-resolve at submit
        // v4.7.19: counter bumps moved to EventLoop_DrainPostFill —
        // see the doctrine note there. Pre-v4.7.19 we bumped here
        // BEFORE Submit could fail (queue full, slot already closed,
        // etc.), causing 7-vs-5 counter-vs-CSV drift. Now bumps fire
        // exactly when HandleFill writes a CSV row.
        // Clear the matching ExecutionCore active flag so the hot
        // path doesn't re-emit on the next tick (race-tolerant —
        // worst case is one duplicate exit event that HandleFill
        // dedups via the empty-slot bitmap check).
        if (state.nodes[tt::NodeIdx{(int16_t)node_id}].core) {
            if (leg == 0) state.nodes[tt::NodeIdx{(int16_t)node_id}].core->active   = 0;
            else          state.nodes[tt::NodeIdx{(int16_t)node_id}].core->active_b = 0;
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
    (void)latest_tick;
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

// E.1.3 3b(ii) commit 3 — how many book passes the composer's shutdown tail runs over the rings
// its (already joined) producers left behind. 16 was the pre-commit-3 figure (the producer_done-
// gated loop that never ran on SIGINT); kept, named. Each pass drains every ring once; a pass
// that books nothing costs a few hundred ns — the number bounds the WORK, not a wait.
constexpr int ENGINE_SHUTDOWN_TAIL_PASSES = 16;

//======================================================================
// [FUNCTION]_[EngineSharded_Drainer_BookPass]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING]]
// [THREAD]_[[COMPOSER_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[ONE booking pass of the live drainer/composer cycle — pump TradeEvents + GUI manual closes into the submit queues, OMS_DrainSubmit, then the phase-separated drain (closes -> post-fill -> opens -> reconciles); returns the pump's drained count (the idle-yield signal). The main loop AND the shutdown tail run this SAME body — extracted at E.1.3 3b(ii) commit 3 so the tail is a named, testable unit instead of a copy of the loop inside a lambda]
// [REFERENCE]_[DECISION]_[[D-440] [D-478]]
// [REFERENCE]_[DESIGN_SPEC]_[phase-separated-drainer-for-safe-cross-temporal-derives]
//======================================================================
// [CODE]
//======================================================================
// Buckets by POINTER: the caller owns the ~147 KB OmsDrainBuckets scratch (the drainer lambda's
// stack local); a second instance here would double the working set for nothing. `now_tick` is
// the producer's tick count at the pass (the pump's cooldown/spacing clock). NOT the compose:
// the money-flag drain, EngineCommon_ComposeAndKillEval, the composer-executed paper reset and
// the BENCH bracket stay in the caller — they are per-CYCLE, this is the per-cycle BOOKING.
template<unsigned F>
inline int EngineSharded_Drainer_BookPass(
    EventLoopState<F>& state,
    OrderManagerState<F>& oms,
    const ControllerConfig<F>& cfg,
    uint64_t now_tick,
    ParameterSlot<Tick<F>>& latest_tick,
    TUISharedState* shared_ptr,          // nullable (ANSI build / harness)
    OmsDrainBuckets* buckets             // the caller's per-thread scratch — by pointer, never a second instance
) {
    // DrainerConstants cache (drainer-thread-stable cfg + state predicates) — one init per pass;
    // state may shift between passes (the tail runs several).
    const DrainerConstants dc = DrainerConstants_Init(state.registered_count, cfg, oms);
    const int total_drained = EngineCommon_DrainEventsAndSubmit<F>(state, oms, now_tick, cfg);
    EngineSharded_SlowPath_DrainManualCloses(state, oms, cfg, latest_tick, shared_ptr);
    OMS_DrainSubmit(&oms, dc.drain_count);
    // Phase-separated drain (replaces the unified OrderManager_Tick on the live path): A closes ->
    // A.5 EngineCommon_DrainPostFill (reads CLOSE-form Position state — unlocks the Phase G+H
    // derives) -> B opens (Portfolio_OpenSlot fires here) -> C reconciles (phase-invariant safe).
    OrderManager_DrainIntoBuckets(&oms, buckets);
    OrderManager_ProcessBucket_Closes(&oms, buckets);
    EngineCommon_DrainPostFill(state, oms, cfg);
    OrderManager_ProcessBucket_Opens(&oms, buckets);
    OrderManager_ProcessBucket_Reconciles(&oms, buckets);
    return total_drained;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineSharded_Drainer_BookPass]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineSharded_Drainer_ShutdownTail]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING] [BOOT_TIME]]
// [THREAD]_[[COMPOSER_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[E.1.3 3b(ii) commit 3 (gate #3 G3-5) — the composer's UNCONDITIONAL shutdown tail: ENGINE_SHUTDOWN_TAIL_PASSES book passes over whatever the already-joined producers left in the rings, then ONE final coherent compose + kill eval so the saves on main see the last books + flags; prints + Health_Logs what it booked. Returns the number of fills booked]
// [REFERENCE]_[DECISION]_[[D-420] [D-440] [D-478]]
//======================================================================
// [CODE]
//======================================================================
// Runs on the composer thread AFTER main has joined every producer of its inputs (order
// sources, then the venue producers — reconciler / REST workers / WS) and set
// agg.composer_stop_request. Until commit 3 this body lived inside the drainer lambda behind
// `if (producer_done)` — a gate the producer could only satisfy after seeing the very flag the
// lambda had already exited on, so the tail never ran on SIGINT / SIGTERM / GUI close
// (verification NEW-1) and a fill landing after the drainer join was stranded. The backtest
// does not call this: it is single-threaded and runs to completion with its own inline final
// flush (the phased-vs-unified asymmetry is homed at §4.4 / E.1.4-slim, not here).
template<unsigned F>
inline int EngineSharded_Drainer_ShutdownTail(
    EventLoopState<F>& state,
    OrderManagerState<F>& oms,
    const ControllerConfig<F>& cfg,
    uint64_t now_tick,
    ParameterSlot<Tick<F>>& latest_tick,
    TUISharedState* shared_ptr,
    OmsDrainBuckets* buckets
) {
    const uint64_t filled_before = OrderManager_TotalFilled(&oms);
    for (int k = 0; k < ENGINE_SHUTDOWN_TAIL_PASSES; ++k) {
        (void)EngineSharded_Drainer_BookPass<F>(state, oms, cfg, now_tick, latest_tick, shared_ptr, buckets);
    }
    // E.1.3 P2-a — one final coherent compose + eval after the tail drains, so shutdown consumers
    // (the owner-side saves) see the last books + flags. MtM price = the producer-published
    // seqlock tick (the ONE safe read; the producer is joined, so it is the last tick it saw).
    {
        Tick<F> _lt{};
        ParameterSlot_Read(&latest_tick, &_lt);
        EngineCommon_ComposeAndKillEval(state, oms, cfg, _lt.price, now_tick);
    }
    const uint64_t booked   = OrderManager_TotalFilled(&oms) - filled_before;
    const int      inflight = OrderManager_InflightCount(&oms);
    std::fprintf(stderr,
        "[sharded] composer shutdown tail: %d pass(es), %llu fill(s) booked, %d order(s) still in flight%s\n",
        ENGINE_SHUTDOWN_TAIL_PASSES, (unsigned long long)booked, inflight,
        inflight ? " (wire-in-flight residual — reconciled at the next boot; E.1.4 A20)" : "");
    Health_Log(HEALTH_INFO, "shutdown", -1,
               "composer tail: passes=%d booked=%llu inflight=%d",
               ENGINE_SHUTDOWN_TAIL_PASSES, (unsigned long long)booked, inflight);
    return (int)booked;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineSharded_Drainer_ShutdownTail]
//======================================================================

} // namespace tt
