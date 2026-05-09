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

#include <stdlib.h>   // atoi, strncpy
#include <string.h>   // strncpy, strncmp

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
    using stamp_str_16 = char[16];   // xgb_tree_method
    using stamp_str_65 = char[65];   // scaler_sha256, overlay_hash, effective_hash, run_name
}

//======================================================================================================
// [REGISTRY ENTRY SHAPE]
//======================================================================================================
// X(name, type, fmt, default_val, get_value_expr, emit_when, doc_comment)
//
//   name           — canonical stamp body key (also struct field name).
//                    Must be a valid C identifier; written to stamp body
//                    as `<name>=<value>\n` line.
//   type           — C++ type: int, uint64_t, double, or `char[N]` for
//                    string fields. Drives struct field type, parser
//                    dispatch, fmt format string.
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
    X(inference_cfg,    "inference cfg fields (5): confidence_threshold_scale, barrier_gate_enabled, confidence_hard_block_threshold, held_out_fraction, freshness_tau") \
    X(scaler,           "scaler fields (2): feature_scaler_present, scaler_sha256")                 \
    X(fees,             "fee rate fields (2): fee_rate_maker, fee_rate_taker")                      \
    X(xgb_hyperparams,  "xgb hyperparams (8): max_depth, learning_rate, n_estimators, subsample, colsample_bytree, min_child_weight, seed, tree_method") \
    X(grid_member_count_group, "grid member metadata (2): grid_member_count, grid_member_idx")      \
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
// Tuple: X(name, group, type, fmt, default_val, get_value_expr, emit_when, doc)

