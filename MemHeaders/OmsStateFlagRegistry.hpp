// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/OmsStateFlagRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BITMAP_PACKED] [OMS_DRAINER] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[OrderManagerState COLD-cluster uint8_t oms_state_flags SSoT — HYBRID word: single-bit flags at explicit bits (0..2, 5) + the 2-bit EVENT_LOG_MODE slot (bits 3..4); bits 6..7 free]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_OMS_STATE_FLAG]        (auto-gen bits/masks + overflow assert + count ride)
//   - [REGISTRY]_[FOREACH_OMS_STATE_MULTI_BIT]   (slot constants + OmsEventLogMode enum + hybrid overlap/capacity asserts ride)
//   - [MACRO]_[OMS_STATE_FLAG_*]
//   - [MACRO]_[OMS_STATE_MULTI_BIT_*]
// [REFERENCE]_[DESIGN_SPEC]_[multi-bit-state-encoding-pattern]
// [REFERENCE]_[INVARIANT]_[[H14] [H9]]
//======================================================================================================
// Bit-packed boolean + K-state state for OrderManagerState (COLD cluster).
// Per the BITMAP_* universalization discipline + DESIGN_SPECS/bitmap-
// flag-api.md + DESIGN_SPECS/multi-bit-state-encoding-pattern.md, when 3+
// boolean flags coexist on the same struct, bit-pack them into a uint8_t/
// uint16_t/uint64_t bitmap rather than carrying byte-per-flag fields with
// their own alignment padding. K-state fields (K=2..16) co-exist in the
// SAME bitmap word via multi-bit slots.
//
// HYBRID PATTERN (v5.15.5.C.3 Phase 3b): single-bit flags + multi-bit slots
// share the uint8_t. Every position is EXPLICIT in its registry row (D-526) and
// the layout assert below is generated over BOTH registries. Layout:
//   bits 0..2  — single-bit flags (FOREACH_OMS_STATE_FLAG)
//   bits 3..4  — EVENT_LOG_MODE (FOREACH_OMS_STATE_MULTI_BIT, 2-bit slot, K=4 states)
//   bit  5     — PAPER_PERSIST (FOREACH_OMS_STATE_FLAG — the first flag placed past the slot, D-526)
//   bits 6..7  — free for a future flag or slot
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
//   - LIVE_TRADING:         set ONCE at OrderManager_Init from the OmsSession (LIVE — D-526; the boot derives the
//                            session from ControllerConfig_IsLiveCapital, the one live-capital predicate).
//   - PARTIAL_EXIT_ENABLED: set ONCE at engine init from cfg lifecycle
//                            flags; toggle requires snapshot v3 reload.
//   - KILL_SWITCH_TRIPPED:  set by the composer only — the per-OMS drawdown
//                            gate (EventLoop_KillSwitchEvaluate) or a consumed
//                            GLOBAL lane of AggregatorState::kill_trip_request
//                            (EventLoop_KillSwitchTrip; D-479). In LIVE never cleared
//                            at runtime — restart-only by design (D-481 / TD-328;
//                            EventLoop_Unpause was deleted at 3b(ii) commit 1). In PAPER
//                            the composer-executed paper reset clears it as a DO_RESET
//                            row (D-481 paper clause, 2026-09-04).
//   - PAPER_PERSIST:        set ONCE at OrderManager_Init from the OmsSession (PAPER — D-526). The paper snapshot
//                            loads / saves (ShardedSnapshot_Load / _Save refuse without it) and the paper reset runs
//                            (its executor refuses without it) ONLY in a PAPER session; LIVE and EPHEMERAL leave it
//                            clear. Boot-latched: SKIP_RESET, SKIP_PERSIST.
//   - EVENT_LOG_MODE:       set ONCE at OrderManager_Init from
//                            cfg.oms_event_log_mode; read by drainer +
//                            backtest hot paths (single thread per OMS).
// No BITMAP_ATOMIC_* / MBS_ATOMIC_* needed; regular BITMAP_SET/CLR + MBS_SET/GET
// suffice.
//
// Wire-format note (parity-tested-by-construction; H9 wire preservation):
// kill_switch_tripped is persisted in ShardedSnapshotPersist as int (4 bytes
// at a fixed offset). Save/load path read/write the BIT VALUE as int —
// wire format unchanged (no snapshot version bump required). EVENT_LOG_MODE
// is SKIP_PERSIST (cfg-derived; wire format unchanged across this addition).
//
// Adding a new single-bit COLD-cluster bool (1 row in FOREACH_OMS_STATE_FLAG):
//   1. Append X(NAME, bit, "doc") with a FREE bit — the generated layout assert
//      refuses one that a flag or a slot already holds
//   2. Auto-generated OMS_STATE_FLAG_<NAME> bit position + MASK_OMS_STATE_<NAME>
//   3. Migrate writer sites: oms.NAME = 1 → OMS_STATE_FLAG_SET(oms, NAME)
//   4. Migrate reader sites: if (oms.NAME) → OMS_STATE_FLAG_IS_SET(oms, NAME)
//   Never MOVE an existing flag or slot to make room: every reader's machine code
//   carries the mask and shift as immediates, so a move changes the code of every
//   reader (D-526 measured it — plans/v5.15-live-readiness/plan_checks/2026-10-09-d526-claims/).
//
// Adding a new K-state COLD-cluster slot (1 row in FOREACH_OMS_STATE_MULTI_BIT):
//   1. Append X(NAME, bits, shift, "doc") to FOREACH_OMS_STATE_MULTI_BIT, in free bits
//   2. The generated layout assert refuses an overlap with any flag or slot
//   3. Auto-generated MASK_OMS_STATE_<NAME> + SHIFT_OMS_STATE_<NAME> + BITS_OMS_STATE_<NAME>
//   4. Migrate accessor sites: oms.NAME = val → MBS_SET_U8(oms.oms_state_flags, ...)
//      and `if (oms.NAME == val)` → `if (MBS_EQ_U8(oms.oms_state_flags, ..., val))`
//
// Cross-references (the BITMAP_* API / X-macro registry disciplines + the
// uint16_t Portfolio bitmap precedent + parity-tested-by-construction):
//   DESIGN_SPECS/bitmap-flag-api.md (7th application of bitmap-flag-api)
//   DESIGN_SPECS/multi-bit-state-encoding-pattern.md (2nd codebase application;
//     the predicted promotion HAPPENED — the manual SHIFT_*/MASK_* + MBS_*
//     accessor discipline is codified as H14)
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

