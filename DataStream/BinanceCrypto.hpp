// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BINANCE WEBSOCKET DATA-STREAM FOR CRYPTO]
//======================================================================================================
// connects to Binance trade websocket (wss://stream.binance.com:9443/ws/<symbol>@trade)
// and produces DataStream<F> structs - same interface the pipeline already consumes
//
// no API key needed for market data - public endpoint, read-only
// handles the full network stack: TCP -> TLS -> WebSocket -> JSON -> FPN
//
// the main loop calls BinanceStream_Poll to check for data, then BinanceStream_ReadTick
// to consume one frame. poll checks SSL_pending FIRST to avoid the SSL-internal-buffer
// vs poll() mismatch - if SSL has buffered data, we return immediately without calling poll()
//
// ping/pong handled transparently inside the frame reader - binance sends a ping every ~3 min
// and disconnects if you dont pong back
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
#include <netdb.h>
#include <poll.h>
#include <time.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/bio.h>
#include <openssl/evp.h>

#include "../FixedPoint/FixedPointN.hpp"
#include "../CoreFrameworks/OrderGates.hpp"

using namespace std;

//======================================================================================================
// [POLL FLAGS]
//======================================================================================================
#define POLL_NONE   0
#define POLL_SOCKET 1
#define POLL_STDIN  2

//======================================================================================================
// [CONFIG]
//======================================================================================================
struct BinanceConfig {
    char symbol[32];            // e.g. "btcusdt" (lowercase)
    int use_testnet;            // 1 = testnet, 0 = production
    int use_binance_us;         // 1 = binance.us endpoint (for US-based users)
    uint32_t poll_timeout_ms;   // poll() timeout in ms (e.g. 100)
    uint32_t reconnect_delay;   // seconds to wait before reconnect attempt
    uint32_t wind_down_minutes; // stop buys X minutes before reconnect
    int tui_enabled;            // 0 = headless mode, 1 = terminal dashboard
    char log_file[256];         // stderr redirect when headless (empty = no redirect)
};

//======================================================================================================
// [STREAM STATE]
//======================================================================================================
// read_buf accumulates partial SSL reads - websocket frames can arrive in fragments
// connect_time tracks the 24-hour session lifecycle for proactive reconnect
//======================================================================================================
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

//======================================================================================================
// [BASE64 ENCODER]
//======================================================================================================
// fixed-size base64 for the 16-byte websocket key - no general purpose encoder needed
// input: 16 bytes, output: 24 chars + null terminator
// we need this for the Sec-WebSocket-Key header in the HTTP upgrade request
//======================================================================================================
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static inline void binance_base64_encode(const unsigned char *input, int input_len, char *output) {
    int i = 0, j = 0;
    while (i < input_len) {
        uint32_t octet_a = (i < input_len) ? input[i++] : 0;
        uint32_t octet_b = (i < input_len) ? input[i++] : 0;
        uint32_t octet_c = (i < input_len) ? input[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        output[j++] = b64_table[(triple >> 18) & 0x3F];
        output[j++] = b64_table[(triple >> 12) & 0x3F];
        output[j++] = b64_table[(triple >> 6)  & 0x3F];
        output[j++] = b64_table[triple         & 0x3F];
    }
    // pad - for 16 bytes input (16 % 3 == 1), last group has 1 real byte, 2 padding
    int pad = (3 - (input_len % 3)) % 3;
    for (int p = 0; p < pad; p++) {
        output[j - 1 - p] = '=';
    }
    output[j] = '\0';
}

//======================================================================================================
// [LAYER 1: TCP CONNECT]
//======================================================================================================
// standard POSIX socket connect - getaddrinfo resolves the hostname, then we connect
// returns the socket fd or -1 on failure
//======================================================================================================
static inline int binance_tcp_connect(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(host, port, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "[BINANCE] getaddrinfo failed: %s\n", gai_strerror(err));
        return -1;
    }

    int sockfd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) continue;

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) break; // success

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);

    if (sockfd == -1) {
        fprintf(stderr, "[BINANCE] TCP connect failed to %s:%s\n", host, port);
    }
    return sockfd;
}

