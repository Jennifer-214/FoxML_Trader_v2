// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [CFG-DERIVED INFERENCE_CFG REGISTRY — v5.15.5.A.7]
//======================================================================================================
// X-macro registry for `inference_cfg_*` fields that are CFG-DERIVED (training-
// time cfg snapshot captured in stamp body for drift detection at load).
//
// CLOSES TECH_DEBT-037: cfg-derived inference_cfg_* fields live in
// FOREACH_STAMP_BOUND_MODEL_CONST (not FOREACH_STAMP_BOUND_CFG) for historical
// taxonomy reasons (v5.14.8.A.merged classification). STAMP_CFG_AUTOPOPULATE
// doesn't reach them; manual section 2a in StampHelper.hpp populated
// `inf.inference_cfg_<name>` from `cfg.<source>` for each — recurring Class 18
// mirror that drifted as new fields were added.
//
// This registry is the SINGLE SOURCE OF TRUTH for the cfg→inf mapping side
// of those entries. The companion macro `INFERENCE_CFG_AUTOPOPULATE(inf, cfg)`
// replaces the manual section 2a with one expansion — adding a new cfg-derived
// inference_cfg_* field becomes 2 registry rows (one here + one in MODEL_CONST
// for ModelHandle field generation / stamp emit / parse).
//
// PATTERN: 3rd application of autopopulate-pattern-for-production-caller-class.md
//   1. STAMP_CFG_AUTOPOPULATE (v5.14.1.E.E.B; FOREACH_STAMP_BOUND_CFG)
//   2. STAMP_MODEL_CONST_AUTOPOPULATE (v5.14.8.A.merged; QUARANTINED v5.15.3.A.1)
//   3. INFERENCE_CFG_AUTOPOPULATE (v5.15.5.A.7; this file)
//
// **Variant note:** This AUTOPOPULATE uses PREFIX-AWARE token-paste — the
// registry tuple stores BARE names (e.g., `confidence_threshold_scale`); the
// macro expands to `inf.inference_cfg_##name` (prefixed). Mirrors the
// MODEL_CONST registry's convention (wire keys + ModelHandle fields have the
// `inference_cfg_` prefix; StampInferenceCfgInputs struct fields use the bare
// name — historical asymmetry that the prefix-aware paste preserves).
//
// DOD PRINCIPLES (CLAUDE.md):
//   - Item 13: X-macro registry as standard pattern for multi-site additions
//   - Item 19: Structural fix preferred (closes Class 18 mirror at section 2a)
//   - Item 20: BITMAP_IS_SET for feature-gate predicates in `gate_when`
//   - Item 21: AUTOPOPULATE companion (3rd application of the pattern)
//
// CROSS-REFS:
//   - ML_Headers/StampBoundModelConstRegistry.hpp (sister: MODEL_CONST entries
//     with inference_cfg_<name> generate ModelHandle fields + stamp emit/parse)
//   - ML_Headers/StampHelper.hpp (consumer: INFERENCE_CFG_AUTOPOPULATE call
//     replaces manual section 2a at lines ~168-187)
//   - ML_Headers/CfgDriftCheckRegistry.hpp (downstream reader: drift checks
//     compare h->inference_cfg_<name> vs cfg.<source>)
//   - tick-trader-percore-workspace/DESIGN_SPECS/autopopulate-pattern-for-production-caller-class.md
//     (canonical pattern doc; 3rd application referenced)
//======================================================================================================
#ifndef CFG_DERIVED_INFERENCE_CFG_REGISTRY_HPP
#define CFG_DERIVED_INFERENCE_CFG_REGISTRY_HPP

#include <stdint.h>
#include "BitmapMacros.hpp"                       // BITMAP_IS_SET for gate_when predicates

//======================================================================================================
// [REGISTRY ENTRY SHAPE — 3-col tuple]
//======================================================================================================
// X(name, cfg_extraction_expr, gate_when)
//
//   name                  — BARE field name (no prefix). The AUTOPOPULATE macro
//                            expands to `inf.inference_cfg_##name` via prefix-aware
//                            token-paste. Mirrors the existing MODEL_CONST registry's
//                            wire-key convention (wire keys carry the prefix; struct
//                            field on StampInferenceCfgInputs uses bare name).
//   cfg_extraction_expr   — Expression evaluated at caller scope (vars `cfg` + `inf`
//                            in scope) to extract cfg-side value. Handles FPN_ToDouble
//                            + BITMAP_IS_SET + direct field reads uniformly. Must
//                            implicit-convert to the inf field's type (compiler
//                            enforces via `(decltype((inf).inference_cfg_##name))(expr)`).
//   gate_when             — Boolean expression at caller scope. When TRUE, populate;
//                            when FALSE, leave inf field at zero/default. Preserves
//                            the existing feature-gate semantics (e.g., bandit fields
//                            only populated when bandit_enabled; fees only when
//                            cost_gate_enabled). Set to literal `1` for always-populate.
//
// HOW AUTOPOPULATE COMPOSES:
//
//   #define X_INFERENCE_CFG_AUTOPOPULATE_ONE(name, cfg_expr, gate_when)                \
//       if (gate_when) {                                                                \
//           (inf).inference_cfg_##name =                                                \
//               (decltype((inf).inference_cfg_##name))(cfg_expr);                       \
//       }
//
//   #define INFERENCE_CFG_AUTOPOPULATE(inf, cfg)                                        \
//       do {                                                                            \
//           STAMP_SET((inf), inference_cfg);  /* set group flag */                      \
//           FOREACH_CFG_DERIVED_INFERENCE_CFG(X_INFERENCE_CFG_AUTOPOPULATE_ONE)         \
//       } while (0)
//
// Caller usage (replaces ~20 lines of manual mapping in StampHelper.hpp section 2a):
//
//   INFERENCE_CFG_AUTOPOPULATE(inf, cfg);  // populates all 11 fields with correct gating
//
// FORWARD-COMPAT: adding a new entry = 1 row here + 1 row in MODEL_CONST_POST_CFG.
// Both are X-macro registry adds; NO manual code. Section 2a never grows.
//======================================================================================================

