// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/DisplayMetaRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [FRAMEWORK_DISCIPLINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the NodeContextDisplayMeta field SSoTs — gate-diagnostic actual/threshold pairs + heterogeneous display-only counters; one row auto-flows struct/init/publisher/GUI]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_GATE_DIAG_PAIR]
//   - [REGISTRY]_[FOREACH_DISPLAY_META_FIELD]   (both registries' count asserts ride this block — file-bottom placement)
// [REFERENCE]_[DESIGN_SPEC]_[display-execution-invariant-registry-pattern]
// [REFERENCE]_[CLASS]_[18]
//======================================================================================================
// X-macro registries for `NodeContextDisplayMeta<F>` fields. Closes the
// Display↔Execution Invariant Class-18 mirror class
// at the structural level: adding a new display-only diagnostic / counter
// is ONE row in this registry; all downstream sites (struct decl + init +
// snapshot publisher reads + PerNodeSnap field decls when applicable +
// GUI render rows when applicable) auto-flow.
//
// Pattern documented in
// `tick-trader-percore-workspace/DESIGN_SPECS/display-execution-invariant-registry-pattern.md`
// (ND2 first explicit reference: this file + the v5.15.5.B.2 ship).
//
// Two registries (mirror the natural type heterogeneity):
//
//   FOREACH_GATE_DIAG_PAIR(X)
//     6 entries producing 12 FPN_Binary<F> fields total — each entry generates an
//     `actual` field + a `threshold/floor/min` field. Field-name suffixes
//     are encoded per-entry to preserve the v5.6.3+ naming convention
//     (spacing_actual/_floor, vwap_actual/_threshold, long_slope/_min, etc.)
//
//   FOREACH_DISPLAY_META_FIELD(X)
//     Heterogeneous (uint8/16/32/64, double, int) display-only counters +
//     edge-trigger states + cfg-drift counters. Per-entry TYPE + INIT_VALUE
//     mirrors the FOREACH_FAILURE_MODE storage-class pattern.
//
// (`slow_path_latency` + `slow_path_breakdown[SP_SECTION_COUNT]` are NOT in
// either registry — they're NodeLatencyStats arrays with their own Init/
// Enable helpers + alignas(64) discipline; better to special-case than to
// shoehorn into a uniform registry shape.)
//
// Cross-references (the Display↔Execution invariant / X-macro registry /
// structural-fix-preferred / AUTOPOPULATE-companion disciplines):
//   DESIGN_SPECS/display-execution-invariant-registry-pattern.md (ND2)
//   DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md
//   DESIGN_SPECS/autopopulate-pattern-for-production-caller-class.md
//   DOCS/EXECUTION_DISPLAY_INVARIANTS.md
//======================================================================================================
#pragma once

#include <cstdint>

