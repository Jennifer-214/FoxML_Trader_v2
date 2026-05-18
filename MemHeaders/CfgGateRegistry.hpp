// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
//======================================================================================================
// [CFG GATE REGISTRY — canonical sparse sidecar for cfg-derived consumer gate_when expressions]
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
// Each row in master FOREACH_PER_CORE_CFG_FIELD + FOREACH_GLOBAL_CFG_FIELD that flags the
// STAMP_BOUND_CFG_DERIVED metadata bit can OPTIONALLY have a custom gate_when expression
// here. Default gates apply if no entry exists:
//
//   For populate consumers (INFERENCE_CFG_POPULATE_FROM_DERIVED, STAMP_CFG_POPULATE_FROM_DERIVED):
//     DEFAULT gate = true (always populate per always-emit canonical Q3.G)
//
//   For drift consumers (DRIFT_CHECK_FROM_DERIVED):
//     DEFAULT gate = stamp_has_inference_cfg (drift check fires when stamp's inference_cfg present)
//
// Per-cohort gates that DIFFER from default (will populate at .B.2 when cohort fields flag the bit):
//   Bandit/Thompson cohort → BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_BANDIT_ENABLED)
//   Ridge cohort → BITMAP_ANY(cfg.ml_cfg_flags, MASK_ML_CFG_RIDGE_WITHIN_HORIZON | MASK_ML_CFG_RIDGE_ACROSS_HORIZONS)
//   Composite confidence cohort → BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED)
//   Soft-risk degradation cohort → cfg.risk_degradation_curve != 0
//
//======================================================================================================
// AT .B.1: BOTH SIDECAR REGISTRIES ARE EMPTY (no cohort fields flag STAMP_BOUND_CFG_DERIVED yet;
// .B.2 cohort migration populates source rows + corresponding sidecar entries together).
//======================================================================================================

#pragma once

#include "../CoreFrameworks/CfgFieldRegistry.hpp"      // CfgFieldDescriptor + FIELD_IDX_<NAME>
#include "../CoreFrameworks/ControllerConfig.hpp"      // ControllerConfig<F>
#include <cstddef>                                      // size_t

//======================================================================================================
// [FOREACH_CFG_GATE — sparse sidecar registries]
//======================================================================================================
//
// Sparse: only rows with NON-DEFAULT gate get an entry. Rows that use the default gate
// (most rows at .B.2 cohort migration) have NO entry here.
//
// 2-tuple shape: X(name, gate_when_expr)
//   name        — must match a FOREACH_PER_CORE_CFG_FIELD or FOREACH_GLOBAL_CFG_FIELD row name
//                 with STAMP_BOUND_CFG_DERIVED metadata bit set
//   gate_when_expr — C++ expression returning bool; references cfg (typed ControllerConfig<F>);
//                    evaluated at consumer macro expansion time (slow-path / stamp-emit cadence)
//
// CI verification (Check 9 at .B.3): every entry's `name` references a real flagged source row.
// Adding an entry without flagging the source row would fail CI.

// v5.15.5.F.4d.1.B.2 Step 5 — populated with cohort-gated entries. Inline gate expressions
// match legacy FOREACH_STAMP_BOUND_CFG emit_when col semantic (Decision 9 v1.2 reframe:
// flat cfg access for SOFTRISK + BLENDED — engine-wide global cfg snapshot at stamp emit).
// .B.3 may extract shared COHORT_GATE_* macros to dedupe across registries (Path γ #3
// structural close); .B.2 keeps inline for minimum viable scope (replacing 3 registries'
// inline predicates is .B.3 work alongside legacy registry deletion).
#define FOREACH_CFG_GATE_PER_CORE(X) \
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

