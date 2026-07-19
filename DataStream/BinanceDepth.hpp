// SPDX-License-Identifier: AGPL-3.0-or-later

//======================================================================================================
// [FILE]_[DataStream/BinanceDepth.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the @depth5@100ms venue stream — own thread parses into a double-buffered BookSnapshot (atomic active_idx swap); engine reads on the slow path, zero hot-path impact; 10Hz strstr scan v5.11.16-audited]
// [CONTAINS]
//   - [STRUCT]_[BookSnapshot]   (BookLevel + BookSnapshot_Init ride)
//   - [STRUCT]_[DepthStream]
//   - [STRUCT]_[DepthSharedState]
//   - [FUNCTION]_[depth_parse_json]
//   - [FUNCTION]_[depth_thread_fn]   (+ DepthStream_Init rides; the DepthRecorder include-cycle break lives here)
//======================================================================================================
// subscribes to Binance @depth5@100ms websocket for top-of-book bid/ask data
// runs on its own thread, writes to double-buffered BookSnapshot
// engine reads snapshot on slow path — zero hot-path impact
//
// uses shared WebSocketUtil.hpp for TCP/SSL/framing (same as BinanceCrypto)
//======================================================================================================
#ifndef BINANCE_DEPTH_HPP
#define BINANCE_DEPTH_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include "WebSocketUtil.hpp"
#include "../CoreFrameworks/Notify.hpp"  // Phase 8b — disconnect alerts
#include <stdlib.h>
#include <time.h>

//======================================================================
// [STRUCT]_[BookSnapshot]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[5-level book + derived spread/mid/imbalance features (BookLevel + BookSnapshot_Init ride) — FPN_Binary feature data (H4); last_update_id + local landing timestamp for the recorder]
// [INSTANTIATION]_[[64]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F> struct BookLevel {
    FPN_Binary<F> price;
    FPN_Binary<F> qty;
};

template <unsigned F> struct BookSnapshot {
    BookLevel<F> bids[5];
    BookLevel<F> asks[5];
    FPN_Binary<F> spread;           // asks[0].price - bids[0].price
    FPN_Binary<F> mid_price;        // (best_bid + best_ask) / 2
    FPN_Binary<F> imbalance;        // (total_bid_qty - total_ask_qty) / (total_bid_qty + total_ask_qty)
    FPN_Binary<F> top_imbalance;    // same but just top level
    uint64_t update_count;
    uint64_t last_update_id; // Binance "lastUpdateId" — monotonic per-symbol update sequence
                             // (0 if missing from message; set by depth_parse_json)
    uint64_t timestamp_us;   // local CLOCK_REALTIME microseconds when snapshot landed
                             // (set by depth_thread_fn after successful parse)
};

template <unsigned F> inline BookSnapshot<F> BookSnapshot_Init() {
    BookSnapshot<F> snap = {};
    for (int i = 0; i < 5; i++) {
        snap.bids[i].price = FPN_Zero<F>();
        snap.bids[i].qty   = FPN_Zero<F>();
        snap.asks[i].price = FPN_Zero<F>();
        snap.asks[i].qty   = FPN_Zero<F>();
    }
    snap.spread = FPN_Zero<F>();
    snap.mid_price = FPN_Zero<F>();
    snap.imbalance = FPN_Zero<F>();
    snap.top_imbalance = FPN_Zero<F>();
    snap.update_count = 0;
    snap.last_update_id = 0;
    snap.timestamp_us = 0;
    return snap;
}
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[416B]
// [ALIGN]_[16]
// [CACHE_LINES]_[7]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[BookSnapshot]
//======================================================================

