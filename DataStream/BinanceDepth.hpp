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
//   - [FUNCTION]_[depth_thread_fn]   (+ DepthShared_Configure / DepthStream_Connect / DepthStream_Disconnect / depth_consume_frame / depth_liveness_check / depth_backoff_s ride; the DepthRecorder include-cycle break lives here)
//======================================================================================================
// subscribes to Binance @depth5@100ms websocket for top-of-book bid/ask data
// runs on its own thread, writes to double-buffered BookSnapshot
// engine reads snapshot on slow path — zero hot-path impact
//
// uses the shared WebSocketUtil.hpp frame family (TCP / SSL / handshake / reader / pong / close) — the ONE
// RFC 6455 client body; BinanceCrypto + BinanceUserData migrate onto it in the 2026-09-05 depth leaf
//======================================================================================================
#ifndef BINANCE_DEPTH_HPP
#define BINANCE_DEPTH_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include "WebSocketUtil.hpp"
#include "DepthGapReasons.hpp"          // the "# GAP" reason vocabulary the ONE Disconnect names (include-order-proof)
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
// [UPDATED]_[2026-08-10]
// [SIZE]_[416B]
// [ALIGN]_[16]
// [CACHE_LINES]_[7]
// [STRADDLE]_[unverified: bids asks]
//======================================================================
// [END_STRUCT]_[BookSnapshot]
//======================================================================

//======================================================================
// [STRUCT]_[DepthStream]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the connection handle — socket + SSL pair + connected flag + pollfd + the thread-private liveness stamps (connect base / last frame / attempts / failing step) — the 2026-09-05 depth leaf]
//======================================================================
// [CODE]
//======================================================================
struct DepthStream {
    int sockfd;                    // -1 when closed (never 0: fd 0 is stdin)
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    int connected;
    struct pollfd pfd;
    // Liveness — depth-thread-PRIVATE (the watchdog runs on the thread that writes them; no cross-thread claim).
    uint64_t connect_mono_s;       // CLOCK_MONOTONIC s at the last successful handshake — the planned-reconnect base
    uint64_t last_frame_mono_us;   // CLOCK_MONOTONIC µs of the last well-formed frame (a control frame counts); Connect stamps it
    uint32_t reconnect_attempts;   // consecutive failed connects = the backoff index; 0 after a success
    uint32_t last_connect_step;    // 0 ok · 1 tcp · 2 tls · 3 handshake — where the last attempt failed
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-09-05]
// [SIZE]_[64B]
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
// [THREAD]_[[DEPTH_WS_WRITER] [PRODUCER_READER]]
// [STRADDLE_EXEMPT]_[symbol]_[init-only symbol string — written once at boot, read-shared thereafter — D-414 leaf-3 2026-08-10]
// [STRADDLE_EXEMPT]_[snapshots]_[double-buffered BookSnapshot pair — access mediated by the atomic active_idx publish protocol (writer touches only the inactive buffer); element-uniform — D-414 leaf-3 2026-08-10]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[engine reads / depth thread writes — double-buffered snapshots + atomic active_idx + quit flag + boot-only connection cfg (DepthShared_Configure; the per-attempt DepthStream_Connect takes NO pointer to this struct — the 2026-09-05 leaf's type-level tooth) + nullable recorder]
// [INSTANTIATION]_[[64]]
//======================================================================
// [CODE]
//======================================================================
struct DepthRecorder;  // fwd decl for the recorder pointer field — full definition in DepthRecorder.hpp,
                       // included near the bottom of this header (just before depth_thread_fn)
                       // so it can call the templated DepthRecorder_Write

