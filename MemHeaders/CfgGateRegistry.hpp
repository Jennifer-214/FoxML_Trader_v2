// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
//======================================================================================================
// [FILE]_[MemHeaders/CfgGateRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [FRAMEWORK_DISCIPLINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the cfg-derived consumer framework home — sparse gate_when sidecars (H18 first canonical) + the 4-registry STAMP_BOUND_DERIVED_COHORT meta-walker + 4 consumer template fns + struct-gen]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_CFG_GATE_PER_NODE]   (+ FOREACH_CFG_GATE_GLOBAL sister rides)
//   - [FUNCTION]_[lookup_populate]   (+ lookup_drift; namespace cfg_gate)
//   - [MACRO]_[FOREACH_STAMP_BOUND_DERIVED_COHORT]
//   - [FUNCTION]_[populate_inference_cfg_from_derived]
//   - [FUNCTION]_[populate_stamp_cfg_from_derived]
//   - [FUNCTION]_[drift_check_from_derived]
//   - [FUNCTION]_[parse_stamp_cfg_to_derived]
//   - [MACRO]_[STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN]   (+ exclusion sidecar + _STAMP_RESULT_* helpers + PARSE wrapper)
//   - [MACRO]_[*_FROM_DERIVED wrappers]
// [REFERENCE]_[DESIGN_SPEC]_[[cfg-derived-consumer-framework] [sidecar-override-pattern-for-registry-auto-flows] [metadata-bit-driven-derived-filter-framework]]
// [REFERENCE]_[INVARIANT]_[[H18] [H15] [H19]]
//======================================================================================================
// v5.15.5.F.4d.1.B.1 — Step 1 of framework consolidation.
//
// Pattern: DESIGN_SPECS/sidecar-override-pattern-for-registry-auto-flows.md Stage 3 ACTIVE
//   (FIRST CANONICAL of gate-type sidecar; sister to FOREACH_DRIFT_OVERRIDE planned at .C
//   which is severity/category-type sidecar — different concerns, same H18 pattern shape).
//
// Framework: DESIGN_SPECS/cfg-derived-consumer-framework.md Stage 3 first reference at this ship.
//
// Discipline: DESIGN_SPECS/canonical-sister-extension-discipline.md
//   (caught Path γ-class structural critique #2 at .B planning 2026-05-17;
//   this file is the canonical structural close of CRIT-1 wider-scope acceptance).
//
//======================================================================================================
// WHAT THIS REGISTRY ENCODES
//======================================================================================================
//
// Each row in master FOREACH_PER_NODE_CFG_FIELD + FOREACH_GLOBAL_CFG_FIELD that flags the
// STAMP_BOUND_CFG_DERIVED metadata bit can OPTIONALLY have a custom gate_when expression
// here. Default gates apply if no entry exists:
//
//   For populate consumers (INFERENCE_CFG_POPULATE_FROM_DERIVED, STAMP_CFG_POPULATE_FROM_DERIVED):
//     DEFAULT gate = true (always populate per always-emit canonical Q3.G)
//
//   For drift consumers (DRIFT_CHECK_FROM_DERIVED):
//     DEFAULT gate = stamp_has_inference_cfg (drift check fires when stamp's inference_cfg present)
//
// Per-cohort gates that DIFFER from default (POPULATED at .B.2 — the shared COHORT_GATE_*
// macros are the SSoT, defined at ML_Headers/MlCfgFlagRegistry.hpp; the Bandit/Thompson gate
// is `(cfg.bandit_algorithm != 0)` matching the legacy emit_when semantic — the BITMAP-bit
// form was tried first and REJECTED for wire-byte parity, see the in-registry note):
//   Bandit/Thompson cohort → COHORT_GATE_BANDIT_THOMPSON  (cfg.bandit_algorithm != 0)
//   BLENDED state-4        → COHORT_GATE_BANDIT_BLEND_STATE_4  (== 4)
//   Ridge cohort           → COHORT_GATE_RIDGE_ANY  (either ridge ml_cfg_flags bit)
//   Composite confidence   → COHORT_GATE_COMPOSITE_CONFIDENCE  (the composite ml_cfg_flags bit)
//   Soft-risk degradation  → COHORT_GATE_SOFTRISK_ENABLED  (cfg.risk_degradation_curve != 0)
//
//======================================================================================================
// HISTORY: at .B.1 both sidecar registries shipped EMPTY (framework-first). .B.2 cohort
// migration populated FOREACH_CFG_GATE_PER_NODE with 16 cohort-gated entries; _GLOBAL
// remains deliberately empty (its rows use the default always-emit gate — see its note).
//======================================================================================================

#pragma once

#include "../CoreFrameworks/CfgFieldRegistry.hpp"      // CfgFieldDescriptor + FIELD_IDX_<NAME>
#include "../CoreFrameworks/ControllerConfig.hpp"      // ControllerConfig<F>
#include "../ML_Headers/MlCfgFlagRegistry.hpp"         // FOREACH_ML_CFG_FLAG + MASK_ML_CFG_* (v5.15.5.F.4d.1.B.3 Step 1.6.5b — self-contained header per WIP2d-1.B.0 Shortsighted #5 close)
#include "../CoreFrameworks/GateCfgFlagRegistry.hpp"   // FOREACH_GATE_CFG_FLAG + MASK_GATE_CFG_* (v5.15.5.F.4d.1.B.3 Step 1.6.5b)
#include "BitmapMacros.hpp"                            // BITMAP_IS_SET (Step 1.6.5b — used inside consumer X-macros for bitmap-bit cohort registries)
#include <cstddef>                                      // size_t
#include <cstdio>                                       // snprintf (canonical body emit)
#include <cstring>                                      // strcmp (parser dispatch)
#include <cstdlib>                                      // atoi (parser dispatch)

