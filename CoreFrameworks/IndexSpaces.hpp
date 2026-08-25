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
    int16_t v;
    explicit constexpr operator int() const { return (int)v; }
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
    int16_t v;
    explicit constexpr operator int() const { return (int)v; }
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

}  // namespace tt
