// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/CfgDriftCheckRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DETERMINISM] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[stamp<->cfg drift-check registry (v5.15.5.A.7) — 18 rows replace 14 manual if-blocks; 3-axis Y3 dispatch (severity/category/compare_kind)]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_CFG_DRIFT_CHECK]
//======================================================================================================
// X-macro registry for stamp↔cfg drift detection at model load + hot-swap +
// backtest validate chokepoints. Sister registry to FOREACH_ARCH_FIELD_DRIFT
// (MemHeaders/ArchFieldDriftRegistry.hpp v5.15.1) but for stamp-bound CFG
// fields rather than architectural hashes.
//
// CLOSES the Class 18 mirror at CoreFrameworks/ModelValidation.hpp where 14
// manual drift-check if-blocks (9 cross-binary WARN + 5 inference_cfg Tier 1/2)
// drifted apart with each new field addition (recurrence count well past 4×
// per structural-fix-preferred-decision-framework.md threshold).
//
// v5.15.5.A.7 additions (cohort-extended at registry END for parity-binding):
//   - ml_tp_pct, ml_sl_pct          — Tier 1 INFERENCE_CFG (PARITY-024 close)
//   - barrier_blend_mode            — Tier 1 INFERENCE_CFG (per-horizon dispatch)
//   - per_horizon_barrier_blend     — Tier 1 INFERENCE_CFG (master feature gate;
//                                      ml_cfg_flags bitmap-derived stamp value)
//
// PATTERNS (DESIGN_SPECS cross-refs):
//   - x-macro-registry-with-presence-dispatch.md    (Y3 token-paste dispatch)
//   - dual-axis-y3-dispatch-pattern.md              (tri-axis: severity × category × compare_kind)
//   - registry-tuple-as-single-source-of-truth.md   (Option D 10-col tuple)
//   - stamp-vs-runtime-drift-detection-registry.md  (canonical drift-detection pattern)
//   - bitmap-flag-api.md                            (per-category fail_mask SET on drift_flags_at_load)
//   - autopopulate-pattern-for-production-caller-class.md (entries auto-flow from FOREACH_STAMP_BOUND_CFG via STAMP_CFG_AUTOPOPULATE)
//   - template-deferred-dependency-injection.md     (caller injects log_fn for testability)
//   - cfg-flag-eligibility-criteria.md              (cohort-audit; ack flags migrated to ops_cfg_flags v5.15.5.A.7)
//
// CLAUDE.md cross-refs: items 13 (X-macro registry), 15 (parity-tested-by-
// construction), 17 (latency-tracked: slow-path/boot only), 18 (slow-path
// branchless mask compute), 19 (structural fix preferred), 20 (BITMAP_* API),
// 23 (type-trait dispatch via templated helpers).
//
// CONSUMERS (one chokepoint walker):
//   - CoreFrameworks/ModelValidation.hpp `NodeModelZoo_ValidateAgainstCfg<F, LogFn>`
//     replaces 14 manual if-blocks with one FOREACH_CFG_DRIFT_CHECK expansion.
//     Walker called from EngineSharded boot, hot-swap (ensemble + single-zoo),
//     and BacktestSharded validate paths (all 4 caller sites unchanged in
//     signature — registry refactor is boundary-stable).
//
// FORWARD-COMPAT (Surface G discipline per wire-format-byte-preservation-discipline.md):
//   gate_when expressions include STAMP_HAS() checks on the relevant has_<group>
//   flag — legacy stamps without the field skip the check silently. Drift bits
//   only set when stamp ACTUALLY has the field AND cfg differs. No false
//   positives on pre-v5.15.5 stamps.
//======================================================================================================
#ifndef CFG_DRIFT_CHECK_REGISTRY_HPP
#define CFG_DRIFT_CHECK_REGISTRY_HPP

