// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

#pragma once
//======================================================================================================
// [FILE]_[CoreFrameworks/IndexSpaces.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the two engine index spaces as DISTINCT types — SlotIdx (portfolio slot) + NodeIdx (execution node) — so mixing them is a compile error rather than a silent 16==16 coincidence]
// [REFERENCE]_[DECISION]_[[D-294] [D-296] [D-437] [D-438]]
// [REFERENCE]_[INVARIANT]_[H22]
// [CONTAINS]
//   - [STRUCT]_[SlotIdx]
//   - [STRUCT]_[NodeIdx]
//======================================================================================================
// WHY THIS HEADER EXISTS
//
// The engine has TWO index spaces and they are not interchangeable:
//
//   SlotIdx — a portfolio slot, [0, MAX_PORTFOLIO_POSITIONS). Indexes
//             portfolio.positions[], active_bitmap bits, the OMS per-slot
//             sibling arrays, and submit_queues[].
//   NodeIdx — an execution node, [0, MAX_EXECUTION_NODES). Indexes
//             state.nodes[], cfg.nodes[], ezoo_refs[], node_cfg_refs[].
//
// Under partial_exit_enabled a node owns TWO slots (2N+0 / 2N+1), so the two
// spaces DIVERGE. Both caps are 16 today (Limits.hpp), which means a mix-up is
// invisible to every bounds check and to the compiler — and pre-commit Check O
// keys on an open-coded `>>`, so it cannot see `per_node_array[a_slot]` at all.
// Eight instances of that class were enumerated at E.1.2.F; two were live.
//
// WHY TYPES AND NOT A RENAME (the load-bearing history, D-438):
// E.1.1 was a ~100-file Core->Node hard rename across this exact surface whose
// completeness oracle was `rg "core_id" returns empty` — 0 residue, verified —
// and it left Order::node_id holding a slot under the comment "which executor
// core". A NAME SWEEP CANNOT SEE THAT A FIELD NAMED node_id CONTAINS A SLOT.
// Only the type system binds. Renaming is a consequence of this header, never
// a substitute for it.
//
// WHY int16_t AND NOT uint8_t:
// these wrap the EXISTING storage width so carriers stay byte-identical
// (Order measured 272 before and after) — no cache-layout re-bless, no size-pin
// move, no wire risk. The type buys ENFORCEMENT, not bytes. D-437 conflated the
// two and claimed a struct shrink that measured false.
//
// WHY explicit-ONLY conversion:
// `explicit operator int` is what makes `per_node_array[a_slot]` fail to
// compile. Every existing site that legitimately needs the integer already
// writes `(int)o->node_id` or `(uint16_t)o->node_id`, and a C-style cast routes
// through an explicit conversion operator unchanged — so the enforcement lands
// on the WRONG sites while the right ones keep building.
//
// THE HONEST CLAIM (calibrated at the CLAIM-1 close, 2026-08-26 — the post-ship
// a-class defeated the unqualified version with five compiling bypasses):
// a wrong-space index WITHOUT VISIBLE CAST CEREMONY is a compile error. The
// two `.v` backdoors are closed (all four types carry private storage), the
// bridges (Sharded_SlotNode / Sharded_LegSlot) are typed so the routine
// crossing never brace-constructs, and a deliberate cross-space rebuild must
// spell a VISIBLE cross-type cast — `tt::NodeIdx{(int)s}` at minimum — where
// the defeated idiom `tt::NodeIdx{s.v}` needed none. What the type system
// cannot stop is a boundary construction from a WRONG raw integer; those live
// only at declared loader/wire seams.
//
// NOT a wire type. Persisted structs keep raw integers; conversion happens at
// the loader/emit boundary (H9/H21 — E.1.3 owns the next snapshot epoch).
//======================================================================================================
#include <cstdint>

namespace tt {

//======================================================================================================
// [STRUCT]_[SlotIdx]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[a portfolio slot in [0, MAX_PORTFOLIO_POSITIONS) — trivially copyable, same size/align as the int16_t it replaces]
//======================================================================================================
// [CODE]
//======================================================================================================
struct SlotIdx {
    SlotIdx() = default;                                   // trivial; value-init zeroes, matching the old aggregate
    explicit constexpr SlotIdx(int16_t x) : v(x) {}        // explicit-only: no implicit construction. A cross-space
                                                           // rebuild must spell tt::NodeIdx{(int)s} — a VISIBLE
                                                           // cross-type cast (the a-class bypass NodeIdx{s.v} needed
                                                           // none). GCC demotes ctor-list narrowing to a warning, so
                                                           // narrowing is NOT the enforcement — private v is.
    explicit constexpr operator int() const { return (int)v; }
private:
    // PRIVATE since the CLAIM-1 close (2026-08-26). Public `v` was the enforcement hole the
    // post-ship a-class proved: `arr.v[s.v]` and `tt::NodeIdx{s.v}` both compiled — and with
    // the bridges returning bare int, the brace-construct was ALSO the routine legit idiom,
    // so correct and wrong were textually identical. With `v` private and the bridges typed
    // (Sharded_SlotNode / Sharded_LegSlot), the routine crossing is bridge-flow and the raw
    // escape is loud: (int16_t)(int)x at a declared boundary.
    int16_t v;
};
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-25]
//------------------------------------------------------------------------------------------------------
// [SIZE]_[2B]
// [ALIGN]_[2]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================================================
// [END_STRUCT]_[SlotIdx]
//======================================================================================================

