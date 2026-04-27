// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [SHARDED TRADE LOG]
//
// Phase 08 of the per-core sharded engine. CSV writer that records one row per
// TradeEvent drained from a per-core event ring. Lives entirely on the
// controller core — single-writer by construction (pitfall P8.2).
//
// Format version 3 (sharded):
//   columns: timestamp_us,core_id,strategy_id,event_type,price,entry_price,
//            exit_price,pnl,fees,balance_after,trade_size
//   - timestamp_us is the originating tick's market timestamp, NOT wall clock
//   - core_id is the execution core that fired the event
//   - strategy_id is the strategy assigned to that core at fill time
//   - event_type is 'E' (entry) or 'X' (exit)
//   - rows are written in arrival order from the drain loop, NOT chronological;
//     consumers sort by timestamp_us if they need market-time ordering (P8.1)
//
// Filename convention: logging/SYMBOL_sharded_order_history.csv
//   - distinct from single_core mode's SYMBOL_order_history.csv (P8.6)
//   - both files coexist if both modes run, useful for validation during the
//     migration period
//
// Pitfalls covered:
//   P8.1 — arrival-order rows, sort by timestamp_us downstream
//   P8.2 — single writer (controller core), no atomics or locks needed
//   P8.3 — 1024-byte snprintf buffer + truncation guard with counter
//   P8.5 — file handle opened once at Init, fwrite buffered, fflush on demand
//   P8.6 — _sharded_ filename suffix
//   P8.8 — timestamp column documented as microseconds in the header
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"
#include "TradeEvent.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace tt {

//======================================================================================================
// [STRUCT]
//======================================================================================================
// File handle is owned for the entire session. trade_count and writes_truncated
// are observability counters surfaced via the TUI / tests; they don't affect
// behavior.
//======================================================================================================
struct ShardedTradeLog {
    FILE*    file;
    uint64_t row_count;          // total rows written this session
    uint64_t writes_truncated;   // snprintf truncations (should always be 0)
};

//======================================================================================================
// [INIT]
//======================================================================================================
// Open logging/SYMBOL_sharded_order_history.csv in append mode. Writes the v3
// header row only if the file is empty (so re-running the engine doesn't
// duplicate the header).
//
// Returns 1 on success, 0 if the file open fails. Caller should treat 0 as
// "logging disabled" and continue without crashing — the engine works fine
// without a trade log, you just don't get the CSV.
//======================================================================================================
inline int ShardedTradeLog_Init(ShardedTradeLog* log, const char* symbol) {
    log->file = nullptr;
    log->row_count = 0;
    log->writes_truncated = 0;

    // Build filename: logging/SYMBOL_order_history.csv
    // matches the path the GUI's TradeReader expects
    char filename[256];
    int n = snprintf(filename, sizeof(filename),
                     "logging/%s_order_history.csv", symbol);
    if (n < 0 || (size_t)n >= sizeof(filename)) return 0;

    // Detect existing content so we don't double-write the header. fopen("a")
    // creates the file if missing — that's intentional.
    int has_content = 0;
    FILE* probe = fopen(filename, "r");
    if (probe) {
        has_content = (fgetc(probe) != EOF);
        fclose(probe);
    }

    log->file = fopen(filename, "a");
    if (!log->file) return 0;

    // v4.7.3: line-buffered so every row auto-flushes to disk on its
    // trailing newline. Default is fully-buffered (8KB), which holds
    // hours of trade data in memory across a typical paper-soak cycle
    // and only flushes on clean shutdown — meaning a crash, kill, or
    // mid-session GUI read sees stale CSV. With trades being rare
    // events (≪ ticks), the per-row fflush syscall cost is negligible.
    setvbuf(log->file, nullptr, _IOLBF, 0);

    if (!has_content) {
        // Two-line header: a comment line documenting format and ordering, then
        // the column names. Tools that read this file should treat the leading
        // '#' line as a version sentinel.
        fprintf(log->file,
            "# v3 sharded engine — rows are in arrival order; sort by timestamp_us for chronological view\n");
        fprintf(log->file,
            "timestamp_us,core_id,strategy_id,event_type,price,entry_price,exit_price,pnl,fees,balance_after,trade_size\n");
        fflush(log->file);
    }
    return 1;
}

