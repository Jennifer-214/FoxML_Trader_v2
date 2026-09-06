// SPDX-License-Identifier: AGPL-3.0-or-later

//======================================================================================================
// [FILE]_[DataStream/WebSocketUtil.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE WebSocket frame family (reader / pong / close) + the shared TCP / SSL / handshake plumbing under the venue streams — RFC 6455 client side: partial-read loops on EVERY field, a 64-bit length compared BEFORE any int cast, control-frame limits enforced at the reader (§5.5), a MASKED payload-echo pong, a masked close, padded base64 for the upgrade key (§4.1); the reader is a template over an Io seam (WsSslIo in production, a byte cursor in the suite). Consumers: BinanceDepth (the 2026-09-05 depth-stall leaf, commit 1); BinanceCrypto + BinanceUserData migrate onto it in that leaf's commits 3-4 (their private copies carry the (int)pay_len length-cast hole and the uncapped pong builder this family closes)]
// [CONTAINS]
//   - [FUNCTION]_[ws_read_frame]   (+ base64 / tcp_connect / ssl_setup / handshake / read_exact / build_pong / build_close / send_pong / send_close / close / stale / planned_reconnect_due family)
//======================================================================================================
// HISTORY. Until 2026-09-05 this file's reader did a single 2-byte SSL_read on the header (no partial-read
// loop), folded a 64-bit length into an `int`, and its pong was `{0x8A, 0x00}` — EMPTY and UNMASKED
// (RFC 6455 §5.1 requires client→server frames masked, §5.5.3 requires the pong to echo the ping
// payload). Binance answered that pong with a 1008 "Pong timeout" close 74-102 s after every connect —
// the depth-stream stall proven at plans/v5.15-live-readiness/plan_checks/2026-09-05-depth-ws-stall-proof/.
// The trade client (BinanceCrypto.hpp) and the user-data client (BinanceUserData.hpp) had CORRECT private
// copies of the same reader + pong — three parallel frame implementations, one drifted (Class 21); this
// file is now the single body all three consume.
//
// 2026-09-05 leaf commit 3 (the BinanceCrypto migration) found FOUR places where this shared family
// was WEAKER than the trade client's private copy it was about to replace — a shared body is only a
// fix if it is a superset, so each was closed here BEFORE the migration: the base64 encoder emitted
// no '=' padding (an RFC-invalid Sec-WebSocket-Key that Binance happens to tolerate); ws_tcp_connect
// stopped at the first resolved address instead of walking them; ws_ssl_setup left DANGLING out-params
// on every failure path; ws_handshake accepted a partial or truncated upgrade write. The control-frame
// limit (§5.5) went in at the same time — the trade client's uncapped pong builder was a reachable
// stack overflow, and enforcing the limit at the reader is what closes the class rather than the case.
//======================================================================================================
#ifndef WEBSOCKET_UTIL_HPP
#define WEBSOCKET_UTIL_HPP

#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <poll.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/tcp.h>

//======================================================================
// [STRUCT]_[WsSslIo]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the production transport for the ws_* frame family — binds the SSL* at the call site, stores nothing else. An Io is any struct with `int read(unsigned char* dst, int want)` (bytes read >0, or <=0 on EOF/error) and `int write(const unsigned char* src, int len)` (bytes written); the suite's byte cursor with a chunk size + a write-capture buffer drives the SAME templates, so the parse is proven invariant to how the bytes arrive (a TLS record boundary can fall inside the 2-byte header, the extended length, or the mask) and the pong that would go on the wire is asserted byte-for-byte]
//======================================================================
// [CODE]
//======================================================================
struct WsSslIo {
    SSL *ssl;
    int read (unsigned char *dst, int want)      { return SSL_read (ssl, dst, want); }
    int write(const unsigned char *src, int len) { return SSL_write(ssl, src, len); }
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-09-05]
// [SIZE]_[8B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[WsSslIo]
//======================================================================

