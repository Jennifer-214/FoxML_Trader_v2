// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [OPS_CFG_FLAG REGISTRY — v5.14.9.F.3]
//======================================================================================================
// Fifth domain registry. Operational-mechanic boolean cfg flags. uint8_t bitmap.
//
// Pattern: heterogeneous-registry-pattern.md DOMAIN SPLIT form (Form 2).
// Cross-ref:
//   - DESIGN_SPECS/heterogeneous-registry-pattern.md (decision framework)
//   - DESIGN_SPECS/bitmap-flag-api.md (BITMAP_* primitives)
//   - DOCS/TECH_DEBT.md TECH_DEBT-023 (cfg-flag-eligibility criteria)
//
// Both entries pass cfg-flag-eligibility: boot-frozen, engine-wide, boot-only-read.
//======================================================================================================
#ifndef OPS_CFG_FLAG_REGISTRY_HPP
#define OPS_CFG_FLAG_REGISTRY_HPP

#include <cstdint>
#include "../MemHeaders/BitmapMacros.hpp"

// Tuple: X(NAME, legacy_field, display_label, section, doc)  [5-col v5.14.9.F.5+]
#define FOREACH_OPS_CFG_FLAG(X)                                                                                                                                  \
    X(SESSION_FILTER_ENABLED,  session_filter_enabled,  "Session Filter",  "Toggles",                  "per-session gate multipliers (Asian/European/US)")        \
    X(NOTIFY_ENABLED,          notify_enabled,          "Notify",          "Operational Monitoring",   "external notification backend (Slack/email/etc.)")

enum OpsCfgFlag {
#define X_GEN_OPS_CFG_BIT(name, legacy_field, display_label, section, doc) OPS_CFG_##name,
    FOREACH_OPS_CFG_FLAG(X_GEN_OPS_CFG_BIT)
    OPS_CFG_COUNT
#undef X_GEN_OPS_CFG_BIT
};

static_assert(OPS_CFG_COUNT <= 8,
              "FOREACH_OPS_CFG_FLAG exhausted uint8_t storage; expand to uint16_t");

#define X_GEN_OPS_CFG_MASK(name, legacy_field, display_label, section, doc) \
    static constexpr uint8_t MASK_OPS_CFG_##name = (uint8_t)(1u << OPS_CFG_##name);
FOREACH_OPS_CFG_FLAG(X_GEN_OPS_CFG_MASK)
#undef X_GEN_OPS_CFG_MASK

#define OPS_CFG_FLAG_AUTOPOPULATE_FROM_PAIR(target_flags, _session_filter, _notify) \
    do {                                                                              \
        uint8_t _new_flags = 0;                                                       \
        _new_flags |= ((_session_filter) ? MASK_OPS_CFG_SESSION_FILTER_ENABLED : (uint8_t)0u); \
        _new_flags |= ((_notify)         ? MASK_OPS_CFG_NOTIFY_ENABLED         : (uint8_t)0u); \
        (target_flags) = _new_flags;                                                  \
    } while (0)

#endif // OPS_CFG_FLAG_REGISTRY_HPP