//======================================================================
// [REGISTRY]_[FOREACH_CFG_GATE_PER_NODE]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the sparse gate_when sidecars (H18 first canonical; FOREACH_CFG_GATE_GLOBAL sister rides) — 16 per-node cohort-gated entries; no entry = default gate applies]
// [COLUMN]_[name]_[must match a FOREACH_PER_NODE_CFG_FIELD / FOREACH_GLOBAL_CFG_FIELD row with the STAMP_BOUND_CFG_DERIVED bit set]
// [COLUMN]_[gate_when_expr]_[C++ bool expression over cfg (typed ControllerConfig<F>); evaluated at consumer expansion (slow-path / stamp-emit cadence)]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_CFG_GATE_PER_NODE(X) \
    /* Bandit/Thompson cohort — MATCHES legacy FOREACH_STAMP_BOUND_CFG emit_when semantic \
     * (cfg.bandit_algorithm != 0) at lines 162-170. Coding-time discovery: my initial \
     * BITMAP_IS_SET(MASK_ML_CFG_BANDIT_ENABLED) attempt diverged from legacy → wire byte \
     * mismatch. Path γ #3 macros at MlCfgFlagRegistry.hpp use the same (!= 0) semantic. */ \
    X(bandit_algorithm,                   COHORT_GATE_BANDIT_THOMPSON) \
    X(thompson_mu_prior,                  COHORT_GATE_BANDIT_THOMPSON) \
    X(thompson_precision_prior,           COHORT_GATE_BANDIT_THOMPSON) \
    X(thompson_precision_obs,             COHORT_GATE_BANDIT_THOMPSON) \
    /* BLENDED state-4 — only emit when bandit_algorithm == 4 */ \
    X(thompson_exp3_blend_alpha,          COHORT_GATE_BANDIT_BLEND_STATE_4) \
    /* Composite confidence cohort */ \
    X(confidence_freshness_tau_secs,      COHORT_GATE_COMPOSITE_CONFIDENCE) \
    X(confidence_capacity_target_dollars, COHORT_GATE_COMPOSITE_CONFIDENCE) \
    X(confidence_capacity_kappa,          COHORT_GATE_COMPOSITE_CONFIDENCE) \
    X(confidence_rmse_baseline,           COHORT_GATE_COMPOSITE_CONFIDENCE) \
    /* Ridge cohort */ \
    X(ridge_lambda,                       COHORT_GATE_RIDGE_ANY) \
    X(ridge_cost_penalty,                 COHORT_GATE_RIDGE_ANY) \
    X(ridge_min_ic_floor,                 COHORT_GATE_RIDGE_ANY) \
    /* Soft-risk degradation cohort */ \
    X(risk_degradation_curve,             COHORT_GATE_SOFTRISK_ENABLED) \
    X(risk_full_size_threshold,           COHORT_GATE_SOFTRISK_ENABLED) \
    X(risk_min_size_threshold,            COHORT_GATE_SOFTRISK_ENABLED) \
    X(risk_min_size_pct,                  COHORT_GATE_SOFTRISK_ENABLED)

#define FOREACH_CFG_GATE_GLOBAL(X) \
    /* No entries at .B.2: trading_mode + ml_buy_threshold both use default always-emit gate \
     * (matching legacy emit_when = 1 for these rows). gap_acceptable_threshold migration \
     * deferred to .B.3 per coding-time discovery (FOREACH_GLOBAL_CFG_FIELD doesn't auto-gen \
     * struct fields; manual cfg storage cleanup is .B.3 scope). */
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Sparse: only rows with NON-DEFAULT gate get an entry. Rows that use the default gate
// (most rows at .B.2 cohort migration) have NO entry here.
//
// 2-tuple shape: X(name, gate_when_expr)
//
// v5.15.5.F.4d.1.B.2 Step 5 — populated with cohort-gated entries. Inline gate expressions
// match legacy FOREACH_STAMP_BOUND_CFG emit_when col semantic (Decision 9 v1.2 reframe:
// flat cfg access for SOFTRISK + BLENDED — engine-wide global cfg snapshot at stamp emit).
// .B.3 may extract shared COHORT_GATE_* macros to dedupe across registries (Path γ #3
// structural close); .B.2 keeps inline for minimum viable scope (replacing 3 registries'
// inline predicates is .B.3 work alongside legacy registry deletion).
//
// CI verification (Check 9 at .B.3): every entry's `name` references a real flagged source row.
// Adding an entry without flagging the source row would fail CI.
//======================================================================
// [END_REGISTRY]_[FOREACH_CFG_GATE_PER_NODE]
//======================================================================

//======================================================================
// [FUNCTION]_[lookup_populate]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the sidecar lookup pair (lookup_drift rides; namespace cfg_gate) — FIELD_IDX switch dispatch; no entry -> default (populate: true / drift: stamp_has_inference_cfg)]
// [REFERENCE]_[INVARIANT]_[H20]
//======================================================================
// [CODE]
//======================================================================
namespace cfg_gate {

    //==================================================================================================
    // [POPULATE lookup — default true; cohort entries override]
    //==================================================================================================
    template <unsigned F>
    inline bool lookup_populate(size_t idx, bool is_per_node, const ControllerConfig<F>& cfg) {
        (void)cfg;  // unused at .B.1 (no per-core or global entries yet); .B.2 entries reference cfg
        if (is_per_node) {
            switch (idx) {
                #define X_CFG_GATE_PER_NODE_POPULATE_CASE(name, expr) \
                    case FIELD_IDX_PER_NODE_##name: return (expr);
                FOREACH_CFG_GATE_PER_NODE(X_CFG_GATE_PER_NODE_POPULATE_CASE)
                #undef X_CFG_GATE_PER_NODE_POPULATE_CASE
                default: return true;  // DEFAULT: always populate per always-emit canonical Q3.G
            }
        } else {
            switch (idx) {
                #define X_CFG_GATE_GLOBAL_POPULATE_CASE(name, expr) \
                    case FIELD_IDX_GLOBAL_##name: return (expr);
                FOREACH_CFG_GATE_GLOBAL(X_CFG_GATE_GLOBAL_POPULATE_CASE)
                #undef X_CFG_GATE_GLOBAL_POPULATE_CASE
                default: return true;
            }
        }
    }

    //==================================================================================================
    // [DRIFT lookup — default stamp_has_inference_cfg; cohort entries override]
    //==================================================================================================
    //
    // Note: stamp_has_inference_cfg passed as bool param (not via ModelStampResult*) to avoid
    // cross-include of ML_Headers/ from MemHeaders/. Consumer macros at the call site compute
    // STAMP_HAS(*handle, inference_cfg) and pass the result.
    //
    template <unsigned F>
    inline bool lookup_drift(size_t idx, bool is_per_node,
                              const ControllerConfig<F>& cfg,
                              bool stamp_has_inference_cfg) {
        (void)cfg;  // unused at .B.1; .B.2 entries may reference cfg state
        if (is_per_node) {
            switch (idx) {
                #define X_CFG_GATE_PER_NODE_DRIFT_CASE(name, expr) \
                    case FIELD_IDX_PER_NODE_##name: return stamp_has_inference_cfg && (expr);
                FOREACH_CFG_GATE_PER_NODE(X_CFG_GATE_PER_NODE_DRIFT_CASE)
                #undef X_CFG_GATE_PER_NODE_DRIFT_CASE
                default: return stamp_has_inference_cfg;
            }
        } else {
            switch (idx) {
                #define X_CFG_GATE_GLOBAL_DRIFT_CASE(name, expr) \
                    case FIELD_IDX_GLOBAL_##name: return stamp_has_inference_cfg && (expr);
                FOREACH_CFG_GATE_GLOBAL(X_CFG_GATE_GLOBAL_DRIFT_CASE)
                #undef X_CFG_GATE_GLOBAL_DRIFT_CASE
                default: return stamp_has_inference_cfg;
            }
        }
    }

}  // namespace cfg_gate
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Each lookup helper switch-dispatches on FIELD_IDX. Empty registries at .B.1 → switch only has
// `default` branch → always returns default. At .B.2 rows populate → switch gets case entries
// → entries override default for flagged rows.
//
// Per H20: switch on size_t with sparse case set is acceptable at slow-path / stamp-emit cadence
// (compiler emits jump table for dense cases; binary search or if-else chain for sparse).
// Mask-bit walker (CFG_FIELD_FOR_EACH_SET_BIT) was rejected for these consumers per the
// compile-time-name-access requirement (consumer macros need cfg.<name> + inf.<name> access;
// runtime idx alone doesn't suffice). See `.B.1` plan body Step 1 design note.
//======================================================================
// [END_FUNCTION]_[lookup_populate]
//======================================================================

