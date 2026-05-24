// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [STAMP-BOUND MODEL-CONST REGISTRY — v5.14.8.0]
//======================================================================================================
// X-macro registry for ARCHITECTURAL stamp body fields — fields that
// describe the model itself (training timestamp, run name, scaler fit
// hash, environment metadata, removal reasons CSV, etc.). Sister
// registry to FOREACH_STAMP_BOUND_CFG (StampBoundCfgRegistry.hpp) which
// covers cfg-bound fields (drift detection between trainer cfg + engine cfg).
//
// CLOSES TECH_DEBT-006: previously, the ~24 architectural stamp body
// fields on ModelStampResult / StampInferenceCfgInputs / ModelHandle
// were declared as MANUAL flat struct fields. Each addition required
// N-site updates: struct field + has_* flag + parser case + emitter
// line + production-caller populator. v5.9.5b production-caller class
// (PARITY-002/003/004/005/008) recurred 4× before STAMP_CFG_AUTOPOPULATE
// extinguished it for cfg-bound fields. This registry extinguishes the
// same class for architectural model-const fields.
//
// SCOPE for v5.14.8.0: registry INFRASTRUCTURE (this file) — empty
// FOREACH_STAMP_BOUND_MODEL_CONST defined; macros + companion AUTOPOPULATE
// + documentation. v5.14.8.A migrates the existing 24 architectural
// fields into entries here (boundary-stable: wire format preserved
// via canonical-order-preserving entry order; field-access syntax
// preserved via X-macro generating same field names).
// v5.14.8.D adds 5 NEW v5.14.8 fields as registry entries
// (training_timestamp_us, run_name, scaler_fit_data_hash,
// removal_reasons_csv, environment_meta).
//
// BIT-PACKING (TECH_DEBT-013): v5.14.8.A also bit-packs the has_*
// flags into a single uint64_t `has_flags` field per the BIT_FLAG
// storage class pattern (CLAUDE.md item 1 — Portfolio uint16_t bitmap
// precedent; FOREACH_FAILURE_MODE pseudo-registry pattern at
// MemHeaders/FailureModeRegistry.hpp). For v5.14.8.0, this header
// uses the same byte-per-has-flag pattern as StampBoundCfgRegistry to
// keep the .0 sub-tag focused on infrastructure; bit-packing migration
// + accessor-macro substitution happens in .A.
//
// FORWARD-COMPAT (Surface G): has_<name>=0 default for legacy stamps
// means the parser leaves new fields untouched on a v5.14.7 stamp;
// load proceeds normally with has_*=0 sentinels. MODEL_FORMAT_VERSION
// stays at 6 (UNCHANGED — Surface G discipline; new fields are
// optional canonical body lines).
//
// SISTER REGISTRY DISTINCTION:
//   FOREACH_STAMP_BOUND_CFG    — cfg fields (drift detection between
//                                 trainer cfg + engine cfg; emit_when
//                                 typically gated on cfg flag)
//   FOREACH_STAMP_BOUND_MODEL_CONST — architectural model-const fields
//                                     (always populated when produced;
//                                     describe the model itself, not cfg)
//======================================================================================================
#ifndef STAMP_BOUND_MODEL_CONST_REGISTRY_HPP
#define STAMP_BOUND_MODEL_CONST_REGISTRY_HPP

#include <stdlib.h>   // atoi, strncpy, strtoull
#include <string.h>   // strncpy, strncmp
#include <cstdint>    // v5.15.5.F.4d — uint64_t used in expansion of FOREACH_STAMP_BOUND_MODEL_CONST_POST_CFG bandit/thompson rows; explicit per IWYU discipline (sister to TECH_DEBT-083 IWYU sweep)
#include <type_traits>  // v5.14.8.A.merged.4 — std::is_array_v / extent_v / is_floating_point_v / is_unsigned_v for tt::stamp_parse_field<T>
#include "../MemHeaders/BitmapMacros.hpp"  // BITMAP_* primitives (v5.14.8.A.0.b.1) backing STAMP_HAS / SET / CLR aliases

//======================================================================================================
// [STRING-FIELD TYPE ALIASES]
//======================================================================================================
// C++ array-typedef forms for char[N] fields. The X-macro tuple's `type`
// column needs a single-token type name; `char[65]` doesn't tokenize
// cleanly (the '[' splits parameters). Aliases give us a single token.
//
// Detection at template-dispatch time: `std::is_array_v<stamp_str_65>`
// returns true; `std::extent_v<stamp_str_65>` gives the size at compile
// time. This lets `if constexpr` parser/emitter macros (v5.14.8.A.0.b
// Step 4) handle string fields uniformly with int/uint/double types.
//
// Naming convention: `stamp_str_<N>` where N = total array size including
// null terminator. Add new aliases as char[N] fields are introduced.
//======================================================================================================
namespace tt {
    using stamp_str_16   = char[16];    // xgb_tree_method, environment_cuda_version, environment_libgomp_version
    using stamp_str_32   = char[32];    // v5.14.8.D — environment_tf_version, environment_pytorch_version
    using stamp_str_64   = char[64];    // v5.14.8.D — run_name, environment_cpu_model
    using stamp_str_65   = char[65];    // scaler_sha256, overlay_hash, effective_hash, scaler_fit_data_hash
    using stamp_str_1024 = char[1024];  // v5.14.8.D — removal_reasons_csv (feature removal lineage)

    // v5.14.8.A.merged.4 — Templated parser helper. Extracts type-dispatch
    // logic from registry's parser X-macro expansion to a template function
    // so each instantiation discards non-matching `if constexpr` branches
    // properly. Required because non-template macro context does NOT discard
    // type-invalid syntax in non-taken if-constexpr branches (e.g.,
    // `(char[16])(double)` is a hard cast error even when type is double).
    //
    // Usage at parser X-macro: `tt::stamp_parse_field(r.name, val);`
    // Each registry consumer (parser per-entry) instantiates this once;
    // instantiation properly discards branches per T.
    //
    // Forward declaration of parse_double_fast to avoid include cycle —
    // the actual definition lives in ModelInference.hpp's namespace tt.
    inline double parse_double_fast(const char* s);

    template <typename T>
    inline void stamp_parse_field(T& dst, const char* val, const char* fmt = "") {
        if constexpr (std::is_array_v<T>) {
            strncpy(dst, val, std::extent_v<T> - 1);
            dst[std::extent_v<T> - 1] = '\0';
        } else if constexpr (std::is_floating_point_v<T>) {
            dst = static_cast<T>(parse_double_fast(val));
        } else if constexpr (std::is_unsigned_v<T>) {
            // v5.15.0.B refinement — auto-detect base from emit format string
            // (DRY: fmt is single source of truth for both emit AND parse).
            // Hex-encoded uint64 fields (build_flags_hash, label_registry_hash,
            // feature_mask) emit via "%016lx" / "%lx" → strchr finds 'x' →
            // base 16. Decimal fields ("%u", "%lu", "%llu", "%d") → base 10.
            // Boot-only path; 1 strchr scan per field load is negligible.
            // Closes the recurring "add a new hex field → also touch the
            // parser manually" Class 18 mirror; future hex fields auto-flow.
            const int base = (fmt[0] != '\0' &&
                              (strchr(fmt, 'x') != nullptr ||
                               strchr(fmt, 'X') != nullptr)) ? 16 : 10;
            dst = static_cast<T>(strtoull(val, nullptr, base));
        } else {
            dst = static_cast<T>(atoi(val));
        }
    }
}

