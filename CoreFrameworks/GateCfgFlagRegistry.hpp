// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/GateCfgFlagRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [BITMAP_PACKED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[gate-domain boolean cfg flags — the 6-col metadata_flags pilot of the FOREACH_<DOMAIN>_CFG_FLAG family]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_GATE_CFG_FLAG]
//======================================================================================================
#ifndef GATE_CFG_FLAG_REGISTRY_HPP
#define GATE_CFG_FLAG_REGISTRY_HPP

#include <cstdint>
#include "../MemHeaders/BitmapMacros.hpp"

//======================================================================
// [REGISTRY]_[FOREACH_GATE_CFG_FLAG]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [BITMAP_PACKED]]
// [REFERENCE]_[DESIGN_SPEC]_[heterogeneous-registry-pattern]
// [REFERENCE]_[DESIGN_SPEC]_[bitmap-flag-api]
// [REFERENCE]_[DESIGN_SPEC]_[canonical-sister-extension-discipline]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-023]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[entry/exit gate boolean cfg flags — one row = enum bit + mask + parser + GUI; metadata_flags routes stamp-binding]
// [COLUMN]_[NAME]_[flag identifier -> GATE_CFG_<NAME> enum bit + MASK_GATE_CFG_<NAME>]   (6-col tuple, v5.15.5.F.4d.1.B.3+)
// [COLUMN]_[legacy_field]_[cfg-file key the parser walker matches]
// [COLUMN]_[display_label]_[GUI checkbox label]
// [COLUMN]_[section]_[GUI collapsing-header / section name]
// [COLUMN]_[metadata_flags]_[CfgFieldDescriptor OR-flags]_[[STAMP_BOUND_CFG_DERIVED]]
// [COLUMN]_[doc]_[operator-facing description]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_GATE_CFG_FLAG(X)                                                                                                                                                                                                                                          \
    X(DEPTH_ENABLED,                depth_enabled,                "Order Book",            "Toggles",         0,                                                "order book depth feed + book imbalance gate")                                                                \
    X(GATE_EMA_ENABLED,             gate_ema_enabled,             "EMA Enabled",           "EMA Gate",        0,                                                "use EMA price for gate (vs rolling avg)")                                                                    \
    X(NO_TRADE_BAND_ENABLED,        no_trade_band_enabled,        "No-Trade Band##bool",   "No-Trade Band",   0,                                                "suppress entries when signal in no-trade band")                                                              \
    X(COST_GATE_ENABLED,            cost_gate_enabled,            "Cost Gate",             "FoxML",           0,                                                "fee-aware sizing gate (require TP > round-trip cost)")                                                       \
    X(BARRIER_GATE_ENABLED,         barrier_gate_enabled,         "Barrier Gate",          "Barrier",         CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED,      "ML 3-class barrier gate (P(peak)/P(stable) gating; stamp-bound via framework walker at .B.3+; was POST_CFG pre-.B.3)") \
    X(PARAM_STALENESS_GATE_ENABLED, param_staleness_gate_enabled, "Param Staleness Gate",  "Risk Management", 0,                                                "GATE_FLAG_STALENESS_ENABLED hot-path gate via slow-path rebuild")

//------------------------------------------------------------------------------------------------------
// [SECTION]_[auto-generated enum + count]
//------------------------------------------------------------------------------------------------------
enum GateCfgFlag {
#define X_GEN_GATE_CFG_BIT(name, legacy_field, display_label, section, metadata_flags, doc) GATE_CFG_##name,
    FOREACH_GATE_CFG_FLAG(X_GEN_GATE_CFG_BIT)
    GATE_CFG_COUNT
#undef X_GEN_GATE_CFG_BIT
};

static_assert(GATE_CFG_COUNT <= 8,
              "FOREACH_GATE_CFG_FLAG exhausted uint8_t storage; expand cfg.gate_cfg_flags to uint16_t");

//------------------------------------------------------------------------------------------------------
// [SECTION]_[auto-generated MASK_GATE_CFG_<NAME> constants]
//------------------------------------------------------------------------------------------------------
#define X_GEN_GATE_CFG_MASK(name, legacy_field, display_label, section, metadata_flags, doc) \
    static constexpr uint8_t MASK_GATE_CFG_##name = (uint8_t)(1u << GATE_CFG_##name);
FOREACH_GATE_CFG_FLAG(X_GEN_GATE_CFG_MASK)
#undef X_GEN_GATE_CFG_MASK

//------------------------------------------------------------------------------------------------------
// [SECTION]_[autopopulate companion]
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Domain identity: entry/exit gate mechanics. Add here if the flag governs
// when/how entries are gated or how exits dispatch (depth book gating, EMA-vs-rolling
// gate price, no-trade band suppression, cost-aware sizing gate, ML barrier gating,
// param staleness gating).
//
// metadata_flags column added at .B.3 Step 0.5d.a.0 per Meta-gap M1b cohort migration discipline;
// sister to FOREACH_ML_CFG_FLAG .B.2 migration. Only BARRIER_GATE_ENABLED has STAMP_BOUND_CFG_DERIVED;
// other 5 rows get 0 (no stamp-binding consumer at this ship).
// CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED requires CfgFieldRegistry.hpp to be in scope at
// CONSUMER X-macro expansion site (not at FOREACH_GATE_CFG_FLAG definition site — text-only here).
//======================================================================
// [COMMENT]_[family position + eligibility]
//----------------------------------------------------------------------
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
// CFG-FLAG ELIGIBILITY (per TECH_DEBT-023): all 6 entries pass all 5 criteria
// (boot-frozen + engine-wide + hot-path-tolerant + no compile-time elision benefit +
//  cfg-domain-coherent — entry/exit gate mechanics).
//======================================================================
// [COMMENT]_[M1b metadata_flags cohort migration — the .B.3 history]
//----------------------------------------------------------------------
// NOTE (v5.15.5.F.4d.1.B.3 Step 0.5d.a.0 — Meta-gap M1b first canonical reference): metadata_flags
// column added 2026-05-18 (sister to FOREACH_ML_CFG_FLAG .B.2 migration at engine commit `de41ff2`)
// per canonical-sister-extension-discipline.md § Sister-registry sig migration as cohort discipline.
// BARRIER_GATE_ENABLED row gets STAMP_BOUND_CFG_DERIVED — stamp emission migrates to framework
// walker (cfg_derived::populate_stamp_cfg_from_derived extended at Step 0.5d.a-d to walk
// FOREACH_GATE_CFG_FLAG with metadata_flags filter); POST_CFG entry at StampBoundModelConstRegistry.hpp:283-284
// (`inference_cfg_barrier_gate_enabled=`) deleted at Decision D mechanism 1 (closes Class 18 mirror
// + Class 32 prefix asymmetry instance). Other 5 rows get 0 (no stamp-binding need at .B.3).
// FOREACH_LIFECYCLE_CFG_FLAG + FOREACH_RISK_CFG_FLAG + FOREACH_OPS_CFG_FLAG stay 5-col per Meta-gap
// M1b § Decision per sister: DEFER with explicit rationale — no STAMP_BOUND-eligible consumer at
// this ship; future ship that needs the column adds migration as setup step in same commit.
//======================================================================
// [DERIVED]   (tool-refreshed — ROW_COUNT/CONSUMERS generators land with the drift-gate generalization; empty skeleton is correct, D-327)
//======================================================================
// [END_REGISTRY]_[FOREACH_GATE_CFG_FLAG]
//======================================================================

#endif // GATE_CFG_FLAG_REGISTRY_HPP
