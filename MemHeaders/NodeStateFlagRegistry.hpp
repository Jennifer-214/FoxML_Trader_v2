// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/NodeStateFlagRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BITMAP_PACKED] [SLOW_PATH] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[NodeContext uint8_t node_state_flags SSoT — 6 slow-path-LIVE BIT_FLAG rows (dirty/kill/model-load/cfg-drift/warmup-log/model-corrupt); single-writer per core]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_NODE_STATE_FLAG]   (auto-gen enum/masks + overflow assert + count macro ride the block)
//   - [MACRO]_[NODE_STATE_FLAG_*]
// [REFERENCE]_[DESIGN_SPEC]_[bitmap-flag-api]
// [REFERENCE]_[INVARIANT]_[H14]
//======================================================================================================
// Bit-packed boolean state for NodeContext. Per the BITMAP_*
// universalization discipline + DESIGN_SPECS/bitmap-flag-api.md, when
// 3+ boolean flags coexist on the same struct, bit-pack them into a
// uint8_t/uint16_t/uint64_t bitmap rather than carrying byte-per-flag
// fields with their own alignment padding.
//
// Distinct from FOREACH_PER_NODE_STATE_FLAG (PerNodeStateFlagsRegistry.hpp)
// which is for PerNodeSnap's snapshot-side observability. This registry
// is for NodeContext's slow-path-LIVE state. Single-writer per core (the
// per-core slow-path thread); single-reader (same thread); no atomics
// needed within the per-core write window.
//
// CLOSES the byte-per-flag pattern on NodeContext (5 booleans × 1 byte +
// _pad_kill[3] alignment padding ~8 bytes per NodeContext × 16 cores =
// 128 bytes per EventLoopState) → 1 uint8_t bitmap (1 byte × 16 cores =
// 16 bytes; net savings ~112 bytes per EventLoopState plus better cache
// locality + branchless multi-flag check via BITMAP_ANY).
//
// Three of these flags (model_load_failed, cfg_drift_strict_refused,
// warmup_log_emitted) temporarily lived on NodeContextDisplayMeta after
// v5.15.5.B.2 (extraction-stage residency). v5.15.5.B.3 moves them back
// to NodeContext as bitmap bits — final home. The other two (dirty,
// node_kill_tripped) stayed on NodeContext through .B.2 and migrate into
// the bitmap here.
//
// Adding a new flag (1 row):
//   1. Append X(NAME, "doc") to FOREACH_NODE_STATE_FLAG
//   2. Auto-generated NODE_STATE_FLAG_<NAME> bit position + MASK_NODE_STATE_<NAME> constant
//   3. Migrate writer sites: ctx.NAME = 1 → NODE_STATE_FLAG_SET(ctx, NAME)
//   4. Migrate reader sites: if (ctx.NAME) → NODE_STATE_FLAG_IS_SET(ctx, NAME)
//
// Cross-references (the BITMAP_* API / X-macro registry disciplines +
// the uint16_t Portfolio bitmap precedent):
//   DESIGN_SPECS/bitmap-flag-api.md (6th application of bitmap-flag-api per /readiness Check 21)
//   DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md
//   cohort-audit rule per cfg-flag-eligibility-criteria.md (all 5 booleans audited together)
//======================================================================================================
#ifndef NODE_STATE_FLAG_REGISTRY_HPP
#define NODE_STATE_FLAG_REGISTRY_HPP

#include <stdint.h>
#include "BitmapMacros.hpp"  // BITMAP_* primitives (v5.14.8.A.0.b)