//----------------------------------------------------------------------
// [MACRO]_[FOREACH_STAMP_BOUND_DERIVED_COHORT]
// [TAG]_[[ENGINE] [CFG_FLOW] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[action-parameterized meta-walker — expands BASE_X##_<SCOPE> over ALL 4 cfg registries unconditionally; registry-coverage drift impossible by construction (Class 21 close)]
//----------------------------------------------------------------------
//
// Single source of truth for cohort coverage. Dispatches to all 4 cfg-domain registries that participate
// in the STAMP_BOUND_CFG_DERIVED cohort. Consumer passes BASE_X token; meta-walker expands to 4
// `FOREACH_<REGISTRY>(BASE_X##_<SCOPE>)` invocations with token-pasted X-macro names per scope.
//
// Closes Class 21 instance at cfg-derived consumer template fn surface (sister-consumer asymmetric
// registry-coverage drift). Caught at Session E 2026-05-18 after 3 consumer template fns at HEAD
// drifted across .B.1/.B.2/.B.3 incremental extension:
//   - populate_inference_cfg_from_derived walked 2 of 4 (per_node + global only)
//   - drift_check_from_derived walked 3 of 4 (missing gate_cfg_flag per Step 0.5d.d DEFERRED)
//   - populate_stamp_cfg_from_derived walked all 4 (complete)
//   - parse_stamp_cfg_to_derived walked all 4 (complete; Step 1.6.3 Approach A option (e))
//
// Drift impossibility by construction:
//   - Consumer CANNOT silently skip a registry — meta-walker expands all 4 FOREACH invocations
//     unconditionally
//   - Per-consumer X-macros MUST exist by `X_<base>_<SCOPE>` naming convention for token-paste
//     `BASE_X##_<SCOPE>` to resolve; missing X-macro = compile error at FOREACH expansion site
//     (preprocessor fails on undefined identifier in macro body)
//   - X-macros themselves CAN be no-op for legitimate skip-cases — but skip is EXPLICITLY VISIBLE
//     at function scope
//
// Pattern shape per DESIGN_SPECS/cfg-derived-consumer-framework.md v1.2 § "Action-parameterized
// meta-walker for cohort consumer template fns".
// Sister memory: feedback_prefer_action_parameterized_walker_over_per_consumer_walker_bodies.md
// Class 18/21 closure precedent: v5.14.2.E.1 EnsembleModelZoo_PostLoadSetup + NodeModelZoo_PostLoadSetup
//   + FOREACH_ENSEMBLE_POST_LOAD / FOREACH_SINGLE_ZOO_POST_LOAD (single source of truth + many
//   consumer views with compile-time enforced inclusion at all sites).
//
// Used by 5 sites:
//   - populate_inference_cfg_from_derived (cfg_derived namespace; filtered by STAMP_BOUND_CFG_DERIVED bit)
//   - populate_stamp_cfg_from_derived    (cfg_derived namespace; filtered by STAMP_BOUND_CFG_DERIVED bit)
//   - drift_check_from_derived           (cfg_derived namespace; filtered by STAMP_BOUND_CFG_DERIVED bit)
//   - parse_stamp_cfg_to_derived         (cfg_derived namespace; filtered by STAMP_BOUND_CFG_DERIVED bit)
//   - STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN (file-scope; UNCONDITIONAL struct-field decl; no filter)
//
// Enrolled in FOREACH_REGISTRY at CoreFrameworks/MetaRegistry.hpp per H15 + H19 (Level 1 meta-walker
// over cfg-derived cohort; sister to FOREACH_PER_NODE_DOMAIN_BITMAP shape but for consumer cohort
// rather than storage cohort).