//======================================================================================================
// [REGISTRY ENTRY SHAPE]
//======================================================================================================
// X(name, group, presence, type, fmt, default_val, get_value_expr, emit_when, doc_comment)
//
//   name           — canonical stamp body key (also struct field name).
//                    Must be a valid C identifier; written to stamp body
//                    as `<name>=<value>\n` line.
//   group          — has_* flag group; "_" for standalone (own bit), or
//                    a group name from FOREACH_STAMP_BOUND_MODEL_CONST_GROUPS.
//                    All entries with the same group share one has_<group>
//                    flag at struct generation time.
//   presence       — token marker controlling which structs include this
//                    field at struct-generation time. Token-paste-based
//                    dispatch (preprocessor):
//                      INCLUDE      — field on ALL THREE structs
//                                     (ModelStampResult parser-side,
//                                     StampInferenceCfgInputs emit-side,
//                                     ModelHandle runtime-side). Default.
//                      SKIP_HANDLE  — field on parser + emit only; NOT
//                                     on ModelHandle (parser checks the
//                                     value but doesn't propagate to
//                                     runtime). Used for: held_out_fraction,
//                                     feature_scaler_present (the boolean;
//                                     scaler_sha256 IS on handle),
//                                     grid_member_count, grid_member_idx,
//                                     label_registry_hash, feature_mask.
//                    Future markers if needed: PARSER_ONLY (parsed but
//                    not emitted; none today). The token-paste pattern
//                    extends with one new HANDLE_GEN_<marker> macro per
//                    new marker.
//   type           — C++ type: int, uint64_t, double, or `tt::stamp_str_N`
//                    typedef for char[N] string fields. Drives struct
//                    field type, parser dispatch, fmt format string.
//   fmt            — printf format. Types:
//                      int       → "%d"
//                      uint64_t  → "%llu"
//                      double    → "%.17g" (lossless round-trip)
//                      char[N]   → "%s" (string field)
//   default_val    — zero-init value: 0 for numeric, "" for char arrays.
//                    Set in verify_model_stamp's init block before parsing.
//   get_value_expr — expression to extract value from variable named
//                    `model_meta` (or equivalent caller-scope name) at
//                    the call site. The macro doesn't know what the
//                    source struct looks like; the caller's scope provides
//                    it. Examples:
//                      `now_us`                   (uint64_t direct)
//                      `meta.run_name`            (char[N] member access)
//                      `compute_fit_hash(scaler)` (function call)
//   emit_when      — boolean expression evaluated at production-caller
//                    emit time. When TRUE, has_<name>=1 + value
//                    populated; when FALSE, has_<name>=0 (skip emit;
//                    legacy-stamp shape).
//                    For ALWAYS-populated fields (e.g., training_timestamp,
//                    run_name): use literal `1`.
//                    For OPTIONAL fields (e.g., scaler_fit_data_hash —
//                    only set when scaler was fit): use guard expression
//                    (e.g., `(meta.scaler_fit_data_hash[0] != '\0')`).
//   doc_comment    — short string explaining the field's purpose. Shown
//                    in compile-time error messages + extracted by
//                    documentation tooling.
//
// IMPORTANT: get_value_expr + emit_when are evaluated at the CALLER
// (BacktestPanels production stamp emit + similar sites). The macro
// doesn't know what the source variables are; caller's scope provides
// them. This keeps source dependencies cleanly separated.
//
// AUTO-POPULATE: production stamp emit uses `STAMP_MODEL_CONST_AUTOPOPULATE
// (inf, model_meta)` macro to expand into per-field gated populator
// code. This eliminates the v5.9.5b production-caller field-population
// gap class — adding a new architectural stamp field becomes ONE line
// in this registry; populator is auto-generated.
//======================================================================================================

// ANTI-PATTERN: Do NOT add architectural stamp body fields as flat
// struct field declarations directly in ModelStampResult /
// StampInferenceCfgInputs / ModelHandle. Use this registry. Each new
// architectural field = 1 registry line; struct + parser + emitter +
// populator all auto-generated.
//
// Adding a new architectural field — example shape:
//   X(my_new_field, _, uint64_t, "%llu", 0,
//       compute_my_value(meta), 1,
//       "doc string explaining the field")
//
// For new GROUP fields:
//   1. Add group declaration to FOREACH_STAMP_BOUND_MODEL_CONST_GROUPS
//   2. Add per-field entries to FOREACH_STAMP_BOUND_MODEL_CONST with
//      group=<group_name>; all fields share one has_<group_name> bit

//======================================================================================================
// [GROUP DECLARATIONS — v5.14.8.A]
//======================================================================================================
// Groups are has_* flags that gate MULTIPLE typed fields populated
// together. Each group declaration auto-allocates one bit position in
// the bitmap. Standalone fields (group="_") auto-allocate their own bits.
//
// Wire format preserved: existing stamps with `has_xgb_hyperparams=1`
// + 8 xgb_* value lines parse identically; group bit set → all 8
// fields populated. C++ has_<group> semantics: gate one bit, populate
// N fields together.

#define FOREACH_STAMP_BOUND_MODEL_CONST_GROUPS(X)                                                   \
    X(inference_cfg,    "inference cfg fields (4): confidence_threshold_scale, barrier_gate_enabled, confidence_hard_block_threshold, held_out_fraction (freshness_tau DELETED v5.14.9.D — TECH_DEBT-004 close)") \
    X(scaler,           "scaler fields (2): feature_scaler_present, scaler_sha256")                 \
    X(fees,             "fee rate fields (2): fee_rate_maker, fee_rate_taker")                      \
    X(xgb_hyperparams,  "xgb hyperparams (9): max_depth, learning_rate, n_estimators, subsample, colsample_bytree, min_child_weight, seed, tree_method, train_nthread (since v5.14.8.A.merged)") \
    X(grid_member,      "grid member metadata (2): grid_member_count, grid_member_idx — renamed from grid_member v5.14.8.A.merged for dispatcher cleanliness") \
    X(label_params,     "label params (3): lookahead_ticks, tp_pct, sl_pct")

//======================================================================================================
// [STANDALONE has_* DECLARATIONS]
//======================================================================================================
// Lists standalone entries (group="_" in main FOREACH). Each gets its
// own has_<name> declaration. Adding a new standalone field requires:
//   1. Add line to this STANDALONE macro (declares has_<name>)
//   2. Add line to main FOREACH_STAMP_BOUND_MODEL_CONST (declares typed value)
// Two places per addition; vs current ~5 sites. Alternative would
// require preprocessor magic to detect group="_" in main FOREACH;
// not worth the complexity for this gain.