namespace tt {

//======================================================================
// [REGISTRY]_[FOREACH_NODE_STATE_FLAG]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BITMAP_PACKED] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[6 BIT_FLAG rows -> sequential NODE_STATE_FLAG_<name> enum bits + MASK_NODE_STATE_<name> uint8_t constants; 2 bits headroom (overflow assert at 8)]
// [COLUMN]_[name]_[UPPERCASE token; produces the NODE_STATE_FLAG_<name> bit + MASK_NODE_STATE_<name> constant]
// [COLUMN]_[doc_string]_[human-readable description for audits + docs]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_NODE_STATE_FLAG(X)                                                                  \
    /* v4.0 pending_params dirty bit. Single-threaded set by RebuildOneCore on cycle close;       */ \
    /* cleared by PushParameters_OneCore after pushing the new param block. Branchless gate at    */ \
    /* PushParameters_OneCore entry: BITMAP_IS_SET → skip push if 0. */                              \
    X(DIRTY,                                                                                          \
      "pending_params has been freshly built and not yet pushed to ExecutionCore")                    \
    /* v3.x kill switch trip. Set by per-core drawdown gate; entries zero-gated with HALT_NODE_KILL  */ \
    /* while bit is set. Open positions ride to TP/SL (we don't force-close on trip; kill blocks    */ \
    /* NEW trades only). Operator can manually reset via TUISharedState::kill_reset_per_node[N].    */ \
    X(KILL_TRIPPED,                                                                                   \
      "per-core kill switch active; entries zero-gated until manual reset clears")                    \
    /* v5.9.0b ML model load refusal. Set by EngineSharded boot OR hot-swap path when NodeModelZoo  */ \
    /* allocation fails or strict-mode verify mismatches. ML strategy falls back to SimpleDip-like  */ \
    /* behavior when set; operator sees the flag in ML Status panel + entry log. (.B.2 → DisplayMeta */ \
    /* temporarily; .B.3 migrates back to NodeContext as a bit.) */                                  \
    X(MODEL_LOAD_FAILED,                                                                              \
      "ML model attempted but refused/missing — treated as no-model-loaded for entry decisions")      \
    /* v5.9.5i cfg-drift strict-mode refusal. Set by NodeModelZoo_ValidateAgainstCfg when Tier 1   */ \
    /* mismatch in strict mode. ML decisions are blocked when set (entries zero-gated; operator    */ \
    /* must reconcile cfg vs stamp or set acknowledge bit + restart). (.B.2 → DisplayMeta; .B.3   */ \
    /* migrates back to NodeContext as a bit.) */                                                    \
    X(CFG_DRIFT_STRICT_REFUSED,                                                                       \
      "cfg-drift strict-mode refusal active for this core; entries blocked until cfg reconciled")     \
    /* v5.9.1 boot-time per-core warmup-complete log edge-trigger. Set ONCE per session per core   */ \
    /* after the first RebuildOneCore cycle observing rolling.count >= min_warmup_samples. Fires   */ \
    /* the log + sets bit so subsequent cycles don't re-emit. (.B.2 → DisplayMeta; .B.3 migrates  */ \
    /* back to NodeContext as a bit.) */                                                             \
    X(WARMUP_LOG_EMITTED,                                                                             \
      "boot-time per-core warmup-complete log has been emitted (edge-trigger)")                       \
    /* v5.15.5.E.0.10 A6 ingress — ML model BARRIER corruption refusal. Set by the per-core slow-   */ \
    /* path thread (single-writer; ordered AFTER the ensemble_handle ACQ_REL hot-swap) when the      */ \
    /* corrupt-arm ratio popcount(ezoo->corrupt_arms_mask)/primary_count > model_corrupt_shalt_ratio */ \
    /* (majority), OR all barrier-arms corrupt (empty barrier-zoo). Node SHALTs: NEW entries zero-    */ \
    /* gated (GATE_FLAG_BUY_BLOCKED + SHALT_MODEL_CORRUPT) + sticky retrain alert. DISTINCT from      */ \
    /* MODEL_LOAD_FAILED (missing→SimpleDip degrade): a corrupt artifact is more alarming than an     */ \
    /* absent one (D-221). Open positions ride their valid pre-corruption barriers (block-new-only). */ \
    X(MODEL_CORRUPT,                                                                                  \
      "ML model barrier failed ingress validation for the majority of arms — node refuses NEW trades (retrain)")

//------------------------------------------------------------------
// [SECTION]_[AUTO-GENERATED BIT POSITIONS + MASK CONSTANTS]
//------------------------------------------------------------------
#define X_GEN_NODE_STATE_BIT(name, doc) NODE_STATE_FLAG_##name,
enum NodeStateFlag {
    FOREACH_NODE_STATE_FLAG(X_GEN_NODE_STATE_BIT)
    NODE_STATE_FLAG_COUNT  // sentinel
};
#undef X_GEN_NODE_STATE_BIT

#define X_GEN_NODE_STATE_MASK(name, doc) \
    static constexpr uint8_t MASK_NODE_STATE_##name = BITMAP_BIT_U8(NODE_STATE_FLAG_##name);
FOREACH_NODE_STATE_FLAG(X_GEN_NODE_STATE_MASK)
#undef X_GEN_NODE_STATE_MASK

// [ASSERT]_[BITMAP_OVERFLOW]_[NODE_STATE_FLAG_COUNT <= 8]
static_assert(NODE_STATE_FLAG_COUNT <= 8,
              "node_state_flags is uint8_t; max 8 entries. Widen to uint16_t "
              "if adding a 9th — and update the field type on NodeContext.");

// Public count for tests (uses >= per /readiness Check 21).
#define X_GEN_NODE_STATE_COUNT_ONE(name, doc) +1
#define FOREACH_NODE_STATE_FLAG_COUNT (0 FOREACH_NODE_STATE_FLAG(X_GEN_NODE_STATE_COUNT_ONE))
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Tuple: X(name, doc_string)
//   name       — UPPERCASE token; produces NODE_STATE_FLAG_<name> bit position
//                + MASK_NODE_STATE_<name> uint8_t mask constant
//   doc_string — human-readable description for audits + docs
//
// 6 entries used; 2 bits headroom in uint8_t. Promote storage to uint16_t
// if/when a 9th entry needs adding (static_assert above catches overflow).
//======================================================================
// [END_REGISTRY]_[FOREACH_NODE_STATE_FLAG]
//======================================================================

}  // namespace tt

