// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/OrderManager.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[single-owner OMS — the drainer-thread order table + async adapter routing + fill handling + the money books]
// [DIAGRAM]
//   drainer thread                          adapter worker thread(s)
//   ──────────────                          ────────────────────────
//   OrderManager_Submit                     pop from adapter queue
//      ├─> allocate slot                    ├─> call BinanceOrderAPI on
//      ├─> mark ORDER_SUBMITTED             │   per-thread instance
//      └─> adapter.submit_market_*          ├─> bracket with steady_clock
//          (returns immediately)            ├─> build OrderResult
//                                           └─> invoke callback
//                                                ├─> push CMD_FILL_RESULT
//                                                │   into oms->result_queue
//                                                └─> return immediately
//
//   OrderManager_Tick (drainer)             (worker keeps looping)
//      └─> drain result_queue
//          ├─> look up order by id
//          ├─> mark FILLED or REJECTED
//          └─> free slot
// [CONTAINS]
//   - [ENUM]_[CommandType]
//   - [STRUCT]_[SubmitCommand]
//   - [STRUCT]_[OrderManagerState]
//   - [FUNCTION]_[OrderManager_FillResultCallback]
//   - [FUNCTION]_[OrderManager_Init]
//   - [FUNCTION]_[OrderManager_Submit]
//   - [FUNCTION]_[OMS_PushSubmit]
//   - [FUNCTION]_[OMS_DrainSubmit]
//   - [FUNCTION]_[OrderManager_AccountMakerTakerFee]
//   - [FUNCTION]_[OMS_GuardTakerBoundFeeBasis]
//   - [FUNCTION]_[handle_buy_fill]
//   - [FUNCTION]_[handle_sell_fill]
//   - [FUNCTION]_[OrderManager_HandleFill]
//   - [FUNCTION]_[OrderManager_ProcessFillCommand]
//   - [FUNCTION]_[OMS_OpenPositionCost]
//   - [FUNCTION]_[OMS_ExpectedFreeCash]
//   - [FUNCTION]_[OrderManager_ProcessReconcile]
//   - [FUNCTION]_[OrderManager_Tick]
//   - [FUNCTION]_[OrderManager_Shutdown]
//   - [FUNCTION]_[OrderManager_OpenCalibrationLog]
//======================================================================================================
// Single-owner OMS for the per-core sharded engine. Owns the in-flight
// order table and routes submissions through an ExchangeAdapter.
//
// The drainer never blocks on the network. The worker thread (or thread
// pool) handles all REST traffic. Drainer cycle drops from worst case
// ~800 ms (4 cores × 200 ms blocking REST in phase 01) to sub-µs
// (drainer just enqueues to the adapter and returns).
//
// CONCURRENCY MODEL:
//   The order table is owned by exactly one thread — the drainer thread,
//   which is the only caller of Submit and Tick. The result_queue is
//   SPSC: the adapter worker is the sole producer (with worker_count==1)
//   and the drainer's Tick is the sole consumer. No locks anywhere.
//
//   When scaling to worker_count > 1, replace result_queue with a real
//   MPSC ring or per-worker queues. The current SPSCRing breaks under
//   multiple producers. See plans/oms/02_async_submission/plan.md.
//
// PAPER MODE:
//   Submit short-circuits — bumps total_submitted and total_filled, never
//   touches the order table or the adapter, returns the next id. The
//   result_queue stays empty so Tick is a no-op. Zero adapter cost in
//   paper mode.
//
// Naming clarification:
//   This is the OMS for live exchange orders. It has nothing to do with
//   MemHeaders/PoolAllocator.hpp:OrderPool, which is the buy gate's order
//   intent pool from the legacy single-core engine.
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"
#include "../Limits.hpp"
#include "IndexSpaces.hpp"   // SlotIdx / NodeIdx / NodeArray (D-438)
#include "ExchangeAdapter.hpp"
#include "Order.hpp"
#include "OrderEventLog.hpp"
#include "Portfolio.hpp"
#include "ShardedTradeLog.hpp"
#include "SPSCRing.hpp"
#include "../MemHeaders/HealthLog.hpp"   // D-479 — OMS_RingFullFatalRecord's durable CRITICAL line (any-thread-safe)
#include "NodeState.hpp"   // P3-b (D-444) — FillEvent/EmitRecord/AggregatorState for the leaf emit path (leaf includes only; cycle-free)
#include "../DataStream/CalibLogColRegistry.hpp"   // v5.14.10.D — FOREACH_CALIB_LOG_COL registry (closes TECH_DEBT-010)
#include "../MemHeaders/OmsStateFlagRegistry.hpp"  // v5.15.5.C.2 (S3a) — FOREACH_OMS_STATE_FLAG bitmap cohort
#include "../MemHeaders/OmsExitPredictorMetaRegistry.hpp"  // v5.15.5.C.2.1 (LOW-2) — FOREACH_OMS_META_SLOT multi-bit cohort
#include "../MemHeaders/OmsFieldRegistry.hpp"              // v5.15.5.C.3 (Phase 3) — canonical FOREACH_OMS_FIELD + projections
#include "../ML_Headers/NodeModelZoo.hpp"                  // v5.15.5.F.4d Step 7 § F — EnsembleModelZoo<F>* for real_on_exit_calibration ezoo_ref cast (transitively pulls PerNodeCfg<F> + BANDIT_MAX_ARMS + NUM_REGIMES + Bandit_GetProbabilities)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace tt {

//======================================================================
// [ENUM]_[CommandType]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[command-queue discriminator — external threads push Commands; OrderManager_Tick drains on the drainer]
//======================================================================
// [CODE]
//======================================================================
enum CommandType : uint8_t {
    CMD_FILL_RESULT = 0,   // from adapter worker (REST ACK or full fill)
    CMD_WS_FILL     = 1,   // from user data websocket (real-time fill)
    CMD_RECONCILE   = 2,   // from reconciliation poller (phase 05)
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// External threads push state updates as Commands; OrderManager_Tick
// drains them on the drainer thread. Phase 02 only uses CMD_FILL_RESULT
// (from the adapter worker thread). Phase 03 will add CMD_RECONCILE
// (from the reconciliation poller). Phase 04 may add CMD_USER_FILL
// (from the user-data websocket).
//======================================================================
// [END_ENUM]_[CommandType]
//======================================================================

//======================================================================
// [STRUCT]_[Command]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CONCURRENCY]]
// [THREAD]_[[WORKER_WRITER] [DRAINER_READER]]
// [SYNC]_[SPSC]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[a drainer-inbound command POD (CMD_FILL_RESULT etc.) — type + order_id + OrderResult; non-templated so the SPSC ring slots stay Money-width-independent]
//======================================================================
// [CODE]
//======================================================================
// Non-templated POD so the SPSCRing slots stay self-contained regardless
// of Money width.
struct Command {
    uint8_t      type;
    uint8_t      _pad0[7];
    uint64_t     order_id;
    OrderResult  result;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[256B]
// [ALIGN]_[8]
// [CACHE_LINES]_[4]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[Command]
//======================================================================

constexpr size_t OMS_RESULT_QUEUE_SIZE = 256;
// P4-pre-3b: the per-node result ring depth. PARTITIONS the total above rather than
// replicating it — see the sizing rationale at the `result_rings` declaration.
constexpr size_t OMS_RESULT_RING_PER_NODE = OMS_RESULT_QUEUE_SIZE / MAX_EXECUTION_NODES;
static_assert(OMS_RESULT_RING_PER_NODE * MAX_EXECUTION_NODES == OMS_RESULT_QUEUE_SIZE,
              "per-node result depth must PARTITION the old total exactly (capacity conserved, "
              "struct does not grow) — a non-divisor silently changes the OMS stack footprint");
// The floor is MAX_INFLIGHT_ORDERS, not a hand-picked small number, and it is EXACT today:
// 256 / 16 nodes = 16 per node, and the order pool is 16. Zero margin is the point — it is the
// conjunction that makes a paper-synth ring-full UNREACHABLE, so the pin must break the build the
// moment any of the three constants moves rather than let the paper path acquire a live fatal arm:
//   (1) the pool bound — at most MAX_INFLIGHT_ORDERS orders can be in flight at once, so at most
//       that many synthetic results can exist before one is consumed;
//   (2) every consumer drains its rings to EMPTY before its OMS_DrainSubmit, so a cycle starts
//       with room for the whole pool; and
//   (3) a pool-full submit pushes NOTHING (it is rejected before a result is synthesized).
// Was `>= 4` (a floor argued from "a node owns <=2 slots + bursts"), which is true but far weaker
// than what the paper path actually relies on.
static_assert(OMS_RESULT_RING_PER_NODE >= MAX_INFLIGHT_ORDERS,
              "per-node ring depth must cover the WHOLE order pool: with (1) at most "
              "MAX_INFLIGHT_ORDERS in flight, (2) every consumer draining to empty before its "
              "DrainSubmit, and (3) a pool-full submit pushing nothing, a paper-synth ring-full is "
              "structurally unreachable — and ONLY while this holds. Raising MAX_INFLIGHT_ORDERS or "
              "MAX_EXECUTION_NODES, or lowering OMS_RESULT_QUEUE_SIZE, makes it reachable again.");

//======================================================================
// [STRUCT]_[OmsCmdRings]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CONCURRENCY] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the per-node Command ring family, as ONE F-INDEPENDENT concrete type — the venue producer threads (WS user-data, REST worker) are non-template code and cannot name OrderManagerState<F>, so the helpers below take THIS rather than the OMS. Both `result_rings` and `ws_rings` are this type; each ring stays strictly SPSC (one producer thread, one consumer)]
// [REFERENCE]_[DECISION]_[D-448]
//======================================================================
// [CODE]
//======================================================================
using OmsCmdRings = tt::NodeArray<SPSCRing<Command, OMS_RESULT_RING_PER_NODE>, MAX_EXECUTION_NODES>;
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-09-06]
// [SIZE]_[67584B]
// [ALIGN]_[64]
// [CACHE_LINES]_[1056]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[OmsCmdRings]
//======================================================================
// D-479 as amended (2026-09-04) — the bounded-push budget for the never-drop fill rings
// (SPSCRing_TryPushBounded on the WS / REST producer threads). RAW TSC CYCLES: the unit
// every rdtsc site + the drainer histogram already use, so no calibration constant exists
// to drift. INTERIM: no build lane can produce the drainer-cycle p100 this was meant to be
// derived from (the LATENCY_BENCH histogram is compiled out of every lane), so the number
// is reasoned, not measured — the composer's worst KNOWN stall is the usleep(100) back-off
// inside OrderEventLog_Append (the drainer cycle exceeds 100us by construction), and the
// binding term is OS descheduling of the consumer (ms), so k=10 → ~10 ms ≈ 25,000,000
// cycles at a nominal 2.5 GHz: far above any burst that clears, far below a stuck consumer's
// minutes. TECH_DEBT-292 records that H8's <=100us slow budget is itself unmet — this
// constant is measured against that reality, not the invariant's letter. RE-DERIVE when a
// -DLATENCY_BENCH=ON lane records a real max_observed. The STUCK case is the watchdog's
// (TECH_DEBT-326), never this budget's.
constexpr uint64_t OMS_RING_PUSH_BUDGET_CYCLES = 25000000ULL;
static_assert(OMS_RING_PUSH_BUDGET_CYCLES >= 1000000ULL,
              "the bounded push must outlast a page-fault / preemption blip (>= 1M cycles) — "
              "a smaller budget turns a healthy consumer's hiccup into a capital halt");

//======================================================================
// [STRUCT]_[SubmitCommand]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CONCURRENCY]]
// [THREAD]_[[SLOW_WRITER] [DRAINER_READER]]
// [SYNC]_[SPSC]
// [REFERENCE]_[DESIGN_SPEC]_[orchestration-helper-with-pod-args-pattern]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE submit shape — API arg struct AND SPSC ring element; producers construct, drainer pops, Submit consumes]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct SubmitCommand {
    // ─── REQUIRED (semantic; ctor-signaled) ───
    tt::SlotIdx             portfolio_slot = {};   // the portfolio SLOT (P.3), never the node
    uint8_t                 order_type   = 0;
    Money                  qty          = Money_Zero();
    uint8_t                 leg          = 0;
    const ::PerNodeCfg<F>*  node_cfg     = nullptr;

    // ─── OPTIONAL (default-init; caller overrides as needed) ───
    uint8_t                 strategy_id  = 0xFF;
    uint8_t                 _pad[2]      = {0, 0};
    Money                  intended_tp  = Money_Zero();
    Money                  intended_sl  = Money_Zero();
    Money                  event_price  = Money_Zero();
    Money                  tp_pct       = Money_Zero();  // A25 (D-205): leg-effective per-fill TP fraction, resolved at submit; 0 → handle_buy_fill keeps the legacy intended_tp path (bytewise-identical)

    // Default ctor — needed for SPSC ring slot init + OMS state value-init compat.
    SubmitCommand() = default;

    // Required-field ctor — recommended form for production + test callers; signals
    // which fields a valid submit needs.
    SubmitCommand(tt::SlotIdx slot, OrderType t, Money q, uint8_t lg, const ::PerNodeCfg<F>* cfg)
        : portfolio_slot(slot), order_type((uint8_t)t), qty(q), leg(lg), node_cfg(cfg) {}
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v4.7.37: SubmitCommand — payload for the per-core submit_queues. Producer
// threads (today: producer slow-path; future: per-core slow-path threads)
// push commands here instead of calling OrderManager_Submit directly. The
// drainer thread (sole Submit caller) pops from these queues each tick,
// preserving the documented "single Submit caller" OMS contract even when
// multiple producer-side threads need to submit orders.
//
// Sizing: 32 slots per queue. Slow-path submit events are rare (force-close
// on time-exit, manual close, drag, swap) — peak <1/sec. 32 is generous.
//
// SubmitCommand is templated on F so the Money fields are sized correctly.
// v5.15.5.F.4c.3 WIP2d-1.B.1 — POD args struct + SPSC wire-format unified (option l).
// SubmitCommand is BOTH the OMS submit-API arg shape AND the SPSC ring element. Producers
// construct one struct; drainer pops the same struct; OrderManager_Submit consumes the same
// struct. Eliminates the prior unpack/repack ceremony.
//
// All fields have default member init for safe value-initialization (required for OMS state
// aggregate init: `OrderManagerState<F> oms{};` value-inits the SPSC ring slot array, which
// in turn value-inits each SubmitCommand slot). C++17 friend-scope rules don't permit nested-
// aggregate-init-through-private-default-ctor, so compile-time node_cfg enforcement isn't
// cleanly achievable until C++20 concepts.
//
// DISCIPLINE: production callers MUST use the required-field ctor + set node_cfg = &cfg.nodes[c].
// Test fixtures may use default-construct + field-by-field assignment with explicit nullptr.
// TT_ASSERT_PRE_RESOLVED_BOUND (Order.hpp) is the runtime backstop.
//
// Two public ctors:
//   1. Default ctor — for SPSC ring slot init + aggregate OMS state value-init compatibility
//   2. Required-field ctor — for production + test caller sites; signals which fields are
//      essential for a valid submit
//
// Per orchestration-helper-with-pod-args-pattern.md (2nd canonical application after
// Stamp_AssembleAndEmit) — POD args pattern promoted to CLAUDE.md item at ship close.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[128B]
// [ALIGN]_[16]
// [CACHE_LINES]_[2]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[SubmitCommand]
//======================================================================

constexpr size_t OMS_SUBMIT_QUEUE_SIZE = 32;  // power of 2

//======================================================================
// [STRUCT]_[OrderManagerState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING] [DATA_ORIENTED_DESIGN]]
// [THREAD]_[[DRAINER_WRITER] [WORKER_PRODUCER] [GUI_READER] [SLOW_WRITER]]
// [STRADDLE_EXEMPT]_[orders]_[element-uniform Order record pool (elements span >64B inherently — the >64B rule applies per element); name-sugar unresolvable only — D-414 leaf-3 2026-08-10]
// RETIRED 2026-08-25 — submit_queues no longer carries a straddle exemption. It was one
// ("element-uniform SPSC ring array; name-sugar unresolvable only", D-414 leaf-3 2026-08-10)
// until the field became tt::SlotArray at E.1.2.F: the wrapper's single T[N] member resolves,
// so the analyzer types it without help. TECH_DEBT-300b's dormant-tag note is what surfaced
// it — an exemption matching no straddler protects nothing while still reading as a guard.
// Left as prose, NOT as a live [STRADDLE_EXEMPT] tag: keeping the tag grammar would keep
// claiming an exemption. Recorded rather than deleted so the next reader knows it WAS
// exempt and why it stopped being (the TECH_DEBT-294 residual class, one instance closed).
// [STRADDLE_EXEMPT]_[last_exit_fill_price]_[16B Money elements, 16-aligned — elements can never cross a 64B line; name-sugar unresolvable only — D-414 leaf-3 2026-08-10]
// [STRADDLE_EXEMPT]_[last_exit_fee]_[16B Money elements, 16-aligned — elements can never cross a 64B line; name-sugar unresolvable only — D-414 leaf-3 2026-08-10]
// [STRADDLE_EXEMPT]_[last_trade_net]_[16B Money elements, 16-aligned — can never cross a 64B line; name-sugar unresolvable only; same-thread by design (leaf writes + ML tail reads on the owning thread, both topologies — H22) — P3-d-ii 2026-08-28]
// [STRADDLE_EXEMPT]_[last_trade_notional]_[16B Money elements, 16-aligned — can never cross a 64B line; name-sugar unresolvable only; same-thread by design (sister of last_trade_net) — P3-d-ii 2026-08-28]
// [SYNC]_[SPSC]
// [SYNC]_[ATOMIC]
// [REFERENCE]_[INVARIANT]_[[H3] [H6] [H20]]
// [REFERENCE]_[DESIGN_SPEC]_[[cache-layout-discipline-for-hot-side-structs] [aggressive-memory-reduction-techniques.md] [bitmap-flag-api.md] [decision-time-data-binding-pattern.md] [hot-side-array-element-alignment-for-sparse-access.md] [multi-bit-state-encoding-pattern.md] [raii-destructor-with-cluster-reorg-interaction.md] [sink-fn-pointer-for-optional-side-effect-pattern.md] [slot-state-foreach-registry-with-storage-routing.md] [spsc-ring-embedded-in-hot-struct-cluster-discipline.md]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the OMS facade — order table + rings + the money books, HOT/WARM/COLD tier-clustered, assert-anchored]
// [REGION]_[HOT cluster — drainer per-cycle]_[orders..event_log]
// [THREAD]_[[DRAINER_WRITER] [WORKER_PRODUCER]]
// [SYNC]_[SPSC]
// [REGION]_[WARM cluster — fill-burst bank state]_[portfolio..last_exit_predicted_meta]
// [THREAD]_[[DRAINER_WRITER]]
// [REGION]_[COLD cluster — boot-set + reconcile-only]_[adapter..last_seen_trade_id]
// [REGION]_[observability atomics]_[total_submitted..total_rejected]
// [THREAD]_[[DRAINER_WRITER] [GUI_READER]]
// [SYNC]_[ATOMIC]
// [REGION]_[safety CAS cluster]_[flatten_pending..recovery_until_us]
// [THREAD]_[[SLOW_WRITER] [SLOW_READER]]
// [SYNC]_[ATOMIC]
// [REFERENCE]_[CLASS]_[[27] [30]]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-12]
//======================================================================
// [CODE]
//======================================================================
// P3-c-ii (D-445): audit-funnel ring depth — 64, deliberately smaller than the fill ring's
// 256 (audit events track fill cadence; overflow is a CORRECT inline drain, never a drop).
inline constexpr int OE_FUNNEL_RING_SIZE = 64;
// P4-pre-2: calib funnel depth — per-COMPLETED-TRADE cadence, drained every apply (see the
// sizing rationale at the calib_rings declaration).
inline constexpr int CALIB_FUNNEL_RING_SIZE = 8;

//----------------------------------------------------------------------
// [SECTION]_[order-id LANE ENCODING — the Phase-4 routing key (P4-pre-3a / D-448)]
//----------------------------------------------------------------------
// The id carries its own routing information so a PRODUCER can address the owning node
// with pure arithmetic and ZERO memory reads:
//
//   bits 63..60 = order slot  (0-15; MAX_INFLIGHT_ORDERS <= 16)   [since v5.11.5.B]
//   bits 59..56 = NODE lane   (0-15; MAX_EXECUTION_NODES  <= 16)  [NEW — P4-pre-3a]
//   bits 55..0  = monotonic counter (7.2e16 ids — ~2000 years at 1/us)
//
// WHY the node lane exists: at the Phase-4 flip the result/ws/reconcile producers (adapter
// worker, WS parser thread, reconcile poller) must route each Command onto the OWNING node's
// ring. Today they hold only the venue's `oms_<id>` string. Deriving the node the obvious way
// — decode the slot, then read `orders[slot].portfolio_slot` — is a cross-thread read of
// leaf-owned state (Class 63) AND is slot-reuse-racy: the slot can be freed and re-allocated
// to another node between the venue's report and the producer's read, so the fill would be
// routed to the wrong node's ring and then dropped by the id-verify. Self-describing ids make
// the whole class unreachable rather than guarded.
//
// This is the ID-LANE half of D-448; the ALLOCATION-partition half (a node draws order slots
// only from its own sub-range) lands at §4.1 and depends on the P4-pre-6 exit-inflight dedup.
// The two are independent: routing reads the lane, never the slot's provenance.
//
// H21 note: this changes id VALUES, never the wire FORMAT (`oms_<decimal>`, which the venue
// echoes back verbatim). No persisted record keys off an id's internal bit layout, and
// in-flight orders do not survive a restart (boot reconciliation re-establishes venue truth),
// so no tombstone is owed. Enrolled below as a compile-time-pinned layout instead.
inline constexpr int      ORDER_ID_SLOT_SHIFT   = 60;
inline constexpr int      ORDER_ID_NODE_SHIFT   = 56;
inline constexpr uint64_t ORDER_ID_LANE_MASK    = 0xFull;
inline constexpr uint64_t ORDER_ID_COUNTER_MASK = (1ull << ORDER_ID_NODE_SHIFT) - 1ull;

static_assert(MAX_EXECUTION_NODES <= 16,
              "order-id node lane is 4 bits (bits 59..56) — raising MAX_EXECUTION_NODES past 16 "
              "silently truncates the routing key (bitmap-overflow-protection-discipline)");

// Decode helpers — the ONLY sanctioned way to read an id's lanes (the open-coded `>> 60`
// spelling is retired; a second spelling is how the two lanes drift apart).
inline int OMS_OrderIdSlot(uint64_t order_id) {
    return (int)((order_id >> ORDER_ID_SLOT_SHIFT) & ORDER_ID_LANE_MASK);
}
inline int OMS_OrderIdNode(uint64_t order_id) {
    return (int)((order_id >> ORDER_ID_NODE_SHIFT) & ORDER_ID_LANE_MASK);
}

template <unsigned F> struct OrderManagerState;
template <unsigned F>
inline void noop_fill_emit(OrderManagerState<F>*, Order<F>*, Money, Money, Money, Money);
// P3-b (D-444) — the leaf emit dispatch: default = immediate apply (test-harness / null-agg);
// boot rewires to the ring path (EngineCommon_FillEmitSink). ONE booking body either way.
template <unsigned F>
inline void OrderManager_FillEmitDirect(OrderManagerState<F>*, const FillEvent<F>&);

template <unsigned F>
struct OrderManagerState {
    //------------------------------------------------------------------
    // [SECTION]_[HOT CLUSTER — drainer reads every cycle]
    //------------------------------------------------------------------
    // (orders, rings, event_log). v5.15.5.C.3 (Finding A') — event_log_mode
    // removed from HOT cluster; now a 2-bit slot in oms_state_flags (COLD).
    Order<F> orders[MAX_INFLIGHT_ORDERS];
    uint16_t order_bitmap;       // 1 = slot in use, 0 = free. uint16_t caps at 16 slots.
    static_assert(MAX_INFLIGHT_ORDERS <= 16,
                  "order_bitmap is uint16_t (16 bits) AND the order-id encodes the slot in 4 bits; "
                  "MAX_INFLIGHT_ORDERS must fit both — raising it past 16 silently overflows (bitmap-overflow-protection-discipline).");
    uint16_t _pad0;
    uint32_t _pad1;
    uint64_t next_order_id;      // monotonic id counter; 0 reserved for "no id"