#define FOREACH_STAMP_BOUND_MODEL_CONST_STANDALONE(X)                                               \
    X(bandit,                       "bandit blend ratio set (1 field)")                             \
    X(training_poll_interval,       "training poll cadence set (1 field)")                          \
    X(model_num_outputs,            "model output dimension set (1 field)")                         \
    X(build_flags_hash,             "build flags hash set (1 field)")                               \
    X(label_registry_hash,          "label registry hash set (1 field)")                            \
    X(feature_mask,                 "feature mask set (1 field)")                                   \
    X(xgb_train_nthread,            "xgb training thread count set (1 field)")

//======================================================================================================
// [TYPED VALUE ENTRIES — v5.14.8.A; emit order = canonical body order]
//======================================================================================================
// Order matches stamp_write_for_model emit sequence (ModelInference.hpp
// :2175-2378). Wire format byte-for-byte preserved for HMAC verification.
// has_* gating: standalone (group="_") gates a single field; group
// (group=<name>) gates all entries with that group_name.
//
// Tuple: X(name, group, presence, type, fmt, default_val, get_value_expr, emit_when, doc)
//
// presence (NEW v5.14.8.A.0.b): controls ModelHandle inclusion.
//   INCLUDE     — on all 3 structs (default; new entries should use this)
//   SKIP_HANDLE — parser/emit only; not on ModelHandle (parser checks but
//                 doesn't propagate to runtime). 6 entries today:
//                 inference_cfg_held_out_fraction, feature_scaler_present
//                 (the boolean; scaler_sha256 IS on handle), grid_member_count,
//                 grid_member_idx, label_registry_hash, feature_mask.

// PRE_CFG section: entries that emit BEFORE FOREACH_STAMP_BOUND_CFG in
// canonical wire format. 26 entries today. Adding new pre-cfg field =
// 1 row here.
#define FOREACH_STAMP_BOUND_MODEL_CONST_PRE_CFG(X)                                                  \
    /* === inference_cfg group (4 fields) DELETED at v5.15.5.F.4d.1.B.3 Phase F HIGH-1 (b) ===   */ \
    /* Cleanup completes the single-source-of-truth migration: 3 rows (confidence_threshold_scale / */ \
    /* barrier_gate_enabled / confidence_hard_block_threshold) had cfg-derived sisters; held_out_  */ \
    /* fraction migrated to cfg-derived cohort at this ship. ModelHandle + ModelStampResult +     */ \
    /* StampInferenceCfgInputs all gain these fields via STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN     */ \
    /* (cfg-derived auto-gen; sister to ModelStampResult struct-gen at ModelInference.hpp:1236).  */ \
    /* v1 stamps continue loading via FOREACH_LEGACY_PREFIXED_KEY back-compat dispatch (parser    */ \
    /* translates `inference_cfg_<name>=` → unprefixed → cfg-derived framework). `has_inference_cfg`*/ \
    /* group bit + group declaration at line 231 retained as dead infra; future ship cleans up.   */ \
    /* v5.14.9.D PRECEDENT: freshness_tau DELETED via same pattern (TECH_DEBT-004 close).         */ \
    /* === bandit (standalone) — emitted at line 2189 === */                                        \
    X(inference_cfg_bandit_blend_ratio,         _, INCLUDE, double, "%g", 0.0,                      \
      inf->bandit_blend_ratio, inf->has_bandit, "bandit blend ratio (Exp3 vs ridge)")               \
    /* === fees group (2 fields) — emitted at line 2195 === */                                      \
    X(inference_cfg_fee_rate_maker,             fees, INCLUDE, double, "%g", 0.0,                   \
      inf->fee_rate_maker, inf->has_fees, "maker fee rate at training time")                        \
    X(inference_cfg_fee_rate_taker,             fees, INCLUDE, double, "%g", 0.0,                   \
      inf->fee_rate_taker, inf->has_fees, "taker fee rate at training time")                        \
    /* === training_poll_interval (standalone) — emitted at line 2203 === */                        \
    X(training_poll_interval,                   _, INCLUDE, uint32_t, "%u", 0,                      \
      (unsigned)inf->training_poll_interval, inf->has_training_poll_interval,                       \
      "training data poll cadence; engine boot WARN on cross-cadence drift")                        \
    /* === scaler group (2 fields) — emitted at line 2211; gated by has_scaler === */               \
    /* v5.14.8.A.0.b — re-added during pre-flight registry data completion;                      */ \
    /* originally dropped between training_poll_interval and xgb_hyperparams in v5.14.8.A.1.    */ \
    /* GATE-NEW-2 wire-format preservation depends on these landing here.                       */ \
    /* feature_scaler_present is parser-only (scaler_sha256 IS on handle, but the boolean isn't  */ \
    /* needed at runtime — has_scaler flag suffices).                                            */ \
    X(feature_scaler_present,                   scaler, SKIP_HANDLE, uint8_t, "%d", 0,              \
      (uint8_t)(inf->feature_scaler_present ? 1 : 0), inf->has_scaler,                              \
      "scaler sidecar present flag (0=no/1=yes; uint8_t for bit-packing efficiency)")               \
    X(scaler_sha256,                            scaler, INCLUDE, tt::stamp_str_65, "%s", "",        \
      inf->scaler_sha256, inf->has_scaler,                                                          \
      "SHA-256 of full scaler sidecar file (64 hex + null)")                                        \
    /* === model_num_outputs (standalone) — emitted at line 2223; gated by has_model_num_outputs */ \
    X(model_num_outputs,                        _, INCLUDE, int, "%d", 0,                           \
      inf->model_num_outputs, inf->has_model_num_outputs,                                           \
      "model output dimension; binary/regression=1, multiclass=N (REFUSE on mismatch)")             \
    /* === xgb_hyperparams group (8 fields) — emitted at line 2234 === */                           \
    X(xgb_max_depth,                            xgb_hyperparams, INCLUDE, int, "%d", 0,             \
      inf->xgb_max_depth, inf->has_xgb_hyperparams, "XGBoost max tree depth")                       \
    X(xgb_learning_rate,                        xgb_hyperparams, INCLUDE, double, "%g", 0.0,        \
      inf->xgb_learning_rate, inf->has_xgb_hyperparams, "XGBoost learning rate")                    \
    X(xgb_n_estimators,                         xgb_hyperparams, INCLUDE, int, "%d", 0,             \
      inf->xgb_n_estimators, inf->has_xgb_hyperparams, "XGBoost number of trees")                   \
    X(xgb_subsample,                            xgb_hyperparams, INCLUDE, double, "%g", 0.0,        \
      inf->xgb_subsample, inf->has_xgb_hyperparams, "XGBoost row subsample fraction")               \
    X(xgb_colsample_bytree,                     xgb_hyperparams, INCLUDE, double, "%g", 0.0,        \
      inf->xgb_colsample_bytree, inf->has_xgb_hyperparams, "XGBoost column subsample per tree")     \
    X(xgb_min_child_weight,                     xgb_hyperparams, INCLUDE, int, "%d", 0,             \
      inf->xgb_min_child_weight, inf->has_xgb_hyperparams, "XGBoost min child weight")              \
    X(xgb_seed,                                 xgb_hyperparams, INCLUDE, int, "%d", 0,             \
      inf->xgb_seed, inf->has_xgb_hyperparams, "XGBoost RNG seed for reproducibility")              \
    /* v5.14.8.A.merged — xgb_tree_method (char[16]) folded into registry as 26th entry.        */ \
    /* Uses tt::stamp_str_16 typedef + if-constexpr dispatch from A.0.b. Replaces manual emit  */ \
    /* (ModelInference.hpp:2243) + manual parse (1629-1633). Wire format byte-identical.        */ \
    X(xgb_tree_method,                          xgb_hyperparams, INCLUDE, tt::stamp_str_16, "%s", "", \
      inf->xgb_tree_method, inf->has_xgb_hyperparams, "XGBoost tree-method enum (e.g., \"hist\")") \
    /* === build_flags_hash (standalone) — emitted at line 2253 === */                              \
    X(build_flags_hash,                         _, INCLUDE, uint64_t, "%016lx", 0,                  \
      (unsigned long)inf->build_flags_hash, inf->has_build_flags_hash,                              \
      "build-time feature flag hash; engine boot WARN on cross-binary drift")                       \
    /* === grid_member_count group (2 fields) — emitted at line 2266 === */                         \
    /* Forensic / informational on parse side; no runtime use → SKIP_HANDLE.                    */  \
    X(grid_member_count,                        grid_member, SKIP_HANDLE, int, "%d", 0, \
      inf->grid_member_count, inf->has_grid_member_count,                                           \
      "ensemble member count when trained as part of horizon set")                                  \
    X(grid_member_idx,                          grid_member, SKIP_HANDLE, int, "%d", 0, \
      inf->grid_member_idx, inf->has_grid_member_count, "this model's index within the grid")       \
    /* === label_registry_hash (standalone) — emitted at line 2278 === */                           \
    /* Parser checks against runtime LABEL_REGISTRY_HASH() at boot; not stored on handle.       */  \
    X(label_registry_hash,                      _, SKIP_HANDLE, uint64_t, "%016lx", 0,              \
      (unsigned long)inf->label_registry_hash, inf->has_label_registry_hash,                        \
      "label registry hash; engine boot REFUSE on mismatch (label set drift)")                      \
    /* === feature_mask (standalone) — emitted at line 2292 === */                                  \
    /* Parser compares against runtime cfg.core_feature_mask[core] at boot; not on handle.      */  \
    X(feature_mask,                             _, SKIP_HANDLE, uint64_t, "%016lx", 0,              \
      (unsigned long)inf->feature_mask_train, inf->has_feature_mask,                                \
      "feature mask at training time; engine compares to runtime feature_mask")                     \
    /* === label_params group (3 fields) — emitted at line 2306 === */                              \
    X(label_lookahead_ticks,                    label_params, INCLUDE, int, "%d", 0,                \
      inf->label_lookahead_ticks, inf->has_label_params, "label lookahead window in ticks")         \
    X(label_tp_pct,                             label_params, INCLUDE, double, "%.6g", 0.0,         \
      inf->label_tp_pct, inf->has_label_params, "label take-profit percent")                        \
    X(label_sl_pct,                             label_params, INCLUDE, double, "%.6g", 0.0,         \
      inf->label_sl_pct, inf->has_label_params, "label stop-loss percent")                          \
    /* === xgb_train_nthread (standalone) — emitted at line 2323 === */                             \
    X(xgb_train_nthread,                        _, INCLUDE, int, "%d", 0,                           \
      inf->xgb_train_nthread, inf->has_xgb_train_nthread,                                           \
      "XGBoost training thread count; lets operator detect serial vs parallel mode forensically")

