// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [OMS STATE FLAGS REGISTRY — v5.15.5.C.2 (S3a) + v5.15.5.C.3 (hybrid extension)]
//======================================================================================================
// Bit-packed boolean + K-state state for OrderManagerState (COLD cluster).
// Per CLAUDE.md item 20 (BITMAP_* universalization) + DESIGN_SPECS/bitmap-
// flag-api.md + DESIGN_SPECS/multi-bit-state-encoding-pattern.md, when 3+
// boolean flags coexist on the same struct, bit-pack them into a uint8_t/
// uint16_t/uint64_t bitmap rather than carrying byte-per-flag fields with
// their own alignment padding. K-state fields (K=2..16) co-exist in the
// SAME bitmap word via multi-bit slots.
//
// HYBRID PATTERN (v5.15.5.C.3 Phase 3b): single-bit flags + multi-bit slots
// share the uint8_t. Layout:
//   bits 0..2  — single-bit flags (FOREACH_OMS_STATE_FLAG, 3 entries today)
//   bits 3..4  — EVENT_LOG_MODE (FOREACH_OMS_STATE_MULTI_BIT, 2-bit slot, K=4 states)
//   bits 5..7  — RESERVED for future cfg-derived flags or multi-bit slots
//
// First codebase application of single-bit + multi-bit cohabitation in one
// bitmap word. Companion: OmsExitPredictorMetaRegistry.hpp uses a similar
// 3-slot layout but PER PORTFOLIO SLOT (uint8_t[16]) rather than struct-
// level — this header is the struct-level analog.
//
// CLOSES the COLD-cluster byte-per-flag pattern on OrderManagerState
// (live_trading int + partial_exit_enabled uint8 + _pad_pe[7] +
// kill_switch_tripped uint8 + _pad_ks[7] + event_log_mode int + _pad_elm[4]
// = ~24 bytes per OMS) → 1 uint8_t bitmap with 3 bits headroom (net savings
// ~19-23 bytes per OMS + branchless multi-flag / multi-state access).
//
// All flags + slots are single-thread (boot-set or paper-reset; never
// cross-thread mutated):
//   - LIVE_TRADING:         set ONCE at engine init from cfg.live_trading.
//   - PARTIAL_EXIT_ENABLED: set ONCE at engine init from cfg lifecycle
//                            flags; toggle requires snapshot v3 reload.
//   - KILL_SWITCH_TRIPPED:  set by per-OMS drawdown gate (drainer thread,
//                            single writer); cleared by EventLoop_Unpause
//                            (same thread on resume path).
//   - EVENT_LOG_MODE:       set ONCE at OrderManager_Init from
//                            cfg.oms_event_log_mode; read by drainer +
//                            backtest hot paths (single thread per OMS).
// No BITMAP_ATOMIC_* / MBS_ATOMIC_* needed; regular BITMAP_SET/CLR + MBS_SET/GET
// suffice.
//
// Wire-format note (CLAUDE.md item 15 — Parity-tested-by-construction):
// kill_switch_tripped is persisted in ShardedSnapshotPersist as int (4 bytes
// at a fixed offset). Save/load path read/write the BIT VALUE as int —
// wire format unchanged (no snapshot version bump required). EVENT_LOG_MODE
// is SKIP_PERSIST (cfg-derived; wire format unchanged across this addition).
//
// Adding a new single-bit COLD-cluster bool (1 row in FOREACH_OMS_STATE_FLAG):
//   1. Append X(NAME, "doc") to FOREACH_OMS_STATE_FLAG
//   2. Auto-generated OMS_STATE_FLAG_<NAME> bit position + MASK_OMS_STATE_<NAME>
//   3. Migrate writer sites: oms.NAME = 1 → OMS_STATE_FLAG_SET(oms, NAME)
//   4. Migrate reader sites: if (oms.NAME) → OMS_STATE_FLAG_IS_SET(oms, NAME)
//
// Adding a new K-state COLD-cluster slot (1 row in FOREACH_OMS_STATE_MULTI_BIT):
//   1. Append X(NAME, bits, shift, "doc") to FOREACH_OMS_STATE_MULTI_BIT
//   2. Pick SHIFT >= OMS_STATE_FLAG_COUNT (after single-bit flag region) +
//      after any prior multi-bit slots. static_assert below catches overlaps.
//   3. Auto-generated MASK_OMS_STATE_<NAME> + SHIFT_OMS_STATE_<NAME> + BITS_OMS_STATE_<NAME>
//   4. Migrate accessor sites: oms.NAME = val → MBS_SET_U8(oms.oms_state_flags, ...)
//      and `if (oms.NAME == val)` → `if (MBS_EQ_U8(oms.oms_state_flags, ..., val))`
//
// Cross-references:
//   CLAUDE.md item 20 (BITMAP_* API)
//   CLAUDE.md item 13 (X-macro registry)
//   CLAUDE.md item 1 (uint16_t Portfolio bitmap precedent)
//   CLAUDE.md item 15 (Parity-tested-by-construction — wire format preserved)
//   DESIGN_SPECS/bitmap-flag-api.md (7th application of bitmap-flag-api)
//   DESIGN_SPECS/multi-bit-state-encoding-pattern.md (2nd codebase application;
//     promotes pattern toward CLAUDE.md item — see CLAUDE.local.md "codify"
//     rule 2026-05-13)
//   DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md
//   v5.15.5.B.3 FOREACH_NODE_STATE_FLAG (NodeStateFlagRegistry.hpp) — sister registry
//   v5.15.5.C.2.1 FOREACH_OMS_META_SLOT (OmsExitPredictorMetaRegistry.hpp) —
//     1st multi-bit application (per-slot scope; this is struct-level analog)
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
    /* uses it for slot→node_id mapping (Sharded_LegSlot). Toggle requires snapshot v3 reload.      */ \
    X(PARTIAL_EXIT_ENABLED,                                                                             \
      "partials enabled: 0 = slot==node_id; 1 = slot = 2*node_id+leg (leg A/B per core)")               \
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

