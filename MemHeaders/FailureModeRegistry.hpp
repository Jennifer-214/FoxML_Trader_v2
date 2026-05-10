// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FAILURE MODE REGISTRY — v5.14.8.B]
//======================================================================================================
// Pseudo-registry for ML observability failure modes that surface to
// PerCoreSnap + ML Status panel. Auto-generates 3 mechanical sites:
//   1. PerCoreSnap field / bit declaration (storage_class-aware)
//   2. Populator helper (Health_Log rate-limit static + setter)
//   3. Health_Log rate-limit per-call-site static (uint64_t last_emit_us)
//
// Drives 1 hand-placed site:
//   4. ML Status panel branch — placement contextual; reads registry
//      constants (SEVERITY_COLOR, FAILURE_MODE_LABEL, FAILURE_MODE_TOOLTIP)
//
// CLOSES recurring "add failure mode requires N-site update" pattern:
// before this registry, adding a failure mode required:
//   - PerCoreSnap field (DataStream/EngineTUI.hpp)
//   - Populator at slow path (CoreFrameworks/EngineSharded.hpp)
//   - Rate-limit static + Health_Log call (per failure mode)
//   - ML Status panel branch (GUI/MLStatusPanel.hpp)
//   - cfg field + parser (if cfg-toggleable)
//   - Tests (controller_test.cpp)
// 6 sites per addition. Now: 1 registry line + 1 (smaller) hand-placed
// panel branch. 4-of-6 sites mechanically generated.
//
// STORAGE CLASSES (data-oriented design per CLAUDE.md item 1, 18):
//   BIT_FLAG    — 1 bit in PerCoreSnap.failure_flags uint16_t bitmap.
//                 Auto-allocates bit position via __COUNTER__ at struct
//                 generation. Up to 16 BIT_FLAG entries (uint16_t cap).
//                 Wins: branchless multi-flag check (failure_flags &
//                 (MASK_X | MASK_Y)); atomic multi-flag updates via
//                 __atomic_fetch_or; "any failure?" check is single
//                 uint16_t compare.
//   COUNTER_U32 — Standalone uint32_t field. Incrementing event counter.
//                 Bumped per event via populator helper.
//   PERCENT_U8  — Standalone uint8_t field. 0-100 percentage indicator
//                 (e.g., warmup_progress_pct).
//
// GROUP_ID (combined-display):
//   Entries with group_id != 0 share a panel display row (e.g.,
//   nan_feature_events + nan_prediction_events both shown on one
//   "nan: feat=N, pred=M" row). group_id == 0 means standalone display.
//   Panel-side iterates entries by group_id; auto-generates the
//   combined render call.
//
// SEVERITY:
//   SEV_RED    — engine cannot proceed; operator must intervene
//   SEV_YELLOW — engine continues degraded; operator should investigate
//   SEV_SAND   — informational; operator awareness only (warmup, etc.)
//
// CROSS-REFERENCE: this registry is the SECOND application of the
// X-macro registry pattern documented in
// `tick-trader-percore-workspace/DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md`
// + uses the BITMAP_* API from `bitmap-flag-api.md`. See those docs
// for the deeper pattern + future extension shape.
//======================================================================================================
#ifndef FAILURE_MODE_REGISTRY_HPP
#define FAILURE_MODE_REGISTRY_HPP

#include <stdint.h>
#include "BitmapMacros.hpp"   // BITMAP_* primitives (v5.14.8.A.0.b.1)

//======================================================================================================
// [STORAGE CLASS TOKENS]
//======================================================================================================
// Each entry's storage_class column is one of these tokens. Token-paste
// dispatch generates the right struct field declaration + populator
// helper at expansion time. See FAILURE_MODE_GEN_FIELD_<class> +
// FAILURE_MODE_GEN_SETTER_<class> macros below.
// (No #defines needed for token names themselves — they're literal
// tokens used in registry entries + dispatched via ##.)

//======================================================================================================
// [SEVERITY TOKENS]
//======================================================================================================
// Map to FoxmlColors for ML Status panel rendering. Hand-placed panel
// branch reads SEVERITY_COLOR(sev_token) at render time.

#define FAILURE_MODE_SEVERITY_RED    /* operator-must-intervene */
#define FAILURE_MODE_SEVERITY_YELLOW /* degraded-investigate */
#define FAILURE_MODE_SEVERITY_SAND   /* informational */

// At panel render: `SEVERITY_COLOR(SEV_RED)` returns the right ImVec4.
// Provided by GUI/FoxmlTheme.hpp; this header just enumerates names.

