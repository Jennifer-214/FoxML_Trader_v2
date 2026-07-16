// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/TradeLogColRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[trade-log CSV column registry — one row = header + entry/exit row writers; append-only column order]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_TRADE_LOG_COL]
//======================================================================================================
#ifndef TRADE_LOG_COL_REGISTRY_HPP
#define TRADE_LOG_COL_REGISTRY_HPP

#include <cstdio>

namespace tt {

//======================================================================
// [REGISTRY]_[FOREACH_TRADE_LOG_COL]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [FRAMEWORK_DISCIPLINE]]
// [REFERENCE]_[DESIGN_SPEC]_[calibration-log-column-registry]
// [REFERENCE]_[INVARIANT]_[H21]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[ShardedTradeLog CSV columns — position-read by operator parsers; DO NOT reorder, APPEND only]
// [COLUMN]_[col_name]_[bare identifier; used for CSV header AND row writers]
// [COLUMN]_[printf_fmt]_[per-column printf format string (e.g. "%lu", "%.8f", "%c")]
// [COLUMN]_[value_expr]_[expression read at row-build time; MUST be valid in caller scope]
//======================================================================
// [CODE]
//======================================================================
// ORDER MATTERS — operator parsers (TradeReader / TUI history panel /
// offline analysis tools) read columns by position.
// DO NOT reorder existing columns; APPEND new columns at the end.
#define FOREACH_TRADE_LOG_COL(X)                                                              \
    X(timestamp_us,    "%lu",  (unsigned long)timestamp_us)                                   \
    X(node_id,         "%u",   (unsigned)node_id)                                             \
    X(strategy_id,     "%u",   (unsigned)strategy_id)                                         \
    X(event_type,      "%c",   event_type)                                                    \
    X(price,           "%.8f", price_v)                                                       \
    X(entry_price,     "%.8f", entry_price_v)                                                 \
    X(exit_price,      "%.8f", exit_price_v)                                                  \
    X(pnl,             "%.8f", pnl_v)                                                         \
    X(fees,            "%.8f", fees_v)                                                        \
    X(balance_after,   "%.8f", balance_after_v)                                               \
    X(trade_size,      "%.8f", trade_size_v)                                                  \
    /* v5.15.5.C.3 Phase 5.A — regime capture at trade emit time. -1 = unknown   */           \
    /* (non-strategy-aware caller, e.g. OMS HandleFill); valid regimes are 0..N-1 */          \
    /* per FOREACH_REGIME (Strategies/StrategyInterface.hpp:181). regime_name is   */          \
    /* the full_name string from REGIME_INFO[].full_name lookup or "UNKNOWN" for  */          \
    /* regime_v < 0. Append-only addition; existing 11-col operator parsers see   */          \
    /* the extra columns as trailing CSV; no breakage.                             */          \
    X(regime,          "%d",   regime_v)                                                       \
    X(regime_name,     "%s",   regime_name_v)

//------------------------------------------------------------------------------
// [SECTION]_[auto-generated count]
//------------------------------------------------------------------------------
#define X_GEN_TRADE_LOG_COUNT_ONE(name, fmt, expr) +1
#define FOREACH_TRADE_LOG_COL_COUNT (0 FOREACH_TRADE_LOG_COL(X_GEN_TRADE_LOG_COUNT_ONE))

//------------------------------------------------------------------------------
// [SECTION]_[header emitter]
//------------------------------------------------------------------------------
// Single function emits the canonical CSV header (column count = the
// registry's row count; was 11 pre-Phase-5.A). Walks registry;
// first column has no leading comma; trailing \n at end. Byte-identical to
// the pre-v5.14.10.F hand-coded header literal at ShardedTradeLog.hpp:118-119
// for the original 11 columns.
inline void TradeLog_EmitHeader(FILE* f) {
    if (!f) return;
    int first = 1;
    #define X_GEN_TRADE_LOG_HEADER(name, fmt, expr)                              \
        do {                                                                       \
            std::fprintf(f, first ? "%s" : ",%s", #name);                          \
            first = 0;                                                             \
        } while (0);
    FOREACH_TRADE_LOG_COL(X_GEN_TRADE_LOG_HEADER)
    #undef X_GEN_TRADE_LOG_HEADER
    std::fprintf(f, "\n");
}

//------------------------------------------------------------------------------
// [SECTION]_[row emitter — snprintf to caller-supplied buffer]
//------------------------------------------------------------------------------
// Used inside RecordEntry / RecordExit. Writes the row to `buf` (size bufsz);
// returns total bytes written via *out_n_ptr. Caller checks truncation
// (n < 0 || n >= bufsz) and bumps writes_truncated on failure (P8.3 pattern).
//
// Macro expansion happens in CALLER scope; reads value_expr column from
// caller-scope variables per CALLER SCOPE CONTRACT above.
//
// Why snprintf-to-buffer (not fprintf-direct like calib log): preserves
// the pre-refactor pattern of building the row in a stack buffer for atomic
// single-write via fwrite. P8.3 truncation guard relies on snprintf's
// length-bounded behavior.
#define TRADE_LOG_EMIT_ROW_TO_BUFFER(buf, bufsz, out_n_ptr)                                          \
    do {                                                                                              \
        char* _trade_buf = (buf);                                                                     \
        size_t _trade_bufsz = (bufsz);                                                                \
        int* _trade_out_n = (out_n_ptr);                                                              \
        size_t _trade_offset = 0;                                                                     \
        int _trade_first = 1;                                                                         \
        FOREACH_TRADE_LOG_COL(X_GEN_TRADE_LOG_ROW_BUF)                                                \
        if (_trade_offset < _trade_bufsz) {                                                            \
            _trade_buf[_trade_offset++] = '\n';                                                       \
            if (_trade_offset < _trade_bufsz) _trade_buf[_trade_offset] = '\0';                       \
        }                                                                                              \
        *_trade_out_n = (int)_trade_offset;                                                           \
    } while (0)

#define X_GEN_TRADE_LOG_ROW_BUF(name, fmt, expr)                                              \
    do {                                                                                       \
        if (_trade_offset < _trade_bufsz) {                                                    \
            int _trade_n = std::snprintf(_trade_buf + _trade_offset,                           \
                                          _trade_bufsz - _trade_offset,                        \
                                          _trade_first ? fmt : "," fmt,                        \
                                          (expr));                                             \
            if (_trade_n < 0 || (size_t)_trade_n >= _trade_bufsz - _trade_offset) {            \
                /* truncation; mark via offset = bufsz so caller's check fires */              \
                _trade_offset = _trade_bufsz;                                                  \
            } else {                                                                            \
                _trade_offset += (size_t)_trade_n;                                             \
            }                                                                                   \
            _trade_first = 0;                                                                  \
        }                                                                                       \
    } while (0);
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]_[origin + caller contract + byte-format preservation]
//----------------------------------------------------------------------
// FOREACH_TRADE_LOG_COL(X) registry — adds a new ShardedTradeLog CSV column
// in 1 row. Second application of the calibration-log-column-registry.md
// pattern (closes /merge-scan N2 finding for ShardedTradeLog; MetricsLog
// deferred — see TECH_DEBT-031).
//
// CALLER SCOPE CONTRACT (both RecordEntry + RecordExit):
//   uint64_t timestamp_us, uint32_t node_id, uint32_t strategy_id,
//   char event_type, double price_v, double entry_price_v, double exit_price_v,
//   double pnl_v, double fees_v, double balance_after_v, double trade_size_v,
//   int regime_v (v5.15.5.C.3 Phase 5.A; -1 = unknown for non-strategy-aware callers),
//   const char* regime_name_v (v5.15.5.C.3 Phase 5.A; "UNKNOWN" for regime_v < 0)
//
// HEADER + ROW EMIT:
//   - TradeLog_EmitHeader(f) — comma-separated col_name list + trailing \n
//   - TRADE_LOG_EMIT_ROW(buf, bufsz, *out_n) — macro expanded inside caller;
//     uses snprintf to build the row in a stack buffer (preserves the
//     pre-refactor P8.3 truncation-guard pattern of fwrite-from-buffer).
//     Returns row length via *out_n; caller validates against bufsz +
//     truncation counter.
//
// BYTE-FORMAT PRESERVATION (historical — the pre-v5.14.10.F 11-column era):
//   Pre-refactor RecordEntry / RecordExit fprintf format strings:
//     RecordEntry: "%lu,%u,%u,E,%.8f,%.8f,0,0,%.8f,%.8f,%.8f\n"
//     RecordExit:  "%lu,%u,%u,X,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f\n"
//   Both produce 11 columns; differ in event_type letter + zero-vs-populated
//   exit_price/pnl. Registry merges both writers into ONE row shape with
//   per-writer value population (event_type='E'/'X'; exit_price/pnl/fees
//   set per writer via caller-scope variables BEFORE TRADE_LOG_EMIT_ROW).
//
// FUTURE COLUMNS (1 row each):
//   - Maker/taker fee bifurcation (separate maker_fee + taker_fee columns)
//   - Slippage attribution (notional vs effective_price columns)
//   - Bandit attribution (which arm + which regime drove this trade — sister
//     to v5.14.10.D cfg=2 calib log telemetry; same per-slot OMS state need)
//
// Pattern documented in DESIGN_SPECS/calibration-log-column-registry.md.
//======================================================================
// [DERIVED]   (tool-refreshed — ROW_COUNT/CONSUMERS generators land with the drift-gate generalization; empty skeleton is correct, D-327)
//======================================================================
// [END_REGISTRY]_[FOREACH_TRADE_LOG_COL]
//======================================================================

}  // namespace tt

#endif // TRADE_LOG_COL_REGISTRY_HPP
