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
#include "../Limits.hpp"             // v5.15.5.C.3 Phase 5.B — MAX_EXECUTION_CORES for per_core_files[]
#include "TradeEvent.hpp"
#include "TradeLogColRegistry.hpp"  // v5.14.10.F — FOREACH_TRADE_LOG_COL registry (closes /merge-scan N2 for trade log)
#include "../Strategies/StrategyInterface.hpp"  // v5.15.5.C.3 Phase 5.A — REGIME_INFO[] lookup for regime_name CSV column

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace tt {

//======================================================================================================
// [STRUCT]
//======================================================================================================
// File handle is owned for the entire session. trade_count and writes_truncated
// are observability counters surfaced via the TUI / tests; they don't affect
// behavior.
//======================================================================================================
struct ShardedTradeLog {
    FILE*    file;                                       // aggregate: logging/SYMBOL_order_history.csv (GUI/TradeReader reads this)
    uint64_t row_count;          // total rows written this session
    uint64_t writes_truncated;   // snprintf truncations (should always be 0)
    // v4.7.18: cache the symbol from Init so Rotate() can rebuild the
    // filename without forcing callers to thread bcfg through every
    // call site. Empty string before first Init.
    char     symbol[32];
    // v5.15.5.C.3 Phase 5.B — hybrid per-core trade-log mirror. Each fill is
    // written to BOTH the aggregate `file` (above; preserves GUI / TradeReader
    // backward compat) AND `per_core_files[event.core_id]` (per-core CSV;
    // enables per-core archive flow + future external consumers without
    // breaking the aggregate-reader contract).
    //
    // Filename pattern: logging/SYMBOL_core_<N>_order_history.csv (N = 0..MAX_EXECUTION_CORES-1)
    //
    // Per-fill cost: 2× fwrite (aggregate + per_core); cost negligible
    // (trades are rare events at strategy fire rate). The per-core file count
    // is bounded at MAX_EXECUTION_CORES (16) — well under any FILE* limit.
    //
    // Phase 5.B is the HYBRID approach (keeps aggregate for GUI compat;
    // adds per-core for archive). The FULL per-core-split + TradeReader
    // merge (per bundled-plan Reader/UI Impact section) is a deferred
    // follow-up if/when TradeReader needs to surface per-core tabs.
    FILE*    per_core_files[MAX_EXECUTION_CORES];
};

//======================================================================================================
// [PER-CORE FILENAME HELPER]  (v5.15.5.C.3 Phase 5.B — structural fix)
//======================================================================================================
// Single source of truth for the per-core mirror filename pattern. Used by
// _Init (open), _Rotate (rename source), and EngineSharded_Run paper-reset
// archive copy. Returns 1 on success, 0 on snprintf truncation.
//
// Class-18 mirror close: prior to this helper, the format string
//   "logging/%s_core_%d_order_history.csv"
// appeared at 3 sites — changing the pattern required updating all 3, or
// the next consumer would silently drift. Helper makes drift impossible.
//======================================================================================================
inline int ShardedTradeLog_FormatPerCoreFilename(char* buf, size_t bufsz,
                                                  const char* symbol, int core_id) {
    int n = snprintf(buf, bufsz,
                     "logging/%s_core_%d_order_history.csv", symbol, core_id);
    return (n >= 0 && (size_t)n < bufsz) ? 1 : 0;
}

//======================================================================================================
// [WRITE ROW]  (v5.15.5.C.3 Phase 5.B — structural fix)
//======================================================================================================
// Single chokepoint for "write one already-formatted row to aggregate file
// + mirror to per-core file + bump row_count". Called by RecordEntry,
// RecordExit, and any future RecordX added later — eliminates the
// "next consumer forgets per-core mirror" class structurally (closes
// Class-18 mirror at the dual-write level).
//
// Caller is responsible for the row buffer + length (already formatted via
// TRADE_LOG_EMIT_ROW_TO_BUFFER + length-checked for truncation). Caller also
// gates on `log->file != nullptr` before invoking — helper trusts the gate
// since the aggregate-write would AV on nullptr (slow-path; defensive null
// recheck would be redundant cost).
//
// Branchless bounds-check on `core_id` via (unsigned) cast: single
// comparison handles negative + overflow simultaneously (canonical idiom).
// Per-core mirror write short-circuits on either bound or nullptr; failure
// non-fatal — aggregate file already has the row.
//======================================================================================================
inline void ShardedTradeLog_WriteRow(ShardedTradeLog* log, int core_id,
                                      const char* row, size_t n) {
    fwrite(row, 1, n, log->file);
    if ((unsigned)core_id < (unsigned)MAX_EXECUTION_CORES &&
        log->per_core_files[core_id]) {
        fwrite(row, 1, n, log->per_core_files[core_id]);
    }
    log->row_count++;
}

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
    // v5.15.5.C.3 Phase 5.B — pre-zero per-core file array. Init walks 0..N-1
    // below; failure on any one is non-fatal (we log + continue without
    // that core's per-core mirror; aggregate file still serves the trade log).
    for (int c = 0; c < MAX_EXECUTION_CORES; ++c) log->per_core_files[c] = nullptr;
    // v4.7.18: cache symbol for Rotate() (must precede the early-return paths
    // below so even a failed Init records what was attempted)
    std::strncpy(log->symbol, symbol, sizeof(log->symbol) - 1);
    log->symbol[sizeof(log->symbol) - 1] = '\0';

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
        // v5.14.10.F — column header via FOREACH_TRADE_LOG_COL registry walk.
        // Byte-identical to pre-refactor literal (operator-parser compat).
        TradeLog_EmitHeader(log->file);
        fflush(log->file);
    }
    // v5.15.5.C.3 Phase 5.B — open per-core mirror files. Each
    // logging/SYMBOL_core_<N>_order_history.csv. Same line-buffering +
    // header-on-first-open behavior as the aggregate. Failure on any
    // individual core's open is non-fatal: that core's per_core_files[c]
    // stays nullptr; RecordEntry/Exit guards against null at the write site.
    for (int c = 0; c < MAX_EXECUTION_CORES; ++c) {
        char per_core_filename[256];
        if (!ShardedTradeLog_FormatPerCoreFilename(
                per_core_filename, sizeof(per_core_filename), symbol, c)) continue;
        int has_content_pc = 0;
        FILE* probe_pc = fopen(per_core_filename, "r");
        if (probe_pc) {
            has_content_pc = (fgetc(probe_pc) != EOF);
            fclose(probe_pc);
        }
        log->per_core_files[c] = fopen(per_core_filename, "a");
        if (!log->per_core_files[c]) continue;
        setvbuf(log->per_core_files[c], nullptr, _IOLBF, 0);
        if (!has_content_pc) {
            fprintf(log->per_core_files[c],
                "# v3 sharded engine (per-core mirror; core=%d) — rows in arrival order; "
                "same column shape as aggregate logging/%s_order_history.csv\n",
                c, symbol);
            TradeLog_EmitHeader(log->per_core_files[c]);
            fflush(log->per_core_files[c]);
        }
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
    // v5.15.5.C.3 Phase 5.B — flush per-core mirror files too.
    for (int c = 0; c < MAX_EXECUTION_CORES; ++c) {
        if (log->per_core_files[c]) fflush(log->per_core_files[c]);
    }
}

