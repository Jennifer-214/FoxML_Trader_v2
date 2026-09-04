// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

#pragma once
//======================================================================================================
// [FILE]_[CoreFrameworks/NodeState.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the E.1.3 capital-plane types (D-439/D-440) — FillEvent (the normalized fill record AND the aggregation delta vehicle) + MoneySnapshot (the ONE published coherent money view) + the 3-tier kill word + NodeState/ClusterState/AggregatorState; VERSION-MANAGED growth per E.1.2 amendment 2: each phase replaces reserved words inside pinned cluster lines and re-pins deliberately]
// [REFERENCE]_[DECISION]_[[D-439] [D-440] [D-441] [D-34] [D-54] [D-193]]
// [REFERENCE]_[INVARIANT]_[[H6] [H12] [H14] [H22]]
// [REFERENCE]_[DESIGN_SPEC]_[[decision-first-cluster-layout-pattern] [cross-thread-snapshot-publish-cluster-isolation] [cross-thread-multiword-read-consistency-discipline] [multi-bit-state-encoding-pattern]]
// [CONTAINS]
//   - [STRUCT]_[FillEvent]
//   - [STRUCT]_[MoneySnapshotNodeRow]
//   - [STRUCT]_[MoneySnapshot]
//   - [MACRO]_[KILLWORD bit layout (SHIFT_/MASK_ per H14)]
//   - [STRUCT]_[NodeState]
//   - [STRUCT]_[ClusterState]
//   - [STRUCT]_[AggregatorState]
//======================================================================================================
// WHY THIS HEADER EXISTS (Phase-0/1 of the merged E.1.3 ship, D-439/D-440)
//
// The merged-design thesis (dive-v2): FillEvent is BOTH the fill-normalization artifact and the
// aggregation delta vehicle; every tier is single-writer. Nodes will (Phase 3+) process their own
// fills, single-write their OWN ledger rows, and emit FillEvents over per-node SPSC rings; the
// COMPOSER (the surviving central thread) applies those rings IN RING ORDER — giving well-defined
// global prefix sums, a per-fill-TRUE ks_peak ratchet, and replay determinism — then publishes
// MoneySnapshot (the ONE coherent money view every former torn-read site reads) and writes the
// 3-tier kill word (sole writer). No 16B atomics anywhere (removed per D-440).
//
// PHASE STATE (version-managed growth; re-pin the asserts in the SAME commit as any field):
//   Phase 0 ✅ shells + TD-299 typed substrate.
//   Phase 1 ▶ FillEvent + MoneySnapshot + kill-word layout + AggregatorState publish/apply state.
//            Production wiring is INTERIM-CENTRAL: the drainer composes the pack from its own
//            (same-thread, coherent) ledger; FillEvent rings + apply machinery are unit-exercised
//            and go production at Phase 3 (the leaf rework). Kill-word stays a DISPLAY COPY of
//            the legacy flags until Phase 2 unifies the writers.
//   Phase 2  kill unification + reader rewires. Phase 3 leaf rework (rings go live).
//   Phase 4  THE FLIP (per-node ownership). NodeState/ClusterState clusters fill along the way.
//
// ⚠ NAME-COLLISION GUARDS (read before assuming):
//   - `CoreFrameworks/EventLoopAggregates.hpp` is the FLOAT_DISPLAY_ONLY TUI adapter — NOT this
//     aggregator. (Phase 2 rewires it into the MoneySnapshot→display shim.)
//   - `MemHeaders/NodeStateFlagRegistry.hpp` / `PerNodeStateFlagsRegistry.hpp` are the NodeContext
//     FLAG vocabulary ("NodeState SHALT" prose) — a different, pre-existing surface.
//   - `NodeSlowState` (ControllerEventLoop.hpp) is the per-node DATA plane (rolling/regime/flow);
//     NodeState here is the per-node CAPITAL plane. Siblings, not duplicates.
//======================================================================================================
#include <cstdint>
#include <cstddef>
#include <atomic>

#include "../Limits.hpp"
#include "../FixedPoint/FixedPointN.hpp"
#include "IndexSpaces.hpp"
#include "ParameterSlot.hpp"
#include "SPSCRing.hpp"
#include "../MemHeaders/KillTripSiteRegistry.hpp"  // D-479 — the GLOBAL lanes of AggregatorState::kill_trip_request