// POST_CFG section: entries that emit AFTER FOREACH_STAMP_BOUND_CFG in
// canonical wire format (v5.14.8.A.merged.4 — TECH_DEBT-006 full closure).
//
// These are architectural fields whose canonical emit order places them
// AFTER the sister stamp-bound cfg fields. The split exists because a
// single FOREACH walk can't pause for the sister registry; the emitter
// walks PRE_CFG → FOREACH_STAMP_BOUND_CFG → POST_CFG to preserve
// canonical wire format byte-for-byte.
//
// Adding new POST_CFG entry: 1 row here + 1 enum bit in StampHasFlagBit
// + 1 MASK constant. Future field = 3-site update (acceptable for
// late-emit since it's bounded vs the original 9-site manual N-site
// pattern).
#define FOREACH_STAMP_BOUND_MODEL_CONST_POST_CFG(X)                                                 \
    /* === v5.14.2.E.2.B model-architectural fields — emitted at line ~2069 === */                  \
    X(expected_num_classes,                     _, SKIP_HANDLE, int, "%d", 0,                       \
      inf->expected_num_classes, inf->has_expected_num_classes,                                     \
      "model output dimension at training time (binary/regression=1, multiclass=K)")                \
    X(expected_role,                            _, SKIP_HANDLE, tt::stamp_str_16, "%s", "",         \
      inf->expected_role, inf->has_expected_role,                                                   \
      "operator's training-time role choice (buy_signal | barrier | regime | exit)")                \
    X(expected_num_features,                    _, SKIP_HANDLE, int, "%d", 0,                       \
      inf->expected_num_features, inf->has_expected_num_features,                                   \
      "MODEL_NUM_FEATURES at training time")                                                        \
    X(expected_feature_format_version,          _, SKIP_HANDLE, int, "%d", 0,                       \
      inf->expected_feature_format_version, inf->has_expected_feature_format_version,               \
      "MODEL_FORMAT_VERSION at training time")                                                      \
    /* === v5.14.3.B overlay-derived fields — emitted at line ~2092 === */                          \
    X(overlay_hash,                             _, INCLUDE, tt::stamp_str_65, "%s", "",             \
      inf->overlay_hash, inf->has_overlay_hash,                                                     \
      "SHA256 of canonical overlay JSON (3-layer fingerprinting layer 2)")                          \
    X(effective_hash,                           _, INCLUDE, tt::stamp_str_65, "%s", "",             \
      inf->effective_hash, inf->has_effective_hash,                                                 \
      "SHA256(layer1 || layer2) (3-layer fingerprinting layer 3)")                                  \
    /* === v5.14.8.D NEW fields — model lineage + environment metadata === */                       \
    /* training_timestamp_us: wall-clock at training time (μs since unix epoch)                  */ \
    /* used by v5.14.8.E stale-model gate (CoreModelZoo_CheckStaleModel)                         */ \
    X(training_timestamp_us,                    _, INCLUDE, uint64_t, "%lu", 0,                     \
      (unsigned long)inf->training_timestamp_us, inf->has_training_timestamp_us,                    \
      "training wall-clock (μs since unix epoch); v5.14.8.E stale-model gate reads this")           \
    /* run_name: operator-set training run identifier (e.g., 'btcusdt_5min_v3') */                  \
    X(run_name,                                 _, INCLUDE, tt::stamp_str_64, "%s", "",             \
      inf->run_name, inf->has_run_name,                                                             \
      "operator-set training run identifier; surfaces in stale-model log + ML Status panel")        \
    /* scaler_fit_data_hash: SHA256 of training data slice used to fit scaler.                   */ \
    /* trainer hashes features_train.tobytes(); verifier WARN/REFUSE on mismatch.                */ \
    X(scaler_fit_data_hash,                     _, INCLUDE, tt::stamp_str_65, "%s", "",             \
      inf->scaler_fit_data_hash, inf->has_scaler_fit_data_hash,                                     \
      "SHA-256 of training data slice fitted by scaler; catches scaler refit drift")                \
    /* removal_reasons_csv: per-feature CSV tracking why feature was excluded from training.    */ \
    /* read-only forensic field; no enforcement.                                                  */ \
    X(removal_reasons_csv,                      _, INCLUDE, tt::stamp_str_1024, "%s", "",           \
      inf->removal_reasons_csv, inf->has_removal_reasons_csv,                                       \
      "CSV: <feature>=<reason>,...; tracks why features were pruned from training")                 \
    /* === environment_meta GROUP (5 fields) — operator audit; informational; no enforcement === */ \
    X(environment_tf_version,                   environment_meta, INCLUDE, tt::stamp_str_32, "%s", "", \
      inf->environment_tf_version, inf->has_environment_meta,                                       \
      "TensorFlow version at training time (e.g., 'tensorflow==2.15.0')")                           \
    X(environment_pytorch_version,              environment_meta, INCLUDE, tt::stamp_str_32, "%s", "", \
      inf->environment_pytorch_version, inf->has_environment_meta,                                  \
      "PyTorch version at training time (e.g., 'torch==2.1.0')")                                    \
    X(environment_cuda_version,                 environment_meta, INCLUDE, tt::stamp_str_16, "%s", "", \
      inf->environment_cuda_version, inf->has_environment_meta,                                     \
      "CUDA version at training time (e.g., '12.1')")                                               \
    X(environment_cpu_model,                    environment_meta, INCLUDE, tt::stamp_str_64, "%s", "", \
      inf->environment_cpu_model, inf->has_environment_meta,                                        \
      "CPU model string from platform.processor() at training time")                                \
    X(environment_libgomp_version,              environment_meta, INCLUDE, tt::stamp_str_16, "%s", "", \
      inf->environment_libgomp_version, inf->has_environment_meta,                                  \
      "libgomp.so version at training time (XGBoost OpenMP runtime)")                               \
    /* === v5.15.5.A.7 — Per-horizon barrier serving cohort (PARITY-024 close).               */    \
    /* Appended AT END of POST_CFG section for HMAC chain byte preservation per                 */    \
    /* wire-format-byte-preservation-discipline.md. Legacy v5.15.4- stamps lack these lines;    */    \
    /* parser tolerates absent keys (Surface G forward-compat); has_inference_cfg group flag    */    \
    /* gates emit (when inference_cfg group is populated, all 4 new fields emit too).            */    \
    /* Population: cfg→inf flows via NEW INFERENCE_CFG_AUTOPOPULATE in StampHelper (CLOSES       */    \
    /* TECH_DEBT-037 — manual section 2a extinct). cfg-side gate (feature-on/off) lives in       */    \
    /* CfgDerivedInferenceCfgRegistry.hpp's gate_when column.                                    */    \
    X(inference_cfg_ml_tp_pct,                  inference_cfg, INCLUDE, double, "%.17g", 0.0,        \
      inf->inference_cfg_ml_tp_pct, inf->has_inference_cfg,                                          \
      "training-time cfg.ml_tp_pct snapshot (legacy single-barrier fallback; drift Tier 1 in strict mode)") \
    X(inference_cfg_ml_sl_pct,                  inference_cfg, INCLUDE, double, "%.17g", 0.0,        \
      inf->inference_cfg_ml_sl_pct, inf->has_inference_cfg,                                          \
      "training-time cfg.ml_sl_pct snapshot (legacy single-barrier fallback; drift Tier 1 in strict mode)") \
    X(inference_cfg_barrier_blend_mode,         inference_cfg, INCLUDE, int, "%d", 0,                \
      inf->inference_cfg_barrier_blend_mode, inf->has_inference_cfg,                                 \
      "training-time cfg.barrier_blend_mode enum (LEGACY/BLEND/DOMINANT/BOTH_*; dispatch shape; drift Tier 1)") \
    X(inference_cfg_per_horizon_barrier_blend,  inference_cfg, INCLUDE, int, "%d", 0,                \
      inf->inference_cfg_per_horizon_barrier_blend, inf->has_inference_cfg,                          \
      "training-time per_horizon_barrier_blend feature master gate (0/1 from ml_cfg_flags bitmap; drift Tier 1)") \
    /* === v5.15.5.F.4d PARITY-026 close — 4 STAMP_BOUND bandit/thompson fields since v5.14.10.B were missing POST_CFG entries + 1 NEW field for BLENDED state === */ \
    /* Per § C.3 of merged plan body. APPEND-at-end preserves HMAC chain byte equivalence for legacy stamps (Surface G forward-compat). */ \
    /* All 5 fields share existing `inference_cfg` STAMP_BIT group (established at .A.7); no new bit allocation. */ \
    X(inference_cfg_bandit_algorithm,           inference_cfg, INCLUDE, int,    "%d",    0,          \
      inf->inference_cfg_bandit_algorithm,           inf->has_inference_cfg,                         \
      "training-time cfg.bandit_algorithm snapshot (drift Tier 2 WARN to avoid false-positive on legacy cfg=2 stamps post-.F.4d Option C semantic flip; semantic for cfg=2 changed from BOTH → EXP3_OP_THOMPSON_GHOST with Class 24 sister attribution fix)") \
    X(inference_cfg_thompson_mu_prior,          inference_cfg, INCLUDE, double, "%.17g", 0.0,        \
      inf->inference_cfg_thompson_mu_prior,          inf->has_inference_cfg,                        \
      "training-time cfg.thompson_mu_prior snapshot (drift Tier 1; Thompson posterior mean prior; parity-critical)") \
    X(inference_cfg_thompson_precision_prior,   inference_cfg, INCLUDE, double, "%.17g", 1.0,        \
      inf->inference_cfg_thompson_precision_prior,   inf->has_inference_cfg,                        \
      "training-time cfg.thompson_precision_prior snapshot (drift Tier 1; Thompson posterior precision prior)") \
    X(inference_cfg_thompson_precision_obs,     inference_cfg, INCLUDE, double, "%.17g", 1.0,        \
      inf->inference_cfg_thompson_precision_obs,     inf->has_inference_cfg,                        \
      "training-time cfg.thompson_precision_obs snapshot (drift Tier 1; Thompson observation precision)") \
    X(inference_cfg_thompson_exp3_blend_alpha,  inference_cfg, INCLUDE, double, "%.17g", 0.5,        \
      inf->inference_cfg_thompson_exp3_blend_alpha,  inf->has_inference_cfg,                        \
      "training-time cfg.thompson_exp3_blend_alpha snapshot (drift Tier 1; only meaningful when bandit_algorithm == 4 BLENDED; reproducibility requires α lock)")

