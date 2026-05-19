// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [STAMP_ASSEMBLE_AND_EMIT — v5.15.3.A canonical helper]
//======================================================================================================
// Single canonical orchestration helper for assembling StampInferenceCfgInputs
// + emitting the stamp body. Used by both production callers:
//   - Backtest_RunFullValidation (single-horizon + multi-horizon-via-RFV)
//   - train_model_worker_fn (Train Model panel; training-only stamp)
//
// Closes the Class 18 mirror between these callers (PARITY-020): both
// previously had parallel ~200/95 LOC blocks of manual StampInferenceCfgInputs
// assembly; train_model_worker_fn was MISSING the STAMP_CFG_AUTOPOPULATE
// call that RFV had → 22 cfg-bound fields silently absent from Train Model
// stamps. After this helper extraction, both callers share the same
// canonical assembly path; future stamp-emit callers (e.g., batch CLI mode
// per TECH_DEBT-034) automatically get all stamp-bound cfg fields.
//
// CLOSES PARITY-020 (train_model_worker_fn AUTOPOPULATE gap) +
// PARITY-021 (grid_member_count orphan-placeholder bug; req_grid_* plumb
// through FullValidationResults sets them).
//
// PATTERN: orchestration-helper-with-pod-args (NEW DESIGN_SPECS draft
// v0.1 — see workspace `DESIGN_SPECS/orchestration-helper-with-pod-args-pattern.md`).
// One level above autopopulate-pattern-for-production-caller-class.md
// (which is companion-macro level). Here: function wraps AUTOPOPULATE +
// manual per-call population + external call. POD args struct with
// default member init lets caller specify only what differs per call.
//
// CROSS-REFERENCES:
//   - CLAUDE.md item 13 (X-macro registry — internal STAMP_CFG_AUTOPOPULATE walk)
//   - CLAUDE.md item 15 (parity-tested-by-construction — single assembly path)
//   - CLAUDE.md item 16 (reuse-audit — 2-3 callers share)
//   - CLAUDE.md item 19 (structural fix preferred — closes Class 18 mirror)
//   - CLAUDE.md item 21 (AUTOPOPULATE companion macro — internal call)
//   - CLAUDE.md item 27 (POD padding determinism — default member init)
//   - DESIGN_SPECS/autopopulate-pattern-for-production-caller-class.md (sister pattern)
//   - DESIGN_SPECS/structural-fix-preferred-decision-framework.md
//======================================================================================================
#ifndef STAMP_HELPER_HPP
#define STAMP_HELPER_HPP

#include <stdint.h>
#include <string.h>
#include <time.h>
#include "ModelInference.hpp"  // StampInferenceCfgInputs, stamp_write_for_model, STAMP_SET, MODEL_FORMAT_VERSION
#include "FeatureRegistry.hpp"  // FEATURE_REGISTRY_HASH, MODEL_NUM_FEATURES
#include "BuildFlags.hpp"      // tt::BUILD_FLAGS_HASH
#include "../Backtest/LabelFunctions.hpp"  // LABEL_REGISTRY_HASH, LabelType_NumClasses
#include "../Backtest/XGBHyperparams.hpp"  // XGBHyperparams_Defaults
#include "../CoreFrameworks/ControllerConfig.hpp"  // ControllerConfig
#include "../Version.hpp"  // ENGINE_VERSION_STRING
#include "../MemHeaders/CfgDerivedInferenceCfgRegistry.hpp"  // v5.15.5.A.7: INFERENCE_CFG_AUTOPOPULATE (closes TECH_DEBT-037 manual section 2a)

namespace tt {

//======================================================================================================
// [StampArgs<F> — POD struct for per-call inputs]
//======================================================================================================
// Default member init for all fields → callers fill only what differs.
// Single-horizon callers (train_model_worker_fn) leave horizon_* +
// grid_member_* at defaults. Multi-horizon callers (mh_run_one_horizon_fv
// → RFV) set them via FullValidationResults.req_grid_*.
//
// Per CLAUDE.md item 27: explicit defaults zero-init padding (POD struct
// is stack-allocated; not used in byte-equivalence contexts directly but
// the discipline applies for any future memcmp use).
//
// Per CLAUDE.local.md cohort-audit rule: cohort = function-arg struct;
// stack-allocated; no cache concerns.

template <unsigned F>
struct StampArgs {
    // === stamp_write_for_model invariants (caller-provided) ===
    int         format_version          = MODEL_FORMAT_VERSION;
    const char* trained_on_iso          = "";   // YYYY-MM-DD; if "" helper fills with today
    double      wf_metric               = 0.0;
    double      held_out_metric         = 0.0;  // 0.0 = training-only stamp (skip gap check)
    double      gap_threshold           = 0.0;  // 0.0 = training-only stamp
    int         force                   = 0;
    uint64_t    feature_registry_hash   = 0;    // 0 = helper fills with FEATURE_REGISTRY_HASH()
    const char* engine_version          = nullptr;  // nullptr = helper fills with ENGINE_VERSION_STRING

