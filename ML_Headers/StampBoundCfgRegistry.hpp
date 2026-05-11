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
// [REGISTRY ENTRY SHAPE — extended v5.14.1.E.E.B for auto-populate]
//======================================================================================================
// X(name, type, fmt, default_val, get_cfg_expr, emit_when)
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
//   emit_when   — boolean expression evaluated at production-caller
//                 emit time. When TRUE, has_<name>=1 + value populated;
//                 when FALSE, has_<name>=0 (skip emit; legacy stamp).
//                 Default-zero cfg means stamp doesn't have the field.
//                 e.g. `cfg.exit_blender_mode` (truthy when set non-zero).
//                 Operator opt-in features are typically gated by their
//                 own enable flag; multi-field features may share a flag.
//
// IMPORTANT: get_cfg_expr + emit_when are evaluated at the CALLER
// (CoreModelZoo + production stamp emit). The macro doesn't know what
// `cfg` is; the caller's scope provides it. This design keeps
// ControllerConfig out of ModelInference.hpp's include graph (one-way
// dep direction).
//
// AUTO-POPULATE (v5.14.1.E.E.B): production stamp emit at BacktestEngine
// uses `STAMP_CFG_AUTOPOPULATE(inf, cfg)` macro to expand into per-field
// gated populator code. This eliminates the v5.9.5b production-caller
// field-population gap class — adding a new stamp-bound field becomes
// ONE line in the registry; populator is auto-generated. Recurrences
// (PARITY-002/003/004/005/008) all stem from forgetting the manual
// populator. Auto-populate makes forgetting impossible.
//======================================================================================================

// v5.14.9.F.2 — Tuple extended 6-col → 7-col. New `emit_source` column is a TOKEN
// (DIRECT_FIELD or BITMAP_BIT) consumed by Y3 token-paste dispatch in STAMP_CFG_AUTOPOPULATE_ONE.
// Most entries are DIRECT_FIELD (get_cfg reads cfg field verbatim). BITMAP_BIT entries
// have get_cfg as a bitmap-extract expression. Both dispatch paths produce identical wire
// bytes today (per heterogeneous-registry-pattern.md Form 3 worked example); future
// emit_source values (e.g., COMPUTED_FROM_GROUP) can have genuinely different handler bodies.
// Adding a new emit_source = ONE new HANDLE_STAMP_EMIT_<MARKER> macro; existing entries unchanged.
//
// Consumers that walk FOREACH_STAMP_BOUND_CFG with their own X macros now accept 7-arg
// signature with `emit_source` as 7th arg (may be unused). Updated consumers:
// CoreModelZoo.hpp (drift check), ModelInference.hpp (struct gen × 2, parser, emit walk).

