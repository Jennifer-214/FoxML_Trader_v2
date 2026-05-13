// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [CORE CONTEXT STATE FLAGS REGISTRY — v5.15.5.B.3]
//======================================================================================================
// Bit-packed boolean state for CoreContext. Per CLAUDE.md item 20
// (BITMAP_* universalization) + DESIGN_SPECS/bitmap-flag-api.md, when
// 3+ boolean flags coexist on the same struct, bit-pack them into a
// uint8_t/uint16_t/uint64_t bitmap rather than carrying byte-per-flag
// fields with their own alignment padding.
//
// Distinct from FOREACH_PER_CORE_STATE_FLAG (PerCoreStateFlagsRegistry.hpp)
// which is for PerCoreSnap's snapshot-side observability. This registry
// is for CoreContext's slow-path-LIVE state. Single-writer per core (the
// per-core slow-path thread); single-reader (same thread); no atomics
// needed within the per-core write window.
//
// CLOSES the byte-per-flag pattern on CoreContext (5 booleans × 1 byte +
// _pad_kill[3] alignment padding ~8 bytes per CoreContext × 16 cores =
// 128 bytes per EventLoopState) → 1 uint8_t bitmap (1 byte × 16 cores =
// 16 bytes; net savings ~112 bytes per EventLoopState plus better cache
// locality + branchless multi-flag check via BITMAP_ANY).
//
// Three of these flags (model_load_failed, cfg_drift_strict_refused,
// warmup_log_emitted) temporarily lived on CoreContextDisplayMeta after
// v5.15.5.B.2 (extraction-stage residency). v5.15.5.B.3 moves them back
// to CoreContext as bitmap bits — final home. The other two (dirty,
// core_kill_tripped) stayed on CoreContext through .B.2 and migrate into
// the bitmap here.
//
// Adding a new flag (1 row):
//   1. Append X(NAME, "doc") to FOREACH_CORE_STATE_FLAG
//   2. Auto-generated CORE_STATE_FLAG_<NAME> bit position + MASK_CORE_STATE_<NAME> constant
//   3. Migrate writer sites: ctx.NAME = 1 → CORE_STATE_FLAG_SET(ctx, NAME)
//   4. Migrate reader sites: if (ctx.NAME) → CORE_STATE_FLAG_IS_SET(ctx, NAME)
//
// Cross-references:
//   CLAUDE.md item 20 (BITMAP_* API)
//   CLAUDE.md item 13 (X-macro registry)
//   CLAUDE.md item 1 (uint16_t Portfolio bitmap precedent)
//   DESIGN_SPECS/bitmap-flag-api.md (6th application of bitmap-flag-api per /readiness Check 21)
//   DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md
//   CLAUDE.local.md 2026-05-11 cohort-audit rule (all 5 booleans audited together)
//======================================================================================================
#ifndef CORE_STATE_FLAG_REGISTRY_HPP
#define CORE_STATE_FLAG_REGISTRY_HPP

#include <stdint.h>
#include "BitmapMacros.hpp"  // BITMAP_* primitives (v5.14.8.A.0.b)