#include <stdint.h>
#include <math.h>      // fabs for EPS_DEFAULT comparison
#include <string.h>    // strcmp for STRING comparison
#include "../MemHeaders/BitmapMacros.hpp"        // BITMAP_IS_SET / BITMAP_SET
#include "../MemHeaders/FailureModeRegistry.hpp" // FAILURE_MASK_cfg_binding_drift + FAILURE_MASK_cfg_cross_binary_drift
#include "../CoreFrameworks/OpsCfgFlagRegistry.hpp" // MASK_OPS_CFG_ACKNOWLEDGE_*_DRIFT for ack-flag dispatch
#include "../CoreFrameworks/GateCfgFlagRegistry.hpp" // MASK_GATE_CFG_* for barrier_gate_enabled stamp value
#include "MlCfgFlagRegistry.hpp"                  // MASK_ML_CFG_* for bandit/per_horizon_barrier_blend
#include "BuildFlags.hpp"                          // tt::BUILD_FLAGS_HASH() for build_flags_hash compare
#include "StampBoundModelConstRegistry.hpp"       // STAMP_HAS macro (BITMAP_IS_SET on h->has_flags)

//======================================================================================================
// [REGISTRY]_[FOREACH_CFG_DRIFT_CHECK]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DETERMINISM]]
// [REFERENCE]_[DESIGN_SPEC]_[dual-axis-y3-dispatch-pattern]
// [REFERENCE]_[CLASS]_[18]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[10-col tuple; Y3 axes: SEVERITY (WARN/TIER1/TIER2) x CATEGORY x COMPARE_KIND; 18 entries + count helper + test instrumentation]
// [COLUMN]_[10-col tuple]_[see the entry-shape doc directly below]
// [REFERENCE]_[PARITY]_[[PARITY-24] [PARITY-26]]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-9]
//======================================================================
// [CODE]
//======================================================================
// REGISTRY ENTRY SHAPE — 10-col tuple
//======================================================================================================
// X(NAME, type, severity, category, compare_kind,
//   get_stamp_expr, get_cfg_expr, gate_when, fail_mask, doc)
//
//   NAME           — diagnostic label + canonical name (lowercase snake_case;
//                    used in log messages via stringification).
//   type           — C++ type token. Used for any future templated-dispatch
//                    helpers (mirrors tt::stamp_parse_field<T> pattern per
//                    CLAUDE.md item 23). Today, used for documentation +
//                    auditability; comparison shape driven by compare_kind.
//   severity       — Y3 token: WARN_ALWAYS | REFUSE_STRICT.
//                    Drives tier counter (++tier1_count vs ++tier2_count) +
//                    REFUSE-in-strict return path.
//   category       — Y3 token: INFERENCE_CFG | CROSS_BINARY.
//                    Drives ack-flag gate (which ops_cfg_flags bit suppresses)
//                    + per-category drift bit (cfg_binding_drift vs
//                    cfg_cross_binary_drift).
//   compare_kind   — Y3 token: EXACT | EPS_DEFAULT | STRING.
//                    Drives comparison shape (`!=` vs `fabs > 1e-6` vs `strcmp`).
//                    Future variants (EPS_TIGHT, EPS_LOOSE, HASH64, etc.) add
//                    1 new HANDLE_DRIFT_CMP_<token> macro each.
//   get_stamp_expr — expression at caller scope to extract stamp value.
//                    Caller's variable name for ModelHandle ptr MUST be `h`.
//                    e.g., `h->confidence_threshold_scale`
//   get_cfg_expr   — expression at caller scope to extract cfg value.
//                    Caller's variable name for ControllerConfig MUST be `cfg`.
//                    e.g., `FPN_ToDouble(cfg.confidence_threshold_scale)`
//                    For bitmap-derived: `BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_...) ? 1 : 0`
//   gate_when      — boolean expression at caller scope. Skip check if false.
//                    Typically `STAMP_HAS(*h, <group>)` for forward-compat
//                    (Surface G: legacy stamps without has_<group>=1 skip).
//                    Can compound with cfg-side enable gates (`STAMP_HAS(*h, fees) &&
//                    BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_COST_GATE_ENABLED)`).
//   fail_mask      — FAILURE_MASK_* constant for `drift_flags_at_load uint16_t`
//                    bit-set on detection. Per-CATEGORY bit (not per-entry —
//                    uint16_t headroom doesn't fit 14+ per-entry bits today).
//                    INFERENCE_CFG entries: FAILURE_MASK_cfg_binding_drift.
//                    CROSS_BINARY entries: FAILURE_MASK_cfg_cross_binary_drift.
//   doc            — short operator-facing description (cfg.example + audit).
//
// HOW THE WALKER COMPOSES (per dual-axis-y3-dispatch-pattern.md):
//
//   For each entry, the walker generates:
//     if (gate_when) {
//         auto stamp_val = get_stamp_expr;  // resolves at caller scope
//         auto cfg_val   = get_cfg_expr;
//         if (HANDLE_DRIFT_CMP_##compare_kind(stamp_val, cfg_val)) {
//             // Drift detected — check category-specific ack flag:
//             if (!HANDLE_DRIFT_CATEGORY_##category_ACK(cfg)) {
//                 // Not ack'd: log + bump counter + set bit
//                 log_fn(...);
//                 HANDLE_DRIFT_SEVERITY_##severity(strict, t1, t2, t1r);
//                 BITMAP_SET(h->drift_flags_at_load, fail_mask);
//             }
//         }
//     }
//
//   Three Y3 axes (severity × category × compare_kind) compose independently.
//   Adding a new value to any axis = 1 new HANDLE_DRIFT_<AXIS>_<token> macro;
//   existing entries unchanged.
//======================================================================================================

