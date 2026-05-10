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

// Empty registry definition for v5.14.8.0 infrastructure ship.
// v5.14.8.A populates with 24 existing migrated fields.
// v5.14.8.D appends 5 new v5.14.8 fields.
//
// ANTI-PATTERN: Do NOT add architectural stamp body fields as flat
// struct field declarations directly in ModelStampResult /
// StampInferenceCfgInputs / ModelHandle. Use this registry. Each new
// architectural field = 1 registry line; struct + parser + emitter +
// populator all auto-generated.
//
// Adding a new architectural field — example shape:
//   X(my_new_field, uint64_t, "%llu", 0,
//       compute_my_value(meta), 1,
//       "doc string explaining the field")

#define FOREACH_STAMP_BOUND_MODEL_CONST(X)                                                          \
    /* v5.14.8.A entries (migrated from manual flat fields) appended here */                       \
    /* v5.14.8.D entries (new v5.14.8 architectural fields) appended after */                      \
    /* registry intentionally empty in v5.14.8.0 — infrastructure ship; */                         \
    /* see plans/2026-05-08-v5.14.8-stamp-lineage-stale-gating.md sub-tags .A + .D */

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
// types (char[N]) auto-detected at compile time via type traits is
// complex in pure macro — for simplicity, the per-entry macro handles
// numeric only; string fields use a separate STAMP_MODEL_CONST_AUTOPOPULATE_STR_ONE
// that's specialized at registry definition time (caller can pick which
// AUTOPOPULATE_ONE variant to use; v5.14.8.A migration codifies the choice).
#define STAMP_MODEL_CONST_AUTOPOPULATE_ONE(name, type, fmt, default_val, get_value, emit_when, doc) \
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

#define STAMP_MODEL_CONST_COUNT_ONE(name, type, fmt, default_val, get_value, emit_when, doc) +1
#define FOREACH_STAMP_BOUND_MODEL_CONST_COUNT  (0 FOREACH_STAMP_BOUND_MODEL_CONST(STAMP_MODEL_CONST_COUNT_ONE))

#endif // STAMP_BOUND_MODEL_CONST_REGISTRY_HPP
