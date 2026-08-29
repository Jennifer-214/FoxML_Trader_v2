// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[DataStream/CalibLogColRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [PERSISTENCE] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the calibration-CSV column SSoT — 47 rows (legacy 9 prefix byte-locked + 6 bandit singletons + 32 per-arm telemetry); header + row emit both walk the ONE list]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_CALIB_LOG_COL]   (8-arm coverage assert + count ride)
//   - [FUNCTION]_[CalibLog_EmitHeader]
//   - [MACRO]_[CALIB_LOG_EMIT_ROW]
// [REFERENCE]_[DESIGN_SPEC]_[calibration-log-column-registry]
//======================================================================================================
// FOREACH_CALIB_LOG_COL(X) registry — adds a new calibration log CSV column
// in 1 row. Closes TECH_DEBT-010 (recurring sister-literal pattern: header
// constant + body fprintf + row formatter all updated in lockstep when
// adding a column).
//
// Tuple: X(col_name, printf_fmt, value_expr)
//   col_name    — bare identifier; used for CSV header AND macro-generated names
//   printf_fmt  — per-column printf format string (e.g. "%llu", "%.4f")
//   value_expr  — expression read at row-write time, against the RECORD (see below)
//
// RECORD CONTRACT (replaced the old CALLER SCOPE CONTRACT at P4-pre-2 / amendment L):
//   Every value_expr reads `r.<field>` of a `CalibRecord` (declared below). Adding a column
//   therefore means: add the field to CalibRecord + populate it in the LEAF builder
//   (`real_on_exit_calibration`, OrderManager.hpp) + add the row here. A column whose field
//   does not exist fails to COMPILE — the previous contract could only fail at the call site
//   of whichever caller happened to lack the local.
//
//   WHY it changed: the row emit RELOCATED off the leaf's shared `FILE*` onto the composer's
//   calib funnel (P4-pre-2), because at the Phase-4 flip N node threads would otherwise
//   fprintf one FILE* — a stdio-lock serialization on the slow path (latency-path Rule 2) on
//   top of a multi-writer file. The leaf now BUILDS the record (every read node-local) and
//   the composer RENDERS it, so there is exactly ONE macro caller. Byte output is unchanged:
//   same registry order, same fmt strings, same values.
//
// HEADER + ROW EMIT:
//   - CalibLog_EmitHeader(f)     — comma-separated col_name list + trailing \n
//   - CALIB_LOG_EMIT_ROW(f, r)   — comma-separated value list + \n, rendered from the record
//
// BYTE-FORMAT PRESERVATION:
//   Existing operator-side parsers (calibration analysis tooling) depend on
//   the legacy 9-column PREFIX `timestamp_us,slot,exit_predicted_flag,predicted_p,
//   entry_price,exit_price,gain_pct,realized_pnl_bps,was_win` (columns are
//   APPEND-ONLY — the Step 8 § M bandit columns append after it). The legacy
//   tuple ORDER + col_name + fmt MUST match the pre-v5.14.10.D output bytewise.
//   Snapshot tests in tests/controller_test.cpp v5.14.10.D verify this.
//
// FUTURE COLUMNS (1 row each):
//   - cfg=2 dual-mode telemetry: LANDED at v5.15.5.F.4d Step 8 § M — the
//     chosen_arm / regime_id_at_emit / thompson_telemetry_arm singletons flow
//     from predict-time to fill-time via Order::flags_packed bits 17-25
//     (the "needs Order struct extension" deferral resolved by landing)
//   - Maker fill metrics (deferred until v6.0 maker work)
//   - New ML observability surfaces (whatever future ships add)
//
// Pattern documented in DESIGN_SPECS/calibration-log-column-registry.md.
//======================================================================================================
#ifndef CALIB_LOG_COL_REGISTRY_HPP
#define CALIB_LOG_COL_REGISTRY_HPP

#include <cstdio>
#include <cstdint>
#include "../ML_Headers/BanditLearning.hpp"  // v5.15.5.F.4d Step 8 § M — BANDIT_MAX_ARMS for hand-written-8-arm static_assert; registry inherently references per-arm bandit state

