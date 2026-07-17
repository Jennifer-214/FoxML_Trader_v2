// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/OmsExitPredictorMetaRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BITMAP_PACKED] [OMS_DRAINER] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-portfolio-slot exit-predictor MBS byte — 2-bit regime + 4-bit arm + valid bit in one uint8_t per slot; replaces two parallel int8_t[16] arrays (-1 sentinel)]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_OMS_META_SLOT]   (auto-gen slot constants + layout asserts ride the block)
//   - [MACRO]_[OMS_META_*]
//   - [MACRO]_[OMS_RESET_PER_SLOT_EXIT_PREDICTOR]
// [REFERENCE]_[DESIGN_SPEC]_[multi-bit-state-encoding-pattern]
// [REFERENCE]_[INVARIANT]_[H14]
//======================================================================================================
// FIRST APPLICATION of DESIGN_SPECS/multi-bit-state-encoding-pattern.md.
//
// Packs per-portfolio-slot exit-predictor state (regime + arm + validity)
// into a single uint8_t per slot, replacing two parallel int8_t[16] arrays
// (last_exit_predicted_arm + last_exit_predicted_regime) that consumed 32
// bytes per OMS for two K-state values + a -1 sentinel.
//
// Pre-LOW-2:
//   int8_t last_exit_predicted_arm[16];      // 16 bytes; -1 = unset
//   int8_t last_exit_predicted_regime[16];   // 16 bytes; -1 = unset
//   Total: 32 bytes per OMS. 2 separate arrays read together.
//
// Post-LOW-2:
//   uint8_t last_exit_predicted_meta[16];    // 16 bytes; bit 6 = valid
//   Total: 16 bytes per OMS. Single array; parallel read of arm + regime.
//   Savings: 16 bytes per OMS.
//
// Per-slot byte layout (per-record packing discipline + multi-bit-state-encoding-pattern.md):
//   bits 0..1 (2 bits) = regime   — REGIME_RANGING(0) .. REGIME_MILD_TREND(3)
//   bits 2..5 (4 bits) = arm      — 0..15 (current ezoo->exit_predictor_count max ~8)
//   bit  6    (1 bit)  = valid    — 1 = arm + regime populated; 0 = unset (replaces -1 sentinel)
//   bit  7             — reserved
//
// Single-thread access (per-arm reward observability invariant):
//   - WRITER: the per-node slow-path body (EngineCommon.hpp) at submit time (single thread per OMS).
//   - READER: drainer in ControllerEventLoop.hpp HandleFill attribution (single thread).
//   - CLEAR:  drainer post-attribution (same thread as read).
// SPSC ring release-acquire fence provides cross-thread visibility (set BEFORE
// OMS_PushSubmit; read AFTER OMS_DrainSubmit). No atomic accessors needed.
//
// Branchless inference API (per multi-bit-state-encoding-pattern.md):
//   OMS_META_GET_REGIME(byte)        — 1 AND, returns uint8_t
//   OMS_META_GET_ARM(byte)           — 1 AND + 1 SHR, returns uint8_t
//   OMS_META_IS_VALID(byte)          — 1 AND, returns bool (via != 0)
//   OMS_META_PACK(arm, regime)       — 1 OR + 1 SHL + bit, returns uint8_t
//   OMS_META_CLEAR(byte)             — store 0, clears valid bit + slots
//
// Parallel decode: arm + regime extracts have no data dependency on each
// other — modern superscalar CPUs decode both in 1-2 cycles via ILP.
// AVX-512 batch decode of the full [16]-byte array is possible if a future
// consumer iterates all slots together.
//
// Cross-references (the per-record packing discipline + the latency-vs-
// cache framework — 16 bytes saved per OMS):
//   DESIGN_SPECS/multi-bit-state-encoding-pattern.md (the design — first application)
//   DESIGN_SPECS/bitmap-flag-api.md (sister 1-bit specialization)
//   v5.15.5.C.2 commit 852a6e3 (OmsStateFlagRegistry.hpp — companion 1-bit-cohort registry)
//   v5.15.5.B.3 commit 4dd721e (NodeStateFlagRegistry.hpp — bitmap-flag-api 6th application)
//======================================================================================================
#ifndef OMS_EXIT_PREDICTOR_META_REGISTRY_HPP
#define OMS_EXIT_PREDICTOR_META_REGISTRY_HPP

#include <stdint.h>
#include "BitmapMacros.hpp"

