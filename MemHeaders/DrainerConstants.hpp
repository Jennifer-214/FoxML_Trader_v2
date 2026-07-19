// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/DrainerConstants.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the per-cycle drainer-thread-stable predicate cache — one Init per drain cycle replaces 6 scattered BITMAP_IS_SET reads with consistency-within-cycle]
// [CONTAINS]
//   - [STRUCT]_[DrainerConstants]
//   - [FUNCTION]_[DrainerConstants_Init]
// [REFERENCE]_[DESIGN_SPEC]_[function-struct-alignment-for-single-mov-access]
//======================================================================================================
// POD struct holding drainer-thread-stable cfg + state predicates.
// Initialized once per drainer cycle at the top of the main loop body (and
// inside each consumer lambda — drain_with_submit / drain_manual_closes /
// drain_post_fill) so all sites within one drain cycle see CONSISTENT
// predicate values.
//
// Replaces the prior ad-hoc pattern of scattered `BITMAP_IS_SET(...)` reads
// (6 sites for partial_on alone per cycle in the prior code).
//
// v5.15.5.F.4d.1.B.8 — `fee_rate_taker_d` field DELETED (Class 27 vestigial
// post-`.F.4c.3` WIP2d-1.B.1 cache deletion; UNREAD by production code;
// per-core fee_rate captured at decision-time on Order.pre_resolved.fee_rate).
//
// LAYOUT (per `function-struct-alignment-for-single-mov-access.md`):
//   - Size-descending field order (no middle padding waste)
//   - Total 16B → fits comfortably within 1 cache line
//   - 7B trailing pad for future field additions without size growth
//   - alignof(4) implicit from `int` field (sufficient for single-mov access)
//   - No `alignas(64)` — struct is drainer-thread-local; not cross-thread; not
//     in hot-inner-loop; per-cycle init cost is amortized across all internal
//     sites that use the cached values
//
// FUTURE EXTENSIONS:
// New drainer-thread-stable cfg / state values get added here as fields.
// Adding the next predicate = 1 row in struct + 1 line in Init function.
// Consumer sites reference the field; no scattered BITMAP_IS_SET reads.
//
// FOREACH-REGISTRY MIGRATION CONSIDERED:
// At v5.15.5.C.4, the struct has 4 fields with ordered initialization
// (drain_count depends on partial_on). Plain POD struct + Init template is
// simpler than X-macro registry for this small + ordered shape. If the
// struct grows past 6-8 fields with parallel-init-safe semantics, migrate
// to FOREACH_DRAINER_CONSTANT registry per
// `DESIGN_SPECS/heterogeneous-registry-pattern.md` DOMAIN SPLIT form.
//
// LATENCY: Init cost: ~5-10 cycles per call (4
// field assignments + 1 BITMAP_IS_SET + 1 multiply). Cheaper than 6×
// scattered BITMAP_IS_SET reads (which the consumers replace). NET SAVINGS.
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"        // FPN_Binary<F>, FPN_ToDouble
#include "OmsStateFlagRegistry.hpp"              // MASK_OMS_STATE_PARTIAL_EXIT_ENABLED
#include "BitmapMacros.hpp"                      // BITMAP_IS_SET

#include <cstdint>

namespace tt {

//======================================================================
// [STRUCT]_[DrainerConstants]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[16B size-descending POD — registered_count + derived drain_count + partial_on, 7B trailing slack; drainer-thread-local (deliberately NO alignas(64))]
//======================================================================
// [CODE]
//======================================================================
struct DrainerConstants {
    // v5.15.5.F.4d.1.B.8 — fee_rate_taker_d field DELETED (Class 27 vestigial sub-instance closure;
    // UNREAD post-.F.4c.3 WIP2d-1.B.1 cache deletion; verified 0 production consumers via grep + /trace-deps).
    // Per-core fee_rate is captured at decision-time on Order.pre_resolved.fee_rate (Class 27 closure pattern).
    int     registered_count;   // 4B, offset 0  — state.registered_count
    int     drain_count;        // 4B, offset 4  — partial_on ? registered_count*2 : registered_count
    bool    partial_on;         // 1B, offset 8  — BITMAP_IS_SET(oms_state_flags, PARTIAL_EXIT_ENABLED)
    uint8_t _pad[7];            // 7B, offset 9-15 — trailing slack for future fields
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Field order: size-descending (8B → 4B → 1B + trailing pad).
// Per `function-struct-alignment-for-single-mov-access.md` — naturally
// aligned; single-mov access for all fields when passed by const ref.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[16B]
// [ALIGN]_[4]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[DrainerConstants]
//======================================================================
// Verify layout assumptions hold (catches silent struct-layout regressions).
// v5.15.5.F.4d.1.B.8: size 24→16 + alignof 8→4 post fee_rate_taker_d deletion (Class 27 vestigial).
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(DrainerConstants) == 16]
static_assert(sizeof(DrainerConstants) == 16, "DrainerConstants size changed; cache/single-mov analysis may be stale");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(DrainerConstants) == 4]
static_assert(alignof(DrainerConstants) == 4, "DrainerConstants alignment changed");

//======================================================================
// [FUNCTION]_[DrainerConstants_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[build the per-cycle cache — call ONCE per drainer cycle at each consumer scope entry; ~5-10 cycles]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline DrainerConstants DrainerConstants_Init(
    int registered_count,
    const ControllerConfig<F>& /*cfg*/,
    const OrderManagerState<F>& oms) {
    DrainerConstants dc;
    dc.registered_count = registered_count;
    dc.partial_on       = BITMAP_IS_SET(oms.oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
    dc.drain_count      = dc.partial_on ? (dc.registered_count * 2) : dc.registered_count;
    return dc;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Call ONCE per drainer cycle at the entry of each consumer scope (main
// loop body, drain_with_submit lambda, drain_post_fill lambda, etc.).
// Cost: ~5-10 cycles (4 field assignments + 1 BITMAP_IS_SET + 1 multiply).
//
// All field values are SAFE TO CACHE FOR ONE CYCLE:
//   - registered_count: boot-set; immutable post-Init.
//   - partial_on: cfg field; can mutate via operator GUI mid-session, but
//     per-cycle caching gives consistency-within-cycle (prior code re-read 6×
//     per cycle, accepting per-read drift as harmless; per-cycle caching is
//     strictly stricter discipline).
//   - drain_count: derived from partial_on + registered_count.
//
// v5.15.5.F.4d.1.B.8 — `cfg` parameter retained for future drainer-thread-stable
// cfg fields; currently unused after fee_rate_taker_d deletion.
//======================================================================
// [END_FUNCTION]_[DrainerConstants_Init]
//======================================================================

}  // namespace tt