//======================================================================
// [FUNCTION]_[ws_read_frame]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the WS utility family (base64 / tcp_connect / ssl_setup / handshake / read_exact / build_pong / build_close / send_pong / send_close / close / stale / planned_reconnect_due ride) — the frame reader loops every field to completion, handles the 7/16/64-bit length forms, compares the 64-bit length against the buffer BEFORE any int cast (WS_READ_TOO_LARGE, the stream is then DESYNCED and the caller must disconnect), unmasks a masked frame, NUL-terminates; the reader + the senders are templated over the Io transport (WsSslIo in production; the suite's byte cursor + write capture) so TLS-record fragmentation is deterministic and the wire bytes are assertable — no fn-pointer, no virtual (H1/H2)]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[BASE64]
//------------------------------------------------------------------
static const char ws_b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static inline void ws_base64_encode(const unsigned char *in, int len, char *out) {
    int i = 0, j = 0;
    while (i < len) {
        uint32_t a = (i < len) ? in[i++] : 0;
        uint32_t b = (i < len) ? in[i++] : 0;
        uint32_t c = (i < len) ? in[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = ws_b64_table[(triple >> 18) & 0x3F];
        out[j++] = ws_b64_table[(triple >> 12) & 0x3F];
        out[j++] = ws_b64_table[(triple >> 6)  & 0x3F];
        out[j++] = ws_b64_table[triple & 0x3F];
    }
    // RFC 4648 §4 PADDING. The final group encodes 3 - (len % 3) bytes that were never
    // there; each one turns the char it produced into '='. Without this the last group
    // reads as data ('AA' for a 16-byte key instead of '=='), which is not base64 —
    // RFC 6455 §4.1 requires Sec-WebSocket-Key to be the base64 of 16 bytes, and a server
    // that validates it rejects the upgrade. Binance does not validate, which is the only
    // reason the depth stream worked with this missing (2026-09-05 leaf commit 3): the
    // trade client's private encoder had the padding and this shared one did not, so
    // migrating BinanceCrypto onto this family without this fix would have REGRESSED a
    // correct handshake into a tolerated-by-luck one.
    const int pad = (3 - (len % 3)) % 3;
    for (int p = 0; p < pad; p++) out[j - 1 - p] = '=';
    out[j] = '\0';
}

//------------------------------------------------------------------
// [SECTION]_[TCP CONNECT]
//------------------------------------------------------------------
// rcv_timeout_ms > 0 sets SO_RCVTIMEO: a blocking SSL_read inside a frame then returns after that long
// instead of parking the thread forever on a peer that stopped mid-record — the hole a poll-timeout
// watchdog cannot see (2026-09-05 depth leaf). 0 = no timeout (the pre-2026-09-05 behaviour).
static inline int ws_tcp_connect(const char *host, int port, uint32_t rcv_timeout_ms) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo hints = {}, *res, *rp;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;
    // Walk EVERY resolved address, not just the first. Binance's stream hosts resolve to a
    // rotating set of A records and any one of them can be refusing connections while its
    // siblings serve; stopping at res->ai_addr turns a routine failover into an outage that
    // looks like the venue is down. The trade client has iterated since v5.11 — this family
    // did not, so the depth stream inherited the weaker behaviour (2026-09-05 leaf commit 3).
    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;   // this one answered
        close(fd);                                                   // never leak the attempt
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));   // parity with the trade client
    if (rcv_timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec  = (time_t)(rcv_timeout_ms / 1000u);
        tv.tv_usec = (suseconds_t)((rcv_timeout_ms % 1000u) * 1000u);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    return fd;
}

