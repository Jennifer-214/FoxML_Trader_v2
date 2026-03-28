// SPDX-License-Identifier: AGPL-3.0-or-later

//======================================================================================================
// [BINANCE DEPTH STREAM]
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
#include <stdlib.h>
#include <time.h>

//======================================================================================================
// [BOOK SNAPSHOT]
//======================================================================================================
template <unsigned F> struct BookLevel {
    FPN<F> price;
    FPN<F> qty;
};

template <unsigned F> struct BookSnapshot {
    BookLevel<F> bids[5];
    BookLevel<F> asks[5];
    FPN<F> spread;           // asks[0].price - bids[0].price
    FPN<F> mid_price;        // (best_bid + best_ask) / 2
    FPN<F> imbalance;        // (total_bid_qty - total_ask_qty) / (total_bid_qty + total_ask_qty)
    FPN<F> top_imbalance;    // same but just top level
    uint64_t update_count;
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
    return snap;
}

//======================================================================================================
// [DEPTH STREAM STATE]
//======================================================================================================
struct DepthStream {
    int sockfd;
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    int connected;
    struct pollfd pfd;
};

//======================================================================================================
// [SHARED STATE] — engine reads, depth thread writes
//======================================================================================================
template <unsigned F> struct DepthSharedState {
    BookSnapshot<F> snapshots[2];
    int active_idx;              // atomic: index the engine reads
    int quit_requested;          // atomic: signal thread to stop
    DepthStream stream;
    char symbol[32];
    char host[128];
    int port;
    int reconnect_delay;
};

//======================================================================================================
// [DEPTH JSON PARSER]
//======================================================================================================
// parses Binance @depth5 format:
// {"lastUpdateId":123,"bids":[["price","qty"],...],"asks":[["price","qty"],...]}
//======================================================================================================
template <unsigned F>
static inline int depth_parse_json(const char *json, int len, BookSnapshot<F> *snap) {
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
    FPN<F> top_total = FPN_AddSat(snap->bids[0].qty, snap->asks[0].qty);
    if (!FPN_IsZero(top_total))
        snap->top_imbalance = FPN_DivNoAssert(
            FPN_Sub(snap->bids[0].qty, snap->asks[0].qty), top_total);

    // full 5-level imbalance
    FPN<F> total_bid = FPN_Zero<F>(), total_ask = FPN_Zero<F>();
    for (int i = 0; i < bid_count; i++) total_bid = FPN_AddSat(total_bid, snap->bids[i].qty);
    for (int i = 0; i < ask_count; i++) total_ask = FPN_AddSat(total_ask, snap->asks[i].qty);
    FPN<F> total = FPN_AddSat(total_bid, total_ask);
    if (!FPN_IsZero(total))
        snap->imbalance = FPN_DivNoAssert(FPN_Sub(total_bid, total_ask), total);

    snap->update_count++;
    return 1;
}

//======================================================================================================
// [INIT]
//======================================================================================================
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

//======================================================================================================
// [THREAD FUNCTION]
//======================================================================================================
template <unsigned F>
static inline void *depth_thread_fn(void *arg) {
    DepthSharedState<F> *shared = (DepthSharedState<F> *)arg;
    DepthStream *ds = &shared->stream;
    char frame_buf[4096];

    while (!__atomic_load_n(&shared->quit_requested, __ATOMIC_ACQUIRE)) {
        if (!ds->connected) {
            sleep(shared->reconnect_delay);
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
        if (plen < 0) { ds->connected = 0; continue; }

        if (opcode == 0x9) { ws_send_pong(ds->ssl); continue; }
        if (opcode == 0x8) { ds->connected = 0; continue; }
        if (opcode != 0x1) continue;

        // parse into back buffer, swap atomically
        int back = 1 - __atomic_load_n(&shared->active_idx, __ATOMIC_ACQUIRE);
        shared->snapshots[back] = shared->snapshots[shared->active_idx];
        if (depth_parse_json<F>(frame_buf, plen, &shared->snapshots[back])) {
            __atomic_store_n(&shared->active_idx, back, __ATOMIC_RELEASE);
        }
    }

    if (ds->connected) ws_close(ds->ssl, ds->ssl_ctx, ds->sockfd);
    return NULL;
}

#endif // BINANCE_DEPTH_HPP
