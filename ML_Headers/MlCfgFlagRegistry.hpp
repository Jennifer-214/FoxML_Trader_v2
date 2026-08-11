// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/MlCfgFlagRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [BITMAP_PACKED] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ML domain cfg-flag registry (v5.14.9.F.2) — uint16_t bitmap (fastest-growing domain); Y3 stamp-emit dispatch for the stamp-bound bits]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_ML_CFG_FLAG]
//======================================================================================================
// Third domain registry. ML/confidence-mechanic boolean cfg flags. uint16_t bitmap on
// ControllerConfig (7 entries; uint8_t would fit but uint16_t for headroom — ML domain
// growing fastest per TECH_DEBT-021 post-paper-test profiling concern).
//
// Pattern: heterogeneous-registry-pattern.md DOMAIN SPLIT form (Form 2) +
//          HYBRID form (Form 3) for stamp-binding integration via Y3 dispatch.
// Cross-ref:
//   - DESIGN_SPECS/heterogeneous-registry-pattern.md (decision framework + Y3 dispatch canon + worked example)
//   - DESIGN_SPECS/bitmap-flag-api.md (BITMAP_* primitives)
//   - DOCS/TECH_DEBT.md TECH_DEBT-023 (cfg-flag-eligibility criteria)
//
// HIGH-RISK INTEGRATION (per plan + /dod-audit HIGH.2 — Y3 DISPATCH LOCKED THIS SHIP):
// `confidence_composite_enabled` is stamp-bound via FOREACH_STAMP_BOUND_CFG.
// .F.2 extends that registry from 6-col → 7-col with new `emit_source` column
// (DIRECT_FIELD vs BITMAP_BIT) + Y3 token-paste dispatch via HANDLE_STAMP_EMIT_*
// handlers per heterogeneous-registry-pattern.md Form 3 worked example.
//
// confidence_composite_enabled entry uses emit_source=BITMAP_BIT; its get_cfg
// expression reads `BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED)
// ? 1 : 0` — produces identical uint bytes (0 or 1) on the wire vs pre-migration
// direct field read, preserving HMAC chain byte-for-byte for existing trained
// stamps. All other entries marked DIRECT_FIELD (no behavior change). Round-trip
// HMAC test validates byte-equivalence load-bearingly.
//
// CFG-FLAG ELIGIBILITY (per TECH_DEBT-023): all 7 entries below pass all 5 criteria.
//
// CFG-FLAG ELIGIBILITY (per TECH_DEBT-023): all 7 entries below pass all 5 criteria.
//======================================================================================================
#ifndef ML_CFG_FLAG_REGISTRY_HPP
#define ML_CFG_FLAG_REGISTRY_HPP

#include <cstdint>
#include "../MemHeaders/BitmapMacros.hpp"

//======================================================================
// [REGISTRY]_[FOREACH_ML_CFG_FLAG]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [BITMAP_PACKED]]
// [REFERENCE]_[DESIGN_SPEC]_[[heterogeneous-registry-pattern] [bitmap-flag-api]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[6-col rows -> enum + MASK_ML_CFG_* + cohort gates + AUTOPOPULATE; 5 STAMP_BOUND_CFG_DERIVED rows in the .B.2 cohort]
// [COLUMN]_[NAME/legacy_field]_[uppercase token + the cfg key]
// [COLUMN]_[display_label/section]_[GUI]
// [COLUMN]_[metadata_flags]_[derived-filter cohort bits (H16)]
// [COLUMN]_[doc]_[audit string]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-100]
// [REFERENCE]_[MEMORY]_[feedback_categorical_triggers_over_hardcoded_refs]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_ML_CFG_FLAG(X)                                                                                                                                                                          \
    X(CONFIDENCE_ENABLED,           confidence_enabled,           "Confidence",            "FoxML",       0,                                                "scale entry threshold by confidence score")                                                  \
    X(CONFIDENCE_COMPOSITE_ENABLED, confidence_composite_enabled, "Composite Confidence",  "FoxML",       CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED,      "use 4-factor composite confidence (vs legacy 3-factor); stamp-bound")                         \
    X(BANDIT_ENABLED,               bandit_enabled,               "Bandit",                "FoxML",       0,                                                "Exp3-IX bandit for buy-signal arm selection (default; cfg.bandit_algorithm=1 swaps to Thompson)") \
    X(EXIT_BANDIT_ENABLED,          exit_bandit_enabled,          "Exit Bandit",           "FoxML",       0,                                                "Exp3-IX bandit for exit-side arm selection (sell-side; v5.13.4)")                              \
    X(USE_EXIT_MODEL,               use_exit_model,               "Use Exit Model",        "FoxML",       0,                                                "use dedicated exit-side ML model (vs entry model fallback)")                                 \
    X(FOXML_VOL_SCALING_ENABLED,    foxml_vol_scaling_enabled,    "Vol Scaling",           "FoxML",       0,                                                "scale trade size by recent volatility (FoxML VolScaler)")                                    \
    X(LAZY_REBUILD_ENABLED,         lazy_rebuild_enabled,         "Lazy Rebuild",          "Performance", 0,                                                "skip slow-path rebuild when no parameter inputs changed")                                    \
    X(RIDGE_WITHIN_HORIZON,         ridge_within_horizon,         "Ridge Within Horizon",  "ML/Ridge",    CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED,      "Ridge blend across role-arms within a horizon (v5.14.0; stamp-bound) [v5.14.11.C cohort migration from direct int]") \
    X(RIDGE_ACROSS_HORIZONS,        ridge_across_horizons,        "Ridge Across Horizons", "ML/Ridge",    CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED,      "Ridge blend across horizons (vs bandit selection); infrastructure-only until consumer ships [v5.14.11.C cohort migration]") \
    X(EXIT_BLENDER_MODE,            exit_blender_mode,            "Exit Blender Mode",     "ML/Ridge",    CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED,      "Ridge blend across exit_predictor handles (v5.14.1.E; stamp-bound) [v5.14.11.C cohort migration]") \
    X(RIDGE_ONLINE_CORR,            ridge_online_corr,            "Ridge Online Corr",     "ML/Ridge",    0,                                                "Use sliding-window incremental correlation matrix in Ridge (default 0=full recompute; v5.14.11)") \
    X(PER_HORIZON_BARRIER_BLEND,    per_horizon_barrier_blend,    "Per-Horizon Barriers",  "FoxML",       CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED,      "Enable per-horizon TP/SL serving via blend/dominant modes (vs cfg-direct LEGACY fallback); paired with cfg.barrier_blend_mode enum [v5.15.5.A.5]. .B.3 Step 1.6.2 cohort bit-add (TECH_DEBT-100 closure; STAMP_BOUND_CFG_DERIVED activates framework walker now that Step 1.6.3 Decision C Approach A struct-gen provides unprefixed inf.per_horizon_barrier_blend field).")