//======================================================================
// [REGISTRY]_[FOREACH_OMS_STATE_FLAG]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BITMAP_PACKED] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[BIT_FLAG rows (LIVE_TRADING / PARTIAL_EXIT_ENABLED / KILL_SWITCH_TRIPPED / PAPER_PERSIST), each at an EXPLICIT bit of the hybrid word (D-526) + MASK_OMS_STATE_<name> constants; per-row range asserts ride, the layout assert over both registries follows FOREACH_OMS_STATE_MULTI_BIT]
// [COLUMN]_[name]_[UPPERCASE token; produces the OMS_STATE_FLAG_<name> bit + MASK_OMS_STATE_<name> constant]
// [COLUMN]_[bit]_[the flag's bit position, 0..7 — explicit, never derived from row order; a FREE bit (the layout assert refuses one a flag or slot holds)]
// [COLUMN]_[doc_string]_[human-readable description for audits + docs]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_OMS_STATE_FLAG(X)                                                                       \
    /* Live-trading mode flag. Set ONCE at OrderManager_Init from the OmsSession (LIVE — D-526).     */ \
    /* Gates Submit-time exchange-adapter dispatch (paper short-circuits; live calls the adapter).  */ \
    X(LIVE_TRADING, 0,                                                                                  \
      "live-trading mode: 0 = paper (adapter callbacks suppressed); 1 = live (adapter required)")       \
    /* Partials geometry mirrored from cfg.lifecycle_cfg_flags. Set ONCE at engine init; drainer     */ \
    /* uses it for slot→node_id mapping (Sharded_LegSlot). Toggle requires snapshot v3 reload.      */ \
    X(PARTIAL_EXIT_ENABLED, 1,                                                                          \
      "partials enabled: 0 = slot==node_id; 1 = slot = 2*node_id+leg (leg A/B per core)")               \
    /* Kill switch trip state. Set by the composer only (the drawdown gate, or a consumed GLOBAL     */ \
    /* lane of kill_trip_request — D-479). LIVE: never cleared at runtime (restart-only, D-481/   */ \
    /* TD-328). PAPER: the composer-executed paper reset clears it (DO_RESET; D-481 paper clause). */ \
    /* Tripping clears every registered core's permission with RELEASE; idempotent. Persisted as   */ \
    /* int (4 bytes) in snapshot — wire format preserved.                                           */ \
    X(KILL_SWITCH_TRIPPED, 2,                                                                           \
      "OMS-wide kill switch tripped; entries blocked until manual resume")                               \
    /* Paper-session persistence (D-526). Set ONCE at OrderManager_Init from the OmsSession (PAPER).  */ \
    /* ShardedSnapshot_Load / _Save refuse without it and the paper reset's executor runs only with  */ \
    /* it — the check at the SINK, so every route to a snapshot passes it. LIVE (exchange truth) and */ \
    /* EPHEMERAL (a synthetic feed, the backtest, a test) leave it clear. Bit 5: the first FREE bit  */ \
    /* past EVENT_LOG_MODE's slot — no existing bit moved.                                            */ \
    X(PAPER_PERSIST, 5,                                                                                 \
      "paper-session persistence: 1 = the snapshot loads / saves and the paper reset runs; 0 = each refuses")

