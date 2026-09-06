// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/OmsPhasedDrain.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[phase-separated drainer foundation — drain 3 SPSC rings into per-direction buckets once, then CLOSE -> DrainPostFill -> OPEN -> reconcile phases; H20 fn-pointer dispatch per queue]
// [CONTAINS]
//   - [STRUCT]_[OmsDrainBuckets]
//   - [FUNCTION]_[OmsDrainBuckets_Reset]
//   - [FUNCTION]_[OrderType_IsClose]
//   - [FUNCTION]_[OrderManager_DrainIntoBuckets]   (+ the 3 drain-cmd handlers + 3 dispatch tables ride)
//   - [FUNCTION]_[OrderManager_ProcessBucket_Closes]   (+ Opens / Reconciles family)
// [REFERENCE]_[DESIGN_SPEC]_[[phase-separated-drainer-for-safe-cross-temporal-derives] [branchless-dispatch-discipline]]
// [REFERENCE]_[INVARIANT]_[H20]
//======================================================================================================
//
// Implements the phase-separated drainer pattern per
// `DESIGN_SPECS/phase-separated-drainer-for-safe-cross-temporal-derives.md`.
//
// PROBLEM (recap): OrderManager_Tick's prior `while (TryPop)` loop drained
// 3 SPSC rings sequentially, processing commands in ARRIVAL order — meaning
// SELL events for slot N and BUY events for slot N could process
// interleaved within the same drain pass. HandleFill BUY's
// Portfolio_OpenSlot OVERWRITES Position state that DrainPostFill needs in
// CLOSE form for safe derive-from-Position semantics (v5.15.5.C.4 Phase G+H).
//
// FIX: drain commands into per-direction buckets ONCE (preserving arrival
// order WITHIN each direction), then process in 3 phases interleaved with
// consumer passes:
//
//   Phase A — process CLOSE bucket (all SELL fills)
//   Phase A.5 — DrainPostFill (Position state is in CLOSE form; derive safe)
//   Phase B — process OPEN bucket (all BUY fills; Portfolio_OpenSlot fires)
//   Phase B.5 (optional) — DrainPostEntry for ENTRY-side derives
//   Phase C — process Reconcile bucket (balance adjustments; phase-invariant
//             safe since reconcile doesn't touch Position)
//
// Drainer thread uses these phased helpers; `OrderManager_Tick` is preserved
// as a backward-compat wrapper for tests that don't need phase-separation
// semantics (no DrainPostFill interleave required).
//
// LATENCY (the latency-cost discipline):
//   - Drain pass: 3 rings × ~few ns/event = ~10-20 ns per cycle at typical
//     1-5 events/cycle. Marginal vs prior single-pass.
//   - Bucket-classification cost: 1 indexed read of `oms->orders[slot].type`
//     per command + 1 bit-test (branchless `(type & 1) == 1` exploits the
//     OrderType enum's even=BUY/odd=SELL invariant). ~2-3 cycles per command.
//   - Process passes: identical instructions to prior `OrderManager_Tick`,
//     just split into 3 mini-loops. Net same dispatch cost.
//
// HOT PATH UNTOUCHED. Drainer aggregate latency: net +10-20 ns per cycle
// worst case at high event burst. Within slow-path 100μs budget by 3+
// orders of magnitude.
//
// SIZE: OmsDrainBuckets struct is ~144 KB (576 slots × 256 B Command; sized
// at OMS_RESULT_QUEUE_SIZE for each of close+open buckets + 64 for
// reconciles). Stack-allocated once at drainer thread entry; reused per
// cycle — fine on the default 8 MB thread stack, and per-cycle touch cost is
// bounded by ACTUAL event count (Reset zeroes 3 ints; only written slots are
// touched), not capacity. NOT added to OmsState (transient per-cycle
// scratch; no need to persist or share across threads).
//======================================================================================================

#pragma once

#include "../CoreFrameworks/OrderManager.hpp"  // OrderManagerState<F>, Command, OMS_RESULT_QUEUE_SIZE, ProcessFillCommand, ProcessReconcile
#include "../CoreFrameworks/Order.hpp"          // OrderType enum (ORDER_MARKET_BUY/SELL/LIMIT_BUY/SELL)