//======================================================================================================
// [STRUCT]_[NodeIdx]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[an execution node in [0, MAX_EXECUTION_NODES) — trivially copyable, same size/align as the int16_t it replaces]
//======================================================================================================
// [CODE]
//======================================================================================================
struct NodeIdx {
    NodeIdx() = default;
    explicit constexpr NodeIdx(int16_t x) : v(x) {}
    explicit constexpr operator int() const { return (int)v; }
private:
    int16_t v;   // private — see the SlotIdx note (CLAIM-1 close)
};
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-25]
//------------------------------------------------------------------------------------------------------
// [SIZE]_[2B]
// [ALIGN]_[2]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================================================
// [END_STRUCT]_[NodeIdx]
//======================================================================================================

// Layout parity with the raw width they replace. If either fires, a carrier
// struct's layout moved and the "byte-identical, no re-bless" premise of D-438
// is broken — stop and re-measure rather than re-blessing a golden.
static_assert(sizeof(SlotIdx) == sizeof(int16_t), "SlotIdx must not change carrier layout");
static_assert(sizeof(NodeIdx) == sizeof(int16_t), "NodeIdx must not change carrier layout");
static_assert(alignof(SlotIdx) == alignof(int16_t), "SlotIdx must not change carrier alignment");
static_assert(alignof(NodeIdx) == alignof(int16_t), "NodeIdx must not change carrier alignment");

//======================================================================================================
// [STRUCT]_[NodeArray]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[a per-NODE array that accepts ONLY a NodeIdx subscript — the enforcement half of the type pair]
//======================================================================================================
// [CODE]
//======================================================================================================
// WHY A WRAPPER AND NOT A BARE ARRAY WITH TYPED CALLERS: a bare `T v[N]` accepts any integral
// subscript forever, so the discipline would live in the CALLER and be re-lost at the next edit —
// which is precisely how ezoo_refs[pslot] survived a 100-file rename. Binding the index space to
// the ARRAY makes the wrong subscript a compile error at every site that will ever exist.
//
// LAYOUT: aggregate containing exactly one T[N] member — same size, same alignment, same offsets
// as the raw array it replaces. Pinned below so a future member addition is a build error rather
// than a silently re-blessed cache-layout golden.
template <typename T, int N>
struct NodeArray {
    T&       operator[](NodeIdx i)       { return v[(int)i]; }
    const T& operator[](NodeIdx i) const { return v[(int)i]; }
private:
    // PRIVATE since the CLAIM-1 close: the public payload was bypass #1 (`arr.v[i]` reopened
    // untyped subscripting wholesale). Whole-array walks index with NodeIdx in a loop.
    T v[N];
};
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-25]
//------------------------------------------------------------------------------------------------------
// [SIZE]_[N*sizeof(T)]
// [ALIGN]_[alignof(T)]
// [CACHE_LINES]_[varies]
// [STRADDLE]_[none]
//======================================================================================================
// [END_STRUCT]_[NodeArray]
//======================================================================================================

//======================================================================================================
// [STRUCT]_[SlotArray]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[a per-SLOT array that accepts ONLY a SlotIdx subscript — the mirror of NodeArray]
//======================================================================================================
// [CODE]
//======================================================================================================
template <typename T, int N>
struct SlotArray {
    T&       operator[](SlotIdx i)       { return v[(int)i]; }
    const T& operator[](SlotIdx i) const { return v[(int)i]; }
private:
    T v[N];   // private — see the NodeArray note (CLAIM-1 close)
};
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-25]
//------------------------------------------------------------------------------------------------------
// [SIZE]_[N*sizeof(T)]
// [ALIGN]_[alignof(T)]
// [CACHE_LINES]_[varies]
// [STRADDLE]_[none]
//======================================================================================================
// [END_STRUCT]_[SlotArray]
//======================================================================================================

// Layout parity with the raw arrays they replace — if either fires, a wrapper grew a member and
// every carrier's offsets moved. Build error beats a re-blessed golden.
static_assert(sizeof(NodeArray<void*, 16>) == sizeof(void* [16]), "NodeArray must not change layout");
static_assert(sizeof(SlotArray<void*, 16>) == sizeof(void* [16]), "SlotArray must not change layout");
static_assert(alignof(NodeArray<void*, 16>) == alignof(void* [16]), "NodeArray must not change alignment");

}  // namespace tt