//======================================================================================================
// [LAYER 2: TLS SETUP]
//======================================================================================================
// OpenSSL TLS handshake over the existing TCP socket
// sets SNI hostname (required by binance) and verifies the handshake completes
// returns 1 on success, 0 on failure
//======================================================================================================
static inline int binance_tls_setup(BinanceStream *bs, const char *host) {
    bs->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!bs->ssl_ctx) {
        fprintf(stderr, "[BINANCE] SSL_CTX_new failed\n");
        return 0;
    }

    bs->ssl = SSL_new(bs->ssl_ctx);
    if (!bs->ssl) {
        fprintf(stderr, "[BINANCE] SSL_new failed\n");
        SSL_CTX_free(bs->ssl_ctx);
        bs->ssl_ctx = NULL;
        return 0;
    }

    SSL_set_fd(bs->ssl, bs->sockfd);

    // SNI - binance requires this or the handshake fails
    SSL_set_tlsext_host_name(bs->ssl, host);

    int ret = SSL_connect(bs->ssl);
    if (ret != 1) {
        fprintf(stderr, "[BINANCE] SSL_connect failed: %d\n", SSL_get_error(bs->ssl, ret));
        SSL_free(bs->ssl);
        SSL_CTX_free(bs->ssl_ctx);
        bs->ssl     = NULL;
        bs->ssl_ctx = NULL;
        return 0;
    }

    return 1;
}

//======================================================================================================
// [LAYER 3: WEBSOCKET HANDSHAKE]
//======================================================================================================
// HTTP upgrade request over TLS - sends the Upgrade: websocket headers and verifies
// the server responds with 101 Switching Protocols
//
// the Sec-WebSocket-Key is 16 random bytes base64-encoded - server echoes back a hash
// of it but we dont bother verifying the accept hash (we trust the connection at this point)
//======================================================================================================
static inline int binance_ws_handshake(BinanceStream *bs, const char *path, const char *host) {
    // generate 16 random bytes for the websocket key
    unsigned char key_bytes[16];
    RAND_bytes(key_bytes, 16);

    char key_b64[32];
    binance_base64_encode(key_bytes, 16, key_b64);

    // build the HTTP upgrade request
    char request[512];
    int req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, key_b64);

    int written = SSL_write(bs->ssl, request, req_len);
    if (written != req_len) {
        fprintf(stderr, "[BINANCE] SSL_write handshake failed\n");
        return 0;
    }

    // read response - we just need to verify "101" is in the status line
    // the full response is small (< 512 bytes), read it all
    char response[1024];
    int total = 0;

    // read until we see the \r\n\r\n that ends the HTTP headers
    while (total < (int)sizeof(response) - 1) {
        int n = SSL_read(bs->ssl, response + total, sizeof(response) - 1 - total);
        if (n <= 0) {
            fprintf(stderr, "[BINANCE] SSL_read handshake response failed\n");
            return 0;
        }
        total += n;
        response[total] = '\0';

        // check if we have the complete headers
        if (strstr(response, "\r\n\r\n")) break;
    }

    // verify 101 Switching Protocols
    if (!strstr(response, "101")) {
        fprintf(stderr, "[BINANCE] WebSocket upgrade failed - no 101 in response\n");
        fprintf(stderr, "[BINANCE] Response: %.200s\n", response);
        return 0;
    }

    return 1;
}

