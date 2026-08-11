// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/RiskCfgFlagRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [BITMAP_PACKED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[risk-domain boolean cfg flags — uint8_t bitmap registry + generated enum/masks/autopopulate]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_RISK_CFG_FLAG]
//======================================================================================================
#ifndef RISK_CFG_FLAG_REGISTRY_HPP
#define RISK_CFG_FLAG_REGISTRY_HPP

#include <cstdint>
#include "../MemHeaders/BitmapMacros.hpp"

//======================================================================
// [REGISTRY]_[FOREACH_RISK_CFG_FLAG]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [BITMAP_PACKED]]
// [REFERENCE]_[DESIGN_SPEC]_[heterogeneous-registry-pattern]
// [REFERENCE]_[DESIGN_SPEC]_[bitmap-flag-api]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-023]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[risk/sizing-mechanic boolean cfg flags — one row = enum bit + mask + parser route + GUI render]
// [COLUMN]_[NAME]_[flag identifier -> RISK_CFG_<NAME> enum bit + MASK_RISK_CFG_<NAME>]   (5-col tuple, v5.14.9.F.5+)
// [COLUMN]_[legacy_field]_[cfg-file key the parser walker matches]
// [COLUMN]_[display_label]_[GUI display string]
// [COLUMN]_[section]_[GUI bucket]
// [COLUMN]_[doc]_[operator-facing description]
// [REFERENCE]_[INVARIANT]_[H14]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_RISK_CFG_FLAG(X)                                                                                                                                                          \
    X(KILL_SWITCH_ENABLED,            kill_switch_enabled,            "Enabled",                "Kill Switch",     "engine-wide loss-cap kill switch (safety-first)")                      \
    X(VOL_SIZING_ENABLED,             vol_sizing_enabled,             "Vol Sizing##bool",       "Vol Sizing",      "scale trade size by realized vol (legacy ATR)")                       \
    X(WS_DEAD_TIME_FLATTEN_ENABLED,   ws_dead_time_flatten_enabled,   "WS Dead-Time Flatten",   "Risk Management", "OMS_FlattenAll when WS dead longer than threshold")                    \
    X(MTM_KILL_SWITCH_ENABLED,        enable_mtm_kill_switch,         "MTM Kill Switch",        "Kill Switch",     "mark-to-market kill switch (halts entries when realized+unrealized P&L crosses kill_switch_threshold_pct; sister to KILL_SWITCH_ENABLED; H14 bitmap migration at v5.15.5.F.4d.1.B.4 Cx-T)") \
    X(SL_COOLDOWN_ADAPTIVE_ENABLED,   sl_cooldown_adaptive,           "Adaptive SL Cooldown",   "Gate Recovery",   "post-stop-loss cooldown mode: 0=fixed cycles (sl_cooldown_cycles), 1=scale by trend confidence; H14 bitmap migration at v5.15.5.F.4d.1.B.4 Cx-U")

enum RiskCfgFlag {
#define X_GEN_RISK_CFG_BIT(name, legacy_field, display_label, section, doc) RISK_CFG_##name,
    FOREACH_RISK_CFG_FLAG(X_GEN_RISK_CFG_BIT)
    RISK_CFG_COUNT
#undef X_GEN_RISK_CFG_BIT
};

static_assert(RISK_CFG_COUNT <= 8,
              "FOREACH_RISK_CFG_FLAG exhausted uint8_t storage; expand to uint16_t");

#define X_GEN_RISK_CFG_MASK(name, legacy_field, display_label, section, doc) \
    static constexpr uint8_t MASK_RISK_CFG_##name = (uint8_t)(1u << RISK_CFG_##name);
FOREACH_RISK_CFG_FLAG(X_GEN_RISK_CFG_MASK)
#undef X_GEN_RISK_CFG_MASK

#define RISK_CFG_FLAG_AUTOPOPULATE_FROM_QUINTUPLE(target_flags, _kill_switch, _vol_sizing, _ws_flatten, _mtm_kill_switch, _sl_cooldown_adaptive) \
    do {                                                                                                                                                            \
        uint8_t _new_flags = 0;                                                                                                                                     \
        _new_flags |= ((_kill_switch)          ? MASK_RISK_CFG_KILL_SWITCH_ENABLED            : (uint8_t)0u);                                                       \
        _new_flags |= ((_vol_sizing)           ? MASK_RISK_CFG_VOL_SIZING_ENABLED             : (uint8_t)0u);                                                       \
        _new_flags |= ((_ws_flatten)           ? MASK_RISK_CFG_WS_DEAD_TIME_FLATTEN_ENABLED   : (uint8_t)0u);                                                       \
        _new_flags |= ((_mtm_kill_switch)      ? MASK_RISK_CFG_MTM_KILL_SWITCH_ENABLED        : (uint8_t)0u);                                                       \
        _new_flags |= ((_sl_cooldown_adaptive) ? MASK_RISK_CFG_SL_COOLDOWN_ADAPTIVE_ENABLED   : (uint8_t)0u);                                                       \
        (target_flags) = _new_flags;                                                                                                                                \
    } while (0)
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Fourth domain registry. Risk/sizing-mechanic boolean cfg flags. uint8_t bitmap.
//
// Pattern: heterogeneous-registry-pattern.md DOMAIN SPLIT form (Form 2).
// Cross-ref:
//   - DESIGN_SPECS/heterogeneous-registry-pattern.md (decision framework)
//   - DESIGN_SPECS/bitmap-flag-api.md (BITMAP_* primitives)
//   - DOCS/TECH_DEBT.md TECH_DEBT-023 (cfg-flag-eligibility criteria)
//
// All entries pass cfg-flag-eligibility: boot-frozen, engine-wide, slow-path-tolerant.
//======================================================================
// [DERIVED]   (tool-refreshed — ROW_COUNT/CONSUMERS generators land with the drift-gate generalization; empty skeleton is correct, D-327)
//======================================================================
// [END_REGISTRY]_[FOREACH_RISK_CFG_FLAG]
//======================================================================

#endif // RISK_CFG_FLAG_REGISTRY_HPP