//------------------------------------------------------------------------------------------------------
// [SECTION]_[AUTO-GENERATED ENUM + COUNT]
//------------------------------------------------------------------------------------------------------
enum MlCfgFlag {
#define X_GEN_ML_CFG_BIT(name, legacy_field, display_label, section, metadata_flags, doc) ML_CFG_##name,
    FOREACH_ML_CFG_FLAG(X_GEN_ML_CFG_BIT)
    ML_CFG_COUNT
#undef X_GEN_ML_CFG_BIT
};

static_assert(ML_CFG_COUNT <= 16,
              "FOREACH_ML_CFG_FLAG exhausted uint16_t storage; expand cfg.ml_cfg_flags to uint32_t");

//------------------------------------------------------------------------------------------------------
// [SECTION]_[AUTO-GENERATED MASK_ML_CFG_<NAME> CONSTANTS]
//------------------------------------------------------------------------------------------------------
#define X_GEN_ML_CFG_MASK(name, legacy_field, display_label, section, metadata_flags, doc) \
    static constexpr uint16_t MASK_ML_CFG_##name = (uint16_t)(1u << ML_CFG_##name);
FOREACH_ML_CFG_FLAG(X_GEN_ML_CFG_MASK)
#undef X_GEN_ML_CFG_MASK

//------------------------------------------------------------------------------------------------------
// [SECTION]_[COHORT GATE MACROS — v5.15.5.F.4d.1.B.2 Step 5.0]
//------------------------------------------------------------------------------------------------------
// Shared cohort gate predicates across 3 registries:
//   - FOREACH_STAMP_BOUND_CFG col 5 (emit_when) at ML_Headers/StampBoundCfgRegistry.hpp
//   - FOREACH_CFG_DRIFT_CHECK col 8 (gate_when) at ML_Headers/CfgDriftCheckRegistry.hpp
//   - FOREACH_CFG_GATE_PER_NODE entries at MemHeaders/CfgGateRegistry.hpp
//
// Path γ #3 structural close (audit synthesis CRIT-CONV-5): 3-way drift surface where
// same conceptual cohort gates were encoded inline at multiple registries. Adding a new
// cohort = 1 new COHORT_GATE_* macro + 1 row per registry referencing it.
//
// Macros expand at consumer expansion time → `cfg` must be in scope (typed
// ControllerConfig<F>); MASK_ML_CFG_* + BITMAP_IS_SET/BITMAP_ANY already in scope via
// this file + MemHeaders/BitmapMacros.hpp.
//
// Semantics match legacy FOREACH_STAMP_BOUND_CFG emit_when (the wire-emit source of
// truth pre-.B.2). Names BANDIT_THOMPSON / BANDIT_BLEND_STATE_4 reflect actual
// semantics (not just "BANDIT_ENABLED"; legacy gate uses cfg.bandit_algorithm != 0
// which means "Thompson-class algorithm active" — distinct from MASK_ML_CFG_BANDIT_ENABLED
// flag which controls bandit selection wiring).