//======================================================================================================
// [STRUCT]_[FillEvent]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING]]
// [THREAD]_[[NODE_SLOW_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the normalized fill record AND the per-node→composer delta vehicle (D-440 thesis) — in-flight SPSC ONLY, NEVER persisted (D-441 #3: no wire/H21 surface); one emitted per applied fill leg by the owning node's fill processing (Phase 3+)]
// [REFERENCE]_[DECISION]_[[D-440] [D-441]]
// [REFERENCE]_[INVARIANT]_[[H1] [H12]]
//======================================================================================================
// [CODE]
//======================================================================================================
// Layout note (H12 + cache): 8B identity head + 8B seq, then the seven 16B Money legs — first
// Money lands at offset 16 with NO hidden padding; sizeof pinned 128 (EXACTLY 2 cache lines per
// ring element — P3-d-i widened the delta vector with the node-row legs {notional,
// entry_fee_leg} + the slot_flat head bit). `venue_fee_reserved` is the E.1.4-slim A4 slot
// (D-441: additive venue-exact commission completion) — 0 until that ship; NOT a live field here.
// Depth 128 (P3-d-i; was 256): the fill + emit rings are DEPTH-PAIRED — the occupancy
// invariant (records push 1:1 after their FillEvent on EQUAL-size rings, so a successful fe
// push guarantees record space) is load-bearing for the lockstep trade-row pairing; unequal
// depths would silently drop rows at the smaller ring. 128/node/cycle is deep headroom
// (worst arrival 512/cycle engine-WIDE) and overflow is a CORRECT inline drain either way.
inline constexpr int FILL_EVENT_RING_SIZE = 128;   // per-node; PAIRED depth for fill_rings + emit_rings
template <unsigned F>
struct FillEvent {
    tt::SlotIdx slot;             // portfolio slot the fill leg applies to
    tt::NodeIdx node;             // owning node (derived at emit via the typed bridge)
    uint8_t     is_sell;          // 0 = BUY leg, 1 = SELL leg
    uint8_t     order_complete;   // venue completeness signal (A16/Class-46 terminal gating)
    uint8_t     is_maker = 0;     // maker/taker discriminator (D-444: the composer books the fee
                                  // triple via OrderManager_AccountMakerTakerFee — the ONE fee body;
                                  // MARKET-only today ⇒ always taker; carried for the counts pair +
                                  // future-LIMIT)
    uint8_t     slot_flat = 0;    // P3-d: 1 = this leg took the position to qty' == 0 (the H22-pure
                                  // LOCAL truth) — gates the composer's W/L classify + partner
                                  // pairing; DISTINCT from order_complete (the venue bit gates only
                                  // order-slot free — Class-46's predicate split, A-2 T3)
    uint64_t    seq;              // per-node emit sequence (apply-order pin + forensics)
    Money       qty;              // filled quantity (this leg)
    Money       price;            // fill price
    Money       fee;              // booked fee for this leg (maker/taker-resolved; SELL: the EXIT
                                  // execution fee only — entry fee rides net + entry_fee_leg)
    Money       net;              // signed realized delta (SELL: gross - total_fee; BUY: zero — buy books Δfee only, gate accounting F8)
    Money       notional;         // P3-d node-row vector: BUY = fill_price×qty (open_notional add);
                                  // SELL = entry_basis×qty (the SYMMETRIC entry-basis relief —
                                  // asymmetry leaks residue per round-trip)
    Money       entry_fee_leg;    // P3-d node-row vector (SELL legs): this leg's apportioned entry
                                  // fee — node_fees books entry+exit AT CLOSE (persisted semantics
                                  // preserved); BUY legs: zero
    Money       venue_fee_reserved;   // RESERVED for E.1.4-slim (venue-exact commission); always Money_Zero() this ship
};
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-28]
// [SIZE]_[128B]
// [ALIGN]_[16]
// [CACHE_LINES]_[2]
// [STRADDLE]_[none]
//======================================================================================================
// [END_STRUCT]_[FillEvent]
//======================================================================================================

