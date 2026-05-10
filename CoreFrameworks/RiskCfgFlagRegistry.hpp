// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [RISK_CFG_FLAG REGISTRY — v5.14.9.F.3]
//======================================================================================================
// Fourth domain registry. Risk/sizing-mechanic boolean cfg flags. uint8_t bitmap.
//
// Pattern: heterogeneous-registry-pattern.md DOMAIN SPLIT form (Form 2).
// Cross-ref:
//   - DESIGN_SPECS/heterogeneous-registry-pattern.md (decision framework)
//   - DESIGN_SPECS/bitmap-flag-api.md (BITMAP_* primitives)
//   - DOCS/TECH_DEBT.md TECH_DEBT-023 (cfg-flag-eligibility criteria)
//
// All 3 entries pass cfg-flag-eligibility: boot-frozen, engine-wide, slow-path-tolerant.
//======================================================================================================
#ifndef RISK_CFG_FLAG_REGISTRY_HPP
#define RISK_CFG_FLAG_REGISTRY_HPP

#include <cstdint>
#include "../MemHeaders/BitmapMacros.hpp"

#define FOREACH_RISK_CFG_FLAG(X)                                                                                          \
    X(KILL_SWITCH_ENABLED,            kill_switch_enabled,            "engine-wide loss-cap kill switch (safety-first)")  \
    X(VOL_SIZING_ENABLED,             vol_sizing_enabled,             "scale trade size by realized vol (legacy ATR)")    \
    X(WS_DEAD_TIME_FLATTEN_ENABLED,   ws_dead_time_flatten_enabled,   "OMS_FlattenAll when WS dead longer than threshold")

enum RiskCfgFlag {
#define X_GEN_RISK_CFG_BIT(name, legacy_field, doc) RISK_CFG_##name,
    FOREACH_RISK_CFG_FLAG(X_GEN_RISK_CFG_BIT)
    RISK_CFG_COUNT
#undef X_GEN_RISK_CFG_BIT
};

static_assert(RISK_CFG_COUNT <= 8,
              "FOREACH_RISK_CFG_FLAG exhausted uint8_t storage; expand to uint16_t");

#define X_GEN_RISK_CFG_MASK(name, legacy_field, doc) \
    static constexpr uint8_t MASK_RISK_CFG_##name = (uint8_t)(1u << RISK_CFG_##name);
FOREACH_RISK_CFG_FLAG(X_GEN_RISK_CFG_MASK)
#undef X_GEN_RISK_CFG_MASK

#define RISK_CFG_FLAG_AUTOPOPULATE_FROM_TRIPLE(target_flags, _kill_switch, _vol_sizing, _ws_flatten) \
    do {                                                                                              \
        uint8_t _new_flags = 0;                                                                       \
        _new_flags |= ((_kill_switch) ? MASK_RISK_CFG_KILL_SWITCH_ENABLED            : (uint8_t)0u); \
        _new_flags |= ((_vol_sizing)  ? MASK_RISK_CFG_VOL_SIZING_ENABLED             : (uint8_t)0u); \
        _new_flags |= ((_ws_flatten)  ? MASK_RISK_CFG_WS_DEAD_TIME_FLATTEN_ENABLED   : (uint8_t)0u); \
        (target_flags) = _new_flags;                                                                  \
    } while (0)

#endif // RISK_CFG_FLAG_REGISTRY_HPP