//------------------------------------------------------------------
// [SECTION]_[SSL SETUP]
//------------------------------------------------------------------
// The out-params are written ONLY on success, and NULLed up front. The previous shape assigned
// them as it went and then freed on the failure paths, so a failed setup handed the caller two
// DANGLING pointers and every caller had to remember to re-NULL them — a use-after-free that
// only the caller's discipline prevented (Class 62: a stranded out-param write). Writing on
// success alone makes the dangling state unrepresentable rather than merely discouraged, which
// is the structural form of the fix (feedback_structural_fix_over_belt_and_suspenders).
static inline int ws_ssl_setup(SSL_CTX **ctx_out, SSL **ssl_out, int sockfd, const char *host) {
    *ctx_out = NULL;
    *ssl_out = NULL;
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return -1;
    SSL *ssl = SSL_new(ctx);
    if (!ssl) { SSL_CTX_free(ctx); return -1; }
    SSL_set_fd(ssl, sockfd);
    SSL_set_tlsext_host_name(ssl, host);  // SNI required by Binance
    if (SSL_connect(ssl) <= 0) {
        SSL_free(ssl); SSL_CTX_free(ctx); return -1;
    }
    *ctx_out = ctx;
    *ssl_out = ssl;
    return 0;
}

//------------------------------------------------------------------
// [SECTION]_[WEBSOCKET HANDSHAKE]
//------------------------------------------------------------------
// Templated over the Io seam like the rest of the family. It was the ONE member left on a bare SSL*,
// so it was the one member the suite could not drive — and that is precisely where the unpadded
// base64 key sat undetected until 2026-09-05. A seam that stops short of a layer leaves that layer
// untested by construction; extend it rather than accept the hole.
template <class Io>
static inline int ws_handshake(Io &io, const char *host, const char *path) {
    unsigned char key_bytes[16];
    RAND_bytes(key_bytes, 16);
    char key_b64[32];
    ws_base64_encode(key_bytes, 16, key_b64);

    char req[512];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        path, host, key_b64);
    // snprintf returns what it WOULD have written: a truncated request would otherwise be sent
    // with a length past the buffer. Then insist the WHOLE request went out — `> 0` accepts a
    // partial write, which puts a half upgrade request on the wire and hangs the handshake read.
    if (n < 0 || n >= (int)sizeof(req)) return -1;
    if (io.write((const unsigned char *)req, n) != n) return -1;

    char resp[1024];
    int total = 0;
    int headers_complete = 0;
    while (total < (int)sizeof(resp) - 1) {
        int r = io.read((unsigned char *)resp + total, (int)sizeof(resp) - 1 - total);
        if (r <= 0) return -1;
        total += r;
        resp[total] = '\0';
        if (strstr(resp, "\r\n\r\n")) { headers_complete = 1; break; }
    }
    // A response that filled the buffer without terminating its headers is not a successful
    // upgrade — the old code fell out of this loop and went straight to the status test.
    if (!headers_complete) return -1;
    // Status-LINE, not "is 101 anywhere in the response". `strstr(resp, "101")` accepts
    // `HTTP/1.1 400 Bad Request\r\nX-Request-Id: 101ab…` as a completed handshake, and then the
    // frame reader parses an HTTP error body as WebSocket frames. RFC 6455 §4.1: the server
    // MUST reply 101, and it is the first token after the version on the first line.
    if (strncmp(resp, "HTTP/1.", 7) != 0) return -1;
    const char *sp = strchr(resp, ' ');
    if (!sp || strncmp(sp + 1, "101", 3) != 0) return -1;
    return 0;
}

//------------------------------------------------------------------
// [SECTION]_[READER RETURN CODES]
//------------------------------------------------------------------
// The transport seam itself is the WsSslIo struct block above this family.
enum : int {
    WS_READ_ERR       = -1,   // EOF / transport error mid-frame (the peer went away) — disconnect
    WS_READ_TOO_LARGE = -2,   // the 64-bit payload length exceeds the buffer; the payload was NOT consumed — the
                              // stream is DESYNCED from here, the caller MUST disconnect (never "skip and continue")
    WS_READ_PROTOCOL  = -3,   // the peer broke RFC 6455 §5.5 (an oversize or fragmented CONTROL frame). Same
                              // desync contract as TOO_LARGE: the payload was not consumed, so disconnect.
};