namespace tt {

//======================================================================================================
// [REGISTRY DEFINITION]
//======================================================================================================
// Tuple: X(name, doc_string)
//   name       — UPPERCASE token; produces CORE_STATE_FLAG_<name> bit position
//                + MASK_CORE_STATE_<name> uint8_t mask constant
//   doc_string — human-readable description for audits + docs
//
// 5 entries used; 3 bits headroom in uint8_t. Promote storage to uint16_t
// if/when a 9th entry needs adding (static_assert below catches overflow).
//======================================================================================================
#define FOREACH_CORE_STATE_FLAG(X)                                                                  \
    /* v4.0 pending_params dirty bit. Single-threaded set by RebuildOneCore on cycle close;       */ \
    /* cleared by PushParameters_OneCore after pushing the new param block. Branchless gate at    */ \
    /* PushParameters_OneCore entry: BITMAP_IS_SET → skip push if 0. */                              \
    X(DIRTY,                                                                                          \
      "pending_params has been freshly built and not yet pushed to ExecutionCore")                    \
    /* v3.x kill switch trip. Set by per-core drawdown gate; entries zero-gated with HALT_CORE_KILL  */ \
    /* while bit is set. Open positions ride to TP/SL (we don't force-close on trip; kill blocks    */ \
    /* NEW trades only). Operator can manually reset via TUISharedState::kill_reset_per_core[N].    */ \
    X(KILL_TRIPPED,                                                                                   \
      "per-core kill switch active; entries zero-gated until manual reset clears")                    \
    /* v5.9.0b ML model load refusal. Set by EngineSharded boot OR hot-swap path when CoreModelZoo  */ \
    /* allocation fails or strict-mode verify mismatches. ML strategy falls back to SimpleDip-like  */ \
    /* behavior when set; operator sees the flag in ML Status panel + entry log. (.B.2 → DisplayMeta */ \
    /* temporarily; .B.3 migrates back to CoreContext as a bit.) */                                  \
    X(MODEL_LOAD_FAILED,                                                                              \
      "ML model attempted but refused/missing — treated as no-model-loaded for entry decisions")      \
    /* v5.9.5i cfg-drift strict-mode refusal. Set by CoreModelZoo_ValidateAgainstCfg when Tier 1   */ \
    /* mismatch in strict mode. ML decisions are blocked when set (entries zero-gated; operator    */ \
    /* must reconcile cfg vs stamp or set acknowledge bit + restart). (.B.2 → DisplayMeta; .B.3   */ \
    /* migrates back to CoreContext as a bit.) */                                                    \
    X(CFG_DRIFT_STRICT_REFUSED,                                                                       \
      "cfg-drift strict-mode refusal active for this core; entries blocked until cfg reconciled")     \
    /* v5.9.1 boot-time per-core warmup-complete log edge-trigger. Set ONCE per session per core   */ \
    /* after the first RebuildOneCore cycle observing rolling.count >= min_warmup_samples. Fires   */ \
    /* the log + sets bit so subsequent cycles don't re-emit. (.B.2 → DisplayMeta; .B.3 migrates  */ \
    /* back to CoreContext as a bit.) */                                                             \
    X(WARMUP_LOG_EMITTED,                                                                             \
      "boot-time per-core warmup-complete log has been emitted (edge-trigger)")

//======================================================================================================
// [AUTO-GENERATED BIT POSITIONS + MASK CONSTANTS]
//======================================================================================================
#define X_GEN_CORE_STATE_BIT(name, doc) CORE_STATE_FLAG_##name,
enum CoreStateFlag {
    FOREACH_CORE_STATE_FLAG(X_GEN_CORE_STATE_BIT)
    CORE_STATE_FLAG_COUNT  // sentinel
};
#undef X_GEN_CORE_STATE_BIT

#define X_GEN_CORE_STATE_MASK(name, doc) \
    static constexpr uint8_t MASK_CORE_STATE_##name = BITMAP_BIT_U8(CORE_STATE_FLAG_##name);
FOREACH_CORE_STATE_FLAG(X_GEN_CORE_STATE_MASK)
#undef X_GEN_CORE_STATE_MASK

static_assert(CORE_STATE_FLAG_COUNT <= 8,
              "core_state_flags is uint8_t; max 8 entries. Widen to uint16_t "
              "if adding a 9th — and update the field type on CoreContext.");

// Public count for tests (uses >= per /readiness Check 21).
#define X_GEN_CORE_STATE_COUNT_ONE(name, doc) +1
#define FOREACH_CORE_STATE_FLAG_COUNT (0 FOREACH_CORE_STATE_FLAG(X_GEN_CORE_STATE_COUNT_ONE))

}  // namespace tt

//======================================================================================================
// [CORE_STATE_FLAG_* convenience macros]
//======================================================================================================
// Mirror PerCoreStateFlagsRegistry STATE_FLAG_* shape. Reads naturally:
//   Set:    CORE_STATE_FLAG_SET(ctx, DIRTY)
//   Clear:  CORE_STATE_FLAG_CLR(ctx, DIRTY)
//   Read:   CORE_STATE_FLAG_IS_SET(ctx, DIRTY)
//   Toggle: CORE_STATE_FLAG_TOGGLE(ctx, DIRTY)
//   Any-of: BITMAP_ANY(ctx.core_state_flags, tt::MASK_CORE_STATE_X | tt::MASK_CORE_STATE_Y)
//
// MASK_CORE_STATE_<name> lives in tt:: namespace; macros assume the tt::
// scope is visible at call sites (most callers already `using namespace tt;`).
//======================================================================================================

#define CORE_STATE_FLAG_IS_SET(ctx, name) BITMAP_IS_SET((ctx).core_state_flags, tt::MASK_CORE_STATE_##name)
#define CORE_STATE_FLAG_SET(ctx, name)    BITMAP_SET((ctx).core_state_flags, tt::MASK_CORE_STATE_##name)
#define CORE_STATE_FLAG_CLR(ctx, name)    BITMAP_CLR((ctx).core_state_flags, tt::MASK_CORE_STATE_##name)
#define CORE_STATE_FLAG_TOGGLE(ctx, name) BITMAP_TOGGLE((ctx).core_state_flags, tt::MASK_CORE_STATE_##name)

#endif  // CORE_STATE_FLAG_REGISTRY_HPP