#define COHORT_GATE_BANDIT_THOMPSON       (cfg.bandit_algorithm != 0)
#define COHORT_GATE_BANDIT_BLEND_STATE_4  (cfg.bandit_algorithm == 4)
#define COHORT_GATE_RIDGE_ANY             BITMAP_ANY(cfg.ml_cfg_flags, MASK_ML_CFG_RIDGE_WITHIN_HORIZON | MASK_ML_CFG_RIDGE_ACROSS_HORIZONS)
#define COHORT_GATE_COMPOSITE_CONFIDENCE  BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED)
#define COHORT_GATE_SOFTRISK_ENABLED      (cfg.risk_degradation_curve != 0)
#define COHORT_GATE_PER_HORIZON_BARRIER   BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_PER_HORIZON_BARRIER_BLEND)
// v5.15.5.F.4d.1.B.3 Step 6.10 (2026-05-24) — 7-site inline-bitmap-gate extraction.
// Sister to existing 6 above; closes Path γ #3 MOSTLY (was PARTIAL pre-this-add) per
// audit finding HIGH-1 (opp-scan H-2 + readiness M1 + dod-audit HIGH-1 cross-flagged).
// Substituted at CfgDriftCheckRegistry.hpp lines 250/256/260/264/268 (5 BANDIT_ENABLED
// sites for bandit_blend_ratio + bandit_algorithm + thompson_mu_prior + thompson_precision_prior + thompson_precision_obs).
// Co-located here despite COST_GATE belonging at GateCfgFlagRegistry — kept all 8 COHORT_GATE_*
// adjacent for discoverability + per [[feedback_categorical_triggers_over_hardcoded_refs]] —
// the cohort-gate macro family is the canonical home; per-mask-registry split would scatter.
#define COHORT_GATE_BANDIT_ENABLED        BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_BANDIT_ENABLED)
#define COHORT_GATE_COST_GATE_ENABLED     BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_COST_GATE_ENABLED)

//------------------------------------------------------------------------------------------------------
// [SECTION]_[AUTOPOPULATE COMPANION]
//------------------------------------------------------------------------------------------------------
// Branchless OR-reduction; compiler emits cmov per row.

#define ML_CFG_FLAG_AUTOPOPULATE_FROM_SEPTUPLE(target_flags, _confidence, _composite, _bandit, _exit_bandit, _use_exit_model, _vol_scaling, _lazy_rebuild) \
    do {                                                                                                                                                  \
        uint16_t _new_flags = 0;                                                                                                                          \
        _new_flags |= ((_confidence)      ? MASK_ML_CFG_CONFIDENCE_ENABLED           : (uint16_t)0u);                                                     \
        _new_flags |= ((_composite)       ? MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED : (uint16_t)0u);                                                     \
        _new_flags |= ((_bandit)          ? MASK_ML_CFG_BANDIT_ENABLED               : (uint16_t)0u);                                                     \
        _new_flags |= ((_exit_bandit)     ? MASK_ML_CFG_EXIT_BANDIT_ENABLED          : (uint16_t)0u);                                                     \
        _new_flags |= ((_use_exit_model)  ? MASK_ML_CFG_USE_EXIT_MODEL               : (uint16_t)0u);                                                     \
        _new_flags |= ((_vol_scaling)     ? MASK_ML_CFG_FOXML_VOL_SCALING_ENABLED    : (uint16_t)0u);                                                     \
        _new_flags |= ((_lazy_rebuild)    ? MASK_ML_CFG_LAZY_REBUILD_ENABLED         : (uint16_t)0u);                                                     \
        (target_flags) = _new_flags;                                                                                                                      \
    } while (0)

//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Tuple: X(NAME, legacy_field, doc)
//
// Domain identity: ML/confidence-mechanic toggles. Add here if the flag governs
// ML pipeline behavior (confidence scoring, bandit warmup, exit-model arms,
// volatility scaling, lazy slow-path rebuild).
//
// Tuple: X(NAME, legacy_field, display_label, section, metadata_flags, doc)  [6-col v5.15.5.F.4d.1.B.2+]
// metadata_flags column added at .B.2 cohort migration; 5 STAMP_BOUND-eligible rows
// gain STAMP_BOUND_CFG_DERIVED bit (CONFIDENCE_COMPOSITE_ENABLED + RIDGE_WITHIN_HORIZON +
// RIDGE_ACROSS_HORIZONS + EXIT_BLENDER_MODE + PER_HORIZON_BARRIER_BLEND); 7 runtime-only
// rows get 0. Cohort fields auto-flow through new cfg-derived consumer framework when the
// framework consumer template fns are extended to walk FOREACH_ML_CFG_FLAG (Step 0.5b of
// .B.2 plan body).
//======================================================================
// [END_REGISTRY]_[FOREACH_ML_CFG_FLAG]
//======================================================================
#endif // ML_CFG_FLAG_REGISTRY_HPP
