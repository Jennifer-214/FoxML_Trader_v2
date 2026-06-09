// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [DEPTH REPLAY STATE]
//======================================================================================================
// Backtest analog of DepthSharedState (DataStream/BinanceDepth.hpp). Reads
// the same daily CSV files DepthRecorder writes (Phase 8a) and exposes a
// BookSnapshot<F> that BacktestSharded_Run can advance in lockstep with the
// tick stream.
//
// CSV format (matches DepthRecorder_Write at DepthRecorder.hpp:249):
//   timestamp_us,last_update_id,bid_price,bid_qty,ask_price,ask_qty
//   one row per @depth5@100ms snapshot (top-of-book only).
//   "# GAP at_us=X reason=Y" comment lines mark recorded gaps; skipped on
//   parse (replay should preserve the silence the live engine would have
//   experienced — i.e., book_imbalance stays at the value held at the
//   previous row until the next valid row is reached).
//
// Single-threaded by construction. Backtest replay runs all state on one
// thread, so no double-buffer / atomic active_idx pattern is needed; the
// `current` snapshot is a plain field updated synchronously inside
// _Advance.
//
// Heap lifecycle (four-site rule from CLAUDE.md "Plan Review Checklist §4"):
//   1. caller NULL-inits rows ptr in any zero-init / aggregate-init path
//      (memset-zero satisfies this — rows stays NULL until _LoadDay)
//   2. _LoadDay frees + re-allocates rows on every call (handles re-load
//      across day boundaries during multi-day backtests)
//   3. _Free releases rows + zeros capacity (idempotent)
//   4. backtest doesn't snapshot DepthReplayState (transient — derived
//      from CSV files), so no snapshot-persistence wiring needed
//======================================================================================================

#ifndef DEPTH_REPLAY_STATE_HPP
#define DEPTH_REPLAY_STATE_HPP

#include "BinanceDepth.hpp"  // BookSnapshot<F>, BookSnapshot_Init
#include "../CoreFrameworks/ParseFast.hpp"  // F-055: tt::parse_double_fast_advance (locale-immune replay parse)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

template <unsigned F>
struct DepthReplayState {
    // The "current" snapshot — what consumers read. Mirrors what
    // DepthSharedState::snapshots[active_idx] exposes in the live engine,
    // so `replay->current.imbalance` and `shared->snapshots[a].imbalance`
    // are interchangeable in slow-path code.
    BookSnapshot<F> current;

    // Heap-allocated row buffer for the currently loaded day. Sorted by
    // timestamp_us (CSV writer guarantees monotonic order, see
    // DepthRecorder.hpp:236-246 — backward jumps in last_update_id flush
    // a "# GAP" line + reset, but timestamps stay monotonic).
    BookSnapshot<F>* rows;
    int row_count;       // valid entries in [0, row_count)
    int row_capacity;    // allocation size
    int cursor;          // next row to consume in _Advance (monotonic)

    int current_day_int; // YYYYMMDD of currently loaded file (-1 = nothing loaded)
    int file_present;    // 1 if last _LoadDay found the file, 0 if missing
    char symbol[32];
    char data_dir[256];  // {base_dir}/{SYMBOL}/depth/
};

//======================================================================================================
// [DATE HELPER]
//======================================================================================================
// Convert a microsecond timestamp to a YYYYMMDD integer (UTC). Mirrors
// DepthRecorder_DateInt — both must agree so file boundaries align with
// what the recorder wrote.
//======================================================================================================
template <unsigned F>
static inline int DepthReplay_DateInt(uint64_t timestamp_us) {
    time_t t = (time_t)(timestamp_us / 1000000ULL);
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    return (tm_utc.tm_year + 1900) * 10000 + (tm_utc.tm_mon + 1) * 100 + tm_utc.tm_mday;
}