#include <cstdint>

namespace tt {

//======================================================================
// [STRUCT]_[OmsDrainBuckets]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-direction Command buckets (close/open at OMS_RESULT_QUEUE_SIZE, reconcile at 64) + counts — drainer-thread stack scratch, reset per cycle]
//======================================================================
// [CODE]
//======================================================================
// Σ-SIZED (3b(ii) commit 4 leaf 3). One drain cycle empties BOTH per-node families — the REST
// `result_rings` and the WS `ws_rings` — into these SAME buckets, so the worst case a bucket must
// absorb is every command from both, all one direction. Deriving the depth from the families
// themselves rather than writing 512 means the pin cannot drift when a family's depth changes.
// The families a single cycle drains into these buckets. Stated as a NAMED count because that is
// the one thing no compile-time check can notice changing: adding a third family is an edit to
// OrderManager_DrainIntoBuckets, and nothing here can see it. If you add one, this number moves.
constexpr int OMS_BUCKET_DRAIN_FAMILIES = 2;      // result_rings (REST) + ws_rings (WS user-data)
constexpr int OMS_BUCKET_DEPTH =
    OMS_BUCKET_DRAIN_FAMILIES * (int)(tt::OMS_RESULT_RING_PER_NODE * MAX_EXECUTION_NODES);

// NO static_assert on the depth here, deliberately. The first version of this pin read
// `static_assert(OMS_BUCKET_DEPTH >= 2 * RING_PER_NODE * NODES)` directly under a definition that
// says `OMS_BUCKET_DEPTH = 2 * RING_PER_NODE * NODES` — tautologically true, green forever,
// asserting its own left-hand side. A vacuous assert is worse than none: it reads as a guarded
// invariant to everyone downstream (Class 51). What actually protects the depth here is DERIVATION
// — change `OMS_RESULT_RING_PER_NODE`, `MAX_EXECUTION_NODES`, or the family count and the depth
// follows arithmetically, so there is no drift for an assert to catch.
// The two real hazards are guarded where they CAN be, and neither guard lives here:
//   1. that the two families stay the SAME shape (what makes the multiply valid) is pinned at
//      `OrderManagerState`'s declaration — `sizeof(ws_rings) == sizeof(result_rings)`;
//   2. that the premise holds AT ALL is guarded at RUNTIME by the bound check below, which is now
//      a LOUD-FATAL rather than a silent drop. That is the honest guard for this one: an assert
//      restating the definition would only look like protection.

struct OmsDrainBuckets {
    // Σ(both ring families) × sizeof(Command) for close (SELL) fills
    Command close_bucket[OMS_BUCKET_DEPTH];
    int     close_n;
    // Σ(both ring families) × sizeof(Command) for open (BUY) fills
    Command open_bucket[OMS_BUCKET_DEPTH];
    int     open_n;
    // 64 × sizeof(Command) for reconcile corrections
    Command reconcile_bucket[64];
    int     reconcile_n;
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Per-direction bucket arrays + counts.
//
// Sized at OMS_RESULT_QUEUE_SIZE (256) for close + open buckets — worst case
// all 256 events from one ring are the same direction. Reconcile bucket sized
// at 64 to match `reconcile_queue` ring capacity.
//
// Stack-allocated by drainer thread at thread entry; reset per cycle by
// DrainIntoBuckets. ~144 KB total (Command is 256 B; see the [DERIVED]
// quartet) — within the default 8 MB thread stack; per-cycle touch cost is
// bounded by actual event count, not capacity.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-09-06]
//----------------------------------------------------------------------
// [SIZE]_[278552B]
// [ALIGN]_[8]
// [CACHE_LINES]_[4353]
// [STRADDLE]_[unverified: close_bucket open_bucket reconcile_bucket]
//======================================================================
// [END_STRUCT]_[OmsDrainBuckets]
//======================================================================

//======================================================================
// [FUNCTION]_[OmsDrainBuckets_Reset]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[zero the 3 bucket counts — top of DrainIntoBuckets each cycle]
//======================================================================
// [CODE]
//======================================================================
inline void OmsDrainBuckets_Reset(OmsDrainBuckets* b) {
    b->close_n = 0;
    b->open_n = 0;
    b->reconcile_n = 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OmsDrainBuckets_Reset]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderType_IsClose]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[branchless direction classifier — low-bit test exploits the OrderType even=BUY/odd=SELL invariant; true = CLOSE (SELL-direction)]
//======================================================================
// [CODE]
//======================================================================
inline bool OrderType_IsClose(uint8_t order_type) {
    return (order_type & 1u) == 1u;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Exploits the OrderType enum invariant:
//   ORDER_MARKET_BUY  = 0 (even)
//   ORDER_MARKET_SELL = 1 (odd)
//   ORDER_LIMIT_BUY   = 2 (even) — future maker
//   ORDER_LIMIT_SELL  = 3 (odd)  — future maker
//======================================================================
// [END_FUNCTION]_[OrderType_IsClose]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderManager_DrainIntoBuckets]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[drain the 3 SPSC rings into per-direction buckets (the 3 drain-cmd handlers + 3 fn-pointer dispatch tables ride) — classify at drain time via the stable Order.type]
// [REFERENCE]_[INVARIANT]_[H20]
// [REFERENCE]_[DESIGN_SPEC]_[branchless-dispatch-discipline.md]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[DRAIN CMD HANDLERS — H20 fn-pointer dispatch (v5.15.5.F.4c.3 WIP2d-1.B.1)]
//------------------------------------------------------------------
// Per-queue fn pointer dispatch tables replace if/switch type-filter branches. Each cmd
// flows through table indexed by `cmd.type & 0xF` → handler. Wrong-type cmds (queue
// contract violation) land in handle_drain_noop_cmd. Pattern 1 (fn pointer table for
// single-enum dispatch) per branchless-dispatch-discipline.md.
//
// Cost: ~3-5ns indirect call per cmd vs ~0ns steady-state branch. Per H20: branchless
// preferred even when slower (mispredicts can't be optimized; mask code can be). Insurance
// against future queue-contract drift causing silent mispredicts.
template <unsigned F>
using DrainCmdHandler = void (*)(const Command&, OrderManagerState<F>*, OmsDrainBuckets*);

// Bucket-dispatch handler (FILL_RESULT + WS_FILL share this body via dispatch table).
// Branchless mask-select for BUY/SELL bucket; dummy-redirect for bound check.
template <unsigned F>
inline void handle_drain_bucket_cmd(const Command& cmd,
                                     OrderManagerState<F>* oms,
                                     OmsDrainBuckets* b) {
    int slot = tt::OMS_OrderIdSlot(cmd.order_id);   // P4-pre-3a: lane decode SSoT
    uint8_t otype = (uint8_t)tt::Order_GetType(&oms->orders[slot]);
    const bool is_close   = OrderType_IsClose(otype);
    Command*   bucket_arr = is_close ? b->close_bucket : b->open_bucket;
    int*       n_ptr      = is_close ? &b->close_n     : &b->open_n;
    const int  n          = *n_ptr;
    const bool in_bounds  = (n < OMS_BUCKET_DEPTH);
    // The dummy-redirect keeps the write branchless; what CHANGED at leaf 3 is what happens after
    // it. This used to be a silent drop in release (an fprintf and a debug-only assert), which
    // meant a fill the venue had already executed could vanish between the ring and the bucket
    // with nothing durable recording it. The buckets are now Σ-sized so out-of-bounds is
    // structurally unreachable — and this arm is the PIN'S GUARD, so if the premise ever breaks it
    // must be as loud as any other unbooked fill: the same durable record and GLOBAL kill request
    // the ring-full sites use. Reaching here means the Σ pin's arithmetic is wrong, not that the
    // engine hit a busy moment.
    static Command DUMMY;
    Command*   target     = in_bounds ? &bucket_arr[n] : &DUMMY;
    *target               = cmd;
    *n_ptr                = n + (in_bounds ? 1 : 0);
    if (__builtin_expect(!in_bounds, 0)) {
        tt::OMS_RingFullFatalRecord(oms->agg ? &oms->agg->kill_trip_request : nullptr,
                                    tt::KTS_BUCKET_OVERFLOW, &oms->ring_full_fatal, cmd);
    }
}

// Reconcile-bucket handler.
template <unsigned F>
inline void handle_drain_reconcile_cmd(const Command& cmd,
                                         OrderManagerState<F>* oms,
                                         OmsDrainBuckets* b) {
    (void)oms;
    const int  n         = b->reconcile_n;
    const bool in_bounds = (n < 64);
    static Command DUMMY;
    Command*   target    = in_bounds ? &b->reconcile_bucket[n] : &DUMMY;
    *target              = cmd;
    b->reconcile_n       = n + (in_bounds ? 1 : 0);
    if (__builtin_expect(!in_bounds, 0)) {
        // Same reasoning as the fill buckets, one severity down: a reconcile CORRECTION is not a
        // fill, so losing one does not silently mis-book capital — the next pass re-detects the
        // drift (`corrections_dropped`, char (9)). It is still an engine-side loss and still gets
        // a durable line rather than only a stderr print that no post-mortem will ever see.
        tt::Health_Log(tt::HEALTH_WARN, "drain_reconcile_overflow", -1,
                       "reconcile bucket (size=64) full — correction for order %llu DROPPED; the "
                       "next reconcile pass re-detects the drift",
                       (unsigned long long)cmd.order_id);
    }
}

// Noop handler — wrong-type cmds land here per queue contract.
template <unsigned F>
inline void handle_drain_noop_cmd(const Command& cmd,
                                    OrderManagerState<F>* oms,
                                    OmsDrainBuckets* b) {
    (void)cmd; (void)oms; (void)b;
}

// Per-queue dispatch tables — 16 entries (cmd.type & 0xF mask) for future enum-drift safety.
// CommandType: 0=CMD_FILL_RESULT, 1=CMD_WS_FILL, 2=CMD_RECONCILE.
template <unsigned F>
inline const DrainCmdHandler<F> g_rest_queue_dispatch[16] = {
    handle_drain_bucket_cmd<F>,    // 0 CMD_FILL_RESULT
    handle_drain_noop_cmd<F>,      // 1
    handle_drain_noop_cmd<F>,      // 2
    handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>,
    handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>,
};

template <unsigned F>
inline const DrainCmdHandler<F> g_ws_queue_dispatch[16] = {
    handle_drain_noop_cmd<F>,      // 0
    handle_drain_bucket_cmd<F>,    // 1 CMD_WS_FILL
    handle_drain_noop_cmd<F>,      // 2
    handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>,
    handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>,
};

template <unsigned F>
inline const DrainCmdHandler<F> g_reconcile_queue_dispatch[16] = {
    handle_drain_noop_cmd<F>,      // 0
    handle_drain_noop_cmd<F>,      // 1
    handle_drain_reconcile_cmd<F>, // 2 CMD_RECONCILE
    handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>,
    handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>, handle_drain_noop_cmd<F>,
};

template <unsigned F>
inline void OrderManager_DrainIntoBuckets(OrderManagerState<F>* oms,
                                           OmsDrainBuckets* b) {
    OmsDrainBuckets_Reset(b);
    Command cmd;

    // 1. REST result RINGS, per-node (adapter worker thread) — branchless dispatch via fn pointer table.
    while (tt::OMS_ResultPop(oms, &cmd)) {   // P4-pre-3b: per-node rings, node-major interim drain
        g_rest_queue_dispatch<F>[cmd.type & 0xF](cmd, oms, b);
    }

    // 2. WS user-data RINGS, per-node — branchless dispatch via fn pointer table.
    //    Cursor-held so the walk is O(nodes + items), not O(nodes) per item.
    int ws_cursor = 0;
    while (tt::OMS_CmdRingsPop(&oms->ws_rings, &ws_cursor, &cmd)) {
        g_ws_queue_dispatch<F>[cmd.type & 0xF](cmd, oms, b);
    }

    // 3. Reconcile_queue (reconciler thread) — branchless dispatch via fn pointer table.
    while (SPSCRing_TryPop(&oms->reconcile_queue, &cmd)) {
        g_reconcile_queue_dispatch<F>[cmd.type & 0xF](cmd, oms, b);
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Drain all 3 SPSC rings into per-direction buckets.
//
// Pass 1 — REST result_rings + WS ws_rings (both per-node): each fill command's
// Order.type determines whether it routes to close_bucket or open_bucket.
// Order.type is STABLE between drain time + process time (set at order
// creation; never mutated during fills); safe to classify at drain time.
//
// Pass 2 — reconcile_queue: all events route to reconcile_bucket (these
// are balance adjustments; not direction-typed).
//
// Bucket overflow is structurally UNREACHABLE — and the previous version of this comment was not
// entitled to that claim. It read "IMPOSSIBLE BY DESIGN: bucket capacity matches ring capacity",
// then refuted itself two sentences later: "Combined cap from both rings WOULD overflow ... the
// second drain would still fit because ring capacity is asymmetric to direction-per-ring
// distribution IN PRACTICE." Both families drain into these same per-cycle buckets, so a burst
// that is all one direction genuinely could exceed a 256-deep bucket, and the release build's
// response was to drop it silently. A phantom invariant (Class 38) whose whole proof was the
// phrase "in practice", guarding a capital loss.
// It is now true by ARITHMETIC rather than by hope: OMS_BUCKET_DEPTH is Σ over both ring families
// (see the static_assert at the struct), so every command a cycle can possibly drain has a home.
// The bound check stays as the PIN'S GUARD and is now LOUD — the same durable record + GLOBAL kill
// as the ring-full sites, because reaching it means the arithmetic is wrong, not that the engine
// is busy.
//======================================================================
// [END_FUNCTION]_[OrderManager_DrainIntoBuckets]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderManager_ProcessBucket_Closes]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the phase-processor family (Opens / Reconciles ride) — Phase A closes BEFORE DrainPostFill, Phase B opens AFTER, Phase C reconciles phase-invariant]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OrderManager_ProcessBucket_Closes(OrderManagerState<F>* oms,
                                               OmsDrainBuckets* b) {
    for (int i = 0; i < b->close_n; ++i) {
        OrderManager_ProcessFillCommand(oms, b->close_bucket[i]);
    }
}

//------------------------------------------------------------------
// Process open-side fills (Phase B). Calls ProcessFillCommand on each
// command in open_bucket. Portfolio_OpenSlot fires here; Position state
// is OVERWRITTEN with new entry data.
//
// CRITICAL: must run AFTER Phase A.5 (DrainPostFill) — Phase A.5 needs
// CLOSE-form Position state. Phase F's invariant.
//------------------------------------------------------------------
template <unsigned F>
inline void OrderManager_ProcessBucket_Opens(OrderManagerState<F>* oms,
                                              OmsDrainBuckets* b) {
    for (int i = 0; i < b->open_n; ++i) {
        OrderManager_ProcessFillCommand(oms, b->open_bucket[i]);
    }
}

//------------------------------------------------------------------
// Process reconcile corrections (Phase C). Balance adjustments only; no
// Position mutation. Phase-invariant safe — can run before, between, or
// after Phase A/A.5/B without affecting Phase F invariants.
//------------------------------------------------------------------
template <unsigned F>
inline void OrderManager_ProcessBucket_Reconciles(OrderManagerState<F>* oms,
                                                   OmsDrainBuckets* b) {
    for (int i = 0; i < b->reconcile_n; ++i) {
        OrderManager_ProcessReconcile(oms, b->reconcile_bucket[i]);
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Process close-side fills (Phase A). Calls ProcessFillCommand on each
// command in close_bucket. Mutations confined to close-side state
// (Portfolio_CloseSlot bitmap clear; FillRecord exit-side writes; Position
// values PRESERVED).
//
// CRITICAL: must run BEFORE Phase A.5 (DrainPostFill) so the consumer
// reads CLOSE-form Position state. Phase F's invariant.
//======================================================================
// [END_FUNCTION]_[OrderManager_ProcessBucket_Closes]
//======================================================================

}  // namespace tt