//======================================================================================================
// Y3 DISPATCH AXIS 1 — SEVERITY
//======================================================================================================
// Drives tier counter mutation + REFUSE-in-strict return path. Two values today;
// future could add FORENSIC_ONLY (no counter, log-only).

#define HANDLE_DRIFT_SEVERITY_WARN_ALWAYS(strict, t1, t2, t1r) \
    do { ++(t2); (void)(strict); (void)(t1); (void)(t1r); } while (0)

#define HANDLE_DRIFT_SEVERITY_REFUSE_STRICT(strict, t1, t2, t1r) \
    do { ++(t1); if (strict) ++(t1r); (void)(t2); } while (0)

//======================================================================================================
// Y3 DISPATCH AXIS 2 — CATEGORY
//======================================================================================================
// Drives ack-flag dispatch + per-category drift bit selection. Two values today;
// future could add SCALER_CFG (when scaler cfg fields grow beyond binding hash).
//
// Each category resolves to (a) which ops_cfg_flags bit suppresses the WARN
// (operator ack escape) and (b) which FAILURE_MASK_* bit gets set on
// drift_flags_at_load when drift detected.
//
// Cohort-migrated at v5.15.5.A.7: acknowledge_inference_cfg_drift +
// acknowledge_cross_binary_version_drift moved from direct int cfg fields to
// ops_cfg_flags bitmap (closes TECH_DEBT-009 boolean-orphan tail).

#define HANDLE_DRIFT_CATEGORY_INFERENCE_CFG_ACK(cfg) \
    BITMAP_IS_SET((cfg).ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_INFERENCE_CFG_DRIFT)

#define HANDLE_DRIFT_CATEGORY_CROSS_BINARY_ACK(cfg) \
    BITMAP_IS_SET((cfg).ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT)

#define HANDLE_DRIFT_CATEGORY_INFERENCE_CFG_FAIL_MASK  FAILURE_MASK_cfg_binding_drift
#define HANDLE_DRIFT_CATEGORY_CROSS_BINARY_FAIL_MASK   FAILURE_MASK_cfg_cross_binary_drift