//======================================================================================================
// [INIT]
//======================================================================================================
// NULL-inits the row buffer + sets paths. Caller MUST call _Free at shutdown
// to release any heap rows accumulated by _LoadDay calls.
//
// base_dir is the parent directory above the symbol directory — e.g.
// "data" → state will read "data/{SYMBOL}/depth/YYYY-MM-DD.csv". Trailing
// slash optional.
//======================================================================================================
template <unsigned F>
static inline void DepthReplayState_Init(DepthReplayState<F>* s,
                                         const char* symbol,
                                         const char* base_dir) {
    memset(s, 0, sizeof(*s));
    s->current = BookSnapshot_Init<F>();
    s->rows = nullptr;
    s->row_count = 0;
    s->row_capacity = 0;
    s->cursor = 0;
    s->current_day_int = -1;
    s->file_present = 0;

    int sym_len = (int)strlen(symbol);
    if (sym_len > 31) sym_len = 31;
    char upper_sym[32];
    for (int i = 0; i < sym_len; i++)
        upper_sym[i] = (symbol[i] >= 'a' && symbol[i] <= 'z') ? symbol[i] - 32 : symbol[i];
    upper_sym[sym_len] = '\0';
    snprintf(s->symbol, sizeof(s->symbol), "%s", upper_sym);

    int base_len = (int)strlen(base_dir);
    int needs_slash = (base_len > 0 && base_dir[base_len - 1] != '/');
    snprintf(s->data_dir, sizeof(s->data_dir), "%s%s%s/depth/",
             base_dir, needs_slash ? "/" : "", upper_sym);
}

//======================================================================================================
// [FREE]
//======================================================================================================
// Idempotent — safe to call on never-initialized memory IF the caller
// zero-inits the struct first (the standard NULL-pointer check below
// short-circuits cleanly). Required at backtest shutdown to avoid leaking
// row buffers across runs.
//======================================================================================================
template <unsigned F>
static inline void DepthReplayState_Free(DepthReplayState<F>* s) {
    if (!s) return;
    if (s->rows) {
        free(s->rows);
        s->rows = nullptr;
    }
    s->row_count = 0;
    s->row_capacity = 0;
    s->cursor = 0;
    s->current_day_int = -1;
    s->file_present = 0;
}

//======================================================================================================
// [LOAD DAY]
//======================================================================================================
// Open data/{SYMBOL}/depth/YYYY-MM-DD.csv and load all rows into rows[].
// Frees + re-allocates the buffer if a previous day was loaded. Returns
// the number of rows loaded (0 if file missing or empty).
//
// Missing-file case: graceful degrade. file_present=0, rows freed, current
// snapshot left at whatever it was (most likely the last row of the
// previous day, or zero-init if first call). Backtest reads still return
// that stale snapshot — caller can poll s->file_present to log a warning.
//
// "# GAP" comment lines are SKIPPED during parse. The replay doesn't try to
// reproduce the gap as a stale-snapshot signal — backtest semantics treat
// the absence of new data as "snapshot remains at previous value," which
// matches what live consumers see during a real disconnect (the depth
// thread fails to update active_idx, so engine reads the same active
// snapshot until reconnect).
//======================================================================================================
template <unsigned F>
static inline int DepthReplayState_LoadDay(DepthReplayState<F>* s, int date_int) {
    // Free prior buffer regardless — we're switching files (or re-loading
    // the same one, which should be idempotent).
    if (s->rows) {
        free(s->rows);
        s->rows = nullptr;
    }
    s->row_count = 0;
    s->row_capacity = 0;
    s->cursor = 0;
    s->file_present = 0;

    int year  = date_int / 10000;
    int month = (date_int / 100) % 100;
    int day   = date_int % 100;

    char path[512];
    snprintf(path, sizeof(path), "%s%04d-%02d-%02d.csv", s->data_dir, year, month, day);

    FILE* fp = fopen(path, "r");
    if (!fp) {
        s->current_day_int = date_int;  // record so we don't keep retrying
        return 0;
    }
    s->file_present = 1;

    // First-pass: count non-comment, non-header lines so we can size the
    // buffer once. CSV is small (~7-8k rows/day for BTCUSDT) so two passes
    // is fine; avoids realloc churn during a tight parse.
    int line_count = 0;
    char buf[512];
    int header_seen = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        if (buf[0] == '#') continue;            // gap markers
        if (!header_seen) { header_seen = 1; continue; }  // CSV header row
        line_count++;
    }
    if (line_count <= 0) {
        fclose(fp);
        s->current_day_int = date_int;
        return 0;
    }

    s->rows = (BookSnapshot<F>*)calloc((size_t)line_count, sizeof(BookSnapshot<F>));
    if (!s->rows) {
        fclose(fp);
        s->current_day_int = date_int;
        return 0;
    }
    s->row_capacity = line_count;

    rewind(fp);
    header_seen = 0;
    int n = 0;
    while (fgets(buf, sizeof(buf), fp) && n < line_count) {
        if (buf[0] == '#') continue;
        if (!header_seen) { header_seen = 1; continue; }

        // Parse: timestamp_us,last_update_id,bid_price,bid_qty,ask_price,ask_qty
        char* p = buf;
        const char* e;  // F-055: locale-immune float parse advances a const cursor; bridge back to p (buf[] is mutable). Integer strtoull is locale-immune → unchanged (TECH_DEBT-144).
        uint64_t ts_us  = strtoull(p, &p, 10); if (*p == ',') p++;
        uint64_t upd_id = strtoull(p, &p, 10); if (*p == ',') p++;
        double bid_p    = tt::parse_double_fast_advance(p, &e); p = (char*)e; if (*p == ',') p++;
        double bid_q    = tt::parse_double_fast_advance(p, &e); p = (char*)e; if (*p == ',') p++;
        double ask_p    = tt::parse_double_fast_advance(p, &e); p = (char*)e; if (*p == ',') p++;
        double ask_q    = tt::parse_double_fast_advance(p, &e);

        BookSnapshot<F>& row = s->rows[n];
        row = BookSnapshot_Init<F>();  // zero everything
        row.timestamp_us   = ts_us;
        row.last_update_id = upd_id;
        row.bids[0].price = FPN_FromDouble<F>(bid_p);
        row.bids[0].qty   = FPN_FromDouble<F>(bid_q);
        row.asks[0].price = FPN_FromDouble<F>(ask_p);
        row.asks[0].qty   = FPN_FromDouble<F>(ask_q);
        // Derived fields, same formulas as depth_parse_json. Top-of-book
        // only (CSV stores top level), so imbalance = top_imbalance.
        row.spread    = FPN_Sub(row.asks[0].price, row.bids[0].price);
        row.mid_price = FPN_DivNoAssert(
            FPN_AddSat(row.bids[0].price, row.asks[0].price),
            FPN_FromDouble<F>(2.0));
        FPN_Binary<F> top_total = FPN_AddSat(row.bids[0].qty, row.asks[0].qty);
        if (!FPN_IsZero(top_total)) {
            row.top_imbalance = FPN_DivNoAssert(
                FPN_Sub(row.bids[0].qty, row.asks[0].qty), top_total);
            // CSV records only top level, so full-book imbalance == top
            row.imbalance = row.top_imbalance;
        }
        row.update_count = (uint64_t)(n + 1);
        n++;
    }
    fclose(fp);

    s->row_count = n;
    s->current_day_int = date_int;
    return n;
}