// Union: walks both PRE_CFG and POST_CFG. Used by struct generation +
// AUTOPOPULATE + entry counting (everything that doesn't care about
// emit ordering relative to FOREACH_STAMP_BOUND_CFG).
#define FOREACH_STAMP_BOUND_MODEL_CONST(X) \
    FOREACH_STAMP_BOUND_MODEL_CONST_PRE_CFG(X) \
    FOREACH_STAMP_BOUND_MODEL_CONST_POST_CFG(X)

//======================================================================================================
// [PARSER DISPATCH MACROS]
//======================================================================================================
// Per-type parsers used in the parser X-macro expansion (verify_model_stamp).
// Add a new type? Add a new STAMP_MODEL_CONST_PARSE_<type> macro below.
//   STAMP_MODEL_CONST_PARSE(int, val)      → atoi(val)
//   STAMP_MODEL_CONST_PARSE(uint64_t, val) → (uint64_t)strtoull(val, NULL, 10)
//   STAMP_MODEL_CONST_PARSE(double, val)   → tt::parse_double_fast(val)
//   STAMP_MODEL_CONST_PARSE(char_array, val, dest, max_len) → strncpy(dest, val, max_len-1)
//======================================================================================================

#define STAMP_MODEL_CONST_PARSE_int(val)      atoi(val)
#define STAMP_MODEL_CONST_PARSE_uint64_t(val) (uint64_t)strtoull(val, NULL, 10)
#define STAMP_MODEL_CONST_PARSE_double(val)   tt::parse_double_fast(val)