static_assert(sizeof(FillEvent<64>) == 128, "FillEvent<64> pinned at 128B (8B id head + 8B seq + 7x16B Money = exactly 2 cache lines) — re-pin deliberately, never drift");
static_assert(alignof(FillEvent<64>) == 16, "FillEvent aligns to Money (16B)");
static_assert(offsetof(FillEvent<64>, seq) == 8  && offsetof(FillEvent<64>, qty) == 16,
              "FillEvent head packs fully (2+2+1+1+1+1 = 8B, no hidden padding — H12)");
static_assert(offsetof(FillEvent<64>, is_maker) == 6 && offsetof(FillEvent<64>, slot_flat) == 7,
              "is_maker + slot_flat ride the head's tail bytes (D-444 / P3-d) — offsets pinned");
static_assert(std::is_trivially_copyable<FillEvent<64>>::value, "FillEvent rides SPSC rings");

// D-444 + A-2 Q4: the ONE construction path — an emit site that misses a field compiles
// silently on the bare aggregate (garbage `seq` corrupts applied_seq forensics); the builder
// makes every live field a named parameter. venue_fee_reserved stays zero this ship (D-441 #3).
template <unsigned F>
inline FillEvent<F> FillEvent_Make(tt::SlotIdx slot, tt::NodeIdx node,
                                   uint8_t is_sell, uint8_t order_complete, uint8_t is_maker,
                                   uint8_t slot_flat, uint64_t seq,
                                   Money qty, Money price, Money fee, Money net,
                                   Money notional, Money entry_fee_leg) {
    FillEvent<F> fe{};
    fe.slot = slot; fe.node = node;
    fe.is_sell = is_sell; fe.order_complete = order_complete; fe.is_maker = is_maker;
    fe.slot_flat = slot_flat;
    fe.seq = seq; fe.qty = qty; fe.price = price; fe.fee = fee; fe.net = net;
    fe.notional = notional; fe.entry_fee_leg = entry_fee_leg;
    fe.venue_fee_reserved = Money_Zero();
    return fe;
}

//======================================================================================================
// [STRUCT]_[EmitRecord]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER]]
// [THREAD]_[[NODE_SLOW_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the trade-log TRAVEL record (plan amendment I / I-2 MED-4 / A-1 T5): the LEAF pre-builds the row payload at fill time (Position re-reads are unsafe at composer emit time — Phase-B overwrite + partial remainder-form), pushes it in LOCKSTEP with its FillEvent on the paired per-node ring; the COMPOSER pops fe -> applies -> pops this -> emits the row with the just-updated (post-apply) balance. Only pushed when trade_log is wired (live boot); in-flight SPSC only, never persisted]
// [REFERENCE]_[DECISION]_[[D-444] [D-445]]
// [REFERENCE]_[INVARIANT]_[[H1] [H12]]
//======================================================================================================
// [CODE]
//======================================================================================================
// Layout: five 16B Money legs + one 16B identity tail = 96B — the SAME ring geometry as
// FillEvent (2-per-3-lines; adjacent rings line-isolated by sizeof%64==0).
template <unsigned F>
struct EmitRecord {
    Money       fill_price;        // entry rows: the entry fill price; exit rows: the exit price
    Money       entry_price_snap;  // exit rows: position entry basis at leaf time (entry rows: == fill_price)
    Money       qty;               // this row's size (leaf-time snapshot)
    Money       net;               // exit rows: realized net; entry rows: Money_Zero()
    Money       fee;               // entry rows: entry fee; exit rows: total fee (entry + exit)
    uint64_t    timestamp_us;      // order submit timestamp (the TradeEvent synth field)
    tt::SlotIdx slot;
    uint8_t     strategy_id;
    uint8_t     is_entry;          // 1 = RecordEntry row, 0 = RecordExit row
    int32_t     _pad0 = 0;         // H12: explicit, zero-init
};

static_assert(sizeof(EmitRecord<64>) == 96,  "EmitRecord<64> pinned at 96B (5x16B Money + 16B tail) — re-pin deliberately");
static_assert(alignof(EmitRecord<64>) == 16, "EmitRecord aligns to Money (16B)");
static_assert(std::is_trivially_copyable<EmitRecord<64>>::value, "EmitRecord rides SPSC rings");
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-28]
// [SIZE]_[96B]
// [ALIGN]_[16]
// [CACHE_LINES]_[2]
// [STRADDLE]_[none]
//======================================================================================================
// [END_STRUCT]_[EmitRecord]
//======================================================================================================