//======================================================================
// [STRUCT]_[DepthStream]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the connection handle — socket + SSL pair + connected flag + pollfd]
//======================================================================
// [CODE]
//======================================================================
struct DepthStream {
    int sockfd;
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    int connected;
    struct pollfd pfd;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[40B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[DepthStream]
//======================================================================

//======================================================================
// [STRUCT]_[DepthSharedState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[engine reads / depth thread writes — double-buffered snapshots + atomic active_idx + quit flag + connection cfg + nullable recorder]
// [INSTANTIATION]_[[64]]
//======================================================================
// [CODE]
//======================================================================
struct DepthRecorder;  // fwd decl for the recorder pointer field — full definition in DepthRecorder.hpp,
                       // included near the bottom of this header (just before depth_thread_fn)
                       // so it can call the templated DepthRecorder_Write

template <unsigned F> struct DepthSharedState {
    BookSnapshot<F> snapshots[2];
    int active_idx;              // atomic: index the engine reads
    int quit_requested;          // atomic: signal thread to stop
    DepthStream stream;
    char symbol[32];
    char host[128];
    int port;
    int reconnect_delay;
    DepthRecorder *recorder;     // null = recording disabled (Phase 8a c5)
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[1056B]
// [ALIGN]_[16]
// [CACHE_LINES]_[17]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[DepthSharedState]
//======================================================================

//======================================================================
// [FUNCTION]_[depth_parse_json]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[parse one @depth5 message into a BookSnapshot — up to 5 [price,qty] pairs per side via digit-by-digit FPN_FromString (locale-immune) + derived spread/mid/imbalances]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
static inline int depth_parse_json(const char *json, int len, BookSnapshot<F> *snap) {
    // v5.11.16 — sanity floor. A real Binance @depth5 message is >= ~150
    // bytes (lastUpdateId + 5 bid pairs + 5 ask pairs). Under 30 is
    // a truncated frame; bail before scanning.
    if (len < 30) return 0;

    // lastUpdateId — monotonic per-symbol update id from Binance.
    // If absent (shouldn't be on @depth5@100ms but defensive), stays 0.
    // Recorder uses a backward jump in this id (0 sentinel excluded) as one
    // signal of a real gap; same-snapshot jumps of 50-500 are NORMAL between
    // 100ms windows and not flagged.
    const char *id_start = strstr(json, "\"lastUpdateId\"");
    if (id_start) {
        const char *colon = strchr(id_start, ':');
        if (colon) snap->last_update_id = strtoull(colon + 1, NULL, 10);
    }

    const char *bids_start = strstr(json, "\"bids\"");
    const char *asks_start = strstr(json, "\"asks\"");
    if (!bids_start || !asks_start) return 0;

    // parse up to 5 [price, qty] pairs starting from a JSON array
    auto parse_levels = [](const char *start, BookLevel<F> *levels, int max_levels) -> int {
        const char *p = strchr(start, '[');
        if (!p) return 0;
        p++;

        int count = 0;
        while (count < max_levels) {
            const char *inner = strchr(p, '[');
            if (!inner) break;
            const char *q1 = strchr(inner, '"');
            if (!q1) break;
            q1++;
            const char *q2 = strchr(q1, '"');
            if (!q2) break;

            char price_str[32];
            int plen_s = (int)(q2 - q1);
            if (plen_s >= 32) break;
            memcpy(price_str, q1, plen_s);
            price_str[plen_s] = '\0';

            const char *q3 = strchr(q2 + 1, '"');
            if (!q3) break;
            q3++;
            const char *q4 = strchr(q3, '"');
            if (!q4) break;

            char qty_str[32];
            int qlen_s = (int)(q4 - q3);
            if (qlen_s >= 32) break;
            memcpy(qty_str, q3, qlen_s);
            qty_str[qlen_s] = '\0';

            levels[count].price = FPN_FromString<F>(price_str);
            levels[count].qty   = FPN_FromString<F>(qty_str);
            count++;

            const char *cl = strchr(q4, ']');
            if (!cl) break;
            p = cl + 1;
        }
        return count;
    };

    int bid_count = parse_levels(bids_start, snap->bids, 5);
    int ask_count = parse_levels(asks_start, snap->asks, 5);
    if (bid_count == 0 || ask_count == 0) return 0;

    // derived fields
    snap->spread = FPN_Sub(snap->asks[0].price, snap->bids[0].price);
    snap->mid_price = FPN_DivNoAssert(
        FPN_AddSat(snap->bids[0].price, snap->asks[0].price),
        FPN_FromDouble<F>(2.0));

    // top-of-book imbalance
    FPN_Binary<F> top_total = FPN_AddSat(snap->bids[0].qty, snap->asks[0].qty);
    if (!FPN_IsZero(top_total))
        snap->top_imbalance = FPN_DivNoAssert(
            FPN_Sub(snap->bids[0].qty, snap->asks[0].qty), top_total);

    // full 5-level imbalance
    FPN_Binary<F> total_bid = FPN_Zero<F>(), total_ask = FPN_Zero<F>();
    for (int i = 0; i < bid_count; i++) total_bid = FPN_AddSat(total_bid, snap->bids[i].qty);
    for (int i = 0; i < ask_count; i++) total_ask = FPN_AddSat(total_ask, snap->asks[i].qty);
    FPN_Binary<F> total = FPN_AddSat(total_bid, total_ask);
    if (!FPN_IsZero(total))
        snap->imbalance = FPN_DivNoAssert(FPN_Sub(total_bid, total_ask), total);

    snap->update_count++;
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// parses Binance @depth5 format:
// {"lastUpdateId":123,"bids":[["price","qty"],...],"asks":[["price","qty"],...]}
//
// v5.11.16 (2026-05-07) — DataStream parsing audit. strstr/strchr scans on
// the JSON buffer are bounded by null termination — `ws_read_frame`
// writes `out[plen] = '\0'` on every frame, clamped
// to plen <= max_len. v5.11.4.A locale-immune parsing already covered
// number extraction (FPN_FromString is digit-by-digit; lastUpdateId reads
// via strtoull which is locale-stable for base-10 unsigned integers).
// Using the `len` parameter (previously unused) as a min-size sanity
// guard catches truncated frames before any scanning.
//======================================================================
// [END_FUNCTION]_[depth_parse_json]
//======================================================================

//======================================================================
// [FUNCTION]_[depth_thread_fn]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the depth thread (DepthStream_Init rides) — interruptible reconnect loop, back-buffer parse + RELEASE active_idx swap, disconnect gap-log + Notify alert, recorder write]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
static inline int DepthStream_Init(DepthSharedState<F> *shared, const char *symbol,
                                    const char *host, int port, int reconnect_delay) {
    snprintf(shared->symbol, sizeof(shared->symbol), "%s", symbol);
    snprintf(shared->host, sizeof(shared->host), "%s", host);
    shared->port = port;
    shared->reconnect_delay = reconnect_delay;
    shared->active_idx = 0;
    shared->quit_requested = 0;
    shared->snapshots[0] = BookSnapshot_Init<F>();
    shared->snapshots[1] = BookSnapshot_Init<F>();

    DepthStream *ds = &shared->stream;
    memset(ds, 0, sizeof(DepthStream));

    ds->sockfd = ws_tcp_connect(host, port);
    if (ds->sockfd < 0) return -1;
    if (ws_ssl_setup(&ds->ssl_ctx, &ds->ssl, ds->sockfd, host) < 0) {
        close(ds->sockfd); return -1;
    }

    char path[128];
    snprintf(path, sizeof(path), "/ws/%s@depth5@100ms", symbol);
    if (ws_handshake(ds->ssl, host, path) < 0) {
        ws_close(ds->ssl, ds->ssl_ctx, ds->sockfd); return -1;
    }

    ds->connected = 1;
    ds->pfd.fd = ds->sockfd;
    ds->pfd.events = POLLIN;
    return 0;
}

//------------------------------------------------------------------
// [SECTION]_[THREAD FUNCTION]
//------------------------------------------------------------------
// DepthRecorder.hpp includes this header for BookSnapshot<F>. Including it
// HERE (after BookSnapshot + DepthSharedState are fully defined, before
// depth_thread_fn) breaks the include cycle: the include guard short-circuits
// the inner BinanceDepth.hpp include in DepthRecorder.hpp, but BookSnapshot
// is already in scope so DepthRecorder_Write's template body resolves.
#include "DepthRecorder.hpp"

template <unsigned F>
static inline void *depth_thread_fn(void *arg) {
    DepthSharedState<F> *shared = (DepthSharedState<F> *)arg;
    DepthStream *ds = &shared->stream;
    char frame_buf[4096];

    while (!__atomic_load_n(&shared->quit_requested, __ATOMIC_ACQUIRE)) {
        if (!ds->connected) {
            // Interruptible sleep — checks shared->quit_requested every 100ms
            // so engine shutdown isn't blocked for reconnect_delay seconds.
            for (uint32_t s = 0; s < shared->reconnect_delay; ++s) {
                for (int j = 0; j < 10; ++j) {
                    if (__atomic_load_n(&shared->quit_requested, __ATOMIC_ACQUIRE))
                        return NULL;
                    struct timespec ts = {0, 100000000};
                    nanosleep(&ts, NULL);
                }
            }
            if (DepthStream_Init(shared, shared->symbol, shared->host,
                                  shared->port, shared->reconnect_delay) < 0) continue;
            ds = &shared->stream;
        }

        // SSL_pending optimization (same as BinanceCrypto)
        int ready = SSL_pending(ds->ssl) > 0;
        if (!ready) {
            int ret = poll(&ds->pfd, 1, 200);
            ready = (ret > 0 && (ds->pfd.revents & POLLIN));
        }
        if (!ready) continue;

        int opcode;
        int plen = ws_read_frame(ds->ssl, frame_buf, sizeof(frame_buf) - 1, &opcode);
        if (plen < 0) {
            // Phase 8a c5: log explicit gap on disconnect. _LogGap zeros
            // last_seen_id so the post-reconnect first _Write skips its
            // internal gap check (no double-flagging).
            if (shared->recorder) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                uint64_t at_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
                DepthRecorder_LogGap(shared->recorder, at_us, "disconnect");
            }
            // Phase 8b: alert. Cooldown collapses repeated disconnect storms.
            if (g_notify) {
                Notify_Send(g_notify, NOTIFY_WARN, NK_DISCONNECT_DEPTH,
                            "Binance depth WS disconnected",
                            "book_imbalance gate is reading stale data until "
                            "reconnect succeeds. Check if frequent or persistent.");
            }
            ds->connected = 0;
            continue;
        }

        if (opcode == 0x9) { ws_send_pong(ds->ssl); continue; }
        if (opcode == 0x8) { ds->connected = 0; continue; }
        if (opcode != 0x1) continue;

        // parse into back buffer, swap atomically
        int back = 1 - __atomic_load_n(&shared->active_idx, __ATOMIC_ACQUIRE);
        shared->snapshots[back] = shared->snapshots[shared->active_idx];
        if (depth_parse_json<F>(frame_buf, plen, &shared->snapshots[back])) {
            // Stamp local landing time for the recorder (Phase 8a). CLOCK_REALTIME
            // matches the wallclock used by gap-detection thresholds in
            // DepthRecorder_Write (>2s wallclock silence = real gap).
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            shared->snapshots[back].timestamp_us =
                (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
            __atomic_store_n(&shared->active_idx, back, __ATOMIC_RELEASE);

            // Phase 8a c5: persist snapshot. Recorder does its own gap
            // detection internally (backward last_update_id OR wallclock >2s).
            if (shared->recorder) {
                DepthRecorder_Write(shared->recorder, &shared->snapshots[back]);
            }
        }
    }

    if (ds->connected) ws_close(ds->ssl, ds->ssl_ctx, ds->sockfd);
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[depth_thread_fn]
//======================================================================

#endif // BINANCE_DEPTH_HPP