//======================================================================================================
// [ROTATE] (v4.7.18 — Reset Paper integration)
//======================================================================================================
// Close the current trade log, rename it to a timestamped backup, and reopen
// fresh. Used by the GUI's "Reset Paper" button so the user gets a clean
// trade-history view without losing the prior session's data.
//
// Naming: logging/SYMBOL_order_history.YYYYMMDD-HHMMSS.csv
//
// Returns 1 on success, 0 if anything fails (no-op on failure — engine keeps
// the existing file open).
//======================================================================================================
inline int ShardedTradeLog_Rotate(ShardedTradeLog* log) {
    if (!log->file || log->symbol[0] == '\0') return 0;
    fclose(log->file);
    log->file = nullptr;

    // v5.15.5.C.3 Phase 5.B — close all per-core mirror files BEFORE
    // computing the rename target (so the rename below sees a stable
    // filesystem snapshot for both aggregate + per-core files).
    for (int c = 0; c < MAX_EXECUTION_CORES; ++c) {
        if (log->per_core_files[c]) {
            fclose(log->per_core_files[c]);
            log->per_core_files[c] = nullptr;
        }
    }

    char src[256], dst[256];
    int sn = snprintf(src, sizeof(src),
                      "logging/%s_order_history.csv", log->symbol);
    if (sn < 0 || (size_t)sn >= sizeof(src)) return 0;

    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    int dn = snprintf(dst, sizeof(dst),
                      "logging/%s_order_history.%04d%02d%02d-%02d%02d%02d.csv",
                      log->symbol,
                      tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                      tm->tm_hour, tm->tm_min, tm->tm_sec);
    if (dn < 0 || (size_t)dn >= sizeof(dst)) return 0;

    if (rename(src, dst) != 0) {
        // rename failed — try to reopen the original file so we don't
        // end up with no log at all
        log->file = fopen(src, "a");
        return 0;
    }

    // v5.15.5.C.3 Phase 5.B — rotate per-core mirror files too.
    // Same timestamp suffix used for the aggregate rename above (`tm` reused).
    // Each rename failure is non-fatal: per-core files might not exist yet
    // (first-run case) or the rename might fail for other reasons; _Init
    // below reopens fresh files regardless.
    for (int c = 0; c < MAX_EXECUTION_CORES; ++c) {
        char src_pc[256], dst_pc[256];
        if (!ShardedTradeLog_FormatPerCoreFilename(
                src_pc, sizeof(src_pc), log->symbol, c)) continue;
        int dn_pc = snprintf(dst_pc, sizeof(dst_pc),
                              "logging/%s_core_%d_order_history.%04d%02d%02d-%02d%02d%02d.csv",
                              log->symbol, c,
                              tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                              tm->tm_hour, tm->tm_min, tm->tm_sec);
        if (dn_pc < 0 || (size_t)dn_pc >= sizeof(dst_pc)) continue;
        // Best-effort rename; ignore failure (file may not exist if Init failed for this core).
        rename(src_pc, dst_pc);
    }

    log->row_count = 0;
    // Reopen fresh — Init writes the header again since file is now empty.
    // _Init also reopens the per-core mirror files.
    return ShardedTradeLog_Init(log, log->symbol);
}