//======================================================================================================
// [STRUCT]_[MoneySnapshotNodeRow]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one node's published money row — composer-written under the pack's seqlock; the rule (gate hft F3): every per-node money field that is persisted or GUI-read and becomes owner-written MUST appear here]
//======================================================================================================
// [CODE]
//======================================================================================================
struct MoneySnapshotNodeRow {
    Money alloc;         // allocated_balance
    Money realized;      // node_realized
    Money fees;          // node_fees
    Money peak;          // node_peak_balance (per-fill-true once the composer owns it — Phase 3)
    Money dd;            // node_dd_pct (recomputed at eval, never persisted — D-420)
    Money unrealized;    // node-computed per-position Money_FillGross sum on the node's OWN price (D-190)
};
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-27]
// [SIZE]_[96B]
// [ALIGN]_[16]
// [CACHE_LINES]_[2]
// [STRADDLE]_[none]
//======================================================================================================
// [END_STRUCT]_[MoneySnapshotNodeRow]
//======================================================================================================

static_assert(sizeof(MoneySnapshotNodeRow) == 96, "MoneySnapshotNodeRow pinned at 6x16B");

//======================================================================================================
// [STRUCT]_[MoneySnapshot]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING]]
// [THREAD]_[[COMPOSER_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[THE published coherent money view (cross-thread-multiword-read-consistency: every former torn-read site reads THIS via the house seqlock, never raw OMS state) — line-0 head {generation, kill-word display copy} so a mirror poll bails without touching the rows (gate dod F4)]
// [REFERENCE]_[DECISION]_[[D-34] [D-427] [D-440]]
// [REFERENCE]_[INVARIANT]_[[H6] [H12]]
//======================================================================================================
// [CODE]
//======================================================================================================
template <unsigned F>
struct alignas(64) MoneySnapshot {
    // ---- line 0 head: cheap-poll fields FIRST (decision-first layout, gate dod F4) ----
    uint64_t generation;          // composer generation (increments per publish; distinct from the
                                  // ParameterSlot's own seq bits — this one is CONTENT, readable post-copy)
    uint64_t kill_word_copy;      // DISPLAY copy of the standalone kill word (the SSoT is
                                  // AggregatorState::kill_word — readers needing authority read THAT atomic)
    // ---- global ledger view ----
    Money balance;
    Money realized;
    Money ks_peak;
    // ---- per-node rows (typed subscripts; composer-written whole under the seqlock —
    //      bulk-copy family: NO per-row isolation needed, single writer; gate dod F1 contrast) ----
    tt::NodeArray<MoneySnapshotNodeRow, MAX_EXECUTION_NODES> rows;
    // ---- tail extension area (P2-f+): global scalars added AFTER rows so head/rows offsets
    //      never move (boundary-stable growth — the same reason the census #3 fix could land
    //      without touching a single existing reader) ----
    Money expected_free = Money_Zero();  // OMS_ExpectedFreeCash at compose (balance − committed
                                         // open cost − inflight notional+fee). Census #3: the LIVE
                                         // reconciler reads THIS, never raw OMS state (detect-only
                                         // contract unchanged, D-216; correction path = E.1.5)
    // Global fee view (census #8 completion): drainer-owned Money totals published so the
    // TUI copy stops reading them raw cross-thread. The 3 scalars + expected_free fill the
    // tail line EXACTLY (4x16B = 64B) — no pad needed, sizeof stays 1664.
    Money total_fees       = Money_Zero();
    Money total_maker_fees = Money_Zero();
    Money total_taker_fees = Money_Zero();
};
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-27]
// [SIZE]_[1664B]
// [ALIGN]_[64]
// [CACHE_LINES]_[26]
// [STRADDLE]_[none]
//======================================================================================================
// [END_STRUCT]_[MoneySnapshot]
//======================================================================================================

static_assert(sizeof(MoneySnapshot<64>) == 1664, "MoneySnapshot<64> pinned: 64B head+global + 16x96B rows + 64B tail extension");
static_assert(offsetof(MoneySnapshot<64>, generation) == 0 && offsetof(MoneySnapshot<64>, rows) == 64,
              "line-0 head {generation, kill_word_copy} then rows at the next line (gate dod F4)");
