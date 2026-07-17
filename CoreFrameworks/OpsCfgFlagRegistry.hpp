// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/OpsCfgFlagRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [BITMAP_PACKED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[operational-domain boolean cfg flags — uint8_t bitmap registry + generated enum/masks]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_OPS_CFG_FLAG]
//======================================================================================================
#ifndef OPS_CFG_FLAG_REGISTRY_HPP
#define OPS_CFG_FLAG_REGISTRY_HPP

#include <cstdint>
#include "../MemHeaders/BitmapMacros.hpp"

//======================================================================
// [REGISTRY]_[FOREACH_OPS_CFG_FLAG]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [BITMAP_PACKED]]
// [REFERENCE]_[DESIGN_SPEC]_[heterogeneous-registry-pattern]
// [REFERENCE]_[DESIGN_SPEC]_[bitmap-flag-api]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-023]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[operational-mechanic boolean cfg flags — one row = enum bit + mask + parser route + GUI render]
// [COLUMN]_[NAME]_[flag identifier -> OPS_CFG_<NAME> enum bit + MASK_OPS_CFG_<NAME>]   (5-col tuple, v5.14.9.F.5+)
// [COLUMN]_[legacy_field]_[cfg-file key the parser walker matches]
// [COLUMN]_[display_label]_[GUI display string]
// [COLUMN]_[section]_[GUI bucket]
// [COLUMN]_[doc]_[operator-facing description]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_OPS_CFG_FLAG(X)                                                                                                                                                                                                                                       \
    X(SESSION_FILTER_ENABLED,                  session_filter_enabled,                  "Session Filter",                  "Toggles",                  "per-session gate multipliers (Asian/European/US)")                                                            \
    X(NOTIFY_ENABLED,                          notify_enabled,                          "Notify",                          "Operational Monitoring",   "external notification backend (Slack/email/etc.)")                                                             \
    X(ACKNOWLEDGE_INFERENCE_CFG_DRIFT,         acknowledge_inference_cfg_drift,         "Ack Inference CFG Drift",         "Drift Acknowledgments",    "suppress stamp↔cfg inference_cfg drift WARN/REFUSE (Tier 1 + Tier 2 in NodeModelZoo_ValidateAgainstCfg)")    \
    X(ACKNOWLEDGE_CROSS_BINARY_DRIFT,          acknowledge_cross_binary_version_drift,  "Ack Cross-Binary Drift",          "Drift Acknowledgments",    "suppress xgb/build_flags/poll_interval cross-binary WARN (training-binary divergence is forensic only)")

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

// v5.15.5.A.7 — OPS_CFG_FLAG_AUTOPOPULATE_FROM_PAIR was the 2-arg init helper for
// the original SESSION_FILTER + NOTIFY pair (legacy from v5.14.9.F.3). Cohort
// growth to 4 entries (with the 2 ack-flag additions) makes the FROM_PAIR shape
// inadequate. Replaced with direct zero-init at the call site:
//   cfg.ops_cfg_flags = 0;
// All 4 entries default OFF (matching legacy direct-int defaults of 0). Operator
// cfg keys then set bits via the FOREACH_OPS_CFG_FLAG parser walker at
// ControllerConfig.hpp:~2220 (legacy_field column auto-routes the parse).
//
// Kept the FROM_PAIR macro definition COMMENTED OUT to surface the rename if
// any caller still references it (would compile-error). One known caller at
// ControllerConfig.hpp:1428 — migrated to direct `cfg.ops_cfg_flags = 0`.
//
// #define OPS_CFG_FLAG_AUTOPOPULATE_FROM_PAIR(target_flags, _session_filter, _notify) /* RETIRED v5.15.5.A.7 */
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.A.7 — Cohort-migrated 2 orphan ack flags from direct int cfg fields:
//   - acknowledge_inference_cfg_drift  (was ControllerConfig.hpp:861 int field; v5.9.5i)
//   - acknowledge_cross_binary_version_drift (was :853 int field; v5.9.4)
// Both are operator-decision flags ("suppress this drift category WARN/REFUSE").
// Closes the orphan-boolean tail of TECH_DEBT-009's v5.14.9.F.4 partial migration.
// Per-core override comes free via PER_NODE_OVERRIDE_BITMAP_DOMAINS (ops domain
// already listed there). Cohort migration per CLAUDE.local.md 2026-05-11 rule +
// cfg-flag-eligibility-criteria.md (boot-frozen, engine-wide, slow-path-tolerant).
//
// Backward-compat: operator's existing `acknowledge_*_drift=1` cfg keys still parse
// (the FOREACH_OPS_CFG_FLAG-walker parser at ControllerConfig.hpp:~2220 matches via
// the legacy_field column; sets the corresponding bit). No cfg-file migration needed.
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Fifth domain registry. Operational-mechanic boolean cfg flags. uint8_t bitmap.
//
// Pattern: heterogeneous-registry-pattern.md DOMAIN SPLIT form (Form 2).
// Cross-ref:
//   - DESIGN_SPECS/heterogeneous-registry-pattern.md (decision framework)
//   - DESIGN_SPECS/bitmap-flag-api.md (BITMAP_* primitives)
//   - DOCS/TECH_DEBT.md TECH_DEBT-023 (cfg-flag-eligibility criteria)
//
// All entries pass cfg-flag-eligibility: boot-frozen, engine-wide, boot-only-read.
//======================================================================
// [DERIVED]   (tool-refreshed — ROW_COUNT/CONSUMERS generators land with the drift-gate generalization; empty skeleton is correct, D-327)
//======================================================================
// [END_REGISTRY]_[FOREACH_OPS_CFG_FLAG]
//======================================================================

#endif // OPS_CFG_FLAG_REGISTRY_HPP
