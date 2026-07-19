#pragma once

//======================================================================
// [FILE]_[GUI/TradeReader.hpp]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[reads the engine CSV trade log into chart markers + an equity curve — a port of tools/chart.py TradeReader; refreshes on file-size change (no per-frame reopen), locale-immune CSV parse]
//======================================================================
// TradeReader — reads engine CSV trade log for chart markers + equity curve
// port of tools/chart.py TradeReader
// refreshes by checking file size (no inotify, same as Python)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <sys/stat.h>
#include "../CoreFrameworks/ParseFast.hpp"  // v5.11.4.C — locale-immune CSV parsing

static constexpr int MAX_TRADES = 512;

//======================================================================
// [STRUCT]_[TradeMarker]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one chart trade marker — price + buy/sell flag + TP/other-exit flag]
//======================================================================
// [CODE]
//======================================================================
struct TradeMarker {
    double price;
    int is_sell;       // 0 = buy, 1 = sell
    int is_tp;         // 1 = TP exit, 0 = SL/other
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[16B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[TradeMarker]
//======================================================================

struct EquityPoint {
    double cumulative_pnl;
};

//======================================================================
// [STRUCT]_[TradeData]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the loaded trade markers + equity curve + the file-size cache for change detection]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
struct TradeData {
    TradeMarker markers[MAX_TRADES];
    int marker_count;

    EquityPoint equity[MAX_TRADES];
    int equity_count;

    int max_visible_markers;  // cap to recent trades (0 = no cap)

    // for tracking file changes
    char csv_path[256];
    long last_size;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[12568B]
// [ALIGN]_[8]
// [CACHE_LINES]_[197]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[TradeData]
//======================================================================

//======================================================================
// [FUNCTION]_[TradeData_Init]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[init a TradeData for a CSV path]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void TradeData_Init(TradeData *td, const char *csv_path) {
    memset(td, 0, sizeof(*td));
    strncpy(td->csv_path, csv_path, 255);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TradeData_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[csv_field]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[extract the Nth comma-separated field from a CSV line]
//======================================================================
// simple CSV field parser: find Nth comma-separated field in line
//======================================================================
// [CODE]
//======================================================================
static inline const char *csv_field(const char *line, int field_idx, char *out, int out_len) {
    int current = 0;
    const char *p = line;
    const char *field_start = p;

    while (*p) {
        if (*p == ',' || *p == '\n' || *p == '\r') {
            if (current == field_idx) {
                int len = (int)(p - field_start);
                if (len >= out_len) len = out_len - 1;
                memcpy(out, field_start, len);
                out[len] = '\0';
                return out;
            }
            current++;
            field_start = p + 1;
        }
        p++;
    }
    // last field (no trailing comma/newline)
    if (current == field_idx) {
        int len = (int)(p - field_start);
        if (len >= out_len) len = out_len - 1;
        memcpy(out, field_start, len);
        out[len] = '\0';
        return out;
    }
    out[0] = '\0';
    return out;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[csv_field]
//======================================================================

//======================================================================
// [FUNCTION]_[TradeData_Refresh]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[reload the trade CSV if its size changed — pair BUY/SELL fees FIFO into net-P&L equity points, cap markers to the recent window]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void TradeData_Refresh(TradeData *td) {
    struct stat st;
    if (stat(td->csv_path, &st) != 0) return;
    if (st.st_size == td->last_size) return;
    td->last_size = st.st_size;

    td->marker_count = 0;
    td->equity_count = 0;

    FILE *f = fopen(td->csv_path, "r");
    if (!f) return;

    char line[1024];
    double cumulative_pnl = 0.0;
    int first_line = 1;

    // CSV columns (from TradeLog.hpp):
    // 0:tick, 1:side, 2:price, 3:quantity, 4:entry_price, 5:delta_pct, 6:exit_reason,
    // 7:buy_price_cond, 8:buy_vol_cond, 9:take_profit, 10:stop_loss,
    // 11:stddev, 12:avg, 13:balance, 14:fee_cost, 15:spacing, 16:gate_dist_pct,
    // 17:strategy, 18:regime

    // track entry fees by entry_price so we can pair them with sells
    // (BUY rows log entry fee in field 14, SELL rows log exit fee in field 14)
    double pending_entry_fees[MAX_TRADES];
    int pending_count = 0;

    while (fgets(line, sizeof(line), f)) {
        if (first_line) { first_line = 0; continue; }  // skip header
        if (td->marker_count >= MAX_TRADES) break;

        char side[16], price_s[32], qty_s[32], entry_s[32], reason_s[16], fee_s[32];
        csv_field(line, 1, side, sizeof(side));
        csv_field(line, 2, price_s, sizeof(price_s));
        csv_field(line, 3, qty_s, sizeof(qty_s));
        csv_field(line, 4, entry_s, sizeof(entry_s));
        csv_field(line, 6, reason_s, sizeof(reason_s));
        csv_field(line, 14, fee_s, sizeof(fee_s));

        // v5.11.4.C — locale-immune CSV field parsing.
        double price = tt::parse_double_fast(price_s);
        if (price < 1.0) continue;

        if (strcmp(side, "BUY") == 0) {
            TradeMarker *m = &td->markers[td->marker_count++];
            m->price = price;
            m->is_sell = 0;
            m->is_tp = 0;
            // stash entry fee for pairing with the corresponding sell
            if (pending_count < MAX_TRADES) {
                pending_entry_fees[pending_count++] = tt::parse_double_fast(fee_s);
            }
        } else if (strcmp(side, "SELL") == 0) {
            double entry = tt::parse_double_fast(entry_s);
            double qty = tt::parse_double_fast(qty_s);
            double exit_fee = tt::parse_double_fast(fee_s);
            if (entry < 1.0) entry = price;

            // pop matching entry fee (FIFO — buys and sells pair in order)
            double entry_fee = 0.0;
            if (pending_count > 0) {
                entry_fee = pending_entry_fees[0];
                // shift remaining (typically 0-1 pending in single-slot mode)
                for (int j = 1; j < pending_count; j++)
                    pending_entry_fees[j-1] = pending_entry_fees[j];
                pending_count--;
            }

            // net P&L: gross price delta minus both entry and exit fees
            double pnl = (price - entry) * qty - entry_fee - exit_fee;
            cumulative_pnl += pnl;

            TradeMarker *m = &td->markers[td->marker_count++];
            m->price = price;
            m->is_sell = 1;
            m->is_tp = (strcmp(reason_s, "TP") == 0) ? 1 : 0;

            if (td->equity_count < MAX_TRADES) {
                td->equity[td->equity_count++].cumulative_pnl = cumulative_pnl;
            }
        }
    }

    fclose(f);

    // cap markers to recent trades — older ones have scrolled off the chart
    // 2× visible candles ≈ visible time window (single-slot: ~1 trade per 1-2 candles)
    // caller can set max_visible_markers before refresh; default 120
    if (td->max_visible_markers > 0 && td->marker_count > td->max_visible_markers) {
        int skip = td->marker_count - td->max_visible_markers;
        memmove(td->markers, td->markers + skip,
                td->max_visible_markers * sizeof(TradeMarker));
        td->marker_count = td->max_visible_markers;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TradeData_Refresh]
//======================================================================