    // Result rings (P4-pre-3b): PER-NODE CMD_FILL_RESULT transport. Producers (the adapter
    // worker thread; the paper-synth inside Submit) route by the id's NODE lane — pure
    // arithmetic, no `orders[]` read (P4-pre-3a). Consumer today = the central drainer, which
    // drains every ring (1 consumer per ring, so the SPSC contract holds); at the flip each
    // node drains ITS OWN ring and the cross-node drain disappears.
    //
    // CAPACITY IS PARTITIONED, NOT REPLICATED: per-node depth x MAX_EXECUTION_NODES ==
    // the old single-ring depth, so OrderManagerState does not grow by the ring family.
    // Replicating the 256-deep ring per node would have added 256B x 256 x 16 = 1MB for THIS
    // family alone, onto a struct that is stack-local in production and in ~34 suite fixtures
    // against an 8MB stack (the hazard the oe_rings depth comment records). Per-node worst
    // case that justifies 16: a node owns <=2 portfolio slots, each with <=1 in-flight order
    // at a time, and the drainer empties every ring each cycle — 16 is ~8x that.
    // v5.15.5.C.1 — alignas(64) at enclosing-struct level per NC2
    // (spsc-ring-embedded-in-hot-struct-cluster-discipline.md); ensures
    // preceding field's tail doesn't share line with ring head.
    alignas(64) tt::NodeArray<SPSCRing<Command, OMS_RESULT_RING_PER_NODE>,
                              MAX_EXECUTION_NODES> result_rings;

    // WS fill rings (phase 04; PARTITIONED per node at 3b(ii) commit 4). The user-data
    // websocket thread is the sole producer and the drainer the sole consumer, so every ring
    // stays strictly SPSC — the partition changes WHICH ring a fill lands in (the id's node
    // lane), never how many threads touch one. Capacity is partitioned exactly as
    // `result_rings` was: 16 nodes x 16 deep = the old single ring's 256, never replicated.
    // Drained after the REST result rings by both consumers.
    alignas(64) OmsCmdRings ws_rings;

    // Reconcile queue (phase 05): reconciler thread is the sole producer,
    // drainer is the sole consumer. Carries CMD_RECONCILE commands with
    // drift amounts. OrderManager_Tick drains this third.
    alignas(64) SPSCRing<Command, 64> reconcile_queue;

    // v4.7.37: per-core submit queues. Producer threads (producer slow-path
    // + per-core slow-path threads in SHARDED per_node_slow execution mode
    // since v5.0.0 default) push SubmitCommands here. The drainer thread pops
    // them in OMS_DrainSubmit and calls OrderManager_Submit serially —
    // preserving the documented "drainer is sole Submit caller" contract.
    //
    // Why per-core (not one queue): when per-core slow-paths spawn (Phase C),
    // each thread is the sole producer for its own ring. SPSC contract
    // holds. With one shared queue, multiple producers would need MPSC.
    // SLOT-keyed, not node-keyed: OMS_PushSubmit indexes these by portfolio_slot (0..2N-1 under
    // partials) — pinned by tests/controller_test.cpp ("OMS_PushSubmit keys queues by
    // portfolio_slot"). Sized by MAX_PORTFOLIO_POSITIONS accordingly; it read
    // MAX_EXECUTION_NODES, which is correct today ONLY because both limits are 16.
    alignas(64) tt::SlotArray<SPSCRing<SubmitCommand<F>, OMS_SUBMIT_QUEUE_SIZE>, MAX_PORTFOLIO_POSITIONS> submit_queues;

    // === EVENT LOG MODE (phase 03 chunk 3) ===
    // 0 = legacy: OMS_Tick only marks FILLED/REJECTED and frees slots.
    //     OnEvent in ControllerEventLoop does the portfolio mutation.
    // 1 = event log: OMS_Tick runs the fill handler which opens/closes
    //     portfolio slots, updates balance, appends to the event log.
    //     OnEvent just bumps counters.
    // v5.15.5.C.1 — relocated to HOT cluster (read per drainer fill-process).
    // v5.15.5.C.3 (Finding A') — int field removed; event_log_mode is now a
    // 2-bit slot in oms_state_flags (FOREACH_OMS_STATE_MULTI_BIT registry
    // in MemHeaders/OmsStateFlagRegistry.hpp). Saves 4 bytes from HOT cluster
    // + closes the byte-per-low-cardinality-int pattern at OMS struct level
    // (per multi-bit-state-encoding-pattern.md; promotes pattern to CLAUDE.md
    // item candidate alongside item 20 bitmap-flag-api per CLAUDE.local.md
    // "codify" rule 2026-05-13). Accessor: MBS_EQ_U8(oms->oms_state_flags,
    // tt::MASK_OMS_STATE_EVENT_LOG_MODE, tt::SHIFT_OMS_STATE_EVENT_LOG_MODE, N).

    // === ORDER EVENT LOG (phase 03 chunk 3) ===
    // append-only log of order lifecycle events. populated in mode 1 by
    // the fill handler inside OMS_Tick. the portfolio can be reconstructed
    // from this log at any time via Portfolio_FromEventLog.
    // v5.15.5.C.1 — relocated to HOT cluster (drainer writes per fill-process
    // via async_ring push). Cross-thread atomics INSIDE event_log
    // (ring_full_spins, writer_realloc_failed_count, log_full_drops) are
    // isolated by OrderEventLog's internal alignas discipline.
    OrderEventLog<F> event_log;

    // P3-c-ii (D-445 / amendment I): the per-node OrderEvent FUNNEL rings — every audit
    // append (fill / rejection / reconcile) rides these to the COMPOSER, which is the sole
    // OrderEventLog_Append caller (the event-log async_ring keeps ONE appender across the
    // Phase-4 flip — the GATE critic #1 MPSC hazard never materializes). Producer per ring =
    // the site's executing thread (central drainer today; the owning node post-flip);
    // consumer = the composer (OMS_EventFunnelDrain inside the apply step). Homed on OMS per
    // the residence pin (NodeState carries leaf includes only; OM already owns the ring family).
    // Depth 64 (NOT the fill ring's 256): audit events track fill cadence, and overflow is a
    // CORRECT inline drain by design (never-drop) — 256 would grow the struct +592KB, which
    // measurably overflowed the 8MB stack across the suite's 34 stack-local OMS fixtures
    // (and production's own EngineSharded_Run function-local; the fixture-allocation pressure
    // itself is a Phase-6 hygiene rider).
    tt::NodeArray<tt::SPSCRing<OrderEvent<F>, OE_FUNNEL_RING_SIZE>, MAX_EXECUTION_NODES> oe_rings;

    // P4-pre-2 (amendment L / (I) residual): the per-node CALIBRATION funnel rings. Same
    // producer/consumer shape as oe_rings above — the leaf BUILDS a CalibRecord at slot-flat
    // (every read node-local) and the composer RENDERS it, so `calibration_log_file` keeps ONE
    // writer across the flip instead of N node threads serializing on the stdio lock (the
    // latency-path Rule-2 hazard on top of a multi-writer FILE*).
    //
    // Depth 8 (vs oe_rings' 64) — sizing is CADENCE-driven, and the oe_rings comment above
    // records why that matters: calib rows fire once per COMPLETED TRADE (slot-flat), not per
    // fill/rejection/reconcile, and the composer drains every apply, so 8 is ~4x the realistic
    // per-drain worst case (a node owns 2 slots). 320B x 8 x 16 = 40KB on a struct that is
    // stack-local in production AND in ~34 suite fixtures.
    //
    // DROP-with-LOUD-counter, NOT never-drop (deliberate, unlike oe_rings): a lost calib row is
    // a missing ML training sample, not a ledger error — and never-drop here would mean either
    // the emitter inline-draining (a node thread touching the shared FILE* + all nodes' rings —
    // exactly the P4G-5/F-2b hazard the gate raised against the oe_rings path) or blocking
    // until the composer drains (a quiesce deadlock shape). Dropping is the only policy that
    // stays correct under the flip; the counter makes it observable rather than silent.
    tt::NodeArray<tt::SPSCRing<CalibRecord, CALIB_FUNNEL_RING_SIZE>, MAX_EXECUTION_NODES> calib_rings;
    uint64_t calib_rows_dropped = 0;   // composer/operator-visible; nonzero ⇒ raise the depth

    //------------------------------------------------------------------
    // [SECTION]_[WARM CLUSTER — read on fill burst (HandleFill + DrainPostFill)]
    //------------------------------------------------------------------
    // === BANK STATE (moved from EventLoopState in phase 03 chunk 1) ===
    // Canonical portfolio + balance. After phase 03 mode 1 ships, these
    // are derived from the order event log. In mode 0 (legacy) and during
    // chunk 1 itself, EventLoop_OnEvent still mutates them directly.
    alignas(64) Portfolio<F> portfolio;
    Money       balance;
    Money       realized_pnl;
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — DELETED: fee_rate, fee_rate_maker, fee_rate_taker, slippage_pct.
    // Per Class 27 structural closure: scalar cfg-mirror caches eliminated. Per-Order fee_rate
    // now lives on Order::pre_resolved (set at submit via Order_BindPreResolved with cfg.nodes[c]).
    // HandleFill reads o->pre_resolved.fee_rate directly; DrainPostFill reads stored last_exit_fee
    // (set by HandleFill SELL). slippage_pct migrated to cfg.nodes[c].slippage_pct + read at
    // EventLoop_OnEvent. Per decision-time-data-binding-pattern.md.
    // Phase 8 (post-coding c10) — maker/taker accounting counters parallel
    // to PortfolioController's (which only fire in legacy mode). HandleFill
    // increments these per fill so sharded mode has correct accounting.
    // Sanity invariant: total_fees == total_maker_fees + total_taker_fees
    // after every fill.
    uint32_t     maker_fills_count;
    uint32_t     taker_fills_count;
    Money       total_maker_fees;
    Money       total_taker_fees;
    Money       total_fees;       // mirrors PortfolioController.total_fees
                                    // for sanity invariant; OMS-side aggregate.

    // === EXIT-FILL FEEDBACK (Phase 6prep sharded c14) ===
    // HandleFill on ORDER_MARKET_SELL sets one bit per closed core in
    // last_closed_mask and writes the realized return into the parallel array.
    // The drainer reads after OrderManager_Tick, calls ConfidenceScorer_Update
    // for ML cores, then clears the mask. Single-threaded by construction —
    // drainer is the sole reader, OMS_Tick (same thread) is the sole writer.
    // realized_return = (exit_price - entry_price) / entry_price as a double
    // (ConfidenceScorer is double-only — see CLAUDE.md FPN_Binary-Only invariant).
    uint16_t     last_closed_mask;
    uint16_t     last_opened_mask;   // v5.15.5.C.1 — relocated adjacent (both per-fill bookkeeping masks)
    uint8_t      _pad_lof[4];        // pad to 8B before double[16] array
    double       last_realized_return[MAX_PORTFOLIO_POSITIONS];

    // === PER-FILL BOOKKEEPING (mode 1 per-core accounting) ===
    // HandleFill writes one record per fill into last_fill[portfolio_slot].
    // The drainer reads after OrderManager_Tick and applies them to the
    // matching NodeContext (mapped via Sharded_LegSlot under partials).
    // Same producer/consumer contract as last_closed_mask:
    //   - producer: HandleFill (drainer thread, OMS_Tick callee)
    //   - consumer: drainer thread post-Tick
    //   single-threaded → no atomics needed.
    //
    // entry_notional / entry_fee — populated on BUY fills; mask bit set in
    //   last_opened_mask (separate from last_closed_mask so paired entry+
    //   exit on different slots in the same drain cycle don't lose data).
    //
    // exit_net_pnl / exit_entry_notional / exit_total_fees / was_win —
    //   populated on SELL fills; mask bit set in last_closed_mask
    //   (existing). Drainer uses these for node_realized accumulation,
    //   node_open_notional decrement, node_fees, node_wins/node_losses.
    // v5.15.5.C.4 Phase H — entry-side fields (entry_notional, entry_fee)
    // v5.15.5.C.4 Phase K — FillRecord struct + last_fill[] array DELETED
    // entirely. All 6 prior FillRecord fields are now Position-derived at
    // DrainPostFill time (Phase G derives exit_net_pnl + exit_entry_notional
    // + exit_total_fees from Position state in CLOSE form; Phase H derives
    // entry_notional + entry_fee from Position state in OPEN form; Phase J
    // moved was_win to OMS-level last_was_win_bitmap). FillRecord-as-snapshot
    // class permanently EXTINCT. Future per-slot scratch state goes through:
    //   - Position struct (FOREACH_POSITION_FIELD; PERSIST or SKIP_PERSIST)
    //   - OMS sibling SoA arrays (FOREACH_OMS_SLOT_SCALAR_ARRAY)
    //   - OMS cross-slot bitmaps (FOREACH_OMS_BITMAP)
    // Per DESIGN_SPECS/slot-state-foreach-registry-with-storage-routing.md.

    // v5.13.0.B / v5.15.5.C.2 (S3b) — per-slot bit set by MLStrategy when
    // exit_predictor fires the exit on this specific slot. Consumed by
    // v5.13.4's reward attribution (drainer post-fill in HandleFill) +
    // cleared after.
    //
    // PARTIALS-AWARE: bit index = portfolio slot (0..MAX_PORTFOLIO_POSITIONS-1)
    // not node_id. Under partials, slot 2c+0 and 2c+1 are independent legs;
    // each has its OWN bit.
    //
    // Single-writer (slow-path MLStrategy thread per-core), single-reader
    // (drainer thread). uint16_t reads are naturally atomic on x86; cross-
    // thread visibility via the SPSC ring's release-acquire fence on the
    // submit/fill path (set BEFORE OMS_PushSubmit returns; read AFTER
    // OMS_DrainSubmit observes the order). No explicit atomic needed.
    //
    // Pre-S3b: uint8_t[16] array + zero-size pad = 16 bytes. Post-S3b:
    // uint16_t bitmap = 2 bytes (+ 6 bytes implicit pad to align next field).
    // 8 bytes saved + mask iteration symmetric with last_closed_mask /
    // last_opened_mask (CLAUDE.md item 20 Variant 6 — bitmap-flag-api.md
    // 8th application).
    uint16_t last_exit_predicted_bitmap;
    // v5.15.5.C.4 Phase J — was_win extracted from FillRecord to cross-slot
    // bitmap (Technique 3 of aggressive-memory-reduction-techniques.md +
    // CLAUDE.md item 20). Saves 8B/record × 16 = 128B; 1B per slot in old
    // FillRecord layout (incl. alignment pad) → 2B total at OMS level.
    uint16_t last_was_win_bitmap;
    // v5.15.5.C.5 — is_maker reverted from Position SKIP_PERSIST to OMS-level
    // cross-slot bitmap per slot-state-foreach-registry-with-storage-routing.md
    // decision tree (sparse-access ephemeral state lives in sibling SoA on OMS).
    // Enables Position to be 184B PERSIST + alignas(64) → 192B = 3 cache lines
    // exact (hot-side-array-element-alignment-for-sparse-access.md).
    // [last_is_maker_bitmap DELETED at E.1.3 P3-f (G-cohort census): write-only —
    //  its claimed "Phase G derive at DrainPostFill" consumer never survived; the
    //  maker/taker telemetry that matters rides the fee-triple counters
    //  (total_maker/taker_fees + counts). Not persisted, not wire → clean delete.]
    // v5.15.5.C.5 — exit_fill_price reverted from Position SKIP_PERSIST to
    // OMS-level sibling SoA array (same OMS-level-sibling reasoning).
    // Per-slot captured at HandleFill SELL; consumed by Phase G derive at
    // DrainPostFill. 384B (24 × 16); not persisted.
    Money last_exit_fill_price[MAX_PORTFOLIO_POSITIONS];
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — exit_fee stored at HandleFill SELL time (from o->pre_resolved.fee_rate).
    // Consumed by EventLoop_DrainPostFillOneCore for per-core accounting. Replaces the prior pattern of
    // RE-COMPUTING exit_fee in DrainPostFill from cfg lookup — that was a Class 27 adjacent shape
    // (DrainPostFill's recompute lost the authoritative pre_resolved.fee_rate that HandleFill captured).
    // Per decision-time-data-binding-pattern.md: the Order's pre_resolved.fee_rate is the canonical
    // value; subsequent consumers READ it, never recompute from cfg. 384B (24 × 16); not persisted.
    // v5.15.5.F.4d Step 7 (§ N.2): enrolled in FOREACH_OMS_PER_SLOT_FIELD registry — closes Class 30
    // latent drift (existed since .F.4c.3 r-4 but wasn't in registry; init/reset hand-maintained).
    Money last_exit_fee[MAX_PORTFOLIO_POSITIONS];

    // P3-d-ii — the per-slot TRADE accumulators: the leaf adds each SELL leg's net (+ the
    // leg's entry-basis notional); the DrainPostFill per-TRADE ML tail (pnl_feeder /
    // ensemble reward / exit-bandit counterfactual / cooldown sign) consumes + zeroes BOTH
    // at slot-flat. Replaces the tail's dead CLOSE-form derive (structurally incompatible
    // with partial closes — A-1 T4; the position is FLAT-form when the tail fires, so
    // qty-derived values are gone). Same-thread today (drainer writes + reads); post-flip
    // BOTH sides live on the owning node's thread (leaf + its tail) — H22-pure.
    Money last_trade_net[MAX_PORTFOLIO_POSITIONS];
    Money last_trade_notional[MAX_PORTFOLIO_POSITIONS];

    // Per-slot bandit reward attribution (.F.4d § N.2 scaffold; WIRED at E.1.3 P3-f).
    // Written at handle_sell_fill FLAT: the trade's realized net bps (last_trade_net /
    // last_trade_notional × 1e4 — the same signal the exit-bandit reward consumes).
    // Read at the calib emit body (real_on_exit_calibration, also flat-gated — read-
    // consistent by construction). The fuller algo-dispatch reward-fn design
    // (g_exit_reward_dispatch[algo]) remains TECH_DEBT-174 parked scope. Sibling-array
    // carrier per decision-time-data-binding-pattern.md (no Class 27 cache-mirror).
    // 128B (16 × 8); not persisted.
    double bandit_reward_bps[MAX_PORTFOLIO_POSITIONS];

    // v5.13.0.B — calibration logging. Populated by slow-path body when
    // the exit-model fires (mirrors last_exit_predicted_bitmap). HandleFill
    // reads + emits CSV row on exit fill (any exit reason — predicted or
    // natural TP/SL/time). Operator post-processes the CSV offline (ROC,
    // Brier, precision-recall). last_exit_predicted_p stores the actual
    // blended probability at submit time.
    double last_exit_predicted_p[MAX_PORTFOLIO_POSITIONS];

