// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/PerNodeStateFlagsRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BITMAP_PACKED] [MONITORING_PLANE] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[PerNodeSnap uint16_t state_flags SSoT — 11 pure BIT_FLAG observability rows; one row auto-flows the STATE_FLAG_<name> bit + MASK_<name> constant]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_PER_NODE_STATE_FLAG]   (auto-gen enum/masks + overflow assert + count macro ride the block)
//   - [MACRO]_[STATE_FLAG_*]
// [REFERENCE]_[DESIGN_SPEC]_[bitmap-flag-api]
// [REFERENCE]_[INVARIANT]_[H14]
//======================================================================================================
// Bit-packed observability state for PerNodeSnap. Distinct from the
// FOREACH_FAILURE_MODE registry (FailureModeRegistry.hpp) which carries
// ML failure modes with severity + grouping + COUNTER_U32 + PERCENT_U8
// storage classes. This registry is simpler: pure BIT_FLAG entries for
// non-failure boolean state (permission gates, display-execution
// invariants, observability flags).
//
// Auto-generates 2 mechanical sites:
//   1. enum STATE_FLAG_<NAME> bit position (sequential allocation)
//   2. MASK_<NAME> uint16_t mask constant
//
// Drives 2 hand-placed sites:
//   3. PerNodeSnap struct field (uint16_t state_flags) — declared once
//   4. Setter / reader sites (writer toggles via BITMAP_SET/CLR/IS_SET;
//      readers via BITMAP_IS_SET — placement contextual per writer)
//
// CLOSES TECH_DEBT-013 candidate (3) — PerNodeSnap non-failure state flags.
// Pre-registry: 6 separate uint8_t fields (~6 bytes per snap × 16 cores
// = 96 bytes per engine). Post-registry: 1 uint16_t (2 bytes × 16 cores
// = 32 bytes). 3× shrink + cache-locality win + branchless multi-flag check.
//
// Pattern: per the BITMAP_* universalization discipline +
// DESIGN_SPECS/bitmap-flag-api.md. Same shape as FailureModeRegistry's
// BIT_FLAG storage class but stripped of severity/grouping/mixed storage
// (this domain doesn't need them).
//
// Adding a new flag (1 row):
//   1. Append X(NAME, "doc") to FOREACH_PER_NODE_STATE_FLAG
//   2. Auto-generated STATE_FLAG_<NAME> bit + MASK_<NAME> constant
//   3. Add setter site at the writer (slow path / snapshot copy)
//   4. Add reader sites where consumers check the bit
//   5. Tests for set/clear/check
//
// Single-threaded access (per-core slow-path writes; GUI display reads via
// double-buffered TUISnapshot — separate publish lifecycle so no atomic
// needed inside the per-core write window).
//======================================================================================================

#ifndef PER_NODE_STATE_FLAGS_REGISTRY_HPP
#define PER_NODE_STATE_FLAGS_REGISTRY_HPP

#include <stdint.h>
#include "BitmapMacros.hpp"   // BITMAP_* API (v5.14.8.A.0.b.1)