//======================================================================================================
// [MULTI-BIT SLOT REGISTRY — K-state fields co-located in oms_state_flags]
//======================================================================================================
// Sister registry to FOREACH_OMS_STATE_FLAG. Hosts K-state slots (K=2..16)
// packed into N-bit regions of the SAME oms_state_flags uint8_t. Per
// DESIGN_SPECS/multi-bit-state-encoding-pattern.md.
//
// Tuple: X(name, bits, shift, doc_string)
//   name       — UPPERCASE token; produces BITS/SHIFT/MASK_OMS_STATE_<name>
//   bits       — slot width (compile-time int constant; 1..8)
//   shift      — slot bit position (compile-time int constant; MUST be
//                 >= OMS_STATE_FLAG_COUNT to avoid overlap with single-bit
//                 region; MUST not overlap with prior multi-bit slots)
//   doc_string — human-readable description for audits + docs
//
// Slot positions are explicit (not auto-derived from preceding slot widths)
// for review-readability — operator sees the bit layout at a glance. Static-
// asserts below validate no overlap between single-bit region + multi-bit
// slots, and no overlap between multi-bit slots.
//======================================================================================================
#define FOREACH_OMS_STATE_MULTI_BIT(X)                                                              \
    /* OMS event-log mode. K=2-4 states:                                                          */ \
    /*   0 = legacy   — OMS_Tick marks orders FILLED/REJECTED; portfolio mutation in              */ \
    /*                   EventLoop_OnEvent (pre-v4.7.15 path; rarely used post-train-serve-parity */ \
    /*                   work, but retained for backward-compat with older test fixtures).        */ \
    /*   1 = event-log — OMS_Tick runs fill handler that opens/closes portfolio slots, updates    */ \
    /*                    balance, appends to event log. EventLoop_OnEvent just bumps counters.   */ \
    /*                    Live engine default; backtest uses this for train-serve parity.         */ \
    /*   2-3 = reserved for future modes (e.g., "live + replay" hybrid).                          */ \
    X(EVENT_LOG_MODE, 2, 3,                                                                          \
      "OMS event-log mode: 0=legacy, 1=event-log (default since v4.7.15 train-serve parity)")

//======================================================================================================
// [AUTO-GENERATED MULTI-BIT SLOT CONSTANTS]
//======================================================================================================
#define X_GEN_OMS_STATE_MULTI_BIT(name, bits, shift, doc)                                             \
    static constexpr uint8_t BITS_OMS_STATE_##name  = (uint8_t)(bits);                                \
    static constexpr uint8_t SHIFT_OMS_STATE_##name = (uint8_t)(shift);                               \
    static constexpr uint8_t MASK_OMS_STATE_##name  =                                                 \
        (uint8_t)(((1u << (bits)) - 1) << (shift));
FOREACH_OMS_STATE_MULTI_BIT(X_GEN_OMS_STATE_MULTI_BIT)
#undef X_GEN_OMS_STATE_MULTI_BIT

// Compile-time sanity:
//   - Multi-bit slots fit in uint8_t (highest used bit < 8).
//   - Multi-bit slots don't overlap with single-bit flag region (bits 0..OMS_STATE_FLAG_COUNT-1).
//   - Multi-bit slots don't overlap with each other.
//
// Single-bit flag region (bits 0..N-1 where N = OMS_STATE_FLAG_COUNT):
static constexpr uint8_t _OMS_STATE_SINGLE_BIT_REGION =
    (uint8_t)((1u << OMS_STATE_FLAG_COUNT) - 1u);