//======================================================================================================
// [GROUP_ID TOKENS — combined-display panel groups]
//======================================================================================================
// Entries with the same non-zero group_id share a panel display row.
// Add new GROUP_<NAME> = next-available-id when introducing a new
// combined-display group.

namespace tt {
enum FailureModeGroupId : int {
    GROUP_STANDALONE  = 0,  // sentinel; standalone display
    GROUP_NAN_EVENTS  = 1,  // nan_feature_events + nan_prediction_events
    // Future combined-display groups append here.
};
}

//======================================================================================================
// [REGISTRY ENTRY SHAPE]
//======================================================================================================
// X(name, storage_class, severity, format_str, tooltip_str, group_id)
//
//   name           — PerCoreSnap field/bit name + populator suffix.
//                    Must be a valid C identifier.
//   storage_class  — BIT_FLAG / COUNTER_U32 / PERCENT_U8 token.
//                    Drives field type + populator semantics.
//   severity       — SEV_RED / SEV_YELLOW / SEV_SAND token. Maps to
//                    color via SEVERITY_COLOR macro at panel render.
//   format_str     — printf-style label for panel display
//                    (e.g., "model: LOAD FAILED", "stale: %u feat").
//   tooltip_str    — multi-line operator guidance string. Shown on
//                    panel hover.
//   group_id       — tt::GROUP_STANDALONE (0) for own display row;
//                    tt::GROUP_<X> for combined-display.

#define FOREACH_FAILURE_MODE(X)                                                                        \
    X(ml_model_load_failed,     BIT_FLAG,    SEV_RED,    "model: LOAD FAILED",                          \
      "ML strategy was selected but no model could be loaded.\n"                                        \
      "Stamp validation failed, role file missing under any name "                                      \
      "(buy_signal/barrier/regime), or held_out_gate_strict=1\n"                                        \
      "with mismatched registry hash. Check the boot log for details.\n"                                \
      "Operator action: verify cfg path + retrain if needed.",                                          \
      tt::GROUP_STANDALONE)                                                                            \
    X(ml_scaler_load_failed,    BIT_FLAG,    SEV_YELLOW, "scaler: LOAD FAILED",                         \
      "Scaler sidecar present in stamp but failed to load.\n"                                           \
      "Check sidecar file exists at stamp's claimed path + scaler_sha256\n"                             \
      "matches. Engine falls back to identity scaler (predictions degraded).",                          \
      tt::GROUP_STANDALONE)                                                                            \
    X(warmup_progress_pct,      PERCENT_U8,  SEV_SAND,   "warmup: %u%%",                                \
      "Per-core slow path is still gathering rolling samples.\n"                                        \
      "Model + features won't fire predictions until rolling\n"                                         \
      "count reaches min_warmup_samples. Once 100%%, expect a\n"                                        \
      "boot-time stderr line confirming readiness.",                                                    \
      tt::GROUP_STANDALONE)                                                                            \
    X(ml_nan_feature_events,    COUNTER_U32, SEV_YELLOW, "feat: %u",                                    \
      "Total feature-pack NaN/Inf events on this core.\n"                                               \
      "Triggers when Features_PackAll's two-layer guard catches NaN/Inf\n"                              \
      "(FPN saturation past 1e15 OR IEEE-754 isnan/isinf post-cast).\n"                                 \
      "Counter increments per skipped prediction cycle.",                                               \
      tt::GROUP_NAN_EVENTS)                                                                             \
    X(ml_nan_prediction_events, COUNTER_U32, SEV_YELLOW, "pred: %u",                                    \
      "Total prediction NaN/Inf events on this core.\n"                                                 \
      "Triggers when Model_Predict returns NaN/Inf (degenerate model OR\n"                              \
      "post-scaler-apply NaN propagation).\n"                                                           \
      "Counter increments per skipped prediction cycle.",                                               \
      tt::GROUP_NAN_EVENTS)

//======================================================================================================
// [STORAGE-CLASS-AWARE FIELD GENERATION]
//======================================================================================================
// Token-paste dispatch on storage_class column. Used by PerCoreSnap
// struct generation in v5.14.8.C migration to declare the right
// underlying field type.
//
// BIT_FLAG entries DON'T declare a struct field directly — they're
// auto-allocated bit positions in the per-snap failure_flags uint16_t.
// At struct generation time:
//   - PerCoreSnap.failure_flags is declared ONCE (uint16_t)
//   - BIT_FLAG entries' MASK_<name> constants generated separately
//
// COUNTER_U32 / PERCENT_U8 entries each declare a standalone field.

