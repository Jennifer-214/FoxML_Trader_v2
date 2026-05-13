// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [DRAINER CONSTANTS]  (v5.15.5.C.4 Phase T1 — drainer-thread-stable cache)
//======================================================================================================
//
// POD struct holding drainer-thread-stable cfg + state predicates.
// Initialized once per drainer cycle at the top of the main loop body (and
// inside each consumer lambda — drain_with_submit / drain_manual_closes /
// drain_post_fill) so all sites within one drain cycle see CONSISTENT
// predicate values.
//
// Replaces the prior ad-hoc pattern of scattered `BITMAP_IS_SET(...)` reads
// + repeated `FPN_ToDouble(cfg.fee_rate_taker)` conversions across multiple
// drainer cycle sites (6 sites for partial_on alone per cycle in the prior
// code).
//
// LAYOUT (per `function-struct-alignment-for-single-mov-access.md`):
//   - Size-descending field order (no middle padding waste)
//   - Total 24B → fits comfortably within 1 cache line
//   - 7B trailing pad for future field additions without size growth
//   - alignas(8) implicit from `double` field (sufficient for single-mov access)
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
// LATENCY: per CLAUDE.md item 17. Init cost: ~5-10 cycles per call (4
// field assignments + 1 BITMAP_IS_SET + 1 multiply). Cheaper than 6×
// scattered BITMAP_IS_SET reads (which the consumers replace). NET SAVINGS.
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"        // FPN<F>, FPN_ToDouble
#include "OmsStateFlagRegistry.hpp"              // MASK_OMS_STATE_PARTIAL_EXIT_ENABLED
#include "BitmapMacros.hpp"                      // BITMAP_IS_SET

#include <cstdint>

namespace tt {

//======================================================================================================
// POD struct holding drainer-thread-stable cfg + state predicates.
//
// Field order: size-descending (8B → 4B → 1B + trailing pad).
// Per `function-struct-alignment-for-single-mov-access.md` — naturally
// aligned; single-mov access for all fields when passed by const ref.
//======================================================================================================
struct DrainerConstants {
    double  fee_rate_taker_d;   // 8B, offset 0  — FPN_ToDouble(cfg.fee_rate_taker); boot-set immutable
    int     registered_count;   // 4B, offset 8  — state.registered_count
    int     drain_count;        // 4B, offset 12 — partial_on ? registered_count*2 : registered_count
    bool    partial_on;         // 1B, offset 16 — BITMAP_IS_SET(oms_state_flags, PARTIAL_EXIT_ENABLED)
    uint8_t _pad[7];            // 7B, offset 17-23 — trailing slack for future fields
};

// Verify layout assumptions hold (catches silent struct-layout regressions):
static_assert(sizeof(DrainerConstants) == 24, "DrainerConstants size changed; cache/single-mov analysis may be stale");
static_assert(alignof(DrainerConstants) == 8, "DrainerConstants alignment changed");

//======================================================================================================
// Initialize DrainerConstants from current state + cfg + oms.
//
// Call ONCE per drainer cycle at the entry of each consumer scope (main
// loop body, drain_with_submit lambda, drain_post_fill lambda, etc.).
// Cost: ~5-10 cycles (4 field assignments + 1 BITMAP_IS_SET + 1 multiply).
//
// All field values are SAFE TO CACHE FOR ONE CYCLE:
//   - fee_rate_taker_d: cfg field is boot-set immutable (per agent investigation
//     2026-05-13). Could be cached for entire drainer thread lifetime if desired;
//     per-cycle caching is simpler + still gives consistency-within-cycle.
//   - registered_count: boot-set; immutable post-Init.
//   - partial_on: cfg field; can mutate via operator GUI mid-session, but
//     per-cycle caching gives consistency-within-cycle (prior code re-read 6×
//     per cycle, accepting per-read drift as harmless; per-cycle caching is
//     strictly stricter discipline).
//   - drain_count: derived from partial_on + registered_count.
//======================================================================================================
template <unsigned F>
inline DrainerConstants DrainerConstants_Init(
    int registered_count,
    const ControllerConfig<F>& cfg,
    const OrderManagerState<F>& oms) {
    DrainerConstants dc;
    dc.fee_rate_taker_d = FPN_ToDouble(cfg.fee_rate_taker);
    dc.registered_count = registered_count;
    dc.partial_on       = BITMAP_IS_SET(oms.oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
    dc.drain_count      = dc.partial_on ? (dc.registered_count * 2) : dc.registered_count;
    return dc;
}

}  // namespace tt
