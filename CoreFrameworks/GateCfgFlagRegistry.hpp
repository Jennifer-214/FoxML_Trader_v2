// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [GATE_CFG_FLAG REGISTRY — v5.14.9.F.1]
//======================================================================================================
// Second domain registry in the FOREACH_<DOMAIN>_CFG_FLAG family.
// Entry/exit gate boolean cfg flags. uint8_t bitmap on ControllerConfig.
//
// Pattern: heterogeneous-registry-pattern.md DOMAIN SPLIT form (Form 2).
// Same shape as FOREACH_LIFECYCLE_CFG_FLAG (v5.14.9.F precedent).
// Closes: TECH_DEBT-013 candidate (5) — boolean subset, GATE domain.
// Cross-ref:
//   - DESIGN_SPECS/heterogeneous-registry-pattern.md (decision framework + Y3 dispatch canon)
//   - DESIGN_SPECS/bitmap-flag-api.md (BITMAP_* primitives)
//   - DOCS/TECH_DEBT.md TECH_DEBT-023 (cfg-flag-eligibility criteria)
//
// CFG-FLAG ELIGIBILITY (per TECH_DEBT-023): all 6 entries below pass all 5 criteria
// (boot-frozen + engine-wide + hot-path-tolerant + no compile-time elision benefit +
//  cfg-domain-coherent — entry/exit gate mechanics).
//
// NOTE: barrier_gate_enabled is stamp-bound via FOREACH_STAMP_BOUND_MODEL_CONST (model-const
// registry, not cfg-bound registry). Stamp emission reads from `inf->barrier_gate_enabled`
// (StampInferenceCfgInputs struct field), populated by callers that read from cfg side.
// Those populator sites migrate to BITMAP_IS_SET as part of this ship's normal cascade —
// no special stamp-binding dispatch needed (that's .F.2 scope for ML-domain stamp-bound flags).
//======================================================================================================
#ifndef GATE_CFG_FLAG_REGISTRY_HPP
#define GATE_CFG_FLAG_REGISTRY_HPP

#include <cstdint>
#include "../MemHeaders/BitmapMacros.hpp"

//------------------------------------------------------------------------------------------------------
// [REGISTRY]
//------------------------------------------------------------------------------------------------------
// Tuple: X(NAME, legacy_field, doc)
//
// Domain identity: entry/exit gate mechanics. Add here if the flag governs
// when/how entries are gated or how exits dispatch (depth book gating, EMA-vs-rolling
// gate price, no-trade band suppression, cost-aware sizing gate, ML barrier gating,
// param staleness gating).

#define FOREACH_GATE_CFG_FLAG(X)                                                                                  \
    X(DEPTH_ENABLED,                depth_enabled,                "order book depth feed + book imbalance gate")  \
    X(GATE_EMA_ENABLED,             gate_ema_enabled,             "use EMA price for gate (vs rolling avg)")      \
    X(NO_TRADE_BAND_ENABLED,        no_trade_band_enabled,        "suppress entries when signal in no-trade band") \
    X(COST_GATE_ENABLED,            cost_gate_enabled,            "fee-aware sizing gate (require TP > round-trip cost)") \
    X(BARRIER_GATE_ENABLED,         barrier_gate_enabled,         "ML 3-class barrier gate (uses P(peak)/P(stable) for gating; stamp-bound via FOREACH_STAMP_BOUND_MODEL_CONST)") \
    X(PARAM_STALENESS_GATE_ENABLED, param_staleness_gate_enabled, "GATE_FLAG_STALENESS_ENABLED hot-path gate via slow-path rebuild")

//------------------------------------------------------------------------------------------------------
// [AUTO-GENERATED ENUM + COUNT]
//------------------------------------------------------------------------------------------------------
enum GateCfgFlag {
#define X_GEN_GATE_CFG_BIT(name, legacy_field, doc) GATE_CFG_##name,
    FOREACH_GATE_CFG_FLAG(X_GEN_GATE_CFG_BIT)
    GATE_CFG_COUNT
#undef X_GEN_GATE_CFG_BIT
};

static_assert(GATE_CFG_COUNT <= 8,
              "FOREACH_GATE_CFG_FLAG exhausted uint8_t storage; expand cfg.gate_cfg_flags to uint16_t");

//------------------------------------------------------------------------------------------------------
// [AUTO-GENERATED MASK_GATE_CFG_<NAME> CONSTANTS]
//------------------------------------------------------------------------------------------------------
#define X_GEN_GATE_CFG_MASK(name, legacy_field, doc) \
    static constexpr uint8_t MASK_GATE_CFG_##name = (uint8_t)(1u << GATE_CFG_##name);
FOREACH_GATE_CFG_FLAG(X_GEN_GATE_CFG_MASK)
#undef X_GEN_GATE_CFG_MASK

//------------------------------------------------------------------------------------------------------
// [AUTOPOPULATE COMPANION]
//------------------------------------------------------------------------------------------------------
// Branchless OR-reduction; compiler emits cmov per row.

#define GATE_CFG_FLAG_AUTOPOPULATE_FROM_HEX(target_flags, _depth, _gate_ema, _no_trade_band, _cost_gate, _barrier_gate, _param_staleness) \
    do {                                                                                                                                 \
        uint8_t _new_flags = 0;                                                                                                          \
        _new_flags |= ((_depth)            ? MASK_GATE_CFG_DEPTH_ENABLED                : (uint8_t)0u);                                  \
        _new_flags |= ((_gate_ema)         ? MASK_GATE_CFG_GATE_EMA_ENABLED             : (uint8_t)0u);                                  \
        _new_flags |= ((_no_trade_band)    ? MASK_GATE_CFG_NO_TRADE_BAND_ENABLED        : (uint8_t)0u);                                  \
        _new_flags |= ((_cost_gate)        ? MASK_GATE_CFG_COST_GATE_ENABLED            : (uint8_t)0u);                                  \
        _new_flags |= ((_barrier_gate)     ? MASK_GATE_CFG_BARRIER_GATE_ENABLED         : (uint8_t)0u);                                  \
        _new_flags |= ((_param_staleness)  ? MASK_GATE_CFG_PARAM_STALENESS_GATE_ENABLED : (uint8_t)0u);                                  \
        (target_flags) = _new_flags;                                                                                                     \
    } while (0)

#endif // GATE_CFG_FLAG_REGISTRY_HPP