//======================================================================================================
// Y3 DISPATCH AXIS 3 — COMPARE_KIND
//======================================================================================================
// Drives comparison shape per-entry. Three values today (EXACT for int/uint
// non-noisy; EPS_DEFAULT for double with 1e-6 absolute epsilon; STRING for
// char[N] arrays). Future: EPS_TIGHT (1e-9), EPS_LOOSE (1e-3), EPS_RELATIVE
// (fabs > 1e-6 * max(|a|,|b|)), HASH64 (explicit uint64 hash compare).
//
// Adding a new compare_kind = 1 new HANDLE_DRIFT_CMP_<TOKEN> macro definition;
// zero existing entries change. Walker auto-flows.

#define HANDLE_DRIFT_CMP_EXACT(stamp, cfg)        ((stamp) != (cfg))
#define HANDLE_DRIFT_CMP_EPS_DEFAULT(stamp, cfg)  (fabs((double)(stamp) - (double)(cfg)) > 1e-6)
#define HANDLE_DRIFT_CMP_STRING(stamp, cfg)       (strcmp((stamp), (cfg)) != 0)

//======================================================================================================
// FOREACH_CFG_DRIFT_CHECK — 18 entries
//======================================================================================================
// Walker composes the 3 Y3 axes per entry. See CoreFrameworks/ModelValidation.hpp
// `NodeModelZoo_ValidateAgainstCfg<F, LogFn>` for the chokepoint consumer.
//
// Entries are grouped by category for readability but the order within FOREACH
// does NOT affect runtime correctness (each entry is independent).
//
// HMAC byte-preservation note: this registry READS stamp body fields. The stamp
// body itself is defined by FOREACH_STAMP_BOUND_CFG; adding a new drift entry
// here requires the corresponding stamp-binding row in StampBoundCfgRegistry.hpp
// (appended AT END per wire-format-byte-preservation-discipline.md).