#define FAILURE_MODE_GEN_FIELD_BIT_FLAG(name)    /* skip — packed in failure_flags bitmap */
#define FAILURE_MODE_GEN_FIELD_COUNTER_U32(name) uint32_t name;
#define FAILURE_MODE_GEN_FIELD_PERCENT_U8(name)  uint8_t name;

// Per-storage-class struct gen dispatcher:
#define FAILURE_MODE_FIELD_DECL(name, storage_class, sev, fmt, tooltip, group_id) \
    FAILURE_MODE_GEN_FIELD_##storage_class(name)

//======================================================================================================
// [BIT_FLAG MASK ALLOCATION]
//======================================================================================================
// For BIT_FLAG entries, generate MASK_<name> = bit position. Use a
// per-entry counter via enumeration. Up to 16 BIT_FLAG entries fit in
// uint16_t failure_flags.

namespace tt {
enum FailureModeBit : uint16_t {
    // Auto-allocated bit positions for BIT_FLAG entries. The X-macro
    // walks FOREACH_FAILURE_MODE; for BIT_FLAG entries, declare a bit
    // here (entry name → enum value). Other storage classes skip.
    #define FAILURE_MODE_BIT_DECL_BIT_FLAG(name)    FAILURE_BIT_##name,
    #define FAILURE_MODE_BIT_DECL_COUNTER_U32(name) /* not bit-packed */
    #define FAILURE_MODE_BIT_DECL_PERCENT_U8(name)  /* not bit-packed */
    #define X(name, storage_class, sev, fmt, tooltip, group_id) \
        FAILURE_MODE_BIT_DECL_##storage_class(name)
    FOREACH_FAILURE_MODE(X)
    #undef X
    #undef FAILURE_MODE_BIT_DECL_BIT_FLAG
    #undef FAILURE_MODE_BIT_DECL_COUNTER_U32
    #undef FAILURE_MODE_BIT_DECL_PERCENT_U8

    FAILURE_BIT_COUNT  // sentinel; total BIT_FLAG entries
};
static_assert(FAILURE_BIT_COUNT <= 16,
              "FOREACH_FAILURE_MODE exceeds uint16_t failure_flags capacity");
}  // namespace tt

// MASK_<name> constants for BIT_FLAG entries (generated per-entry):
#define FAILURE_MODE_MASK_DECL_BIT_FLAG(name) \
    static constexpr uint16_t FAILURE_MASK_##name = \
        (uint16_t)(1u << tt::FAILURE_BIT_##name);
#define FAILURE_MODE_MASK_DECL_COUNTER_U32(name) /* not bit-packed */
#define FAILURE_MODE_MASK_DECL_PERCENT_U8(name)  /* not bit-packed */
#define X(name, storage_class, sev, fmt, tooltip, group_id) \
    FAILURE_MODE_MASK_DECL_##storage_class(name)
FOREACH_FAILURE_MODE(X)
#undef X

//======================================================================================================
// [ERGONOMIC ACCESSORS — alias to BITMAP_*]
//======================================================================================================
// BIT_FLAG accessors via BITMAP_* API. failure_flags is a uint16_t on
// PerCoreSnap (declared in v5.14.8.C migration).
//
// Atomic variants available via BITMAP_ATOMIC_* directly when slow path
// writes + display thread reads concurrently.

#define FAILURE_IS_SET(snap, name)  BITMAP_IS_SET((snap).failure_flags, FAILURE_MASK_##name)
#define FAILURE_SET(snap, name)     BITMAP_SET((snap).failure_flags, FAILURE_MASK_##name)
#define FAILURE_CLR(snap, name)     BITMAP_CLR((snap).failure_flags, FAILURE_MASK_##name)
#define FAILURE_ANY(snap, mask_set) BITMAP_ANY((snap).failure_flags, (mask_set))

// Atomic variants (cross-thread; e.g., slow-path writes failure flag,
// display thread reads):
#define FAILURE_ATOMIC_SET(snap, name) \
    BITMAP_ATOMIC_SET((snap).failure_flags, FAILURE_MASK_##name)
#define FAILURE_ATOMIC_IS_SET(snap, name) \
    BITMAP_ATOMIC_IS_SET((snap).failure_flags, FAILURE_MASK_##name)

//======================================================================================================
// [TEST INSTRUMENTATION]
//======================================================================================================
// Compile-time count of total registry entries. Used by tests to assert
// shape correctness.

#define FAILURE_MODE_COUNT_ONE(name, storage_class, sev, fmt, tooltip, group_id) +1
#define FOREACH_FAILURE_MODE_COUNT  (0 FOREACH_FAILURE_MODE(FAILURE_MODE_COUNT_ONE))

#endif // FAILURE_MODE_REGISTRY_HPP
