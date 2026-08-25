// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/OmsPushExitHelper.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the 6-arg market-sell exit submit helper — 4-site Class-18 extraction wrapping OMS_PushSubmit with degenerate TP/SL + required node_cfg (the silent-zero-fee structural close)]
// [CONTAINS]
//   - [FUNCTION]_[OMS_PushExitForSlot]
// [REFERENCE]_[DESIGN_SPEC]_[structural-fix-preferred-decision-framework]
// [REFERENCE]_[CLASS]_[18]
//======================================================================================================
// 4-site Class-18 helper extraction per the structural-fix-preferred
// gradient (structural fix when bug class can recur) +
// `DESIGN_SPECS/structural-fix-preferred-decision-framework.md`.
//
// PROBLEM: 4 callers issued the same 8-arg `OMS_PushSubmit` call with
// degenerate TP/SL (FPN_Zero) and ORDER_MARKET_SELL baked in — a recurring
// Class-18 mirror. Future signature changes to OMS_PushSubmit would require
// 4 site updates, and a future RecordX consumer would inevitably duplicate
// the 8-arg shape.
//
// FIX: extract a 6-arg helper that wraps OMS_PushSubmit with the
// market-sell + degenerate-TP/SL pattern baked in. Caller passes only the
// 6 args that actually vary across sites: slot/qty/strategy_id/event_price
// (and optional leg).
//
// 4 production callers (post-extraction):
//   1. `CoreFrameworks/EngineSharded.hpp` — drain_manual_closes
//      (GUI-driven force-close; with explicit leg arg under partials)
//   2. `CoreFrameworks/EngineSharded.hpp` — ML exit-predictor submit
//      (slow-path strategy thread when exit_predictor fires)
//   3. `CoreFrameworks/ControllerEventLoop.hpp` — TimeExitOneCore
//      (max_hold_ticks expired)
//   4. `CoreFrameworks/ControllerEventLoop.hpp` — FlattenAll
//      (WS staleness / kill-switch flatten-all)
//
// Site mismatched out (excluded from helper): EngineSharded's
// drain_with_submit mixed
// entry+exit branch uses pre-computed leg_tp + explicit intended_sl;
// structurally different (not a market-sell-with-zero-TP/SL shape).
//
// DISCIPLINE (per `function-struct-alignment-for-single-mov-access.md`):
//   - Helper is `inline` in header → compile-time-resolved offset folding
//   - Pass OMS by pointer (matches OMS_PushSubmit signature; single
//     mov-via-register for OMS access)
//   - FPN_Binary<F> + integer args pass via SysV registers; no stack churn
//   - Templated on <F> for compile-time inlining
//   - Forwards return bool from OMS_PushSubmit (caller can check)
//
// LATENCY: ZERO net change. Helper compiles to
// the SAME instructions as the prior inline call (inline keyword + same
// arg shape). Verified at code review; bench gate at v5.15.5.C.3 Phase 7.B
// captures drainer p99 for spot-check post-ship.
//======================================================================================================

#pragma once

#include "../CoreFrameworks/OrderManager.hpp"  // OrderManagerState<F>, OMS_PushSubmit, OrderType
#include "../FixedPoint/FixedPointN.hpp"        // FPN_Binary<F>, FPN_Zero<F>()

namespace tt {

//======================================================================
// [FUNCTION]_[OMS_PushExitForSlot]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[market-sell exit submit for a slot with degenerate TP/SL — node_cfg REQUIRED (the silent-zero-fee close); forwards OMS_PushSubmit's bool]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline bool OMS_PushExitForSlot(OrderManagerState<F>* oms,
                                 int16_t slot,
                                 Money qty,
                                 uint8_t strategy_id,
                                 Money event_price,
                                 uint8_t leg,
                                 const ::PerNodeCfg<F>* node_cfg) {
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — option (A refined): required-field ctor + optional assignments.
    // Helper bakes in ORDER_MARKET_SELL; caller varies the rest. intended_tp/intended_sl/_pad
    // take SubmitCommand default (FPN_Zero).
    SubmitCommand<F> cmd(tt::SlotIdx{(int16_t)slot}, ORDER_MARKET_SELL, qty, leg, node_cfg);
    cmd.strategy_id = strategy_id;
    cmd.event_price = event_price;
    return OMS_PushSubmit(oms, cmd);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Push a market-sell exit submit for `slot` with degenerate TP/SL.
// Wraps OMS_PushSubmit(ORDER_MARKET_SELL, qty, FPN_Zero, FPN_Zero, ...).
// Caller passes slot / qty / strategy_id / event_price / leg / node_cfg.
//
// v5.15.5.F.4c.3 WIP2d-1.B.1: `node_cfg` REQUIRED (no default). Closes silent-zero-fee
// class structurally — every helper caller must thread per-core cfg through. Helper
// forwards to OMS_PushSubmit; SubmitCommand carries node_cfg to drainer; drainer calls
// Order_BindPreResolved at OrderManager_Submit time → pre_resolved.fee_rate set.
//
// Returns: true on successful push to OMS submit_queue; false on
// invalid slot OR queue full. Callers should treat false as a soft
// error (existing inline-call sites mostly ignored the return value;
// helper preserves the same contract).
//======================================================================
// [END_FUNCTION]_[OMS_PushExitForSlot]
//======================================================================

}  // namespace tt
