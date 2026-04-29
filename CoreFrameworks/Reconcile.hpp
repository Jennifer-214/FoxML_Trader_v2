// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).

//======================================================================================================
// [LIVE EXCHANGE RECONCILIATION — v5.2.1 Phase 1]
//======================================================================================================
// On boot in live mode (engine_mode=live), call OrderManager_Reconcile to
// sync local view with exchange truth. Catches:
//   - Pre-shutdown live orders that are still open on Binance
//   - Fills the engine missed (WS disconnect during fill, crash mid-process)
//   - Local-vs-exchange position disagreement (snapshot stale or corrupted)
//
// Design: this header has the LOGIC (parsing JSON + applying decisions)
// separated from the NETWORK (which lives in BinanceOrderAPI.hpp). Tests
// pass mocked JSON strings and verify behavior without making real REST
// calls. Live boot calls the network functions, then the logic functions.
//
// Phases:
//   v5.2.1 Phase 1 (this) — boot-time reconcile only. account + openOrders
//                            + myTrades fetched once, reconciled, then
//                            engine starts consuming ticks.
//   Phase 2 (deferred)   — WS reconnect re-fetches openOrders + myTrades
//   Phase 3 (deferred)   — heartbeat poll every reconcile_interval_sec
//   Phase 4 (deferred)   — manual cancel detection
//
// Why dry_run is the default safe mode: first live deploy against testnet
// should LOG what would change without applying. After confirmed safe,
// flip reconcile_dry_run=0 in cfg.
//======================================================================================================

#ifndef RECONCILE_HPP
#define RECONCILE_HPP

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace tt {

//======================================================================================================
// [PARSED RECONCILE STRUCTS]
//======================================================================================================

struct ReconcileOpenOrder {
    int64_t order_id;
    char    symbol[16];
    double  price;
    double  orig_qty;
    char    status[16];          // "NEW", "PARTIALLY_FILLED", etc.
    char    side[8];             // "BUY", "SELL"
    char    client_order_id[40]; // engine sets these as "tt-..." prefix
    int     is_ours;             // 1 if client_order_id starts with "tt-"
};

struct ReconcileTrade {
    int64_t trade_id;
    int64_t order_id;
    double  price;
    double  qty;
    double  commission;
    int64_t time_ms;
    int     is_buyer;     // 1 = BUY fill
    int     is_maker;     // 1 = maker fill
};

struct ReconcileResult {
    // Inputs (echoed for logging)
    double exchange_usdt;
    double exchange_btc;
    int    open_order_count;
    int    new_trade_count;       // since last_processed_trade_id
    // Decisions
    int    refused_boot;          // 1 = CRITICAL disagreement, refuse to start
    char   refusal_reason[256];
    int    cancel_actions;        // # of orders we'd cancel (or did, if !dry_run)
    int    fills_to_replay;       // # of trades to replay through HandleFill
    // Diagnostic flags
    int    has_local_position_no_exchange; // local says open, exchange says flat
    int    has_exchange_position_no_local; // exchange says BTC > dust, local says 0 positions
};

//======================================================================================================
// [JSON PARSING — minimal, no allocator, no malloc]
//======================================================================================================
// We parse Binance's REST responses with strstr-based field extraction.
// Same approach BinanceOrderAPI.hpp uses for account balance parsing.
// Robust enough for real responses, simple enough to unit-test with
// mocked strings.
//======================================================================================================

// Find next '{' after `cursor`. Returns pointer to it, or nullptr.
static inline const char* reconcile_next_object(const char* cursor) {
    if (!cursor) return nullptr;
    return strchr(cursor, '{');
}

// Find matching close brace for object starting at `obj_start` (must
// point to '{'). Returns pointer to the matching '}', or nullptr.
// Naive — counts braces, doesn't handle escaped braces in strings, but
// Binance JSON doesn't have those.
static inline const char* reconcile_object_end(const char* obj_start) {
    if (!obj_start || *obj_start != '{') return nullptr;
    int depth = 0;
    for (const char* p = obj_start; *p; ++p) {
        if (*p == '{') depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) return p;
        }
    }
    return nullptr;
}

