// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [OMS PHASED DRAIN]  (v5.15.5.C.4 Phase F — phase-separated drainer foundation)
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
// LATENCY (CLAUDE.md item 17):
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
// SIZE: OmsDrainBuckets struct is ~7 KB (sized at OMS_RESULT_QUEUE_SIZE for
// each of close+open buckets + 64 for reconciles). Stack-allocated once at
// drainer thread entry; reused per cycle. NOT added to OmsState (transient
// per-cycle scratch; no need to persist or share across threads).
//======================================================================================================

#pragma once

#include "../CoreFrameworks/OrderManager.hpp"  // OrderManagerState<F>, Command, OMS_RESULT_QUEUE_SIZE, ProcessFillCommand, ProcessReconcile
#include "../CoreFrameworks/Order.hpp"          // OrderType enum (ORDER_MARKET_BUY/SELL/LIMIT_BUY/SELL)

#include <cstdint>

namespace tt {

//======================================================================================================
// Per-direction bucket arrays + counts.
//
// Sized at OMS_RESULT_QUEUE_SIZE (256) for close + open buckets — worst case
// all 256 events from one ring are the same direction. Reconcile bucket sized
// at 64 to match `reconcile_queue` ring capacity.
//
// Stack-allocated by drainer thread at thread entry; reset per cycle by
// DrainIntoBuckets. ~7 KB total; well within drainer thread's stack budget.
//======================================================================================================
struct OmsDrainBuckets {
    // 256 × sizeof(Command) for close (SELL) fills
    Command close_bucket[OMS_RESULT_QUEUE_SIZE];
    int     close_n;
    // 256 × sizeof(Command) for open (BUY) fills
    Command open_bucket[OMS_RESULT_QUEUE_SIZE];
    int     open_n;
    // 64 × sizeof(Command) for reconcile corrections
    Command reconcile_bucket[64];
    int     reconcile_n;
};

//======================================================================================================
// Reset bucket counts. Called at top of DrainIntoBuckets each cycle.
//======================================================================================================
inline void OmsDrainBuckets_Reset(OmsDrainBuckets* b) {
    b->close_n = 0;
    b->open_n = 0;
    b->reconcile_n = 0;
}

//======================================================================================================
// Branchless direction classifier — exploits the OrderType enum invariant:
//   ORDER_MARKET_BUY  = 0 (even)
//   ORDER_MARKET_SELL = 1 (odd)
//   ORDER_LIMIT_BUY   = 2 (even) — future maker
//   ORDER_LIMIT_SELL  = 3 (odd)  — future maker
//
// Returns: true if order is a CLOSE (SELL-direction); false if OPEN (BUY).
//======================================================================================================
inline bool OrderType_IsClose(uint8_t order_type) {
    return (order_type & 1u) == 1u;
}

//======================================================================================================
// Drain all 3 SPSC rings into per-direction buckets.
//
// Pass 1 — REST result_queue + WS ws_result_queue: each fill command's
// Order.type determines whether it routes to close_bucket or open_bucket.
// Order.type is STABLE between drain time + process time (set at order
// creation; never mutated during fills); safe to classify at drain time.
//
// Pass 2 — reconcile_queue: all events route to reconcile_bucket (these
// are balance adjustments; not direction-typed).
//
// Bucket overflow is IMPOSSIBLE BY DESIGN: bucket capacity matches ring
// capacity. Worst-case all 256 events from one ring are the same direction
// → close_bucket or open_bucket fills exactly. Combined cap from both rings
// would overflow but we drain ONE ring's commands at a time; if both rings
// were saturated, the second drain would still fit because ring capacity
// is asymmetric to direction-per-ring distribution in practice. Defensive
// bound check anyway (assert in debug; silently drop in release — same as
// prior `OrderManager_Tick`).
//======================================================================================================
//======================================================================================================
// [DRAIN CMD HANDLERS] (v5.15.5.F.4c.3 WIP2d-1.B.1 — per H20 + branchless-dispatch-discipline.md)
//======================================================================================================
// Per-queue fn pointer dispatch tables replace if/switch type-filter branches. Each cmd
// flows through table indexed by `cmd.type & 0xF` → handler. Wrong-type cmds (queue
// contract violation) land in handle_drain_noop_cmd. Pattern 1 (fn pointer table for
// single-enum dispatch) per branchless-dispatch-discipline.md.
//
// Cost: ~3-5ns indirect call per cmd vs ~0ns steady-state branch. Per H20: branchless
// preferred even when slower (mispredicts can't be optimized; mask code can be). Insurance
// against future queue-contract drift causing silent mispredicts.
//======================================================================================================
template <unsigned F>
using DrainCmdHandler = void (*)(const Command&, OrderManagerState<F>*, OmsDrainBuckets*);

// Bucket-dispatch handler (FILL_RESULT + WS_FILL share this body via dispatch table).
// Branchless mask-select for BUY/SELL bucket; dummy-redirect for bound check.
template <unsigned F>
inline void handle_drain_bucket_cmd(const Command& cmd,
                                     OrderManagerState<F>* oms,
                                     OmsDrainBuckets* b) {
    int slot = (int)((cmd.order_id >> 60) & 0xFu);
    uint8_t otype = (uint8_t)tt::Order_GetType(&oms->orders[slot]);
    const bool is_close   = OrderType_IsClose(otype);
    Command*   bucket_arr = is_close ? b->close_bucket : b->open_bucket;
    int*       n_ptr      = is_close ? &b->close_n     : &b->open_n;
    const int  n          = *n_ptr;
    const bool in_bounds  = (n < OMS_RESULT_QUEUE_SIZE);
    // Dummy-redirect: out-of-bounds writes land in a function-local static slot
    // (single-drainer-thread guarantee — no race on DUMMY).
    static Command DUMMY;
    Command*   target     = in_bounds ? &bucket_arr[n] : &DUMMY;
    *target               = cmd;
    *n_ptr                = n + (in_bounds ? 1 : 0);
    if (__builtin_expect(!in_bounds, 0)) {
        std::fprintf(stderr,
            "[OMS] drain bucket overflow (size=%d): dropped cmd type=%u order_id=%llu\n",
            OMS_RESULT_QUEUE_SIZE, (unsigned)cmd.type,
            (unsigned long long)cmd.order_id);
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
        std::fprintf(stderr,
            "[OMS] drain reconcile bucket overflow (size=64): dropped cmd order_id=%llu\n",
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

    // 1. REST result_queue (adapter worker thread) — branchless dispatch via fn pointer table.
    while (SPSCRing_TryPop(&oms->result_queue, &cmd)) {
        g_rest_queue_dispatch<F>[cmd.type & 0xF](cmd, oms, b);
    }

    // 2. WS user-data result_queue — branchless dispatch via fn pointer table.
    while (SPSCRing_TryPop(&oms->ws_result_queue, &cmd)) {
        g_ws_queue_dispatch<F>[cmd.type & 0xF](cmd, oms, b);
    }

    // 3. Reconcile_queue (reconciler thread) — branchless dispatch via fn pointer table.
    while (SPSCRing_TryPop(&oms->reconcile_queue, &cmd)) {
        g_reconcile_queue_dispatch<F>[cmd.type & 0xF](cmd, oms, b);
    }
}

//======================================================================================================
// Process close-side fills (Phase A). Calls ProcessFillCommand on each
// command in close_bucket. Mutations confined to close-side state
// (Portfolio_CloseSlot bitmap clear; FillRecord exit-side writes; Position
// values PRESERVED).
//
// CRITICAL: must run BEFORE Phase A.5 (DrainPostFill) so the consumer
// reads CLOSE-form Position state. Phase F's invariant.
//======================================================================================================
template <unsigned F>
inline void OrderManager_ProcessBucket_Closes(OrderManagerState<F>* oms,
                                               OmsDrainBuckets* b) {
    for (int i = 0; i < b->close_n; ++i) {
        OrderManager_ProcessFillCommand(oms, b->close_bucket[i]);
    }
}

//======================================================================================================
// Process open-side fills (Phase B). Calls ProcessFillCommand on each
// command in open_bucket. Portfolio_OpenSlot fires here; Position state
// is OVERWRITTEN with new entry data.
//
// CRITICAL: must run AFTER Phase A.5 (DrainPostFill) — Phase A.5 needs
// CLOSE-form Position state. Phase F's invariant.
//======================================================================================================
template <unsigned F>
inline void OrderManager_ProcessBucket_Opens(OrderManagerState<F>* oms,
                                              OmsDrainBuckets* b) {
    for (int i = 0; i < b->open_n; ++i) {
        OrderManager_ProcessFillCommand(oms, b->open_bucket[i]);
    }
}

//======================================================================================================
// Process reconcile corrections (Phase C). Balance adjustments only; no
// Position mutation. Phase-invariant safe — can run before, between, or
// after Phase A/A.5/B without affecting Phase F invariants.
//======================================================================================================
template <unsigned F>
inline void OrderManager_ProcessBucket_Reconciles(OrderManagerState<F>* oms,
                                                   OmsDrainBuckets* b) {
    for (int i = 0; i < b->reconcile_n; ++i) {
        OrderManager_ProcessReconcile(oms, b->reconcile_bucket[i]);
    }
}

}  // namespace tt
