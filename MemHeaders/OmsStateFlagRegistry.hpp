// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [OMS STATE FLAGS REGISTRY — v5.15.5.C.2 (S3a)]
//======================================================================================================
// Bit-packed boolean state for OrderManagerState (COLD cluster). Per
// CLAUDE.md item 20 (BITMAP_* universalization) + DESIGN_SPECS/bitmap-
// flag-api.md, when 3+ boolean flags coexist on the same struct,
// bit-pack them into a uint8_t/uint16_t/uint64_t bitmap rather than
// carrying byte-per-flag fields with their own alignment padding.
//
// CLOSES the COLD-cluster byte-per-flag pattern on OrderManagerState
// (live_trading int + partial_exit_enabled uint8 + _pad_pe[7] +
// kill_switch_tripped uint8 + _pad_ks[7] = 20 bytes per OMS) → 1 uint8_t
// bitmap with 5 bits headroom (net savings ~12-19 bytes per OMS + better
// branchless multi-flag check via BITMAP_ANY).
//
// All 3 flags are single-thread (boot-set or paper-reset; never
// cross-thread mutated):
//   - LIVE_TRADING:         set ONCE at engine init from cfg.live_trading.
//   - PARTIAL_EXIT_ENABLED: set ONCE at engine init from cfg lifecycle
//                            flags; toggle requires snapshot v3 reload.
//   - KILL_SWITCH_TRIPPED:  set by per-OMS drawdown gate (drainer thread,
//                            single writer); cleared by EventLoop_Unpause
//                            (same thread on resume path).
// No BITMAP_ATOMIC_* needed; regular BITMAP_SET/CLR suffices.
//
// Wire-format note (CLAUDE.md item 15 — Parity-tested-by-construction):
// kill_switch_tripped is persisted in ShardedSnapshotPersist as int (4 bytes
// at a fixed offset). Save/load path read/write the BIT VALUE as int —
// wire format unchanged (no snapshot version bump required).
//
// Adding a new COLD-cluster bool (1 row):
//   1. Append X(NAME, "doc") to FOREACH_OMS_STATE_FLAG
//   2. Auto-generated OMS_STATE_FLAG_<NAME> bit position + MASK_OMS_STATE_<NAME>
//   3. Migrate writer sites: oms.NAME = 1 → OMS_STATE_FLAG_SET(oms, NAME)
//   4. Migrate reader sites: if (oms.NAME) → OMS_STATE_FLAG_IS_SET(oms, NAME)
//
// Cross-references:
//   CLAUDE.md item 20 (BITMAP_* API)
//   CLAUDE.md item 13 (X-macro registry)
//   CLAUDE.md item 1 (uint16_t Portfolio bitmap precedent)
//   CLAUDE.md item 15 (Parity-tested-by-construction — wire format preserved)
//   DESIGN_SPECS/bitmap-flag-api.md (7th application of bitmap-flag-api)
//   DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md
//   v5.15.5.B.3 FOREACH_CORE_STATE_FLAG (CoreStateFlagRegistry.hpp) — sister registry
//======================================================================================================
#ifndef OMS_STATE_FLAG_REGISTRY_HPP
#define OMS_STATE_FLAG_REGISTRY_HPP

#include <stdint.h>
#include "BitmapMacros.hpp"  // BITMAP_* primitives (v5.14.8.A.0.b)