#define FOREACH_STAMP_BOUND_MODEL_CONST(X)                                                          \
    /* === inference_cfg group (5 fields) — emitted at line 2175 === */                            \
    X(inference_cfg_confidence_threshold_scale, inference_cfg, double, "%g", 0.0,                   \
      inf->confidence_threshold_scale, inf->has_inference_cfg, "confidence threshold scale")        \
    X(inference_cfg_barrier_gate_enabled,       inference_cfg, int,    "%d", 0,                     \
      inf->barrier_gate_enabled, inf->has_inference_cfg, "barrier gate enabled flag")               \
    X(inference_cfg_confidence_hard_block_threshold, inference_cfg, double, "%g", 0.0,              \
      inf->confidence_hard_block_threshold, inf->has_inference_cfg, "confidence hard-block threshold") \
    X(inference_cfg_held_out_fraction,          inference_cfg, double, "%g", 0.0,                   \
      inf->held_out_fraction, inf->has_inference_cfg, "held-out fraction at training")              \
    X(inference_cfg_freshness_tau,              inference_cfg, double, "%g", 0.0,                   \
      inf->freshness_tau, inf->has_inference_cfg, "freshness tau decay")                            \
    /* === bandit (standalone) — emitted at line 2189 === */                                        \
    X(inference_cfg_bandit_blend_ratio,         _, double, "%g", 0.0,                               \
      inf->bandit_blend_ratio, inf->has_bandit, "bandit blend ratio (Exp3 vs ridge)")               \
    /* === fees group (2 fields) — emitted at line 2195 === */                                      \
    X(inference_cfg_fee_rate_maker,             fees, double, "%g", 0.0,                            \
      inf->fee_rate_maker, inf->has_fees, "maker fee rate at training time")                        \
    X(inference_cfg_fee_rate_taker,             fees, double, "%g", 0.0,                            \
      inf->fee_rate_taker, inf->has_fees, "taker fee rate at training time")                        \
    /* === training_poll_interval (standalone) — emitted at line 2203 === */                        \
    X(training_poll_interval,                   _, uint32_t, "%u", 0,                               \
      (unsigned)inf->training_poll_interval, inf->has_training_poll_interval,                       \
      "training data poll cadence; engine boot WARN on cross-cadence drift")                        \
    /* === scaler group (2 fields) — emitted at line 2211; gated by has_scaler === */               \
    /* v5.14.8.A.0.b — re-added during pre-flight registry data completion;                      */ \
    /* originally dropped between training_poll_interval and xgb_hyperparams in v5.14.8.A.1.    */ \
    /* GATE-NEW-2 wire-format preservation depends on these landing here.                       */ \
    X(feature_scaler_present,                   scaler, uint8_t, "%d", 0,                           \
      (uint8_t)(inf->feature_scaler_present ? 1 : 0), inf->has_scaler,                              \
      "scaler sidecar present flag (0=no/1=yes; uint8_t for bit-packing efficiency)")               \
    X(scaler_sha256,                            scaler, tt::stamp_str_65, "%s", "",                 \
      inf->scaler_sha256, inf->has_scaler,                                                          \
      "SHA-256 of full scaler sidecar file (64 hex + null)")                                        \
    /* === model_num_outputs (standalone) — emitted at line 2223; gated by has_model_num_outputs */ \
    X(model_num_outputs,                        _, int, "%d", 0,                                    \
      inf->model_num_outputs, inf->has_model_num_outputs,                                           \
      "model output dimension; binary/regression=1, multiclass=N (REFUSE on mismatch)")             \
    /* === xgb_hyperparams group (8 fields) — emitted at line 2234 === */                           \
    X(xgb_max_depth,                            xgb_hyperparams, int, "%d", 0,                      \
      inf->xgb_max_depth, inf->has_xgb_hyperparams, "XGBoost max tree depth")                       \
    X(xgb_learning_rate,                        xgb_hyperparams, double, "%g", 0.0,                 \
      inf->xgb_learning_rate, inf->has_xgb_hyperparams, "XGBoost learning rate")                    \
    X(xgb_n_estimators,                         xgb_hyperparams, int, "%d", 0,                      \
      inf->xgb_n_estimators, inf->has_xgb_hyperparams, "XGBoost number of trees")                   \
    X(xgb_subsample,                            xgb_hyperparams, double, "%g", 0.0,                 \
      inf->xgb_subsample, inf->has_xgb_hyperparams, "XGBoost row subsample fraction")               \
    X(xgb_colsample_bytree,                     xgb_hyperparams, double, "%g", 0.0,                 \
      inf->xgb_colsample_bytree, inf->has_xgb_hyperparams, "XGBoost column subsample per tree")     \
    X(xgb_min_child_weight,                     xgb_hyperparams, int, "%d", 0,                      \
      inf->xgb_min_child_weight, inf->has_xgb_hyperparams, "XGBoost min child weight")              \
    X(xgb_seed,                                 xgb_hyperparams, int, "%d", 0,                      \
      inf->xgb_seed, inf->has_xgb_hyperparams, "XGBoost RNG seed for reproducibility")              \
    /* xgb_tree_method is char[16] — handled by separate string-type macros at struct-gen time */   \
    /* === build_flags_hash (standalone) — emitted at line 2253 === */                              \
    X(build_flags_hash,                         _, uint64_t, "%016lx", 0,                           \
      (unsigned long)inf->build_flags_hash, inf->has_build_flags_hash,                              \
      "build-time feature flag hash; engine boot WARN on cross-binary drift")                       \
    /* === grid_member_count group (2 fields) — emitted at line 2266 === */                         \
    X(grid_member_count,                        grid_member_count_group, int, "%d", 0,              \
      inf->grid_member_count, inf->has_grid_member_count,                                           \
      "ensemble member count when trained as part of horizon set")                                  \
    X(grid_member_idx,                          grid_member_count_group, int, "%d", 0,              \
      inf->grid_member_idx, inf->has_grid_member_count, "this model's index within the grid")       \
    /* === label_registry_hash (standalone) — emitted at line 2278 === */                           \
    X(label_registry_hash,                      _, uint64_t, "%016lx", 0,                           \
      (unsigned long)inf->label_registry_hash, inf->has_label_registry_hash,                        \
      "label registry hash; engine boot REFUSE on mismatch (label set drift)")                      \
    /* === feature_mask (standalone) — emitted at line 2292 === */                                  \
    X(feature_mask,                             _, uint64_t, "%016lx", 0,                           \
      (unsigned long)inf->feature_mask_train, inf->has_feature_mask,                                \
      "feature mask at training time; engine compares to runtime feature_mask")                     \
    /* === label_params group (3 fields) — emitted at line 2306 === */                              \
    X(label_lookahead_ticks,                    label_params, int, "%d", 0,                         \
      inf->label_lookahead_ticks, inf->has_label_params, "label lookahead window in ticks")         \
    X(label_tp_pct,                             label_params, double, "%.6g", 0.0,                  \
      inf->label_tp_pct, inf->has_label_params, "label take-profit percent")                        \
    X(label_sl_pct,                             label_params, double, "%.6g", 0.0,                  \
      inf->label_sl_pct, inf->has_label_params, "label stop-loss percent")                          \
    /* === xgb_train_nthread (standalone) — emitted at line 2323 === */                             \
    X(xgb_train_nthread,                        _, int, "%d", 0,                                    \
      inf->xgb_train_nthread, inf->has_xgb_train_nthread,                                           \
      "XGBoost training thread count; lets operator detect serial vs parallel mode forensically")

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