namespace tt {

//======================================================================
// [REGISTRY]_[FOREACH_OMS_META_SLOT]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BITMAP_PACKED] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[3 MBS slot rows (REGIME 2b@0 / ARM 4b@2 / VALID 1b@6; bit 7 reserved) -> OMS_META_<NAME>_{BITS,SHIFT,MASK} constants; contiguity + non-overlap assert-locked]
// [COLUMN]_[name]_[UPPERCASE token; produces OMS_META_<NAME>_{BITS,SHIFT,MASK}]
// [COLUMN]_[bits]_[slot bit-width (compile-time int constant)]
// [COLUMN]_[shift]_[slot bit position (compile-time int constant)]
// [COLUMN]_[doc_string]_[human-readable description for audits + docs]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_OMS_META_SLOT(X)                                                                       \
    /* Regime classification at exit-predictor submit time. 2 bits = 4 states                       */ \
    /* (RANGING/TRENDING/VOLATILE/MILD_TREND). Used by drainer for Bandit_Update.                   */ \
    X(REGIME, 2, 0,                                                                                    \
      "regime at exit submit; 2-bit value: REGIME_RANGING..REGIME_MILD_TREND")                         \
    /* Bandit arm chosen at exit-predictor submit time. 4 bits = 0..15 (current ezoo->exit_         */ \
    /* predictor_count maxes out near 8; 4 bits gives headroom).                                    */ \
    X(ARM, 4, 2,                                                                                       \
      "bandit arm chosen at exit submit; 4-bit value 0..15")                                           \
    /* Validity flag. 1 = arm + regime are populated (set at submit time); 0 = unset (post-init,   */ \
    /* post-attribution clear). Replaces the int8_t -1 sentinel from pre-LOW-2 design.             */ \
    X(VALID, 1, 6,                                                                                     \
      "1 = arm + regime populated; 0 = unset (replaces -1 sentinel)")

//------------------------------------------------------------------
// [SECTION]_[AUTO-GENERATED SLOT CONSTANTS]
//------------------------------------------------------------------
#define X_GEN_OMS_META_SLOT(name, bits, shift, doc)                                                    \
    static constexpr uint8_t OMS_META_##name##_BITS  = (uint8_t)(bits);                                \
    static constexpr uint8_t OMS_META_##name##_SHIFT = (uint8_t)(shift);                               \
    static constexpr uint8_t OMS_META_##name##_MASK  = (uint8_t)(((1u << (bits)) - 1) << (shift));
FOREACH_OMS_META_SLOT(X_GEN_OMS_META_SLOT)
#undef X_GEN_OMS_META_SLOT

// Compile-time sanity:
//   - All slots fit in low 7 bits (bit 7 reserved).
//   - Slots don't overlap (mask AND = 0 between distinct slots).
// [ASSERT]_[BITMAP_OVERFLOW]_[(REGIME|ARM|VALID masks) <= 0x7F — bit 7 reserved]
static_assert((OMS_META_REGIME_MASK | OMS_META_ARM_MASK | OMS_META_VALID_MASK) <= 0x7F,
              "OMS exit-predictor meta slots overflow uint8_t low 7 bits; bit 7 reserved");
// [ASSERT]_[LAYOUT_LOCK]_[(REGIME_MASK & ARM_MASK) == 0]
static_assert((OMS_META_REGIME_MASK & OMS_META_ARM_MASK) == 0,
              "OMS_META_REGIME and OMS_META_ARM slot masks overlap");
// [ASSERT]_[LAYOUT_LOCK]_[(ARM_MASK & VALID_MASK) == 0]
static_assert((OMS_META_ARM_MASK & OMS_META_VALID_MASK) == 0,
              "OMS_META_ARM and OMS_META_VALID slot masks overlap");
// [ASSERT]_[LAYOUT_LOCK]_[(REGIME_MASK & VALID_MASK) == 0]
static_assert((OMS_META_REGIME_MASK & OMS_META_VALID_MASK) == 0,
              "OMS_META_REGIME and OMS_META_VALID slot masks overlap");
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Tuple: X(name, bits, shift, doc_string)
//   name       — UPPERCASE token; produces OMS_META_<NAME>_{BITS,SHIFT,MASK}
//   bits       — slot bit-width (compile-time int constant)
//   shift      — slot bit position (compile-time int constant)
//   doc_string — human-readable description for audits + docs
//
// Slot layout is contiguous + non-overlapping; static_asserts above validate.
// Bit 7 reserved for future extension.
//======================================================================
// [END_REGISTRY]_[FOREACH_OMS_META_SLOT]
//======================================================================

}  // namespace tt