//======================================================================================================
// [ADVANCE]
//======================================================================================================
// Walk cursor forward to the latest row whose timestamp_us is <= target_us.
// Updates `current` to that row. Cursor is monotonic — never rewinds within
// a day. Calls _LoadDay implicitly when target's date crosses
// current_day_int.
//
// If no rows are loaded (file missing or empty), this is a no-op — `current`
// stays at whatever value it last held. Consumers can read s->file_present
// to detect this case.
//
// O(1) amortized per tick (cursor advances by however many rows are <=
// target since the last call). The first call after a long gap may walk
// many rows but each row is touched once; subsequent ticks see the
// already-advanced cursor.
//======================================================================================================
template <unsigned F>
static inline void DepthReplayState_Advance(DepthReplayState<F>* s,
                                            uint64_t target_us) {
    int target_day = DepthReplay_DateInt<F>(target_us);
    if (target_day != s->current_day_int) {
        DepthReplayState_LoadDay(s, target_day);
    }
    if (!s->rows || s->row_count == 0) return;

    // Walk forward while next row is still <= target. Stops at first row
    // whose timestamp exceeds target — that row will be consumed on a
    // future _Advance call.
    while (s->cursor < s->row_count &&
           s->rows[s->cursor].timestamp_us <= target_us) {
        s->current = s->rows[s->cursor];
        s->cursor++;
    }
}

//======================================================================================================
// [GET CURRENT]
//======================================================================================================
// Convenience accessor. The `current` field is also directly readable —
// this just makes intent clearer at call sites.
//======================================================================================================
template <unsigned F>
static inline const BookSnapshot<F>* DepthReplayState_GetSnapshot(const DepthReplayState<F>* s) {
    return &s->current;
}

#endif // DEPTH_REPLAY_STATE_HPP