//======================================================================================================
// [Sidecar lookup helpers — used by consumer macros at Step 2]
//======================================================================================================
//
// Each lookup helper switch-dispatches on FIELD_IDX. Empty registries at .B.1 → switch only has
// `default` branch → always returns default. At .B.2 rows populate → switch gets case entries
// → entries override default for flagged rows.
//
// Per H20: switch on size_t with sparse case set is acceptable at slow-path / stamp-emit cadence
// (compiler emits jump table for dense cases; binary search or if-else chain for sparse).
// Mask-bit walker (CFG_FIELD_FOR_EACH_SET_BIT) was rejected for these consumers per the
// compile-time-name-access requirement (consumer macros need cfg.<name> + inf.<name> access;
// runtime idx alone doesn't suffice). See `.B.1` plan body Step 1 design note.

namespace cfg_gate {

    //==================================================================================================
    // [POPULATE lookup — default true; cohort entries override]
    //==================================================================================================
    template <unsigned F>
    inline bool lookup_populate(size_t idx, bool is_per_core, const ControllerConfig<F>& cfg) {
        (void)cfg;  // unused at .B.1 (no per-core or global entries yet); .B.2 entries reference cfg
        if (is_per_core) {
            switch (idx) {
                #define X_CFG_GATE_PER_CORE_POPULATE_CASE(name, expr) \
                    case FIELD_IDX_PER_CORE_##name: return (expr);
                FOREACH_CFG_GATE_PER_CORE(X_CFG_GATE_PER_CORE_POPULATE_CASE)
                #undef X_CFG_GATE_PER_CORE_POPULATE_CASE
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
    inline bool lookup_drift(size_t idx, bool is_per_core,
                              const ControllerConfig<F>& cfg,
                              bool stamp_has_inference_cfg) {
        (void)cfg;  // unused at .B.1; .B.2 entries may reference cfg state
        if (is_per_core) {
            switch (idx) {
                #define X_CFG_GATE_PER_CORE_DRIFT_CASE(name, expr) \
                    case FIELD_IDX_PER_CORE_##name: return stamp_has_inference_cfg && (expr);
                FOREACH_CFG_GATE_PER_CORE(X_CFG_GATE_PER_CORE_DRIFT_CASE)
                #undef X_CFG_GATE_PER_CORE_DRIFT_CASE
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

//======================================================================================================
// [Consumer macros — Step 2 of .B.1 framework consolidation]
//======================================================================================================
//
// 3 derived-filter consumer macros (each Stage 3 first reference at .B.1 ship):
//   INFERENCE_CFG_POPULATE_FROM_DERIVED — populates StampInferenceCfgInputs from runtime cfg
//   STAMP_CFG_POPULATE_FROM_DERIVED    — emits canonical body bytes for HMAC chain
//   DRIFT_CHECK_FROM_DERIVED           — checks drift between stamp + runtime cfg
//
// Each walks FOREACH_PER_CORE_CFG_FIELD + FOREACH_GLOBAL_CFG_FIELD via X-macro filtered
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
// (cfg.name direct vs cfg.per_core[N].name indexed) follows H17 cfg struct auto-gen convention
// — verified at .B.2 first cohort migration when actual rows flag the bit.

//==================================================================================================
// [Consumer template fns — Step 2 of .B.1 framework consolidation]
//==================================================================================================
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

    //==============================================================================================
    // [populate_inference_cfg_from_derived — sister to legacy INFERENCE_CFG_AUTOPOPULATE]
    //==============================================================================================
    template <unsigned F, typename InfT>
    inline void populate_inference_cfg_from_derived(InfT& inf, const ControllerConfig<F>& cfg) {
        (void)inf; (void)cfg;  // 0-row walk at .B.1 → both unused

        #define X_INFERENCE_CFG_POPULATE_PER_CORE(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                constexpr size_t _idx = FIELD_IDX_PER_CORE_##name; \
                const bool _gate = cfg_gate::lookup_populate(_idx, /*is_per_core*/true, cfg); \
                tt::cfg_populate_inf_field(cfg.name, \
                                            inf.name, \
                                            inf.has_##name, _gate); \
            }
        FOREACH_PER_CORE_CFG_FIELD(X_INFERENCE_CFG_POPULATE_PER_CORE)
        #undef X_INFERENCE_CFG_POPULATE_PER_CORE

        #define X_INFERENCE_CFG_POPULATE_GLOBAL(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                constexpr size_t _idx = FIELD_IDX_GLOBAL_##name; \
                const bool _gate = cfg_gate::lookup_populate(_idx, /*is_per_core*/false, cfg); \
                tt::cfg_populate_inf_field(cfg.name, \
                                            inf.name, \
                                            inf.has_##name, _gate); \
            }
        FOREACH_GLOBAL_CFG_FIELD(X_INFERENCE_CFG_POPULATE_GLOBAL)
        #undef X_INFERENCE_CFG_POPULATE_GLOBAL
    }

    //==============================================================================================
    // [populate_stamp_cfg_from_derived — emit canonical body bytes for HMAC chain]
    //==============================================================================================
    template <unsigned F>
    inline size_t populate_stamp_cfg_from_derived(char* buf, size_t cap, const ControllerConfig<F>& cfg) {
        size_t written = 0u;
        (void)buf; (void)cap; (void)cfg; (void)written;  // 0-row walk at .B.1

        #define X_STAMP_CFG_POPULATE_PER_CORE(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                constexpr size_t _idx = FIELD_IDX_PER_CORE_##name; \
                const bool _gate = cfg_gate::lookup_populate(_idx, /*is_per_core*/true, cfg); \
                if (_gate) { \
                    written += tt::cfg_emit_field( \
                        cfg.name, \
                        g_per_core_cfg_field_descriptors[_idx], \
                        buf + written, \
                        (cap > written) ? (cap - written) : 0u); \
                } \
            }
        FOREACH_PER_CORE_CFG_FIELD(X_STAMP_CFG_POPULATE_PER_CORE)
        #undef X_STAMP_CFG_POPULATE_PER_CORE

        #define X_STAMP_CFG_POPULATE_GLOBAL(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                constexpr size_t _idx = FIELD_IDX_GLOBAL_##name; \
                const bool _gate = cfg_gate::lookup_populate(_idx, /*is_per_core*/false, cfg); \
                if (_gate) { \
                    written += tt::cfg_emit_field( \
                        cfg.name, \
                        g_global_cfg_field_descriptors[_idx], \
                        buf + written, \
                        (cap > written) ? (cap - written) : 0u); \
                } \
            }
        FOREACH_GLOBAL_CFG_FIELD(X_STAMP_CFG_POPULATE_GLOBAL)
        #undef X_STAMP_CFG_POPULATE_GLOBAL

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
        FOREACH_ML_CFG_FLAG(X_STAMP_CFG_POPULATE_ML_CFG_FLAG)
        #undef X_STAMP_CFG_POPULATE_ML_CFG_FLAG

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
        FOREACH_GATE_CFG_FLAG(X_STAMP_CFG_POPULATE_GATE_CFG_FLAG)
        #undef X_STAMP_CFG_POPULATE_GATE_CFG_FLAG

        return written;
    }

    //==============================================================================================
    // [drift_check_from_derived — branchless trigger via mask-select per H20]
    //==============================================================================================
    // Caller passes pre-extracted bools (stamp_has_inference_cfg + failure_mask) to avoid
    // cross-include of ML_Headers from MemHeaders/CfgGateRegistry.hpp.
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
        (void)handle; (void)cfg; (void)drift_count;  // 0-row walk at .B.1
        (void)reason_buf; (void)reason_cap;          // first-failure-wins; nullable opt-in

        // v5.15.5.F.4d.1.B.3 Step 0.5a — first-failure-wins attribution per
        // failure-attribution-buffer-pattern.md. Each X-macro variant writes
        // attribution into caller-allocated reason_buf ONLY IF buf empty + trigger fires.
        // Caller passes nullptr to opt out (no snprintf overhead).

        #define X_DRIFT_CHECK_PER_CORE(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                constexpr size_t _idx = FIELD_IDX_PER_CORE_##name; \
                const bool _gate = cfg_gate::lookup_drift(_idx, /*is_per_core*/true, cfg, stamp_has_inference_cfg); \
                const bool _drifted = tt::cfg_drift_compare(handle.name, cfg.name); \
                const bool _trigger = _gate & _drifted; \
                failure_flags |= ((uint64_t)_trigger * failure_mask); \
                drift_count += (int)_trigger; \
                if (_trigger && reason_buf && reason_buf[0] == '\0') { \
                    tt::cfg_drift_format_reason(reason_buf, reason_cap, #name, handle.name, cfg.name); \
                } \
            }
        FOREACH_PER_CORE_CFG_FIELD(X_DRIFT_CHECK_PER_CORE)
        #undef X_DRIFT_CHECK_PER_CORE

        #define X_DRIFT_CHECK_GLOBAL(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                constexpr size_t _idx = FIELD_IDX_GLOBAL_##name; \
                const bool _gate = cfg_gate::lookup_drift(_idx, /*is_per_core*/false, cfg, stamp_has_inference_cfg); \
                const bool _drifted = tt::cfg_drift_compare(handle.name, cfg.name); \
                const bool _trigger = _gate & _drifted; \
                failure_flags |= ((uint64_t)_trigger * failure_mask); \
                drift_count += (int)_trigger; \
                if (_trigger && reason_buf && reason_buf[0] == '\0') { \
                    tt::cfg_drift_format_reason(reason_buf, reason_cap, #name, handle.name, cfg.name); \
                } \
            }
        FOREACH_GLOBAL_CFG_FIELD(X_DRIFT_CHECK_GLOBAL)
        #undef X_DRIFT_CHECK_GLOBAL

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
        FOREACH_ML_CFG_FLAG(X_DRIFT_CHECK_ML_CFG_FLAG)
        #undef X_DRIFT_CHECK_ML_CFG_FLAG

        // v5.15.5.F.4d.1.B.3 Step 0.5d.d DEFERRED — sister drift walker for FOREACH_GATE_CFG_FLAG
        // requires `handle.barrier_gate_enabled` discrete field on ModelStampResult, which
        // Step 1.6.3 (Decision C Approach A unconditional struct-gen) provides. Walker emit
        // (Step 0.5d.a above at populate_stamp_cfg_from_derived) landed; drift check rejoins
        // after Step 1.6.3 lands ModelStampResult auto-gen extension.
    }

    //==============================================================================================
    // [parse_stamp_cfg_to_derived — 4-walker parser dispatch for stamp parse-side]
    //==============================================================================================
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
    template <unsigned F, typename ResultT>
    inline bool parse_stamp_cfg_to_derived(ResultT& r, const char* key, const char* val) {
        #define X_PARSE_PER_CORE(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                if (strcmp(key, #name) == 0) { \
                    constexpr size_t _idx = FIELD_IDX_PER_CORE_##name; \
                    tt::cfg_parse_field(r.name, g_per_core_cfg_field_descriptors[_idx], val); \
                    r.has_##name = 1; \
                    return true; \
                } \
            }
        FOREACH_PER_CORE_CFG_FIELD(X_PARSE_PER_CORE)
        #undef X_PARSE_PER_CORE

        #define X_PARSE_GLOBAL(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
            if constexpr (((meta) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                if (strcmp(key, #name) == 0) { \
                    constexpr size_t _idx = FIELD_IDX_GLOBAL_##name; \
                    tt::cfg_parse_field(r.name, g_global_cfg_field_descriptors[_idx], val); \
                    r.has_##name = 1; \
                    return true; \
                } \
            }
        FOREACH_GLOBAL_CFG_FIELD(X_PARSE_GLOBAL)
        #undef X_PARSE_GLOBAL

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
        FOREACH_ML_CFG_FLAG(X_PARSE_ML_CFG_FLAG)
        #undef X_PARSE_ML_CFG_FLAG

        // Gate cfg flag bitmap-bool entries: same shape as ML.
        #define X_PARSE_GATE_CFG_FLAG(NAME, legacy_field, display_label, section, metadata_flags, doc) \
            if constexpr (((metadata_flags) & CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED) != 0) { \
                if (strcmp(key, #legacy_field) == 0) { \
                    r.legacy_field = atoi(val); \
                    r.has_##legacy_field = 1; \
                    return true; \
                } \
            }
        FOREACH_GATE_CFG_FLAG(X_PARSE_GATE_CFG_FLAG)
        #undef X_PARSE_GATE_CFG_FLAG

        return false;
    }

}  // namespace cfg_derived

//==============================================================================================
// [STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN — unconditional struct-field auto-gen for 4 registries]
//==============================================================================================
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
#define FOREACH_STAMP_RESULT_FIELD_EXCLUSION(X) \
    X(xgb_min_child_weight) \
    X(xgb_seed)             \
    X(xgb_train_nthread)

// X-macro field helpers (private framework internals; underscore prefix):
#define _STAMP_RESULT_PER_CORE_FIELD(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
    uint8_t has_##name; STORAGE_T name;
#define _STAMP_RESULT_GLOBAL_FIELD(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) \
    uint8_t has_##name; STORAGE_T name;
#define _STAMP_RESULT_ML_CFG_FIELD(NAME, legacy_field, display_label, section, metadata_flags, doc) \
    uint8_t has_##legacy_field; int legacy_field;
#define _STAMP_RESULT_GATE_CFG_FIELD(NAME, legacy_field, display_label, section, metadata_flags, doc) \
    uint8_t has_##legacy_field; int legacy_field;

#define STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN() \
    FOREACH_PER_CORE_CFG_FIELD(_STAMP_RESULT_PER_CORE_FIELD) \
    FOREACH_GLOBAL_CFG_FIELD(_STAMP_RESULT_GLOBAL_FIELD) \
    FOREACH_ML_CFG_FLAG(_STAMP_RESULT_ML_CFG_FIELD) \
    FOREACH_GATE_CFG_FLAG(_STAMP_RESULT_GATE_CFG_FIELD)

// PARSE_STAMP_CFG_TO_DERIVED: caller-facing macro wrapper. Returns true on match.
// F hardcoded to 64 (production FPN<F> precision) — ResultT must have matching FPN<64> fields
// via STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN() with `static constexpr unsigned F = 64;` in scope.
// When ModelStampResult templated in future ship, change to `decltype((r))::F`.
#define PARSE_STAMP_CFG_TO_DERIVED(r, key, val) \
    cfg_derived::parse_stamp_cfg_to_derived<64>((r), (key), (val))

//==================================================================================================
// [Consumer macros — thin wrappers around template fns]
//==================================================================================================
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

//======================================================================================================
// [Cross-references]
//======================================================================================================
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
// CLAUDE.md:
//   - H15: every FOREACH_X registry enrolled in FOREACH_REGISTRY meta-registry (4 enrollments
//     pending at Step 4: FOREACH_CFG_GATE_PER_CORE + _GLOBAL + 3 consumer macros)
//   - H18: custom-semantics via sidecar override pattern (this file's structural shape)
//   - H19: meta-registry topology — sidecar Level 1 with PARENT = FOREACH_METADATA_BIT
//   - item 31: framework-driven extensibility (this file IS the framework infrastructure)
//   - item 19: structural fix preferred (this file closes Class 14/18/21 for cfg-derived surface)
//
// Cohort migration entries: deferred to .B.2 per per-sub-ship cycle. .B.2 plan body skeleton:
// subplans/2026-05-17-v5.15.5.F.4d.1.B.2-cohort-migration.md