    // v5.13.4 / v5.15.5.C.2.1 (LOW-2) — per-slot exit-predictor arm + regime
    // capture, bit-packed via FOREACH_OMS_META_SLOT (first application of
    // DESIGN_SPECS/multi-bit-state-encoding-pattern.md).
    //
    // Slow-path body writes the dominant exit_predictor horizon idx + regime
    // at submit time; HandleFill reads at fill time for Bandit_Update on
    // exit_bandits[regime]. Captured per-slot (not per-core) so partials legs
    // A/B independently attributable + stable across subsequent slow-path
    // cycles (subsequent predicts on the same core can't overwrite this
    // slot's chosen arm because there's already a fill pending). Cleared
    // in HandleFill post-update (drainer; same thread as read).
    //
    // Per-slot byte layout (see OmsExitPredictorMetaRegistry.hpp):
    //   bits 0..1 = regime (2 bits, 4 states)
    //   bits 2..5 = arm (4 bits, 0..15)
    //   bit 6     = valid flag (1 = populated; 0 = unset, replaces -1 sentinel)
    //   bit 7     = reserved
    //
    // Pre-LOW-2: int8_t last_exit_predicted_arm[16] + int8_t last_exit_
    // predicted_regime[16] = 32 bytes. Post-LOW-2: uint8_t[16] = 16 bytes.
    // 16 bytes saved per OMS. Parallel decode of arm + regime via ILP at
    // consumer sites (no data dependency between extracts).
    uint8_t last_exit_predicted_meta[MAX_PORTFOLIO_POSITIONS];
    // MAX_PORTFOLIO_POSITIONS=16 → 16 bytes; 16 % 8 == 0, so no padding required.

    //------------------------------------------------------------------
    // [SECTION]_[COLD CLUSTER — boot-set + reconcile-only (drainer hot path doesn't read)]
    //------------------------------------------------------------------

    // Exchange adapter (by value). In paper mode all function pointers
    // are null and the OMS short-circuits before touching them. In live
    // mode the OMS calls submit_market_buy / submit_market_sell from
    // OrderManager_Submit and the adapter callback fires later from a
    // worker thread.
    // v5.15.5.C.1 — relocated to COLD cluster (set at boot, read only on
    // live Submit path which is rare relative to drainer-cycle reads).
    alignas(64) ExchangeAdapter<F> adapter;

    // === COLD-CLUSTER BOOLEAN STATE (v5.15.5.C.2 / S3a) ===
    // Bit-packed cohort of 3 single-thread boolean state flags:
    //   LIVE_TRADING         (bit 0) — boot-set; gates Submit adapter dispatch
    //   PARTIAL_EXIT_ENABLED (bit 1) — boot-set; drainer slot→node_id mapping
    //   KILL_SWITCH_TRIPPED  (bit 2) — drainer-thread write; serialized as int
    //                                    in snapshot (wire format preserved)
    // Pre-.C.2 layout: int live_trading + uint8 + _pad_pe[7] + uint8 + _pad_ks[7]
    //                   = 20 bytes. Post-.C.2: uint8_t bitmap (+ implicit padding) = 1 byte.
    // Accessor: BITMAP_IS_SET(oms.oms_state_flags, tt::MASK_OMS_STATE_<NAME>)
    //           or OMS_STATE_FLAG_IS_SET(oms, <NAME>). See OmsStateFlagRegistry.hpp.
    // 5 bits headroom for future COLD-cluster booleans (static_assert catches overflow).
    uint8_t oms_state_flags;
    // v5.15.5.C.2.1 (MEDIUM-2 close from /dod-audit on 852a6e3): explicit 7-byte
    // pad locking the COLD-cluster alignment gap before Money ks_min_balance.
    // Money needs 8-byte alignment (uint64 internally + CLAUDE.md item 27
    // padding-determinism). The 7-byte gap is structurally inherent; explicit
    // pad matches the prior _pad_pe[7] + _pad_ks[7] discipline pre-S3a.
    // Offset-lock static_assert at the end of struct (line ~520).
    uint8_t _pad_osf[7];

    // === KILL SWITCH THRESHOLDS (moved from EventLoopState in phase 03 chunk 1) ===
    // Configured by EventLoopState_ConfigureKillSwitch (which now writes
    // here through the OMS pointer). Disabled by default (both thresholds
    // zero). Tripping clears every registered core's permission with
    // RELEASE. There is NO runtime resume: the GLOBAL kill is restart-only by
    // design (D-481 / TECH_DEBT-328 — EventLoop_Unpause was dead code and was
    // deleted at E.1.3 3b(ii) commit 1; the GUI reset clears NODE lanes only).
    // kill_switch_tripped bit lives in oms_state_flags above (v5.15.5.C.2).
    Money  ks_min_balance;       // trip if balance < this
    Money  ks_max_drawdown_pct;  // trip if (peak - balance) / peak > this (0 = disabled)
    Money  ks_peak_balance;      // running max of balance, updated on exits
    uint64_t ks_trips_total;      // count of trip events (observability)

    // v5.15.5.C.3 — paper-session start time (microseconds). Set at OrderManager_Init
    // to now_us(); updated to now_us() at each paper-reset (end of previous session +
    // start of new session). Persisted via FOREACH_OMS_PERSIST_FIELD so paper-mode
    // restart resumes from the same session-start anchor. Used by the paper-reset
    // archive flow (Phase 6) to format `{start_iso}_to_{end_iso}.paper` directory
    // names — gives operator a date-range identifier per paper session for offline
    // analysis (per_strategy_per_regime comparison across sessions).
    uint64_t paper_session_start_us;

    // FILE* opened lazily by OrderManager_OpenCalibrationLog (engine boot
    // calls when cfg.calibration_log_path is non-empty). Drainer thread
    // (sole HandleFill caller) is the only writer. Closed in Shutdown.
    FILE* calibration_log_file;

    // === TRADE LOG (moved from EventLoopState in phase 03 chunk 1) ===
    // Optional CSV trade log. nullptr → no logging (default). Not owned —
    // the engine main owns the ShardedTradeLog object and passes a pointer
    // here via EventLoopState_AttachTradeLog (which now writes through to
    // the OMS).
    ShardedTradeLog* trade_log;

    // v5.14.4.0 — high-watermark trade_id we've seen via myTrades (boot
    // reconcile path). Used by Reconcile_ApplyMissedFills (v5.14.4.B) to
    // filter out trades we already saw, replaying ONLY trades with
    // trade_id > last_seen_trade_id. Monotonic; updated post-reconcile
    // to max(seen) so the next reconcile cycle skips replay-applied trades.
    //
    // Single source of truth for "what trades have we observed?" — sister
    // to total_filled (which counts observed-and-applied) but specifically
    // tracks the exchange-side trade_id high-watermark for replay-safety.
    //
    // Future-thinking: when WS-side fill stream is added (v5.14.x+ post-
    // boot reconcile), bump this on every WS fill so post-disconnect
    // reconcile only replays trades newer than the WS-stream high water.
    // Boot today = once at startup; not a per-cycle path.
    uint64_t last_seen_trade_id;

    //------------------------------------------------------------------
    // [SECTION]_[CROSS-THREAD OBSERVABILITY COUNTERS — alignas(64) cluster (NC1 ND1)]
    //------------------------------------------------------------------
    // Writer = drainer thread (atomic_fetch_add per fill).
    // Reader = snapshot publisher (GUI thread) at 60 Hz via .load(RELAXED).
    // Isolation prevents publisher reads from invalidating cache lines of
    // drainer's other write targets (warm fee_state / last_fill etc.).
    // Observability counters. Atomic so the TUI render loop on a different
    // core can read them without locks. Relaxed ordering throughout —
    // these are display-only.
    alignas(64) std::atomic<uint64_t> total_submitted;
    std::atomic<uint64_t> total_filled;
    std::atomic<uint64_t> total_rejected;
    // D-479 — how many times a producer gave up on a never-drop fill ring and went LOUD-FATAL.
    // Its value rides IN the durable record as `fatal_n`, so the log line and this counter agree.
    // NOT total_rejected: that is the VENUE-reject counter, and conflating an unbooked fill with a
    // venue rejection would misreport capital. Restart-only (SKIP_RESET, D-481) — a fatal survives
    // a paper reset by design, because the engine it describes is the one that must be restarted.
    std::atomic<uint64_t> ring_full_fatal;

    //------------------------------------------------------------------
    // [SECTION]_[CROSS-THREAD SAFETY CAS CLUSTER — alignas(64) cluster (NC1 ND1)]
    //------------------------------------------------------------------
    // Writer = N slow-path threads via CAS (compare_exchange_strong).
    // Reader = slow-path predicate in RebuildOneCore.
    // Isolation prevents N-thread CAS contention from invalidating
    // neighbor warm fields (kill_switch_tripped, ks_*, etc.).
    // v5.12.1.A.2 — WS-staleness emergency-flatten flag. CAS-set by
    // EventLoop_CheckWsStaleness when the producer's last-tick gap exceeds
    // cfg.ws_dead_time_flatten_threshold_secs. Multiple slow-paths may
    // race for the CAS; only one wins and submits the flatten via
    // EventLoop_FlattenAll. Read by EventLoop_RebuildOneCore (.A.3) for
    // recovery-refusal gating during the post-flatten reconcile window.
    // std::atomic<int> for the compare_exchange_strong primitive.
    alignas(64) std::atomic<int> flatten_pending;  // 0 = normal, 1 = flatten fired
    // v5.12.1.A.3 — recovery refusal deadline. Set by CheckWsStaleness
    // alongside flatten_pending=1: deadline = now_us + recovery_delay_secs*1e6.
    // RebuildOneCore reads this; while now_us < deadline, it forces
    // BUY_BLOCKED + SHALT_RECOVERY on every core's pending_params.
    // Cleared together with flatten_pending after deadline expires
    // (auto-recovery; no manual reset required).
    std::atomic<uint64_t> recovery_until_us;  // 0 = no recovery window active

    //------------------------------------------------------------------
    // [SECTION]_[RAII destructor (v5.11.26)]
    //------------------------------------------------------------------
    // Stops the OrderEventLog async writer thread (v5.11.3.B feature) +
    // closes the disk file + frees the mmap'd entry buffer when this
    // struct goes out of scope. Idempotent on default-init / never-Init'd
    // state.
    //
    // v5.15.5.C.1 — destructor body unchanged through tier reorg per NC1
    // (raii-destructor-with-cluster-reorg-interaction.md). Field references
    // resolve by name; reorg changes offsets but not names. The cleanup
    // pairing (every Init alloc matches a Shutdown free) is preserved
    // because the reorg adds/removes ZERO members — only reorders.
    //
    // Why RAII (vs explicit OrderManager_Shutdown at every site):
    //  - 113 OrderManagerState declarations across the codebase, ZERO
    //    callers of OrderManager_Shutdown today (production-side
    //    EngineSharded_Run never calls it; OS cleanup at exit only).
    //  - Tests: 9 sites call OrderManager_Init directly; none call
    //    Shutdown. Each previously leaked the writer thread; ASAN
    //    flagged stack-use-after-scope at SPSCRing_TryPop because the
    //    writer outlived the test scope and read stale stack data.
    //  - One destructor closes both classes of leak in one place.
    //
    // Why safe (no copy/move concerns):
    //  - grep verified zero copy-init / memcpy of OrderManagerState
    //    across the codebase. struct is always declared then used by
    //    pointer; never duplicated.
    //  - Compiler-generated copy/move constructors would be deleted by
    //    SPSCRing's deleted copy semantics anyway (mutex-like guarantee
    //    via std::atomic<uint64_t>).
    // v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer dispatch.
    // Per DESIGN_SPECS/sink-fn-pointer-for-optional-side-effect-pattern.md. Default = noop fn
    // (always-call, deterministic no-op). Real fns wired at boot when trade_log /
    // calibration_log_file Init succeeds. Eliminates `if (oms->trade_log)` /
    // `if (oms->calibration_log_file)` callsite branches in handle_buy_fill / handle_sell_fill.
    // Indirect call cost ~3-5ns vs predicted-correctly branch ~0-1ns; branchless wins on
    // p99 + variance per H20 + Caramel principle ("branchless even when slightly slower").
    // P3-d-ii: 6th arg = the LEG qty (exit/calib rows carry the leg, not a Position re-read —
    // the Position is remainder-form after Portfolio_CloseSlotLeg; entry passes fill_qty twice).
    void (*on_entry_fill_emit)(OrderManagerState<F>*, Order<F>*, Money, Money, Money, Money) = &noop_fill_emit<F>;
    void (*on_exit_fill_emit)(OrderManagerState<F>*, Order<F>*, Money, Money, Money, Money)  = &noop_fill_emit<F>;
    void (*on_exit_calibration)(OrderManagerState<F>*, Order<F>*, Money, Money, Money, Money) = &noop_fill_emit<F>;

    // P3-b (D-444) — the leaf FILL-EMIT dispatch (Pattern 5 sister to the sinks above).
    // Default = OrderManager_FillEmitDirect (immediate apply through the ONE booking body —
    // preserves every direct-HandleFill test oracle; arrival-order schedule, single-thread).
    // Boot (EngineCommon_BootGlobal, BOTH drivers) rewires to EngineCommon_FillEmitSink:
    // ring-deferred emit, applied by the composer in DEFINED ring order (the production
    // schedule). `agg` is the composer's state, reachable from the OMS for the ring path +
    // the composer-side trade-row emit; null in bare test fixtures.
    void (*fill_emit)(OrderManagerState<F>*, const FillEvent<F>&) = &OrderManager_FillEmitDirect<F>;
    AggregatorState<F>* agg = nullptr;
    // P3-d-ii — OPAQUE EventLoopState backref for the EngineCommon-side apply path (the
    // composer's node-row booking needs state.nodes; OrderManager cannot include CEL — cycle).
    // void* + ONE cast at the TYPED consumer (EngineCommon) — the sanctioned ezoo_refs shape
    // (ML-agnostic void* on OMS, cast where the type is known). Set at BootGlobal beside agg;
    // null in bare harnesses (the direct path books the global ledger only).
    void* els = nullptr;
    // Per-node FillEvent emit sequence (FillEvent.seq source). Element n is written ONLY by
    // node n's emitting thread (the central drainer for all n under Phase-3 topology; the
    // owning node post-flip) — per-element single-writer, H22-safe across the Phase-4 flip.
    tt::NodeArray<uint64_t, MAX_EXECUTION_NODES> fill_emit_seq = {};

    // v5.15.5.F.4d Step 7 § F — per-core ezoo + node_cfg lookup for calib log consumer.
    // Per-NODE ARRAYS indexed by the DERIVED node at consumer (sister to per-slot last_exit_fee[]
    // + bandit_reward_bps[] sibling-array pattern; OmsState is engine-wide single instance, NOT
    // per-core). void* keeps OmsState ML-agnostic (sister to ctx.ensemble_handle on NodeContext);
    // cast to EnsembleModelZoo<F>* / const PerNodeCfg<F>* in real_on_exit_calibration via
    // oms->ezoo_refs[Sharded_SlotNode(slot)]. Default nullptr (test fixtures + pre-boot + non-ML);
    // wired at EngineSharded per-core init alongside state.nodes[i].ensemble_handle.
    // NodeArray, not a bare array: these accept ONLY a NodeIdx subscript, so the mis-index this
    // pair actually carried (read with a SLOT under a false "== pslot" comment) is now a COMPILE
    // ERROR rather than a convention. Layout is identical to the raw arrays (pinned in IndexSpaces).
    tt::NodeArray<void*, MAX_EXECUTION_NODES>       ezoo_refs     = {};   // EnsembleModelZoo<F>* per-node (lazy-cast)
    tt::NodeArray<const void*, MAX_EXECUTION_NODES> node_cfg_refs = {};   // const PerNodeCfg<F>* per-node (lazy-cast)