//----------------------------------------------------------------------
// [MACRO]_[NODE_STATE_FLAG_*]
// [TAG]_[[ENGINE] [BITMAP_PACKED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[ergonomic bare-name accessors over ctx.node_state_flags (mirror the STATE_FLAG_* shape) — IS_SET/SET/CLR/TOGGLE + BITMAP_ANY for multi-flag]
//----------------------------------------------------------------------
// Mirror PerNodeStateFlagsRegistry STATE_FLAG_* shape. Reads naturally:
//   Set:    NODE_STATE_FLAG_SET(ctx, DIRTY)
//   Clear:  NODE_STATE_FLAG_CLR(ctx, DIRTY)
//   Read:   NODE_STATE_FLAG_IS_SET(ctx, DIRTY)
//   Toggle: NODE_STATE_FLAG_TOGGLE(ctx, DIRTY)
//   Any-of: BITMAP_ANY(ctx.node_state_flags, tt::MASK_NODE_STATE_X | tt::MASK_NODE_STATE_Y)
//
// MASK_NODE_STATE_<name> lives in tt:: namespace; macros assume the tt::
// scope is visible at call sites (most callers already `using namespace tt;`).

#define NODE_STATE_FLAG_IS_SET(ctx, name) BITMAP_IS_SET((ctx).node_state_flags, tt::MASK_NODE_STATE_##name)
#define NODE_STATE_FLAG_SET(ctx, name)    BITMAP_SET((ctx).node_state_flags, tt::MASK_NODE_STATE_##name)
#define NODE_STATE_FLAG_CLR(ctx, name)    BITMAP_CLR((ctx).node_state_flags, tt::MASK_NODE_STATE_##name)
#define NODE_STATE_FLAG_TOGGLE(ctx, name) BITMAP_TOGGLE((ctx).node_state_flags, tt::MASK_NODE_STATE_##name)

#endif  // NODE_STATE_FLAG_REGISTRY_HPP