static_assert(std::is_trivially_copyable<MoneySnapshot<64>>::value, "rides ParameterSlot");

//======================================================================================================
// [MACRO]_[KILLWORD bit layout]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING]]
// [REFERENCE]_[INVARIANT]_[H14]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the 3-tier kill word — ONE uint64 so a single relaxed atomic load reads ALL tiers at the same instant (the coherence property; recorded divergence from kill-switch-hierarchical-pattern's per-tier bytes, gate dod F2); SHIFT_/MASK_ constants per H14, paired overflow static_asserts per bitmap-overflow-protection]
//======================================================================================================
// Tier layout (append-only within the word; the word itself is IN-MEMORY ONLY — never persisted,
// never wire: persisted kill state remains the existing OMS/NodeContext flags, which SEED this
// word at boot [Phase 2, gate merge F3c]):
//   bit  0        GLOBAL kill tripped
//   bits 8..15    per-CLUSTER kill tripped  (cap TBD at E.1.5 — 8 bits reserved; static_assert
//                 lands with the cap. Single-cluster deployment today uses bit 8.)
//   bits 16..31   per-NODE kill tripped     (bit 16+n for node n)
#define KILLWORD_SHIFT_GLOBAL   0u
#define KILLWORD_MASK_GLOBAL    (1ull << KILLWORD_SHIFT_GLOBAL)
#define KILLWORD_SHIFT_CLUSTER  8u
#define KILLWORD_SHIFT_NODE     16u
#define KILLWORD_NODE_MASK(n)   (1ull << (KILLWORD_SHIFT_NODE + (unsigned)(n)))
static_assert(MAX_EXECUTION_NODES <= 16,
              "KILLWORD per-NODE tier holds 16 bits (16..31) — widen the layout DELIBERATELY (and "
              "re-pin) before raising MAX_EXECUTION_NODES past 16 (bitmap-overflow-protection)");

//======================================================================================================
// [STRUCT]_[DragCmd]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one GUI stop/TP drag request — producer packages, composer applies (P2-c; the raw cross-thread Position write is retired). Hot re-arm of live_tp/live_sl from the applied value lands at Phase 5 (TD-184's execution-effect half)]
//======================================================================================================
// [CODE]
//======================================================================================================
struct DragCmd {
    tt::SlotIdx slot;
    uint8_t     is_tp;          // 1 = take-profit drag, 0 = stop-loss drag
    uint8_t     _pad0[5] = {0,0,0,0,0};   // H12: explicit
    uint64_t    _pad1 = 0;      // H12: align price to 16
    Money       price;
};
static_assert(sizeof(DragCmd) == 32 && std::is_trivially_copyable<DragCmd>::value,
              "DragCmd pinned at 32B (8B id + 8B pad + 16B Money), SPSC-ridable");
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-27]
// [SIZE]_[32B]
// [ALIGN]_[16]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================================================
// [END_STRUCT]_[DragCmd]
//======================================================================================================

//======================================================================================================
// [STRUCT]_[NodeState]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN] [CAPITAL_BEARING]]
// [THREAD]_[[NODE_SLOW_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-node capital-plane ledger row — four alignas(64) cluster lines, single-written by the owning node's slow thread (H22); Phase-0 shell: reserved words, offsets pinned; fills at Phases 2-4]
//======================================================================================================
// [CODE]
//======================================================================================================
template <unsigned F>
struct alignas(64) NodeState {
    // ---- line 0 · cluster: slow_account (Phase 3/4 fill: the node-owned ledger fields whose
    //      published view is MoneySnapshotNodeRow) ----
    alignas(64) uint64_t _slow_account_reserved[8] = {};   // H12: explicit zero-init

    // ---- line 1 · cluster: drainer_state (Phase 4 fill: absorbed per-node drain locals —
    //      bucket cursors, tail-drain state, per-node H8 histogram handle) ----
    alignas(64) uint64_t _drainer_state_reserved[8] = {};

    // ---- line 2 · cluster: kill_mirror (Phase 2 fill: the node's READ-ONLY mirror of the
    //      composer's kill word + the per-node reset flag [owner zeroes its OWN rows — gate
    //      blindspot punch 3]; the node never writes shared kill state) ----
    alignas(64) uint64_t _kill_mirror_reserved[8] = {};