    ~OrderManagerState() {
        OrderManager_Shutdown(this);
    }
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer forward declarations.
// Per DESIGN_SPECS/sink-fn-pointer-for-optional-side-effect-pattern.md. Noop fn provides
// the always-call default; real fns wired at boot when respective subsystems enable.
//======================================================================
// [COMMENT]_[the facade — what lives here + why]
//----------------------------------------------------------------------
// Phase 03 (option D — facade): the OMS now owns all the financial state
// that used to live in EventLoopState. EventLoopState becomes a thin
// dispatcher with a pointer back to here.
//
// Field groups:
//   - order table (orders[], order_bitmap, next_order_id)
//   - exchange wiring (adapter, live_trading, result_queue)
//   - bank state (portfolio, balance, realized_pnl, fee_rate) — phase 03
//   - kill switch (ks_*, kill_switch_tripped, ks_trips_total) — phase 03
//   - trade log pointer (not owned, points at the engine's CSV writer) — phase 03
//   - observability counters (total_submitted/filled/rejected)
//
// The bank state and kill switch fields used to live in EventLoopState.
// Moved here in phase 03 chunk 1 so the OMS is the single source of truth
// for everything money-related. EventLoopState_Balance and friends are
// the forwarding accessors that let existing call sites keep working.
//======================================================================
// [COMMENT]_[C.1 tier reorganization]
//----------------------------------------------------------------------
// v5.15.5.C.1 — OrderManagerState HOT/WARM/COLD tier reorganization
// (cache-layout-discipline-for-hot-side-structs.md Rule 4) + cross-thread
// atomic cluster isolation via alignas(64) (cross-thread-snapshot-publish-
// cluster-isolation.md / ND1) + embedded-SPSCRing cluster discipline
// (spsc-ring-embedded-in-hot-struct-cluster-discipline.md / NC2) + RAII
// destructor preservation through tier reorg (raii-destructor-with-cluster-
// reorg-interaction.md / NC1).
//
// Pre-.C.1 the struct interleaved HOT (orders, rings) with COLD (adapter,
// live_trading) with WARM-on-fill (portfolio, fee state) with CROSS-THREAD
// (total_*, flatten_pending) — drainer per-cycle access pulled scattered
// cache lines across the entire ~150 KB struct. Post-.C.1 clusters are
// explicit + compile-time-locked via static_asserts.
//
// FIELD NAMES UNCHANGED — only field positions move. All ~46 consumer
// access sites (`oms->total_submitted` etc.) work without modification.
// Compiler resolves names at compile time; reorg changes resolved offsets
// only. RAII destructor (`~OrderManagerState()`) refs fields by name; reorg
// is bytewise-safe per NC1.
//
// Persistence: OMS state is NOT byte-persisted. OrderEventLog writes
// individual OrderEvent records (NOT struct bytes); Portfolio_FromEventLog
// replays on restart. Snapshot publisher reads individual fields. Reorg
// preserves all wire-format semantics.
//
// Bench-gate context: TECH_DEBT-012 flags this as PERFORMANCE-CRITICAL
// (drainer reads OMS every cycle). Tier reorg is pure data-layout change
// with compile-time-enforced sizeof/offsetof asserts — bytewise-safe + the
// 3027-test regression + parity_harness byte-equivalence checks act as the
// gate. The HIGH-RISK changes per TECH_DEBT-012 (FOREACH_OMS_INIT registry
// conversion) land in .C.3 where rdtsc-bracket bench instrumentation is
// added with a runtime cfg toggle.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-09-06]
//----------------------------------------------------------------------
// [SIZE]_[457984B]
// [ALIGN]_[64]
// [CACHE_LINES]_[7156]
// [STRADDLE]_[unverified: orders last_exit_fill_price last_exit_fee last_trade_net last_trade_notional]
//======================================================================
// [END_STRUCT]_[OrderManagerState]
//======================================================================

// v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 noop fn definition.
// Single noop shared across all 3 fn-pointer fields (same sig). Used as the "always-call"
// default when subsystems are disabled; production cost = 1 indirect call (~3-5ns deterministic).
template <unsigned F>
inline void noop_fill_emit(OrderManagerState<F>*, Order<F>*, Money, Money, Money, Money) {}

// v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 real fn definitions.
// Wrap the previous `if (oms->trade_log) { ... }` / `if (oms->calibration_log_file) { ... }` bodies.
// Set at boot site (e.g., ShardedTradeLog_Init, calibration_log_open) when respective subsystem enables.
// P3-b flip (D-444 / amendment I): these sink bodies now PRE-BUILD the row as an EmitRecord
// pushed in LOCKSTEP right after the leaf's FillEvent (same node ring index, same call
// sequence) — the COMPOSER emits the actual CSV row with the post-apply balance (the leaf
// no longer knows balance-after; its ledger effect is the FillEvent). Occupancy invariant:
// records push 1:1 with FillEvents on equal-size rings, so a successful fe push guarantees
// record space (never-drop inherited). Null-agg harness: direct emit — under FillEmitDirect
// the apply already booked at the leaf's emit, so oms->balance IS balance-after (same value).
template <unsigned F>
inline void real_on_entry_fill_emit(OrderManagerState<F>* oms, Order<F>* o,
                                     Money fill_price, Money fill_qty, Money entry_fee, Money /*unused*/) {
    if (__builtin_expect(oms->agg != nullptr, 1)) {
        const int partial_on = BITMAP_IS_SET(oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED) ? 1 : 0;
        const tt::NodeIdx nn{(int16_t)BITMAP_SLOT_NODE((int)o->portfolio_slot, partial_on)};
        EmitRecord<F> rec{};
        rec.fill_price = fill_price; rec.entry_price_snap = fill_price;
        rec.qty = fill_qty; rec.net = Money_Zero(); rec.fee = entry_fee;
        rec.timestamp_us = o->submitted_at_us;
        rec.slot = o->portfolio_slot; rec.strategy_id = o->strategy_id; rec.is_entry = 1;
        (void)tt::SPSCRing_TryPush(&oms->agg->emit_rings[nn], rec);
        return;
    }
    TradeEvent<F> synth{};
    synth.price     = fill_price;
    synth.timestamp = o->submitted_at_us;
    // D1/Class-61 close: the SLOT goes to the log as the typed parameter —
    // synth.node_id is no longer written (it used to smuggle the slot through
    // a field whose declared meaning is the NODE).
    synth.type      = TRADE_EVENT_ENTRY;
    ShardedTradeLog_RecordEntry(oms->trade_log, synth, o->portfolio_slot,
                                o->strategy_id,
                                fill_price, fill_qty, entry_fee, oms->balance);
}

template <unsigned F>
inline void real_on_exit_fill_emit(OrderManagerState<F>* oms, Order<F>* o,
                                    Money fill_price, Money net, Money total_fee, Money q_leg) {
    // Re-read position state (preserved through CloseSlot per Phase F invariant — only the
    // active_bitmap bit was cleared; entry_price + quantity remain stored on Position).
    // SAFE here (leaf-time, Phase A) — NOT safe at composer emit time (Phase-B overwrite +
    // partial remainder-form), which is exactly why the record pre-builds (A-1 T5).
    const int pslot = (int)o->portfolio_slot;
    const Money entry_price_snap = oms->portfolio.positions[pslot].entry_price;   // basis survives CloseSlotLeg
    const Money qty_snap         = q_leg;   // P3-d-ii: the LEG qty (Position is remainder-form here)
    if (__builtin_expect(oms->agg != nullptr, 1)) {
        const int partial_on = BITMAP_IS_SET(oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED) ? 1 : 0;
        const tt::NodeIdx nn{(int16_t)BITMAP_SLOT_NODE(pslot, partial_on)};
        EmitRecord<F> rec{};
        rec.fill_price = fill_price; rec.entry_price_snap = entry_price_snap;
        rec.qty = qty_snap; rec.net = net; rec.fee = total_fee;
        rec.timestamp_us = o->submitted_at_us;
        rec.slot = o->portfolio_slot; rec.strategy_id = o->strategy_id; rec.is_entry = 0;
        (void)tt::SPSCRing_TryPush(&oms->agg->emit_rings[nn], rec);
        return;
    }
    TradeEvent<F> synth{};
    synth.price     = fill_price;
    synth.timestamp = o->submitted_at_us;
    // D1/Class-61 close: typed SLOT param (see the entry-emit note above).
    synth.type      = TRADE_EVENT_EXIT;
    ShardedTradeLog_RecordExit(oms->trade_log, synth, o->portfolio_slot,
                               o->strategy_id,
                               entry_price_snap, fill_price,
                               qty_snap, net, total_fee, oms->balance);
}

template <unsigned F>
inline void real_on_exit_calibration(OrderManagerState<F>* oms, Order<F>* o,
                                      Money fill_price, Money net, Money total_fee, Money q_leg) {
    const int pslot = (int)o->portfolio_slot;
    const Money entry_price_snap = oms->portfolio.positions[pslot].entry_price;
    const Money qty_snap         = q_leg;   // P3-d-ii: the LEG qty (Position is remainder-form at flat)
    const uint64_t ts_us = (uint64_t)
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    const double entry_d_calib = Money_ToDouble(entry_price_snap);
    const double exit_d_calib  = Money_ToDouble(fill_price);
    const double gain_pct      = entry_d_calib > 0.0
        ? (exit_d_calib - entry_d_calib) / entry_d_calib * 100.0 : 0.0;
    const double notional_d    = entry_d_calib > 0.0
        ? entry_d_calib * Money_ToDouble(qty_snap) : 0.0;
    const double pnl_bps       = notional_d > 0.0
        ? Money_ToDouble(net) / notional_d * 10000.0 : 0.0;
    const uint8_t pred_flag    = (uint8_t)BITMAP_IS_SET(oms->last_exit_predicted_bitmap, BITMAP_BIT_U16(pslot));
    const double pred_p        = oms->last_exit_predicted_p[pslot];
    (void)total_fee;

    // v5.15.5.F.4d Step 7 § F — decode bandit context from Order::flags_packed bits 17-25
    // (Pattern 4 decision-time-bound; sister to MASK_ORDER_PRE_RESOLVED at bit 16).
    const int bandit_active_state = MBS_OrderBanditActiveState(*o);
    const int bandit_regime       = MBS_OrderBanditRegime(*o);
    const int bandit_chosen_arm   = MBS_OrderBanditChosenArm(*o);

    // v5.15.5.F.4d Step 7 § F — per-slot bandit reward from FOREACH_OMS_PER_SLOT_FIELD sibling
    // array (added Step 7 § N.2; written at HandleFill SELL).
    const double reward_bps_attributed = oms->bandit_reward_bps[pslot];

    // v5.15.5.F.4d Step 7 § F — cast per-NODE ezoo_refs / node_cfg_refs to typed pointers for
    // ML-side access. Both are [MAX_EXECUTION_NODES] and are WRITTEN by node index at
    // EngineSharded/Run.hpp (per-core boot loop), so they must be READ by node index too.
    // They were read with `pslot` under a comment asserting "== pslot since each core owns 1
    // portfolio position" — false whenever partial_exit_enabled=1, where a node owns slots
    // 2N+0 / 2N+1: node 0's leg B read node 1's zoo, and nodes at slot>=num_nodes read nullptr.
    // Nullptr-defensive: test fixtures + non-ML cores have wiring pointers nullptr; telemetry
    // coalesces to 0/0.0 placeholders.
    const tt::NodeIdx pnode{ (int16_t)BITMAP_SLOT_NODE(
        pslot, OMS_STATE_FLAG_IS_SET(*oms, PARTIAL_EXIT_ENABLED)) };
    auto* ezoo     = static_cast<EnsembleModelZoo<F>*>(oms->ezoo_refs[pnode]);
    auto* node_cfg = static_cast<const PerNodeCfg<F>*>(oms->node_cfg_refs[pnode]);

    // Telemetry — null-coalesced.
    const int    thompson_telemetry_arm    = ezoo     ? ezoo->last_predicted_buy_thompson_arm               : 0;
    const double thompson_exp3_blend_alpha = node_cfg ? FPN_ToDouble(node_cfg->thompson_exp3_blend_alpha) : 0.0;

    // Per-arm Exp3 probabilities (telemetry). bandit_regime bounds-clamped defensively
    // (cfg parser clamps but belt-and-suspenders for replay-from-old-stamp scenarios).
    const int regime_clamped = (bandit_regime >= 0 && bandit_regime < NUM_REGIMES) ? bandit_regime : 0;
    double exp3_probs[BANDIT_MAX_ARMS] = {0};
    if (ezoo) Bandit_GetProbabilities(&ezoo->bandits[regime_clamped], exp3_probs);

    // P4-pre-2 (amendment L): BUILD the row payload; the COMPOSER renders it (single FILE*
    // writer across the flip). Every read above is node-local — the Position basis, the
    // per-slot sibling arrays, this node's ezoo/cfg — so the build stays correct when this
    // body moves onto the owning node's thread; only the stdio write relocates.
    CalibRecord rec{};
    rec.ts_us            = ts_us;
    rec.pslot            = pslot;
    rec.pred_flag        = (uint32_t)pred_flag;
    rec.pred_p           = pred_p;
    rec.entry_price      = entry_d_calib;
    rec.exit_price       = exit_d_calib;
    rec.gain_pct         = gain_pct;
    rec.pnl_bps          = pnl_bps;
    rec.was_win          = BITMAP_IS_SET(oms->last_was_win_bitmap, BITMAP_BIT_U16(pslot)) ? 1 : 0;
    rec.bandit_algorithm = bandit_active_state;
    rec.regime_id        = bandit_regime;
    rec.chosen_arm       = bandit_chosen_arm;
    rec.reward_bps       = reward_bps_attributed;
    rec.thompson_tel_arm = thompson_telemetry_arm;
    rec.thompson_blend_alpha = thompson_exp3_blend_alpha;
    for (int a = 0; a < BANDIT_MAX_ARMS; ++a) {
        rec.exp3_w[a]         = exp3_probs[a];
        rec.thompson_mu[a]    = ezoo ? ezoo->buy_thompson_bandits[regime_clamped].mu_post[a]        : 0.0;
        rec.thompson_prec[a]  = ezoo ? ezoo->buy_thompson_bandits[regime_clamped].precision_post[a] : 0.0;
        rec.thompson_pulls[a] = ezoo ? (uint32_t)ezoo->buy_thompson_bandits[regime_clamped].total_pulls[a] : 0u;
    }
    OMS_CalibFunnelPush(oms, rec, pslot);
}

// ════════════════════════════════════════════════════════════════════════
// v5.15.5.C.1 — Layout invariants. Compile-time-enforced cluster anchors
// per `cache-layout-discipline-for-hot-side-structs.md` Rule 3+4 +
// `cross-thread-snapshot-publish-cluster-isolation.md` (ND1) +
// `spsc-ring-embedded-in-hot-struct-cluster-discipline.md` (NC2).
// Catches future field-insertion that silently breaks alignment.
// ════════════════════════════════════════════════════════════════════════
// [ASSERT]_[LAYOUT_LOCK]_[alignof(OrderManagerState<64>) >= 64]
static_assert(alignof(OrderManagerState<64>) >= 64,
              "OrderManagerState MUST be 64-byte aligned (cluster anchors + alignas(64) on result_rings).");
// HOT cluster — first SPSCRing anchor (preceding fields = orders[] + scalars).
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(result_rings) % 64 == 0]
static_assert(offsetof(OrderManagerState<64>, result_rings) % 64 == 0,
              "result_rings (HOT cluster ring 1) MUST start at a cache-line boundary. "
              "See spsc-ring-embedded-in-hot-struct-cluster-discipline.md.");
// P4-pre-3b: the partition must not have grown the struct — each SPSCRing carries its own
// producer/consumer lines, so 16 rings cost 16 head/tail line-pairs the single ring did not.
// Pinned so a depth change that quietly re-inflates the family is a compile error, not a
// discovery made when a stack-local fixture segfaults.
static_assert(sizeof(tt::NodeArray<SPSCRing<Command, OMS_RESULT_RING_PER_NODE>, MAX_EXECUTION_NODES>)
              <= sizeof(SPSCRing<Command, OMS_RESULT_QUEUE_SIZE>) + 64 * 2 * MAX_EXECUTION_NODES,
              "per-node result rings must stay within the old single-ring payload + per-ring "
              "head/tail line overhead (capacity is PARTITIONED, never replicated)");
// HOT cluster — second SPSCRing anchor (the WS family; same partition, same reasoning).
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(ws_rings) % 64 == 0]
static_assert(offsetof(OrderManagerState<64>, ws_rings) % 64 == 0,
              "ws_rings (HOT cluster ring 2) MUST start at a cache-line boundary. "
              "See spsc-ring-embedded-in-hot-struct-cluster-discipline.md.");
// The WS family is the SAME type as result_rings, so the partition pin above covers its size too;
// this one pins that they did not silently DIVERGE (a depth change to one and not the other would
// break the shared OmsCmdRings helpers' node arithmetic in a way no call site would show).
static_assert(sizeof(OrderManagerState<64>::ws_rings) == sizeof(OrderManagerState<64>::result_rings),
              "the WS and REST families must stay the SAME shape — the OmsCmdRings helpers derive "
              "the node lane identically for both, so a divergent depth silently mis-routes one");
// SIZE PIN — amendment (m) asked for a `check_struct_size_budget.py` MANIFEST row here. Measured:
// this header FAILS TO LINK a standalone probe (4 undefined simdjson refs), exactly like
// `NodeSlowState<64>`, whose row that tool's manifest deliberately omits for the same reason —
// adding it returns rc 2 and REDs the whole gate. So the coverage lands in the STRONGER form the
// manifest itself prescribes (CLAUDE.md gradient: compile-time enforcement > CI check), and the
// tool's blind spot for link-heavy headers stays homed as TECH_DEBT-309 rather than worked around.
// Re-derive after a deliberate layout change; a surprise here is a silent growth.
static_assert(sizeof(OrderManagerState<64>) == 457984,
              "OrderManagerState<64> size moved. Expected 457,984 B (3b(ii) commit 4 leaf 1 added "
              "the per-node ws_rings family: +1,920 B over the single ring it replaced).");

// WARM cluster — Portfolio anchor (per-fill bookkeeping cluster start).
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(portfolio) % 64 == 0]
static_assert(offsetof(OrderManagerState<64>, portfolio) % 64 == 0,
              "Portfolio (WARM cluster anchor) MUST start at a cache-line boundary "
              "(separates HOT event_log from WARM per-fill state).");
// COLD cluster — adapter anchor (boot-set fields cluster start).
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(adapter) % 64 == 0]
static_assert(offsetof(OrderManagerState<64>, adapter) % 64 == 0,
              "ExchangeAdapter (COLD cluster anchor) MUST start at a cache-line boundary "
              "(separates WARM per-fill state from COLD boot-set state).");
// Cross-thread observability cluster — total_submitted anchor.
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(total_submitted) % 64 == 0]
static_assert(offsetof(OrderManagerState<64>, total_submitted) % 64 == 0,
              "Observability atomics cluster (total_submitted/filled/rejected) MUST be "
              "alignas(64)-isolated. Snapshot publisher reads at 60 Hz; isolation "
              "prevents reads from invalidating drainer-written neighbor warm fields. "
              "See cross-thread-snapshot-publish-cluster-isolation.md (ND1).");
// Cross-thread safety CAS cluster — flatten_pending anchor.
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(flatten_pending) % 64 == 0]
static_assert(offsetof(OrderManagerState<64>, flatten_pending) % 64 == 0,
              "Safety CAS atomics cluster (flatten_pending/recovery_until_us) MUST be "
              "alignas(64)-isolated. N slow-path threads CAS-contend; isolation prevents "
              "RFO storms on neighbor cold fields. "
              "See cross-thread-snapshot-publish-cluster-isolation.md (ND1).");
// v5.15.5.C.2.1 (MEDIUM-2 close from /dod-audit) — explicit 8-byte alignment
// lock for the COLD-cluster post-bitmap gap. Money ks_min_balance needs
// 8-byte alignment per CLAUDE.md item 27 (struct padding determinism). The
// _pad_osf[7] field is the explicit pad; this assert confirms the offset
// remains 8-byte-aligned after future field additions to the COLD cluster.
// Compile-time check; zero runtime cost (offsetof + % 8 fold to constant).
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(ks_min_balance) % 8 == 0]
static_assert(offsetof(OrderManagerState<64>, ks_min_balance) % 8 == 0,
              "ks_min_balance (Money) MUST be 8-byte aligned. If this trips, "
              "the COLD cluster gained a non-8-aligned field between oms_state_flags "
              "and ks_min_balance. Re-check _pad_osf[7] explicit pad.");

//======================================================================
// [FUNCTION]_[OMS_CmdRingsPush]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CONCURRENCY] [SUPPORTIVE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[P4-pre-3b producer side: route a CMD_FILL_RESULT onto the OWNING node's result ring, addressed by the id's NODE lane — pure arithmetic, zero reads of leaf-owned state. Returns false on a full ring so the caller keeps its existing loud-drop behavior]
// [REFERENCE]_[DECISION]_[D-448]
//======================================================================
// [CODE]
//======================================================================
// The lane is stamped at Submit (P4-pre-3a). Deriving the node any other way here — decoding the
// slot and reading orders[slot].portfolio_slot — would be a cross-thread read at the flip AND
// slot-reuse-racy (the slot can belong to another node by the time the venue's report arrives).
// Takes the RINGS, not the OMS: the WS user-data producer is non-template code and cannot name
// OrderManagerState<F>, and a second copy of this arithmetic living over there is exactly the
// parallel-implementation shape that produced PARITY-071 one directory over.
static inline bool OMS_CmdRingsPush(OmsCmdRings* rings, const Command& cmd) {
    const tt::NodeIdx nn{(int16_t)OMS_OrderIdNode(cmd.order_id)};
    return SPSCRing_TryPush(&(*rings)[nn], cmd);
}

// The INTERIM central drain: one consumer walks every node's ring, which keeps each ring SPSC.
// The CURSOR is caller-held so a drain-to-empty loop is O(nodes + items) — it finishes node n
// before moving to n+1 and never revisits. The restart-at-0 forwarder below is O(nodes) PER ITEM,
// which is fine for a test popping one command and wrong for a live drain of a burst.
// RETIRES at the flip, when each node drains only its own ring.
static inline bool OMS_CmdRingsPop(OmsCmdRings* rings, int* cursor, Command* out) {
    for (; *cursor < MAX_EXECUTION_NODES; ++(*cursor)) {
        if (SPSCRing_TryPop(&(*rings)[tt::NodeIdx{(int16_t)*cursor}], out)) return true;
    }
    return false;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OMS_CmdRingsPush]
//======================================================================

//======================================================================
// [FUNCTION]_[OMS_RingFullFatalRecord]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CONCURRENCY] [LIVE_TRADING] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[D-479 as amended (gate #3 G3-2) — the DURABLE fatal record a PRODUCER thread writes when a fill ring stays full past its bounded push: the DECODED fill (venue id, our id, command type, price, qty, commission, commission asset, complete/terminal, reason — SIDE is not carried by Command/OrderResult: the WS "S" field is unparsed, ACC-7) — the producer's OWN counter bumped FIRST (its value rides in the record as fatal_n), the decoded fill written as a Health_Log CRITICAL line SECOND, the GLOBAL lane of kill_trip_request set LAST. Non-template on purpose: the WS parser is F-independent code. Fatal-path-only latency; every ring stays one-producer; H3-clean (Health_Log is per-call fopen/fprintf/fclose, any-thread-safe). The channel's presence is a live-readiness REFUSE check (health_log_path_set)]
// [REFERENCE]_[DECISION]_[D-479]
//======================================================================
// [CODE]
//======================================================================
// WHY the record is a Health_Log line and not a ring/mailbox/file: every in-memory carrier is
// unread by construction in the one case this exists for (a stuck consumer), and the rejected
// overflow side-ring argument applies to each of them verbatim. The house durable channel is
// already read by the GUI and outlives the process. E.1.4's venue-truth reconcile re-derives
// the fill from GetMyTrades; this line is what tells the operator WHICH fill to look for.
//
// ORDER matters: counter FIRST (the producer's own telemetry, relaxed — its value rides in the
// record as fatal_n), record SECOND (durable), trip LAST — so by the time the composer consumes
// the lane the evidence already exists on disk. (Comment corrected 2026-09-04, AR-8 N-1: the
// 2026-09-03 wording had the first two swapped; the code was always counter-first.)
// `trip_word` = &agg.kill_trip_request (the producer stores the pointer at init — it cannot name
// AggregatorState<F> from non-template code); NULL is tolerated so a harness can exercise the
// record half alone. `own_counter` = the producer's relaxed atomic (ws_push_fatal / rest_push_fatal
// land with their producers in 3b(ii) commit 4); NULL skips the bump.
static inline void OMS_RingFullFatalRecord(std::atomic<uint32_t>* trip_word,
                                           KillTripSite site,
                                           std::atomic<uint64_t>* own_counter,
                                           const Command& cmd) {
    const unsigned long long n =
        own_counter ? (unsigned long long)(own_counter->fetch_add(1, std::memory_order_relaxed) + 1) : 0ULL;
    // node_id -1: the record is account-level — the id lane is the only routing the composer will
    // reconstruct, and it rides in our_id below. Display-only doubles: OrderResult carries them.
    const int durable = tt::Health_Log(tt::HEALTH_CRITICAL, "ring_full_fatal", -1,
                   "site=%s fatal_n=%llu FILL NOT BOOKED — venue_id=%s our_id=%llu cmd_type=%u "
                   "success=%d price=%.8f qty=%.8f commission=%.8f %s complete=%u terminal=%u "
                   "reason=%s (GLOBAL kill requested; restart-only — reconcile from venue truth)",
                   tt::KillTripSite_Name((uint8_t)site), n,
                   cmd.result.exchange_id[0] ? cmd.result.exchange_id : "(none)",
                   (unsigned long long)cmd.order_id, (unsigned)cmd.type,
                   cmd.result.success, cmd.result.avg_fill_price, cmd.result.fill_qty,
                   cmd.result.commission,
                   cmd.result.commission_asset[0] ? cmd.result.commission_asset : "(asset?)",
                   (unsigned)cmd.result.order_complete, (unsigned)cmd.result.venue_terminal,
                   cmd.result.error_message[0] ? cmd.result.error_message : "(none)");
    // The stderr line carries the SAME decoded fill so an operator without a health log path
    // still has the recovery input in the terminal scrollback. Health_Log returns 0 when
    // health_log_path is empty (the cfg default) — that is a live-readiness REFUSE
    // (health_log_path_set), so live capital never reaches this line without the channel;
    // paper/shadow says so loudly instead of pretending the record was durable (V-1 M-3).
    std::fprintf(stderr,
                 "[OMS] RING FULL FATAL at %s (fatal_n=%llu): fill NOT booked — venue_id=%s our_id=%llu "
                 "cmd_type=%u qty=%.8f @ %.8f commission=%.8f %s reason=%s; GLOBAL kill requested "
                 "(restart-only). %s\n",
                 tt::KillTripSite_Name((uint8_t)site), n,
                 cmd.result.exchange_id[0] ? cmd.result.exchange_id : "(none)",
                 (unsigned long long)cmd.order_id, (unsigned)cmd.type,
                 cmd.result.fill_qty, cmd.result.avg_fill_price, cmd.result.commission,
                 cmd.result.commission_asset[0] ? cmd.result.commission_asset : "(asset?)",
                 cmd.result.error_message[0] ? cmd.result.error_message : "(none)",
                 durable ? "The health log holds the durable record."
                         : "HEALTH LOG UNAVAILABLE (health_log_path empty, or unwritable — a LIVE boot refuses the latter at check_health_log_path_set) — this line is the ONLY record.");
    if (trip_word) trip_word->fetch_or(KILL_TRIP_LANE_GLOBAL(site), std::memory_order_release);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OMS_RingFullFatalRecord]
//======================================================================