//------------------------------------------------------------------
// [SECTION]_[AUTO-GENERATED BIT POSITIONS + MASK CONSTANTS]
//------------------------------------------------------------------
// The bit is the row's own column — never row order, so appending a row can never renumber another flag. No COUNT
// sentinel: with explicit positions an enum's next value is not a count (FOREACH_OMS_STATE_FLAG_COUNT below is).
#define X_GEN_OMS_STATE_BIT(name, bit, doc) OMS_STATE_FLAG_##name = (bit),
enum OmsStateFlag {
    FOREACH_OMS_STATE_FLAG(X_GEN_OMS_STATE_BIT)
};
#undef X_GEN_OMS_STATE_BIT

#define X_GEN_OMS_STATE_MASK(name, bit, doc) \
    inline constexpr uint8_t MASK_OMS_STATE_##name = BITMAP_BIT_U8(bit);
FOREACH_OMS_STATE_FLAG(X_GEN_OMS_STATE_MASK)
#undef X_GEN_OMS_STATE_MASK

// [ASSERT]_[BITMAP_OVERFLOW]_[every flag's bit inside the uint8_t — one assert per row]
#define X_GEN_OMS_STATE_BIT_RANGE(name, bit, doc)                                                          \
    static_assert((bit) >= 0 && (bit) < 8, "OMS state flag " #name ": bit outside the uint8_t "            \
                  "oms_state_flags (0..7) — pick a free bit, or widen the word (and every BITMAP_*_U8 accessor)");
FOREACH_OMS_STATE_FLAG(X_GEN_OMS_STATE_BIT_RANGE)
#undef X_GEN_OMS_STATE_BIT_RANGE

// Public count for tests (uses >= per /readiness Check 21).
#define X_GEN_OMS_STATE_COUNT_ONE(name, bit, doc) +1
#define FOREACH_OMS_STATE_FLAG_COUNT (0 FOREACH_OMS_STATE_FLAG(X_GEN_OMS_STATE_COUNT_ONE))
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Tuple: X(name, bit, doc_string)
//   name       — UPPERCASE token; produces OMS_STATE_FLAG_<name> bit position
//                + MASK_OMS_STATE_<name> uint8_t mask constant
//   bit        — the flag's EXPLICIT position (0..7)
//   doc_string — human-readable description for audits + docs
//
// HYBRID word: bits 3..4 host the EVENT_LOG_MODE slot, so the next flag takes a FREE
// bit (5..7) — written in its row, checked by the layout assert below the multi-bit
// registry (it refuses a bit any flag or slot already holds). Until D-526 the bit came
// from row order, so a 4th flag landed on bit 3 inside EVENT_LOG_MODE's slot, and
// "bump EVENT_LOG_MODE's SHIFT" was the documented way out — which changes the machine
// code of every EVENT_LOG_MODE reader (the mask and shift are immediates). Never move
// an existing flag or slot; take a free bit, or widen the word.
//======================================================================
// [END_REGISTRY]_[FOREACH_OMS_STATE_FLAG]
//======================================================================