//======================================================================================================
// [LAYER 4: WEBSOCKET FRAME READER]
//======================================================================================================
// reads one complete websocket frame from SSL, handling partial reads
//
// websocket frame format (RFC 6455):
//   byte 0: [FIN:1][RSV:3][opcode:4]
//   byte 1: [MASK:1][payload_len:7]
//   if payload_len == 126: next 2 bytes are the real length (big-endian)
//   if payload_len == 127: next 8 bytes are the real length (big-endian)
//   if MASK bit set: next 4 bytes are the masking key
//   then: payload data
//
// server-to-client frames are NOT masked (per RFC 6455)
// returns payload length, sets *opcode, fills buf with payload
// returns -1 on error
//======================================================================================================
static inline int binance_ws_read_frame(BinanceStream *bs, char *buf, int buf_size, int *opcode) {
    unsigned char header[2];
    int n;

    // read the 2-byte header - loop handles partial reads
    int hdr_read = 0;
    while (hdr_read < 2) {
        n = SSL_read(bs->ssl, header + hdr_read, 2 - hdr_read);
        if (n <= 0) return -1;
        hdr_read += n;
    }

    *opcode          = header[0] & 0x0F;
    int masked       = (header[1] >> 7) & 1;
    uint64_t pay_len = header[1] & 0x7F;

    // extended payload length
    if (pay_len == 126) {
        unsigned char ext[2];
        int ext_read = 0;
        while (ext_read < 2) {
            n = SSL_read(bs->ssl, ext + ext_read, 2 - ext_read);
            if (n <= 0) return -1;
            ext_read += n;
        }
        pay_len = __builtin_bswap16(*(uint16_t *)ext);
    } else if (pay_len == 127) {
        unsigned char ext[8];
        int ext_read = 0;
        while (ext_read < 8) {
            n = SSL_read(bs->ssl, ext + ext_read, 8 - ext_read);
            if (n <= 0) return -1;
            ext_read += n;
        }
        pay_len = __builtin_bswap64(*(uint64_t *)ext);
    }

    // masking key (server frames shouldn't be masked, but handle it anyway)
    unsigned char mask_key[4] = {0, 0, 0, 0};
    if (masked) {
        int mk_read = 0;
        while (mk_read < 4) {
            n = SSL_read(bs->ssl, mask_key + mk_read, 4 - mk_read);
            if (n <= 0) return -1;
            mk_read += n;
        }
    }

    // clamp to buffer size
    if ((int)pay_len > buf_size - 1) {
        fprintf(stderr, "[BINANCE] frame too large: %lu bytes\n", (unsigned long)pay_len);
        return -1;
    }

    // read payload - loop until we have all pay_len bytes
    int payload_read = 0;
    while (payload_read < (int)pay_len) {
        n = SSL_read(bs->ssl, buf + payload_read, (int)pay_len - payload_read);
        if (n <= 0) return -1;
        payload_read += n;
    }

    // unmask if needed
    if (masked) {
        for (int i = 0; i < (int)pay_len; i++) {
            buf[i] ^= mask_key[i & 3];
        }
    }

    buf[pay_len] = '\0';
    return (int)pay_len;
}

//======================================================================================================
// [WEBSOCKET PONG]
//======================================================================================================
// client-to-server frames MUST be masked (RFC 6455 section 5.1)
// pong echoes the same payload back with opcode 0xA
//======================================================================================================
static inline int binance_ws_send_pong(BinanceStream *bs, const char *payload, int len) {
    // frame: [0x8A] [0x80 | len] [4-byte mask] [masked payload]
    // max pong payload from binance is tiny (usually empty or a few bytes)
    unsigned char frame[256];
    int pos = 0;

    frame[pos++] = 0x8A;  // FIN + pong opcode

    // payload length with mask bit set
    if (len < 126) {
        frame[pos++] = 0x80 | (unsigned char)len;
    } else {
        // pong payloads should never be this big, but handle it
        frame[pos++] = 0x80 | 126;
        uint16_t be_len = __builtin_bswap16((uint16_t)len);
        memcpy(frame + pos, &be_len, 2);
        pos += 2;
    }

    // generate masking key
    unsigned char mask_key[4];
    RAND_bytes(mask_key, 4);
    memcpy(frame + pos, mask_key, 4);
    pos += 4;

    // masked payload
    for (int i = 0; i < len; i++) {
        frame[pos++] = payload[i] ^ mask_key[i & 3];
    }

    int written = SSL_write(bs->ssl, frame, pos);
    return (written == pos) ? 1 : 0;
}

//======================================================================================================
// [WEBSOCKET CLOSE FRAME]
//======================================================================================================
// sends a clean close frame (opcode 0x8) - masked, no payload
//======================================================================================================
static inline int binance_ws_send_close(BinanceStream *bs) {
    unsigned char frame[8];
    frame[0] = 0x88;  // FIN + close opcode
    frame[1] = 0x80;  // masked, zero length

    unsigned char mask_key[4];
    RAND_bytes(mask_key, 4);
    memcpy(frame + 2, mask_key, 4);

    int written = SSL_write(bs->ssl, frame, 6);
    return (written == 6) ? 1 : 0;
}