namespace tt {

//======================================================================
// [REGISTRY]_[FOREACH_PER_NODE_STATE_FLAG]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BITMAP_PACKED] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[11 BIT_FLAG rows -> sequential STATE_FLAG_<name> enum bits + MASK_<name> uint16_t constants; 5 bits headroom (overflow assert at 16)]
// [COLUMN]_[name]_[UPPERCASE token; produces the STATE_FLAG_<name> bit + MASK_<name> constant]
// [COLUMN]_[doc_string]_[human-readable description for audits + cfg.example]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_PER_NODE_STATE_FLAG(X)                                                              \
    /* Migrated from PerNodeSnap.permission (v5.6.1) — 0=forbidden, 1=allowed */                   \
    X(PERMISSION_ALLOWED,                                                                            \
      "entries allowed for this core (post-warmup; no kill-switch trip)")                           \
    /* Migrated from PerNodeSnap.bitmap_consistency (v5.6.1) — display↔execution invariant */      \
    X(BITMAP_CONSISTENT,                                                                             \
      "(positions[i].idx >= 0) == (active|active_b) — display↔execution invariant")                 \
    /* Migrated from PerNodeSnap.gate_direction — 0=buy below, 1=buy above */                       \
    X(GATE_BUY_ABOVE,                                                                                \
      "buy direction: bit set = buy above (MOM); bit clear = buy below (MR/DIP/EMA/ML)")            \
    /* Migrated from PerNodeSnap.is_ml — 1=STRATEGY_ML core */                                      \
    X(IS_ML,                                                                                         \
      "STRATEGY_ML core with ML extras valid (ml_* fields populated)")                              \
    /* Migrated from PerNodeSnap.ml_model_loaded — 1=zoo has at least one role loaded */            \
    X(ML_MODEL_LOADED,                                                                               \
      "zoo has at least one role loaded; non-zero prediction expected")                             \
    /* Migrated from PerNodeSnap.strategy_was_explicit_set (v5.9.0c) — cfg explicit-set bit */     \
    X(STRATEGY_EXPLICITLY_SET,                                                                       \
      "operator set node_N_strategy= explicitly in cfg (vs defaulted)")                             \
    /* v5.14.9.B.2 NEW — soft risk degradation ladder bottom hit this cycle */                      \
    X(LADDER_BOTTOM_HIT,                                                                             \
      "ladder factor=0 fired this cycle → entry blocked + SHALT_LOW_CONFIDENCE")                    \
    /* v5.15.1 — TECH_DEBT-028 close: 4 bool-as-uint8 PerNodeSnap fields                          */ \
    /* migrated to state_flags bitmap (matches cohort homogeneity rule;                            */ \
    /* per-snapshot-cluster-layout-pattern + bitmap-flag-api).                                     */ \
    X(ML_SCALER_PRESENT,                                                                             \
      "ANY zoo role has has_scaler=1 (aggregate; per-role granularity in handles)")                 \
    X(DRIFT_BREACHED,                                                                                \
      "drift_history.breached at snapshot time (composite confidence drift gate tripped)")          \
    X(DRIFT_KILL_TRIPPED,                                                                            \
      "drift-induced kill switch fired (auto_kill_on_drift cfg + breach persisted)")                \
    X(NODE_KILL_TRIPPED,                                                                             \
      "operator-driven core kill OR MTM-kill OR manual-kill active right now")

//------------------------------------------------------------------
// [SECTION]_[AUTO-GENERATED BIT POSITIONS + MASK CONSTANTS]
//------------------------------------------------------------------

#define X_GEN_STATE_FLAG_BIT(name, doc) STATE_FLAG_##name,
enum PerNodeStateFlag {
    FOREACH_PER_NODE_STATE_FLAG(X_GEN_STATE_FLAG_BIT)
    PER_NODE_STATE_FLAG_COUNT  // sentinel
};
#undef X_GEN_STATE_FLAG_BIT

#define X_GEN_STATE_FLAG_MASK(name, doc) \
    static constexpr uint16_t MASK_##name = BITMAP_BIT_U16(STATE_FLAG_##name);
FOREACH_PER_NODE_STATE_FLAG(X_GEN_STATE_FLAG_MASK)
#undef X_GEN_STATE_FLAG_MASK

// [ASSERT]_[BITMAP_OVERFLOW]_[PER_NODE_STATE_FLAG_COUNT <= 16]
static_assert(PER_NODE_STATE_FLAG_COUNT <= 16,
              "PerNodeSnap state_flags uint16_t exhausted; expand to uint32_t");

// Public count macro for tests (uses >= per /readiness Check 21).
#define X_GEN_STATE_FLAG_COUNT_ONE(name, doc) +1
#define FOREACH_PER_NODE_STATE_FLAG_COUNT (0 FOREACH_PER_NODE_STATE_FLAG(X_GEN_STATE_FLAG_COUNT_ONE))
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Tuple: X(name, doc_string)
//   name           — UPPERCASE token; produces STATE_FLAG_<name> bit + MASK_<name> constant
//   doc_string     — human-readable description for audits + cfg.example
//
// Names match the SEMANTIC ROLE of the flag, not the source field name
// it migrated from. E.g., the old `permission` uint8_t → MASK_PERMISSION_ALLOWED
// (positive form: bit set = entries allowed). Follow this convention for
// new flags so reader sites read naturally: `if (BITMAP_IS_SET(flags, MASK_X))`.
//======================================================================
// [END_REGISTRY]_[FOREACH_PER_NODE_STATE_FLAG]
//======================================================================

}  // namespace tt

//----------------------------------------------------------------------
// [MACRO]_[STATE_FLAG_*]
// [TAG]_[[ENGINE] [BITMAP_PACKED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[ergonomic bare-name accessors over snap.state_flags (mirror the FAILURE_IS_SET shape) — IS_SET/SET/CLR/TOGGLE + BITMAP_ANY for multi-flag]
//----------------------------------------------------------------------
// Same ergonomic pattern as FAILURE_IS_SET in FailureModeRegistry. Lets
// GUI + slow-path call sites use bare flag names without scope-prefixing.
//
// Read:    STATE_FLAG_IS_SET(snap, PERMISSION_ALLOWED)
// Set:     STATE_FLAG_SET   (snap, PERMISSION_ALLOWED)
// Clear:   STATE_FLAG_CLR   (snap, PERMISSION_ALLOWED)
// Toggle:  STATE_FLAG_TOGGLE(snap, PERMISSION_ALLOWED)
// Any-of:  BITMAP_ANY(snap.state_flags, tt::MASK_X | tt::MASK_Y)
//
// Note: MASK_<name> lives in tt:: namespace (auto-generated by registry).
// These macros assume the tt:: scope is visible at the call site (most
// callers already `using namespace tt;` for FAILURE_IS_SET sister macro).

#define STATE_FLAG_IS_SET(snap, name)  BITMAP_IS_SET((snap).state_flags, tt::MASK_##name)
#define STATE_FLAG_SET(snap, name)     BITMAP_SET((snap).state_flags, tt::MASK_##name)
#define STATE_FLAG_CLR(snap, name)     BITMAP_CLR((snap).state_flags, tt::MASK_##name)
#define STATE_FLAG_TOGGLE(snap, name)  BITMAP_TOGGLE((snap).state_flags, tt::MASK_##name)

#endif  // PER_NODE_STATE_FLAGS_REGISTRY_HPP