// RFC 6455 §5.5: a control frame (0x8 close / 0x9 ping / 0xA pong — the 0x08 bit of the opcode)
// carries at most 125 bytes and MUST NOT be fragmented. Enforcing it AT THE READER is what makes
// the pong path safe by construction: the reply builder no longer has to be the thing that
// defends against a 4 KB "ping", and a peer that sends one is broken in a way that is legible
// (a distinct return) rather than silent (a pong that never goes out, then a venue-side timeout).
enum : int { WS_CONTROL_MAX_PAYLOAD = 125 };

// Read exactly `want` bytes (loops over partial reads). 1 = done, 0 = EOF/error before `want`.
template <class Io>
static inline int ws_read_exact(Io &r, unsigned char *dst, int want) {
    int got = 0;
    while (got < want) {
        int n = r.read(dst + got, want - got);
        if (n <= 0) return 0;
        got += n;
    }
    return 1;
}

//------------------------------------------------------------------
// [SECTION]_[WEBSOCKET FRAME READER]
//------------------------------------------------------------------
// Returns the payload length (>= 0; out[len] = '\0') with *opcode / *fin set, or WS_READ_ERR /
// WS_READ_TOO_LARGE. Every field is read to completion; the length is compared as uint64_t against
// max_len BEFORE any narrowing (a 127-form length >= 2^31 cast to int is NEGATIVE and would pass a
// signed guard, skip the payload loop, and NUL-terminate at out[len] — out of bounds).
template <class Io>
static inline int ws_read_frame(Io &r, char *out, int max_len, int *opcode, int *fin) {
    unsigned char hdr[2];
    if (!ws_read_exact(r, hdr, 2)) return WS_READ_ERR;
    *fin    = (hdr[0] >> 7) & 1;
    *opcode = hdr[0] & 0x0F;
    const int masked = (hdr[1] >> 7) & 1;
    uint64_t plen = hdr[1] & 0x7F;

    if (plen == 126) {
        unsigned char ext[2];
        if (!ws_read_exact(r, ext, 2)) return WS_READ_ERR;
        plen = ((uint64_t)ext[0] << 8) | (uint64_t)ext[1];
    } else if (plen == 127) {
        unsigned char ext[8];
        if (!ws_read_exact(r, ext, 8)) return WS_READ_ERR;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | (uint64_t)ext[i];
    }

    // Checked before the mask + payload reads: the frame is already unusable, and consuming more
    // of it would only make the desync harder to reason about.
    if ((*opcode & 0x08) && (plen > (uint64_t)WS_CONTROL_MAX_PAYLOAD || *fin == 0))
        return WS_READ_PROTOCOL;

    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked && !ws_read_exact(r, mask, 4)) return WS_READ_ERR;

    if (max_len < 0 || plen > (uint64_t)max_len) return WS_READ_TOO_LARGE;   // BEFORE any int cast
    const int n = (int)plen;

    if (n > 0 && !ws_read_exact(r, (unsigned char *)out, n)) return WS_READ_ERR;
    if (masked) for (int i = 0; i < n; i++) out[i] = (char)((unsigned char)out[i] ^ mask[i & 3]);
    out[n] = '\0';
    return n;
}