    // === Model-const per-call fields (training-time caller args) ===
    // Horizon (single-horizon callers leave at 0 → label_params skipped)
    int    horizon_ticks       = 0;
    double horizon_tp_pct      = 0.0;
    double horizon_sl_pct      = 0.0;

    // Grid identification (single-horizon: count=1, idx=0; multi: caller sets)
    int    grid_member_count   = 1;
    int    grid_member_idx     = 0;
    int    horizon_count       = 1;

    // Operator-set run name (empty = no run_name)
    const char* run_name       = "";

    // XGBoost hyperparam snap (operator panel OR cfg-derived per caller)
    int    snap_max_depth        = 6;
    double snap_learning_rate    = 0.1;
    int    snap_n_estimators     = 100;
    double snap_subsample        = 0.8;
    double snap_colsample_bytree = 0.8;
    int    snap_min_child_weight = 5;
    int    snap_seed             = 42;
    // Tree method as string (e.g., "hist", "exact", "approx", "auto"). Callers
    // with UI dropdown indices map idx→string at call site (single source of
    // truth for the lookup table lives at the caller). Empty = "hist" default.
    const char* snap_tree_method = "hist";
    int    snap_train_nthread    = 1;  // single-thread default per v5.10 determinism

    // Scaler binding (empty string = no scaler attached)
    const char* scaler_sha256_hex = "";

    // Label
    int label_kind = 0;