    // ---- line 3 · cluster: binding (Phase 3 fill: typed-bridge/cfg binding — resolved
    //      PerNodeCfg ref, FillEvent emit cursor, flatten flag [design row 9]) ----
    alignas(64) uint64_t _binding_reserved[8] = {};
};
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-27]
// [SIZE]_[256B]
// [ALIGN]_[64]
// [CACHE_LINES]_[4]
// [STRADDLE]_[none]
//======================================================================================================
// [END_STRUCT]_[NodeState]
//======================================================================================================

//======================================================================================================
// [STRUCT]_[ClusterState]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN] [CAPITAL_BEARING]]
// [THREAD]_[[COMPOSER_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-cluster shell — E.1.5 consumes (A34 governs on the slice); single-cluster deployment today, cluster CAP lands with E.1.5; Phase-0: one pinned line]
//======================================================================================================
// [CODE]
//======================================================================================================
template <unsigned F>
struct alignas(64) ClusterState {
    // ---- line 0 · cluster: slice (E.1.5 fill: the cluster's slice of the global aggregate +
    //      cluster kill-tier mirror; composer-written, E.1.5-read) ----
    alignas(64) uint64_t _slice_reserved[8] = {};   // H12: explicit zero-init
};
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-27]
// [SIZE]_[64B]
// [ALIGN]_[64]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================================================
// [END_STRUCT]_[ClusterState]
//======================================================================================================

//======================================================================================================
// [STRUCT]_[AggregatorState]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN] [CAPITAL_BEARING]]
// [THREAD]_[[COMPOSER_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the composer's state (sole writer = the central thread) — the standalone kill-word SSoT (its own line; readers relaxed-atomic-load it, gate merge F4), the per-node apply cursors, the reserved Phase-3 ledger line, and the MoneySnapshot publish port (house seqlock)]
// [REFERENCE]_[DECISION]_[[D-34] [D-54] [D-440] [D-441]]
// [REFERENCE]_[INVARIANT]_[[H3] [H6] [H22]]
//======================================================================================================
// [CODE]
//======================================================================================================
template <unsigned F>
struct alignas(64) AggregatorState {
    // ---- line 0 · the 3-tier kill word — the SSoT, on its OWN line (H6). Composer is the sole
    //      writer (Phase 2+); every other thread reads with a relaxed atomic load. The pack's
    //      kill_word_copy is the display twin, labeled as such (gate merge F4). Phase-1 interim:
    //      composed FROM the legacy flags each publish; writers unify at Phase 2. ----
    alignas(64) std::atomic<uint64_t> kill_word{0};
    uint64_t generation = 0;            // composer generation (mirrors into each published pack)
    // P2-a: kill-RESET requests — any thread (GUI/producer) fetch_or's node bits; ONLY the
    // composer consumes (exchange(0)) and applies (clear flag, zero peak/dd, re-arm drift latch).
    // The old producer-side reset body is retired (gate blindspot punch 3 / merge F3b).
    std::atomic<uint32_t> kill_reset_mask{0};
    // P2-d: owner-side save request — producer sets (paper periodic cadence); the composer
    // executes ShardedSnapshot_Save on ITS thread (it owns every field the save reads).
    // Paper-only I/O at ~1/13min — the drainer-budget exception is documented at the call.
    std::atomic<uint32_t> save_request{0};
    // P3-a (plan amendment H): full-ring inline-drain events — operator-visible telemetry for the
    // never-drop policy (the counter and the drain are COMPLEMENTS, not alternatives; Rule 2).
    // Single-writer: the emit path == the composer thread under central topology; Phase 4 re-homes
    // emit-side counting per-node with the parked-event cursor (NodeState binding line).
    uint64_t fill_ring_full_events = 0;
    // P3-c (D-445): paper-reset as a composer-executed COMMAND — the producer packages the GUI
    // request here (fetch-style store); the drainer/composer executes the WHOLE reset flow at its
    // cycle tail (everything pending drained + applied FIRST — pre-reset fills book to pre-reset
    // state). reset_seq = completion counter (composer-written; the producer copies it to the GUI's
    // paper_reset_seq — consumers stay GUI-agnostic per blindspot punch 9).
    std::atomic<uint32_t> reset_request{0};
    uint32_t reset_seq = 0;
    // D-479 as amended (gate #3 G3-1, 3b(ii) commit 1) — the kill-TRIP request word, the sibling
    // of kill_reset_mask above and the (L)(2) drift-trip->command vehicle landed early. Bits 0..15
    // = per-NODE trip lanes; bits 16..31 = GLOBAL trip lanes, one per FOREACH_KILL_TRIP_SITE row
    // (KillTripSiteRegistry.hpp). ANY thread fetch_or(release)s a lane (a venue producer that found
    // a fill ring full past its bounded push; a node's drift latch); ONLY the composer consumes
    // (exchange(0, acq_rel)) at compose step 0a and applies — node lanes replay the per-node trip
    // body, GLOBAL lanes are EventLoop_KillSwitchTrip's first live caller + the OEVT_RING_FULL_FATAL
    // marker row. Design row 2 holds: the composer stays the SOLE writer of the KILL bits and of
    // kill_word — this word is design row 3's "manual trip = INPUT". A GLOBAL lane is RESTART-ONLY
    // (D-481: the global kill bit has no runtime reset path, on purpose — an unbooked fill has
    // already corrupted the ledger every node reads). Cluster-external-capable by construction
    // (D-480): any holder of this struct's pointer can request; a cross-cluster mediator needs no
    // second word. On line 0 with the other request atomics — fatal-path-only writes, so the
    // fetch_or never contends with the hot threads' relaxed kill_word loads in steady state.
    std::atomic<uint32_t> kill_trip_request{0};
    // Composer-written: how many GLOBAL fatal lanes it has consumed (the OEVT_RING_FULL_FATAL
    // marker row's qty = this sequence, so the log can be read as "the Nth fatal"). Never reset.
    uint32_t kill_trip_fatal_seq = 0;
    uint64_t _pad_line0[2] = {};        // H12: explicit pad to the line boundary (24 B before; 16 B now)