//======================================================================
// [STRUCT]_[CalibRecord]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [PERSISTENCE]]
// [THREAD]_[[NODE_WRITER] [COMPOSER_READER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[P4-pre-2 (amendment L): the calibration row's VALUE payload — built leaf-side at slot-flat (every read is node-local), transported over the per-node calib funnel, rendered composer-side. Replaces the registry's old CALLER SCOPE CONTRACT with a typed record contract: FOREACH_CALIB_LOG_COL value_exprs now read r.<field>, so there is exactly ONE macro caller (the composer's render) instead of an implicit contract on whatever locals a caller happened to have]
// [REFERENCE]_[DECISION]_[[D-445] [D-449]]
//======================================================================
// [CODE]
//======================================================================
// double (not Money) BY DESIGN: this is the DISPLAY payload — the row's values are already
// past their `Money_ToDouble` conversion at build time (H4 display-only arm). No money
// DECISION is made from these; they are CSV bytes. Do not route accounting through them.
struct CalibRecord {
    uint64_t ts_us            = 0;
    double   pred_p           = 0.0;
    double   entry_price      = 0.0;
    double   exit_price       = 0.0;
    double   gain_pct         = 0.0;
    double   pnl_bps          = 0.0;
    double   reward_bps       = 0.0;
    double   thompson_blend_alpha = 0.0;
    double   exp3_w[BANDIT_MAX_ARMS]        = {};
    double   thompson_mu[BANDIT_MAX_ARMS]   = {};
    double   thompson_prec[BANDIT_MAX_ARMS] = {};
    uint32_t thompson_pulls[BANDIT_MAX_ARMS] = {};
    int32_t  pslot            = 0;
    uint32_t pred_flag        = 0;
    int32_t  was_win          = 0;
    int32_t  bandit_algorithm = 0;
    int32_t  regime_id        = 0;
    int32_t  chosen_arm       = 0;
    int32_t  thompson_tel_arm = 0;
    int32_t  _pad0            = 0;   // H12: explicit, zero-init
};

// Ring-element size pin (the FillEvent/EmitRecord precedent — growth is DELIBERATE: edit this
// assert in the same commit as the field). Field ORDER is deliberate too: u64 -> 7 doubles ->
// the three 8-wide double arrays -> the u32 array -> the small ints, which lands ZERO padding
// at exactly 320B (5 cache lines, no array straddling a line).
//
// NOT bit-packed, deliberately (operator Q, 2026-08-29): the seven small ints WOULD fit one
// u32 (~24B/record, ~3KB across 16 nodes), but every one of them is consumed by a %d/%u CSV
// column — packing would force an MBS_* unpack at render and cost the registry its
// one-row-per-column readability, to optimize a term (footprint) that never compounds here:
// the record is touched twice per COMPLETED TRADE and its consumer is 47 fprintf calls. The
// bit-packing gradient targets hot-path / L1-resident / slot-bitmap state; this is neither.
// H14 is satisfied by construction (nothing packed ⇒ no bitfield syntax).
static_assert(sizeof(CalibRecord) == 320,
              "CalibRecord is a per-node ring element — re-pin deliberately when a column's field lands");
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-29]
// [SIZE]_[320B]
// [ALIGN]_[8]
// [CACHE_LINES]_[5]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[CalibRecord]
//======================================================================