//======================================================================================================
// [LAYER 5: JSON PARSER]
//======================================================================================================
// fixed-format parser - not general purpose. binance trade messages always have
// "p":"<price>" and "q":"<quantity>" fields. we scan for the key, extract the value
// between quotes, null-terminate it
//
// no allocations, no recursion, no tree building - just two string scans
// returns 1 if both fields found, 0 otherwise
//======================================================================================================
static inline int binance_parse_trade(const char *json, int len, char *price_str, char *qty_str) {
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

    return 1;
}

//======================================================================================================
// [INIT]
//======================================================================================================
// full connection setup: TCP -> TLS -> WebSocket handshake
// after this returns 1, the stream is ready to receive trade data
//
// OpenSSL is initialized once with OPENSSL_init_ssl - safe to call multiple times
// (internally tracks whether its already been called)
//======================================================================================================
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
    const char *port;
    if (config->use_testnet) {
        host = "testnet.binance.vision";
        port = "443";
    } else if (config->use_binance_us) {
        host = "stream.binance.us";
        port = "9443";
    } else {
        host = "data-stream.binance.vision";
        port = "443";
    }

    // layer 1: TCP
    bs->sockfd = binance_tcp_connect(host, port);
    if (bs->sockfd < 0) return 0;

    // layer 2: TLS
    if (!binance_tls_setup(bs, host)) {
        close(bs->sockfd);
        bs->sockfd = -1;
        return 0;
    }

    // layer 3: WebSocket handshake
    // build the path: /ws/<symbol>@trade
    char path[128];
    snprintf(path, sizeof(path), "/ws/%s@trade", config->symbol);

    if (!binance_ws_handshake(bs, path, host)) {
        SSL_shutdown(bs->ssl);
        SSL_free(bs->ssl);
        SSL_CTX_free(bs->ssl_ctx);
        close(bs->sockfd);
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

    fprintf(stderr, "[BINANCE] connected to %s - %s@trade\n", host, config->symbol);
    return 1;
}

//======================================================================================================
// [CLOSE]
//======================================================================================================
// clean shutdown: send close frame, SSL shutdown, close socket, free resources
//======================================================================================================
static inline void BinanceStream_Close(BinanceStream *bs) {
    if (!bs->connected) return;

    // try to send a clean close frame - if it fails thats fine, were closing anyway
    binance_ws_send_close(bs);

    if (bs->ssl) {
        SSL_shutdown(bs->ssl);
        SSL_free(bs->ssl);
        bs->ssl = NULL;
    }
    if (bs->ssl_ctx) {
        SSL_CTX_free(bs->ssl_ctx);
        bs->ssl_ctx = NULL;
    }
    if (bs->sockfd >= 0) {
        close(bs->sockfd);
        bs->sockfd = -1;
    }

    bs->connected = 0;
    fprintf(stderr, "[BINANCE] connection closed\n");
}

//======================================================================================================
// [RECONNECT]
//======================================================================================================
// close the existing connection and re-establish from scratch
// waits reconnect_delay seconds before attempting (avoids hammering binance)
//======================================================================================================
static inline int BinanceStream_Reconnect(BinanceStream *bs, const BinanceConfig *config) {
    fprintf(stderr, "[BINANCE] reconnecting in %u seconds...\n", config->reconnect_delay);
    BinanceStream_Close(bs);

    if (config->reconnect_delay > 0) {
        sleep(config->reconnect_delay);
    }

    return BinanceStream_Init(bs, config);
}

//======================================================================================================
// [POLL]
//======================================================================================================
// checks for available data on the socket and stdin
//
// CRITICAL: checks SSL_pending() FIRST. OpenSSL buffers decrypted data internally -
// poll() only sees the raw socket, so it can report "no data ready" while SSL has a
// complete frame sitting in its internal buffer. by checking SSL_pending first, we
// avoid this mismatch entirely - if SSL has buffered data, we return POLL_SOCKET
// immediately without ever calling poll()
//
// returns OR'd combination of POLL_NONE, POLL_SOCKET, POLL_STDIN
//======================================================================================================
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