//======================================================================================================
// [BIT ALLOCATION FOR has_flags — v5.14.8.A.merged]
//======================================================================================================
// Each group + each standalone entry gets ONE bit position in the
// uint64_t has_flags field that lives on ModelStampResult /
// StampInferenceCfgInputs / ModelHandle (post-Option-1 unification).
//
// Allocation: groups first (6 bits), then standalones (7 bits) = 13
// total. uint64_t has 51 bits headroom for future fields. Hand-allocated
// so debug output reads cleanly; build-time test asserts STAMP_BIT_COUNT
// matches FOREACH_STAMP_BOUND_MODEL_CONST_GROUP_COUNT + standalone count.
//
// Adding new group: 1 enum line + 1 MASK line + 1 GROUPS macro entry +
// 1 dispatcher #define (STAMP_AUTOPOPULATE_SET_HAS_<newgroup>).
// Adding new standalone: 1 enum line + 1 MASK line + 1 FOREACH entry
// (group="_") + 1 STANDALONE macro entry. Build-time count check catches
// site-drift.
//
// Standalone has_* names use the entry's full canonical name (mechanical
// derivation; no STANDALONE list dispatch). Verbose for some entries
// (has_inference_cfg_bandit_blend_ratio) but unambiguous + IDE auto-
// completes. Eliminates 2-site STANDALONE dispatch maintenance.
//======================================================================================================
namespace tt {
enum StampHasFlagBit : uint64_t {
    // === Group bits (one per group; gates ALL fields in group) ===
    STAMP_BIT_inference_cfg = 0,
    STAMP_BIT_scaler,
    STAMP_BIT_fees,
    STAMP_BIT_xgb_hyperparams,
    STAMP_BIT_grid_member,
    STAMP_BIT_label_params,
    STAMP_BIT_environment_meta,    // v5.14.8.D — 5-field environment_meta group

    // === Standalone bits — PRE_CFG section (emitted before FOREACH_STAMP_BOUND_CFG) ===
    STAMP_BIT_inference_cfg_bandit_blend_ratio,
    STAMP_BIT_training_poll_interval,
    STAMP_BIT_model_num_outputs,
    STAMP_BIT_build_flags_hash,
    STAMP_BIT_label_registry_hash,
    STAMP_BIT_feature_mask,
    STAMP_BIT_xgb_train_nthread,

    // === Standalone bits — POST_CFG section (emitted after FOREACH_STAMP_BOUND_CFG; v5.14.8.A.merged.4) ===
    // Late-emit architectural fields. Order matches canonical wire format:
    // expected_num_classes → expected_role → expected_num_features →
    // expected_feature_format_version → overlay_hash → effective_hash.
    STAMP_BIT_expected_num_classes,
    STAMP_BIT_expected_role,
    STAMP_BIT_expected_num_features,
    STAMP_BIT_expected_feature_format_version,
    STAMP_BIT_overlay_hash,
    STAMP_BIT_effective_hash,
    // v5.14.8.D NEW POST_CFG standalone bits:
    STAMP_BIT_training_timestamp_us,
    STAMP_BIT_run_name,
    STAMP_BIT_scaler_fit_data_hash,
    STAMP_BIT_removal_reasons_csv,

    STAMP_BIT_COUNT  // sentinel: total used bits (must be ≤ 64 for uint64_t has_flags)
};
static_assert(STAMP_BIT_COUNT <= 64, "stamp body has_flags exceeds uint64_t capacity");
}  // namespace tt

// MASK_<X> constants — one per bit position. Used by STAMP_HAS / SET / CLR
// accessor macros + caller code that does multi-flag checks via
// BITMAP_ANY(s.has_flags, MASK_X | MASK_Y).
#define MASK_inference_cfg                          (1ULL << tt::STAMP_BIT_inference_cfg)
#define MASK_scaler                                 (1ULL << tt::STAMP_BIT_scaler)
#define MASK_fees                                   (1ULL << tt::STAMP_BIT_fees)
#define MASK_xgb_hyperparams                        (1ULL << tt::STAMP_BIT_xgb_hyperparams)
#define MASK_grid_member                            (1ULL << tt::STAMP_BIT_grid_member)
#define MASK_label_params                           (1ULL << tt::STAMP_BIT_label_params)
#define MASK_inference_cfg_bandit_blend_ratio       (1ULL << tt::STAMP_BIT_inference_cfg_bandit_blend_ratio)
#define MASK_training_poll_interval                 (1ULL << tt::STAMP_BIT_training_poll_interval)
#define MASK_model_num_outputs                      (1ULL << tt::STAMP_BIT_model_num_outputs)
#define MASK_build_flags_hash                       (1ULL << tt::STAMP_BIT_build_flags_hash)
#define MASK_label_registry_hash                    (1ULL << tt::STAMP_BIT_label_registry_hash)
#define MASK_feature_mask                           (1ULL << tt::STAMP_BIT_feature_mask)
#define MASK_xgb_train_nthread                      (1ULL << tt::STAMP_BIT_xgb_train_nthread)
// POST_CFG late-emit standalone masks (v5.14.8.A.merged.4):
#define MASK_expected_num_classes                   (1ULL << tt::STAMP_BIT_expected_num_classes)
#define MASK_expected_role                          (1ULL << tt::STAMP_BIT_expected_role)
#define MASK_expected_num_features                  (1ULL << tt::STAMP_BIT_expected_num_features)
#define MASK_expected_feature_format_version        (1ULL << tt::STAMP_BIT_expected_feature_format_version)
#define MASK_overlay_hash                           (1ULL << tt::STAMP_BIT_overlay_hash)
#define MASK_effective_hash                         (1ULL << tt::STAMP_BIT_effective_hash)
// v5.14.8.D — 5 new fields:
#define MASK_environment_meta                       (1ULL << tt::STAMP_BIT_environment_meta)
#define MASK_training_timestamp_us                  (1ULL << tt::STAMP_BIT_training_timestamp_us)
#define MASK_run_name                               (1ULL << tt::STAMP_BIT_run_name)
#define MASK_scaler_fit_data_hash                   (1ULL << tt::STAMP_BIT_scaler_fit_data_hash)
#define MASK_removal_reasons_csv                    (1ULL << tt::STAMP_BIT_removal_reasons_csv)

