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
//   value_expr  — expression read at row-write time; MUST be valid in the
//                 caller scope (HandleFill body for the entry-fill writer)
//
// CALLER SCOPE CONTRACT:
//   Row-write expansion expects these variables in scope (matches real_on_exit_calibration
//   body at OrderManager.hpp post-v5.15.5.F.4d Step 7 § F):
//     uint64_t ts_us, int pslot, uint8_t pred_flag, double pred_p,
//     double entry_d_calib, double exit_d_calib, double gain_pct, double pnl_bps,
//     OrderManagerState<F>* oms,
//     // v5.15.5.F.4d Step 7 § F + Step 8 § M:
//     int bandit_active_state, int bandit_regime, int bandit_chosen_arm,
//     double reward_bps_attributed, int thompson_telemetry_arm,
//     double thompson_exp3_blend_alpha, int regime_clamped,
//     double exp3_probs[BANDIT_MAX_ARMS],
//     EnsembleModelZoo<F>* ezoo (nullable; null-coalesce per-arm Thompson telemetry to 0.0/0u)
//
// HEADER + ROW EMIT:
//   - CalibLog_EmitHeader(f) — comma-separated col_name list + trailing \n
//   - CALIB_LOG_EMIT_ROW(f) — macro expanded inside caller; comma-separated value list + \n
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
#include "../ML_Headers/BanditLearning.hpp"  // v5.15.5.F.4d Step 8 § M — BANDIT_MAX_ARMS for hand-written-8-arm static_assert; registry inherently references per-arm bandit state

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
    X(timestamp_us,        "%llu",  (unsigned long long)ts_us)                                                                                         \
    X(slot,                "%d",    (int)pslot)                                                                                                        \
    X(exit_predicted_flag, "%u",    (unsigned)pred_flag)                                                                                               \
    X(predicted_p,         "%.6f",  pred_p)                                                                                                            \
    X(entry_price,         "%.4f",  entry_d_calib)                                                                                                     \
    X(exit_price,          "%.4f",  exit_d_calib)                                                                                                      \
    X(gain_pct,            "%.6f",  gain_pct)                                                                                                          \
    X(realized_pnl_bps,    "%.4f",  pnl_bps)                                                                                                           \
    X(was_win,             "%d",    (BITMAP_IS_SET(oms->last_was_win_bitmap, BITMAP_BIT_U16(pslot)) ? 1 : 0))                                          \
    /* v5.15.5.F.4d Step 8 § M — 6 bandit-context singletons (decoded from Order::flags_packed bits 17-25 + per-core cfg + per-slot reward) */         \
    X(bandit_algorithm,           "%d",   bandit_active_state)                                                                                         \
    X(regime_id_at_emit,          "%d",   bandit_regime)                                                                                               \
    X(chosen_arm,                 "%d",   bandit_chosen_arm)                                                                                           \
    X(reward_bps_attributed,      "%.6f", reward_bps_attributed)                                                                                       \
    X(thompson_telemetry_arm,     "%d",   thompson_telemetry_arm)                                                                                      \
    X(thompson_exp3_blend_alpha,  "%.6f", thompson_exp3_blend_alpha)                                                                                   \
    /* v5.15.5.F.4d Step 8 § M — 32 per-arm cols (8 arms × {exp3_w, thompson_mu, thompson_prec, thompson_pulls}); arm-major layout */                  \
    X(exp3_w_arm0,         "%.6f", exp3_probs[0])                                                                                                      \
    X(thompson_mu_arm0,    "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].mu_post[0]                  : 0.0))                                  \
    X(thompson_prec_arm0,  "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].precision_post[0]           : 0.0))                                  \
    X(thompson_pulls_arm0, "%u",   (ezoo ? (unsigned)ezoo->buy_thompson_bandits[regime_clamped].total_pulls[0]    : 0u))                                   \
    X(exp3_w_arm1,         "%.6f", exp3_probs[1])                                                                                                      \
    X(thompson_mu_arm1,    "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].mu_post[1]                  : 0.0))                                  \
    X(thompson_prec_arm1,  "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].precision_post[1]           : 0.0))                                  \
    X(thompson_pulls_arm1, "%u",   (ezoo ? (unsigned)ezoo->buy_thompson_bandits[regime_clamped].total_pulls[1]    : 0u))                                   \
    X(exp3_w_arm2,         "%.6f", exp3_probs[2])                                                                                                      \
    X(thompson_mu_arm2,    "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].mu_post[2]                  : 0.0))                                  \
    X(thompson_prec_arm2,  "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].precision_post[2]           : 0.0))                                  \
    X(thompson_pulls_arm2, "%u",   (ezoo ? (unsigned)ezoo->buy_thompson_bandits[regime_clamped].total_pulls[2]    : 0u))                                   \
    X(exp3_w_arm3,         "%.6f", exp3_probs[3])                                                                                                      \
    X(thompson_mu_arm3,    "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].mu_post[3]                  : 0.0))                                  \
    X(thompson_prec_arm3,  "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].precision_post[3]           : 0.0))                                  \
    X(thompson_pulls_arm3, "%u",   (ezoo ? (unsigned)ezoo->buy_thompson_bandits[regime_clamped].total_pulls[3]    : 0u))                                   \
    X(exp3_w_arm4,         "%.6f", exp3_probs[4])                                                                                                      \
    X(thompson_mu_arm4,    "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].mu_post[4]                  : 0.0))                                  \
    X(thompson_prec_arm4,  "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].precision_post[4]           : 0.0))                                  \
    X(thompson_pulls_arm4, "%u",   (ezoo ? (unsigned)ezoo->buy_thompson_bandits[regime_clamped].total_pulls[4]    : 0u))                                   \
    X(exp3_w_arm5,         "%.6f", exp3_probs[5])                                                                                                      \
    X(thompson_mu_arm5,    "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].mu_post[5]                  : 0.0))                                  \
    X(thompson_prec_arm5,  "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].precision_post[5]           : 0.0))                                  \
    X(thompson_pulls_arm5, "%u",   (ezoo ? (unsigned)ezoo->buy_thompson_bandits[regime_clamped].total_pulls[5]    : 0u))                                   \
    X(exp3_w_arm6,         "%.6f", exp3_probs[6])                                                                                                      \
    X(thompson_mu_arm6,    "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].mu_post[6]                  : 0.0))                                  \
    X(thompson_prec_arm6,  "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].precision_post[6]           : 0.0))                                  \
    X(thompson_pulls_arm6, "%u",   (ezoo ? (unsigned)ezoo->buy_thompson_bandits[regime_clamped].total_pulls[6]    : 0u))                                   \
    X(exp3_w_arm7,         "%.6f", exp3_probs[7])                                                                                                      \
    X(thompson_mu_arm7,    "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].mu_post[7]                  : 0.0))                                  \
    X(thompson_prec_arm7,  "%.6f", (ezoo ? ezoo->buy_thompson_bandits[regime_clamped].precision_post[7]           : 0.0))                                  \
    X(thompson_pulls_arm7, "%u",   (ezoo ? (unsigned)ezoo->buy_thompson_bandits[regime_clamped].total_pulls[7]    : 0u))

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
#define CALIB_LOG_EMIT_ROW(file_handle)                                                            \
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