//======================================================================================================
// [READ TICK]
//======================================================================================================
// reads one websocket frame, handles ping/pong transparently, parses JSON trade data
// into the DataStream output struct
//
// ping handling: if we get a ping (opcode 0x9), we immediately pong and loop to read
// the next frame. this is invisible to the caller - they just get trade data or an error
//
// returns 1 on success (out filled with price + volume), 0 on error/disconnect
//======================================================================================================
template <unsigned F>
static inline int BinanceStream_ReadTick(BinanceStream *bs, DataStream<F> *out) {
    if (!bs->connected) return 0;

    char frame_buf[4096];
    int opcode;

    while (1) {
        int payload_len = binance_ws_read_frame(bs, frame_buf, sizeof(frame_buf), &opcode);
        if (payload_len < 0) {
            fprintf(stderr, "[BINANCE] frame read error - connection lost\n");
            bs->connected = 0;
            return 0;
        }

        if (opcode == 0x9) {
            // ping - respond with pong immediately, then read next frame
            binance_ws_send_pong(bs, frame_buf, payload_len);
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
            if (!binance_parse_trade(frame_buf, payload_len, price_str, qty_str)) {
                // not a trade message (could be a subscription confirmation or error)
                // skip it and read next frame
                continue;
            }

            out->price  = FPN_FromString<F>(price_str);
            out->volume = FPN_FromString<F>(qty_str);
            out->price_d  = atof(price_str);   // stash double for TUI (hidden in I/O path)
            out->volume_d = atof(qty_str);
            bs->tick_count++;
            return 1;
        }

        // unknown opcode - skip and continue
    }
}

//======================================================================================================
// [SESSION LIFECYCLE]
//======================================================================================================
// binance auto-disconnects after 24 hours. we proactively reconnect before that:
// - wind down starts at connect_time + 23h25m (disable buy gate)
// - reconnect triggers at connect_time + 23h30m (close all positions, reconnect)
// - 30 min buffer before the hard 24h cutoff
//
// BinanceStream_InWindDown: returns 1 if we're in the wind-down period
// BinanceStream_ShouldReconnect: returns 1 if it's time to close and reconnect
//======================================================================================================
static inline int BinanceStream_InWindDown(BinanceStream *bs, uint32_t wind_down_minutes) {
    if (!bs->connected) return 0;

    uint64_t now     = (uint64_t)time(NULL);
    uint64_t elapsed = now - bs->connect_time;

    // wind down starts at 23h30m - wind_down_minutes
    // e.g. with wind_down_minutes=5: wind down at 23h25m = 84300 seconds
    uint64_t wind_down_start = (23 * 3600 + 30 * 60) - (wind_down_minutes * 60);

    return (elapsed >= wind_down_start) ? 1 : 0;
}

static inline int BinanceStream_ShouldReconnect(BinanceStream *bs) {
    if (!bs->connected) return 0;

    uint64_t now     = (uint64_t)time(NULL);
    uint64_t elapsed = now - bs->connect_time;

    // reconnect at 23h30m = 84600 seconds (30 min buffer before 24h cutoff)
    return (elapsed >= 84600) ? 1 : 0;
}

//======================================================================================================
// [HAS PENDING DATA]
//======================================================================================================
// exposes SSL_pending check for the main loop's burst drain logic
// returns 1 if SSL has buffered data that can be read without blocking
//======================================================================================================
static inline int BinanceStream_HasPending(BinanceStream *bs) {
    if (!bs->connected || !bs->ssl) return 0;
    return (SSL_pending(bs->ssl) > 0) ? 1 : 0;
}

//======================================================================================================
// [CONFIG LOADER]
//======================================================================================================
// parses binance-specific fields from the engine config file
// same key=value format as ControllerConfig_Load, skips # comments and empty lines
//======================================================================================================
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
            config.use_testnet = atoi(val);
        } else if (strcmp(key, "use_binance_us") == 0) {
            config.use_binance_us = atoi(val);
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

//======================================================================================================
//======================================================================================================
#endif // BINANCE_CRYPTO_HPP