//======================================================================================================
// [STAMP_HAS / SET / CLR accessor macros — alias to BITMAP_* API]
//======================================================================================================
// Ergonomic accessors for the has_flags uint64_t bitmap. Aliases to the
// BITMAP_* primitives so callers don't touch raw bit operations.
//
// Usage:
//   if (STAMP_HAS(*m, inference_cfg)) { ... }   // group bit check
//   if (STAMP_HAS(*m, training_poll_interval)) { ... }  // standalone bit
//   if (BITMAP_ANY(m->has_flags,                // multi-flag branchless
//        MASK_inference_cfg | MASK_xgb_hyperparams)) { ... }
//
// Atomic variants available via BITMAP_ATOMIC_* directly when cross-thread
// visibility is needed (e.g., observability flags written by slow path,
// read by display thread).
//======================================================================================================
#define STAMP_HAS(s, name)  BITMAP_IS_SET((s).has_flags, MASK_##name)
#define STAMP_SET(s, name)  BITMAP_SET((s).has_flags, MASK_##name)
#define STAMP_CLR(s, name)  BITMAP_CLR((s).has_flags, MASK_##name)
#define STAMP_ANY(s, mask_set) BITMAP_ANY((s).has_flags, (mask_set))

//======================================================================================================
// [AUTO-POPULATE MACRO — eliminates v5.9.5b production-caller class]
//======================================================================================================
// Single-call auto-populate for production stamp emit. Replaces manual
// per-field populator blocks with one X-macro expansion that:
//   - For each registry entry: evaluate emit_when at the call site
//   - When TRUE: set inf.has_<name> = 1 + inf.<name> = (type)(get_value_expr)
//   - When FALSE: leave both at zero-init defaults (legacy stamp shape)
//
// Caller usage:
//
//   void some_emit_function(StampInferenceCfgInputs& inf,
//                            const ModelMetaSource& meta,
//                            uint64_t now_us) {
//     STAMP_MODEL_CONST_AUTOPOPULATE(inf, meta, now_us);
//   }
//
// `inf`, `meta`, `now_us` are the variable names the macro expansion
// uses. Caller MUST name their variables to match what the registry
// entries reference in get_value_expr. This eliminates the v5.9.5b
// production-caller field-population gap class — adding a new
// architectural field becomes ONE line in FOREACH_STAMP_BOUND_MODEL_CONST;
// the auto-populate expansion picks it up automatically next compile.
//
// String-field handling: char[N] fields use strncpy with explicit
// max_len from the registry; the macro form for char arrays is
// auto-dispatched per-type at expansion time (see .A migration code).
//======================================================================================================

// v5.15.3.A — STAMP_MODEL_CONST_AUTOPOPULATE QUARANTINED (PARITY-022).
//
// The macro is semantically wrong for model-const fields. Registry tuples
// pass `inf->X` as the `get_value` column (e.g., StampBoundModelConstRegistry.hpp:360-361,
// :431-432), so expansion becomes `(inf).X = (type)(inf->X)` — self-referential
// no-op. The macro had 0 production callers; bug was latent.
//
// Root cause: model-const fields (training_timestamp, run_name, grid_member_count,
// xgb hyperparams, etc.) come from training-time CALLER ARGS — NOT cfg
// auto-derivation. The companion-AUTOPOPULATE pattern (proven for
// STAMP_CFG_AUTOPOPULATE on cfg-bound fields) doesn't apply because there's
// no `cfg.X` to derive from. They populate via `StampArgs` in the
// `Stamp_AssembleAndEmit` helper (v5.15.3.A).
//
// Future architectural-field AUTOPOPULATE redesign tracked as TECH_DEBT-036
// (registry tuple restructure with separate value-source column).
//
// Any future caller that tries to use this macro fires the static_assert
// below at compile time — the error message points operators to the
// canonical alternative (Stamp_AssembleAndEmit).
#define STAMP_MODEL_CONST_AUTOPOPULATE(inf, meta, now_us)                            \
    static_assert(false,                                                              \
        "STAMP_MODEL_CONST_AUTOPOPULATE is QUARANTINED (PARITY-022; v5.15.3.A). "    \
        "Model-const fields populate manually from StampArgs in callers like "        \
        "Stamp_AssembleAndEmit. See TECH_DEBT-036 for architectural-field "          \
        "AUTOPOPULATE redesign.")

// Per-entry expansion. Numeric types use direct assignment; string
// types (char[N]) need separate strncpy-based handling. v5.14.8.A
// migration uses the numeric-only AUTOPOPULATE for simplicity; string
// fields (xgb_tree_method, scaler_sha256, etc.) populate manually
// alongside the AUTOPOPULATE call.
//
// Tuple signature (9 params; v5.14.8.A.0.b adds presence column):
//   X(name, group, presence, type, fmt, default_val, get_value, emit_when, doc)
//
// has_* dispatch (v5.14.8.A.merged): for grouped entries, sets the
// group bit (e.g., MASK_inference_cfg). For standalone entries
// (group="_"), sets the entry's own bit (e.g., MASK_training_poll_interval).
// Token-paste dispatch via STAMP_AUTOPOPULATE_SET_HAS_<group>(name).
// Adding new group = 1 new dispatcher #define (one per group); standalone
// is automatic via group="_" → STAMP_AUTOPOPULATE_SET_HAS__(name).
//
// String-field handling (v5.14.8.A.0.b.4): tt::stamp_str_N typedefs
// detected via std::is_array_v<type>. Numeric types use direct cast +
// assignment; string types use strncpy + null-terminate via
// std::extent_v<type> for size.