namespace tt {

//======================================================================================================
// [REGISTRY DEFINITION]
//======================================================================================================
// Tuple: X(name, doc_string)
//   name       — UPPERCASE token; produces OMS_STATE_FLAG_<name> bit position
//                + MASK_OMS_STATE_<name> uint8_t mask constant
//   doc_string — human-readable description for audits + docs
//
// 3 entries used; 5 bits headroom in uint8_t. Promote storage to uint16_t
// if/when a 9th entry needs adding (static_assert below catches overflow).
//======================================================================================================
#define FOREACH_OMS_STATE_FLAG(X)                                                                       \
    /* Live-trading mode flag. Set ONCE at engine init from cfg.live_trading. Gates Submit-time      */ \
    /* exchange-adapter dispatch (paper mode short-circuits; live mode calls adapter callbacks).     */ \
    X(LIVE_TRADING,                                                                                     \
      "live-trading mode: 0 = paper (adapter callbacks suppressed); 1 = live (adapter required)")       \
    /* Partials geometry mirrored from cfg.lifecycle_cfg_flags. Set ONCE at engine init; drainer     */ \
    /* uses it for slot→core_id mapping (Sharded_LegSlot). Toggle requires snapshot v3 reload.      */ \
    X(PARTIAL_EXIT_ENABLED,                                                                             \
      "partials enabled: 0 = slot==core_id; 1 = slot = 2*core_id+leg (leg A/B per core)")               \
    /* Kill switch trip state. Set by per-OMS drawdown gate (drainer thread); cleared by             */ \
    /* EventLoop_Unpause (same thread). Tripping clears every registered core's permission with      */ \
    /* RELEASE; idempotent. Persisted as int (4 bytes) in snapshot — wire format preserved.          */ \
    X(KILL_SWITCH_TRIPPED,                                                                              \
      "OMS-wide kill switch tripped; entries blocked until manual resume")

//======================================================================================================
// [AUTO-GENERATED BIT POSITIONS + MASK CONSTANTS]
//======================================================================================================
#define X_GEN_OMS_STATE_BIT(name, doc) OMS_STATE_FLAG_##name,
enum OmsStateFlag {
    FOREACH_OMS_STATE_FLAG(X_GEN_OMS_STATE_BIT)
    OMS_STATE_FLAG_COUNT  // sentinel
};
#undef X_GEN_OMS_STATE_BIT

#define X_GEN_OMS_STATE_MASK(name, doc) \
    static constexpr uint8_t MASK_OMS_STATE_##name = BITMAP_BIT_U8(OMS_STATE_FLAG_##name);
FOREACH_OMS_STATE_FLAG(X_GEN_OMS_STATE_MASK)
#undef X_GEN_OMS_STATE_MASK

static_assert(OMS_STATE_FLAG_COUNT <= 8,
              "oms_state_flags is uint8_t; max 8 entries. Widen to uint16_t "
              "if adding a 9th — and update the field type on OrderManagerState.");

// Public count for tests (uses >= per /readiness Check 21).
#define X_GEN_OMS_STATE_COUNT_ONE(name, doc) +1
#define FOREACH_OMS_STATE_FLAG_COUNT (0 FOREACH_OMS_STATE_FLAG(X_GEN_OMS_STATE_COUNT_ONE))

}  // namespace tt

//======================================================================================================
// [OMS_STATE_FLAG_* convenience macros]
//======================================================================================================
// Mirror CORE_STATE_FLAG_* shape. Reads naturally:
//   Set:    OMS_STATE_FLAG_SET(oms, KILL_SWITCH_TRIPPED)
//   Clear:  OMS_STATE_FLAG_CLR(oms, KILL_SWITCH_TRIPPED)
//   Read:   OMS_STATE_FLAG_IS_SET(oms, KILL_SWITCH_TRIPPED)
//   Toggle: OMS_STATE_FLAG_TOGGLE(oms, KILL_SWITCH_TRIPPED)
//   Any-of: BITMAP_ANY(oms.oms_state_flags, tt::MASK_OMS_STATE_X | tt::MASK_OMS_STATE_Y)
//
// Macro arg `oms` is a VALUE/REFERENCE (e.g., `oms` or `*state->oms`).
// For raw pointer call sites where dereference is awkward, use the underlying
// BITMAP_* primitives directly: `BITMAP_IS_SET(state->oms->oms_state_flags,
// tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED)`.
//
// MASK_OMS_STATE_<name> lives in tt:: namespace; macros assume the tt::
// scope is visible at call sites (most callers already `using namespace tt;`).
//======================================================================================================

#define OMS_STATE_FLAG_IS_SET(oms, name) BITMAP_IS_SET((oms).oms_state_flags, tt::MASK_OMS_STATE_##name)
#define OMS_STATE_FLAG_SET(oms, name)    BITMAP_SET((oms).oms_state_flags, tt::MASK_OMS_STATE_##name)
#define OMS_STATE_FLAG_CLR(oms, name)    BITMAP_CLR((oms).oms_state_flags, tt::MASK_OMS_STATE_##name)
#define OMS_STATE_FLAG_TOGGLE(oms, name) BITMAP_TOGGLE((oms).oms_state_flags, tt::MASK_OMS_STATE_##name)

#endif  // OMS_STATE_FLAG_REGISTRY_HPP
