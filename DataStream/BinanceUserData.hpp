// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BINANCE USER DATA STREAM]
//
// Websocket subscriber for Binance's user data stream. Receives real-time
// executionReport events (fills, rejections, cancellations) and pushes them
// into the OMS via a dedicated SPSC ring.
//
// Phase 04 of the OMS plan — see plans/oms/04_user_data_websocket/plan.md.
//
// Architecture:
//   Two threads spawned by BinanceUserData_Start:
//
//   1. WS thread — obtains a listen key via POST /api/v3/userDataStream,
//      connects WSS to /ws/<listenKey>, reads frames in a loop, parses
//      executionReport JSON, pushes CMD_WS_FILL into ws_result_queue.
//      On disconnect: sleep, re-obtain key, reconnect.
//
//   2. Keepalive thread — every 25 minutes, PUTs /api/v3/userDataStream
//      with the current listen key to refresh it (Binance expires at 60 min).
//      On failure: sets a flag that forces the WS thread to reconnect.
//
// REST calls use a dedicated BinanceOrderAPI instance (not shared with the
// adapter workers). Follows the established rule: per-thread instances,
// never share, never lock. The listen key REST calls need the X-MBX-APIKEY
// header but NOT HMAC signing (Binance docs: userDataStream endpoints are
// API-key-only, not signed).
//
// The ws_result_queue pointer is stored at init time. It points into the
// OMS's OrderManagerState. The WS thread is the sole producer; the drainer
// thread is the sole consumer via OrderManager_Tick. SPSC contract holds.
//
// Reuses the same SSL/WebSocket patterns from DataStream/BinanceCrypto.hpp
// (tcp_connect, tls_setup, ws_handshake, ws_read_frame, ws_send_pong).
// The functions are duplicated here as static inlines with a "ud_" prefix
// because BinanceCrypto.hpp's versions operate on BinanceStream* and have
// market-data-specific fields mixed in. The WS protocol code is ~120 lines
// and proven stable across months of live market data streaming.
//======================================================================================================

#pragma once

#include "../CoreFrameworks/ExchangeAdapter.hpp"
#include "../CoreFrameworks/SPSCRing.hpp"
#include "../CoreFrameworks/OrderManager.hpp"
#include "../CoreFrameworks/Notify.hpp"  // Phase 8b — disconnect alerts
#include "BinanceOrderAPI.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>

#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>

namespace tt {

//======================================================================================================
// [STATE]
//======================================================================================================
struct BinanceUserDataState {
    // Connection state (WS thread owned)
    int      sockfd;
    SSL_CTX* ssl_ctx;
    SSL*     ssl;
    char     listen_key[128];
    int      connected;

    // Credentials (for listen key REST calls — own instance, not shared)
    BinanceOrderAPI rest_api;
    char     ws_host[64];          // "stream.binance.vision" or testnet
    char     rest_host[64];        // "testnet.binance.vision" or "api.binance.us"

    // Output: SPSC ring for fill events → OMS drainer
    SPSCRing<Command, OMS_RESULT_QUEUE_SIZE>* ws_result_queue;

    // Threads
    std::thread ws_thread;
    std::thread keepalive_thread;
    std::atomic<int> shutdown_requested;
    std::atomic<int> keepalive_failed;  // WS thread checks this to force reconnect

    // Observability (atomic for TUI reads from render thread)
    std::atomic<int>      ws_connected;     // 1 when WSS is live
    std::atomic<uint64_t> fills_received;
    std::atomic<uint64_t> events_received;  // all events (fills + non-fills)
    std::atomic<uint64_t> reconnect_count;
};

//======================================================================================================
// [WS PROTOCOL HELPERS — duplicated from BinanceCrypto.hpp with ud_ prefix]
//======================================================================================================

static inline int ud_tcp_connect(const char* host, const char* port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(host, port, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "[UserData] getaddrinfo failed: %s\n", gai_strerror(err));
        return -1;
    }
    int sockfd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) continue;
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);
    if (sockfd == -1)
        fprintf(stderr, "[UserData] TCP connect failed to %s:%s\n", host, port);
    return sockfd;
}