// Token-paste dispatcher: STAMP_AUTOPOPULATE_SET_HAS_<group_name>(entry_name)
// resolves to the right has_* field set. Standalone uses entry name; grouped
// entries share the group bit.
//
// Note on STAMP_HAS_FIELD_ON: for the EARLY pre-Option-1 phase (before
// struct migration), structs still have legacy per-entry has_* fields.
// We use STAMP_SET (BITMAP_SET on has_flags) since post-migration structs
// will have uint64_t has_flags. Dispatching via macro keeps the migration
// boundary localized.
#define STAMP_AUTOPOPULATE_SET_HAS__(name)                  STAMP_SET((inf), name)
#define STAMP_AUTOPOPULATE_SET_HAS_inference_cfg(name)      STAMP_SET((inf), inference_cfg)
#define STAMP_AUTOPOPULATE_SET_HAS_scaler(name)             STAMP_SET((inf), scaler)
#define STAMP_AUTOPOPULATE_SET_HAS_fees(name)               STAMP_SET((inf), fees)
#define STAMP_AUTOPOPULATE_SET_HAS_xgb_hyperparams(name)    STAMP_SET((inf), xgb_hyperparams)
#define STAMP_AUTOPOPULATE_SET_HAS_grid_member(name)        STAMP_SET((inf), grid_member)
#define STAMP_AUTOPOPULATE_SET_HAS_label_params(name)       STAMP_SET((inf), label_params)
// v5.14.8.D — environment_meta group dispatcher:
#define STAMP_AUTOPOPULATE_SET_HAS_environment_meta(name)   STAMP_SET((inf), environment_meta)

// Token-paste dispatcher for emit-time has_* check. Mirrors
// STAMP_AUTOPOPULATE_SET_HAS but for READING the bit (boolean) in the
// X-macro emit walk inside stamp_write_for_model. Same semantics —
// for grouped entries, checks the group bit; for standalone (group="_"),
// checks the entry's own bit.
#define STAMP_EMIT_CHECK_HAS__(name)                  STAMP_HAS(*inf, name)
#define STAMP_EMIT_CHECK_HAS_inference_cfg(name)      STAMP_HAS(*inf, inference_cfg)
#define STAMP_EMIT_CHECK_HAS_scaler(name)             STAMP_HAS(*inf, scaler)
#define STAMP_EMIT_CHECK_HAS_fees(name)               STAMP_HAS(*inf, fees)
#define STAMP_EMIT_CHECK_HAS_xgb_hyperparams(name)    STAMP_HAS(*inf, xgb_hyperparams)
#define STAMP_EMIT_CHECK_HAS_grid_member(name)        STAMP_HAS(*inf, grid_member)
#define STAMP_EMIT_CHECK_HAS_label_params(name)       STAMP_HAS(*inf, label_params)
// v5.14.8.D — environment_meta group emit-check:
#define STAMP_EMIT_CHECK_HAS_environment_meta(name)   STAMP_HAS(*inf, environment_meta)

// Parser-side dispatchers (hardcoded `r` target — used inside
// verify_model_stamp body; r is the local ModelStampResult). For
// grouped entries: set the group bit. For standalone (group="_"):
// set the entry's own bit. Mirrors STAMP_AUTOPOPULATE_SET_HAS pattern
// but with `r` target instead of `inf`.
//
// Used by v5.14.8.A.merged.4 + v5.14.8.D POST_CFG parser X-macro walks.
#define STAMP_PARSER_SET_HAS__(name)                  STAMP_SET(r, name)
#define STAMP_PARSER_SET_HAS_inference_cfg(name)      STAMP_SET(r, inference_cfg)
#define STAMP_PARSER_SET_HAS_scaler(name)             STAMP_SET(r, scaler)
#define STAMP_PARSER_SET_HAS_fees(name)               STAMP_SET(r, fees)
#define STAMP_PARSER_SET_HAS_xgb_hyperparams(name)    STAMP_SET(r, xgb_hyperparams)
#define STAMP_PARSER_SET_HAS_grid_member(name)        STAMP_SET(r, grid_member)
#define STAMP_PARSER_SET_HAS_label_params(name)       STAMP_SET(r, label_params)
#define STAMP_PARSER_SET_HAS_environment_meta(name)   STAMP_SET(r, environment_meta)

#define STAMP_MODEL_CONST_AUTOPOPULATE_ONE(name, group, presence, type, fmt, default_val, get_value, emit_when, doc) \
    if (emit_when) {                                                                  \
        STAMP_AUTOPOPULATE_SET_HAS_##group(name);                                     \
        if constexpr (std::is_array_v<type>) {                                        \
            strncpy((inf).name, (get_value), std::extent_v<type> - 1);                \
            (inf).name[std::extent_v<type> - 1] = '\0';                               \
        } else {                                                                      \
            (inf).name = (type)(get_value);                                           \
        }                                                                             \
    }

//======================================================================================================
// [TEST INSTRUMENTATION]
//======================================================================================================
// Compile-time field count for tests. Counts entries via macro counting.
// Used by tests to assert "all N expected fields are present in the
// registry" — catches accidental row deletion during refactors.
//
// Extensibility loop test pattern (CLAUDE.md Check 26 discipline):
//   #define X(name, group, presence, type, fmt, def, get, when, doc) \
//       do { /* compile-time existence check */ \
//           StampInferenceCfgInputs inf{}; \
//           (void)inf.has_##name; \
//           (void)inf.name; \
//       } while (0);
//   FOREACH_STAMP_BOUND_MODEL_CONST(X)
//   #undef X
//======================================================================================================

#define STAMP_MODEL_CONST_COUNT_ONE(name, group, presence, type, fmt, default_val, get_value, emit_when, doc) +1
#define FOREACH_STAMP_BOUND_MODEL_CONST_COUNT  (0 FOREACH_STAMP_BOUND_MODEL_CONST(STAMP_MODEL_CONST_COUNT_ONE))

// Group counter: how many group has_* flags exist (for bit allocation).
#define STAMP_MODEL_CONST_GROUP_COUNT_ONE(group_name, doc) +1
#define FOREACH_STAMP_BOUND_MODEL_CONST_GROUP_COUNT \
    (0 FOREACH_STAMP_BOUND_MODEL_CONST_GROUPS(STAMP_MODEL_CONST_GROUP_COUNT_ONE))

//======================================================================================================
// [STRUCT-GENERATION DISPATCH (presence-aware) — v5.14.8.A.0.b]
//======================================================================================================
// The token-paste pattern that lets ModelHandle struct generation skip
// fields marked SKIP_HANDLE while ModelStampResult + StampInferenceCfgInputs
// include all fields.
//
// Usage in v5.14.8.A.merged ModelHandle struct generation:
//   #define X(name, group, presence, type, fmt, def, get, when, doc) \
//       STAMP_HANDLE_GEN_##presence(name, type)
//   FOREACH_STAMP_BOUND_MODEL_CONST(X)
//   #undef X
//
// Token-paste resolves to STAMP_HANDLE_GEN_INCLUDE or STAMP_HANDLE_GEN_SKIP_HANDLE
// based on the presence column. Adding a new presence marker (e.g.,
// PARSER_ONLY) means defining one new STAMP_HANDLE_GEN_<MARKER> macro;
// no per-entry boilerplate.
//======================================================================================================

#define STAMP_HANDLE_GEN_INCLUDE(name, type)      type name;
#define STAMP_HANDLE_GEN_SKIP_HANDLE(name, type)  /* skip — parser-only */

#endif // STAMP_BOUND_MODEL_CONST_REGISTRY_HPP