//------------------------------------------------------------------
// [SECTION]_[FRAME BUILDERS — pure, mask injected]
//------------------------------------------------------------------
// Client→server frames MUST be masked (RFC 6455 §5.1). The builders take the mask so the suite can
// assert the exact bytes; the senders draw it from RAND_bytes. Returns the frame length, or 0 if it
// does not fit `cap`.
static inline int ws_build_pong(const char *payload, int len, const unsigned char mask[4],
                                unsigned char *frame, int cap) {
    if (len < 0 || len > 0xFFFF) return 0;
    const int hdr = (len < 126) ? 2 : 4;
    if (hdr + 4 + len > cap) return 0;
    int pos = 0;
    frame[pos++] = 0x8A;                                        // FIN + pong
    if (len < 126) {
        frame[pos++] = (unsigned char)(0x80 | len);             // mask bit + 7-bit length
    } else {
        frame[pos++] = (unsigned char)(0x80 | 126);
        frame[pos++] = (unsigned char)((len >> 8) & 0xFF);
        frame[pos++] = (unsigned char)(len & 0xFF);
    }
    memcpy(frame + pos, mask, 4); pos += 4;
    for (int i = 0; i < len; i++) frame[pos++] = (unsigned char)((unsigned char)payload[i] ^ mask[i & 3]);
    return pos;
}

// A masked close frame with an empty payload (6 bytes). RFC 6455 §5.5.1 allows the empty body.
static inline int ws_build_close(const unsigned char mask[4], unsigned char *frame) {
    frame[0] = 0x88;        // FIN + close
    frame[1] = 0x80;        // mask bit, length 0
    memcpy(frame + 2, mask, 4);
    return 6;
}

//------------------------------------------------------------------
// [SECTION]_[SENDERS]
//------------------------------------------------------------------
// The pong ECHOES the ping payload (RFC 6455 §5.5.3) — Binance closes a session whose pong does not
// (code 1008 "Pong timeout"; proven 2026-09-05). Ping payloads are tiny (Binance sends ~13 B); a
// payload that would not fit the 256 B stack frame is refused (0) rather than truncated (a truncated
// echo is a wrong echo). Returns 1 when the whole frame was written.
template <class Io>
static inline int ws_send_pong(Io &io, const char *payload, int len) {
    unsigned char frame[256];
    unsigned char mask[4];
    RAND_bytes(mask, 4);
    const int n = ws_build_pong(payload, len, mask, frame, (int)sizeof(frame));
    if (n <= 0) return 0;
    return (io.write(frame, n) == n) ? 1 : 0;
}

template <class Io>
static inline int ws_send_close(Io &io) {
    unsigned char frame[8];
    unsigned char mask[4];
    RAND_bytes(mask, 4);
    const int n = ws_build_close(mask, frame);
    return (io.write(frame, n) == n) ? 1 : 0;
}

//------------------------------------------------------------------
// [SECTION]_[WEBSOCKET CLOSE]
//------------------------------------------------------------------
static inline void ws_close(SSL *ssl, SSL_CTX *ctx, int sockfd) {
    WsSslIo io{ssl};
    ws_send_close(io);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(sockfd);
}

//------------------------------------------------------------------
// [SECTION]_[LIVENESS — pure helpers the client loops consume]
//------------------------------------------------------------------
// Stale = no frame for more than threshold_us. last_us == 0 means "no frame yet on this connection"
// (the pre-warmup window is the connect's own timeout, not staleness). Monotonic microseconds.
static inline int ws_stale(uint64_t now_us, uint64_t last_us, uint64_t threshold_us) {
    return (last_us != 0 && now_us > last_us && (now_us - last_us) > threshold_us) ? 1 : 0;
}

// Binance drops every WebSocket session at 24 h; reconnect proactively at 23 h 30 m (a 30-min buffer)
// so the cut lands at a moment the client chooses. The SSoT for the number — the trade client's
// literal (BinanceCrypto.hpp BinanceStream_ShouldReconnect) is to be pointed here.
static const uint64_t WS_PLANNED_RECONNECT_S = 23ULL * 3600ULL + 30ULL * 60ULL;   // 84600
static inline int ws_planned_reconnect_due(uint64_t now_s, uint64_t connect_s) {
    return (connect_s != 0 && now_s >= connect_s && (now_s - connect_s) >= WS_PLANNED_RECONNECT_S) ? 1 : 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ws_read_frame]
//======================================================================

#endif // WEBSOCKET_UTIL_HPP