template <unsigned F> struct DepthSharedState {
    BookSnapshot<F> snapshots[2];
    DepthStream stream;          // 64 B, depth-thread-private; sits right after the 16-aligned 832 B pair = its own line
    int active_idx;              // atomic: index the engine reads
    int quit_requested;          // atomic: signal thread to stop
    char symbol[32];
    char host[128];
    int port;
    DepthRecorder *recorder;     // null = recording disabled (Phase 8a c5)
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-09-05]
// [SIZE]_[1088B]
// [ALIGN]_[16]
// [CACHE_LINES]_[17]
// [STRADDLE]_[unverified: snapshots]
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
// [OVERVIEW]_[the depth thread + its lifecycle family (DepthShared_Configure boot-only · DepthStream_Connect per attempt, no shared pointer · the ONE DepthStream_Disconnect: close → "# GAP" → Notify → ZERO-publish · depth_consume_frame over the Io seam · depth_liveness_check on the poll-timeout path · depth_backoff_s ride) — bounded-backoff reconnect loop, stale watchdog + the 23h30m planned reconnect, back-buffer parse + RELEASE active_idx swap, recorder write; the 2026-09-05 depth-stall leaf (D-487)]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[THE DepthRecorder INCLUDE — the cycle break]
//------------------------------------------------------------------
// DepthRecorder.hpp includes this header for BookSnapshot<F>. Including it
// HERE (after BookSnapshot + DepthSharedState are fully defined, before
// depth_thread_fn) breaks the include cycle: the include guard short-circuits
// the inner BinanceDepth.hpp include in DepthRecorder.hpp, but BookSnapshot
// is already in scope so DepthRecorder_Write's template body resolves.
#include "DepthRecorder.hpp"

//------------------------------------------------------------------
// [SECTION]_[LIVENESS CONSTANTS + BACKOFF]
//------------------------------------------------------------------
// D-487 call 2: named constants, not cfg rows (promote if a second depth cadence ever ships).
static const uint64_t DEPTH_STALE_THRESHOLD_US = 10ULL * 1000000ULL;   // >= 100 missed frames on the 100 ms stream
static const uint32_t DEPTH_RCV_TIMEOUT_MS     = 10000;                 // SO_RCVTIMEO = the same 10 s: a wedge INSIDE SSL_read returns
static const uint32_t DEPTH_BACKOFF_MIN_S      = 2;
static const uint32_t DEPTH_BACKOFF_MAX_S      = 30;
static const uint32_t DEPTH_LOUD_FIRST         = 3;                     // stderr on the first three failed attempts …
static const uint32_t DEPTH_LOUD_EVERY         = 30;                    // … then every 30th (Notify keeps its own 60 s cooldown)

// Bounded exponential backoff: attempt 0 (the first) is immediate; then 2, 4, 8, 16, 30, 30, … seconds.
static inline uint32_t depth_backoff_s(uint32_t attempts) {
    if (attempts == 0) return 0;
    const uint32_t shift = (attempts - 1 < 4) ? (attempts - 1) : 4;
    const uint32_t s = DEPTH_BACKOFF_MIN_S << shift;
    return (s < DEPTH_BACKOFF_MAX_S) ? s : DEPTH_BACKOFF_MAX_S;
}