#define FOREACH_CFG_DRIFT_CHECK(X)                                                                                                                                                                                                                                              \
    /* ====== CROSS_BINARY category (WARN-only; suppressed by acknowledge_cross_binary_version_drift) ====== */                                                                                                                                                                  \
    /* v5.14.1.B precedent: training_poll_interval — model trained at different poll cadence may diverge bytewise. */                                                                                                                                                            \
    X(training_poll_interval,                uint32_t, WARN_ALWAYS,    CROSS_BINARY,    EXACT,                                                                                                                                                                                   \
      h->training_poll_interval,             cfg.poll_interval,                                                                                                                                                                                                                  \
      STAMP_HAS(*h, training_poll_interval), FAILURE_MASK_cfg_cross_binary_drift,                                                                                                                                                                                                \
      "stamp training_poll_interval vs cfg.poll_interval; cross-binary forensic only (model already trained)")                                                                                                                                                                  \
    /* XGBoost hyperparams block (5 sub-checks; v5.9.4 + v5.9.5h). */                                                                                                                                                                                                            \
    X(xgb_subsample,                         double,   WARN_ALWAYS,    CROSS_BINARY,    EPS_DEFAULT,                                                                                                                                                                             \
      h->xgb_subsample,                      FPN_ToDouble(cfg.xgb_subsample),                                                                                                                                                                                                    \
      STAMP_HAS(*h, xgb_hyperparams),        FAILURE_MASK_cfg_cross_binary_drift,                                                                                                                                                                                                \
      "stamp xgb_subsample vs cfg; cross-binary forensic")                                                                                                                                                                                                                       \
    X(xgb_colsample_bytree,                  double,   WARN_ALWAYS,    CROSS_BINARY,    EPS_DEFAULT,                                                                                                                                                                             \
      h->xgb_colsample_bytree,               FPN_ToDouble(cfg.xgb_colsample_bytree),                                                                                                                                                                                             \
      STAMP_HAS(*h, xgb_hyperparams),        FAILURE_MASK_cfg_cross_binary_drift,                                                                                                                                                                                                \
      "stamp xgb_colsample_bytree vs cfg; cross-binary forensic")                                                                                                                                                                                                                \
    X(xgb_min_child_weight,                  int,      WARN_ALWAYS,    CROSS_BINARY,    EXACT,                                                                                                                                                                                   \
      h->xgb_min_child_weight,               cfg.xgb_min_child_weight,                                                                                                                                                                                                           \
      STAMP_HAS(*h, xgb_hyperparams),        FAILURE_MASK_cfg_cross_binary_drift,                                                                                                                                                                                                \
      "stamp xgb_min_child_weight vs cfg; cross-binary forensic")                                                                                                                                                                                                                \
    X(xgb_seed,                              int,      WARN_ALWAYS,    CROSS_BINARY,    EXACT,                                                                                                                                                                                   \
      h->xgb_seed,                           cfg.xgb_seed,                                                                                                                                                                                                                       \
      STAMP_HAS(*h, xgb_hyperparams),        FAILURE_MASK_cfg_cross_binary_drift,                                                                                                                                                                                                \
      "stamp xgb_seed vs cfg; cross-binary forensic")                                                                                                                                                                                                                            \
    X(xgb_tree_method,                       char[16], WARN_ALWAYS,    CROSS_BINARY,    STRING,                                                                                                                                                                                  \
      h->xgb_tree_method,                    cfg.xgb_tree_method,                                                                                                                                                                                                                \
      STAMP_HAS(*h, xgb_hyperparams),        FAILURE_MASK_cfg_cross_binary_drift,                                                                                                                                                                                                \
      "stamp xgb_tree_method vs cfg; cross-binary forensic (string compare)")                                                                                                                                                                                                    \
    /* Build flags hash — cross-binary forensic; ALSO covered by FOREACH_ARCH_FIELD_DRIFT (sets per-entry bit).  */                                                                                                                                                              \
    /* Inclusion here drives the WARN log + per-category bit; complementary surface (not duplicate detection).  */                                                                                                                                                              \
    X(build_flags_hash,                      uint64_t, WARN_ALWAYS,    CROSS_BINARY,    EXACT,                                                                                                                                                                                   \
      h->build_flags_hash,                   tt::BUILD_FLAGS_HASH(),                                                                                                                                                                                                             \
      STAMP_HAS(*h, build_flags_hash),       FAILURE_MASK_cfg_cross_binary_drift,                                                                                                                                                                                                \
      "stamp build_flags_hash vs current compile build; cross-binary forensic (predictions may diverge)")                                                                                                                                                                        \
    /* v5.11.42 D.1 — xgb_train_nthread mode-divergence forensic (parallel vs serial training).  */                                                                                                                                                                              \
    X(xgb_train_nthread,                     int,      WARN_ALWAYS,    CROSS_BINARY,    EXACT,                                                                                                                                                                                   \
      h->xgb_train_nthread,                  cfg.xgb_train_nthread,                                                                                                                                                                                                              \
      STAMP_HAS(*h, xgb_train_nthread),      FAILURE_MASK_cfg_cross_binary_drift,                                                                                                                                                                                                \
      "stamp xgb_train_nthread vs cfg; parallel(=1) vs serial(>1) mode divergence; bytewise retrain would diverge")                                                                                                                                                              \
    /* ====== INFERENCE_CFG category Tier 1 (REFUSE in strict; suppressed by acknowledge_inference_cfg_drift) ====== */                                                                                                                                                          \
    /* v5.9.5i precedent — directly affects serving math; strict-mode REFUSE is correct (silent prediction drift). */                                                                                                                                                            \
    X(confidence_threshold_scale,            double,   REFUSE_STRICT,  INFERENCE_CFG,   EPS_DEFAULT,                                                                                                                                                                             \
      FPN_ToDouble(h->confidence_threshold_scale), FPN_ToDouble(cfg.confidence_threshold_scale),                                                                                                                                                                   \
      STAMP_HAS(*h, inference_cfg),          FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                     \
      "Tier 1 confidence threshold scale drift (REFUSE in strict; affects model fire-prob threshold)")                                                                                                                                                                           \
    X(barrier_gate_enabled,                  int,      REFUSE_STRICT,  INFERENCE_CFG,   EXACT,                                                                                                                                                                                   \
      h->barrier_gate_enabled, (BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_BARRIER_GATE_ENABLED) ? 1 : 0),                                                                                                                                                    \
      STAMP_HAS(*h, inference_cfg),          FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                     \
      "Tier 1 barrier_gate_enabled drift (REFUSE in strict; affects barrier compute path)")                                                                                                                                                                                      \
    /* ====== INFERENCE_CFG category Tier 2 (WARN always) ====== */                                                                                                                                                                                                              \
    X(confidence_hard_block_threshold,       double,   WARN_ALWAYS,    INFERENCE_CFG,   EPS_DEFAULT,                                                                                                                                                                             \
      FPN_ToDouble(h->confidence_hard_block_threshold), FPN_ToDouble(cfg.confidence_hard_block_threshold),                                                                                                                                                         \
      STAMP_HAS(*h, inference_cfg),          FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                     \
      "Tier 2 confidence_hard_block_threshold drift (WARN; advisory only)")                                                                                                                                                                                                      \
    X(bandit_blend_ratio,                    double,   WARN_ALWAYS,    INFERENCE_CFG,   EPS_DEFAULT,                                                                                                                                                                             \
      FPN_ToDouble(h->bandit_blend_ratio),   FPN_ToDouble(cfg.bandit_blend_ratio),                                                                                                                                                                                                \
      COHORT_GATE_BANDIT_ENABLED, FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                                \
      "Tier 2 bandit_blend_ratio drift (gated by bandit_enabled cohort; WARN)")                                                                                                                                                                                                  \
    /* === v5.15.5.F.4d PARITY-026 close — 4 STAMP_BOUND bandit/thompson fields since v5.14.10.B were missing drift-check rows + 1 NEW field for BLENDED state === */ \
    /* Post-.B.3 Step 1.6.6.a + Step 6.10 (2026-05-24): 4 gate substitutions to COHORT_GATE_BANDIT_BLEND_STATE_4 + COHORT_GATE_PER_HORIZON_BARRIER + 5 BANDIT_ENABLED substitutions to COHORT_GATE_BANDIT_ENABLED + 2 COST_GATE_ENABLED substitutions to COHORT_GATE_COST_GATE_ENABLED. STAMP_HAS group-bit gate intentionally dropped: drift now fires when cfg cohort is enabled regardless of legacy-stamp absence — correctly catches "trained without feature, now using feature" parity break. Per [[feedback_no_defer_for_effort]] + Path γ #3 MOSTLY close. */ \
    X(bandit_algorithm,                      int,      WARN_ALWAYS,    INFERENCE_CFG,   EXACT,                                                                                                                                                                                   \
      h->bandit_algorithm,                   cfg.bandit_algorithm,                                                                                                                                                                                                               \
      COHORT_GATE_BANDIT_ENABLED, FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                                \
      "Tier 2 bandit_algorithm enum drift (WARN to avoid false-positive on legacy cfg=2 stamps post-.F.4d Option C semantic flip; gate: bandit-enabled cohort)")                                                                                                                 \
    X(thompson_mu_prior,                     double,   REFUSE_STRICT,  INFERENCE_CFG,   EPS_DEFAULT,                                                                                                                                                                             \
      FPN_ToDouble(h->thompson_mu_prior),    FPN_ToDouble(cfg.thompson_mu_prior),                                                                                                                                                                                                \
      COHORT_GATE_BANDIT_ENABLED, FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                                \
      "Tier 1 thompson_mu_prior drift (parity-critical; posterior mean prior)")                                                                                                                                                                                                  \
    X(thompson_precision_prior,              double,   REFUSE_STRICT,  INFERENCE_CFG,   EPS_DEFAULT,                                                                                                                                                                             \
      FPN_ToDouble(h->thompson_precision_prior), FPN_ToDouble(cfg.thompson_precision_prior),                                                                                                                                                                                         \
      COHORT_GATE_BANDIT_ENABLED, FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                                \
      "Tier 1 thompson_precision_prior drift (parity-critical; posterior precision prior)")                                                                                                                                                                                      \
    X(thompson_precision_obs,                double,   REFUSE_STRICT,  INFERENCE_CFG,   EPS_DEFAULT,                                                                                                                                                                             \
      FPN_ToDouble(h->thompson_precision_obs), FPN_ToDouble(cfg.thompson_precision_obs),                                                                                                                                                                                           \
      COHORT_GATE_BANDIT_ENABLED, FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                                \
      "Tier 1 thompson_precision_obs drift (parity-critical; observation precision)")                                                                                                                                                                                            \
    X(thompson_exp3_blend_alpha,             double,   REFUSE_STRICT,  INFERENCE_CFG,   EPS_DEFAULT,                                                                                                                                                                             \
      FPN_ToDouble(h->thompson_exp3_blend_alpha), FPN_ToDouble(cfg.thompson_exp3_blend_alpha),                                                                                                                                                                                        \
      COHORT_GATE_BANDIT_BLEND_STATE_4, FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                          \
      "Tier 1 thompson_exp3_blend_alpha drift (gated to BLENDED state cfg=4; reproducibility requires α locked to training-time value)")                                                                                                                                         \
    X(fee_rate_maker,                        double,   WARN_ALWAYS,    INFERENCE_CFG,   EPS_DEFAULT,                                                                                                                                                                             \
      Money_ToDouble(h->fee_rate_maker),       Money_ToDouble(cfg.fee_rate_maker),                                                                                                                                                                                                   \
      COHORT_GATE_COST_GATE_ENABLED, FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                             \
      "Tier 2 fee_rate_maker drift (gated by cost_gate_enabled cohort; WARN)")                                                                                                                                                                                                   \
    X(fee_rate_taker,                        double,   WARN_ALWAYS,    INFERENCE_CFG,   EPS_DEFAULT,                                                                                                                                                                             \
      Money_ToDouble(h->fee_rate_taker),       Money_ToDouble(cfg.fee_rate_taker),                                                                                                                                                                                                   \
      COHORT_GATE_COST_GATE_ENABLED, FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                             \
      "Tier 2 fee_rate_taker drift (gated by cost_gate_enabled cohort; WARN)")                                                                                                                                                                                                   \
    /* ====== INFERENCE_CFG category Tier 1 — v5.15.5.A.7 PARITY-024 cohort (per-horizon barrier serving) ====== */                                                                                                                                                              \
    /* All 4 entries map to FOREACH_STAMP_BOUND_CFG appendix rows (lines 177-189). gate_when uses STAMP_HAS(*h,    */                                                                                                                                                            \
    /* <field>) for Surface G forward-compat — legacy v5.15.4- stamps without these fields skip silently.        */                                                                                                                                                              \
    /* per_horizon_barrier_blend is master ON/OFF (always checked when has flag set); other 3 gated additionally   */                                                                                                                                                            \
    /* by the feature being enabled in cfg (operator turning feature off → model trained with feature ON →         */                                                                                                                                                            \
    /* per_horizon_barrier_blend Tier 1 drift fires; ml_tp_pct/sl_pct/blend_mode checks gated by feature-on        */                                                                                                                                                            \
    /* because mismatched-while-disabled is moot, mismatched-while-enabled is the parity violation).               */                                                                                                                                                            \
    /* For the 4 .A.7 entries, gate_when uses STAMP_HAS(*h, inference_cfg) — the existing  */                                                                                                                                                                                  \
    /* MODEL_CONST group flag (set whenever any inference_cfg_* field is present in stamp). */                                                                                                                                                                                  \
    /* No per-field has_* flag for these new entries (they share has_inference_cfg). The     */                                                                                                                                                                                 \
    /* additional BITMAP_IS_SET(..., MASK_ML_CFG_PER_HORIZON_BARRIER_BLEND) gate on the cfg   */                                                                                                                                                                                 \
    /* side scopes the check to "feature enabled in current cfg" — preserves Surface G       */                                                                                                                                                                                 \
    /* forward-compat: legacy stamps without per-horizon fields are 0.0 at h->inference_cfg_*;*/                                                                                                                                                                                 \
    /* if cfg has feature OFF (default), gate is false, no false-positive drift. If cfg has   */                                                                                                                                                                                \
    /* feature ON with legacy stamp, drift DOES fire (Tier 1 REFUSE) — that's the intentional */                                                                                                                                                                                \
    /* parity catch: operator enabled new feature on a model trained without it.              */                                                                                                                                                                                \
    X(ml_tp_pct,                             double,   REFUSE_STRICT,  INFERENCE_CFG,   EPS_DEFAULT,                                                                                                                                                                             \
      Money_ToDouble(h->ml_tp_pct),            Money_ToDouble(cfg.ml_tp_pct),                                                                                                                                                                                                        \
      COHORT_GATE_PER_HORIZON_BARRIER, FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                           \
      "Tier 1 ml_tp_pct drift (gated by per_horizon_barrier_blend feature; REFUSE in strict — silent miscalibration risk)")                                                                                                                                                      \
    X(ml_sl_pct,                             double,   REFUSE_STRICT,  INFERENCE_CFG,   EPS_DEFAULT,                                                                                                                                                                             \
      Money_ToDouble(h->ml_sl_pct),            Money_ToDouble(cfg.ml_sl_pct),                                                                                                                                                                                                        \
      COHORT_GATE_PER_HORIZON_BARRIER, FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                           \
      "Tier 1 ml_sl_pct drift (gated by per_horizon_barrier_blend feature; REFUSE in strict — silent miscalibration risk)")                                                                                                                                                      \
    X(barrier_blend_mode,                    int,      REFUSE_STRICT,  INFERENCE_CFG,   EXACT,                                                                                                                                                                                   \
      h->barrier_blend_mode,                 cfg.barrier_blend_mode,                                                                                                                                                                                                             \
      COHORT_GATE_PER_HORIZON_BARRIER, FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                           \
      "Tier 1 barrier_blend_mode drift (gated by per_horizon_barrier_blend feature; REFUSE in strict — dispatch shape mismatch)")                                                                                                                                                \
    X(per_horizon_barrier_blend,             int,      REFUSE_STRICT,  INFERENCE_CFG,   EXACT,                                                                                                                                                                                   \
      h->per_horizon_barrier_blend,          (BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_PER_HORIZON_BARRIER_BLEND) ? 1 : 0),                                                                                                                                                   \
      STAMP_HAS(*h, inference_cfg), FAILURE_MASK_cfg_binding_drift,                                                                                                                                                                                                              \
      "Tier 1 per_horizon_barrier_blend master gate drift (REFUSE in strict — model calibrated under different feature regime)")

//======================================================================================================
// TEST INSTRUMENTATION
//======================================================================================================
// Compile-time field count for tests + audit. Counts entries via macro counting.
// Used by tests to assert "all N expected fields are present" — catches accidental
// row deletion during refactors.

#define CFG_DRIFT_CHECK_COUNT_ONE(name, type, severity, category, compare_kind, get_stamp, get_cfg, gate_when, fail_mask, doc) +1
#define FOREACH_CFG_DRIFT_CHECK_COUNT  (0 FOREACH_CFG_DRIFT_CHECK(CFG_DRIFT_CHECK_COUNT_ONE))

//======================================================================
// [END_CODE]
//======================================================================
// [END_REGISTRY]_[FOREACH_CFG_DRIFT_CHECK]
//======================================================================
#endif // CFG_DRIFT_CHECK_REGISTRY_HPP