//======================================================================
// [FUNCTION]_[OMS_CmdRingsPushOrTrip]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CONCURRENCY] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the NEVER-DROP push for a venue producer thread: try, then pause-spin within a raw-TSC budget, and on exhaustion go LOUD-FATAL (durable record + GLOBAL kill request) rather than drop a fill. Defined AFTER OMS_RingFullFatalRecord because it calls it]
// [REFERENCE]_[DECISION]_[D-479]
//======================================================================
// [CODE]
//======================================================================
// The NEVER-DROP push for a venue producer thread: try, then pause-spin within a raw-TSC budget,
// and on exhaustion go LOUD-FATAL (durable record + GLOBAL kill request) rather than drop a fill.
// Non-template for the same reason as the family: the WS producer is F-independent code.
//
// `abort` is nullptr at BOTH venue sites (A-2 F-1, orchestrator-confirmed): shutdown joins the WS
// producer BEFORE it stops the composer, so the consumer is live through the join and the <=10 ms
// budget already bounds the wait. An abort wired here would manufacture a false restart-only fatal
// at a clean shutdown — the one outcome worse than waiting.
static inline bool OMS_CmdRingsPushOrTrip(OmsCmdRings* rings, const Command& cmd,
                                          uint64_t budget_cycles,
                                          const std::atomic<int>* abort,
                                          std::atomic<uint32_t>* trip_word,
                                          KillTripSite site,
                                          std::atomic<uint64_t>* own_counter) {
    const tt::NodeIdx nn{(int16_t)OMS_OrderIdNode(cmd.order_id)};
    if (SPSCRing_TryPushBounded(&(*rings)[nn], cmd, budget_cycles, abort)) return true;
    OMS_RingFullFatalRecord(trip_word, site, own_counter, cmd);
    return false;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OMS_CmdRingsPushOrTrip]
//======================================================================

//======================================================================
// [FUNCTION]_[OMS_ResultPop]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CONCURRENCY] [SUPPORTIVE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[P4-pre-3b consumer side, INTERIM: pop the next CMD_FILL_RESULT under the central topology by walking the per-node rings in node order (FIFO within a node). One consumer across all rings keeps every ring SPSC. RETIRES at the flip, when each node drains only its own ring]
// [REFERENCE]_[DECISION]_[D-448]
//======================================================================
// [CODE]
//======================================================================
// INTERIM restart-at-0 forwarder, kept for its TEST callers only — it re-walks from node 0 on
// every call, so a drain-to-empty loop over it is O(nodes) per item. Both PRODUCTION consumers
// take the cursor form `OMS_CmdRingsPop(rings, &cursor, out)` instead.
// ORDER NOTE (deliberate, measured): the old single ring drained in PUSH order across all nodes;
// this drains NODE-MAJOR. Per-node FIFO — the only order a node's own accounting depends on — is
// preserved exactly. Cross-node interleaving changes, which is the same reordering the flip makes
// permanent and PARITY-052 already pins as schedule-defined (totals are order-invariant; peaks and
// emit ids are not).
template <unsigned F>
inline bool OMS_ResultPop(OrderManagerState<F>* oms, Command* out) {
    int cursor = 0;
    return OMS_CmdRingsPop(&oms->result_rings, &cursor, out);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OMS_ResultPop]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderManager_FillResultCallback]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CONCURRENCY] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the adapter-worker's ONLY write path into the OMS — push a Command into the SPSC result_queue and return]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