// EVENT_LOG_MODE-specific overlap check (extend with similar checks per added slot):
static_assert((MASK_OMS_STATE_EVENT_LOG_MODE & _OMS_STATE_SINGLE_BIT_REGION) == 0,
              "EVENT_LOG_MODE multi-bit slot overlaps single-bit flag region; "
              "increase SHIFT_OMS_STATE_EVENT_LOG_MODE or compact the single-bit cohort");
static_assert(SHIFT_OMS_STATE_EVENT_LOG_MODE + BITS_OMS_STATE_EVENT_LOG_MODE <= 8,
              "EVENT_LOG_MODE slot overflows uint8_t; widen oms_state_flags to uint16_t "
              "(and update the field type on OrderManagerState + BITMAP_BIT_U8 → BITMAP_BIT_U16 throughout)");

// EVENT_LOG_MODE value-capacity check (K <= slot width):
// EVENT_LOG_MODE has K=2 used today (legacy/event-log); K=4 supported by 2-bit slot.
// If a future contributor adds a 5th mode, this static_assert tells them to widen the slot.
enum OmsEventLogMode {
    OMS_EVENT_LOG_MODE_LEGACY    = 0,
    OMS_EVENT_LOG_MODE_EVENT_LOG = 1,
    // 2, 3 reserved for future modes
    OMS_EVENT_LOG_MODE_COUNT  // sentinel; must remain <= (1 << BITS_OMS_STATE_EVENT_LOG_MODE)
};
static_assert(OMS_EVENT_LOG_MODE_COUNT <= (1u << BITS_OMS_STATE_EVENT_LOG_MODE),
              "OMS_EVENT_LOG_MODE_COUNT exceeds EVENT_LOG_MODE slot capacity; "
              "widen BITS_OMS_STATE_EVENT_LOG_MODE in FOREACH_OMS_STATE_MULTI_BIT");

// Public count for multi-bit slots (uses >= per /readiness Check 21).
#define X_GEN_OMS_STATE_MULTI_BIT_COUNT_ONE(name, bits, shift, doc) +1
#define FOREACH_OMS_STATE_MULTI_BIT_COUNT \
    (0 FOREACH_OMS_STATE_MULTI_BIT(X_GEN_OMS_STATE_MULTI_BIT_COUNT_ONE))

}  // namespace tt

//======================================================================================================
// [OMS_STATE_FLAG_* convenience macros]
//======================================================================================================
// Mirror NODE_STATE_FLAG_* shape. Reads naturally:
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

//======================================================================================================
// [OMS_STATE_MULTI_BIT_* convenience macros — K-state slot accessors]
//======================================================================================================
// Sister set to OMS_STATE_FLAG_* but for K-state slots (FOREACH_OMS_STATE_MULTI_BIT).
// All slots are co-located in oms_state_flags uint8_t per the hybrid layout
// documented at the top of this header.
//
// Reads naturally:
//   Read:        int m = OMS_STATE_MULTI_BIT_GET(oms, EVENT_LOG_MODE);
//   Write:       OMS_STATE_MULTI_BIT_SET(oms, EVENT_LOG_MODE, 1);
//   Branchless equality:
//                if (OMS_STATE_MULTI_BIT_EQ(oms, EVENT_LOG_MODE, 1)) { ... }
//
// MASK + SHIFT auto-derived from registry; call sites stay short. Macro arg
// `oms` is a VALUE/REFERENCE (e.g., `oms` or `*state->oms`). For raw pointer
// call sites use the underlying MBS_* primitives directly with `state->oms->oms_state_flags`.
//======================================================================================================

#define OMS_STATE_MULTI_BIT_GET(oms, name) \
    MBS_GET_U8((oms).oms_state_flags, tt::MASK_OMS_STATE_##name, tt::SHIFT_OMS_STATE_##name)

#define OMS_STATE_MULTI_BIT_SET(oms, name, val) \
    MBS_SET_U8((oms).oms_state_flags, tt::MASK_OMS_STATE_##name, tt::SHIFT_OMS_STATE_##name, (val))

#define OMS_STATE_MULTI_BIT_EQ(oms, name, val) \
    MBS_EQ_U8((oms).oms_state_flags, tt::MASK_OMS_STATE_##name, tt::SHIFT_OMS_STATE_##name, (val))

#endif  // OMS_STATE_FLAG_REGISTRY_HPP