// Extract a string field by key. Searches `obj_start..obj_end` for
// "<key>":"<value>" and copies value. Returns 1 on success.
static inline int reconcile_get_str(const char* obj_start, const char* obj_end,
                                     const char* key, char* out, size_t out_cap) {
    if (out_cap == 0) return 0;
    out[0] = '\0';
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char* p = strstr(obj_start, search);
    if (!p || p >= obj_end) return 0;
    p += strlen(search);
    const char* end = strchr(p, '"');
    if (!end || end >= obj_end) return 0;
    size_t n = end - p;
    if (n >= out_cap) n = out_cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

// Extract a double field by key. Handles both "key":123.45 and
// "key":"123.45" forms (Binance often uses string-encoded numbers).
static inline double reconcile_get_double(const char* obj_start, const char* obj_end,
                                            const char* key) {
    char search[64];
    // Try string form first
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char* p = strstr(obj_start, search);
    if (p && p < obj_end) {
        return atof(p + strlen(search));
    }
    // Numeric form
    snprintf(search, sizeof(search), "\"%s\":", key);
    p = strstr(obj_start, search);
    if (p && p < obj_end) {
        return atof(p + strlen(search));
    }
    return 0.0;
}

// Extract an int64 field by key.
static inline int64_t reconcile_get_int64(const char* obj_start, const char* obj_end,
                                            const char* key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* p = strstr(obj_start, search);
    if (!p || p >= obj_end) return 0;
    return (int64_t)atoll(p + strlen(search));
}

// Extract a bool field by key. Binance uses true/false literal, no quotes.
static inline int reconcile_get_bool(const char* obj_start, const char* obj_end,
                                       const char* key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* p = strstr(obj_start, search);
    if (!p || p >= obj_end) return 0;
    p += strlen(search);
    return (strncmp(p, "true", 4) == 0) ? 1 : 0;
}

//======================================================================================================
// [PARSE OPEN ORDERS]
//======================================================================================================
// Input: raw JSON array from /api/v3/openOrders.
// Output: fills `out` array up to `out_cap`. Returns count parsed.
//======================================================================================================
inline int Reconcile_ParseOpenOrders(const char* json, ReconcileOpenOrder* out, int out_cap) {
    if (!json || !out || out_cap <= 0) return 0;
    int count = 0;
    const char* cursor = json;
    while (count < out_cap) {
        const char* obj_start = reconcile_next_object(cursor);
        if (!obj_start) break;
        const char* obj_end = reconcile_object_end(obj_start);
        if (!obj_end) break;
        ReconcileOpenOrder& o = out[count];
        memset(&o, 0, sizeof(o));
        o.order_id = reconcile_get_int64(obj_start, obj_end, "orderId");
        reconcile_get_str(obj_start, obj_end, "symbol", o.symbol, sizeof(o.symbol));
        o.price = reconcile_get_double(obj_start, obj_end, "price");
        o.orig_qty = reconcile_get_double(obj_start, obj_end, "origQty");
        reconcile_get_str(obj_start, obj_end, "status", o.status, sizeof(o.status));
        reconcile_get_str(obj_start, obj_end, "side", o.side, sizeof(o.side));
        reconcile_get_str(obj_start, obj_end, "clientOrderId",
                          o.client_order_id, sizeof(o.client_order_id));
        // "tt-" prefix = engine-submitted (matches BinanceAdapter convention)
        o.is_ours = (strncmp(o.client_order_id, "tt-", 3) == 0) ? 1 : 0;
        count++;
        cursor = obj_end + 1;
    }
    return count;
}

//======================================================================================================
// [PARSE MY TRADES]
//======================================================================================================
// Input: raw JSON array from /api/v3/myTrades.
// Output: fills `out` array up to `out_cap`. Returns count parsed.
//======================================================================================================
inline int Reconcile_ParseMyTrades(const char* json, ReconcileTrade* out, int out_cap) {
    if (!json || !out || out_cap <= 0) return 0;
    int count = 0;
    const char* cursor = json;
    while (count < out_cap) {
        const char* obj_start = reconcile_next_object(cursor);
        if (!obj_start) break;
        const char* obj_end = reconcile_object_end(obj_start);
        if (!obj_end) break;
        ReconcileTrade& t = out[count];
        memset(&t, 0, sizeof(t));
        t.trade_id = reconcile_get_int64(obj_start, obj_end, "id");
        t.order_id = reconcile_get_int64(obj_start, obj_end, "orderId");
        t.price = reconcile_get_double(obj_start, obj_end, "price");
        t.qty = reconcile_get_double(obj_start, obj_end, "qty");
        t.commission = reconcile_get_double(obj_start, obj_end, "commission");
        t.time_ms = reconcile_get_int64(obj_start, obj_end, "time");
        t.is_buyer = reconcile_get_bool(obj_start, obj_end, "isBuyer");
        t.is_maker = reconcile_get_bool(obj_start, obj_end, "isMaker");
        count++;
        cursor = obj_end + 1;
    }
    return count;
}

//======================================================================================================
// [DECIDE — pure function, fully testable with mocked inputs]
//======================================================================================================
// Given parsed exchange truth + local view summary, compute reconcile
// decisions. Does NOT execute (caller does, gated on dry_run flag).
//
// Inputs:
//   exchange_usdt, exchange_btc       — from /api/v3/account
//   open_orders, n_open               — from /api/v3/openOrders parse
//   trades, n_trades                  — from /api/v3/myTrades parse
//                                        (already filtered to since-last)
//   local_open_position_count         — from oms->portfolio.active_bitmap popcount
//   dust_threshold_btc                — exchange BTC below this = "no position"
//                                        (Binance min trade ≈ 0.00001 BTC)
//
// Outputs ReconcileResult with planned actions. Caller applies them.
//======================================================================================================
inline ReconcileResult Reconcile_Decide(
        double exchange_usdt,
        double exchange_btc,
        const ReconcileOpenOrder* open_orders, int n_open,
        const ReconcileTrade* trades, int n_trades,
        int local_open_position_count,
        double dust_threshold_btc = 0.00001) {
    ReconcileResult r;
    memset(&r, 0, sizeof(r));
    r.exchange_usdt = exchange_usdt;
    r.exchange_btc  = exchange_btc;
    r.open_order_count = n_open;
    r.new_trade_count = n_trades;

    // Decision 1: any of OUR open orders? Cancel them. Pre-shutdown
    // leftovers shouldn't auto-resume.
    for (int i = 0; i < n_open; ++i) {
        if (open_orders[i].is_ours) r.cancel_actions++;
    }

    // Decision 2: replay missed fills. Caller passes already-filtered
    // (since-last-processed-id) trades, so replay all of them.
    r.fills_to_replay = n_trades;

    // Decision 3: local-vs-exchange position state.
    int has_exchange_btc = (exchange_btc > dust_threshold_btc) ? 1 : 0;
    r.has_local_position_no_exchange = (local_open_position_count > 0 && !has_exchange_btc) ? 1 : 0;
    r.has_exchange_position_no_local = (has_exchange_btc && local_open_position_count == 0) ? 1 : 0;

    // CRITICAL refusal: exchange has BTC but local has no positions.
    // Could be testnet residue (resolved by manual cleanup), real position
    // (engine should refuse to overwrite), or a snapshot/accounting bug
    // (engine should refuse to silently corrupt). Either way, REFUSE BOOT.
    if (r.has_exchange_position_no_local) {
        r.refused_boot = 1;
        snprintf(r.refusal_reason, sizeof(r.refusal_reason),
            "Exchange has %.6f BTC but local snapshot has 0 open positions. "
            "Manual investigation required: testnet residue, real position, "
            "or accounting bug. Refusing to boot.", exchange_btc);
    }
    // The reverse case (local says open, exchange says flat) is handled
    // softer: log + force-close local slot. Not a refusal — it likely
    // means a sell fill that the engine processed but didn't snapshot
    // before crashing. Caller force-closes.

    return r;
}

//======================================================================================================
// [APPLY — caller-driven, gated on dry_run]
//======================================================================================================
// Logging-only stub. Real apply happens in EngineSharded boot when this
// header is wired into main.cpp. The key actions:
//   1. Log all decisions (always)
//   2. If !dry_run:
//      a. Cancel "is_ours" open orders via REST
//      b. Synthesize fill commands for each unprocessed trade and
//         enqueue via OrderManager_ProcessFillCommand
//      c. Force-close local slots that are no longer on exchange
//      d. Update oms->last_processed_trade_id to highest in trades[]
//   3. If refused_boot: caller exits / refuses to advance
//======================================================================================================
inline void Reconcile_LogReport(const ReconcileResult& r, int dry_run) {
    fprintf(stderr,
        "[reconcile] exchange: USDT=%.2f BTC=%.6f | %d open orders, %d new trades\n",
        r.exchange_usdt, r.exchange_btc, r.open_order_count, r.new_trade_count);
    if (r.cancel_actions > 0) {
        fprintf(stderr,
            "[reconcile] %s: cancel %d engine-submitted open orders\n",
            dry_run ? "DRY_RUN" : "APPLY", r.cancel_actions);
    }
    if (r.fills_to_replay > 0) {
        fprintf(stderr,
            "[reconcile] %s: replay %d missed fills via HandleFill\n",
            dry_run ? "DRY_RUN" : "APPLY", r.fills_to_replay);
    }
    if (r.has_local_position_no_exchange) {
        fprintf(stderr,
            "[reconcile] %s: local has open position but exchange BTC=0 — "
            "force-close local slot (likely missed sell fill)\n",
            dry_run ? "DRY_RUN" : "APPLY");
    }
    if (r.refused_boot) {
        fprintf(stderr,
            "[reconcile] CRITICAL — refusing boot: %s\n", r.refusal_reason);
    }
}

}  // namespace tt

#endif  // RECONCILE_HPP
