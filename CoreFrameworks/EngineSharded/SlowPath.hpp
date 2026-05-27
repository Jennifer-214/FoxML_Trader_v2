// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [EngineSharded/SlowPath.hpp — drainer slow-path hoisted helpers]
//======================================================================================================
// Sub-file of CoreFrameworks/EngineSharded.hpp (split per file-size-split-discipline.md
// at v5.15.5.F.4d.1.B.6; subfolder pattern first canonical).
//
// Contains:
//   - EngineSharded_SlowPath_DrainPostFill — hoisted from drain_post_fill lambda
//   - EngineSharded_SlowPath_DrainManualCloses — MERGED hoist of drain_manual_closes
//     LIVE + NO-OP variants per Decision H (single function with #ifdef inside body)
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

//======================================================================================================
// EngineSharded_SlowPath_DrainPostFill — hoist of drain_post_fill lambda
//======================================================================================================
// Per-fill bookkeeping that runs on the drainer thread after order fills come
// back from the exchange (Binance user-data WS → drainer reads → calls this).
//
// Originally a lambda inside EngineSharded_Run; hoisted to named function at
// v5.15.5.F.4d.1.B.6 per Decision B (Option a; captures → explicit args).
// Body unchanged from lambda body modulo capture → arg translation.
//
// v5.15.5.F.4c.3 WIP2d-1.B.1 — static const fee_rate_taker_d cache DELETED (Class 27
// fn-local variant; froze first-cfg-value globally). fee_rate_taker_for_cf scalar
// param chain DELETED from EventLoop_DrainPostFill / DrainPostFillOneCore signatures —
// OneCore reads per-core fee from o->pre_resolved.fee_rate (Order carries pre-resolved
// value via Order_BindPreResolved at submit time). See decision-time-data-binding-
// pattern.md + RECURRING_BUG_PATTERNS Class 27.
//======================================================================================================
template<unsigned F>
inline void EngineSharded_SlowPath_DrainPostFill(
    EventLoopState<F>& state,
    OrderManagerState<F>& oms,
    const ControllerConfig<F>& cfg
) {
    EventLoop_DrainPostFill(&state, &oms, cfg.sl_cooldown_cycles,
                             cfg.ensemble_trade_reward_mult,
                             cfg.confidence_ic_floor,
                             cfg.confidence_ic_floor_window,
                             cfg.auto_kill_on_drift,
                             // v5.13.4 — sell-side bandit attribution
                             BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_EXIT_BANDIT_ENABLED));
}

//======================================================================================================
// EngineSharded_SlowPath_DrainManualCloses — MERGED hoist of drain_manual_closes (Decision H)
//======================================================================================================
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
//======================================================================================================
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
        FPN<F> qty = oms.portfolio.positions[slot].quantity;
        if (FPN_IsZero(qty)) continue;
        // Map slot → core_id for strategy_id + leg lookup
        // v5.15.5.C.2 (S3a + S4): canonical mirror via bit-packed oms_state_flags.
        // v5.15.5.C.4 Phase T1: partial_on hoisted to lambda-scope above.
        int core_id = partial_on ? (slot >> 1) : slot;
        int leg     = partial_on ? (slot & 1)  : 0;
        if (core_id < 0 || core_id >= state.registered_count) continue;
        uint8_t strategy_id = state.cores[core_id].strategy_id;
        // Use latest tick price as fill price for paper mode. Live
        // mode would route to a real adapter SELL — same Submit call.
        FPN<F> fill_px = FPN_FromDouble<F>(
            last_price.load(std::memory_order_relaxed));
        if (FPN_IsZero(fill_px)) {
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
            &cfg.cores[core_id]);  // v5.15.5.F.4c.3 WIP2d-1.B.1: per-core cfg for pre-resolve at submit
        // v4.7.19: counter bumps moved to EventLoop_DrainPostFill —
        // see the doctrine note there. Pre-v4.7.19 we bumped here
        // BEFORE Submit could fail (queue full, slot already closed,
        // etc.), causing 7-vs-5 counter-vs-CSV drift. Now bumps fire
        // exactly when HandleFill writes a CSV row.
        // Clear the matching ExecutionCore active flag so the hot
        // path doesn't re-emit on the next tick (race-tolerant —
        // worst case is one duplicate exit event that HandleFill
        // dedups via the empty-slot bitmap check).
        if (state.cores[core_id].core) {
            if (leg == 0) state.cores[core_id].core->active   = 0;
            else          state.cores[core_id].core->active_b = 0;
        }
        std::fprintf(stderr,
            "[manual-close] slot %d (core %d leg %s): force-exit @ %.2f, qty %.6f\n",
            slot, core_id, leg == 0 ? "A" : "B",
            FPN_ToDouble(fill_px), FPN_ToDouble(qty));
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

} // namespace tt