//======================================================================
// [REGISTRY]_[FOREACH_CALIB_LOG_COL]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[47 append-only CSV columns (legacy 9 byte-locked prefix + 6 bandit singletons + 32 per-arm ARM-MAJOR) — order IS the wire format for operator parsers]
// [COLUMN]_[col_name]_[bare identifier; CSV header AND macro-generated names]
// [COLUMN]_[printf_fmt]_[per-column printf format string]
// [COLUMN]_[value_expr]_[expression read at row-write time; MUST be valid in the caller scope (see CALLER SCOPE CONTRACT)]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_CALIB_LOG_COL(X)                                                                                                                       \
    /* legacy 9 cols — UNCHANGED (operator parsers depend on byte order) */                                                                            \
    X(timestamp_us,        "%llu",  (unsigned long long)r.ts_us)                                                                                         \
    X(slot,                "%d",    (int)r.pslot)                                                                                                        \
    X(exit_predicted_flag, "%u",    (unsigned)r.pred_flag)                                                                                               \
    X(predicted_p,         "%.6f",  r.pred_p)                                                                                                            \
    X(entry_price,         "%.4f",  r.entry_price)                                                                                                     \
    X(exit_price,          "%.4f",  r.exit_price)                                                                                                      \
    X(gain_pct,            "%.6f",  r.gain_pct)                                                                                                          \
    X(realized_pnl_bps,    "%.4f",  r.pnl_bps)                                                                                                           \
    X(was_win,             "%d",    r.was_win)                                          \
    /* v5.15.5.F.4d Step 8 § M — 6 bandit-context singletons (decoded from Order::flags_packed bits 17-25 + per-core cfg + per-slot reward) */         \
    X(bandit_algorithm,           "%d",   r.bandit_algorithm)                                                                                         \
    X(regime_id_at_emit,          "%d",   r.regime_id)                                                                                               \
    X(chosen_arm,                 "%d",   r.chosen_arm)                                                                                           \
    X(reward_bps_attributed,      "%.6f", r.reward_bps)                                                                                       \
    X(thompson_telemetry_arm,     "%d",   r.thompson_tel_arm)                                                                                      \
    X(thompson_exp3_blend_alpha,  "%.6f", r.thompson_blend_alpha)                                                                                   \
    /* v5.15.5.F.4d Step 8 § M — 32 per-arm cols (8 arms × {exp3_w, thompson_mu, thompson_prec, thompson_pulls}); arm-major layout */                  \
    X(exp3_w_arm0,         "%.6f", r.exp3_w[0])                                                                                                      \
    X(thompson_mu_arm0,    "%.6f", r.thompson_mu[0])                                  \
    X(thompson_prec_arm0,  "%.6f", r.thompson_prec[0])                                  \
    X(thompson_pulls_arm0, "%u",   r.thompson_pulls[0])                                   \
    X(exp3_w_arm1,         "%.6f", r.exp3_w[1])                                                                                                      \
    X(thompson_mu_arm1,    "%.6f", r.thompson_mu[1])                                  \
    X(thompson_prec_arm1,  "%.6f", r.thompson_prec[1])                                  \
    X(thompson_pulls_arm1, "%u",   r.thompson_pulls[1])                                   \
    X(exp3_w_arm2,         "%.6f", r.exp3_w[2])                                                                                                      \
    X(thompson_mu_arm2,    "%.6f", r.thompson_mu[2])                                  \
    X(thompson_prec_arm2,  "%.6f", r.thompson_prec[2])                                  \
    X(thompson_pulls_arm2, "%u",   r.thompson_pulls[2])                                   \
    X(exp3_w_arm3,         "%.6f", r.exp3_w[3])                                                                                                      \
    X(thompson_mu_arm3,    "%.6f", r.thompson_mu[3])                                  \
    X(thompson_prec_arm3,  "%.6f", r.thompson_prec[3])                                  \
    X(thompson_pulls_arm3, "%u",   r.thompson_pulls[3])                                   \
    X(exp3_w_arm4,         "%.6f", r.exp3_w[4])                                                                                                      \
    X(thompson_mu_arm4,    "%.6f", r.thompson_mu[4])                                  \
    X(thompson_prec_arm4,  "%.6f", r.thompson_prec[4])                                  \
    X(thompson_pulls_arm4, "%u",   r.thompson_pulls[4])                                   \
    X(exp3_w_arm5,         "%.6f", r.exp3_w[5])                                                                                                      \
    X(thompson_mu_arm5,    "%.6f", r.thompson_mu[5])                                  \
    X(thompson_prec_arm5,  "%.6f", r.thompson_prec[5])                                  \
    X(thompson_pulls_arm5, "%u",   r.thompson_pulls[5])                                   \
    X(exp3_w_arm6,         "%.6f", r.exp3_w[6])                                                                                                      \
    X(thompson_mu_arm6,    "%.6f", r.thompson_mu[6])                                  \
    X(thompson_prec_arm6,  "%.6f", r.thompson_prec[6])                                  \
    X(thompson_pulls_arm6, "%u",   r.thompson_pulls[6])                                   \
    X(exp3_w_arm7,         "%.6f", r.exp3_w[7])                                                                                                      \
    X(thompson_mu_arm7,    "%.6f", r.thompson_mu[7])                                  \
    X(thompson_prec_arm7,  "%.6f", r.thompson_prec[7])                                  \
    X(thompson_pulls_arm7, "%u",   r.thompson_pulls[7])

// v5.15.5.F.4d Step 8 § M — hand-written 8-arm coverage invariant.
// If BANDIT_MAX_ARMS grows, append 4 more rows (exp3_w_armN + thompson_mu_armN + thompson_prec_armN
// + thompson_pulls_armN) to FOREACH_CALIB_LOG_COL above + bump this assert.
// [ASSERT]_[REGISTRY_COVERAGE]_[BANDIT_MAX_ARMS == 8 — hand-written per-arm rows track it]
static_assert(BANDIT_MAX_ARMS == 8,
              "FOREACH_CALIB_LOG_COL hand-written for 8 arms; bump arm rows + BANDIT_MAX_ARMS together");