static void OrderManager_FillResultCallback(void* user_ctx,
                                             uint64_t client_id,
                                             const OrderResult* result) {
    OrderManagerState<F>* oms = (OrderManagerState<F>*)user_ctx;
    Command cmd;
    cmd.type     = (uint8_t)CMD_FILL_RESULT;
    cmd.order_id = client_id;
    cmd.result   = *result;
    // D-479: the REST worker never DROPS a fill. It waits within the budget, and if the ring is
    // still full it writes the durable record and requests the GLOBAL kill — an unbooked fill is a
    // capital divergence from venue truth, so the engine stops rather than continues wrong.
    (void)OMS_CmdRingsPushOrTrip(&oms->result_rings, cmd, OMS_RING_PUSH_BUDGET_CYCLES,
                                 /*abort=*/nullptr,
                                 oms->agg ? &oms->agg->kill_trip_request : nullptr,
                                 KTS_REST_RING_FULL, &oms->ring_full_fatal);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Templated on F so it matches the OrderManagerState<F>* user_ctx. Each F
// instantiation produces a distinct function pointer, which the adapter
// stores type-erased as `OrderCallback`. The callback is the only path
// by which the worker thread mutates OMS state — it pushes a Command
// into the result_queue and returns. The drainer thread later picks it
// up via OrderManager_Tick.
//
// Pushing into the SPSC ring is wait-free. If the queue is full (should
// never happen with size 256 unless something is very wrong), the result
// is dropped with a log message.
//======================================================================
// [END_FUNCTION]_[OrderManager_FillResultCallback]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderManager_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [REFERENCE]_[INVARIANT]_[H17]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one-call OMS init — registry-driven OMS_INIT_AUTOPOPULATE; a new OMS field is ONE registry row]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OrderManager_Init(OrderManagerState<F>* oms,
                              const ExchangeAdapter<F>& adapter,
                              int live_trading,
                              int partial_exit_enabled,
                              Money starting_balance,
                              int event_log_mode = 0,
                              const char* event_log_path = "logging/order_events.bin") {
    OMS_INIT_AUTOPOPULATE(oms, adapter, live_trading, partial_exit_enabled,
                          starting_balance, event_log_mode, event_log_path);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Zero the order table, clear the bitmap, install the adapter and the
// live_trading flag. The adapter is copied by value — the caller still
// owns whatever the adapter.ctx points at, but the function pointers
// and the ctx pointer are captured into the OMS for its lifetime.
//
// In paper mode the caller passes a default-constructed (zero-initialized)
// adapter — all function pointers null. The OMS will short-circuit Submit
// to FILLED before touching them.
//
// OMS owns the bank state since v4.x (originally Phase 03 chunk 1B).
// OrderManager_Init takes starting_balance + fee_rate so the OMS is fully
// self-contained from init onwards. EventLoopState_Init takes an OMS
// pointer and forwards all financial reads through the OMS.
//
// event_log_mode parameter (default 0) — a LOGGING toggle since P3-f (D-441):
//   0 = event-log append OFF. Booking is IDENTICAL to mode 1 (one pipeline:
//       ProcessFillCommand→HandleFill→FillEvent→composer apply).
//   1 = event-log append ON (fill/rejection/terminal/reconcile audit rows ride
//       the funnel into the persisted OrderEventLog; warm-restart folds replay it).
//   2-3 = reserved for future modes. Stored as 2-bit slot in oms_state_flags
//         (v5.15.5.C.3 MULTI_BIT slot — see FOREACH_OMS_STATE_MULTI_BIT).
//   Value 0 stays VALID (append-off) — no H21 tombstone owed; the OLD mode-0
//   direct-booking body (EventLoop_OnEvent) + the paper count-and-return Submit
//   shortcut died at P3-f.
//
// partial_exit_enabled parameter (v5.15.5.C.3 Finding A):
//   0 = single-leg geometry; slot index == node_id (1:1 mapping).
//   1 = paired-leg geometry; slot index = 2*node_id + leg (legs A+B per core).
//   Set as BIT in oms_state_flags (MASK_OMS_STATE_PARTIAL_EXIT_ENABLED).
//   Pre-Finding A: engine boot called OMS_STATE_FLAG_SET(PARTIAL_EXIT_ENABLED)
//   externally after OrderManager_Init returned (Class-18 mirror at the
//   external SET site). Post-Finding A: passed as OrderManager_Init parameter;
//   the registry walk inside OMS_INIT_AUTOPOPULATE sets the bit via the
//   BIT-kind row for `partial_exit_enabled`.
//
// v5.9.5e — `event_log_path` lets callers separate the in-memory event log
// infrastructure (single-writer mode=1 used by both live + backtest for
// fill+drain pipeline parity since v4.7.15) from the on-disk persistence
// (live engine only — restart-recovery via replay). Backtest passes nullptr
// or "" to use in-memory-only mode=1: no disk load, no append-on-write.
// Pre-v5.9.5e the path was hardcoded "logging/order_events.bin" and
// backtest_mode=1 silently inherited live OMS state across runs (stale
// balance, polluted next_event_id), breaking backtest hermeticity. The
// feature/label collection pipeline doesn't read OMS, so ML training
// output was unaffected — but Past Runs P&L + trade history started
// from the contaminated balance.
//
// v5.15.5.C.3 Phase 3b — body migration from ~140 LOC manual init to a
// single OMS_INIT_AUTOPOPULATE invocation. All scalar/BIT/MULTI_BIT/ATOMIC
// field init lives in FOREACH_OMS_FIELD (MemHeaders/OmsFieldRegistry.hpp);
// sub-struct inits (Portfolio, Order per-slot loop, FillRecord per-slot,
// SPSC rings, OrderEventLog conditional + StartAsyncWriter) live in the
// AUTOPOPULATE macro body. Adding a new OMS-level field is now ONE row in
// the canonical registry; INIT/RESET/PERSIST views auto-flow.
//
// v5.15.5.F.4c.3 WIP2d-1.B.1 — `fee_rate` param DELETED. Per-Order fee_rate lives on
// Order::pre_resolved (set at submit via Order_BindPreResolved with cfg.nodes[c]). OMS no
// longer holds a global fee_rate. Callers drop the arg.
//======================================================================
// [END_FUNCTION]_[OrderManager_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderManager_Submit]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING]]
// [REFERENCE]_[DESIGN_SPEC]_[[decision-time-data-binding-pattern] [adversarial-pessimistic-simulation-discipline.md]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[allocate slot + bind pre-resolved cfg + async adapter submit (or paper synth fill) — returns immediately]
// [REFERENCE]_[CLASS]_[46]
// [REFERENCE]_[DECISION]_[[D-106] [D-202]]
// [REFERENCE]_[INVARIANT]_[H20]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline uint64_t OrderManager_Submit(OrderManagerState<F>* oms, const SubmitCommand<F>& cmd) {
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — option (l): SubmitCommand POD is canonical arg.
    // Local extraction matches prior positional names so body internals are unchanged.
    // Production callers MUST set cmd.node_cfg = &cfg.nodes[c] (discipline; runtime TT_ASSERT
    // backstop catches misses at HandleFill).
    const tt::SlotIdx            portfolio_slot = cmd.portfolio_slot;
    const OrderType              type        = (OrderType)cmd.order_type;
    const Money                 qty         = cmd.qty;
    const Money                 intended_tp = cmd.intended_tp;
    const Money                 intended_sl = cmd.intended_sl;
    const uint8_t                strategy_id = cmd.strategy_id;
    const Money                 event_price = cmd.event_price;
    const uint8_t                leg         = cmd.leg;
    const ::PerNodeCfg<F>* const node_cfg    = cmd.node_cfg;
    uint64_t id = oms->next_order_id++;

    // P3-f (D-441 mode-0 unify): the mode-0 paper count-and-return shortcut is
    // DELETED — every mode allocates a slot + runs the synth/venue fill through
    // ProcessFillCommand→HandleFill (ONE booking pipeline; the mode bit only
    // gates the event-log append). The shortcut was the OMS half of the second
    // booking path (OnEvent booked directly while the OMS just counted).

    // Allocate a slot.
    uint16_t free_mask = (uint16_t)~oms->order_bitmap;
    if (free_mask == 0) {
        std::fprintf(stderr,
                     "[OMS] order table full (%d slots), dropping submission "
                     "for node=%d type=%u\n",
                     MAX_INFLIGHT_ORDERS, (int)portfolio_slot, (unsigned)type);
        return 0;
    }
    int slot = __builtin_ctz((unsigned int)free_mask);
    oms->order_bitmap |= (uint16_t)(1u << slot);

    // v5.11.5.B — encode slot in the upper 4 bits of the order id. The wire
    // representation (clientOrderId on the exchange) and Order::id both
    // carry this encoded value. ProcessFillCommand decodes the slot
    // directly from cmd.order_id for O(1) lookup, replacing the prior
    // O(MAX_INFLIGHT_ORDERS) linear scan over order_bitmap.
    //
    // Encoding (see § order-id LANE ENCODING): bits 63..60 = slot, bits 59..56 = NODE lane,
    // bits 55..0 = monotonic counter.
    //
    // Audit: LATENCY_OPTIMIZATION_AUDIT.md Part 9. Plan: master plan
    // v5.11.5 item 3.
    //
    // P4-pre-3a: stamp the OWNING node into the id at submit — the one site that knows the
    // portfolio slot for certain, on the thread that owns it. Every downstream producer then
    // routes by arithmetic instead of reading `orders[]` across threads. The derive is the
    // house canonical (`Sharded_SlotNode` via BITMAP_SLOT_NODE), not a second spelling.
    const int submit_node = BITMAP_SLOT_NODE((int)portfolio_slot,
                                             OMS_STATE_FLAG_IS_SET(*oms, PARTIAL_EXIT_ENABLED));
    // The counter is MASKED to its 56 bits rather than left to run into the lanes: at
    // 7.2e16 ids it can only wrap in theory, but a wrap that silently rewrote the routing
    // key would misroute fills, while a wrapped counter merely repeats an id the slot lane
    // still disambiguates. Fail toward the recoverable side.
    uint64_t encoded_id = (id & ORDER_ID_COUNTER_MASK)
                        | ((uint64_t)submit_node << ORDER_ID_NODE_SHIFT)
                        | ((uint64_t)slot        << ORDER_ID_SLOT_SHIFT);
    Order_Init(&oms->orders[slot], encoded_id, portfolio_slot, type);
    id = encoded_id;  // returned to caller + used in cmd.order_id below
    oms->orders[slot].submitted_at_us = (uint64_t)
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    oms->orders[slot].requested_qty = qty;
    oms->orders[slot].intended_tp   = intended_tp;
    oms->orders[slot].intended_sl   = intended_sl;
    oms->orders[slot].strategy_id   = strategy_id;
    oms->orders[slot].event_price   = event_price;
    Order_SetLeg(&oms->orders[slot], leg);  // P.3: 0/1 for partial exits
    Order_SetState(&oms->orders[slot], ORDER_SUBMITTED);
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — decision-time data binding: pre-resolve fee_rate /
    // slippage_pct from per-core cfg onto Order before any fill can arrive. is_maker
    // defaults to false at Order_Init (set later by adapter executionReport for LIMIT
    // orders; MARKET orders are always taker — current engine is MARKET-only).
    // Per DESIGN_SPECS/decision-time-data-binding-pattern.md + branchless-dispatch-
    // discipline.md (branchless via cmov-to-stub; always-call Order_BindPreResolved
    // regardless of whether caller passed cfg). nullptr → zero-init stub → pre_resolved
    // values stay FPN_Zero (same effective behavior as skip-when-null, but no branch).
    static const ::PerNodeCfg<F> NULL_PER_NODE_CFG_STUB{};
    const ::PerNodeCfg<F>* effective_cfg = node_cfg ? node_cfg : &NULL_PER_NODE_CFG_STUB;
    Order_BindPreResolved(&oms->orders[slot], *effective_cfg);
    // A25 (D-205): tp_pct is resolved at the submit SITE (ResolvePerFillTpPct needs
    // StrategyParameters, not include-reachable here) + carried on the command; set it on
    // the Order's pre_resolved beside the bound fee_rate/slippage_pct. 0 (unwired/test
    // paths) → handle_buy_fill keeps the legacy intended_tp anchor (bytewise-identical).
    oms->orders[slot].pre_resolved.tp_pct = cmd.tp_pct;
    oms->total_submitted.fetch_add(1, std::memory_order_relaxed);

    // Paper mode + event log (mode 1): push a synthetic fill result so
    // OMS_Tick runs the fill handler uniformly. The fill price is the
    // event_price captured at submit time. No adapter call needed.
    if (!BITMAP_IS_SET(oms->oms_state_flags, tt::MASK_OMS_STATE_LIVE_TRADING)) {
        Command cmd;
        cmd.type     = (uint8_t)CMD_FILL_RESULT;
        cmd.order_id = id;
        std::memset(&cmd.result, 0, sizeof(cmd.result));
        cmd.result.success        = 1;
        // v5.15.5.F.4d.1.E.0.10 A9 — apply paper/backtest slippage HERE: the single production slip SSoT
        // (D-202 + adversarial-pessimistic-simulation-discipline.md). Paper-gated BY CONSTRUCTION — this is
        // the !LIVE_TRADING branch; live books the real executionReport price untouched. Consumes the bound
        // pre_resolved.slippage_pct (set at Order_BindPreResolved above) — closes the bound-but-unread orphan
        // (the old EventLoop_OnEvent slip was dead-in-mode-1, now deleted). Pessimistic-sim: entry fills WORSE
        // (higher), exit fills WORSE (lower); slip_pct=0 (default) -> Money_Mul=0 -> no-op, so no guard branch.
        // Branchless sign-select per H20: both sides computed; cmov-select on the value (not a branch).
        const Money a9_slip      = Money_Mul(event_price, oms->orders[slot].pre_resolved.slippage_pct);
        const Money a9_buy_fill  = Money_Add(event_price, a9_slip);   // entry (BUY): pay higher
        const Money a9_sell_fill = Money_Sub(event_price, a9_slip);   // exit (SELL): receive lower
        const Money a9_fill      = (type == ORDER_MARKET_SELL) ? a9_sell_fill : a9_buy_fill;
        cmd.result.avg_fill_price = Money_ToDouble(a9_fill);
        cmd.result.fill_qty       = Money_ToDouble(qty);
        // A17 (.E.0.10): the paper synth fills the FULL requested qty in ONE synthetic
        // event → terminal by construction → order_complete=1, so OMS_Tick's gate
        // (order_complete ? ORDER_FILLED : ORDER_PARTIAL, ~:1432) lands FILLED instead
        // of a slot-leaking ORDER_PARTIAL. This is RBP Class 46's false-positive surface:
        // asserting completeness here encodes a structural truth (fill_qty == requested
        // qty on the synchronous synth path), NOT an unverified assumption. The REST/live
        // path (BinanceAdapter) instead READS the venue's own "status" — D-106 let-the-
        // venue-decide — because there completeness is not guaranteed by construction.
        cmd.result.order_complete = 1;
        std::strncpy(cmd.result.exchange_id, "PAPER",
                     sizeof(cmd.result.exchange_id) - 1);
        // PAPER: the plain push, never the bounded one. The producer here IS the consumer thread,
        // so a spin would be a self-wait that can never clear — the one place where waiting is
        // strictly worse than failing. The full is structurally unreachable while the pin above
        // holds (OMS_RESULT_RING_PER_NODE >= MAX_INFLIGHT_ORDERS + drain-to-empty before
        // DrainSubmit + a pool-full submit pushing nothing), so this arm is the pin's guard, not an
        // expected path — and if it ever runs, the pin's premise is broken and the record says so.
        if (!OMS_CmdRingsPush(&oms->result_rings, cmd)) {
            // The pin's premise is broken if we are here. Record it durably and FREE THE SLOT: the
            // synthetic fill never reached the drainer, so nothing downstream will ever complete
            // this order, and leaving the slot allocated leaks it out of a 16-deep pool for the
            // life of the process.
            OMS_RingFullFatalRecord(oms->agg ? &oms->agg->kill_trip_request : nullptr,
                                    KTS_PAPER_SYNTH_RING_FULL, &oms->ring_full_fatal, cmd);
            Order_SetState(&oms->orders[slot], ORDER_REJECTED);
            oms->order_bitmap &= (uint16_t)~(1u << slot);
            // NOT total_rejected: that counter means "the VENUE rejected this order". Booking an
            // engine-side ring failure there would misreport capital as a venue outcome.
            return 0;
        }
        return id;
    }

    // Defensive: live mode requires a wired adapter.
    if (oms->adapter.submit_market_buy == nullptr ||
        oms->adapter.submit_market_sell == nullptr) {
        std::fprintf(stderr,
                     "[OMS] live mode but adapter is not wired, rejecting order %llu\n",
                     (unsigned long long)id);
        Order_SetState(&oms->orders[slot], ORDER_REJECTED);
        oms->total_rejected.fetch_add(1, std::memory_order_relaxed);
        oms->order_bitmap &= (uint16_t)~(1u << slot);
        return 0;
    }

    // Async submit to the adapter. The callback fires later from the
    // worker thread once the REST round trip completes.
    double qty_d = Money_ToDouble(qty);
    int submit_ok = (type == ORDER_MARKET_BUY)
        ? oms->adapter.submit_market_buy(oms->adapter.ctx, id, qty_d,
                                          OrderManager_FillResultCallback<F>,
                                          (void*)oms)
        : oms->adapter.submit_market_sell(oms->adapter.ctx, id, qty_d,
                                           OrderManager_FillResultCallback<F>,
                                           (void*)oms);
    if (!submit_ok) {
        std::fprintf(stderr,
                     "[OMS] adapter rejected enqueue for order %llu, freeing slot\n",
                     (unsigned long long)id);
        Order_SetState(&oms->orders[slot], ORDER_REJECTED);
        oms->total_rejected.fetch_add(1, std::memory_order_relaxed);
        oms->order_bitmap &= (uint16_t)~(1u << slot);
        return 0;
    }
    return id;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Paper mode:
//   Mode 0 (legacy): Bump total_submitted and total_filled, return the next
//   id. No slot allocation, no adapter call, no result queue traffic.
//   Symmetric with the legacy fire-and-forget pattern but routed through the
//   OMS so the counters are consistent across paper and live.
//
//   Mode 1 (event log): allocate a slot, mark ORDER_SUBMITTED, push a
//   synthetic CMD_FILL_RESULT into the result queue so OMS_Tick handles
//   it uniformly. The fill handler then opens/closes portfolio slots and
//   appends to the event log, just like in live mode.
//
// Live mode:
//   1. Allocate a free slot from the bitmap. Drop on full (logged).
//   2. Populate the Order with id, type, qty, mark ORDER_SUBMITTED.
//   3. Call adapter.submit_market_buy or _market_sell with the OMS
//      pointer as user_ctx and OrderManager_FillResultCallback<F> as
//      the callback. The adapter enqueues to its worker thread and
//      returns immediately — Submit does NOT block.
//   4. If the adapter rejects the enqueue (its queue is full), mark
//      the slot REJECTED and free it.
//
// qty is Money per the FPN_Binary-only-accounting rule in CLAUDE.md.
//
// Phase 03 chunk 3: extra context parameters for the fill handler.
//   intended_tp / intended_sl: TP/SL to apply at fill time (entry only).
//   strategy_id: STRATEGY_* constant for trade log CSV.
//   event_price: market price at submit time; used as the fill price in
//     paper mode (no adapter callback to supply one).
//======================================================================
// [END_FUNCTION]_[OrderManager_Submit]
//======================================================================

//======================================================================
// [FUNCTION]_[OMS_PushSubmit]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[producer-side submit funnel — push the SubmitCommand into its portfolio SLOT's SPSC queue; drainer pops serially]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline bool OMS_PushSubmit(OrderManagerState<F>* oms, const SubmitCommand<F>& cmd) {
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — option (l): SubmitCommand POD is canonical arg.
    // No internal assembly; caller constructs the cmd struct directly + we push it.
    // Eliminates the prior 9-field unpack/repack ceremony.
    if ((int)cmd.portfolio_slot < 0 || (int)cmd.portfolio_slot >= MAX_PORTFOLIO_POSITIONS) {
        std::fprintf(stderr,
                     "[OMS] PushSubmit: invalid portfolio_slot=%d (max=%d)\n",
                     (int)cmd.portfolio_slot, MAX_PORTFOLIO_POSITIONS);
        return false;
    }
    bool pushed = SPSCRing_TryPush(&oms->submit_queues[cmd.portfolio_slot], cmd);
    if (!pushed) {
        std::fprintf(stderr,
                     "[OMS] PushSubmit: queue full for node=%d type=%u "
                     "(drainer starved?)\n",
                     (int)cmd.portfolio_slot, (unsigned)cmd.order_type);
    }
    return pushed;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v4.7.37 (Phase B reordered): producer threads call OMS_PushSubmit instead
// of OrderManager_Submit. Drainer thread pops from submit_queues and calls
// OrderManager_Submit serially, preserving the documented "drainer is sole
// Submit caller" OMS contract.
//
// Per-core SPSC: each node_id has its own queue. Today's caller (producer
// slow-path) is the sole producer for ALL queues — still SPSC per ring.
// When Phase C spawns per-core slow-paths, each thread is the sole producer
// for its own ring. SPSC contract holds in both modes.
//
// Returns false if the queue is full (caller should consider this an error
// — slow-path submission backlog suggests drainer is starved). Returns true
// on successful push.
//======================================================================
// [END_FUNCTION]_[OMS_PushSubmit]
//======================================================================

//======================================================================
// [FUNCTION]_[OMS_DrainSubmit]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[drainer-side pop of the per-SLOT submit queues — calls OrderManager_Submit serially; returns count drained]
//======================================================================
// [CODE]
//======================================================================
// D3 close (a-class, 2026-08-26): the parameter is a SLOT count and always was —
// every production caller passes DrainerConstants.drain_count = registered_count
// × (partials ? 2 : 1). It was NAMED num_nodes and CLAMPED against
// MAX_EXECUTION_NODES, which is benign only while the two caps are equal (16==16
// is a coincidence, not an invariant — Limits.hpp); if MAX_PORTFOLIO_POSITIONS
// ever grew past the node cap, the old clamp would silently strand the leg-B
// queues. Named + clamped in the space it actually iterates.
template <unsigned F>
inline int OMS_DrainSubmit(OrderManagerState<F>* oms, int num_slots) {
    int drained = 0;
    int max = (num_slots > MAX_PORTFOLIO_POSITIONS) ? MAX_PORTFOLIO_POSITIONS : num_slots;
    for (int s = 0; s < max; ++s) {
        SubmitCommand<F> cmd;
        while (SPSCRing_TryPop(&oms->submit_queues[tt::SlotIdx{(int16_t)s}], &cmd)) {
            // v5.15.5.F.4c.3 WIP2d-1.B.1 — option (l): pass POD directly; no unpack ceremony.
            OrderManager_Submit(oms, cmd);
            drained++;
        }
    }
    return drained;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v4.7.37 (Phase B reordered): called from the drainer thread's loop, before
// OrderManager_Tick. Drains all per-core submit queues and calls
// OrderManager_Submit for each command. Single-threaded — preserves OMS
// contract.
//
// Returns the number of commands drained.
//======================================================================
// [END_FUNCTION]_[OMS_DrainSubmit]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderManager_AccountMakerTakerFee]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING]]
// [REFERENCE]_[INVARIANT]_[H20]
// [REFERENCE]_[CLASS]_[18]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE fee-bookkeeping body — branchless maker/taker counters + fee buckets; Class-18 mirror close]
// [REFERENCE]_[DESIGN_SPEC]_[branchless-dispatch-discipline.md]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OrderManager_AccountMakerTakerFee(
    OrderManagerState<F>* oms, int is_maker, Money fee) {
    oms->total_fees = Money_Add(oms->total_fees, fee);
    const bool maker = is_maker != 0;
    oms->maker_fills_count   = (uint32_t)(oms->maker_fills_count + (uint32_t)maker);
    oms->taker_fills_count   = (uint32_t)(oms->taker_fills_count + (uint32_t)(!maker));
    oms->total_maker_fees    = maker ? Money_Add(oms->total_maker_fees, fee) : oms->total_maker_fees;
    oms->total_taker_fees    = maker ? oms->total_taker_fees : Money_Add(oms->total_taker_fees, fee);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.C.2 (S5): single-source-of-truth for the 8-line fee bookkeeping
// duplicated byte-identical at both entry-fill and exit-fill sites in
// HandleFill (only differing in entry_fee vs exit_fee). Class 18 mirror
// close (CLAUDE.md item 19 + DESIGN_SPECS/structural-fix-preferred-
// decision-framework.md). Future fee categories (taker_rebate,
// partial_taker, etc.) extend this single helper rather than touching
// N call sites.
//
// v5.15.5.F.4c.3 WIP2d-1.B.1 — branchless via mask-select counters + ternary FPN_Binary store.
// Pre-r-6: `if (is_maker) { maker++; maker_fees+= } else { taker++; taker_fees+= }` — 1 branch per fill.
// Post-r-6: always-compute both counter increments via mask (0 or 1); ternary FPN_AddSat-or-nop on each
// fee bucket lowers to cmov. Per branchless-dispatch-discipline.md Pattern 3 mask-select. ~3-5 cycles
// extra per fill (drainer slow path; ~0.005% of 100μs budget) for deterministic latency.
//======================================================================
// [END_FUNCTION]_[OrderManager_AccountMakerTakerFee]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderManager_LedgerApplyFill]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING]]
// [THREAD]_[[COMPOSER_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[THE one booking body (D-444): every FillEvent — live composer, backtest inline apply, the direct test-harness path, (E.1.4) boot-replay — books through HERE into the EXISTING oms fields (restore/reset/persist ride their registry rows unchanged; ownership-by-TOPOLOGY, only the composer calls the ring apply). Fee triple books via OrderManager_AccountMakerTakerFee — ONE fee body, ONE writer (A-1 T2); fe.fee = this leg's EXECUTION fee only, entry fee rides SELL net (no double-count in either surface). The drift oracle's independent leg (start + Σnode_realized) must NEVER route through this body — it would self-confirm. Moved from EngineCommon at P3-b (natural home: pure OMS math; the leaves' direct-emit fallback needs it below EngineCommon in the include graph)]
// [REFERENCE]_[DECISION]_[[D-440] [D-441] [D-444]]
// [REFERENCE]_[INVARIANT]_[[H4]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OrderManager_LedgerApplyFill(OrderManagerState<F>& oms, const FillEvent<F>& fe) {
    // BUY legs carry net == 0 (buy books Δfee only — gate accounting F8); SELL legs carry
    // the signed realized delta. Addition is exact (Money): the TOTALS are order-free.
    OrderManager_AccountMakerTakerFee(&oms, (int)fe.is_maker, fe.fee);
    oms.balance      = Money_Add(oms.balance,      fe.net);
    oms.realized_pnl = Money_Add(oms.realized_pnl, fe.net);
    // Source-purity (D-441 / gate accounting F4): the ratchet reads ONLY the realized-equity
    // balance — NEVER an unrealized-inclusive value. Per-fill-TRUE by construction.
    oms.ks_peak_balance = Money_Max(oms.ks_peak_balance, oms.balance);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OrderManager_LedgerApplyFill]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderManager_FillEmitDirect]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [SUPPORTIVE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the default fill_emit sink: IMMEDIATE apply through the ONE booking body (null-agg test-harness path — preserves every direct-HandleFill oracle's arrival-order semantics). NOT a twin formula: same body as the ring apply, different schedule (single-thread). Production boots rewire fill_emit to the ring sink]
// [REFERENCE]_[DECISION]_[[D-444]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OrderManager_FillEmitDirect(OrderManagerState<F>* oms, const FillEvent<F>& fe) {
    OrderManager_LedgerApplyFill(*oms, fe);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OrderManager_FillEmitDirect]
//======================================================================

//======================================================================
// [FUNCTION]_[OMS_GuardTakerBoundFeeBasis]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [SUPPORTIVE]]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-154]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[dormant fee-desync tripwire — a maker fill on a TAKER-bound fee_rate fails LOUD, never silently]
// [REFERENCE]_[INVARIANT]_[H20]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OMS_GuardTakerBoundFeeBasis(const Order<F>* o) {
    if (__builtin_expect(Order_GetIsMaker(o) != 0, 0)) {
        fprintf(stderr, "[FATAL] OrderManager fee-desync: maker fill on a TAKER-bound fee_rate — LIMIT orders "
                        "were enabled without re-resolving fee_rate from is_maker at fill (TECH_DEBT-154).\n");
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// GUARD — maker/taker fee-desync (DORMANT; the reactivatable-assumption shape, Class-40 sibling).
// pre_resolved.fee_rate is bound at SUBMIT as TAKER (the engine is MARKET-only — OrderManager_Submit refuses
// non-MARKET, and MARKET fills are always taker). So a maker fill CANNOT occur today: this is a NEVER-TAKEN
// branch (zero behavior change now). If it ever fires, LIMIT orders were enabled WITHOUT re-resolving
// fee_rate from is_maker at fill time → the taker-rate fee is charged + bucketed as maker (a live fee
// over-charge + wrong P&L split). Fail LOUD so it can't ship silently. The real fix: re-resolve
// pre_resolved.fee_rate from is_maker BEFORE enabling LIMIT orders (TECH_DEBT-154). __builtin_expect-rare
// per H20 exception (cold capital-check; matches the file's existing FILE*-null guard convention).
//======================================================================
// [END_FUNCTION]_[OMS_GuardTakerBoundFeeBasis]
//======================================================================

//------------------------------------------------------------------------------------------------------
// [SECTION]_[Pattern 1 1D type dispatch handlers — v5.15.5.F.4c.3 WIP2d-1.B.1]
//------------------------------------------------------------------------------------------------------
// BUY/SELL dispatch via fn pointer table indexed by Order_GetType(o). Per branchless-dispatch-
// discipline.md Pattern 1 + H20 invariant. Class 28 first canonical. Inner bodies use Pattern 3
// mask-selects for all data-dependent dispatch (TP/SL/INSIDE reason, peak balance via FPN_Max,
// last_realized_return write mask-gated, last_is_maker / last_was_win bitmaps mask-select).
// FILE* null guards tagged __builtin_expect per H20 exception #4 (fprintf side effects can't be
// cheaply mask-gated; Phase 2 Portfolio/TradeLog mask-param refactor handles the remaining cases).
//------------------------------------------------------------------------------------------------------

//======================================================================
// [FUNCTION]_[handle_buy_fill]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING]]
// [REFERENCE]_[DECISION]_[D-173]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[entry fill — open portfolio slot at the ACTUAL fill (per-fill TP anchor) + fee bookkeeping + entry emit]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-154]
// [REFERENCE]_[DESIGN_SPEC]_[sink-fn-pointer-for-optional-side-effect-pattern.md]
//======================================================================
// [CODE]
//======================================================================
// BUY handler — entry fill: open portfolio slot + record entry fee + bump counters + trade log.
template <unsigned F>
inline void handle_buy_fill(OrderManagerState<F>* oms, Order<F>* o, Money fill_price, Money fill_qty,
                            Money booked_fee) {
    // Ship-B P3: the fee is resolved ONCE in OrderManager_HandleFill (venue-reported
    // commission preferred per D-173, computed fallback) and threaded here — the
    // handler no longer derives its own copy (booking-rule SSoT).
    const Money entry_fee  = booked_fee;
    OMS_GuardTakerBoundFeeBasis(o);   // dormant fee-desync guard (TECH_DEBT-154); never-taken while MARKET-only
    // P3-b flip (D-444): the BUY leg's global effect is Δfee ONLY (gate accounting F8), and it
    // rides the FillEvent — the composer books it through the ONE body (fee triple included).
    // The direct AccountMakerTakerFee call died here. No balance write at buy (flat-account).
    {
        const int partial_on = BITMAP_IS_SET(oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED) ? 1 : 0;
        const tt::NodeIdx fill_node{(int16_t)BITMAP_SLOT_NODE((int)o->portfolio_slot, partial_on)};
        oms->fill_emit(oms, FillEvent_Make<F>(
            o->portfolio_slot, fill_node, /*is_sell=*/0,
            (uint8_t)(Order_GetState(o) == ORDER_FILLED), (uint8_t)(Order_GetIsMaker(o) != 0),
            /*slot_flat=*/0, ++oms->fill_emit_seq[fill_node],
            fill_qty, fill_price, entry_fee, Money_Zero(),
            /*notional=*/Money_Mul(fill_price, fill_qty), /*entry_fee_leg=*/Money_Zero()));
    }
    // A25 (D-205): arm the trail anchor (original_tp) relative to the ACTUAL fill, not the
    // expected-entry intended_tp — post-A9 they diverge under slippage, so the 4 sharded
    // *_ExitAdjustSharded trails + the exit-bandit counterfactual (ControllerEventLoop.hpp:1749)
    // were arming at the wrong price. tp_pct (leg-effective, resolved at submit) carries the
    // per-fill fraction; tp_pct==0 → fallback to intended_tp = the legacy path (bytewise-identical).
    const Money per_fill_tp = !Money_IsZero(o->pre_resolved.tp_pct)
        ? Money_Add(fill_price, Money_Mul(fill_price, o->pre_resolved.tp_pct))
        : o->intended_tp;
    // P3-d-ii (A16 ACCUMULATE): the first leg opens the slot (TP/SL anchor at the first
    // fill per A25); subsequent legs of a partially-filling order ACCUMULATE — weighted-avg
    // entry basis, qty + entry_fee add (Portfolio_AccumulateSlotLeg). The overwrite-on-
    // every-leg died here (each partial leg used to re-open the slot, discarding prior legs).
    if ((oms->portfolio.active_bitmap >> (int)o->portfolio_slot) & 1u) {
        Portfolio_AccumulateSlotLeg(&oms->portfolio, (int)o->portfolio_slot,
                                    fill_price, fill_qty, entry_fee);
    } else {
        Portfolio_OpenSlot(&oms->portfolio, (int)o->portfolio_slot,
                           fill_price, fill_qty,
                           per_fill_tp, o->intended_sl, entry_fee);
    }
    oms->last_opened_mask |= (uint16_t)(1u << (int)o->portfolio_slot);
    // v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer dispatch (branchless).
    // Default = noop_fill_emit (no-op); set to real_on_entry_fill_emit at boot when trade_log Init succeeds.
    // Per DESIGN_SPECS/sink-fn-pointer-for-optional-side-effect-pattern.md.
    oms->on_entry_fill_emit(oms, o, fill_price, fill_qty, entry_fee, Money_Zero());
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[handle_buy_fill]
//======================================================================

//======================================================================
// [FUNCTION]_[handle_sell_fill]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING] [CRITICAL]]
// [REFERENCE]_[DECISION]_[[D-173] [D-190]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[exit fill — close slot, book net P&L into balance/realized, exit-side scratch arrays, calib + exit emits]
// [REFERENCE]_[INVARIANT]_[H20]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-154]
//======================================================================
// [CODE]
//======================================================================
// SELL handler — exit fill: close portfolio slot, compute P&L, update balance, write trade log.
template <unsigned F>
inline void handle_sell_fill(OrderManagerState<F>* oms, Order<F>* o, Money fill_price, Money fill_qty,
                             Money booked_fee) {
    const int pslot = (int)o->portfolio_slot;
    // v4.7.19 race guard — H20 exception #4 (genuine predicate; alternative requires Portfolio refactor).
    // __builtin_expect-rare: production fires only on rare hot-path-SG / manual-close race.
    if (__builtin_expect((oms->portfolio.active_bitmap & (uint16_t)(1u << pslot)) == 0, 0)) {
        std::fprintf(stderr,
            "[OMS] handle_sell_fill: slot %d SELL on already-closed slot — "
            "no-op (race between manual close and hot-path SG)\n", pslot);
        return;
    }
    const Money entry_price_snap = oms->portfolio.positions[pslot].entry_price;
    const Money pos_entry_fee    = oms->portfolio.positions[pslot].entry_fee;
    const Money pos_qty          = oms->portfolio.positions[pslot].quantity;   // remaining BEFORE this leg
    const Money tp_snap          = oms->portfolio.positions[pslot].take_profit_price;
    const Money sl_snap          = oms->portfolio.positions[pslot].stop_loss_price;

    // ── P3-d-ii (Class-46 CLOSE): close BY fill_qty — the completeness-assuming whole-close
    //    died here. Σlegs ≤ position is clamped LOUD (a replayed/duplicated leg cannot
    //    over-close — the MED-2 floor); a zero leg is a no-op.
    Money q_leg = fill_qty;
    if (__builtin_expect(Money_Gt(q_leg, pos_qty), 0)) {
        std::fprintf(stderr,
            "[OMS] handle_sell_fill: slot %d leg qty %.8f EXCEEDS remaining %.8f — CLAMPED "
            "(Σlegs ≤ position; duplicate/replayed leg or venue-feed fault — investigate)\n",
            pslot, Money_ToDouble(fill_qty), Money_ToDouble(pos_qty));
        q_leg = pos_qty;
    }
    if (__builtin_expect(Money_IsZero(q_leg), 0)) return;
    const uint8_t leg_flat = (uint8_t)(Money_Eq(q_leg, pos_qty) ? 1 : 0);

    // Entry-fee apportionment through the D-447 SSoT (pro-rata + residual-final-leg;
    // I-2 HIGH-3 conservation by construction) — decrements the Position's own tracker.
    const Money entry_fee_leg =
        Portfolio_ConsumeEntryFeeLeg(&oms->portfolio.positions[pslot].entry_fee,
                                     q_leg, pos_qty);

    // Per-leg realized return (feature-plane; same price formula — identical to HEAD at
    // full-close). The CLOSED mask is now SLOT-FLAT-gated: the per-TRADE ML tail
    // (ConfidenceScorer / pnl_feeder / ensemble / cooldown) fires at TRADE end — a partial
    // leg is not a concluded trade (the Class-46 predicate split, A-2 T3).
    const double entry_price_d   = Money_ToDouble(entry_price_snap);
    const double exit_price_d    = Money_ToDouble(fill_price);
    const bool   valid_entry     = entry_price_d > 0.0;
    const double computed_ret    = valid_entry ? (exit_price_d - entry_price_d) / entry_price_d : 0.0;
    oms->last_realized_return[pslot] = valid_entry ? computed_ret : oms->last_realized_return[pslot];
    const uint16_t closed_bit    = (uint16_t)(1u << pslot);
    oms->last_closed_mask        = (uint16_t)(oms->last_closed_mask |
                                              ((valid_entry && leg_flat) ? closed_bit : (uint16_t)0));

    const Money gross         = Portfolio_CloseSlotLeg(&oms->portfolio, pslot, fill_price, q_leg);
    const Money exit_fee      = booked_fee;   // Ship-B P3: resolved once in HandleFill (D-173 rule)
    OMS_GuardTakerBoundFeeBasis(o);   // dormant fee-desync guard (TECH_DEBT-154); never-taken while MARKET-only
    const Money total_fee     = Money_Add(entry_fee_leg, exit_fee);
    const Money net           = Money_Sub(gross, total_fee);
    // Trade accumulators for the per-TRADE ML tail (consumed + zeroed at flat by
    // DrainPostFill — the CLOSE-form derive is dead; the position is FLAT-form there).
    oms->last_trade_net[pslot]      = Money_Add(oms->last_trade_net[pslot], net);
    oms->last_trade_notional[pslot] = Money_Add(oms->last_trade_notional[pslot],
                                                Money_Mul(entry_price_snap, q_leg));
    // P3-b flip (D-444): the leaf's ONLY global-ledger effect is this emit — the composer
    // books {net -> balance/realized_pnl, per-fill peak ratchet, exit_fee -> the maker/taker
    // triple, the node row via the delta legs} through the ONE body, in DEFINED ring order.
    // fe.fee = the EXIT execution fee only; the entry fee rides inside `net` + entry_fee_leg
    // (A-1 T2 pin). notional = this leg's entry-basis relief; the composer's slot tracker
    // makes the FINAL leg's relief telescope exactly (zero ULP residue).
    {
        const int partial_on = BITMAP_IS_SET(oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED) ? 1 : 0;
        const tt::NodeIdx fill_node{(int16_t)BITMAP_SLOT_NODE(pslot, partial_on)};
        oms->fill_emit(oms, FillEvent_Make<F>(
            o->portfolio_slot, fill_node, /*is_sell=*/1,
            (uint8_t)(Order_GetState(o) == ORDER_FILLED), (uint8_t)(Order_GetIsMaker(o) != 0),
            leg_flat,
            ++oms->fill_emit_seq[fill_node],
            q_leg, fill_price, exit_fee, net,
            /*notional=*/Money_Mul(entry_price_snap, q_leg),
            entry_fee_leg));
    }

    // Exit-side scratch on OMS sibling arrays.
    oms->last_exit_fill_price[pslot] = fill_price;
    oms->last_exit_fee[pslot]        = exit_fee;

    // Branchless mask-select on last_was_win_bitmap (Pattern 3). P3-d-ii: written at FLAT
    // only, from the TRADE accumulator (the trade's total net, not this leg's) — the
    // per-TRADE consumers (cooldown sign, W/L display) want the concluded trade.
    const uint16_t win_bit       = BITMAP_BIT_U16(pslot);
    const uint16_t flat_sel      = (uint16_t)-(int16_t)leg_flat;
    const uint16_t was_win_mask  = (uint16_t)((Money_Gt(oms->last_trade_net[pslot], Money_Zero()) ? win_bit : (uint16_t)0) & flat_sel);
    oms->last_was_win_bitmap     = (uint16_t)((oms->last_was_win_bitmap & (uint16_t)~(win_bit & flat_sel)) | was_win_mask);

    // P3-f (TECH_DEBT-174 partial): bandit_reward_bps WIRED — the trade's realized net
    // bps (net/notional × 1e4), written at FLAT from the same accumulators the exit-bandit
    // reward consumes (single-source-of-semantics; the calib column was emitting a
    // never-written 0). Telemetry double math (display-only per H4). The algo-dispatch
    // reward-fn design (g_exit_reward_dispatch) stays TECH_DEBT-174 parked scope.
    {
        const double tnet_d  = Money_ToDouble(oms->last_trade_net[pslot]);
        const double tnotl_d = Money_ToDouble(oms->last_trade_notional[pslot]);
        const double bps     = (tnotl_d > 0.0) ? (tnet_d / tnotl_d) * 1e4 : 0.0;
        oms->bandit_reward_bps[pslot] = leg_flat ? bps : oms->bandit_reward_bps[pslot];
    }

    // v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer dispatch (branchless).
    // Default = noop_fill_emit; set to real_on_exit_calibration when calibration_log_file fopen() succeeds.
    // P3-d-ii: the calibration row is a per-TRADE artifact — emit at slot-flat only
    // (branchless pointer select; a partial leg is not a concluded trade).
    (leg_flat ? oms->on_exit_calibration : &noop_fill_emit<F>)(oms, o, fill_price, net, total_fee, q_leg);

    // v5.1.6 exit reason diagnostic — branchless ternary chain (replaces if-else-if; cmov on pointer).
    {
        const double entry_d = Money_ToDouble(entry_price_snap);
        const double exit_d  = Money_ToDouble(fill_price);
        const double tp_d    = Money_ToDouble(tp_snap);
        const double sl_d    = Money_ToDouble(sl_snap);
        const double gain    = entry_d > 0.0 ? (exit_d - entry_d) / entry_d : 0.0;
        const double net_d   = Money_ToDouble(net);
        const double fee_d   = Money_ToDouble(total_fee);
        // Branchless: chained ternaries → cmov on const char*.
        const bool is_tp     = (tp_d > 0.0) && (exit_d >= tp_d - 1e-6);
        const bool is_sl     = (sl_d > 0.0) && (exit_d <= sl_d + 1e-6);
        const char* reason   = is_tp ? "TP_HIT" : (is_sl ? "SL_HIT" : "INSIDE");
        std::fprintf(stderr,
            "[exit-diag] slot=%d strat=%u reason=%s entry=%.2f exit=%.2f "
            "tp=%.2f sl=%.2f gain=%+.4f%% gross=%+.4f fees=%.4f net=%+.4f\n",
            pslot, (unsigned)o->strategy_id, reason,
            entry_d, exit_d, tp_d, sl_d, gain * 100.0,
            Money_ToDouble(gross), fee_d, net_d);
    }

    // v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer dispatch (branchless).
    // Default = noop_fill_emit; set to real_on_exit_fill_emit at boot when trade_log Init succeeds.
    oms->on_exit_fill_emit(oms, o, fill_price, net, total_fee, q_leg);   // per LEG (venue-truth: one CSV row per execution)
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[handle_sell_fill]
//======================================================================

// Fn pointer table — indexed by OrderType (0=MARKET_BUY, 1=MARKET_SELL, 2=LIMIT_BUY, 3=LIMIT_SELL).
// 4 × 8B = 32B = 1 cache line; L1-hot on pinned drainer.
template <unsigned F>
using FillHandler = void (*)(OrderManagerState<F>*, Order<F>*, Money, Money, Money /*booked_fee*/);

template <unsigned F>
inline constexpr FillHandler<F> g_fill_dispatch[4] = {
    &handle_buy_fill<F>,    // ORDER_MARKET_BUY  = 0
    &handle_sell_fill<F>,   // ORDER_MARKET_SELL = 1
    &handle_buy_fill<F>,    // ORDER_LIMIT_BUY   = 2 (future)
    &handle_sell_fill<F>,   // ORDER_LIMIT_SELL  = 3 (future)
};

//======================================================================
// [FUNCTION]_[OMS_EventFunnelDrain]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [SUPPORTIVE]]
// [THREAD]_[[COMPOSER_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the composer's audit-event funnel drain (P3-c-ii / D-445): pop every per-node OrderEvent ring in DEFINED order (n ascending, FIFO within) -> OrderEventLog_Append — ids assigned in composer program order (deterministic; the replay fold is per-slot so cross-type interleave is oracle-neutral, A-2 T2). Called from the apply step tail — every apply site funnels automatically (M5)]
// [REFERENCE]_[DECISION]_[[D-445]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int OMS_EventFunnelDrain(OrderManagerState<F>* oms) {
    int drained = 0;
    for (int n = 0; n < MAX_EXECUTION_NODES; ++n) {
        const tt::NodeIdx nn{(int16_t)n};
        OrderEvent<F> ev;
        while (tt::SPSCRing_TryPop(&oms->oe_rings[nn], &ev)) {
            (void)OrderEventLog_Append(&oms->event_log, ev);
            ++drained;
        }
    }
    return drained;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OMS_EventFunnelDrain]
//======================================================================

//======================================================================
// [FUNCTION]_[OMS_EventFunnelPush]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [SUPPORTIVE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the audit-append producer side (P3-c-ii / D-445): route an OrderEvent onto the emitting node's funnel ring (slot < 0 = the -1 non-node sentinel, e.g. reconcile audits — routed to ring 0). NEVER-DROP (audit records): full ring under central topology = inline-drain-then-repush (the emitter IS the drain thread). Null-agg harness = direct Append — Append IS the body either way, the ring is transport only]
// [REFERENCE]_[DECISION]_[[D-445]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OMS_EventFunnelPush(OrderManagerState<F>* oms, const OrderEvent<F>& ev, int slot) {
    if (__builtin_expect(oms->agg == nullptr, 0)) {
        (void)OrderEventLog_Append(&oms->event_log, ev);
        return;
    }
    const int partial_on = BITMAP_IS_SET(oms->oms_state_flags,
                                         tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED) ? 1 : 0;
    const tt::NodeIdx nn{(int16_t)(slot >= 0 ? BITMAP_SLOT_NODE(slot, partial_on) : 0)};
    if (__builtin_expect(tt::SPSCRing_TryPush(&oms->oe_rings[nn], ev), 1)) return;
    (void)OMS_EventFunnelDrain(oms);   // never-drop: drain the prefix inline, then re-push
    (void)tt::SPSCRing_TryPush(&oms->oe_rings[nn], ev);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OMS_EventFunnelPush]
//======================================================================

//======================================================================
// [FUNCTION]_[OMS_CalibFunnelDrain]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [PERSISTENCE] [SUPPORTIVE]]
// [THREAD]_[[COMPOSER_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[P4-pre-2: the composer's calibration-row funnel drain — pop every per-node CalibRecord ring in DEFINED order (n ascending, FIFO within) and render each through CALIB_LOG_EMIT_ROW. Sole writer of calibration_log_file. Called from the apply tail beside OMS_EventFunnelDrain, so every apply site (live tail, live shutdown, backtest per-tick + finals) funnels automatically (M5)]
// [REFERENCE]_[DECISION]_[[D-445] [D-449]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int OMS_CalibFunnelDrain(OrderManagerState<F>* oms) {
    int drained = 0;
    for (int n = 0; n < MAX_EXECUTION_NODES; ++n) {
        const tt::NodeIdx nn{(int16_t)n};
        CalibRecord r;
        while (tt::SPSCRing_TryPop(&oms->calib_rings[nn], &r)) {
            CALIB_LOG_EMIT_ROW(oms->calibration_log_file, r);   // the ONE macro caller
            ++drained;
        }
    }
    return drained;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OMS_CalibFunnelDrain]
//======================================================================

//======================================================================
// [FUNCTION]_[OMS_CalibFunnelPush]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [PERSISTENCE] [SUPPORTIVE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[P4-pre-2: the calibration-row producer side — route a leaf-built CalibRecord onto the emitting node's funnel ring. DROP-with-counter on a full ring (telemetry, not conservation — see the calib_rings declaration for why never-drop is WRONG here under the flip). Null-agg harness = direct render, mirroring the audit funnel's direct-Append arm: the render IS the body either way, the ring is transport only]
// [REFERENCE]_[DECISION]_[[D-445] [D-449]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OMS_CalibFunnelPush(OrderManagerState<F>* oms, const CalibRecord& r, int slot) {
    if (__builtin_expect(oms->agg == nullptr, 0)) {
        CALIB_LOG_EMIT_ROW(oms->calibration_log_file, r);
        return;
    }
    const int partial_on = BITMAP_IS_SET(oms->oms_state_flags,
                                         tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED) ? 1 : 0;
    const tt::NodeIdx nn{(int16_t)(slot >= 0 ? BITMAP_SLOT_NODE(slot, partial_on) : 0)};
    if (__builtin_expect(tt::SPSCRing_TryPush(&oms->calib_rings[nn], r), 1)) return;
    // Full ring: DROP + count (never inline-drain — that would put a node thread on the
    // shared FILE* and on every node's ring). Loud once per occurrence; the counter is the
    // standing signal that the depth needs raising.
    ++oms->calib_rows_dropped;
    std::fprintf(stderr, "[calib] WARN: node %d funnel full — calibration row DROPPED "
                 "(total dropped %llu; raise CALIB_FUNNEL_RING_SIZE)\n",
                 (int)nn, (unsigned long long)oms->calib_rows_dropped);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OMS_CalibFunnelPush]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderManager_HandleFill]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING] [CRITICAL]]