#define STAMP_MODEL_CONST_AUTOPOPULATE(inf, meta, now_us)                            \
    do {                                                                              \
        _Pragma("GCC diagnostic push")                                                \
        _Pragma("GCC diagnostic ignored \"-Wunused-value\"")                          \
        FOREACH_STAMP_BOUND_MODEL_CONST(STAMP_MODEL_CONST_AUTOPOPULATE_ONE)           \
        _Pragma("GCC diagnostic pop")                                                 \
    } while (0)

// Per-entry expansion. Numeric types use direct assignment; string
// types (char[N]) need separate strncpy-based handling. v5.14.8.A
// migration uses the numeric-only AUTOPOPULATE for simplicity; string
// fields (xgb_tree_method, scaler_sha256, etc.) populate manually
// alongside the AUTOPOPULATE call.
//
// Tuple signature (8 params): X(name, group, type, fmt, default_val,
// get_value, emit_when, doc). Group is documentation-only at autopopulate
// time (used by struct generation at v5.14.8.A; auto-populate just
// gates per-entry on the explicit emit_when).
#define STAMP_MODEL_CONST_AUTOPOPULATE_ONE(name, group, type, fmt, default_val, get_value, emit_when, doc) \
    if (emit_when) {                                                                  \
        (inf).has_##name = 1;                                                         \
        (inf).name       = (type)(get_value);                                         \
    }

//======================================================================================================
// [TEST INSTRUMENTATION]
//======================================================================================================
// Compile-time field count for tests. Counts entries via macro counting.
// Used by tests to assert "all N expected fields are present in the
// registry" — catches accidental row deletion during refactors.
//
// Extensibility loop test pattern (CLAUDE.md Check 26 discipline):
//   #define X(name, type, fmt, def, get, when, doc) \
//       do { /* compile-time existence check */ \
//           StampInferenceCfgInputs inf{}; \
//           (void)inf.has_##name; \
//           (void)inf.name; \
//       } while (0);
//   FOREACH_STAMP_BOUND_MODEL_CONST(X)
//   #undef X
//======================================================================================================

#define STAMP_MODEL_CONST_COUNT_ONE(name, group, type, fmt, default_val, get_value, emit_when, doc) +1
#define FOREACH_STAMP_BOUND_MODEL_CONST_COUNT  (0 FOREACH_STAMP_BOUND_MODEL_CONST(STAMP_MODEL_CONST_COUNT_ONE))

// Group counter: how many group has_* flags exist (for bit allocation).
#define STAMP_MODEL_CONST_GROUP_COUNT_ONE(group_name, doc) +1
#define FOREACH_STAMP_BOUND_MODEL_CONST_GROUP_COUNT \
    (0 FOREACH_STAMP_BOUND_MODEL_CONST_GROUPS(STAMP_MODEL_CONST_GROUP_COUNT_ONE))

#endif // STAMP_BOUND_MODEL_CONST_REGISTRY_HPP