static inline uint64_t depth_wall_us(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
static inline uint64_t depth_mono_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

//------------------------------------------------------------------
// [SECTION]_[BOOT-ONLY CONFIGURE]
//------------------------------------------------------------------
// Called ONCE before the thread is spawned. The strings are copied into zeroed LOCALS first, so a caller
// passing the struct's own fields (the pre-2026-09-05 reconnect arm did exactly that) cannot hit glibc's
// overlapping-snprintf self-copy, which yields an EMPTY string. The per-attempt Connect cannot reach any
// of these fields by construction (it takes no DepthSharedState*). The recorder pointer is the caller's.
template <unsigned F>
static inline void DepthShared_Configure(DepthSharedState<F> *shared, const char *symbol,
                                         const char *host, int port) {
    char sym_local[sizeof(shared->symbol)];  memset(sym_local,  0, sizeof(sym_local));
    char host_local[sizeof(shared->host)];   memset(host_local, 0, sizeof(host_local));
    snprintf(sym_local,  sizeof(sym_local),  "%s", symbol ? symbol : "");
    snprintf(host_local, sizeof(host_local), "%s", host   ? host   : "");
    memcpy(shared->symbol, sym_local,  sizeof(shared->symbol));
    memcpy(shared->host,   host_local, sizeof(shared->host));
    shared->port = port;
    shared->active_idx = 0;
    shared->quit_requested = 0;
    shared->snapshots[0] = BookSnapshot_Init<F>();
    shared->snapshots[1] = BookSnapshot_Init<F>();
    memset(&shared->stream, 0, sizeof(DepthStream));
    shared->stream.sockfd = -1;
    shared->stream.pfd.fd = -1;
}

//------------------------------------------------------------------
// [SECTION]_[PER-ATTEMPT CONNECT]
//------------------------------------------------------------------
// Returns 0 on success, else the STEP that failed: 1 = tcp, 2 = tls, 3 = handshake. Every partial resource
// is released on failure (sockfd = -1, ssl = ctx = NULL). Takes NO DepthSharedState*: the shared strings,
// the quit flag and the snapshot buffers are UNREACHABLE from here — the type-level tooth for the
// re-init-under-a-live-reader defect (-Werror=restrict stayed silent on the real self-copy, Class 51).
// Stamps last_frame_mono_us at the handshake so staleness counts from the CONNECT: a session that
// handshakes and never sends a frame trips the watchdog too. SO_RCVTIMEO bounds the handshake's reads as well.
static inline int DepthStream_Connect(DepthStream *ds, const char *host, int port, const char *symbol) {
    ds->connected = 0;
    ds->ssl = NULL; ds->ssl_ctx = NULL;
    ds->sockfd = ws_tcp_connect(host, port, DEPTH_RCV_TIMEOUT_MS);
    if (ds->sockfd < 0) { ds->sockfd = -1; ds->last_connect_step = 1; return 1; }
    if (ws_ssl_setup(&ds->ssl_ctx, &ds->ssl, ds->sockfd, host) < 0) {   // frees its own partial ssl/ctx
        close(ds->sockfd); ds->sockfd = -1; ds->ssl = NULL; ds->ssl_ctx = NULL;
        ds->last_connect_step = 2; return 2;
    }
    char path[160];
    snprintf(path, sizeof(path), "/ws/%s@depth5@100ms", symbol);
    WsSslIo hio{ds->ssl};
    if (ws_handshake(hio, host, path) < 0) {
        ws_close(ds->ssl, ds->ssl_ctx, ds->sockfd);
        ds->sockfd = -1; ds->ssl = NULL; ds->ssl_ctx = NULL;
        ds->last_connect_step = 3; return 3;
    }
    ds->connected = 1;
    ds->pfd.fd = ds->sockfd;
    ds->pfd.events = POLLIN;
    ds->pfd.revents = 0;
    const uint64_t now_us = depth_mono_us();
    ds->connect_mono_s = now_us / 1000000ULL;
    ds->last_frame_mono_us = now_us;
    ds->last_connect_step = 0;
    return 0;
}

//------------------------------------------------------------------
// [SECTION]_[THE ONE DISCONNECT]
//------------------------------------------------------------------
// Every arm converges here: close the socket → ONE "# GAP" line (reason = the H21 string-const) → Notify
// (its 60 s per-kind cooldown collapses storms) → ZERO-publish through the back-buffer RELEASE swap so the
// slow path's book_imbalance gate fails CLOSED instead of reading a book from before the disconnect (the
// 2026-09-05 session served a 13:29 book as live for 40+ min). The recorder is BYPASSED for the zero
// snapshot — the gap line is the record; a zero row would poison replay. update_count is carried (monotonic).
template <unsigned F>
static inline void DepthStream_Disconnect(DepthSharedState<F> *shared, const char *reason, uint64_t wall_now_us) {
    DepthStream *ds = &shared->stream;
    if (ds->ssl) ws_close(ds->ssl, ds->ssl_ctx, ds->sockfd);
    else if (ds->sockfd >= 0) close(ds->sockfd);
    ds->ssl = NULL; ds->ssl_ctx = NULL; ds->sockfd = -1; ds->pfd.fd = -1;
    ds->connected = 0;

    if (shared->recorder) DepthRecorder_LogGap(shared->recorder, wall_now_us, reason);

    if (g_notify) {
        char body[256];
        snprintf(body, sizeof(body),
                 "reason=%s. The book_imbalance gate reads a ZERO book (fails closed) until the reconnect "
                 "succeeds. Check if frequent or persistent.", reason);
        Notify_Send(g_notify, NOTIFY_WARN, NK_DISCONNECT_DEPTH, "Binance depth WS disconnected", body);
    }

    const int active = __atomic_load_n(&shared->active_idx, __ATOMIC_ACQUIRE);
    const int back = 1 - active;
    shared->snapshots[back] = BookSnapshot_Init<F>();
    shared->snapshots[back].update_count = shared->snapshots[active].update_count;
    shared->snapshots[back].timestamp_us = wall_now_us;
    __atomic_store_n(&shared->active_idx, back, __ATOMIC_RELEASE);
    fprintf(stderr, "[depth] disconnected (reason=%s)\n", reason);
}

//------------------------------------------------------------------
// [SECTION]_[ONE FRAME]
//------------------------------------------------------------------
// The per-frame body, templated over the Io transport so the suite drives it through a byte cursor with the
// SSL* NULL (WsSslIo in production). Returns 1 = still connected, 0 = disconnected (the arm ran).
template <unsigned F, class Io>
static inline int depth_consume_frame(DepthSharedState<F> *shared, Io &io, char *frame_buf, int frame_cap,
                                      uint64_t wall_now_us, uint64_t mono_now_us) {
    DepthStream *ds = &shared->stream;
    int opcode = 0, fin = 0;
    const int plen = ws_read_frame(io, frame_buf, frame_cap, &opcode, &fin);
    if (plen == WS_READ_TOO_LARGE) {
        // The stream is DESYNCED (the oversize payload was not consumed) — never "skip and continue".
        fprintf(stderr, "[depth] oversize frame (> %d B) — disconnecting\n", frame_cap);
        DepthStream_Disconnect(shared, DEPTH_GAP_REASON_FRAME_TOO_LARGE, wall_now_us);
        return 0;
    }
    if (plen == WS_READ_PROTOCOL) {
        // RFC 6455 §5.5 violated (an oversize or fragmented control frame). Same desync contract as
        // TOO_LARGE. Logged distinctly because the wire cause differs from a plain EOF — the reason
        // code stays DISCONNECT so no new persisted identifier is minted for a peer-broken case (H21).
        fprintf(stderr, "[depth] protocol violation: control frame > %d B or fragmented — disconnecting\n",
                WS_CONTROL_MAX_PAYLOAD);
        DepthStream_Disconnect(shared, DEPTH_GAP_REASON_DISCONNECT, wall_now_us);
        return 0;
    }
    if (plen < 0) {                         // WS_READ_ERR: EOF / transport error / SO_RCVTIMEO inside a frame
        DepthStream_Disconnect(shared, DEPTH_GAP_REASON_DISCONNECT, wall_now_us);
        return 0;
    }
    ds->last_frame_mono_us = mono_now_us;   // any well-formed frame is the peer alive (a ping counts)

    if (opcode == 0x9) {
        // The pong ECHOES the ping payload, MASKED (RFC 6455 §5.1 + §5.5.3) — the 2026-09-05 fix. A failed
        // write = the peer is gone.
        if (!ws_send_pong(io, frame_buf, plen)) {
            DepthStream_Disconnect(shared, DEPTH_GAP_REASON_DISCONNECT, wall_now_us);
            return 0;
        }
        return 1;
    }
    if (opcode == 0x8) {
        // The server's close: a 2-byte big-endian code + a UTF-8 reason. Binance's 1008 "Pong timeout" was
        // INVISIBLE before this line — the old arm was silent.
        const int code = (plen >= 2) ? ((((unsigned char)frame_buf[0]) << 8) | (unsigned char)frame_buf[1]) : 0;
        fprintf(stderr, "[depth] server close code=%d reason=\"%.*s\"\n",
                code, (plen > 2) ? plen - 2 : 0, (plen > 2) ? frame_buf + 2 : "");
        DepthStream_Disconnect(shared, DEPTH_GAP_REASON_DISCONNECT, wall_now_us);
        return 0;
    }
    if (opcode != 0x1) return 1;            // binary / pong / continuation: ignored (liveness stamped above)

    // parse into the back buffer, publish with a RELEASE swap (the pre-leaf body, unchanged)
    const int active = __atomic_load_n(&shared->active_idx, __ATOMIC_ACQUIRE);
    const int back = 1 - active;
    shared->snapshots[back] = shared->snapshots[active];
    if (depth_parse_json<F>(frame_buf, plen, &shared->snapshots[back])) {
        // CLOCK_REALTIME landing time — the recorder's gap thresholds are wallclock (>2 s silence = a real gap).
        shared->snapshots[back].timestamp_us = wall_now_us;
        __atomic_store_n(&shared->active_idx, back, __ATOMIC_RELEASE);
        if (shared->recorder) DepthRecorder_Write(shared->recorder, &shared->snapshots[back]);
    }
    return 1;
}

//------------------------------------------------------------------
// [SECTION]_[LIVENESS CHECK — the poll-timeout path, never tick-gated]
//------------------------------------------------------------------
// Runs on the 200 ms idle branch (a stalled stream never ticks — the TD-338 trap). Returns 1 = fine, 0 = disconnected.
template <unsigned F>
static inline int depth_liveness_check(DepthSharedState<F> *shared, uint64_t wall_now_us, uint64_t mono_now_us) {
    DepthStream *ds = &shared->stream;
    if (!ds->connected) return 1;
    if (ws_stale(mono_now_us, ds->last_frame_mono_us, DEPTH_STALE_THRESHOLD_US)) {
        fprintf(stderr, "[depth] no frame for %llu ms — disconnecting (stale)\n",
                (unsigned long long)((mono_now_us - ds->last_frame_mono_us) / 1000ULL));
        DepthStream_Disconnect(shared, DEPTH_GAP_REASON_STALE, wall_now_us);
        return 0;
    }
    if (ws_planned_reconnect_due(mono_now_us / 1000000ULL, ds->connect_mono_s)) {
        fprintf(stderr, "[depth] planned reconnect at 23h30m of session\n");
        DepthStream_Disconnect(shared, DEPTH_GAP_REASON_PLANNED_RECONNECT, wall_now_us);
        return 0;
    }
    return 1;
}

//------------------------------------------------------------------
// [SECTION]_[THREAD FUNCTION]
//------------------------------------------------------------------
template <unsigned F>
static inline void *depth_thread_fn(void *arg) {
    DepthSharedState<F> *shared = (DepthSharedState<F> *)arg;
    DepthStream *ds = &shared->stream;
    char frame_buf[4096];
    pthread_setname_np(pthread_self(), "depth-ws");   // `ps -L` / `top -H` name; 15 chars max

    while (!__atomic_load_n(&shared->quit_requested, __ATOMIC_ACQUIRE)) {
        if (!ds->connected) {
            // Bounded backoff (the first attempt is immediate); interruptible — checks quit_requested every
            // 100 ms so engine shutdown is never blocked behind a reconnect wait.
            const uint32_t wait_s = depth_backoff_s(ds->reconnect_attempts);
            for (uint32_t s = 0; s < wait_s; ++s) {
                for (int j = 0; j < 10; ++j) {
                    if (__atomic_load_n(&shared->quit_requested, __ATOMIC_ACQUIRE)) return NULL;
                    struct timespec ts = {0, 100000000};
                    nanosleep(&ts, NULL);
                }
            }
            const int step = DepthStream_Connect(ds, shared->host, shared->port, shared->symbol);
            if (step != 0) {
                ds->reconnect_attempts++;
                const uint32_t n = ds->reconnect_attempts;
                if (n <= DEPTH_LOUD_FIRST || (n % DEPTH_LOUD_EVERY) == 0) {
                    static const char *const step_name[4] = {"ok", "tcp", "tls", "handshake"};
                    fprintf(stderr, "[depth] connect attempt %u failed at step %d (%s) — next in %u s\n",
                            n, step, step_name[step & 3], depth_backoff_s(n));
                }
                continue;
            }
            ds->reconnect_attempts = 0;
            fprintf(stderr, "[depth] connected (%s:%d %s@depth5@100ms)\n", shared->host, shared->port, shared->symbol);
        }

        // SSL_pending first (a TLS record can carry more than one frame), then poll with a 200 ms timeout.
        int ready = SSL_pending(ds->ssl) > 0;
        if (!ready) {
            ds->pfd.revents = 0;
            const int ret = poll(&ds->pfd, 1, 200);
            ready = (ret > 0 && (ds->pfd.revents & (POLLIN | POLLHUP | POLLERR)));   // a FIN/RST is readable too
        }
        const uint64_t mono_now = depth_mono_us();
        const uint64_t wall_now = depth_wall_us();
        if (!ready) { depth_liveness_check(shared, wall_now, mono_now); continue; }

        WsSslIo io{ds->ssl};
        depth_consume_frame(shared, io, frame_buf, (int)sizeof(frame_buf) - 1, wall_now, mono_now);
    }

    // Quiet shutdown: no gap line / Notify / zero-publish — the slow paths are joined right after this thread.
    if (ds->connected) {
        ws_close(ds->ssl, ds->ssl_ctx, ds->sockfd);
        ds->connected = 0; ds->sockfd = -1; ds->ssl = NULL; ds->ssl_ctx = NULL;
    }
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[depth_thread_fn]
//======================================================================

#endif // BINANCE_DEPTH_HPP