//======================================================================
// [REGISTRY]_[FOREACH_OMS_STATE_MULTI_BIT]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BITMAP_PACKED] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[K-state slots co-located in the SAME uint8_t (EVENT_LOG_MODE 2b@3, K=4 capacity; OmsEventLogMode enum + its capacity assert ride); explicit SHIFTs for at-a-glance layout; per-row range asserts + the layout assert over BOTH registries generated (TECH_DEBT-042)]
// [COLUMN]_[name]_[UPPERCASE token; produces BITS/SHIFT/MASK_OMS_STATE_<name>]
// [COLUMN]_[bits]_[slot width (compile-time int constant; 1..8)]
// [COLUMN]_[shift]_[slot bit position; in FREE bits — clear of every flag and every other slot (the generated layout assert enforces)]
// [COLUMN]_[doc_string]_[human-readable description for audits + docs]
//======================================================================
// [CODE]
//======================================================================
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

//------------------------------------------------------------------
// [SECTION]_[AUTO-GENERATED MULTI-BIT SLOT CONSTANTS]
//------------------------------------------------------------------
#define X_GEN_OMS_STATE_MULTI_BIT(name, bits, shift, doc)                                             \
    inline constexpr uint8_t BITS_OMS_STATE_##name  = (uint8_t)(bits);                                \
    inline constexpr uint8_t SHIFT_OMS_STATE_##name = (uint8_t)(shift);                               \
    inline constexpr uint8_t MASK_OMS_STATE_##name  =                                                 \
        (uint8_t)(((1u << (bits)) - 1) << (shift));
FOREACH_OMS_STATE_MULTI_BIT(X_GEN_OMS_STATE_MULTI_BIT)
#undef X_GEN_OMS_STATE_MULTI_BIT

// [ASSERT]_[BITMAP_OVERFLOW]_[every slot non-empty and inside the uint8_t — one assert per row]
#define X_GEN_OMS_STATE_SLOT_RANGE(name, bits, shift, doc)                                                 \
    static_assert((bits) >= 1 && (shift) >= 0 && (shift) + (bits) <= 8, "OMS state slot " #name ": "       \
                  "empty (bits < 1) or outside the uint8_t oms_state_flags (shift + bits must be <= 8) — pick free bits, or widen " \
                  "the word (and every BITMAP_*_U8 / MBS_*_U8 accessor)");
FOREACH_OMS_STATE_MULTI_BIT(X_GEN_OMS_STATE_SLOT_RANGE)
#undef X_GEN_OMS_STATE_SLOT_RANGE

// [ASSERT]_[LAYOUT_LOCK]_[no two rows of EITHER registry share a bit — generated, TECH_DEBT-042]
// One table of every mask in the word — each flag, then each slot — and one check over it: no mask overlapping any
// earlier one (an EMPTY mask cannot reach it — each row's range assert above refuses one). It replaces the hand-rolled per-slot asserts (EVENT_LOG_MODE vs a single-bit region
// derived from the flag COUNT — which stopped describing the layout once positions became explicit), so a flag or a
// slot added to either registry is checked against every other row with no assert to write.
#define X_GEN_OMS_STATE_FLAG_MASK_ELEM(name, bit, doc) MASK_OMS_STATE_##name,
#define X_GEN_OMS_STATE_SLOT_MASK_ELEM(name, bits, shift, doc) MASK_OMS_STATE_##name,
inline constexpr uint8_t _OMS_STATE_LAYOUT_MASKS[] = {
    FOREACH_OMS_STATE_FLAG(X_GEN_OMS_STATE_FLAG_MASK_ELEM)
    FOREACH_OMS_STATE_MULTI_BIT(X_GEN_OMS_STATE_SLOT_MASK_ELEM)
};
#undef X_GEN_OMS_STATE_FLAG_MASK_ELEM
#undef X_GEN_OMS_STATE_SLOT_MASK_ELEM
constexpr bool _OmsState_LayoutDisjoint() {
    uint8_t held = 0;
    for (uint8_t m : _OMS_STATE_LAYOUT_MASKS) {
        if ((held & m) != 0) return false;
        held = (uint8_t)(held | m);
    }
    return true;
}
static_assert(_OmsState_LayoutDisjoint(),
              "oms_state_flags layout: two rows of FOREACH_OMS_STATE_FLAG / FOREACH_OMS_STATE_MULTI_BIT share a bit "
              "— give the new row a FREE bit; never move an existing one (D-526)");