// [REFERENCE]_[DECISION]_[D-173]
// [REFERENCE]_[INVARIANT]_[H20]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[fill entrypoint — fee booking rule SSoT (venue-USDT authoritative, computed fallback) + audit append + Pattern-1 dispatch]
// [REFERENCE]_[CLASS]_[28]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OrderManager_HandleFill(OrderManagerState<F>* oms, Order<F>* o,
                                     Money fill_price, Money fill_qty,
                                     double venue_commission = 0.0,
                                     const char* venue_commission_asset = nullptr) {
    // Bounds guard — H20 exception #4 (genuine predicate without alternative); __builtin_expect-rare.
    if (__builtin_expect((int)o->portfolio_slot < 0 || (int)o->portfolio_slot >= MAX_PORTFOLIO_POSITIONS, 0)) {
        std::fprintf(stderr,
                     "[OMS] fill handler: node_id %d out of range [0,%d), "
                     "skipping order %llu\n",
                     (int)o->portfolio_slot, MAX_PORTFOLIO_POSITIONS,
                     (unsigned long long)o->id);
        return;
    }
    // Pre-resolve bind discipline runtime warn (cost ~1 cycle in production).
    Order_WarnIfNotPreResolved(o, "OrderManager_HandleFill");
    // ── Ship-B P3 fee booking rule (D-173, SSoT) ──────────────────────────────────
    // Venue-reported commission is AUTHORITATIVE when it arrives in the quote asset
    // (the engine's books are USDT-quoted; multi-quote rides the .E.1 venue registry).
    // Anything else (BNB-paid, base-asset, absent) books the COMPUTED fee
    // (pre_resolved rate × notional) and warns — the operator-visible signal that
    // venue fees and engine books have diverged (the D-173 degrade arm).
    const Money computed_fee = Money_Mul(Money_Mul(fill_price, fill_qty),
                                         o->pre_resolved.fee_rate);
    Money booked_fee = computed_fee;
    if (venue_commission > 0.0 && venue_commission_asset) {
        if (std::strcmp(venue_commission_asset, "USDT") == 0) {
            booked_fee = Money{ money_from_double_payload(venue_commission) };
        } else {
            std::fprintf(stderr,
                "[OMS] fill %llu: venue commission %.8f %s != quote asset — booking "
                "COMPUTED fee (D-173 fallback; check BNB-burn / fee-asset config)\n",
                (unsigned long long)o->id, venue_commission, venue_commission_asset);
        }
    }
    // Audit log append — common across BUY/SELL/future LIMIT. The S-3 fee slot makes
    // the event log fee-self-contained from this epoch on. P3-c-ii: rides the FUNNEL
    // (the composer is the sole Append caller — D-445). P3-f (D-441): mode-gated HERE
    // now that booking runs in both modes — the mode bit is a LOGGING toggle only.
    if (MBS_EQ_U8(oms->oms_state_flags, tt::MASK_OMS_STATE_EVENT_LOG_MODE,
                  tt::SHIFT_OMS_STATE_EVENT_LOG_MODE, 1)) {
        OMS_EventFunnelPush(oms,
            OrderEvent_MakeFill<F>(
                o->id, o->submitted_at_us,
                Order_GetType(o), (int16_t)(int)o->portfolio_slot,   // OrderEvent.node_id stays a raw int16_t (persisted; loud boundary cast — .v is private since the CLAIM-1 close)
                fill_price, fill_qty,
                o->intended_tp, o->intended_sl,
                booked_fee,
                (uint8_t)(Order_GetState(o) == ORDER_FILLED)),   // P3-e: PARTIAL legs append as OEVT_PARTIAL_FILL
            (int)o->portfolio_slot);
    }
    // Pattern 1 1D dispatch — branchless via fn pointer table. Deterministic latency regardless
    // of BUY/SELL access pattern. Closes Class 28 first canonical.
    g_fill_dispatch<F>[(uint8_t)Order_GetType(o)](oms, o, fill_price, fill_qty, booked_fee);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Slim entrypoint: bounds guard (H20 exception #4) + pre-resolve discipline warn + audit log append
// + branchless dispatch via fn pointer table. All BUY/SELL-specific logic lives in handle_buy_fill /
// handle_sell_fill above. Future LIMIT_BUY/LIMIT_SELL types extend mechanically — 2 rows in g_fill_dispatch.
//======================================================================
// [END_FUNCTION]_[OrderManager_HandleFill]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderManager_ProcessFillCommand]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[unified REST/WS fill handler — O(1) encoded-slot lookup, dedup, state transition, mode-1 dispatch, slot free]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int OrderManager_ProcessFillCommand(OrderManagerState<F>* oms, const Command& cmd) {
    // WS surprise fill (order_id == 0): log and skip.
    if (cmd.order_id == 0 && cmd.type == (uint8_t)CMD_WS_FILL) {
        std::fprintf(stderr,
                     "[OMS] WS surprise fill (no clientOrderId), ignoring — "
                     "reconciliation will catch it\n");
        return 0;
    }

    // v5.11.5.B — O(1) slot lookup via encoded id.
    // Bits 63..60 of cmd.order_id carry the slot index assigned at submit
    // time. Decode + verify the slot still holds the expected order
    // (defends against late-arriving callbacks for an already-freed slot
    // that has been reused for a different order).
    int slot = OMS_OrderIdSlot(cmd.order_id);   // P4-pre-3a: lane decode SSoT
    if ((oms->order_bitmap & (uint16_t)(1u << slot)) == 0 ||
        oms->orders[slot].id != cmd.order_id) {
        slot = -1;  // slot freed, or reused for a different order
    }

    if (slot < 0) {
        // Order not found. For REST: slot freed already. For WS: REST beat us
        // (order already processed and freed). Either way, skip silently.
        if (cmd.type == (uint8_t)CMD_FILL_RESULT) {
            std::fprintf(stderr,
                         "[OMS] result for unknown order %llu (slot freed already?), ignoring\n",
                         (unsigned long long)cmd.order_id);
        }
        return 0;
    }

    Order<F>* o = &oms->orders[slot];

    // Dedup: if order is already FILLED (another source beat us), skip.
    if (Order_GetState(o) == ORDER_FILLED) return 0;

    if (cmd.result.success) {
        std::strncpy(o->exchange_id, cmd.result.exchange_id,
                     sizeof(o->exchange_id) - 1);
        o->exchange_id[sizeof(o->exchange_id) - 1] = '\0';

        // ACK-only results (from REST when WS is active): fill_qty == 0.
        // Transition to ACKNOWLEDGED, keep the slot open for the WS fill.
        // P3-e (D-446): a zero-fill TERMINAL result is NOT an ack — it falls through to
        // the terminal-incomplete disposition below. And a late ACK never DOWNGRADES a
        // PARTIAL back to ACKNOWLEDGED (the A-1 T3 state-regression guard).
        if (cmd.result.fill_qty == 0.0 && cmd.result.avg_fill_price == 0.0 &&
            !cmd.result.venue_terminal) {
            if (Order_GetState(o) != ORDER_PARTIAL) {
                Order_SetState(o, ORDER_ACKNOWLEDGED);
            }
            return 1;  // slot stays open — don't free
        }

        o->avg_fill_price = Money{ money_from_double_payload(cmd.result.avg_fill_price) };  // OrderResult ring bridge (scaled-i64 vehicle rides P3/S-8); last leg's avg (display/context)
        // P3-e (A-1 T3 pin): fill_qty is a PER-LEG INCREMENT (WS "l"; REST single-response
        // trivially so) — filled_qty is the RUNNING total. The TRIPWIRE: a qty-bearing
        // CMD_FILL_RESULT arriving when legs already accumulated = the REST-cumulative
        // regime leaking into a WS stream (structurally unreachable at HEAD topology;
        // E.1.4's GetStatus re-arm must hit THIS seam deliberately, never silently).
        if (__builtin_expect(cmd.type == (uint8_t)CMD_FILL_RESULT &&
                             !Money_IsZero(o->filled_qty) &&
                             cmd.result.fill_qty > 0.0, 0)) {
            std::fprintf(stderr,
                "[OMS] order %llu: qty-bearing REST result after WS legs (filled=%.8f, "
                "incoming=%.8f) — REFUSED as a leg (regime tripwire, D-446/A-1 T3; "
                "authoritative re-query is E.1.4's seam)\n",
                (unsigned long long)o->id, Money_ToDouble(o->filled_qty),
                cmd.result.fill_qty);
            return 0;
        }
        o->filled_qty = Money_Add(o->filled_qty,
                                  Money{ money_from_double_payload(cmd.result.fill_qty) });
        // Phase 8: maker/taker flag from Binance executionReport, parsed in c3.
        // Fee_Compute reads this for entry-fee math when the controller books
        // the fill. is_maker stays at Order_Init's 0 (taker) for synchronous
        // REST fills (Phase 02 path) — Binance market orders are taker by def.
        Order_SetIsMaker(o, (bool)cmd.result.is_maker);
        // Phase 8: pick FILLED vs PARTIAL based on Binance "X" field (parsed
        // in c3 as order_complete). For ACK-only paths above we already
        // returned with ORDER_ACKNOWLEDGED, so reaching here means an actual
        // fill (full or partial) happened. order_complete=0 = PARTIALLY_FILLED.
        // Defensive: if order_complete is missing from the event (parser sets
        // 0 in that case), we err toward PARTIAL — keeps the order alive in
        // the OMS, won't lose track. Subsequent fill events resolve to FILLED.
        Order_SetState(o, cmd.result.order_complete ? ORDER_FILLED : ORDER_PARTIAL);
        if (Order_GetState(o) == ORDER_FILLED) {
            oms->total_filled.fetch_add(1, std::memory_order_relaxed);
        }

        // Fill handler — BOTH modes since P3-f (D-441 unify): one booking pipeline;
        // the mode bit only gates the event-log append inside HandleFill. P3-e: the
        // LEG qty (the increment), not the running total — and a zero-qty result
        // (pure-expire terminal) books nothing.
        if (cmd.result.fill_qty > 0.0) {
            Money fill_price = o->avg_fill_price;
            Money fill_qty   = Money{ money_from_double_payload(cmd.result.fill_qty) };
            OrderManager_HandleFill(oms, o, fill_price, fill_qty,
                                     cmd.result.commission, cmd.result.commission_asset);
        }

        // ── P3-e (D-446): TERMINAL-INCOMPLETE disposition — the venue ended the order
        //    short of FILLED (EXPIRED/CANCELED after zero-or-partial execution). Booked
        //    legs STAND; the order dies terminal (the tail frees the slot); the audit
        //    row rides the funnel; a SELL's un-executed remainder RE-SUBMITS immediately
        //    (the venue killed our exit — we still want out; per-slot, via the normal
        //    submit path on THIS thread, which owns Submit under both topologies).
        if (cmd.result.venue_terminal && !cmd.result.order_complete) {
            Order_SetState(o, ORDER_CANCELED);
            const int tslot = (int)o->portfolio_slot;
            const Money remaining =
                ((oms->portfolio.active_bitmap & (uint16_t)(1u << tslot)) != 0)
                    ? oms->portfolio.positions[tslot].quantity : Money_Zero();
            // P3-close (a-class 1c): SNAPSHOT the dying order's fields, FREE its slot,
            // THEN re-submit — the remainder exit is guaranteed ≥1 free slot (the dying
            // order's own), so a FULL table can never silently drop it; the aliasing
            // question dies by construction (nothing reads `o` past the free — if the
            // re-submit reuses this slot, `o`'s storage now holds the NEW order, and
            // the terminal-tier tail free below no-ops on its non-terminal state).
            const uint64_t            dead_id     = o->id;
            const uint64_t            dead_ts     = o->submitted_at_us;
            const OrderType           dead_type   = Order_GetType(o);
            const Money               dead_filled = o->filled_qty;
            const Money               dead_tp     = o->intended_tp;
            const Money               dead_sl     = o->intended_sl;
            const uint8_t             dead_strat  = o->strategy_id;
            const Money               dead_avg    = o->avg_fill_price;
            const OrderPreResolved<F> dead_pre    = o->pre_resolved;
            oms->order_bitmap = (uint16_t)(oms->order_bitmap & ~(uint16_t)(1u << slot));
            if (MBS_EQ_U8(oms->oms_state_flags, tt::MASK_OMS_STATE_EVENT_LOG_MODE, tt::SHIFT_OMS_STATE_EVENT_LOG_MODE, 1)) {
                OrderEvent<F> tev{};
                tev.type         = OEVT_TERMINAL_INCOMPLETE;
                tev.order_id     = dead_id;
                tev.timestamp_us = dead_ts;
                tev.order_type   = dead_type;
                tev.node_id      = (int16_t)tslot;
                tev.qty          = dead_filled;   // what DID execute (running total)
                std::snprintf(tev.reason, sizeof(tev.reason), "term-incomplete rem=%.8f",
                              Money_ToDouble(remaining));
                OMS_EventFunnelPush(oms, tev, tslot);
            }
            std::fprintf(stderr,
                "[OMS] order %llu TERMINAL-INCOMPLETE node=%d executed=%.8f remaining=%.8f%s\n",
                (unsigned long long)dead_id, tslot, Money_ToDouble(dead_filled),
                Money_ToDouble(remaining),
                (dead_type == ORDER_MARKET_SELL && !Money_IsZero(remaining))
                    ? " — RE-SUBMITTING remainder exit" : "");
            if (dead_type == ORDER_MARKET_SELL && !Money_IsZero(remaining)) {
                SubmitCommand<F> re(tt::SlotIdx{(int16_t)tslot}, ORDER_MARKET_SELL, remaining,
                                    (uint8_t)(tslot & 1), /*node_cfg=*/nullptr);
                re.intended_tp = dead_tp;
                re.intended_sl = dead_sl;
                re.strategy_id = dead_strat;
                re.event_price = dead_avg;
                uint64_t rid = OrderManager_Submit(oms, re);
                // ≥1 slot free by construction (freed above) → Submit cannot return 0
                // here; carry the ORIGINAL order's pre-resolved binding forward from
                // the SNAPSHOT (node_cfg was nullptr; the Class-29 guard).
                int rslot = OMS_OrderIdSlot(rid);   // P4-pre-3a: lane decode SSoT
                oms->orders[rslot].pre_resolved = dead_pre;
            }
        }
    } else {
        std::fprintf(stderr,
                     "[OMS] order %llu FAIL node=%d code=%d msg=%s\n",
                     (unsigned long long)o->id,
                     (int)o->portfolio_slot,
                     cmd.result.error_code,
                     cmd.result.error_message);
        Order_SetState(o, ORDER_REJECTED);
        oms->total_rejected.fetch_add(1, std::memory_order_relaxed);

        // Mode 1: append rejection to event log for the audit trail. P3-c-ii: rides the
        // FUNNEL (this site moves to node threads at Phase 4 — the funnel keeps the
        // event-log async_ring single-appender, D-445 / GATE critic #1).
        if (MBS_EQ_U8(oms->oms_state_flags, tt::MASK_OMS_STATE_EVENT_LOG_MODE, tt::SHIFT_OMS_STATE_EVENT_LOG_MODE, 1)) {
            OMS_EventFunnelPush(oms,
                OrderEvent_MakeRejection<F>(
                    o->id, o->submitted_at_us,
                    Order_GetType(o), (int16_t)(int)o->portfolio_slot,   // raw int16_t on the persisted path (loud boundary cast)
                    cmd.result.error_message),
                (int)o->portfolio_slot);
        }
    }

    // P3-d-ii (Class-46, the ORDER-slot half): free ONLY on a TERMINAL state — a PARTIAL
    // leg keeps the order slot alive awaiting its remaining legs (the unconditional free
    // was the mask that hid the whole-close bug; Order_IsTerminal is the completeness
    // predicate here — FILLED/REJECTED terminal, PARTIAL/ACK working). Branchless mask.
    {
        const uint16_t term_sel = (uint16_t)-(int16_t)(Order_IsTerminal(o) ? 1 : 0);
        oms->order_bitmap &= (uint16_t)~((uint16_t)(1u << slot) & term_sel);
    }
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Looks up the order by id, applies the fill or rejection, runs the mode 1
// fill handler if applicable, frees the slot. Called from the unified drain
// loop in OrderManager_Tick for both CMD_FILL_RESULT and CMD_WS_FILL.
//
// Returns 1 if the command was processed, 0 if skipped (dedup, not found, etc.)
//======================================================================
// [END_FUNCTION]_[OrderManager_ProcessFillCommand]
//======================================================================

//======================================================================
// [FUNCTION]_[OMS_OpenPositionCost]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [DECIMAL]]
// [REFERENCE]_[DECISION]_[[D-216] [D-109]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[committed-cash SSoT — sum over open slots of entry notional + entry fee; shared by reconciler + .E.1 venue-net]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline Money OMS_OpenPositionCost(const OrderManagerState<F>* oms) {
    Money cost = Money_Zero();
    uint16_t bm = oms->portfolio.active_bitmap;
    while (bm) {
        int idx = __builtin_ctz(bm);
        const Position<F>& p = oms->portfolio.positions[idx];
        cost = Money_Add(cost, Money_Add(Money_Mul(p.entry_price, p.quantity), p.entry_fee));
        bm &= (uint16_t)(bm - 1);
    }
    return cost;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Sum over open slots of (entry_price*qty + entry_fee) = the cash that left the account
// to OPEN the held positions. oms->balance is FLAT-ACCOUNT value (start + sum realized; a
// BUY never debits it -- Portfolio_OpenSlot only), so to predict the venue's FREE cash you
// net this committed cost back out. Single-sourced (A21/D-216) so the reconciler and the
// .E.1 venue-net aggregator share ONE body (avoids a Class-43 parallel-derivation vs
// node_open_notional). entry_fee = the per-Position BOOKED fee (venue-exact in USDT, D-109).
//======================================================================
// [END_FUNCTION]_[OMS_OpenPositionCost]
//======================================================================

//======================================================================
// [FUNCTION]_[OMS_ExpectedFreeCash]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [DECIMAL]]
// [REFERENCE]_[DECISION]_[D-216]
// [REFERENCE]_[CLASS]_[38]
// [REFERENCE]_[INVARIANT]_[H4]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[cash-leg reconcile formula — ledger minus committed cost minus inflight-BUY reserve; pure + unit-testable]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline Money OMS_ExpectedFreeCash(const OrderManagerState<F>* oms) {
    Money expected = Money_Sub(oms->balance, OMS_OpenPositionCost(oms));
    uint16_t bm = oms->order_bitmap;
    for (int i = 0; i < MAX_INFLIGHT_ORDERS; ++i) {
        if (((bm >> i) & 1) == 0) continue;
        const Order<F>& o = oms->orders[i];
        OrderState ostate = Order_GetState(&o);
        // P3-e-ii: ORDER_PARTIAL joined the reserve set — before P3 a partial never
        // persisted (slot freed per event) so SUBMITTED|ACK was the complete working
        // set; now a partial BUY's un-executed remainder is real future cash outflow,
        // and `remain` is live (filled_qty accumulates since P3-e-i).
        if ((ostate == ORDER_SUBMITTED || ostate == ORDER_ACKNOWLEDGED ||
             ostate == ORDER_PARTIAL) &&
            Order_GetType(&o) == ORDER_MARKET_BUY) {
            Money remain   = Money_Sub(o.requested_qty, o.filled_qty);
            Money notional = Money_Mul(remain, o.event_price);
            Money est_fee  = Money_Mul(notional, o.pre_resolved.fee_rate);
            expected = Money_Sub(expected, Money_Add(notional, est_fee));
        }
    }
    return expected;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// expected_free_cash = balance - OMS_OpenPositionCost - Sum_inflight((requested-filled)*price + est_fee).
// Asset-agnostic STRUCTURE (cash = ledger - committed-cost - inflight-reserved); long-only cost-sign +
// single-currency settlement are crypto-spot-specific -- .E.6 equities extend with signed-qty + a margin
// term (D-216). All Money (H4). NOTE: o.filled_qty is the RUNNING total since P3-e-i (accumulated per
// leg at ProcessFillCommand) -> the -filled term is LIVE and LOAD-BEARING — the P3-e-ii EFC oracle
// (partial-BUY remainder reserve, 9900.6862-family char) witnesses it in both failure directions; the
// old Class-38 "INERT" note died with the A2/A16 landing. NOTE: inflight
// SELL is omitted -- cash-leg-correct; a mis-booked SELL is caught by the BTC/qty leg (.E.1 venue-net).
//======================================================================
// [END_FUNCTION]_[OMS_ExpectedFreeCash]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderManager_ProcessReconcile]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [LIVE_TRADING]]
// [REFERENCE]_[DECISION]_[D-216]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[reconcile command handler — DETECT-ONLY advisory alert + audit-trail append; NEVER mutates the ledger]
// [REFERENCE]_[INVARIANT]_[H9]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OrderManager_ProcessReconcile(OrderManagerState<F>* oms, const Command& cmd) {
    double drift = cmd.result.avg_fill_price;           // repurposed field
    double exchange_balance = cmd.result.fill_qty;      // repurposed field

    // A21 (D-216): DETECT-ONLY. The reconciler is ADVISORY (ReconciliationLoop.hpp:16);
    // it must NOT mutate the authoritative ledger. oms->balance is realized-derived
    // (balance == start + sum realized -- tested), whereas exchange_balance is venue
    // FREE-USDT; writing it back (a) breaks that invariant + boot-replay determinism
    // (H9 -- replay reconstructs start+sum-realized, not a venue number), and (b)
    // ratcheting ks_peak from a non-realized source mis-arms the kill-switch drawdown
    // denominator (Knight-adjacent). The drift was detected + logged by the Pass; the
    // authoritative venue-net correction (open-position value modeled) defers to .E.1.
    // So: alert, keep the audit trail below, write NOTHING.
    std::fprintf(stderr,
                 "[OMS] RECONCILE ALERT (advisory, detect-only -- ledger NOT modified): "
                 "drift=$%.4f venue_free_usdt=$%.4f active_bitmap=0x%04X; .E.1 venue-net owns the correction\n",
                 drift, exchange_balance, (unsigned)oms->portfolio.active_bitmap);

    if (MBS_EQ_U8(oms->oms_state_flags, tt::MASK_OMS_STATE_EVENT_LOG_MODE, tt::SHIFT_OMS_STATE_EVENT_LOG_MODE, 1)) {
        OrderEvent<F> recon_event;
        std::memset(&recon_event, 0, sizeof(recon_event));
        recon_event.type       = OEVT_RECONCILED;
        recon_event.order_type = ORDER_MARKET_BUY;  // placeholder
        recon_event.node_id    = -1;
        recon_event.price      = Money{ money_from_double_payload(drift) };
        std::strncpy(recon_event.reason, cmd.result.error_message,
                     sizeof(recon_event.reason) - 1);
        recon_event.reason[sizeof(recon_event.reason) - 1] = '\0';
        // P3-c-ii: rides the FUNNEL; -1 = the non-node sentinel (routed to ring 0).
        OMS_EventFunnelPush(oms, recon_event, /*slot=*/-1);
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OrderManager_ProcessReconcile]
//======================================================================

//======================================================================
// [FUNCTION]_[OMS_StaleInflightSweep]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the stale-inflight AGE detector, extracted so the LIVE drainer and the backtest Tick run the identical body (M5): an order still working OMS_STALE_INFLIGHT_WARN_US after submit is a transport gap. DETECT-ONLY and warned once per order — no state change, no slot free]
// [REFERENCE]_[DECISION]_[D-446]
//======================================================================
// [CODE]
//======================================================================
// EXTRACTED at 3b(ii) commit 4 leaf 4. It used to live only in OrderManager_Tick step 4 — which the
// LIVE engine never calls (the drainer runs DrainIntoBuckets + the ProcessBucket_* passes), so the
// detector existed but had no live caller: a diagnostic that could only ever fire in a backtest,
// which is the one place a transport gap cannot happen. Class 44's shape (a computed signal whose
// consumer is dead), and PARITY-068's residue.
//
// WHY NOT INSIDE DrainIntoBuckets: a pre-bucket sweep would warn-once on an order whose fill is
// sitting in THIS cycle's bucket, unprocessed — the warning would fire for an order that is about
// to complete normally, and because the warn is once-per-order that false positive is permanent.
// The live caller is therefore a named step in EngineSharded_Drainer_BookPass AFTER
// OrderManager_ProcessBucket_Reconciles, i.e. after this cycle's fills have actually been applied.
// (3b(iii) re-homes it into OMS_AccountRingsDrain when that lands.)
template <unsigned F>
inline void OMS_StaleInflightSweep(OrderManagerState<F>* oms) {
    // DETECT-ONLY, LOUD. Live only (paper synth results land same-cycle).
    //    Live only (paper synth results land same-cycle). An order still working
    //    OMS_STALE_INFLIGHT_WARN_US after submit = a transport gap (lost WS
    //    terminal report / REST response never arrived). No state change, no
    //    free: a guessed timeout while the venue actually filled books a phantom
    //    at reconcile. E.1.4's authoritative GetStatus re-query owns recovery
    //    (ORDER_TIMEOUT stays its landing pad). Warned ONCE per order (bit 26).
    //    Branch shape: empty-bitmap short-circuit = zero cost in the common
    //    no-inflight case; the per-order arms are rare-cold diagnostics
    //    (branchless-dispatch decision matrix, __builtin_expect-rare tier).
    if (oms->order_bitmap != 0 &&
        BITMAP_IS_SET(oms->oms_state_flags, tt::MASK_OMS_STATE_LIVE_TRADING)) {
        const uint64_t now_us = (uint64_t)
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        uint16_t bm = oms->order_bitmap;
        while (bm) {
            const int slot = __builtin_ctz(bm);
            bm = (uint16_t)(bm & (bm - 1));
            Order<F>* o = &oms->orders[slot];
            if (Order_IsTerminal(o) || Order_GetStaleWarned(o)) continue;
            if (__builtin_expect(now_us - o->submitted_at_us > OMS_STALE_INFLIGHT_WARN_US, 0)) {
                Order_SetStaleWarned(o, true);
                std::fprintf(stderr,
                    "[OMS] STALE-INFLIGHT order %llu node=%d state=%d age=%.1fs "
                    "filled=%.8f/%.8f — transport gap suspected (lost WS terminal or "
                    "REST response); detect-only, E.1.4 GetStatus owns recovery\n",
                    (unsigned long long)o->id, (int)o->portfolio_slot,
                    (int)Order_GetState(o),
                    (double)(now_us - o->submitted_at_us) / 1e6,
                    Money_ToDouble(o->filled_qty), Money_ToDouble(o->requested_qty));
            }
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OMS_StaleInflightSweep]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderManager_Tick]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CRITICAL]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-drain-pass command pump — drains REST/WS/reconcile SPSC rings through the unified dispatcher]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OrderManager_Tick(OrderManagerState<F>* oms) {
    // Drain all three command queues through the unified dispatcher, then run
    // the stale-inflight age sweep (step 4). Adding a new command source: add
    // one SPSCRing field, one drain call here, one handler function. No duplication.
    Command cmd;

    // 1. REST fills (adapter worker thread)
    while (OMS_ResultPop(oms, &cmd)) {   // P4-pre-3b: per-node rings, node-major interim drain
        if (cmd.type == (uint8_t)CMD_FILL_RESULT)
            OrderManager_ProcessFillCommand(oms, cmd);
    }

    // 2. WS fills (user data websocket thread) — per-node rings, node-major interim drain.
    //    The cursor advances across nodes and never revisits: node-major, drain-to-empty.
    int ws_cursor = 0;
    while (OMS_CmdRingsPop(&oms->ws_rings, &ws_cursor, &cmd)) {
        if (cmd.type == (uint8_t)CMD_WS_FILL)
            OrderManager_ProcessFillCommand(oms, cmd);
    }

    // 3. Reconciliation corrections (reconciler thread)
    while (SPSCRing_TryPop(&oms->reconcile_queue, &cmd)) {
        if (cmd.type == (uint8_t)CMD_RECONCILE)
            OrderManager_ProcessReconcile(oms, cmd);
    }

    // 4. The stale-inflight age sweep — the SAME helper the live drainer calls
    //    (OMS_StaleInflightSweep, above). Tick is the BACKTEST/test path; the live path reaches it
    //    from EngineSharded_Drainer_BookPass. One body, two callers, M5-identical by construction.
    OMS_StaleInflightSweep(oms);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Drainer thread calls this on every drain pass. Drains three SPSC rings
// sequentially (REST fills, WS fills, reconcile corrections) and dispatches
// each command to the appropriate handler. Adding a new command source is
// one new SPSCRing field + one drain call here + one handler function.
//======================================================================
// [END_FUNCTION]_[OrderManager_Tick]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderManager_Shutdown]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[final calib-funnel flush, then free the event log (stops its async writer thread) + close the calibration log; the RAII destructor's body]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OrderManager_Shutdown(OrderManagerState<F>* oms) {
    // P4-pre-2: flush any calib rows still in the funnel BEFORE the file closes. The live
    // shutdown path does reach a final apply (which drains) before OMS shutdown, but that is
    // a property of the CALLER's ordering — this makes tail-row survival a property of the
    // OWNER instead, for every current and future shutdown path. Single-threaded here BY
    // ORDERING since E.1.3 3b(ii) commit 3: EngineSharded_Run joins every OMS writer — the
    // order sources, then the venue producers (reconciler / REST workers / WS), then the
    // composer after its unconditional tail — BEFORE this call (until commit 3 the venue
    // producers were still live at this point; the claim was true only by luck). Idempotent
    // (empty rings = no-op) and nullptr-safe (the render is a no-op on a null FILE*).
    (void)OMS_CalibFunnelDrain(oms);
    OrderEventLog_Free(&oms->event_log);
    // v5.13.0.B — calibration log cleanup. nullptr-safe: most runs leave
    // it null (cfg.calibration_log_path empty by default).
    if (oms->calibration_log_file) {
        std::fclose(oms->calibration_log_file);
        oms->calibration_log_file = nullptr;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OrderManager_Shutdown]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderManager_OpenCalibrationLog]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[open the calibration CSV (v5.13.0.B) + wire the Pattern-5 calib sink to real; non-fatal on failure]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-10]