//----------------------------------------------------------------------
// [MACRO]_[OMS_META_*]
// [TAG]_[[ENGINE] [BITMAP_PACKED] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[branchless MBS inference API — GET_REGIME/GET_ARM/IS_VALID/PACK/CLEAR; 1-2 instruction ops, ILP parallel decode]
//----------------------------------------------------------------------
// Per DESIGN_SPECS/multi-bit-state-encoding-pattern.md. All macros are 1-2
// instruction ops; no branches. Modern compilers (GCC 13+, Clang 17+) emit
// parallel decode when GET_REGIME + GET_ARM are read together in the same
// expression (ILP — independent extracts, no data dependency).

// Extract regime (low 2 bits). Single AND.
#define OMS_META_GET_REGIME(byte) \
    ((uint8_t)((byte) & tt::OMS_META_REGIME_MASK))

// Extract arm (bits 2..5). AND + SHR (often fused into BMI BEXTR by compiler).
#define OMS_META_GET_ARM(byte) \
    ((uint8_t)(((byte) & tt::OMS_META_ARM_MASK) >> tt::OMS_META_ARM_SHIFT))

// Test validity bit. Returns bool (via != 0 — avoids int-truncation hazard).
#define OMS_META_IS_VALID(byte) \
    (((byte) & tt::OMS_META_VALID_MASK) != 0)

// Pack arm + regime + valid into a single byte. Compile-time-constant when
// arm + regime are literals.
#define OMS_META_PACK(arm, regime)                                                  \
    ((uint8_t)(                                                                      \
        ((uint8_t)(regime) & tt::OMS_META_REGIME_MASK) |                             \
        (((uint8_t)(arm)   << tt::OMS_META_ARM_SHIFT) & tt::OMS_META_ARM_MASK) |     \
        tt::OMS_META_VALID_MASK                                                      \
    ))

// Clear the slot (sets byte to 0 — valid=0, arm=0, regime=0). Replaces the
// pre-LOW-2 dual `int8_t = -1` assignments.
#define OMS_META_CLEAR(byte) \
    ((byte) = (uint8_t)0)

//----------------------------------------------------------------------
// [MACRO]_[OMS_RESET_PER_SLOT_EXIT_PREDICTOR]
// [TAG]_[[ENGINE] [OMS_DRAINER] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[single chokepoint clearing all 3 per-slot exit-predictor components (bitmap bit + p + meta byte) — closes the 2-site Class-18 mirror; 3 single-cycle ops]
// [REFERENCE]_[CLASS]_[18]
//----------------------------------------------------------------------
// Shared macro that clears ALL THREE components of one slot's exit-predictor
// state atomically (from a per-slot single-thread perspective):
//   1. BITMAP_CLR on last_exit_predicted_bitmap (bit per slot)
//   2. last_exit_predicted_p[slot] = 0.0
//   3. OMS_META_CLEAR(last_exit_predicted_meta[slot])
//
// Closes the per-slot Class-18 mirror identified by /merge-scan MEDIUM-1
// (the same 3-line sequence appeared at 2 sites: OrderManager_Init per-slot
// loop body + ControllerEventLoop.hpp DrainPostFill post-attribution clear).
// Adding a future per-slot exit-predictor state field (e.g., per-slot bandit
// arm telemetry) now expands ONE macro definition — not 2 parallel sites.
//
// CALL CONTRACT:
//   - `oms` must be a non-null OrderManagerState<F>* pointer
//   - `slot` must be in [0, MAX_PORTFOLIO_POSITIONS) — caller verifies bounds
//   - Single-thread per-OMS access (BITMAP_CLR is non-atomic; correct because
//     the DrainPostFill caller + AUTOPOPULATE caller are single-thread per OMS
//     by the codebase invariant — see ControllerEventLoop.hpp DrainPostFill
//     thread-safety comment)
//
// Cost: 3 single-cycle ops (AND mask + zero store + zero store). Sub-cycle
// total on modern superscalar; well within slow-path budget
// (the DrainPostFill site is slow-path-cadence; OMS_INIT_AUTOPOPULATE
// site is boot-time).
#define OMS_RESET_PER_SLOT_EXIT_PREDICTOR(oms, slot)                                           \
    do {                                                                                       \
        BITMAP_CLR((oms)->last_exit_predicted_bitmap, BITMAP_BIT_U16(slot));                    \
        (oms)->last_exit_predicted_p[(slot)] = 0.0;                                             \
        OMS_META_CLEAR((oms)->last_exit_predicted_meta[(slot)]);                                \
    } while (0)

#endif  // OMS_EXIT_PREDICTOR_META_REGISTRY_HPP