    // ---- lines 1-2 · per-node FillEvent apply cursors (Phase 3 goes production; unit-exercised
    //      from Phase 1). applied_seq[n] = last FillEvent.seq applied from node n's ring —
    //      the defined ring-order apply state (determinism anchor). ----
    alignas(64) tt::NodeArray<uint64_t, MAX_EXECUTION_NODES> applied_seq = {};

    // D-444: the P1 `led_*` shadow ledger is RETIRED — the composer applies FillEvents into the
    // EXISTING `oms.{balance, realized_pnl, ks_peak_balance}` through EngineCommon_LedgerApplyFill
    // (ownership-by-TOPOLOGY: only the composer calls the apply; storage stays where restore/
    // reset/persist already live). In-memory only — offsets below re-pinned same commit.

    // ---- the publish port: the house seqlock, instantiated (TD-240 precedent — reuse outright,
    //      never a third seqlock). 1 writer (composer), N readers. ----
    tt::ParameterSlot<MoneySnapshot<F>> publish;

    // ---- per-node FillEvent rings (P1-c machinery; PRODUCTION at Phase 3 — the leaves emit,
    //      the composer applies IN RING ORDER n=0..N ascending, FIFO within each ring: the
    //      defined apply order that makes global prefix sums + the peak replay-deterministic).
    //      Single producer per ring (the owning node, Phase 3+; unit harnesses until then);
    //      single consumer (the composer). ----
    tt::NodeArray<tt::SPSCRing<FillEvent<F>, FILL_EVENT_RING_SIZE>, MAX_EXECUTION_NODES> fill_rings;

    // ---- P2-c: GUI-drag command ring — the producer's per-tick drag pickup PACKAGES the
    //      request (it no longer writes Position cross-thread: the census WRITE-hazard row
    //      closes); the composer applies on the thread that owns positions. True SPSC:
    //      producer=the GUI-pickup (producer thread), consumer=the composer. ----
    tt::SPSCRing<DragCmd, 8> drag_ring;

    // ---- P3-b: per-node trade-log EmitRecord rings — PAIRED with fill_rings in push order
    //      (leaf pushes fe THEN its record on the SAME node index; the composer pops in the
    //      same lockstep). Pushed ONLY when trade_log is wired (live boot); empty otherwise. ----
    tt::NodeArray<tt::SPSCRing<EmitRecord<F>, FILL_EVENT_RING_SIZE>, MAX_EXECUTION_NODES> emit_rings;

