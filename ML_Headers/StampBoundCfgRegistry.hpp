// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [STAMP-BOUND CFG REGISTRY — v5.14.1.B.3]
//======================================================================================================
// X-macro registry for cfg fields that are bound into the model stamp body
// for train↔serve drift detection. Closes PARITY-004 (HIGH) + PARITY-005
// (MEDIUM) + partially resurrects v5.9.2b's abandoned
// inference_cfg_drift_count mechanism (CLEANUP-001).
//
// PATTERN (CLAUDE.md item 13): adding a new stamp-bound cfg field becomes
// ONE line in this registry. The X-macro auto-generates:
//   - struct fields in StampInferenceCfgInputs (emit-side) +
//     ModelStampResult (parse-side), each with `has_<name>` Surface G
//     forward-compat flag
//   - emit code in stamp_write_for_model (gated by has_<name>=1)
//   - parser branches in verify_model_stamp (sets has_<name>=1 on match)
//   - zero-init at top of verify_model_stamp (has_<name>=0 default for
//     legacy stamps; drift check skipped on legacy)
//   - drift comparison at caller sites (CoreModelZoo); increments
//     ModelStampResult.inference_cfg_drift_count on mismatch
//
// SCOPE for v5.14.1.B.3: 10 NEW Ridge + composite cfg fields. Existing
// v5.9.2b inference_cfg_* fields (9 fields) + v5.11.18a feature_mask +
// v5.11.41 label_params + xgb_train_nthread stay on the manual path
// for v5.14.1.B.3 to keep the surface area bounded; v5.15+ cleanup
// migrates them into this registry (see
// plans/2026-05-09-v5.15-plus-cleanup-backlog.md → CLEANUP-001).
//
// FORWARD-COMPAT (Surface G): has_<name>=0 default for legacy stamps
// means the parser leaves new fields untouched on a v5.13.x stamp; the
// drift check skips silently because has_<name>=0. MODEL_FORMAT_VERSION
// stays at 6 (UNCHANGED — Surface G discipline; new fields are optional
// canonical body lines).
//======================================================================================================
#ifndef STAMP_BOUND_CFG_REGISTRY_HPP
#define STAMP_BOUND_CFG_REGISTRY_HPP

#include <stdlib.h>   // atoi
// tt::parse_double_fast comes from ModelInference.hpp's includes; this
// header is included AFTER that namespace is available. We use a small
// helper macro to dispatch parser by type so the X-macro stays clean.

//======================================================================================================
// [REGISTRY ENTRY SHAPE]
//======================================================================================================
// X(name, type, fmt, default_val, get_cfg_expr)
//
//   name        — canonical stamp body key (also struct field name).
//                 Must be a valid C identifier; written to stamp body as
//                 `<name>=<value>\n` line.
//   type        — C++ type for the field (`int` or `double`). Drives
//                 struct field type, parser dispatch, fmt format string.
//   fmt         — printf format (`"%d"` for int, `"%.17g"` for double
//                 lossless round-trip per v5.9.4a precedent).
//   default_val — zero-init value (0 for int, 0.0 for double). Set in
//                 verify_model_stamp's init block before parsing.
//   get_cfg_expr — expression to extract value from variable named `cfg`
//                 at the call site. e.g. `cfg.ridge_within_horizon`
//                 (direct int access) or `FPN_ToDouble(cfg.ridge_lambda)`
//                 (FPN→double conversion at boundary).
//
// IMPORTANT: get_cfg_expr is evaluated at the CALLER (CoreModelZoo +
// production stamp emit). The macro doesn't know what `cfg` is; the
// caller's scope provides it. This design keeps ControllerConfig out of
// ModelInference.hpp's include graph (one-way dep direction).
//======================================================================================================

#define FOREACH_STAMP_BOUND_CFG(X)                                                                  \
    /* v5.14.1.B.3 — Ridge risk-parity blending (PARITY-004) */                                     \
    X(ridge_within_horizon,                int,    "%d",     0,   cfg.ridge_within_horizon)         \
    X(ridge_across_horizons,               int,    "%d",     0,   cfg.ridge_across_horizons)        \
    X(ridge_lambda,                        double, "%.17g",  0.0, FPN_ToDouble(cfg.ridge_lambda))   \
    X(ridge_cost_penalty,                  double, "%.17g",  0.0, FPN_ToDouble(cfg.ridge_cost_penalty))  \
    X(ridge_min_ic_floor,                  double, "%.17g",  0.0, FPN_ToDouble(cfg.ridge_min_ic_floor))  \
    /* v5.14.1.B.3 — Composite confidence (PARITY-005) */                                           \
    X(confidence_composite_enabled,        int,    "%d",     0,   cfg.confidence_composite_enabled) \
    X(confidence_freshness_tau_secs,       double, "%.17g",  0.0, FPN_ToDouble(cfg.confidence_freshness_tau_secs))     \
    X(confidence_capacity_target_dollars,  double, "%.17g",  0.0, FPN_ToDouble(cfg.confidence_capacity_target_dollars))\
    X(confidence_capacity_kappa,           double, "%.17g",  0.0, FPN_ToDouble(cfg.confidence_capacity_kappa))         \
    X(confidence_rmse_baseline,            double, "%.17g",  0.0, FPN_ToDouble(cfg.confidence_rmse_baseline))         \
    /* v5.14.1.D — Feature winsorization (PARITY drift detection) */                                  \
    X(winsor_pct_low,                      double, "%.17g",  0.0, FPN_ToDouble(cfg.winsor_pct_low))                  \
    X(winsor_pct_high,                     double, "%.17g",  0.0, FPN_ToDouble(cfg.winsor_pct_high))                 \
    /* v5.14.1.E — Exit-side blender selector (PARITY drift detection) */                             \
    X(exit_blender_mode,                   int,    "%d",     0,   cfg.exit_blender_mode)

//======================================================================================================
// [PARSER DISPATCH MACROS]
//======================================================================================================
// Per-type parsers used in the parser X-macro expansion (verify_model_stamp).
// Add a new type? Add a new STAMP_CFG_PARSE_<type> macro below.
//   STAMP_CFG_PARSE(int, val)    → atoi(val)
//   STAMP_CFG_PARSE(double, val) → tt::parse_double_fast(val)
//======================================================================================================

#define STAMP_CFG_PARSE_int(val)    atoi(val)
#define STAMP_CFG_PARSE_double(val) tt::parse_double_fast(val)
#define STAMP_CFG_PARSE(type, val)  STAMP_CFG_PARSE_##type(val)

//======================================================================================================
// [TEST INSTRUMENTATION]
//======================================================================================================
// Compile-time field count for tests. Counts entries via macro counting.
// Used by tests to assert "all 10 expected fields are present in the
// registry" — catches accidental row deletion during refactors.
//======================================================================================================

#define STAMP_CFG_COUNT_ONE(name, type, fmt, default_val, get_cfg) +1
#define FOREACH_STAMP_BOUND_CFG_COUNT  (0 FOREACH_STAMP_BOUND_CFG(STAMP_CFG_COUNT_ONE))

#endif // STAMP_BOUND_CFG_REGISTRY_HPP