static inline int ud_tls_setup(BinanceUserDataState* s, const char* host) {
    s->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!s->ssl_ctx) return 0;
    s->ssl = SSL_new(s->ssl_ctx);
    if (!s->ssl) { SSL_CTX_free(s->ssl_ctx); s->ssl_ctx = NULL; return 0; }
    SSL_set_fd(s->ssl, s->sockfd);
    SSL_set_tlsext_host_name(s->ssl, host);
    if (SSL_connect(s->ssl) != 1) {
        SSL_free(s->ssl); SSL_CTX_free(s->ssl_ctx);
        s->ssl = NULL; s->ssl_ctx = NULL;
        return 0;
    }
    return 1;
}

static inline int ud_ws_handshake(BinanceUserDataState* s, const char* path, const char* host) {
    unsigned char key_bytes[16];
    RAND_bytes(key_bytes, 16);
    // inline base64 for 16 bytes
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char key_b64[32];
    int i = 0, j = 0;
    while (i < 16) {
        uint32_t a = key_bytes[i++], b = (i<16)?key_bytes[i++]:0, c = (i<16)?key_bytes[i++]:0;
        uint32_t t = (a<<16)|(b<<8)|c;
        key_b64[j++]=b64[(t>>18)&0x3F]; key_b64[j++]=b64[(t>>12)&0x3F];
        key_b64[j++]=b64[(t>>6)&0x3F];  key_b64[j++]=b64[t&0x3F];
    }
    int pad = (3 - (16 % 3)) % 3;
    for (int p = 0; p < pad; p++) key_b64[j-1-p] = '=';
    key_b64[j] = '\0';

    char req[512];
    int len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n", path, host, key_b64);
    if (SSL_write(s->ssl, req, len) != len) return 0;
    char resp[1024]; int total = 0;
    while (total < (int)sizeof(resp)-1) {
        int n = SSL_read(s->ssl, resp+total, (int)sizeof(resp)-1-total);
        if (n <= 0) return 0;
        total += n; resp[total] = '\0';
        if (strstr(resp, "\r\n\r\n")) break;
    }
    return strstr(resp, "101") ? 1 : 0;
}

static inline int ud_ws_read_frame(BinanceUserDataState* s, char* buf, int buf_size, int* opcode) {
    unsigned char header[2];
    int hdr_read = 0;
    while (hdr_read < 2) {
        int n = SSL_read(s->ssl, header+hdr_read, 2-hdr_read);
        if (n <= 0) return -1;
        hdr_read += n;
    }
    *opcode = header[0] & 0x0F;
    int masked = (header[1]>>7)&1;
    uint64_t pay_len = header[1] & 0x7F;
    if (pay_len == 126) {
        unsigned char ext[2]; int r=0;
        while (r<2) { int n=SSL_read(s->ssl,ext+r,2-r); if(n<=0) return -1; r+=n; }
        pay_len = __builtin_bswap16(*(uint16_t*)ext);
    } else if (pay_len == 127) {
        unsigned char ext[8]; int r=0;
        while (r<8) { int n=SSL_read(s->ssl,ext+r,8-r); if(n<=0) return -1; r+=n; }
        pay_len = __builtin_bswap64(*(uint64_t*)ext);
    }
    unsigned char mask_key[4] = {};
    if (masked) {
        int r=0;
        while (r<4) { int n=SSL_read(s->ssl,mask_key+r,4-r); if(n<=0) return -1; r+=n; }
    }
    if ((int)pay_len > buf_size-1) return -1;
    int pr=0;
    while (pr<(int)pay_len) {
        int n=SSL_read(s->ssl, buf+pr, (int)pay_len-pr);
        if (n<=0) return -1; pr+=n;
    }
    if (masked) for (int i=0;i<(int)pay_len;i++) buf[i]^=mask_key[i&3];
    buf[pay_len] = '\0';
    return (int)pay_len;
}

static inline int ud_ws_send_pong(BinanceUserDataState* s, const char* payload, int len) {
    unsigned char frame[256];
    int pos = 0;
    frame[pos++] = 0x8A;
    if (len < 126) { frame[pos++] = 0x80|(unsigned char)len; }
    else { frame[pos++]=0x80|126; uint16_t be=__builtin_bswap16((uint16_t)len); memcpy(frame+pos,&be,2); pos+=2; }
    unsigned char mk[4]; RAND_bytes(mk,4); memcpy(frame+pos,mk,4); pos+=4;
    for (int i=0;i<len;i++) frame[pos++]=payload[i]^mk[i&3];
    return (SSL_write(s->ssl, frame, pos) == pos) ? 1 : 0;
}