#define FOREACH_STAMP_BOUND_CFG(X)                                                                                                                          \
    /* v5.14.1.B.3 — Ridge risk-parity blending (PARITY-004) */                                                                                              \
    /* v5.14.11.C — ridge_within_horizon + ridge_across_horizons migrated to ml_cfg_flags bitmap (cohort).        */                                          \
    /*              emit_source flipped DIRECT_FIELD → BITMAP_BIT for both with ?1:0 ternary normalization for     */                                         \
    /*              HMAC byte-equivalence per wire-format-byte-preservation-discipline.md + v5.14.10 Surprise 6.   */                                         \
    /*              emit_when uses BITMAP_ANY for the OR-of-both-bits predicate.                                  */                                          \
    /* emit_when: any Ridge mode enabled (cohort bitmap) */                                                                                                  \
    X(ridge_within_horizon,                int,    "%d",     0,                                                                                              \
        (BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_RIDGE_WITHIN_HORIZON) ? 1 : 0),                                                                          \
        BITMAP_ANY(cfg.ml_cfg_flags, MASK_ML_CFG_RIDGE_WITHIN_HORIZON | MASK_ML_CFG_RIDGE_ACROSS_HORIZONS), BITMAP_BIT)                                       \
    X(ridge_across_horizons,               int,    "%d",     0,                                                                                              \
        (BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_RIDGE_ACROSS_HORIZONS) ? 1 : 0),                                                                         \
        BITMAP_ANY(cfg.ml_cfg_flags, MASK_ML_CFG_RIDGE_WITHIN_HORIZON | MASK_ML_CFG_RIDGE_ACROSS_HORIZONS), BITMAP_BIT)                                       \
    X(ridge_lambda,                        double, "%.17g",  0.0, FPN_ToDouble(cfg.ridge_lambda),                                                            \
        BITMAP_ANY(cfg.ml_cfg_flags, MASK_ML_CFG_RIDGE_WITHIN_HORIZON | MASK_ML_CFG_RIDGE_ACROSS_HORIZONS), DIRECT_FIELD)                                     \
    X(ridge_cost_penalty,                  double, "%.17g",  0.0, FPN_ToDouble(cfg.ridge_cost_penalty),                                                      \
        BITMAP_ANY(cfg.ml_cfg_flags, MASK_ML_CFG_RIDGE_WITHIN_HORIZON | MASK_ML_CFG_RIDGE_ACROSS_HORIZONS), DIRECT_FIELD)                                     \
    X(ridge_min_ic_floor,                  double, "%.17g",  0.0, FPN_ToDouble(cfg.ridge_min_ic_floor),                                                      \
        BITMAP_ANY(cfg.ml_cfg_flags, MASK_ML_CFG_RIDGE_WITHIN_HORIZON | MASK_ML_CFG_RIDGE_ACROSS_HORIZONS), DIRECT_FIELD)                                     \
    /* v5.14.1.B.3 — Composite confidence (PARITY-005) */                                                                                                    \
    /* emit_when: composite enabled */                                                                                                                       \
    /* v5.14.9.F.2 — confidence_composite_enabled migrated to ml_cfg_flags bitmap. */                                                                        \
    /*               get_cfg + emit_when read via BITMAP_IS_SET → produce identical wire bytes. */                                                           \
    /*               emit_source=BITMAP_BIT (Y3 dispatch shape per heterogeneous-registry-pattern.md). */                                                    \
    X(confidence_composite_enabled,        int,    "%d",     0,                                                                                              \
        (BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED) ? 1 : 0),                                                                  \
        BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED), BITMAP_BIT)                                                               \
    X(confidence_freshness_tau_secs,       double, "%.17g",  0.0, FPN_ToDouble(cfg.confidence_freshness_tau_secs),                                           \
        BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED), DIRECT_FIELD)                                                              \
    X(confidence_capacity_target_dollars,  double, "%.17g",  0.0, FPN_ToDouble(cfg.confidence_capacity_target_dollars),                                      \
        BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED), DIRECT_FIELD)                                                              \
    X(confidence_capacity_kappa,           double, "%.17g",  0.0, FPN_ToDouble(cfg.confidence_capacity_kappa),                                               \
        BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED), DIRECT_FIELD)                                                              \
    X(confidence_rmse_baseline,            double, "%.17g",  0.0, FPN_ToDouble(cfg.confidence_rmse_baseline),                                                \
        BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED), DIRECT_FIELD)                                                              \
    /* v5.14.1.D — Feature winsorization (PARITY drift detection) */                                                                                         \
    /* emit_when: cfg has valid winsor range (low > 0 AND high < 1 AND low < high) */                                                                        \
    X(winsor_pct_low,                      double, "%.17g",  0.0, FPN_ToDouble(cfg.winsor_pct_low),                                                          \
        (FPN_ToDouble(cfg.winsor_pct_low) > 0.0 && FPN_ToDouble(cfg.winsor_pct_high) < 1.0 &&                                                                \
         FPN_ToDouble(cfg.winsor_pct_low) < FPN_ToDouble(cfg.winsor_pct_high)), DIRECT_FIELD)                                                                \
    X(winsor_pct_high,                     double, "%.17g",  0.0, FPN_ToDouble(cfg.winsor_pct_high),                                                         \
        (FPN_ToDouble(cfg.winsor_pct_low) > 0.0 && FPN_ToDouble(cfg.winsor_pct_high) < 1.0 &&                                                                \
         FPN_ToDouble(cfg.winsor_pct_low) < FPN_ToDouble(cfg.winsor_pct_high)), DIRECT_FIELD)                                                                \
    /* v5.14.1.E — Exit-side blender selector (PARITY drift detection) */                                                                                    \
    /* v5.14.11.C — exit_blender_mode migrated to ml_cfg_flags bitmap (cohort). emit_source DIRECT_FIELD → BITMAP_BIT */                                     \
    X(exit_blender_mode,                   int,    "%d",     0,                                                                                              \
        (BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_EXIT_BLENDER_MODE) ? 1 : 0),                                                                             \
        BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_EXIT_BLENDER_MODE), BITMAP_BIT)                                                                          \
    /* v5.14.9.C — Soft risk degradation ladder (4 fields). emit_when: ladder enabled. */                                                                    \
    X(risk_degradation_curve,              int,    "%d",     0,   cfg.risk_degradation_curve,                                                                \
        (cfg.risk_degradation_curve != 0), DIRECT_FIELD)                                                                                                     \
    X(risk_full_size_threshold,            double, "%.17g",  0.0, FPN_ToDouble(cfg.risk_full_size_threshold),                                                \
        (cfg.risk_degradation_curve != 0), DIRECT_FIELD)                                                                                                     \
    X(risk_min_size_threshold,             double, "%.17g",  0.0, FPN_ToDouble(cfg.risk_min_size_threshold),                                                 \
        (cfg.risk_degradation_curve != 0), DIRECT_FIELD)                                                                                                     \
    X(risk_min_size_pct,                   double, "%.17g",  0.0, FPN_ToDouble(cfg.risk_min_size_pct),                                                       \
        (cfg.risk_degradation_curve != 0), DIRECT_FIELD)                                                                                                     \
    /* v5.14.2.E.2 — expected.cfg → stamp body migration. Always emit (model trained with these values). */                                                  \
    X(ml_buy_threshold,                    double, "%.17g",  0.0, FPN_ToDouble(cfg.ml_buy_threshold),                                                        \
        1, DIRECT_FIELD)                                                                                                                                     \
    X(gap_acceptable_threshold,            double, "%.17g",  0.0, FPN_ToDouble(cfg.gap_acceptable_threshold),                                                \
        1, DIRECT_FIELD)                                                                                                                                     \
    /* v5.14.10.B — Bayesian Thompson sampling bandit (4 fields stamp-bound; rng_seed excluded — runtime-only state). */                                     \
    /* emit_when: cfg.bandit_algorithm != 0 (only emit when Thompson active; legacy stamps without these fields load with has_*=0 per Surface G). */         \
    X(bandit_algorithm,                    int,    "%d",     0,   cfg.bandit_algorithm,                                                                      \
        (cfg.bandit_algorithm != 0), DIRECT_FIELD)                                                                                                           \
    X(thompson_mu_prior,                   double, "%.17g",  0.0, FPN_ToDouble(cfg.thompson_mu_prior),                                                       \
        (cfg.bandit_algorithm != 0), DIRECT_FIELD)                                                                                                           \
    X(thompson_precision_prior,            double, "%.17g",  1.0, FPN_ToDouble(cfg.thompson_precision_prior),                                                \
        (cfg.bandit_algorithm != 0), DIRECT_FIELD)                                                                                                           \
    X(thompson_precision_obs,              double, "%.17g",  1.0, FPN_ToDouble(cfg.thompson_precision_obs),                                                  \
        (cfg.bandit_algorithm != 0), DIRECT_FIELD)

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
// [AUTO-POPULATE MACRO — v5.14.1.E.E.B]
//======================================================================================================
// Single-call auto-populate for production stamp emit at BacktestEngine
// (and any future production caller). Replaces 13 manual populator blocks
// with one X-macro expansion that:
//   - For each registry entry: evaluate emit_when at the call site
//   - When TRUE: set inf.has_<name> = 1 + inf.<name> = (type)(get_cfg_expr)
//   - When FALSE: leave both at zero-init defaults (legacy stamp shape)
//
// Caller usage:
//
//   void some_emit_function(StampInferenceCfgInputs& inf,
//                            const ControllerConfig<F>& cfg) {
//     STAMP_CFG_AUTOPOPULATE(inf, cfg);
//   }
//
// `inf` and `cfg` are the variable names the macro expansion uses.
// Caller MUST name their variables exactly `inf` and `cfg` (or wrap
// in a small block + alias). This eliminates the v5.9.5b production-
// caller field-population gap class — adding a new stamp-bound field
// becomes ONE line in FOREACH_STAMP_BOUND_CFG; the auto-populate
// expansion picks it up automatically next compile.
//======================================================================================================