namespace tt {

//======================================================================
// [REGISTRY]_[FOREACH_GATE_DIAG_PAIR]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [FRAMEWORK_DISCIPLINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[6 gate-diagnostic actual/threshold pairs -> 12 FPN_Binary fields on NodeContextDisplayMeta; suffixes preserve the v5.6.3+ naming]
// [COLUMN]_[FAMILY]_[registry constant suffix (UPPER_SNAKE)]
// [COLUMN]_[ACTUAL_FIELD]_[lowercase field name for the actual side (becomes diag_<name>)]
// [COLUMN]_[OTHER_FIELD]_[lowercase field name for the threshold/floor/min side]
// [COLUMN]_[DOC]_[human description (tooltips, log header)]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_GATE_DIAG_PAIR(X)                                                                  \
    X(SPACING,      spacing_actual,    spacing_floor,     "|bg_threshold - last_entry| vs stddev × spacing_multiplier") \
    X(VWAP,         vwap_actual,       vwap_threshold,    "bg_price_threshold vs VWAP - VWAP × vwap_offset")             \
    X(LONG_SLOPE,   long_slope,        long_slope_min,    "long_rel_slope vs cfg.min_long_slope")                         \
    X(VOLUME_DELTA, volume_delta,      volume_delta_min,  "rolling.volume_delta vs cfg.min_buy_delta")                    \
    X(STDDEV_PCT,   stddev_pct,        stddev_pct_min,    "rolling.price_stddev / rolling.price_avg vs cfg.min_stddev_pct") \
    X(TP_PCT,       tp_pct_actual,     tp_pct_floor,      "out.tp_pct vs 3 × fee_rate_taker")
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Tuple shape: (FAMILY, ACTUAL_FIELD, OTHER_FIELD, DOC)
//   FAMILY       — registry constant suffix (UPPER_SNAKE; used in future
//                  enum constants like GATE_DIAG_SPACING if needed)
//   ACTUAL_FIELD — full lowercase field name for the "actual" side
//                  (becomes `diag_<ACTUAL_FIELD>` on NodeContextDisplayMeta);
//                  preserves v5.6.3+ naming (some have _actual suffix, some
//                  don't, per the original gate semantics)
//   OTHER_FIELD  — full lowercase field name for the threshold/floor/min side
//                  (becomes `diag_<OTHER_FIELD>`)
//   DOC          — human description (shown in tooltips, log header, etc.)
//
// To add a 7th gate diagnostic: append one row here. The DisplayMeta struct,
// snapshot publisher reads, init, PerNodeSnap field decls (manual today),
// and GUI render block all auto-flow.
//======================================================================
// [END_REGISTRY]_[FOREACH_GATE_DIAG_PAIR]
//======================================================================

//======================================================================
// [REGISTRY]_[FOREACH_DISPLAY_META_FIELD]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [FRAMEWORK_DISCIPLINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[10 heterogeneous display-only fields (counters / edge-triggers / cfg-drift / the extracted drift_breach_first_us) — TYPE + INIT_VALUE per row, FOREACH_FAILURE_MODE storage-class shape]
// [COLUMN]_[type]_[C++ storage type on NodeContextDisplayMeta]
// [COLUMN]_[NAME]_[field name (snake_case)]
// [COLUMN]_[INIT_VALUE]_[default-initializer + reset-init value (v5.6.3+ EventLoopState_Init semantics)]
// [COLUMN]_[DOC]_[human description]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_DISPLAY_META_FIELD(X)                                                              \
    /* Health-log + edge-trigger state */                                                          \
    X(uint16_t,  prev_gate_log_state,         0xFFFF, "v5.6.6 packed gate-state for cat=\"gate\" health-log edge-trigger; sentinel 0xFFFF forces first-cycle baseline emit") \
    X(uint32_t,  barrier_shadow_event_count,  0,    "v5.15.5.A.6 shadow ring writes (barrier modes 3/4)")                          \
    /* ML rate-limit + decision-context counters */                                                \
    X(uint64_t,  last_ml_critical_log_us,     0,    "v5.9.0b rate-limit gate for ML→SimpleDip CRITICAL log (per-node)")            \
    X(double,    last_ml_threshold,           0.0,  "v5.9.0b ml_buy_threshold at last decision (display + entry log)")             \
    X(double,    last_ml_effective_threshold, 0.0,  "v5.9.0b post-confidence-damping threshold actually used")                     \
    X(uint32_t,  nan_feature_events_total,    0,    "v5.9.0b Features_PackAll -1 sentinel count on this node")                     \
    X(uint32_t,  nan_prediction_events_total, 0,    "v5.9.0b Model_Predict NaN/Inf events on this node")                           \
    /* Cfg-drift counter state (booleans moved to node_state_flags bitmap in v5.15.5.B.3) */       \
    X(uint8_t,   cfg_drift_tier1_count,       0,    "v5.9.5i cfg drift Tier 1 count")                                              \
    X(uint8_t,   cfg_drift_tier2_count,       0,    "v5.9.5i cfg drift Tier 2 count")                                              \
    /* v5.15.5.E.B: drift_history.breach_first_us EXTRACTED here per cache-layout-discipline Rule 1. */                            \
    /* Write-only currently (set at first-breach detection in ControllerEventLoop; never read).      */                            \
    /* Preserved for future GUI panel display + observability. Closes Class-18 mirror — would be     */                            \
    /* a 3rd DisplayMeta sibling-struct creation if not unified here.                                */                            \
    X(uint64_t,  drift_breach_first_us,       0,    "v5.15.5.E.B wall-clock at first drift breach detection on this node")
    /* v5.15.5.B.3: model_load_failed, cfg_drift_strict_refused, warmup_log_emitted */                                             \
    /* migrated to node_state_flags bitmap on NodeContext (NodeStateFlagRegistry.hpp).             */                              \
    /* Final home — closes the Class 18 mirror they represented. */

//------------------------------------------------------------------
// [SECTION]_[REGISTRY ENTRY COUNTS — both registries; test instrumentation]
//------------------------------------------------------------------
// Count of entries — useful for static_asserts + iteration helpers
#define FOREACH_GATE_DIAG_PAIR_X_COUNT(...) 1+
constexpr int GATE_DIAG_PAIR_COUNT = FOREACH_GATE_DIAG_PAIR(FOREACH_GATE_DIAG_PAIR_X_COUNT) 0;
#undef FOREACH_GATE_DIAG_PAIR_X_COUNT
// [ASSERT]_[REGISTRY_COVERAGE]_[GATE_DIAG_PAIR_COUNT == 6]
static_assert(GATE_DIAG_PAIR_COUNT == 6,
              "FOREACH_GATE_DIAG_PAIR is expected to have exactly 6 entries; "
              "adjust this assert when adding/removing entries (the count "
              "informs cluster-size math + downstream registry consumers).");

#define FOREACH_DISPLAY_META_FIELD_X_COUNT(...) 1+
constexpr int DISPLAY_META_FIELD_COUNT = FOREACH_DISPLAY_META_FIELD(FOREACH_DISPLAY_META_FIELD_X_COUNT) 0;
#undef FOREACH_DISPLAY_META_FIELD_X_COUNT
// [ASSERT]_[REGISTRY_COVERAGE]_[DISPLAY_META_FIELD_COUNT == 10]
static_assert(DISPLAY_META_FIELD_COUNT == 10,
              "FOREACH_DISPLAY_META_FIELD is expected to have exactly 10 entries "
              "at v5.15.5.E.B ship (was 9 at .B.3; +1 drift_breach_first_us "
              "extracted from DriftHistory per Rule 1 display-only extraction).");
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Tuple shape: (TYPE, NAME, INIT_VALUE, DOC)
//   TYPE       — C++ type (uint16_t / uint32_t / uint64_t / double / int /
//                uint8_t etc.) for the field on NodeContextDisplayMeta
//   NAME       — field name (lowercase snake_case)
//   INIT_VALUE — value used in struct default-initializer + reset-init
//                helper; matches the v5.6.3+ EventLoopState_Init semantics
//   DOC        — human description (for header docs)
//
// Mirror pattern from FOREACH_FAILURE_MODE per-entry storage class
// (v5.14.8.B). Future fields: append one row.
//
// Per the structural-fix-preferred gradient, some boolean entries were
// FLAGGED for .B.3 migration → uint8_t node_state_flags bitmap on
// NodeContext. .B.3 SHIPPED: those rows (model_load_failed /
// cfg_drift_strict_refused / warmup_log_emitted) were removed from this
// registry; the bitmap on NodeContext (NodeStateFlagRegistry.hpp) is
// their final home — see the trailer note above the count section.
//======================================================================
// [END_REGISTRY]_[FOREACH_DISPLAY_META_FIELD]
//======================================================================

}  // namespace tt