    // === Architectural fields (training-time identity) ===
    // Defaults zero = helper falls back to derive from label_kind / MODEL_NUM_FEATURES
    int         req_num_outputs   = 0;    // 0 = helper derives from label_kind
    const char* req_role          = "";   // "" = no expected_role emit
};

//======================================================================================================
// [Stamp_AssembleAndEmit — canonical helper]
//======================================================================================================
// Walks STAMP_CFG_AUTOPOPULATE for cfg-bound fields (closes PARITY-020 for
// callers that previously missed this call). Manually populates per-call
// model-const fields from StampArgs (NOT via the quarantined
// STAMP_MODEL_CONST_AUTOPOPULATE macro per PARITY-022). Computes today's
// ISO date if `args.trained_on_iso` is empty. Falls back to build-time
// constants for feature_registry_hash + engine_version when args defaults
// (allows callers to override but doesn't require it).
//
// Returns: StampWriteResult from stamp_write_for_model.

template <unsigned F>
inline StampWriteResult Stamp_AssembleAndEmit(
    const char* output_stamp_path,
    const char* hmac_secret,
    const ControllerConfig<F>& cfg,
    const StampArgs<F>& args) {

    StampInferenceCfgInputs inf = {};

    // ────────────────────────────────────────────────────────────────────
    // (1) CFG-bound fields — framework-driven via INFERENCE_CFG_POPULATE_FROM_DERIVED.
    // Walks master FOREACH_PER_CORE_CFG_FIELD + FOREACH_GLOBAL_CFG_FIELD
    // filtered by STAMP_BOUND_CFG_DERIVED metadata bit; populates inf.<field>
    // + has_<field> per the per-field cohort gate from cfg_gate::lookup_populate.
    // Closes PARITY-020: any future caller that calls this helper automatically
    // gets the cohort cfg-bound fields (ridge_*, composite_*, winsor_*, ml_*,
    // thompson_*, trading_mode, etc.) without needing to remember the call.
    // Migrated from legacy STAMP_CFG_AUTOPOPULATE at v5.15.5.F.4d.1.B.3 Step 1.6.5
    // (closes Class 18 mirror at inf-struct surface; cohort framework consolidation).
    // ────────────────────────────────────────────────────────────────────
    INFERENCE_CFG_POPULATE_FROM_DERIVED(inf, cfg);

    // ────────────────────────────────────────────────────────────────────
    // (2a) Legacy INFERENCE_CFG_AUTOPOPULATE call ELIMINATED at v5.15.5.F.4d.1.B.3 Step 1.5.
    //
    // Pre-Step-1.5: this section called INFERENCE_CFG_AUTOPOPULATE(inf, cfg)
    // which walked legacy MemHeaders/CfgDerivedInferenceCfgRegistry.hpp
    // (14 entries) and set PREFIXED `inf.inference_cfg_<name>` fields.
    //
    // Post-Step-1.5: legacy walker call REMOVED. The cfg-derived cohort
    // (post-Step-1.6.2 15-key scope: 9 thompson/.A.7 cohort + 1 standalone
    // bandit_blend_ratio + 5 model-state cohort + per_horizon_barrier_blend
    // ml_cfg_flag) is now fully covered by INFERENCE_CFG_POPULATE_FROM_DERIVED
    // at section (1) above (framework walker; sets UNPREFIXED `inf.<name>` per
    // master FOREACH_PER_CORE_CFG_FIELD + FOREACH_GLOBAL_CFG_FIELD +
    // FOREACH_ML_CFG_FLAG + FOREACH_GATE_CFG_FLAG filtered by
    // STAMP_BOUND_CFG_DERIVED bit). Drift impossible by construction.
    //
    // Legacy CfgDerivedInferenceCfgRegistry.hpp file + INFERENCE_CFG_AUTOPOPULATE
    // macro definition + legacy prefixed inf.inference_cfg_<name> struct fields
    // are DELETED at Step 2 (FORCED LAST in BUILD-FORCED sequencing). Until
    // then, prefixed fields exist on inf struct as transitional state but
    // are not populated (orphaned; cleared by `StampInferenceCfgInputs inf = {};`
    // at line 146).
    //
    // Closes Class 18 mirror at inf-struct surface for cfg-derived cohort
    // (production walker is single source of truth via framework).
    //
    // BITMAP_IS_SET STAMP_SET checks below preserve wire-byte-relevant has-flag
    // semantics. The bandit_blend_ratio prefixed has-flag controls legacy emit
    // walker's `inference_cfg_bandit_blend_ratio=X` wire key emission at
    // ModelInference.hpp:1817 FOREACH_STAMP_BOUND_CFG walker — REQUIRED at
    // Step 1.5 (Phase F trio NOT YET LANDED; legacy walker still active +
    // wire format must stay byte-preserved until SOFT bump). Removed at Step 2
    // when legacy POST_CFG entry + prefixed struct field deleted atomically.
    // The fees group has-flag is similarly wire-byte-relevant for fees emit.
    // ────────────────────────────────────────────────────────────────────
    if (BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_BANDIT_ENABLED)) {
        STAMP_SET(inf, inference_cfg_bandit_blend_ratio);
    }
    if (BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_COST_GATE_ENABLED)) {
        STAMP_SET(inf, fees);
    }

    // ────────────────────────────────────────────────────────────────────
    // (2b) Per-call model-const fields — manually populated from StampArgs.
    // STAMP_MODEL_CONST_AUTOPOPULATE is QUARANTINED (PARITY-022; see
    // ML_Headers/StampBoundModelConstRegistry.hpp). These fields come from
    // training-time caller args (not cfg auto-derivation), so manual
    // population from StampArgs is correct.
    // ────────────────────────────────────────────────────────────────────

    // Training poll interval (from cfg; same on all callers)
    STAMP_SET(inf, training_poll_interval);
    inf.training_poll_interval = cfg.poll_interval;

    // Model output count (from label_kind, or caller override)
    {
        STAMP_SET(inf, model_num_outputs);
        int K = (args.req_num_outputs > 0)
              ? args.req_num_outputs
              : LabelType_NumClasses(args.label_kind);
        inf.model_num_outputs = (K >= 2) ? K : 1;
    }

    // XGBoost hyperparams (operator-tunable per caller)
    {
        STAMP_SET(inf, xgb_hyperparams);
        inf.xgb_max_depth        = args.snap_max_depth;
        inf.xgb_learning_rate    = args.snap_learning_rate;
        inf.xgb_n_estimators     = args.snap_n_estimators;
        inf.xgb_subsample        = args.snap_subsample;
        inf.xgb_colsample_bytree = args.snap_colsample_bytree;
        inf.xgb_min_child_weight = args.snap_min_child_weight;
        inf.xgb_seed             = args.snap_seed;
        const char* tm = (args.snap_tree_method && args.snap_tree_method[0])
                       ? args.snap_tree_method : "hist";
        size_t tmln = strnlen(tm, sizeof(inf.xgb_tree_method) - 1);
        memcpy(inf.xgb_tree_method, tm, tmln);
        inf.xgb_tree_method[tmln] = '\0';
    }

    // XGBoost training thread count (forensic; v5.11.41 CRITICAL-2 close)
    STAMP_SET(inf, xgb_train_nthread);
    inf.xgb_train_nthread = args.snap_train_nthread > 0 ? args.snap_train_nthread : 1;

    // Build flags + label registry hashes (build-time identity)
    STAMP_SET(inf, build_flags_hash);
    inf.build_flags_hash = tt::BUILD_FLAGS_HASH();
    STAMP_SET(inf, label_registry_hash);
    inf.label_registry_hash = LABEL_REGISTRY_HASH();

    // Label params (only if caller provides horizon_ticks > 0; v5.11.41 CRITICAL-1 close)
    if (args.horizon_ticks > 0) {
        STAMP_SET(inf, label_params);
        inf.label_lookahead_ticks = args.horizon_ticks;
        inf.label_tp_pct          = args.horizon_tp_pct;
        inf.label_sl_pct          = args.horizon_sl_pct;
    }

    // Grid identification (PARITY-021 close — these were orphan-placeholder
    // fields in FOREACH_STAMP_BOUND_MODEL_CONST_PRE_CFG that no production
    // caller populated). Always emit; defaults grid_member_count=1 + idx=0
    // + horizon_count=1 for single-horizon callers.
    STAMP_SET(inf, grid_member);
    inf.grid_member_count = args.grid_member_count;
    inf.grid_member_idx   = args.grid_member_idx;

    // Scaler binding (only if caller provides scaler_sha256_hex)
    if (args.scaler_sha256_hex && args.scaler_sha256_hex[0]) {
        STAMP_SET(inf, scaler);
        inf.feature_scaler_present = 1;
        size_t shn = strnlen(args.scaler_sha256_hex,
                             sizeof(inf.scaler_sha256) - 1);
        memcpy(inf.scaler_sha256, args.scaler_sha256_hex, shn);
        inf.scaler_sha256[shn] = '\0';
    }

    // Run identification (only if caller provides run_name)
    if (args.run_name && args.run_name[0]) {
        STAMP_SET(inf, run_name);
        size_t rnln = strnlen(args.run_name, sizeof(inf.run_name) - 1);
        memcpy(inf.run_name, args.run_name, rnln);
        inf.run_name[rnln] = '\0';
    }

    // Architectural fields (training-time identity)
    if (args.req_num_outputs > 0) {
        STAMP_SET(inf, expected_num_classes);
        inf.expected_num_classes = args.req_num_outputs;
    }
    if (args.req_role && args.req_role[0]) {
        STAMP_SET(inf, expected_role);
        size_t rln = strnlen(args.req_role, sizeof(inf.expected_role) - 1);
        memcpy(inf.expected_role, args.req_role, rln);
        inf.expected_role[rln] = '\0';
    }
    // Build constants — always emit (training-time = build-time identity)
    STAMP_SET(inf, expected_num_features);
    inf.expected_num_features = (int)MODEL_NUM_FEATURES;
    STAMP_SET(inf, expected_feature_format_version);
    inf.expected_feature_format_version = (int)MODEL_FORMAT_VERSION;

    // ────────────────────────────────────────────────────────────────────
    // (3) Resolve caller-default fallbacks for stamp_write_for_model args.
    // ────────────────────────────────────────────────────────────────────

    // Today's ISO date if caller didn't provide
    char today_local[16] = {0};
    const char* trained_on = args.trained_on_iso;
    if (!trained_on || !trained_on[0]) {
        time_t now = time(NULL);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        strftime(today_local, sizeof(today_local), "%Y-%m-%d", &tm_buf);
        trained_on = today_local;
    }

    uint64_t feat_hash = (args.feature_registry_hash != 0)
                       ? args.feature_registry_hash
                       : FEATURE_REGISTRY_HASH();
    const char* eng_ver = (args.engine_version && args.engine_version[0])
                        ? args.engine_version
                        : ENGINE_VERSION_STRING;

    // ────────────────────────────────────────────────────────────────────
    // (4) Final emit via stamp_write_for_model (signature stable since
    // v5.14.8.A.merged; helper just wraps).
    // ────────────────────────────────────────────────────────────────────
    return stamp_write_for_model(
        output_stamp_path,
        hmac_secret,
        args.format_version,
        trained_on,
        args.wf_metric,
        args.held_out_metric,
        args.gap_threshold,
        args.force,
        feat_hash,
        eng_ver,
        /*inf=*/&inf);
}

}  // namespace tt

#endif  // STAMP_HELPER_HPP