// [REFERENCE]_[DESIGN_SPEC]_[sink-fn-pointer-for-optional-side-effect-pattern.md]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int OrderManager_OpenCalibrationLog(OrderManagerState<F>* oms,
                                             const char* path) {
    if (!path || path[0] == '\0') return 0;  // disabled — not an error
    oms->calibration_log_file = std::fopen(path, "a");
    if (!oms->calibration_log_file) {
        std::fprintf(stderr,
            "[OMS] OpenCalibrationLog: fopen failed for '%s' — calibration "
            "logging disabled this session\n", path);
        return -1;
    }
    // v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer wire-to-real.
    // Per DESIGN_SPECS/sink-fn-pointer-for-optional-side-effect-pattern.md. Default = noop;
    // set-to-real when fopen succeeds → handle_sell_fill dispatches to calibration row emit.
    oms->on_exit_calibration = &tt::real_on_exit_calibration<F>;
    // Header row only if file is empty (new). Use ftell after open.
    std::fseek(oms->calibration_log_file, 0, SEEK_END);
    long sz = std::ftell(oms->calibration_log_file);
    if (sz == 0) {
        // v5.14.10.D — registry-driven header via FOREACH_CALIB_LOG_COL.
        // Byte-identical to prior hand-coded literal; closes TECH_DEBT-010.
        CalibLog_EmitHeader(oms->calibration_log_file);
        std::fflush(oms->calibration_log_file);
    }
    return 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Opens cfg.calibration_log_path in append mode. Writes a header row if
// the file is new (size == 0). Engine boot calls this AFTER OrderManager_Init
// when the cfg field is non-empty. Single-thread (boot); after this returns
// the FILE* is read-only on the drainer thread (sole writer in HandleFill).
//
// Returns 0 on success / -1 on failure (failure is non-fatal: log goes to
// stderr; engine continues without calibration logging this session).
//======================================================================
// [END_FUNCTION]_[OrderManager_OpenCalibrationLog]
//======================================================================

//----------------------------------------------------------------------
// [SECTION]_[introspection helpers — for tests and TUI]
//----------------------------------------------------------------------
template <unsigned F>
inline uint64_t OrderManager_TotalSubmitted(const OrderManagerState<F>* oms) {
    return oms->total_submitted.load(std::memory_order_relaxed);
}

template <unsigned F>
inline uint64_t OrderManager_TotalFilled(const OrderManagerState<F>* oms) {
    return oms->total_filled.load(std::memory_order_relaxed);
}

template <unsigned F>
inline uint64_t OrderManager_TotalRejected(const OrderManagerState<F>* oms) {
    return oms->total_rejected.load(std::memory_order_relaxed);
}

template <unsigned F>
inline int OrderManager_InflightCount(const OrderManagerState<F>* oms) {
    return __builtin_popcount((unsigned int)oms->order_bitmap);
}

}  // namespace tt
