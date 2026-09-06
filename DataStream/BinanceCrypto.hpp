// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[DataStream/BinanceCrypto.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the @trade market-data stream — full TCP -> TLS -> WebSocket -> JSON -> FPN stack producing DataStream<F>; SSL_pending-first poll; 24h session lifecycle; public endpoint (no key)]
// [CONTAINS]
//   - [STRUCT]_[BinanceConfig]   (POLL_* flags ride)
//   - [STRUCT]_[BinanceStream]
//   - [FUNCTION]_[binance_parse_trade]
//   - [FUNCTION]_[BinanceStream_Init]   (+ Close / Reconnect + the g_binance_shutdown_flag hook)
//   - [FUNCTION]_[BinanceStream_Poll]
//   - [FUNCTION]_[BinanceStream_ReadTick]
//   - [FUNCTION]_[BinanceStream_InWindDown]   (+ ShouldReconnect / HasPending session family)
//   - [FUNCTION]_[BinanceConfig_Load]   (+ cfg selector / fault flag / config_ok ride)
//======================================================================================================
// connects to Binance trade websocket (wss://stream.binance.com:9443/ws/<symbol>@trade)
// and produces DataStream<F> structs - same interface the pipeline already consumes
//
// no API key needed for market data - public endpoint, read-only
// handles the full network stack: TCP -> TLS -> WebSocket -> JSON -> FPN_Binary
//
// the main loop calls BinanceStream_Poll to check for data, then BinanceStream_ReadTick
// to consume one frame. poll checks SSL_pending FIRST to avoid the SSL-internal-buffer
// vs poll() mismatch - if SSL has buffered data, we return immediately without calling poll()
//
// ping/pong handled transparently inside BinanceStream_ReadTick's frame loop - binance sends a
// ping every ~3 min and kills the session (1008 "Pong timeout") if the masked payload echo does
// not come back; the reader returns the opcode, the loop answers it. Invisible to the caller.
//
// 24-hour session lifecycle: connect -> warmup -> trade -> wind down -> close all -> reconnect
// no positions carry across sessions
//======================================================================================================
#ifndef BINANCE_CRYPTO_HPP
#define BINANCE_CRYPTO_HPP

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
// netdb.h / netinet/tcp.h / errno.h left with the connect layer when it moved to WebSocketUtil.hpp
// (leaf commit 3) — getaddrinfo, TCP_NODELAY and the setsockopt strerror all live there now.
#include <poll.h>
#include <time.h>
#include <csignal>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/bio.h>
#include <openssl/evp.h>

#include "WebSocketUtil.hpp"          // the ONE RFC 6455 frame family (PARITY-071 / D-487)
#include "../FixedPoint/FixedPointN.hpp"
#include "../CoreFrameworks/OrderGates.hpp"
#include "../CoreFrameworks/Notify.hpp"  // Phase 8b — disconnect alerts
#include "../CoreFrameworks/ParseFast.hpp"  // v5.11.4.A — std::from_chars wrapper

using namespace std;

//======================================================================
// [STRUCT]_[BinanceConfig]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CFG_FLOW]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[venue selection + poll/reconnect/wind-down knobs + the N1 malformed-venue fault flag (POLL_* result flags ride); binance_config_ok() gates boot]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[POLL FLAGS]
//------------------------------------------------------------------
#define POLL_NONE   0
#define POLL_SOCKET 1
#define POLL_STDIN  2

struct BinanceConfig {
    char symbol[32];            // e.g. "btcusdt" (lowercase)
    int use_testnet;            // 1 = testnet, 0 = production
    int use_binance_us;         // 1 = binance.us endpoint (for US-based users)
    uint32_t poll_timeout_ms;   // poll() timeout in ms (e.g. 100)
    uint32_t reconnect_delay;   // seconds to wait before reconnect attempt
    uint32_t wind_down_minutes; // stop buys X minutes before reconnect
    int tui_enabled;            // 0 = headless mode, 1 = terminal dashboard
    char log_file[256];         // stderr redirect when headless (empty = no redirect)
    uint32_t cfg_load_fault_flags = 0;  // N1 (③ reuse) — set on a MALFORMED venue selector; binance_config_ok() => boot REFUSED
};

//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[316B]
// [ALIGN]_[4]
// [CACHE_LINES]_[5]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[BinanceConfig]
//======================================================================