// EVENT_LOG_MODE value-capacity check (K <= slot width):
// EVENT_LOG_MODE has K=2 used today (legacy/event-log); K=4 supported by 2-bit slot.
// If a future contributor adds a 5th mode, this static_assert tells them to widen the slot.
enum OmsEventLogMode {
    OMS_EVENT_LOG_MODE_LEGACY    = 0,
    OMS_EVENT_LOG_MODE_EVENT_LOG = 1,
    // 2, 3 reserved for future modes
    OMS_EVENT_LOG_MODE_COUNT  // sentinel; must remain <= (1 << BITS_OMS_STATE_EVENT_LOG_MODE)
};
// [ASSERT]_[BITMAP_OVERFLOW]_[OMS_EVENT_LOG_MODE_COUNT <= 1 << EVENT_LOG_MODE_BITS]
static_assert(OMS_EVENT_LOG_MODE_COUNT <= (1u << BITS_OMS_STATE_EVENT_LOG_MODE),
              "OMS_EVENT_LOG_MODE_COUNT exceeds EVENT_LOG_MODE slot capacity; "
              "widen BITS_OMS_STATE_EVENT_LOG_MODE in FOREACH_OMS_STATE_MULTI_BIT");

// Public count for multi-bit slots (uses >= per /readiness Check 21).
#define X_GEN_OMS_STATE_MULTI_BIT_COUNT_ONE(name, bits, shift, doc) +1
#define FOREACH_OMS_STATE_MULTI_BIT_COUNT \
    (0 FOREACH_OMS_STATE_MULTI_BIT(X_GEN_OMS_STATE_MULTI_BIT_COUNT_ONE))
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Tuple: X(name, bits, shift, doc_string)
//   name       — UPPERCASE token; produces BITS/SHIFT/MASK_OMS_STATE_<name>
//   bits       — slot width (compile-time int constant; 1..8)
//   shift      — slot bit position (compile-time int constant; MUST sit in
//                 FREE bits — clear of every single-bit flag and every other
//                 slot; the generated layout assert refuses an overlap)
//   doc_string — human-readable description for audits + docs
//
// Slot positions are explicit (not auto-derived from preceding slot widths)
// for review-readability — operator sees the bit layout at a glance; since
// D-526 the single-bit flags' positions are explicit too. The generated asserts
// check every row's range and that no two rows of either registry share a bit.
//======================================================================
// [END_REGISTRY]_[FOREACH_OMS_STATE_MULTI_BIT]
//======================================================================

}  // namespace tt

//----------------------------------------------------------------------
// [MACRO]_[OMS_STATE_FLAG_*]
// [TAG]_[[ENGINE] [BITMAP_PACKED] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[ergonomic bare-name accessors over oms.oms_state_flags (mirror the NODE_STATE_FLAG_* shape) — IS_SET/SET/CLR/TOGGLE + BITMAP_ANY for multi-flag]
//----------------------------------------------------------------------
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

#define OMS_STATE_FLAG_IS_SET(oms, name) BITMAP_IS_SET((oms).oms_state_flags, tt::MASK_OMS_STATE_##name)
#define OMS_STATE_FLAG_SET(oms, name)    BITMAP_SET((oms).oms_state_flags, tt::MASK_OMS_STATE_##name)
#define OMS_STATE_FLAG_CLR(oms, name)    BITMAP_CLR((oms).oms_state_flags, tt::MASK_OMS_STATE_##name)
#define OMS_STATE_FLAG_TOGGLE(oms, name) BITMAP_TOGGLE((oms).oms_state_flags, tt::MASK_OMS_STATE_##name)

//----------------------------------------------------------------------
// [MACRO]_[OMS_STATE_MULTI_BIT_*]
// [TAG]_[[ENGINE] [BITMAP_PACKED] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[K-state slot accessors over the hybrid word — GET/SET/EQ via the MBS_*_U8 primitives; MASK + SHIFT auto-derived from the registry]
//----------------------------------------------------------------------
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

#define OMS_STATE_MULTI_BIT_GET(oms, name) \
    MBS_GET_U8((oms).oms_state_flags, tt::MASK_OMS_STATE_##name, tt::SHIFT_OMS_STATE_##name)

#define OMS_STATE_MULTI_BIT_SET(oms, name, val) \
    MBS_SET_U8((oms).oms_state_flags, tt::MASK_OMS_STATE_##name, tt::SHIFT_OMS_STATE_##name, (val))

#define OMS_STATE_MULTI_BIT_EQ(oms, name, val) \
    MBS_EQ_U8((oms).oms_state_flags, tt::MASK_OMS_STATE_##name, tt::SHIFT_OMS_STATE_##name, (val))

#endif  // OMS_STATE_FLAG_REGISTRY_HPP