static inline void ud_ws_close(BinanceUserDataState* s) {
    if (s->ssl) {
        unsigned char frame[8] = {0x88, 0x80, 0,0,0,0};
        RAND_bytes(frame+2, 4);
        SSL_write(s->ssl, frame, 6);
        SSL_shutdown(s->ssl); SSL_free(s->ssl); s->ssl = NULL;
    }
    if (s->ssl_ctx) { SSL_CTX_free(s->ssl_ctx); s->ssl_ctx = NULL; }
    if (s->sockfd >= 0) { close(s->sockfd); s->sockfd = -1; }
    s->connected = 0;
    s->ws_connected.store(0, std::memory_order_relaxed);
}

//======================================================================================================
// [LISTEN KEY MANAGEMENT — REST calls on dedicated instance]
//======================================================================================================

// POST /api/v3/userDataStream — creates a listen key. API-key-only, no HMAC.
// Retries with exponential backoff (1s, 2s, 4s, 8s, max 30s). Returns 1 on
// success (listen_key populated), 0 after all retries exhausted.
static inline int ud_obtain_listen_key(BinanceUserDataState* s) {
    int delays[] = {0, 1, 2, 4, 8, 15, 30};
    int max_attempts = 7;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        if (attempt > 0) {
            fprintf(stderr, "[UserData] listen key retry %d/%d after %ds\n",
                    attempt, max_attempts-1, delays[attempt]);
            for (int i = 0; i < delays[attempt]*10; ++i) {
                if (s->shutdown_requested.load(std::memory_order_acquire)) return 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        char body[512];
        int status = binance_rest_request(&s->rest_api, "POST",
                                           "/api/v3/userDataStream", "",
                                           body, sizeof(body));
        if (status == 200) {
            char key[128] = {};
            binance_json_extract_str(body, "listenKey", key, sizeof(key));
            if (key[0] != '\0') {
                strncpy(s->listen_key, key, sizeof(s->listen_key)-1);
                s->listen_key[sizeof(s->listen_key)-1] = '\0';
                fprintf(stderr, "[UserData] obtained listen key: %.20s...\n", s->listen_key);
                return 1;
            }
            fprintf(stderr, "[UserData] listen key empty in response: %s\n", body);
        } else {
            fprintf(stderr, "[UserData] listen key POST failed (status %d): %s\n", status, body);
        }
    }
    fprintf(stderr, "[UserData] CRITICAL: all listen key attempts exhausted\n");
    return 0;
}

// PUT /api/v3/userDataStream?listenKey=<key> — refresh keepalive.
static inline int ud_keepalive_listen_key(BinanceUserDataState* s) {
    char params[256];
    snprintf(params, sizeof(params), "listenKey=%s", s->listen_key);
    char body[512];
    int status = binance_rest_request(&s->rest_api, "PUT",
                                       "/api/v3/userDataStream", params,
                                       body, sizeof(body));
    if (status != 200) {
        fprintf(stderr, "[UserData] keepalive PUT failed (status %d): %s\n", status, body);
        return 0;
    }
    return 1;
}

//======================================================================================================
// [EXECUTION REPORT PARSER]
//======================================================================================================
// Extracts fill data from a Binance executionReport JSON event.
// Returns 1 if this is a fill event (x == "TRADE"), 0 otherwise.
//
// Relevant fields from the Binance docs:
//   "e": "executionReport"   — event type
//   "x": "TRADE"             — execution type (TRADE = fill)
//   "X": "FILLED"            — order status
//   "c": "oms_123"           — clientOrderId (our idempotency key)
//   "i": 12345               — exchange orderId
//   "L": "60123.45"          — last executed price
//   "l": "0.001"             — last executed quantity
//   "n": "0.06"              — commission amount
//   "N": "BNB"               — commission asset
//   "t": 98765               — trade ID (for deduplication)
//   "T": 1234567890123       — transaction time (ms)
//======================================================================================================
static inline int ud_parse_execution_report(const char* json, int len,
                                             Command* cmd_out,
                                             uint64_t* trade_id_out) {
    (void)len;
    // check event type
    char event_type[32] = {};
    binance_json_extract_str(json, "e", event_type, sizeof(event_type));
    if (strcmp(event_type, "executionReport") != 0) return 0;

    // check execution type — only "TRADE" is a fill
    char exec_type[16] = {};
    binance_json_extract_str(json, "x", exec_type, sizeof(exec_type));
    if (strcmp(exec_type, "TRADE") != 0) return 0;

    // extract clientOrderId — should be "oms_<id>"
    char client_oid[64] = {};
    binance_json_extract_str(json, "c", client_oid, sizeof(client_oid));
    uint64_t oms_order_id = 0;
    if (strncmp(client_oid, "oms_", 4) == 0) {
        oms_order_id = strtoull(client_oid + 4, NULL, 10);
    }

    // extract exchange orderId
    char exchange_oid[32] = {};
    binance_json_extract_str(json, "i", exchange_oid, sizeof(exchange_oid));

    // fill data
    double fill_price = binance_json_extract_double(json, "L");
    double fill_qty   = binance_json_extract_double(json, "l");
    *trade_id_out     = (uint64_t)binance_json_extract_double(json, "t");

    // Phase 8 — maker/taker + order status + commission.
    // "m": Binance encodes booleans as bare true / false in JSON. The
    // existing extract_str returns the literal text — we check the first char.
    // Defensive default: missing "m" → is_maker=0 (taker, slightly overstates
    // fees, conservative) per master plan.
    char m_str[8] = {};
    binance_json_extract_str(json, "m", m_str, sizeof(m_str));
    int is_maker = (m_str[0] == 't' || m_str[0] == 'T') ? 1 : 0;

    // "X": order status. "FILLED" → terminal; anything else (including
    // "PARTIALLY_FILLED") is non-terminal. Defensive default: missing "X"
    // → order_complete=0 (assume partial — keeps order alive in OMS,
    // worst case we wait for next event to confirm).
    char order_status[24] = {};
    binance_json_extract_str(json, "X", order_status, sizeof(order_status));
    int order_complete = (strcmp(order_status, "FILLED") == 0) ? 1 : 0;

    // Commission: "n" amount + "N" asset. Recorded for audit; not the
    // authoritative fee number (Fee_Compute computes from cfg rates).
    double commission_amt = binance_json_extract_double(json, "n");
    char comm_asset[8] = {};
    binance_json_extract_str(json, "N", comm_asset, sizeof(comm_asset));

    // build the Command
    memset(cmd_out, 0, sizeof(*cmd_out));
    cmd_out->type     = CMD_WS_FILL;
    cmd_out->order_id = oms_order_id;
    cmd_out->result.success        = 1;
    cmd_out->result.avg_fill_price = fill_price;
    cmd_out->result.fill_qty       = fill_qty;
    cmd_out->result.error_code     = 0;
    strncpy(cmd_out->result.exchange_id, exchange_oid,
            sizeof(cmd_out->result.exchange_id) - 1);
    // Phase 8 fields
    cmd_out->result.is_maker       = (uint8_t)is_maker;
    cmd_out->result.order_complete = (uint8_t)order_complete;
    cmd_out->result.commission     = commission_amt;
    strncpy(cmd_out->result.commission_asset, comm_asset,
            sizeof(cmd_out->result.commission_asset) - 1);

    return 1;
}

//======================================================================================================
// [WS THREAD BODY]
//======================================================================================================
static inline void ud_ws_thread(BinanceUserDataState* s) {
    while (s->shutdown_requested.load(std::memory_order_acquire) == 0) {
        // 1. Obtain listen key
        if (!ud_obtain_listen_key(s)) {
            fprintf(stderr, "[UserData] failed to obtain listen key, retrying in 5s\n");
            for (int i = 0; i < 50 && !s->shutdown_requested.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        s->keepalive_failed.store(0, std::memory_order_relaxed);

        // 2. Connect WSS
        s->sockfd = ud_tcp_connect(s->ws_host, "443");
        if (s->sockfd < 0) {
            fprintf(stderr, "[UserData] TCP connect failed, retrying in 5s\n");
            for (int i = 0; i < 50 && !s->shutdown_requested.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (!ud_tls_setup(s, s->ws_host)) {
            fprintf(stderr, "[UserData] TLS setup failed, retrying in 5s\n");
            close(s->sockfd); s->sockfd = -1;
            for (int i = 0; i < 50 && !s->shutdown_requested.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        char ws_path[256];
        snprintf(ws_path, sizeof(ws_path), "/ws/%s", s->listen_key);
        if (!ud_ws_handshake(s, ws_path, s->ws_host)) {
            fprintf(stderr, "[UserData] WS handshake failed, retrying in 5s\n");
            ud_ws_close(s);
            for (int i = 0; i < 50 && !s->shutdown_requested.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        s->connected = 1;
        s->ws_connected.store(1, std::memory_order_relaxed);
        s->reconnect_count.fetch_add(1, std::memory_order_relaxed);
        fprintf(stderr, "[UserData] WSS connected to %s%s\n", s->ws_host, ws_path);

        // 3. Read loop
        char frame_buf[4096];
        while (s->shutdown_requested.load(std::memory_order_acquire) == 0) {
            // check if keepalive failed (forces reconnect)
            if (s->keepalive_failed.load(std::memory_order_relaxed)) {
                fprintf(stderr, "[UserData] keepalive failed, reconnecting\n");
                break;
            }

            // check if data is available (poll with 200ms timeout so we
            // can check shutdown_requested periodically)
            // IMPORTANT: check SSL_pending first — OpenSSL may have buffered
            // decrypted data that poll() won't see (same pattern as
            // BinanceStream_Poll in BinanceCrypto.hpp).
            if (SSL_pending(s->ssl) == 0) {
                struct pollfd pfd;
                pfd.fd = s->sockfd;
                pfd.events = POLLIN;
                int ready = poll(&pfd, 1, 200);
                if (ready <= 0) continue;
                if (pfd.revents & (POLLERR | POLLHUP)) {
                    fprintf(stderr, "[UserData] socket error/hangup\n");
                    break;
                }
            }

            int opcode = 0;
            int payload_len = ud_ws_read_frame(s, frame_buf, sizeof(frame_buf), &opcode);
            if (payload_len < 0) {
                fprintf(stderr, "[UserData] frame read error, reconnecting\n");
                break;
            }

            // ping → pong
            if (opcode == 0x9) {
                ud_ws_send_pong(s, frame_buf, payload_len);
                continue;
            }
            // close frame
            if (opcode == 0x8) {
                fprintf(stderr, "[UserData] server sent close frame\n");
                break;
            }
            // text frame (0x1) — JSON event
            if (opcode != 0x1) continue;

            s->events_received.fetch_add(1, std::memory_order_relaxed);

            Command cmd;
            uint64_t trade_id = 0;
            if (ud_parse_execution_report(frame_buf, payload_len, &cmd, &trade_id)) {
                if (!SPSCRing_TryPush(s->ws_result_queue, cmd)) {
                    fprintf(stderr, "[UserData] ws_result_queue full, dropping fill "
                                     "for order %llu\n",
                                     (unsigned long long)cmd.order_id);
                } else {
                    s->fills_received.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // disconnected — clean up and loop back
        ud_ws_close(s);
        if (s->shutdown_requested.load(std::memory_order_acquire)) break;

        fprintf(stderr, "[UserData] disconnected, reconnecting in 2s\n");
        // Phase 8b: alert. Cooldown collapses the keepalive/frame-read/disconnect
        // log triplet into one alert per cooldown window — this is the convergence
        // point that always runs on disconnect, so a single Send here is enough.
        if (g_notify) {
            Notify_Send(g_notify, NOTIFY_WARN, NK_DISCONNECT_USERDATA,
                        "Binance user-data WS disconnected",
                        "Reconnecting automatically in 2 seconds. "
                        "Investigate if disconnects are frequent or persistent.");
        }
        for (int i = 0; i < 20 && !s->shutdown_requested.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

//======================================================================================================
// [KEEPALIVE THREAD BODY]
//======================================================================================================
// Refreshes the listen key every 25 minutes (Binance expires at 60 min).
// On failure, sets keepalive_failed so the WS thread reconnects with a
// fresh key.
//======================================================================================================
static inline void ud_keepalive_thread(BinanceUserDataState* s) {
    int consecutive_failures = 0;
    while (s->shutdown_requested.load(std::memory_order_acquire) == 0) {
        // sleep 25 minutes, checking shutdown every 100ms
        for (int i = 0; i < 15000; ++i) {
            if (s->shutdown_requested.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (s->listen_key[0] == '\0') continue;
        if (!s->ws_connected.load(std::memory_order_relaxed)) {
            consecutive_failures = 0;  // reset on disconnect (WS thread reconnects)
            continue;
        }

        // Try keepalive PUT
        if (ud_keepalive_listen_key(s)) {
            fprintf(stderr, "[UserData] listen key refreshed\n");
            consecutive_failures = 0;
            // Sync clock every keepalive cycle (~25 min) to prevent
            // timestamp drift on signed requests.
            BinanceOrderAPI_SyncClock(&s->rest_api);
            continue;
        }

        // First failure: retry once after 5s
        fprintf(stderr, "[UserData] keepalive failed, retrying in 5s\n");
        for (int i = 0; i < 50; ++i) {
            if (s->shutdown_requested.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (ud_keepalive_listen_key(s)) {
            fprintf(stderr, "[UserData] keepalive retry succeeded\n");
            consecutive_failures = 0;
            continue;
        }

        // Second failure: signal WS thread to reconnect (which recreates the key)
        consecutive_failures++;
        fprintf(stderr, "[UserData] keepalive retry failed (%d consecutive), "
                         "signaling reconnect\n", consecutive_failures);
        s->keepalive_failed.store(1, std::memory_order_relaxed);

        // Circuit breaker: after 5 consecutive failures across reconnects,
        // something is fundamentally wrong (API key revoked, network down,
        // Binance outage). Stop trying.
        if (consecutive_failures >= 5) {
            fprintf(stderr, "[UserData] CRITICAL: 5 consecutive keepalive failures, "
                             "circuit breaker tripped — WS fills disabled\n");
            s->shutdown_requested.store(1, std::memory_order_release);
            return;
        }
    }
}

//======================================================================================================
// [INIT]
//======================================================================================================
// Initialize the user data stream state. Does NOT connect — call
// BinanceUserData_Start to spawn the threads and begin streaming.
//
// ws_host: the websocket host ("stream.binance.vision" for testnet,
//          "stream.binance.com" for production)
// rest_host: the REST host for listen key calls (same as adapter REST host)
// ws_result_queue: pointer into the OMS's dedicated WS SPSC ring
//======================================================================================================
static inline int BinanceUserData_Init(BinanceUserDataState* s,
                                        const char* ws_host,
                                        const char* rest_host,
                                        const char* api_key,
                                        const char* api_secret,
                                        const char* symbol,
                                        SPSCRing<Command, OMS_RESULT_QUEUE_SIZE>* ws_result_queue) {
    s->sockfd    = -1;
    s->ssl_ctx   = NULL;
    s->ssl       = NULL;
    s->connected = 0;
    s->listen_key[0] = '\0';
    strncpy(s->ws_host, ws_host, sizeof(s->ws_host)-1);
    s->ws_host[sizeof(s->ws_host)-1] = '\0';
    strncpy(s->rest_host, rest_host, sizeof(s->rest_host)-1);
    s->rest_host[sizeof(s->rest_host)-1] = '\0';
    s->ws_result_queue = ws_result_queue;

    s->shutdown_requested.store(0, std::memory_order_relaxed);
    s->keepalive_failed.store(0, std::memory_order_relaxed);
    s->ws_connected.store(0, std::memory_order_relaxed);
    s->fills_received.store(0, std::memory_order_relaxed);
    s->events_received.store(0, std::memory_order_relaxed);
    s->reconnect_count.store(0, std::memory_order_relaxed);

    // Initialize the dedicated REST API instance for listen key management.
    // This instance is ONLY used by the WS thread and keepalive thread
    // (serialized — WS thread only does REST during reconnect, keepalive
    // only fires when WS is connected). No concurrent use.
    if (!BinanceOrderAPI_Init(&s->rest_api, rest_host, api_key, api_secret, symbol)) {
        fprintf(stderr, "[UserData] failed to init REST API instance\n");
        return 0;
    }
    return 1;
}

//======================================================================================================
// [START]
//======================================================================================================
static inline void BinanceUserData_Start(BinanceUserDataState* s) {
    s->ws_thread        = std::thread(ud_ws_thread, s);
    s->keepalive_thread = std::thread(ud_keepalive_thread, s);
    fprintf(stderr, "[UserData] WS + keepalive threads started\n");
}

//======================================================================================================
// [SHUTDOWN]
//======================================================================================================
static inline void BinanceUserData_Shutdown(BinanceUserDataState* s) {
    s->shutdown_requested.store(1, std::memory_order_release);
    if (s->ws_thread.joinable())        s->ws_thread.join();
    if (s->keepalive_thread.joinable()) s->keepalive_thread.join();
    ud_ws_close(s);
    BinanceOrderAPI_Cleanup(&s->rest_api);
    fprintf(stderr, "[UserData] shutdown complete (fills=%llu events=%llu reconnects=%llu)\n",
            (unsigned long long)s->fills_received.load(),
            (unsigned long long)s->events_received.load(),
            (unsigned long long)s->reconnect_count.load());
}

}  // namespace tt
