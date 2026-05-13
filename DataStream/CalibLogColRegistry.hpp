// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [CALIBRATION LOG COLUMN REGISTRY — v5.14.10.D]
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
//   Row-write expansion expects these variables in scope (matches OrderManager_HandleFill
//   calibration log body at OrderManager.hpp:991-1019 BEFORE this refactor):
//     uint64_t ts_us, int pslot, uint8_t pred_flag, double pred_p,
//     double entry_d_calib, double exit_d_calib, double gain_pct, double pnl_bps,
//     OrderManagerState<F>* oms
//
// HEADER + ROW EMIT:
//   - CalibLog_EmitHeader(f) — comma-separated col_name list + trailing \n
//   - CALIB_LOG_EMIT_ROW(f) — macro expanded inside caller; comma-separated value list + \n
//
// BYTE-FORMAT PRESERVATION:
//   Existing operator-side parsers (calibration analysis tooling) depend on
//   the EXACT 9-column shape of `timestamp_us,slot,exit_predicted_flag,predicted_p,
//   entry_price,exit_price,gain_pct,realized_pnl_bps,was_win`. The registry
//   tuple ORDER + col_name + fmt MUST match the pre-refactor output bytewise.
//   Snapshot tests in tests/controller_test.cpp v5.14.10.D verify this.
//
// FUTURE COLUMNS (1 row each):
//   - cfg=2 dual-mode telemetry: exp3_chosen_arm, thompson_chosen_arm, regime_id_at_pick
//     (DEFERRED — needs Order struct or OMS state extension to flow data
//     from predict-time to fill-time; tracked as TECH_DEBT-NNN)
//   - Maker fill metrics (deferred until v6.0 maker work)
//   - New ML observability surfaces (whatever future ships add)
//
// Pattern documented in DESIGN_SPECS/calibration-log-column-registry.md.
//======================================================================================================
#ifndef CALIB_LOG_COL_REGISTRY_HPP
#define CALIB_LOG_COL_REGISTRY_HPP

#include <cstdio>

//======================================================================================================
// [REGISTRY TUPLE]
//======================================================================================================
// Order MATTERS — operator parsers depend on column ordering.
// DO NOT reorder existing columns; APPEND new columns at the end.
#define FOREACH_CALIB_LOG_COL(X)                                                                          \
    X(timestamp_us,        "%llu",  (unsigned long long)ts_us)                                            \
    X(slot,                "%d",    (int)pslot)                                                           \
    X(exit_predicted_flag, "%u",    (unsigned)pred_flag)                                                  \
    X(predicted_p,         "%.6f",  pred_p)                                                               \
    X(entry_price,         "%.4f",  entry_d_calib)                                                        \
    X(exit_price,          "%.4f",  exit_d_calib)                                                         \
    X(gain_pct,            "%.6f",  gain_pct)                                                             \
    X(realized_pnl_bps,    "%.4f",  pnl_bps)                                                              \
    X(was_win,             "%d",    (BITMAP_IS_SET(oms->last_was_win_bitmap, BITMAP_BIT_U16(pslot)) ? 1 : 0))

//======================================================================================================
// [AUTO-GENERATED COUNT]
//======================================================================================================
#define X_GEN_CALIB_LOG_COUNT_ONE(name, fmt, expr) +1
#define FOREACH_CALIB_LOG_COL_COUNT (0 FOREACH_CALIB_LOG_COL(X_GEN_CALIB_LOG_COUNT_ONE))

//======================================================================================================
// [HEADER EMITTER — comma-separated col_name list + trailing \n]
//======================================================================================================
// Single function emits the canonical CSV header. Walks registry; first
// column has no leading comma; trailing \n at end. Byte-identical to the
// pre-v5.14.10.D hand-coded header literal at OrderManager.hpp:1293-1295.
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

//======================================================================================================
// [ROW EMITTER — macro expanded in caller scope]
//======================================================================================================
// Used as a STATEMENT inside HandleFill (or any other caller that has the
// expected variables in scope per CALLER SCOPE CONTRACT above). Wraps in
// a do-while-0 block so it can be used like a single statement; declares
// `_calib_first` local to avoid clashing with caller variables.
//
// Walks registry; emits each value with caller-supplied fmt; comma-separates;
// adds trailing \n. Byte-identical to the pre-v5.14.10.D hand-coded fprintf
// at OrderManager.hpp:1008-1013.
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