    // ---- P3-d-ii: composer-owned per-slot residual trackers (in-memory; single-writer =
    //      the composer). slot_notional makes the open-notional relief TELESCOPE exactly:
    //      BUY legs add fe.notional here + to the node row; non-final SELL legs relieve
    //      fe.notional; the FINAL leg (fe.slot_flat) relieves the tracked REMAINDER — Σ
    //      reliefs ≡ Σ adds by construction, zero ULP residue (the symmetric-subtraction
    //      discipline, exact). pending_trade_net accumulates SELL-leg nets per slot; the
    //      W/L classify + partner pairing fire ONCE at slot-flat on the TRADE total
    //      (I-2 MED-1 — per-leg booking would count one trade N times). ----
    tt::SlotArray<Money, MAX_PORTFOLIO_POSITIONS> slot_notional = {};
    tt::SlotArray<Money, MAX_PORTFOLIO_POSITIONS> pending_trade_net = {};

    // NOTE (paper/live partition — D-441 #4): modes are separate PROCESSES; mixed-mode totals
    // cannot occur. If a mixed-mode deployment ever exists, the partition hook is a second
    // ledger line + row set HERE, keyed by mode — never in the fill leaves.
};
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-28]
// [SIZE]_[467328B]
// [ALIGN]_[64]
// [CACHE_LINES]_[7302]
// [STRADDLE]_[none]
//======================================================================================================
// [END_STRUCT]_[AggregatorState]
//======================================================================================================

// Layout pins — growth is DELIBERATE (edit the assert in the same commit as the field).
static_assert(sizeof(NodeState<64>) == 256,      "NodeState<64> Phase-0 shell: 4 x 64B cluster lines");
static_assert(offsetof(NodeState<64>, _slow_account_reserved)  == 0,   "cluster line 0 anchor");
static_assert(offsetof(NodeState<64>, _drainer_state_reserved) == 64,  "cluster line 1 anchor");
static_assert(offsetof(NodeState<64>, _kill_mirror_reserved)   == 128, "cluster line 2 anchor");
static_assert(offsetof(NodeState<64>, _binding_reserved)       == 192, "cluster line 3 anchor");
static_assert(sizeof(ClusterState<64>) == 64,    "ClusterState<64> Phase-0 shell: 1 x 64B line");
static_assert(offsetof(AggregatorState<64>, kill_word)   == 0,   "kill word owns line 0 (SSoT, H6)");
static_assert(offsetof(AggregatorState<64>, kill_trip_request) + sizeof(uint32_t) <= 64 &&
              offsetof(AggregatorState<64>, kill_trip_fatal_seq) + sizeof(uint32_t) <= 64,
              "the kill-TRIP request word + its fatal sequence ride line 0 with the other request "
              "atomics (D-479 G3-1: zero growth — they replaced pad words)");
static_assert(offsetof(AggregatorState<64>, applied_seq) == 64,  "apply cursors at lines 1-2");
static_assert(offsetof(AggregatorState<64>, publish)     == 192, "publish port after the state lines (D-444: led_* line retired; re-pinned 256->192)");
static_assert(offsetof(AggregatorState<64>, fill_rings)  == 192 + sizeof(tt::ParameterSlot<MoneySnapshot<64>>),
              "fill rings follow the publish port");
static_assert(sizeof(AggregatorState<64>) == 192 + sizeof(tt::ParameterSlot<MoneySnapshot<64>>)
              + sizeof(tt::NodeArray<tt::SPSCRing<FillEvent<64>, FILL_EVENT_RING_SIZE>, MAX_EXECUTION_NODES>)
              + sizeof(tt::SPSCRing<DragCmd, 8>)
              + sizeof(tt::NodeArray<tt::SPSCRing<EmitRecord<64>, FILL_EVENT_RING_SIZE>, MAX_EXECUTION_NODES>)
              + 2 * sizeof(tt::SlotArray<Money, MAX_PORTFOLIO_POSITIONS>),
              "AggregatorState<64> = 3 state lines + publish port + fill rings + drag ring + emit rings + 2 per-slot residual trackers (re-pin deliberately)");
static_assert(alignof(NodeState<64>) == 64 && alignof(ClusterState<64>) == 64 &&
              alignof(AggregatorState<64>) == 64, "capital-plane types are cache-line aligned (H6)");