//------------------------------------------------------------------
// [SECTION]_[AUTO-GENERATED COUNT]
//------------------------------------------------------------------
#define X_GEN_CALIB_LOG_COUNT_ONE(name, fmt, expr) +1
#define FOREACH_CALIB_LOG_COL_COUNT (0 FOREACH_CALIB_LOG_COL(X_GEN_CALIB_LOG_COUNT_ONE))
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Order MATTERS — operator parsers depend on column ordering.
// DO NOT reorder existing columns; APPEND new columns at the end.
//
// v5.15.5.F.4d Step 8 § M ACTIVE — 6 singleton + 32 per-arm bandit-telemetry cols appended after
// legacy 9. Decoded from Order::flags_packed bits 17-25 via MBS_* accessors (singletons) + per-slot
// FOREACH_OMS_PER_SLOT_FIELD bandit_reward_bps[pslot] + per-arm Exp3 probabilities (via
// Bandit_GetProbabilities into local exp3_probs[BANDIT_MAX_ARMS]) + per-arm Thompson posterior
// state (ezoo->buy_thompson_bandits[regime_clamped].{mu_post,precision_post,total_pulls}[arm]). All
// ezoo-touching cols null-coalesce to 0/0.0 when ezoo_ref is nullptr (test fixtures + non-ML cores
// with calibration_log_path set). Per-arm hand-written (8 arms × 4 families = 32 rows; sidecar M.2
// chose hand-write over preprocessor token-paste indirection for robustness + auditability).
// Per-arm layout is ARM-MAJOR (exp3_w_arm0, thompson_mu_arm0, thompson_prec_arm0, thompson_pulls_arm0,
// then arm1, ..., arm7) so per-arm clusters stay grouped for CSV scrubbing.
// Adding a 9th arm later: append 4 more rows + bump BANDIT_MAX_ARMS at BanditLearning.hpp.
//======================================================================
// [END_REGISTRY]_[FOREACH_CALIB_LOG_COL]
//======================================================================

//======================================================================
// [FUNCTION]_[CalibLog_EmitHeader]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[canonical CSV header from the registry walk — first column uncomma'd, trailing newline; byte-identical to the pre-v5.14.10.D hand-coded literal]
//======================================================================
// [CODE]
//======================================================================
inline void CalibLog_EmitHeader(FILE* f) {
    if (!f) return;
    int first = 1;
    #define X_GEN_CALIB_LOG_HEADER(name, fmt, expr)                              \
        do {                                                                       \
            std::fprintf(f, first ? "%s" : ",%s", #name);                          \
            first = 0;                                                             \
        } while (0);
    FOREACH_CALIB_LOG_COL(X_GEN_CALIB_LOG_HEADER)
    #undef X_GEN_CALIB_LOG_HEADER
    std::fprintf(f, "\n");
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[CalibLog_EmitHeader]
//======================================================================

//----------------------------------------------------------------------
// [MACRO]_[CALIB_LOG_EMIT_ROW]
// [TAG]_[[ENGINE] [ML_INFERENCE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the row emitter statement (+ its X_GEN_CALIB_LOG_ROW walker) — expands IN CALLER SCOPE per the CALLER SCOPE CONTRACT; byte-identical to the pre-v5.14.10.D hand-coded fprintf]
//----------------------------------------------------------------------
// Used as a STATEMENT inside HandleFill (or any other caller that has the
// expected variables in scope per CALLER SCOPE CONTRACT above). Wraps in
// a do-while-0 block so it can be used like a single statement; declares
// `_calib_first` local to avoid clashing with caller variables.
//
// Walks registry; emits each value with caller-supplied fmt; comma-separates;
// adds trailing \n.
#define CALIB_LOG_EMIT_ROW(file_handle, r)                                                         \
    do {                                                                                            \
        int _calib_first = 1;                                                                       \
        FILE* _calib_f = (file_handle);                                                             \
        if (_calib_f) {                                                                              \
            FOREACH_CALIB_LOG_COL(X_GEN_CALIB_LOG_ROW)                                              \
            std::fprintf(_calib_f, "\n");                                                           \
        }                                                                                            \
    } while (0)

#define X_GEN_CALIB_LOG_ROW(name, fmt, expr)                                       \
    do {                                                                            \
        std::fprintf(_calib_f, _calib_first ? fmt : "," fmt, (expr));               \
        _calib_first = 0;                                                            \
    } while (0);

#endif // CALIB_LOG_COL_REGISTRY_HPP