#define FOREACH_CFG_DERIVED_INFERENCE_CFG(X)                                                                                                                                          \
    /* === inference_cfg group (4 fields; always populate when inference_cfg group flag set; gate_when=1) === */                                                                       \
    X(confidence_threshold_scale,         FPN_ToDouble(cfg.confidence_threshold_scale),                                            1)                                                  \
    X(barrier_gate_enabled,                (BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_BARRIER_GATE_ENABLED) ? 1 : 0),         1)                                                  \
    X(confidence_hard_block_threshold,    FPN_ToDouble(cfg.confidence_hard_block_threshold),                                       1)                                                  \
    X(held_out_fraction,                   FPN_ToDouble(cfg.held_out_fraction),                                                     1)                                                  \
    /* === bandit (1 field; gated by bandit_enabled) === */                                                                                                                            \
    X(bandit_blend_ratio,                  FPN_ToDouble(cfg.bandit_blend_ratio),                                                    BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_BANDIT_ENABLED)) \
    /* === fees (2 fields; gated by cost_gate_enabled) === */                                                                                                                          \
    X(fee_rate_maker,                      FPN_ToDouble(cfg.fee_rate_maker),                                                        BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_COST_GATE_ENABLED)) \
    X(fee_rate_taker,                      FPN_ToDouble(cfg.fee_rate_taker),                                                        BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_COST_GATE_ENABLED)) \
    /* === v5.15.5.A.7 PARITY-024 cohort (per-horizon barrier serving; gated by per_horizon_barrier_blend feature) === */                                                              \
    X(ml_tp_pct,                           FPN_ToDouble(cfg.ml_tp_pct),                                                             BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_PER_HORIZON_BARRIER_BLEND)) \
    X(ml_sl_pct,                           FPN_ToDouble(cfg.ml_sl_pct),                                                             BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_PER_HORIZON_BARRIER_BLEND)) \
    X(barrier_blend_mode,                  cfg.barrier_blend_mode,                                                                  BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_PER_HORIZON_BARRIER_BLEND)) \
    X(per_horizon_barrier_blend,           (BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_PER_HORIZON_BARRIER_BLEND) ? 1 : 0),         1)

//======================================================================================================
// [AUTOPOPULATE COMPANION MACRO — INFERENCE_CFG_AUTOPOPULATE]
//======================================================================================================
// Single-call populator for all cfg-derived inference_cfg_* fields. Replaces
// ~20 LOC of manual section 2a (StampHelper.hpp:168-187 pre-v5.15.5.A.7) with
// one X-macro expansion.
//
// Caller MUST name variables `inf` (StampInferenceCfgInputs &) and `cfg`
// (ControllerConfig<F> const&) exactly. The macro expands at preprocessor
// time; no runtime cost beyond per-entry gate_when branch + assignment.
//
// SIDE EFFECT: sets `inf.has_inference_cfg = 1` (group flag) so downstream
// stamp emit walks emit all 11 fields' wire-key lines. Forward-compat preserved:
// the existing 7 entries' MODEL_CONST registry rows (lines 280-301 of
// StampBoundModelConstRegistry.hpp) ALL use `inf->has_inference_cfg` as their
// emit_when gate; 4 new POST_CFG entries (lines ~445+ added v5.15.5.A.7) share
// the same group flag.

#define X_INFERENCE_CFG_AUTOPOPULATE_ONE(name, cfg_expr, gate_when)                                  \
    if (gate_when) {                                                                                 \
        (inf).inference_cfg_##name = (decltype((inf).inference_cfg_##name))(cfg_expr);               \
    }

#define INFERENCE_CFG_AUTOPOPULATE(inf, cfg)                                                          \
    do {                                                                                              \
        STAMP_SET((inf), inference_cfg);                                                              \
        FOREACH_CFG_DERIVED_INFERENCE_CFG(X_INFERENCE_CFG_AUTOPOPULATE_ONE)                           \
    } while (0)

//======================================================================================================
// [TEST INSTRUMENTATION]
//======================================================================================================
// Compile-time field count for tests + audit. Used by tests to assert the
// registry matches the MODEL_CONST inference_cfg_* entries:
//
//   FOREACH_CFG_DERIVED_INFERENCE_CFG_COUNT should equal the count of
//   `inference_cfg_*` entries in FOREACH_STAMP_BOUND_MODEL_CONST. Maintains
//   "every cfg-derived MODEL_CONST entry has a cfg→inf mapping" invariant.

#define CFG_DERIVED_INFERENCE_CFG_COUNT_ONE(name, cfg_expr, gate_when) +1
#define FOREACH_CFG_DERIVED_INFERENCE_CFG_COUNT  (0 FOREACH_CFG_DERIVED_INFERENCE_CFG(CFG_DERIVED_INFERENCE_CFG_COUNT_ONE))

#endif // CFG_DERIVED_INFERENCE_CFG_REGISTRY_HPP