//======================================================================
// [REGISTRY]_[FOREACH_STAMP_BOUND_DERIVED_COHORT]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the cfg-derived cohort META-walker — one BASE_X token expands to all 4 cfg registries (PER_NODE + GLOBAL + ML_CFG_FLAG + GATE_CFG_FLAG) so a consumer cannot silently skip one; Class-21 drift-impossible by construction]
// [REFERENCE]_[INVARIANT]_[[H15] [H19]]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_STAMP_BOUND_DERIVED_COHORT(BASE_X)                                                  \
    FOREACH_PER_NODE_CFG_FIELD(BASE_X##_PER_NODE)                                                   \
    FOREACH_GLOBAL_CFG_FIELD(BASE_X##_GLOBAL)                                                       \
    FOREACH_ML_CFG_FLAG(BASE_X##_ML_CFG_FLAG)                                                       \
    FOREACH_GATE_CFG_FLAG(BASE_X##_GATE_CFG_FLAG)
//======================================================================
// [END_CODE]
//======================================================================
// [END_REGISTRY]_[FOREACH_STAMP_BOUND_DERIVED_COHORT]
//======================================================================

//------------------------------------------------------------------
// [SECTION]_[Consumer macros — Step 2 of .B.1 framework consolidation]
//------------------------------------------------------------------
//
// 3 derived-filter consumer macros (each Stage 3 first reference at .B.1 ship):
//   INFERENCE_CFG_POPULATE_FROM_DERIVED — populates StampInferenceCfgInputs from runtime cfg
//   STAMP_CFG_POPULATE_FROM_DERIVED    — emits canonical body bytes for HMAC chain
//   DRIFT_CHECK_FROM_DERIVED           — checks drift between stamp + runtime cfg
//
// Each walks FOREACH_PER_NODE_CFG_FIELD + FOREACH_GLOBAL_CFG_FIELD via X-macro filtered
// iteration with `if constexpr ((meta) & STAMP_BOUND_CFG_DERIVED)` filter. Compile-time name
// access (cfg.<name>, inf.<name>, handle.<name>) — distinct from CFG_FIELD_FOR_EACH_SET_BIT
// mask-bit walker which gives runtime idx only (suitable for synthetic-emit test cases but
// NOT for production value access requiring compile-time name resolution).
//
// At .B.1: ZERO source rows flag STAMP_BOUND_CFG_DERIVED metadata bit → if-constexpr filter
// excludes all rows → walker body never instantiated → vacuous PASS for tests + wire_format
// invariants helper. .B.2 cohort migration populates source rows; walker becomes non-empty.
//
// Caller context: macros expect cfg.<name> + inf.<name> + handle.<name> + FAILURE_MASK_cfg_inference_drift
// + STAMP_HAS macro to be in scope. Typical callers are in ML_Headers (where stamp helpers are
// defined) and CoreFrameworks (where cfg fields are defined). Per-core field access pattern
// (cfg.name direct vs cfg.per_node[N].name indexed) follows H17 cfg struct auto-gen convention
// — verified at .B.2 first cohort migration when actual rows flag the bit.

//------------------------------------------------------------------
// [SECTION]_[Consumer template fns — Step 2 of .B.1 framework consolidation]
//------------------------------------------------------------------
//
// Per C++ if-constexpr discard rules: false branches must be SYNTACTICALLY VALID at non-template
// scope (name resolution happens for discarded branches). To make .B.1's 0-row walker compile
// cleanly when the inf/handle struct DOESN'T have fields for every cfg row, we wrap the consumer
// logic in TEMPLATE FNs where inf/handle become template-dependent types → if-constexpr false
// branch is properly discarded from name resolution.
//
// This was a structural design correction discovered at Step 6 build verify (consumer macros'
// false branches tried to name-resolve `inf.inference_cfg_<name>` for names like
// `take_profit_pct` that don't exist in StampInferenceCfgInputs at HEAD). Template fns fix this.
//
// Pattern note for `.B.2` cohort migration: when STAMP_BOUND_CFG_DERIVED bit flagged on 24+
// source rows, the actual struct fields (`inf.inference_cfg_<name>` + `inf.has_inference_cfg_<name>`)
// need to exist on StampInferenceCfgInputs. Either the struct gets auto-extended via X-macro
// reduction over flagged rows, or `.B.2` cohort migration manually adds the fields. This is a
// `.B.2` design decision documented in postmortem.

namespace cfg_derived {

//======================================================================
// [FUNCTION]_[populate_inference_cfg_from_derived]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[populate StampInferenceCfgInputs from runtime cfg via the 4-registry meta-walker — gate-looked-up scalars + bitmap-bit cohort walkers; sister to legacy INFERENCE_CFG_AUTOPOPULATE]
// [REFERENCE]_[CLASS]_[21]
//======================================================================
// [CODE]
//======================================================================
    // v5.15.5.F.4d.1.B.3 Step 1.6.5b — refactored to use FOREACH_STAMP_BOUND_DERIVED_COHORT
    // meta-walker (single source of truth for cohort coverage). NEW walkers for ml_cfg_flag +
    // gate_cfg_flag cohort registries added (closes Class 21 instance + absorbs Step 0.5d.d
    // for populate_inf side; was MISSING at HEAD pre-refactor).
    template <unsigned F, typename InfT>
    inline void populate_inference_cfg_from_derived(InfT& inf, const ControllerConfig<F>& cfg) {
        (void)inf; (void)cfg;

        #define X_INFERENCE_CFG_POPULATE_PER_NODE(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                constexpr size_t _idx = FIELD_IDX_PER_NODE_##name; \
                const bool _gate = cfg_gate::lookup_populate(_idx, /*is_per_node*/true, cfg); \
                tt::cfg_populate_inf_field(cfg.name, inf.name, inf.has_##name, _gate); \
            }

        #define X_INFERENCE_CFG_POPULATE_GLOBAL(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                constexpr size_t _idx = FIELD_IDX_GLOBAL_##name; \
                const bool _gate = cfg_gate::lookup_populate(_idx, /*is_per_node*/false, cfg); \
                tt::cfg_populate_inf_field(cfg.name, inf.name, inf.has_##name, _gate); \
            }

        // NEW v5.15.5.F.4d.1.B.3 Step 1.6.5b — ml_cfg_flag cohort walker (was MISSING at HEAD).
        // Bitmap-bit semantic: cfg.ml_cfg_flags bit set → inf.<legacy_field> = 1; cleared → 0.
        // No gate lookup (the bit IS the value; absence = 0). Sets has_<legacy_field>
        // unconditionally so populate-time presence is recorded for stamp emission.
        #define X_INFERENCE_CFG_POPULATE_ML_CFG_FLAG(NAME, legacy_field, display_label, section, metadata_flags, doc) \
            if constexpr (((metadata_flags) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                const int _bit_val = BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_##NAME) ? 1 : 0; \
                inf.has_##legacy_field = 1; \
                inf.legacy_field = _bit_val; \
            }

        // NEW v5.15.5.F.4d.1.B.3 Step 1.6.5b — gate_cfg_flag cohort walker (was MISSING at HEAD;
        // sister to ml_cfg_flag walker above; absorbs Step 0.5d.d populate_inf side).
        #define X_INFERENCE_CFG_POPULATE_GATE_CFG_FLAG(NAME, legacy_field, display_label, section, metadata_flags, doc) \
            if constexpr (((metadata_flags) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                const int _bit_val = BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_##NAME) ? 1 : 0; \
                inf.has_##legacy_field = 1; \
                inf.legacy_field = _bit_val; \
            }

        FOREACH_STAMP_BOUND_DERIVED_COHORT(X_INFERENCE_CFG_POPULATE)

        #undef X_INFERENCE_CFG_POPULATE_PER_NODE
        #undef X_INFERENCE_CFG_POPULATE_GLOBAL
        #undef X_INFERENCE_CFG_POPULATE_ML_CFG_FLAG
        #undef X_INFERENCE_CFG_POPULATE_GATE_CFG_FLAG
    }
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[populate_inference_cfg_from_derived]
//======================================================================

//======================================================================
// [FUNCTION]_[populate_stamp_cfg_from_derived]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[emit canonical key=value body bytes for the HMAC chain via the meta-walker — gate-filtered; H9 wire preservation (X-macro bodies verbatim from pre-refactor)]
// [REFERENCE]_[INVARIANT]_[H9]
// [REFERENCE]_[CLASS]_[32]
//======================================================================
// [CODE]
//======================================================================
    // v5.15.5.F.4d.1.B.3 Step 1.6.5b — refactored to use FOREACH_STAMP_BOUND_DERIVED_COHORT
    // meta-walker. X-macro BODIES VERBATIM from pre-refactor (wire-format byte preservation —
    // emit order preserved by FOREACH walker semantics + Layer 5b invariants tolerate the
    // structural reshape). Only structural change: 4 individual FOREACH invocations folded into
    // 1 meta-walker invocation; per-scope X-macros remain in function scope with same bodies.
    template <unsigned F>
    inline size_t populate_stamp_cfg_from_derived(char* buf, size_t cap, const ControllerConfig<F>& cfg) {
        size_t written = 0u;
        (void)buf; (void)cap; (void)cfg; (void)written;

        #define X_STAMP_CFG_POPULATE_PER_NODE(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                constexpr size_t _idx = FIELD_IDX_PER_NODE_##name; \
                const bool _gate = cfg_gate::lookup_populate(_idx, /*is_per_node*/true, cfg); \
                if (_gate) { \
                    written += tt::cfg_emit_field( \
                        cfg.name, \
                        g_per_node_cfg_field_descriptors[_idx], \
                        buf + written, \
                        (cap > written) ? (cap - written) : 0u); \
                } \
            }

        #define X_STAMP_CFG_POPULATE_GLOBAL(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                constexpr size_t _idx = FIELD_IDX_GLOBAL_##name; \
                const bool _gate = cfg_gate::lookup_populate(_idx, /*is_per_node*/false, cfg); \
                if (_gate) { \
                    written += tt::cfg_emit_field( \
                        cfg.name, \
                        g_global_cfg_field_descriptors[_idx], \
                        buf + written, \
                        (cap > written) ? (cap - written) : 0u); \
                } \
            }

        // v5.15.5.F.4d.1.B.2 Step 0.5b — emit BITMAP_BIT rows from FOREACH_ML_CFG_FLAG.
        // Inline snprintf (no CfgFieldDescriptor since ML_CFG_FLAG rows are in a separate
        // registry). %d format matches legacy BITMAP_BIT wire emit at StampBoundCfgRegistry.hpp:106-146;
        // ternary normalization to 0/1 int.
        #define X_STAMP_CFG_POPULATE_ML_CFG_FLAG(NAME, legacy_field, display_label, section, metadata_flags, doc) \
            if constexpr (((metadata_flags) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                const int _bit_val = BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_##NAME) ? 1 : 0; \
                const size_t _remain = (cap > written) ? (cap - written) : 0u; \
                int _n_written = snprintf(buf + written, _remain, "%s=%d\n", #legacy_field, _bit_val); \
                if (_n_written > 0) written += static_cast<size_t>(_n_written); \
            }

        // v5.15.5.F.4d.1.B.3 Step 0.5d.a — sister walker for FOREACH_GATE_CFG_FLAG bitmap-bool
        // rows flagged STAMP_BOUND_CFG_DERIVED. Sister-extension to X_STAMP_CFG_POPULATE_ML_CFG_FLAG
        // (sig migration at Step 0.5d.a.0 added metadata_flags column to FOREACH_GATE_CFG_FLAG).
        // Currently 1 flagged row (BARRIER_GATE_ENABLED — closes Class 32 instance + activates
        // framework walker per Meta-gap M1b cohort migration discipline). Adapts MASK_GATE_CFG_*
        // prefix + reads cfg.gate_cfg_flags bitmap.
        #define X_STAMP_CFG_POPULATE_GATE_CFG_FLAG(NAME, legacy_field, display_label, section, metadata_flags, doc) \
            if constexpr (((metadata_flags) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                const int _bit_val = BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_##NAME) ? 1 : 0; \
                const size_t _remain = (cap > written) ? (cap - written) : 0u; \
                int _n_written = snprintf(buf + written, _remain, "%s=%d\n", #legacy_field, _bit_val); \
                if (_n_written > 0) written += static_cast<size_t>(_n_written); \
            }

        FOREACH_STAMP_BOUND_DERIVED_COHORT(X_STAMP_CFG_POPULATE)

        #undef X_STAMP_CFG_POPULATE_PER_NODE
        #undef X_STAMP_CFG_POPULATE_GLOBAL
        #undef X_STAMP_CFG_POPULATE_ML_CFG_FLAG
        #undef X_STAMP_CFG_POPULATE_GATE_CFG_FLAG

        return written;
    }
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[populate_stamp_cfg_from_derived]
//======================================================================

//======================================================================
// [FUNCTION]_[drift_check_from_derived]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[stamp-vs-runtime drift check via the meta-walker — branchless mask-select trigger (H20) + first-failure-wins reason attribution; caller passes pre-extracted stamp_has + failure_mask]
// [REFERENCE]_[INVARIANT]_[H20]
// [REFERENCE]_[DESIGN_SPEC]_[failure-attribution-buffer-pattern.md]
//======================================================================
// [CODE]
//======================================================================
    // Caller passes pre-extracted bools (stamp_has_inference_cfg + failure_mask) to avoid
    // cross-include of ML_Headers from MemHeaders/CfgGateRegistry.hpp.
    //
    // v5.15.5.F.4d.1.B.3 Step 1.6.5b — refactored to use FOREACH_STAMP_BOUND_DERIVED_COHORT
    // meta-walker. NEW X_DRIFT_CHECK_GATE_CFG_FLAG walker added (closes Step 0.5d.d drift_check
    // side; requires handle.<legacy_field> discrete fields which Step 1.6.3 Decision C Approach A
    // unconditional struct-gen provides via STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN). PER_NODE +
    // GLOBAL + ML_CFG_FLAG X-macros VERBATIM from pre-refactor (drift-comparison semantics
    // preserved).
    template <unsigned F, typename HandleT>
    inline void drift_check_from_derived(uint64_t& failure_flags,
                                          bool stamp_has_inference_cfg,
                                          uint64_t failure_mask,
                                          const HandleT& handle,
                                          const ControllerConfig<F>& cfg,
                                          int& drift_count,
                                          char* reason_buf,
                                          size_t reason_cap) {
        (void)failure_flags; (void)stamp_has_inference_cfg; (void)failure_mask;
        (void)handle; (void)cfg; (void)drift_count;
        (void)reason_buf; (void)reason_cap;          // first-failure-wins; nullable opt-in

        // v5.15.5.F.4d.1.B.3 Step 0.5a — first-failure-wins attribution per
        // failure-attribution-buffer-pattern.md. Each X-macro variant writes
        // attribution into caller-allocated reason_buf ONLY IF buf empty + trigger fires.
        // Caller passes nullptr to opt out (no snprintf overhead).

        #define X_DRIFT_CHECK_PER_NODE(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                constexpr size_t _idx = FIELD_IDX_PER_NODE_##name; \
                const bool _gate = cfg_gate::lookup_drift(_idx, /*is_per_node*/true, cfg, stamp_has_inference_cfg); \
                const bool _drifted = tt::cfg_drift_compare(handle.name, cfg.name); \
                const bool _trigger = _gate & _drifted; \
                failure_flags |= ((uint64_t)_trigger * failure_mask); \
                drift_count += (int)_trigger; \
                if (_trigger && reason_buf && reason_buf[0] == '\0') { \
                    tt::cfg_drift_format_reason(reason_buf, reason_cap, #name, handle.name, cfg.name); \
                } \
            }

        #define X_DRIFT_CHECK_GLOBAL(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                constexpr size_t _idx = FIELD_IDX_GLOBAL_##name; \
                const bool _gate = cfg_gate::lookup_drift(_idx, /*is_per_node*/false, cfg, stamp_has_inference_cfg); \
                const bool _drifted = tt::cfg_drift_compare(handle.name, cfg.name); \
                const bool _trigger = _gate & _drifted; \
                failure_flags |= ((uint64_t)_trigger * failure_mask); \
                drift_count += (int)_trigger; \
                if (_trigger && reason_buf && reason_buf[0] == '\0') { \
                    tt::cfg_drift_format_reason(reason_buf, reason_cap, #name, handle.name, cfg.name); \
                } \
            }

        // v5.15.5.F.4d.1.B.2 Step 0.5b — drift check BITMAP_BIT rows from FOREACH_ML_CFG_FLAG.
        // handle.<legacy_field> stores recorded stamp value (int 0 or 1 per legacy emit);
        // current cfg bit value via BITMAP_IS_SET; mismatch = drift. Branchless mask-select per H20.
        // v5.15.5.F.4d.1.B.3 Step 0.5a — first-failure-wins attribution snprintf added.
        #define X_DRIFT_CHECK_ML_CFG_FLAG(NAME, legacy_field, display_label, section, metadata_flags, doc) \
            if constexpr (((metadata_flags) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                const int _bit_val = BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_##NAME) ? 1 : 0; \
                const bool _drifted = (handle.legacy_field != _bit_val); \
                const bool _trigger = stamp_has_inference_cfg & _drifted; \
                failure_flags |= ((uint64_t)_trigger * failure_mask); \
                drift_count += (int)_trigger; \
                if (_trigger && reason_buf && reason_buf[0] == '\0') { \
                    tt::cfg_drift_format_reason(reason_buf, reason_cap, #legacy_field, handle.legacy_field, _bit_val); \
                } \
            }

        // NEW v5.15.5.F.4d.1.B.3 Step 1.6.5b — sister drift walker for FOREACH_GATE_CFG_FLAG
        // bitmap-bool rows flagged STAMP_BOUND_CFG_DERIVED. Closes Step 0.5d.d DEFERRED
        // (drift_check side); requires handle.<legacy_field> discrete fields which Step 1.6.3
        // Decision C Approach A unconditional struct-gen provides via STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN
        // (_STAMP_RESULT_GATE_CFG_FLAG X-macro auto-gens `uint8_t has_<legacy_field>; int legacy_field;`).
        // Semantic mirrors X_DRIFT_CHECK_ML_CFG_FLAG: reads cfg.gate_cfg_flags bitmap; compares
        // against handle.<legacy_field> recorded stamp value; branchless trigger + first-failure
        // attribution.
        #define X_DRIFT_CHECK_GATE_CFG_FLAG(NAME, legacy_field, display_label, section, metadata_flags, doc) \
            if constexpr (((metadata_flags) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                const int _bit_val = BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_##NAME) ? 1 : 0; \
                const bool _drifted = (handle.legacy_field != _bit_val); \
                const bool _trigger = stamp_has_inference_cfg & _drifted; \
                failure_flags |= ((uint64_t)_trigger * failure_mask); \
                drift_count += (int)_trigger; \
                if (_trigger && reason_buf && reason_buf[0] == '\0') { \
                    tt::cfg_drift_format_reason(reason_buf, reason_cap, #legacy_field, handle.legacy_field, _bit_val); \
                } \
            }

        FOREACH_STAMP_BOUND_DERIVED_COHORT(X_DRIFT_CHECK)

        #undef X_DRIFT_CHECK_PER_NODE
        #undef X_DRIFT_CHECK_GLOBAL
        #undef X_DRIFT_CHECK_ML_CFG_FLAG
        #undef X_DRIFT_CHECK_GATE_CFG_FLAG
    }
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[drift_check_from_derived]
//======================================================================

//======================================================================
// [FUNCTION]_[parse_stamp_cfg_to_derived]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[stamp parse-side: match one key=value against flagged rows across the 4 registries -> parse into struct field + set has_<name>; false = caller falls through to legacy handlers]
// [REFERENCE]_[INVARIANT]_[H8]
//======================================================================
// [CODE]
//======================================================================
    // v5.15.5.F.4d.1.B.3 Step 1.6.3 (Decision C Approach A option (e) framework consolidation;
    // codified at v1.12 plan body) — sister to populate_stamp_cfg_from_derived + drift_check_from_derived
    // + populate_inference_cfg_from_derived (4-of-4 cfg-derived consumer family).
    //
    // Matches a single key=value pair against currently-flagged STAMP_BOUND_CFG_DERIVED rows across
    // 4 cfg registries (per-core + global + ml_cfg_flag + gate_cfg_flag); on match, parses value into
    // the corresponding struct field via tt::cfg_parse_field<T> + sets Surface G has_<name> flag +
    // returns true. Returns false if no match (caller falls through to legacy / model-const handlers).
    //
    // Caller (ModelInference.hpp parser): `else if (cfg_derived::parse_stamp_cfg_to_derived<F>(r, key, val)) { /* matched */ }`
    //
    // Branchless: if-constexpr meta-bit filter compiles to ONLY flagged rows' strcmp branches; runtime
    // cost = up to 27 strcmp comparisons (boot/load-time; not hot path; acceptable per H8).
    // v5.15.5.F.4d.1.B.3 Step 1.6.5b — refactored to use FOREACH_STAMP_BOUND_DERIVED_COHORT
    // meta-walker. parse fn already walked all 4 registries pre-refactor (Step 1.6.3 Approach A
    // option (e) landed at WIP-checkpoint 3); refactor preserves behavior + makes drift impossible
    // for future cohort registry additions (consumer template fn auto-extends with cohort).
    template <unsigned F, typename ResultT>
    inline bool parse_stamp_cfg_to_derived(ResultT& r, const char* key, const char* val) {
        #define X_PARSE_PER_NODE(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                if (strcmp(key, #name) == 0) { \
                    constexpr size_t _idx = FIELD_IDX_PER_NODE_##name; \
                    tt::cfg_parse_field(r.name, g_per_node_cfg_field_descriptors[_idx], val, /*wire_context=*/true); \
                    r.has_##name = 1; \
                    return true; \
                } \
            }

        #define X_PARSE_GLOBAL(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                if (strcmp(key, #name) == 0) { \
                    constexpr size_t _idx = FIELD_IDX_GLOBAL_##name; \
                    tt::cfg_parse_field(r.name, g_global_cfg_field_descriptors[_idx], val, /*wire_context=*/true); \
                    r.has_##name = 1; \
                    return true; \
                } \
            }

        // ML cfg flag bitmap-bool entries: wire key = #legacy_field; storage = int (per legacy emit pattern).
        // Parser matches lowercase legacy_field key; sets r.legacy_field = atoi(val); sets r.has_<legacy_field>.
        #define X_PARSE_ML_CFG_FLAG(NAME, legacy_field, display_label, section, metadata_flags, doc) \
            if constexpr (((metadata_flags) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                if (strcmp(key, #legacy_field) == 0) { \
                    r.legacy_field = atoi(val); \
                    r.has_##legacy_field = 1; \
                    return true; \
                } \
            }

        // Gate cfg flag bitmap-bool entries: same shape as ML.
        #define X_PARSE_GATE_CFG_FLAG(NAME, legacy_field, display_label, section, metadata_flags, doc) \
            if constexpr (((metadata_flags) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                if (strcmp(key, #legacy_field) == 0) { \
                    r.legacy_field = atoi(val); \
                    r.has_##legacy_field = 1; \
                    return true; \
                } \
            }

        FOREACH_STAMP_BOUND_DERIVED_COHORT(X_PARSE)

        #undef X_PARSE_PER_NODE
        #undef X_PARSE_GLOBAL
        #undef X_PARSE_ML_CFG_FLAG
        #undef X_PARSE_GATE_CFG_FLAG

        return false;
    }
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[parse_stamp_cfg_to_derived]
//======================================================================

//======================================================================
// [FUNCTION]_[copy_stamp_result_to_handle]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the FIFTH walker — copy the parsed cfg-derived cohort sr -> handle at load; per-field has_-gated; closes PARITY-043 (33 of 36 cohort fields were parsed into sr and never copied, so drift rows compared a permanent 0)]
// [REFERENCE]_[CLASS]_[18]
//======================================================================
// [CODE]
//======================================================================
    // E.1.2.C leg 2 (2026-08-20) — the walker family had 4 of its 5 legs
    // (parse / inf-populate / emit / drift); the sr->handle COPY was never
    // built, so every cfg-derived field the stamp carried died in the local
    // `sr` and the FOREACH_CFG_DRIFT_CHECK rows compared handle-side zeros
    // against live cfg (two REFUSE_STRICT rows false-firing on EVERY load at
    // a default cfg once bandit_enabled defaulted ON). Per-field `has_`
    // gating preserves Surface-G semantics: an absent key leaves has_=0 and
    // the drift rows' cfg-cohort-on gate still fires — the INTENDED
    // "trained-without-feature" catch (CfgDriftCheckRegistry.hpp § gates).
    // The FOREACH_STAMP_RESULT_FIELD_EXCLUSION names are safe by the same
    // mechanism: their master-cfg has_ twins are never set (the MODEL_CONST
    // parse consumes those keys first), so this walker never copies them.
    template <typename HandleT, typename ResultT>
    inline void copy_stamp_result_to_handle(HandleT& h, const ResultT& r) {
        (void)h; (void)r;

        #define X_HANDLE_COPY_PER_NODE(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                if (r.has_##name) { h.name = r.name; h.has_##name = 1; } \
            }

        #define X_HANDLE_COPY_GLOBAL(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                if (r.has_##name) { h.name = r.name; h.has_##name = 1; } \
            }

        #define X_HANDLE_COPY_ML_CFG_FLAG(NAME, legacy_field, display_label, section, metadata_flags, doc) \
            if constexpr (((metadata_flags) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                if (r.has_##legacy_field) { h.legacy_field = r.legacy_field; h.has_##legacy_field = 1; } \
            }

        #define X_HANDLE_COPY_GATE_CFG_FLAG(NAME, legacy_field, display_label, section, metadata_flags, doc) \
            if constexpr (((metadata_flags) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                if (r.has_##legacy_field) { h.legacy_field = r.legacy_field; h.has_##legacy_field = 1; } \
            }

        FOREACH_STAMP_BOUND_DERIVED_COHORT(X_HANDLE_COPY)

        #undef X_HANDLE_COPY_PER_NODE
        #undef X_HANDLE_COPY_GLOBAL
        #undef X_HANDLE_COPY_ML_CFG_FLAG
        #undef X_HANDLE_COPY_GATE_CFG_FLAG
    }
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[copy_stamp_result_to_handle]
//======================================================================

}  // namespace cfg_derived

//----------------------------------------------------------------------
// [MACRO]_[STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN]
// [TAG]_[[ENGINE] [CFG_FLOW] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[UNCONDITIONAL struct-field auto-gen over the 4 registries (_STAMP_RESULT_* helpers + PARSE wrapper ride) — closes Class 14/18/21 for the cfg-derived surface]
// [REFERENCE]_[CLASS]_[18]
//----------------------------------------------------------------------
// v5.15.5.F.4d.1.B.3 Step 1.6.3 (Decision C Approach A option (e) framework consolidation;
// codified at v1.12 plan body) — sister to STAMP_CFG_AUTOPOPULATE + INFERENCE_CFG_POPULATE_FROM_DERIVED
// macros. Invoked at struct scope to declare ALL stamp-bound derived fields from 4 cfg registries
// (per-core + global + ml_cfg_flag + gate_cfg_flag) UNCONDITIONALLY (no metadata filter).
//
// Caller (ModelInference.hpp ModelStampResult + StampInferenceCfgInputs struct definitions):
//   `STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN()`
//
// Approach A unconditional: emits `uint8_t has_<name>; STORAGE_T <name>;` for ALL rows (not just
// STAMP_BOUND_CFG_DERIVED-flagged subset). Bounded .bss cost (~1-1.5KB per struct); preserves
// Surface G semantic per-entry. Sister consumers (parse + emit + drift) filter by metadata bit
// at use sites — struct holds all fields, walkers process flagged subset.
//
// Bug classes closed: Class 14 (struct field that doesn't exist for flagged rows) +
// Class 18 (mirror between struct-gen + emit walker) + Class 21 (parallel-descriptor across walkers).
//
// Sparse exclusion sidecar — names declared in FOREACH_STAMP_BOUND_MODEL_CONST (architectural-constants
// registry) that collide with master cfg names. Per H18 SIDECAR OVERRIDE pattern + Pillar B13
// (cross-walker struct-field uniqueness; codified at .B.3 v1.12 mid-coding).
//
// Why duplicate names exist: training-time architectural constants (xgb_*) live in MODEL_CONST as
// "training snapshot recorded in stamp"; same field name appears in master FOREACH_GLOBAL_CFG_FIELD
// as "runtime cfg value". Conceptually distinct semantic (training-time recorded vs runtime live);
// SAME field name. ModelStampResult holds the TRAINING-TIME value via MODEL_CONST walker; master
// walker must SKIP these names to avoid duplicate member declaration.
//
// Mechanism: each struct site uses #define/#undef redirect bracket (see ModelInference.hpp struct
// definitions). Excluded names get redirected to dead `_stamp_result_excluded_<name>` field
// during macro expansion (~16 bytes wasted per excluded name × 2 structs = ~96 bytes for current 3).
//
// CI tool `check_struct_field_uniqueness.py` (NEW at .B.3) enforces: any name in BOTH master cfg AND
// FOREACH_STAMP_BOUND_MODEL_CONST MUST appear in this exclusion sidecar (else CI fail at build).
// Future collisions = add 1 sidecar entry + 2 #define/#undef lines per struct site. Bounded scope.
//----------------------------------------------------------------------

//======================================================================
// [REGISTRY]_[FOREACH_STAMP_RESULT_FIELD_EXCLUSION]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the H18 sparse exclusion sidecar — 3 names colliding between master cfg + MODEL_CONST; struct sites redirect them to dead fields (see the prose above)]
// [COLUMN]_[name]_[cfg field name declared in FOREACH_STAMP_BOUND_MODEL_CONST that collides with a master cfg row]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_STAMP_RESULT_FIELD_EXCLUSION(X) \
    X(xgb_min_child_weight) \
    X(xgb_seed)             \
    X(xgb_train_nthread)
//======================================================================
// [END_CODE]
//======================================================================
// [END_REGISTRY]_[FOREACH_STAMP_RESULT_FIELD_EXCLUSION]
//======================================================================

// X-macro field helpers (private framework internals; underscore prefix). Names follow
// `_STAMP_RESULT_<SCOPE>` naming convention required by FOREACH_STAMP_BOUND_DERIVED_COHORT
// meta-walker token-paste (`BASE_X##_<SCOPE>` where BASE_X=`_STAMP_RESULT`).
// v5.15.5.F.4d.1.B.3 Step 1.6.5b — renamed from `_STAMP_RESULT_<SCOPE>_FIELD` (4 sites) to
// match meta-walker naming convention. Bodies VERBATIM.
#define _STAMP_RESULT_PER_NODE(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
    uint8_t has_##name; STORAGE_T name;
#define _STAMP_RESULT_GLOBAL(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
    uint8_t has_##name; STORAGE_T name;
#define _STAMP_RESULT_ML_CFG_FLAG(NAME, legacy_field, display_label, section, metadata_flags, doc) \
    uint8_t has_##legacy_field; int legacy_field;
#define _STAMP_RESULT_GATE_CFG_FLAG(NAME, legacy_field, display_label, section, metadata_flags, doc) \
    uint8_t has_##legacy_field; int legacy_field;

// v5.15.5.F.4d.1.B.3 Step 1.6.5b — refactored to use FOREACH_STAMP_BOUND_DERIVED_COHORT
// meta-walker (single source of truth for cohort coverage). Sister to the 4 cfg-derived
// consumer template fns; struct-gen variant is UNCONDITIONAL (no metadata-bit filter at
// X-macro body — emits has + value field for ALL rows; consumer-side walkers filter at use
// site via if-constexpr).
#define STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN() \
    FOREACH_STAMP_BOUND_DERIVED_COHORT(_STAMP_RESULT)

// PARSE_STAMP_CFG_TO_DERIVED: caller-facing macro wrapper. Returns true on match.
// F hardcoded to 64 (production FPN_Binary<F> precision) — ResultT must have matching FPN_Binary<64> fields
// via STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN() with `static constexpr unsigned F = 64;` in scope.
// When ModelStampResult templated in future ship, change to `decltype((r))::F`.
#define PARSE_STAMP_CFG_TO_DERIVED(r, key, val) \
    cfg_derived::parse_stamp_cfg_to_derived<64>((r), (key), (val))

// COPY_RESULT_TO_HANDLE_FROM_DERIVED: the load-time sr->handle copy for the
// cfg-derived cohort (E.1.2.C leg 2 — the fifth walker; PARITY-043 close).
// Type-generic (both structs carry the AUTO_GEN fields); call after Model_Load
// success so a refused load never carries copied state.
#define COPY_RESULT_TO_HANDLE_FROM_DERIVED(handle, sr) \
    cfg_derived::copy_stamp_result_to_handle((handle), (sr))

//----------------------------------------------------------------------
// [MACRO]_[*_FROM_DERIVED wrappers]
// [TAG]_[[ENGINE] [CFG_FLOW]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[thin caller-facing wrappers over the 4 consumer template fns — preserve the familiar call-site shape; DRIFT wrapper extracts STAMP_HAS + failure_mask at the ML_Headers caller]
//----------------------------------------------------------------------
// Macros preserve familiar call-site shape; template fn provides correct C++ if-constexpr semantics.

#define INFERENCE_CFG_POPULATE_FROM_DERIVED(inf, cfg) \
    cfg_derived::populate_inference_cfg_from_derived((inf), (cfg))

#define STAMP_CFG_POPULATE_FROM_DERIVED(buf, cap, cfg) \
    cfg_derived::populate_stamp_cfg_from_derived((buf), (cap), (cfg))

// DRIFT_CHECK_FROM_DERIVED: caller computes stamp_has + provides failure_mask (avoids cross-include).
// Caller is in ML_Headers context (where STAMP_HAS macro + FAILURE_MASK_cfg_binding_drift are
// in scope per MemHeaders/FailureModeRegistry.hpp). The macro wrapper extracts both + calls
// template fn. Per CfgDriftCheckRegistry.hpp:104, INFERENCE_CFG drift category maps to
// FAILURE_MASK_cfg_binding_drift (not _cfg_inference_drift; that symbol doesn't exist).
//
// v5.15.5.F.4d.1.B.3 Step 0.5a — reason_buf + reason_cap args added per
// failure-attribution-buffer-pattern.md § Framework-extension shape (Stage 3 first canonical).
// Caller passes ModelStampResult.reason + sizeof(.reason) for first-drift attribution;
// pass nullptr to opt out (no snprintf overhead).
#define DRIFT_CHECK_FROM_DERIVED(failure_flags, handle, cfg, drift_count_ref, reason_buf, reason_cap) \
    cfg_derived::drift_check_from_derived( \
        (failure_flags), \
        STAMP_HAS((handle), inference_cfg), \
        FAILURE_MASK_cfg_binding_drift, \
        (handle), \
        (cfg), \
        (drift_count_ref), \
        (reason_buf), \
        (reason_cap))

//------------------------------------------------------------------
// [SECTION]_[Cross-references]
//------------------------------------------------------------------
//
// DESIGN_SPECS:
//   - cfg-derived-consumer-framework.md (this file is Stage 3 first reference at .B.1 ship close)
//   - canonical-sister-extension-discipline.md (the discipline that produced this file's shape)
//   - sidecar-override-pattern-for-registry-auto-flows.md (first canonical of gate-type sidecar)
//   - metadata-bit-driven-derived-filter-framework.md (parent — provides STAMP_BOUND_CFG_DERIVED bit)
//
// Skills:
//   - /anti-spaghetti (first canonical run validated this file's structural shape; 0 new
//     parallel infrastructure introduced)
//   - /readiness Check 29 (verified "Canonical sister registries considered" section)
//   - /plan-draft (scaffolds future plans from template that produced this ship's plan body)
//
// CLAUDE.md invariants (+ the framework-driven-extensibility / structural-fix-
// preferred disciplines — this file IS the framework infrastructure and closes
// Class 14/18/21 for the cfg-derived surface):
//   - H15: every FOREACH_X registry enrolled in FOREACH_REGISTRY meta-registry
//     (LANDED — FOREACH_CFG_GATE_PER_NODE + _GLOBAL + FOREACH_STAMP_BOUND_DERIVED_COHORT
//     all have MetaRegistry.hpp rows)
//   - H18: custom-semantics via sidecar override pattern (this file's structural shape)
//   - H19: meta-registry topology — sidecar Level 1 with PARENT = FOREACH_METADATA_BIT
//
// Cohort migration entries: deferred to .B.2 per per-sub-ship cycle. .B.2 plan body skeleton:
// subplans/2026-05-17-v5.15.5.F.4d.1.B.2-cohort-migration.md