//======================================================================
// [STRUCT]_[BinanceStream]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[connection + 8K frame-accumulation buffer + tick counter + connect_time (24h lifecycle) + socket/stdin pollfds]
//======================================================================
// [CODE]
//======================================================================
struct BinanceStream {
    int sockfd;
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    char read_buf[8192];        // frame accumulation buffer - 8k covers max trade JSON easily
    int read_pos;               // current write position in read_buf
    int read_len;               // not used for accumulation, reserved
    uint64_t tick_count;
    uint64_t connect_time;      // epoch seconds, for 24-hour reconnect tracking
    int connected;
    struct pollfd pfds[2];      // [0] = socket, [1] = stdin
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// read_buf accumulates partial SSL reads - websocket frames can arrive in fragments
// connect_time tracks the 24-hour session lifecycle for proactive reconnect
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[8264B]
// [ALIGN]_[8]
// [CACHE_LINES]_[130]
// [STRADDLE]_[pfds@8244]
//======================================================================
// [END_STRUCT]_[BinanceStream]
//======================================================================

//======================================================================
// [SECTION]_[THE WS LAYER FAMILY — NOW SHARED (WebSocketUtil.hpp)]
//----------------------------------------------------------------------
// base64 / tcp_connect / ssl_setup / handshake / frame reader / pong / close USED to live here as a
// PRIVATE copy — one of three hand-written implementations of RFC 6455 client framing in this dir
// (depth, trade, user-data). Nothing compared them, so they drifted in both directions: the depth
// copy answered pings with an empty UNMASKED pong and Binance killed every session after ~90 s,
// while THIS copy got the pong right but carried two holes of its own. Both are closed by consuming
// the ONE body (PARITY-071; D-487; 2026-09-05 leaf commit 3):
//
//   - `if ((int)pay_len > buf_size - 1)` narrowed the 64-bit length BEFORE the bounds check, and
//     then wrote `buf[pay_len] = '\0'` with the WIDE value. A 127-form length of 2^32+5 casts to 5,
//     passes the guard, consumes 5 bytes of a 4-billion-byte frame, and NUL-writes ~4 GB past the
//     buffer. `ws_read_frame` compares as uint64_t before any narrowing.
//   - `binance_ws_send_pong` built into a 256-byte stack frame with NO capacity check while the
//     reader handed it payloads from a 4096-byte buffer — a ping over ~250 bytes smashed the
//     producer thread's stack. `ws_build_pong` refuses what will not fit, and `ws_read_frame` now
//     rejects an oversize control frame at the reader (RFC 6455 §5.5) so the length never arrives.
//
// The migration also went the other way: this file's private copy was BETTER than the shared family
// in four places (padded base64, multi-address connect, non-dangling ssl out-params, a whole-request
// handshake write). Those were fixed IN WebSocketUtil.hpp first — a shared body is only a fix if it
// is a superset of every copy it replaces.
//
// Logging stays HERE, at the call sites: the shared bodies are I/O-free so the suite can drive them.
//======================================================================

//======================================================================
// [FUNCTION]_[binance_parse_trade]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[fixed-format trade extract ("p"/"q" quoted strings + "m" bare bool) — two bounded scans, no alloc/recursion; v5.11.16-audited (rationale in the comment block)]
//======================================================================
// [CODE]
//======================================================================
static inline int binance_parse_trade(const char *json, int len, char *price_str, char *qty_str, int *is_buyer_maker) {
    // v5.11.16 — sanity floor. A real Binance trade message is >= ~120 bytes
    // (event type + symbol + ids + price + qty + timestamps). Anything under
    // 20 is a truncated/non-trade frame; bail before scanning.
    if (len < 20) return 0;

    // find "p":" - the price field
    const char *p_key = "\"p\":\"";
    const char *p_pos = strstr(json, p_key);
    if (!p_pos) return 0;

    const char *p_start = p_pos + strlen(p_key);
    const char *p_end   = strchr(p_start, '"');
    if (!p_end) return 0;

    int p_len = (int)(p_end - p_start);
    if (p_len >= 64) return 0;  // sanity check
    memcpy(price_str, p_start, p_len);
    price_str[p_len] = '\0';

    // find "q":" - the quantity field
    const char *q_key = "\"q\":\"";
    const char *q_pos = strstr(json, q_key);
    if (!q_pos) return 0;

    const char *q_start = q_pos + strlen(q_key);
    const char *q_end   = strchr(q_start, '"');
    if (!q_end) return 0;

    int q_len = (int)(q_end - q_start);
    if (q_len >= 64) return 0;
    memcpy(qty_str, q_start, q_len);
    qty_str[q_len] = '\0';

    // find "m": - the is_buyer_maker field (boolean, not quoted)
    // true = buyer was maker (seller-initiated), false = buyer was taker (buyer-initiated)
    *is_buyer_maker = 0;
    const char *m_key = "\"m\":";
    const char *m_pos = strstr(json, m_key);
    if (m_pos) {
        const char *m_val = m_pos + strlen(m_key);
        *is_buyer_maker = (*m_val == 't') ? 1 : 0;
    }

    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// fixed-format parser - not general purpose. binance trade messages always have
// "p":"<price>" and "q":"<quantity>" fields. we scan for the key, extract the value
// between quotes, null-terminate it
//
// no allocations, no recursion, no tree building - just two string scans
// returns 1 if both fields found, 0 otherwise
//
// v5.11.16 (2026-05-07) — DataStream parsing audit. The strstr/strchr calls
// are safe to use without explicit length bounds because the caller
// (`ws_read_frame`, WebSocketUtil.hpp — was the private `binance_ws_read_frame`
// until the 2026-09-05 leaf commit 3) writes `out[len] = '\0'` after every frame
// read AND refuses any payload longer than the max_len it was given, so the null
// terminator is guaranteed to land within the buffer. That bound is now compared
// as uint64_t BEFORE any narrowing — the private reader compared `(int)pay_len`,
// which a 127-form length of 2^32+5 slipped past as 5. v5.11.4.A locale-immune parsing
// covered the actual number-extraction (FPN_FromString is digit-by-digit;
// out->price_d / out->volume_d use tt::parse_double_fast). Audit verdict:
// no behavior change needed; using the `len` parameter (previously unused)
// as a min-size sanity guard catches truncated frames without scanning.
//======================================================================
// [END_FUNCTION]_[binance_parse_trade]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceStream_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the connection lifecycle family (Close / Reconnect + the g_binance_shutdown_flag hook ride) — TCP -> TLS -> WS handshake; interruptible reconnect delay]
//======================================================================
// [CODE]
//======================================================================
static inline int BinanceStream_Init(BinanceStream *bs, const BinanceConfig *config) {
    memset(bs, 0, sizeof(BinanceStream));
    bs->sockfd    = -1;
    bs->connected = 0;

    // init OpenSSL - safe to call multiple times, internally idempotent
    OPENSSL_init_ssl(0, NULL);

    // pick host and port based on endpoint selection
    // data-stream.binance.vision: port 443, no geo-restriction, public data only
    // testnet: port 443
    // binance.us: port 9443
    // production: port 9443 (geo-restricted in some regions)
    const char *host;
    int port;
    if (config->use_testnet) {
        host = "testnet.binance.vision";
        port = 443;
    } else if (config->use_binance_us) {
        host = "stream.binance.us";
        port = 9443;
    } else {
        host = "data-stream.binance.vision";
        port = 443;
    }

    // layer 1: TCP. rcv_timeout 0 = block indefinitely inside a read, which is the behaviour this
    // producer has always had; giving the trade socket an SO_RCVTIMEO is its own tracked item, kept
    // out of a migration whose whole claim is that the producer's behaviour did not change.
    bs->sockfd = ws_tcp_connect(host, port, 0);
    if (bs->sockfd < 0) {
        fprintf(stderr, "[BINANCE] TCP connect failed to %s:%d\n", host, port);
        return 0;
    }

    // layer 2: TLS
    if (ws_ssl_setup(&bs->ssl_ctx, &bs->ssl, bs->sockfd, host) < 0) {
        fprintf(stderr, "[BINANCE] TLS setup failed for %s\n", host);
        close(bs->sockfd);
        bs->sockfd = -1;
        return 0;
    }

    // layer 3: WebSocket handshake
    // build the path: /ws/<symbol>@trade
    char path[128];
    snprintf(path, sizeof(path), "/ws/%s@trade", config->symbol);

    WsSslIo hio{bs->ssl};
    if (ws_handshake(hio, host, path) < 0) {
        fprintf(stderr, "[BINANCE] WebSocket upgrade failed (no 101) for %s%s\n", host, path);
        ws_close(bs->ssl, bs->ssl_ctx, bs->sockfd);   // close frame + shutdown + free + close(fd)
        bs->ssl     = NULL;
        bs->ssl_ctx = NULL;
        bs->sockfd  = -1;
        return 0;
    }

    // setup poll descriptors
    bs->pfds[0].fd     = bs->sockfd;
    bs->pfds[0].events = POLLIN;
    bs->pfds[1].fd     = STDIN_FILENO;
    bs->pfds[1].events = POLLIN;

    bs->connected    = 1;
    bs->connect_time = (uint64_t)time(NULL);
    bs->tick_count   = 0;
    bs->read_pos     = 0;
    bs->read_len     = 0;

    fprintf(stderr, "[BINANCE] connected to %s:%d - %s@trade\n", host, port, config->symbol);
    return 1;
}

//------------------------------------------------------------------
// [SECTION]_[CLOSE]
//------------------------------------------------------------------
// clean shutdown: send close frame, SSL shutdown, close socket, free resources
static inline void BinanceStream_Close(BinanceStream *bs) {
    if (!bs->connected) return;

    // ws_close sends the masked close frame, then SSL_shutdown / SSL_free / SSL_CTX_free / close(fd).
    // If it fails thats fine, were closing anyway.
    if (bs->ssl) {
        ws_close(bs->ssl, bs->ssl_ctx, bs->sockfd);
        bs->ssl     = NULL;
        bs->ssl_ctx = NULL;
        bs->sockfd  = -1;
    } else if (bs->sockfd >= 0) {
        close(bs->sockfd);
        bs->sockfd = -1;
    }

    bs->connected = 0;
    fprintf(stderr, "[BINANCE] connection closed\n");
}

//------------------------------------------------------------------
// [SECTION]_[RECONNECT]
//------------------------------------------------------------------
// close the existing connection and re-establish from scratch
// waits reconnect_delay seconds before attempting (avoids hammering binance)
// Engine sets this at startup so reconnect can break out of its delay
// sleep when the user closes the GUI / hits Ctrl+C. NULL (default) =
// no shutdown signal wired; degrades to a normal blocking sleep.
// C++17 inline variable: single instance shared across TUs, no linker conflict.
inline volatile std::sig_atomic_t* g_binance_shutdown_flag = nullptr;

static inline int BinanceStream_Reconnect(BinanceStream *bs, const BinanceConfig *config) {
    fprintf(stderr, "[BINANCE] reconnecting in %u seconds...\n", config->reconnect_delay);
    // Phase 8b: alert at the convergence point (every reconnect path lands here).
    if (g_notify) {
        char body[256];
        snprintf(body, sizeof(body),
                 "Trade WS disconnected. Reconnecting in %u seconds. "
                 "Investigate if this fires repeatedly.",
                 config->reconnect_delay);
        Notify_Send(g_notify, NOTIFY_WARN, NK_DISCONNECT_TRADE,
                    "Binance trade WS disconnected", body);
    }
    BinanceStream_Close(bs);

    // Interruptible sleep: poll the shutdown flag every 100ms instead of
    // blocking for `reconnect_delay` seconds. Without this, closing the GUI
    // during a reconnect window leaves the producer thread stuck in sleep()
    // for up to reconnect_delay seconds — feels like the process won't die.
    if (config->reconnect_delay > 0) {
        for (uint32_t s = 0; s < config->reconnect_delay; ++s) {
            for (int j = 0; j < 10; ++j) {
                if (g_binance_shutdown_flag && *g_binance_shutdown_flag) {
                    return 0;  // bail; caller's outer loop will see shutdown
                }
                struct timespec ts = {0, 100000000};  // 100ms
                nanosleep(&ts, NULL);
            }
        }
    }

    return BinanceStream_Init(bs, config);
}

//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[BinanceStream_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceStream_Poll]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[socket + stdin readiness -> OR'd POLL_* flags — SSL_pending checked FIRST (OpenSSL internal buffer vs poll() mismatch); rationale in the comment block]
//======================================================================
// [CODE]
//======================================================================
static inline int BinanceStream_Poll(BinanceStream *bs, uint32_t timeout_ms) {
    if (!bs->connected) return POLL_NONE;

    // SSL_pending check FIRST - avoids the SSL-vs-poll mismatch entirely
    // if OpenSSL has buffered decrypted data, we have frames to read right now
    if (SSL_pending(bs->ssl) > 0) {
#ifndef MULTICORE_TUI
        // still check stdin with zero timeout so we dont miss TUI commands
        // (in multicore mode, TUI thread owns STDIN — engine skips it)
        bs->pfds[1].revents = 0;
        poll(&bs->pfds[1], 1, 0);  // non-blocking stdin check
#endif
        int result = POLL_SOCKET;
#ifndef MULTICORE_TUI
        if (bs->pfds[1].revents & POLLIN) result |= POLL_STDIN;
#endif
        return result;
    }

    // normal poll - wait for socket data or stdin input
#ifdef MULTICORE_TUI
    int nfds = 1;  // socket only — TUI thread owns STDIN
#else
    int nfds = 2;  // socket + stdin
#endif
    int ret = poll(bs->pfds, nfds, timeout_ms);
    if (ret <= 0) return POLL_NONE;  // timeout or error

    int result = POLL_NONE;
    if (bs->pfds[0].revents & POLLIN)  result |= POLL_SOCKET;
#ifndef MULTICORE_TUI
    if (bs->pfds[1].revents & POLLIN)  result |= POLL_STDIN;
#endif
    return result;
}

//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// checks for available data on the socket and stdin
//
// CRITICAL: checks SSL_pending() FIRST. OpenSSL buffers decrypted data internally -
// poll() only sees the raw socket, so it can report "no data ready" while SSL has a
// complete frame sitting in its internal buffer. by checking SSL_pending first, we
// avoid this mismatch entirely - if SSL has buffered data, we return POLL_SOCKET
// immediately without ever calling poll()
//
// returns OR'd combination of POLL_NONE, POLL_SOCKET, POLL_STDIN
//======================================================================
// [END_FUNCTION]_[BinanceStream_Poll]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceStream_ReadTick]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[consume one frame -> DataStream<F> — transparent ping/pong; 1 on success, 0 on error/disconnect]
// [REFERENCE]_[DECISION]_[D-102]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
static inline int BinanceStream_ReadTick(BinanceStream *bs, DataStream<F> *out) {
    if (!bs->connected) return 0;

    char frame_buf[4096];
    int opcode = 0, fin = 0;
    WsSslIo io{bs->ssl};

    while (1) {
        // sizeof-1: ws_read_frame bounds the payload at max_len and NUL-terminates at out[len], so the
        // terminator of a maximal frame lands on the last byte of frame_buf (the same 4095-byte
        // effective capacity the private reader had, which compared against buf_size - 1).
        int payload_len = ws_read_frame(io, frame_buf, (int)sizeof(frame_buf) - 1, &opcode, &fin);
        if (payload_len < 0) {
            // Every negative return leaves the stream DESYNCED (the payload was not consumed), so the
            // only correct response to any of them is to drop the connection — never skip and continue.
            const char *why = (payload_len == WS_READ_TOO_LARGE) ? "frame larger than the 4095-byte buffer"
                            : (payload_len == WS_READ_PROTOCOL)  ? "RFC 6455 control-frame violation (>125 B or fragmented)"
                                                                 : "read error / EOF mid-frame";
            fprintf(stderr, "[BINANCE] %s - connection lost\n", why);
            bs->connected = 0;
            return 0;
        }

        if (opcode == 0x9) {
            // ping - respond with a MASKED payload-echo pong immediately, then read the next frame.
            // A failed write means the peer is gone; the venue kills a session whose pong does not
            // echo (code 1008 "Pong timeout" — the 2026-09-05 depth stall).
            if (!ws_send_pong(io, frame_buf, payload_len)) {
                fprintf(stderr, "[BINANCE] pong write failed - connection lost\n");
                bs->connected = 0;
                return 0;
            }
            continue;
        }

        if (opcode == 0x8) {
            // close frame from server
            fprintf(stderr, "[BINANCE] server sent close frame\n");
            bs->connected = 0;
            return 0;
        }

        if (opcode == 0x1) {
            // text frame - trade data JSON
            char price_str[64], qty_str[64];
            int is_buyer_maker = 0;
            if (!binance_parse_trade(frame_buf, payload_len, price_str, qty_str, &is_buyer_maker)) {
                // not a trade message (could be a subscription confirmation or error)
                // skip it and read next frame
                continue;
            }

            // D-102: venue decimal strings parse EXACTLY into decimal money (no binary
            // round-trip). Sticky MONEY_PARSE_* flags drain at the drainer cycle tail (P3).
            // Parse flags are STICKY on the producer thread (observational; S-17):
            // MALFORMED/OVERFLOW from a venue string is a wire-contract violation —
            // warn loudly at the parse seam (cold path; flags are ~always zero).
            const MoneyParse _pp = Money_FromString(price_str);
            const MoneyParse _qp = Money_FromString(qty_str);
            if (__builtin_expect((_pp.flags | _qp.flags) &
                                 (MONEY_PARSE_MALFORMED | MONEY_PARSE_OVERFLOW), 0)) {
                fprintf(stderr, "[ws-parse] MONEY parse flags=0x%x/0x%x on '%s'/'%s' — "
                        "venue wire contract violated\n",
                        _pp.flags, _qp.flags, price_str, qty_str);
            }
            out->price  = _pp.value;
            out->volume = _qp.value;
            // v5.11.19 — derive TUI doubles from the FPN_Binary values directly
            // instead of running a separate parse_double_fast pass on the
            // same string. Saves one parse per tick (BinanceCrypto's hot
            // ingestion is the only per-tick site) AND eliminates the
            // parity hazard of two parsers ever rounding differently
            // (FPN_FromString uses digit-by-digit integer math, locale-
            // immune by construction; tt::parse_double_fast uses
            // std::from_chars). The two paths agree for in-spec tick
            // strings today, but the duplication invited future drift.
            // FPN_ToDouble is a deterministic conversion (uint64_t
            // limb math + ldexp combination), so this is a strict
            // tightening: every TUI double is now provably consistent
            // with its FPN_Binary value.
            out->price_d  = Money_ToDouble(out->price);
            out->volume_d = Money_ToDouble(out->volume);
            out->is_buyer_maker = is_buyer_maker;
            bs->tick_count++;
            return 1;
        }

        // unknown opcode - skip and continue
    }
}

//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// reads one websocket frame, handles ping/pong transparently, parses JSON trade data
// into the DataStream output struct
//
// ping handling: if we get a ping (opcode 0x9), we immediately pong and loop to read
// the next frame. this is invisible to the caller - they just get trade data or an error
//
// returns 1 on success (out filled with price + volume), 0 on error/disconnect
//======================================================================
// [END_FUNCTION]_[BinanceStream_ReadTick]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceStream_InWindDown]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the 24h session family (ShouldReconnect / HasPending ride) — wind-down at 23h25m, reconnect at 23h30m (30-min buffer before Binance's hard cutoff)]
//======================================================================
// [CODE]
//======================================================================
static inline int BinanceStream_InWindDown(BinanceStream *bs, uint32_t wind_down_minutes) {
    if (!bs->connected) return 0;

    uint64_t now     = (uint64_t)time(NULL);
    uint64_t elapsed = now - bs->connect_time;

    // wind down starts at the planned reconnect minus wind_down_minutes
    // e.g. with wind_down_minutes=5: wind down at 23h25m = 84300 seconds
    uint64_t wind_down_start = WS_PLANNED_RECONNECT_S - (uint64_t)(wind_down_minutes * 60);

    return (elapsed >= wind_down_start) ? 1 : 0;
}

static inline int BinanceStream_ShouldReconnect(BinanceStream *bs) {
    if (!bs->connected) return 0;

    // WS_PLANNED_RECONNECT_S (23h30m = 84600 s, a 30-min buffer before Binance's 24 h cutoff) is the
    // SSoT this file used to spell as its own literal. The helper also guards the two cases the bare
    // `now - connect_time` subtraction got wrong: connect_time 0 (never connected) and a backwards
    // wall clock, both of which underflow unsigned into a huge elapsed and force a spurious reconnect.
    return ws_planned_reconnect_due((uint64_t)time(NULL), bs->connect_time);
}

//------------------------------------------------------------------
// [SECTION]_[HAS PENDING DATA]
//------------------------------------------------------------------
// exposes SSL_pending check for the main loop's burst drain logic
// returns 1 if SSL has buffered data that can be read without blocking
static inline int BinanceStream_HasPending(BinanceStream *bs) {
    if (!bs->connected || !bs->ssl) return 0;
    return (SSL_pending(bs->ssl) > 0) ? 1 : 0;
}

//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// binance auto-disconnects after 24 hours. we proactively reconnect before that:
// - wind down starts at connect_time + 23h25m (disable buy gate)
// - reconnect triggers at connect_time + 23h30m (close all positions, reconnect)
// - 30 min buffer before the hard 24h cutoff
//======================================================================
// [END_FUNCTION]_[BinanceStream_InWindDown]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceConfig_Load]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CFG_FLOW] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[boot cfg parse (venue-selector fault flag + selector + config_ok ride) — refuse-don't-coerce on malformed venue selectors (N1; a typo must NOT flip testnet->PROD)]
// [REFERENCE]_[INVARIANT]_[H22]
//======================================================================
// [CODE]
//======================================================================
// parses binance-specific fields from the engine config file
// same key=value format as ControllerConfig_Load, skips # comments and empty lines
// N1 (③ config-compiler reuse) — BinanceConfig venue-selector malformed-capture. `atoi` swallows a
// malformed value ->0, and 0 is the MORE-DANGEROUS value (use_testnet=0 = PRODUCTION venue) -> a typo
// silently flips testnet->PROD (capital-conditional; the H22 cross-parser asymmetry the swallow-coerce
// sweep found -- ControllerConfig now refuses malformed capital cfg, so this sibling boot parser must too).
// Detect malformed (locale-immune parse_int_checked) -> set a fault bit; main.cpp's boot gate refuses
// (refuse-don't-coerce, the SAME model as ControllerConfig's cfg_compile_ok). Empty = keep the default.
inline constexpr uint32_t BINANCE_CFG_FAULT_VENUE_MALFORMED = 1u << 0;
inline int binance_cfg_selector(BinanceConfig& config, const char* key, const char* val, int current) {
    if (val[0] == '\0') return current;  // empty = keep default
    bool malformed = false;
    long v = tt::parse_int_checked(val, &malformed);
    if (malformed) {
        config.cfg_load_fault_flags |= BINANCE_CFG_FAULT_VENUE_MALFORMED;
        fprintf(stderr, "[cfg] FATAL: venue selector %s='%s' is MALFORMED (expected 0/1) -> boot REFUSED. "
                "A typo here can silently flip testnet->PRODUCTION; be explicit.\n", key, val);
        return current;  // value moot -- boot will refuse
    }
    return (int)v;
}
// N1 -- boot-gate predicate (sister to ControllerConfig's cfg_compile_ok); main.cpp refuses on false.
inline bool binance_config_ok(const BinanceConfig& c) { return c.cfg_load_fault_flags == 0u; }

static inline BinanceConfig BinanceConfig_Load(const char *filepath) {
    BinanceConfig config;
    memset(&config, 0, sizeof(config));

    // defaults
    strcpy(config.symbol, "btcusdt");
    config.use_testnet       = 1;
    config.use_binance_us    = 0;
    config.poll_timeout_ms   = 100;
    config.reconnect_delay   = 5;
    config.wind_down_minutes = 5;
    config.tui_enabled       = 1;
    strcpy(config.log_file, "engine.log");

    FILE *f = fopen(filepath, "r");
    if (!f) {
        fprintf(stderr, "[BINANCE] config file not found: %s, using defaults\n", filepath);
        return config;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        // strip newline
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        const char *key = line;
        char *val = eq + 1;

        // strip inline comments and trailing whitespace from value
        char *comment = strchr(val, '#');
        if (comment) *comment = '\0';
        int vlen = strlen(val);
        while (vlen > 0 && (val[vlen-1] == ' ' || val[vlen-1] == '\t')) val[--vlen] = '\0';

        if (strcmp(key, "symbol") == 0) {
            strncpy(config.symbol, val, sizeof(config.symbol) - 1);
        } else if (strcmp(key, "use_testnet") == 0) {
            config.use_testnet = binance_cfg_selector(config, key, val, config.use_testnet);
        } else if (strcmp(key, "use_binance_us") == 0) {
            config.use_binance_us = binance_cfg_selector(config, key, val, config.use_binance_us);
        } else if (strcmp(key, "poll_timeout_ms") == 0) {
            config.poll_timeout_ms = (uint32_t)atol(val);
        } else if (strcmp(key, "reconnect_delay") == 0) {
            config.reconnect_delay = (uint32_t)atol(val);
        } else if (strcmp(key, "wind_down_minutes") == 0) {
            config.wind_down_minutes = (uint32_t)atol(val);
        } else if (strcmp(key, "tui_enabled") == 0) {
            config.tui_enabled = atoi(val);
        } else if (strcmp(key, "log_file") == 0) {
            strncpy(config.log_file, val, sizeof(config.log_file) - 1);
        }
    }

    fclose(f);
    return config;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[BinanceConfig_Load]
//======================================================================
#endif // BINANCE_CRYPTO_HPP