#define STAMP_CFG_AUTOPOPULATE(inf, cfg)                                            \
    do {                                                                            \
        _Pragma("GCC diagnostic push")                                              \
        _Pragma("GCC diagnostic ignored \"-Wunused-value\"")                        \
        FOREACH_STAMP_BOUND_CFG(STAMP_CFG_AUTOPOPULATE_ONE)                         \
        _Pragma("GCC diagnostic pop")                                               \
    } while (0)

// v5.14.9.F.2 — Y3 dispatch: STAMP_CFG_AUTOPOPULATE_ONE token-pastes emit_source
// to dispatch to the appropriate handler. Both handlers share body shape today
// (snprintf with get_cfg expression); the registry tuple supports per-entry emit_source
// (DIRECT_FIELD or BITMAP_BIT) so future emit_source values can have different bodies
// without restructuring. Per DESIGN_SPECS/heterogeneous-registry-pattern.md Y3 dispatch canon.

#define STAMP_CFG_AUTOPOPULATE_ONE(name, type, fmt, default_val, get_cfg, emit_when, emit_source) \
    HANDLE_STAMP_EMIT_##emit_source(name, type, fmt, default_val, get_cfg, emit_when, emit_source)

#define HANDLE_STAMP_EMIT_DIRECT_FIELD(name, type, fmt, default_val, get_cfg, emit_when, _src) \
    if (emit_when) {                                                                            \
        (inf).has_##name = 1;                                                                   \
        (inf).name       = (type)(get_cfg);                                                     \
    }

#define HANDLE_STAMP_EMIT_BITMAP_BIT(name, type, fmt, default_val, get_cfg, emit_when, _src) \
    if (emit_when) {                                                                          \
        (inf).has_##name = 1;                                                                 \
        (inf).name       = (type)(get_cfg);                                                   \
    }

//======================================================================================================
// [TEST INSTRUMENTATION]
//======================================================================================================
// Compile-time field count for tests. Counts entries via macro counting.
// Used by tests to assert "all 10 expected fields are present in the
// registry" — catches accidental row deletion during refactors.
//======================================================================================================

#define STAMP_CFG_COUNT_ONE(name, type, fmt, default_val, get_cfg, emit_when, emit_source) +1
#define FOREACH_STAMP_BOUND_CFG_COUNT  (0 FOREACH_STAMP_BOUND_CFG(STAMP_CFG_COUNT_ONE))

#endif // STAMP_BOUND_CFG_REGISTRY_HPP