inline void ShardedTradeLog_Close(ShardedTradeLog* log) {
    if (log->file) {
        fflush(log->file);
        fclose(log->file);
        log->file = nullptr;
    }
    // v5.15.5.C.3 Phase 5.B — close per-core mirror files at shutdown.
    for (int c = 0; c < MAX_EXECUTION_CORES; ++c) {
        if (log->per_core_files[c]) {
            fflush(log->per_core_files[c]);
            fclose(log->per_core_files[c]);
            log->per_core_files[c] = nullptr;
        }
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
                                         FPN_Binary<F> entry_price,
                                         FPN_Binary<F> trade_size,
                                         FPN_Binary<F> entry_fee,
                                         FPN_Binary<F> balance_after,
                                         int regime = -1) {
    if (!log->file) return;
    char row[1024];
    // v5.14.10.F — registry-driven row build via FOREACH_TRADE_LOG_COL.
    // v5.15.5.C.3 Phase 5.A — added regime + regime_name columns. Append-only
    // addition: existing 11-col operator parsers see 2 extra trailing CSV
    // columns; no breakage. regime = -1 from non-strategy-aware callers
    // (e.g. OMS HandleFill in event_log_mode=1) resolves to "UNKNOWN" name.
    // Caller-scope variables set BEFORE the registry walk macro per
    // CALLER SCOPE CONTRACT in TradeLogColRegistry.hpp.
    uint64_t timestamp_us  = event.timestamp;
    uint32_t core_id       = event.core_id;
    char     event_type    = 'E';
    double   price_v       = FPN_ToDouble(event.price);
    double   entry_price_v = FPN_ToDouble(entry_price);
    double   exit_price_v  = 0.0;                                // entry: exit_price unused
    double   pnl_v         = 0.0;                                // entry: pnl unused
    double   fees_v        = FPN_ToDouble(entry_fee);            // entry: fees = entry_fee
    double   balance_after_v = FPN_ToDouble(balance_after);
    double   trade_size_v    = FPN_ToDouble(trade_size);
    int      regime_v        = regime;
    const char* regime_name_v = (regime >= 0 && regime < NUM_REGIMES)
                                    ? REGIME_INFO[regime].full_name
                                    : "UNKNOWN";
    int n = 0;
    TRADE_LOG_EMIT_ROW_TO_BUFFER(row, sizeof(row), &n);
    if (n < 0 || (size_t)n >= sizeof(row)) {
        log->writes_truncated++;
        return;
    }
    // v5.15.5.C.3 Phase 5.B — single chokepoint: aggregate write + per-core
    // mirror + row_count bump. See ShardedTradeLog_WriteRow comment for
    // Class-18 mirror close rationale.
    ShardedTradeLog_WriteRow(log, (int)core_id, row, (size_t)n);
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
                                        FPN_Binary<F> entry_price,
                                        FPN_Binary<F> exit_price,
                                        FPN_Binary<F> trade_size,
                                        FPN_Binary<F> net_pnl,
                                        FPN_Binary<F> total_fees,
                                        FPN_Binary<F> balance_after,
                                        int regime = -1) {
    if (!log->file) return;
    char row[1024];
    // v5.14.10.F — registry-driven row build via FOREACH_TRADE_LOG_COL.
    // v5.15.5.C.3 Phase 5.A — added regime + regime_name columns (see RecordEntry).
    uint64_t timestamp_us  = event.timestamp;
    uint32_t core_id       = event.core_id;
    char     event_type    = 'X';
    double   price_v       = FPN_ToDouble(event.price);
    double   entry_price_v = FPN_ToDouble(entry_price);
    double   exit_price_v  = FPN_ToDouble(exit_price);
    double   pnl_v         = FPN_ToDouble(net_pnl);              // exit: pnl = net_pnl
    double   fees_v        = FPN_ToDouble(total_fees);           // exit: fees = total_fees
    double   balance_after_v = FPN_ToDouble(balance_after);
    double   trade_size_v    = FPN_ToDouble(trade_size);
    int      regime_v        = regime;
    const char* regime_name_v = (regime >= 0 && regime < NUM_REGIMES)
                                    ? REGIME_INFO[regime].full_name
                                    : "UNKNOWN";
    int n = 0;
    TRADE_LOG_EMIT_ROW_TO_BUFFER(row, sizeof(row), &n);
    if (n < 0 || (size_t)n >= sizeof(row)) {
        log->writes_truncated++;
        return;
    }
    // v5.15.5.C.3 Phase 5.B — single chokepoint: aggregate write + per-core
    // mirror + row_count bump. See ShardedTradeLog_WriteRow comment for
    // Class-18 mirror close rationale.
    ShardedTradeLog_WriteRow(log, (int)core_id, row, (size_t)n);
}

}  // namespace tt
