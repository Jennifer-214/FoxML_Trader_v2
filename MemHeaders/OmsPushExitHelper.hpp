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
//   - [FUNCTION]_[OMS_PushExitResolvedQty]
//   - [FUNCTION]_[Node_RequestExit]
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
// Production callers (E.1.3 P4-pre-7, D-490 — the four former direct sites route through ONE kernel):
//   - OMS_PushExitResolvedQty (below) — the per-slot KERNEL; its callers are FlattenAll's loop, the
//     manual close (EngineSharded/SlowPath.hpp) and the composer's exit-request drain
//     (EngineCommon_DrainExitRequests). TimeExitOneCore and the exit-predictor arm no longer push:
//     they POST ExitReqs (Node_RequestExit, below) that the drain turns into this helper's command.
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
//
// E.1.3 P4-pre-7 (D-490): OMS_PushExitResolvedQty is the per-slot KERNEL layered on this helper
// (qty re-derived on the owner thread + the F-096 zero-qty guard); FlattenAll's loop, the manual
// close and the composer's exit-request drain call the kernel. Node_RequestExit is the node-side
// request push onto the node's own AggregatorState ring — the slow threads stop pushing
// submit_queues directly at the leaf's capital commit (the composer is the sole producer).
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

//======================================================================
// [FUNCTION]_[OMS_PushExitResolvedQty]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING]]
// [THREAD]_[[COMPOSER_WRITER]]
// [REFERENCE]_[DECISION]_[[D-490]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the per-slot exit KERNEL (E.1.3 P4-pre-7): re-derive qty from the slot on the OWNER thread, the F-096 zero-qty guard, then OMS_PushExitForSlot with the CARRIED sid / leg / price — ONE body under FlattenAll's loop, the manual close and the exit-request drain. Returns 1 pushed / 0 zero-qty skip / -1 queue full]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int OMS_PushExitResolvedQty(OrderManagerState<F>* oms,
                                   int16_t slot,
                                   uint8_t strategy_id,
                                   Money event_price,
                                   uint8_t leg,
                                   const ::PerNodeCfg<F>* node_cfg) {
    // qty is the ONE derived byte of the command: read on the thread that owns positions, at
    // push time — never carried across a ring (a carried qty can be stale by a partial fill).
    const Money qty = oms->portfolio.positions[slot].quantity;
    if (Money_IsZero(qty)) return 0;   // F-096: an emptied slot never becomes a zero-qty SELL (the manual-close / predictor guard, in ONE place)
    return OMS_PushExitForSlot(oms, slot, qty, strategy_id, event_price, leg, node_cfg) ? 1 : -1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OMS_PushExitResolvedQty]
//======================================================================

//======================================================================
// [FUNCTION]_[Node_RequestExit]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CONCURRENCY]]
// [THREAD]_[[SLOW_WRITER]]
// [SYNC]_[SPSC]
// [REFERENCE]_[DECISION]_[[D-490]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the node-side half of the exit request (E.1.3 P4-pre-7): push ONE ExitReq onto the node's OWN ring (1 request = 1 push, never coalesced — the drain's FIFO IS the HEAD order); a FULL ring is COUNTED (exit_requests_dropped) + LOUD (Health_Log WARN) and the request is dropped — the predicate re-fires next cadence. Returns 1 posted / 0 dropped]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int Node_RequestExit(SPSCRing<ExitReq, EXIT_REQ_RING_SIZE>* ring,
                            const ExitReq& req,
                            OrderManagerState<F>* oms,
                            int node_id) {
    if (SPSCRing_TryPush(ring, req)) return 1;
    // Producer-side drop: counted on the OMS atomic from the SLOW thread (the ring_full_fatal
    // producer-thread precedent — relaxed fetch_add, display-only ordering). Per drop, not
    // rate-limited: a full ring means the composer has not drained for EXIT_REQ_RING_SIZE
    // cadences — that stall is the loud fact, and it is CRITICAL-visible elsewhere already.
    const uint64_t dropped = oms->exit_requests_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
    Health_Log(HEALTH_WARN, "exit_request_ring_full", node_id,
               "exit-request ring (size=%d) full — request DROPPED (slot=%d reason=%u; total dropped %llu); "
               "the next slow cadence re-fires the predicate",
               (int)EXIT_REQ_RING_SIZE, (int)req.slot, (unsigned)req.reason, (unsigned long long)dropped);
    fprintf(stderr, "[node %d] exit_req_ring full, dropping request slot=%d reason=%u (total dropped %llu)\n",
            node_id, (int)req.slot, (unsigned)req.reason, (unsigned long long)dropped);
    return 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Node_RequestExit]
//======================================================================

}  // namespace tt
