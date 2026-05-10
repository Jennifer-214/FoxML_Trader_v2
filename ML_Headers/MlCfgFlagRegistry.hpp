// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ML_CFG_FLAG REGISTRY — v5.14.9.F.2]
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

//------------------------------------------------------------------------------------------------------
// [REGISTRY]
//------------------------------------------------------------------------------------------------------
// Tuple: X(NAME, legacy_field, doc)
//
// Domain identity: ML/confidence-mechanic toggles. Add here if the flag governs
// ML pipeline behavior (confidence scoring, bandit warmup, exit-model arms,
// volatility scaling, lazy slow-path rebuild).

// Tuple: X(NAME, legacy_field, display_label, section, doc)  [5-col v5.14.9.F.5+]
#define FOREACH_ML_CFG_FLAG(X)                                                                                                                                                                          \
    X(CONFIDENCE_ENABLED,           confidence_enabled,           "Confidence",            "FoxML",       "scale entry threshold by confidence score")                                                  \
    X(CONFIDENCE_COMPOSITE_ENABLED, confidence_composite_enabled, "Composite Confidence",  "FoxML",       "use 4-factor composite confidence (vs legacy 3-factor); stamp-bound")                         \
    X(BANDIT_ENABLED,               bandit_enabled,               "Bandit",                "FoxML",       "Thompson-sampling bandit for buy-signal arm selection")                                      \
    X(EXIT_BANDIT_ENABLED,          exit_bandit_enabled,          "Exit Bandit",           "FoxML",       "Thompson-sampling bandit for exit-side arm selection")                                       \
    X(USE_EXIT_MODEL,               use_exit_model,               "Use Exit Model",        "FoxML",       "use dedicated exit-side ML model (vs entry model fallback)")                                 \
    X(FOXML_VOL_SCALING_ENABLED,    foxml_vol_scaling_enabled,    "Vol Scaling",           "FoxML",       "scale trade size by recent volatility (FoxML VolScaler)")                                    \
    X(LAZY_REBUILD_ENABLED,         lazy_rebuild_enabled,         "Lazy Rebuild",          "Performance", "skip slow-path rebuild when no parameter inputs changed")

//------------------------------------------------------------------------------------------------------
// [AUTO-GENERATED ENUM + COUNT]
//------------------------------------------------------------------------------------------------------
enum MlCfgFlag {
#define X_GEN_ML_CFG_BIT(name, legacy_field, display_label, section, doc) ML_CFG_##name,
    FOREACH_ML_CFG_FLAG(X_GEN_ML_CFG_BIT)
    ML_CFG_COUNT
#undef X_GEN_ML_CFG_BIT
};

static_assert(ML_CFG_COUNT <= 16,
              "FOREACH_ML_CFG_FLAG exhausted uint16_t storage; expand cfg.ml_cfg_flags to uint32_t");

//------------------------------------------------------------------------------------------------------
// [AUTO-GENERATED MASK_ML_CFG_<NAME> CONSTANTS]
//------------------------------------------------------------------------------------------------------
#define X_GEN_ML_CFG_MASK(name, legacy_field, display_label, section, doc) \
    static constexpr uint16_t MASK_ML_CFG_##name = (uint16_t)(1u << ML_CFG_##name);
FOREACH_ML_CFG_FLAG(X_GEN_ML_CFG_MASK)
#undef X_GEN_ML_CFG_MASK

//------------------------------------------------------------------------------------------------------
// [AUTOPOPULATE COMPANION]
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

#endif // ML_CFG_FLAG_REGISTRY_HPP