//======================================================================================================
// [FLUSH / CLOSE]
//======================================================================================================
// Flush is meant to be called from the slow path every K iterations so the CSV
// stays current without paying an fflush() syscall on every row. Close is
// called once at engine shutdown.
//======================================================================================================
inline void ShardedTradeLog_Flush(ShardedTradeLog* log) {
    if (log->file) fflush(log->file);
}

inline void ShardedTradeLog_Close(ShardedTradeLog* log) {
    if (log->file) {
        fflush(log->file);
        fclose(log->file);
        log->file = nullptr;
    }
}

//======================================================================================================
// [RECORD ENTRY]
//======================================================================================================
// Emit one 'E' row. Exit-only fields (exit_price, pnl, fees) are written as 0
// — consumers should ignore them on entry rows by checking event_type == 'E'.
//
// Caller is responsible for passing the actual fill price and the entry fee
// computed from notional × fee_rate. balance_after is the balance immediately
// after the entry fee was deducted (or the same as before if entry fees are
// deferred to settlement — both modes work, just be consistent).
//
// pitfall P8.3 — snprintf return value checked, truncation counter bumped.
//======================================================================================================
template <unsigned F>
inline void ShardedTradeLog_RecordEntry(ShardedTradeLog* log,
                                         const TradeEvent<F>& event,
                                         uint8_t strategy_id,
                                         FPN<F> entry_price,
                                         FPN<F> trade_size,
                                         FPN<F> entry_fee,
                                         FPN<F> balance_after) {
    if (!log->file) return;
    char row[1024];
    int n = snprintf(row, sizeof(row),
        "%lu,%u,%u,E,%.8f,%.8f,0,0,%.8f,%.8f,%.8f\n",
        (unsigned long)event.timestamp,
        (unsigned)event.core_id,
        (unsigned)strategy_id,
        FPN_ToDouble(event.price),
        FPN_ToDouble(entry_price),
        FPN_ToDouble(entry_fee),
        FPN_ToDouble(balance_after),
        FPN_ToDouble(trade_size));
    if (n < 0 || (size_t)n >= sizeof(row)) {
        log->writes_truncated++;
        return;
    }
    fwrite(row, 1, (size_t)n, log->file);
    log->row_count++;
}

//======================================================================================================
// [RECORD EXIT]
//======================================================================================================
// Emit one 'X' row. All columns populated. net_pnl is post-fee P&L (gross
// minus entry_fee minus exit_fee), total_fees is entry_fee + exit_fee.
//
// pitfall P8.7 — caller must compute net_pnl and total_fees BEFORE invoking
// this function. EventLoop_OnEvent already does the math in the right order.
//======================================================================================================
template <unsigned F>
inline void ShardedTradeLog_RecordExit(ShardedTradeLog* log,
                                        const TradeEvent<F>& event,
                                        uint8_t strategy_id,
                                        FPN<F> entry_price,
                                        FPN<F> exit_price,
                                        FPN<F> trade_size,
                                        FPN<F> net_pnl,
                                        FPN<F> total_fees,
                                        FPN<F> balance_after) {
    if (!log->file) return;
    char row[1024];
    int n = snprintf(row, sizeof(row),
        "%lu,%u,%u,X,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f\n",
        (unsigned long)event.timestamp,
        (unsigned)event.core_id,
        (unsigned)strategy_id,
        FPN_ToDouble(event.price),
        FPN_ToDouble(entry_price),
        FPN_ToDouble(exit_price),
        FPN_ToDouble(net_pnl),
        FPN_ToDouble(total_fees),
        FPN_ToDouble(balance_after),
        FPN_ToDouble(trade_size));
    if (n < 0 || (size_t)n >= sizeof(row)) {
        log->writes_truncated++;
        return;
    }
    fwrite(row, 1, (size_t)n, log->file);
    log->row_count++;
}

}  // namespace tt
