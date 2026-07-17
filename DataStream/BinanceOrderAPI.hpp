// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[DataStream/BinanceOrderAPI.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[HMAC-signed REST order placement/query — market buy/sell with venue-filter clamps, retry/backoff, clock sync; venue decimals ride the D-123 known-temporary double bridge (string-direct Money_FromString = .E.3); fee contract gaps tracked at TECH_DEBT-169]
// [CONTAINS]
//   - [STRUCT]_[SymbolFilters]   (order-status codes + BinanceErrorCode enum ride)
//   - [STRUCT]_[BinanceOrderAPI]
//   - [FUNCTION]_[binance_json_extract]   (+ current_ms / hmac wrapper / extract_str / extract_double / round_qty / step_decimals helper family)
//   - [FUNCTION]_[binance_rest_request]   (+ tcp_connect / tls_setup / signed_request / retry_request transport family)
//   - [FUNCTION]_[BinanceOrderAPI_MarketBuy]   (+ the public API family: Cleanup / MarketSell / CancelOrder / GetStatus / ServerTime / LoadFilters / GetBalance(s) / GetOpenOrders / GetMyTrades / SyncClock / Init)
//   - [FUNCTION]_[LoadSecrets]
//======================================================================================================
// places and manages orders via Binance REST API (https://api.binance.com/api/v3/order)
// uses HMAC-SHA256 signing for authentication
// separate SSL connection from the websocket data stream
//
// STATUS: HARDENED but not yet validated on a live exchange (2026-03-23)
// SSL response accumulation loop, retry with exponential backoff, exchange filter
// validation (LOT_SIZE, minNotional), fill price parsing, balance query, clock sync
//
// testnet: testnet.binance.vision (free test API keys, no real money, may need VPN from US)
// production: api.binance.com (real money, be careful)
// binance US: api.binance.us (for US-based users)
//
// all functions return 1 on success, 0 on failure
// order IDs are returned as strings (Binance uses uint64 but string is safer for portability)
//======================================================================================================
#ifndef BINANCE_ORDER_API_HPP
#define BINANCE_ORDER_API_HPP

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/tcp.h>  // v5.11.0.C — TCP_NODELAY / IPPROTO_TCP
#include "../CoreFrameworks/ParseFast.hpp"  // v5.11.4.A — std::from_chars wrapper
#include <errno.h>        // v5.11.0.C — strerror(errno) on setsockopt fail

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#include "BinanceCrypto.hpp"  // for g_binance_shutdown_flag (interruptible REST sleeps)
#include "../MemHeaders/HmacSha256.hpp"  // v5.3.0 Phase B — shared HMAC primitive (used by binance_hmac_sha256 wrapper below)

//======================================================================
// [STRUCT]_[SymbolFilters]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[venue-DEFINED trading constants as exact DECIMAL Money (D-106 source-authority; exchangeInfo strings <=8dp) — lot step/min/max + min_notional + derived qty_decimals (order-status codes + BinanceErrorCode ride)]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[ORDER STATUS CODES]
//------------------------------------------------------------------
#define ORDER_STATUS_UNKNOWN         0
#define ORDER_STATUS_NEW             1
#define ORDER_STATUS_PARTIALLY_FILLED 2
#define ORDER_STATUS_FILLED          3
#define ORDER_STATUS_CANCELED        4
#define ORDER_STATUS_REJECTED        5
#define ORDER_STATUS_EXPIRED         6

//------------------------------------------------------------------
// [SECTION]_[SYMBOL FILTERS — queried from /api/v3/exchangeInfo at init]
//------------------------------------------------------------------
// Binance error codes (subset that affects order routing decisions).
// Full list at https://binance-docs.github.io/apidocs/spot/en/#error-codes
enum BinanceErrorCode {
    BINANCE_OK               =     0,
    BINANCE_RATE_LIMIT       = -1003,  // too many requests
    BINANCE_LOT_SIZE         = -1013,  // invalid quantity (step size or min/max)
    BINANCE_TIMESTAMP        = -1021,  // timestamp for this request is outside recvWindow
    BINANCE_SIGNATURE        = -1022,  // signature verification failed
    BINANCE_INSUFFICIENT_BAL = -2010,  // insufficient balance
    BINANCE_DUPLICATE_ORDER  = -2010,  // same code as insufficient (context-dependent)
    BINANCE_UNKNOWN_ORDER    = -2013,  // order does not exist
    BINANCE_NOTIONAL         = -1013,  // below MIN_NOTIONAL (same code as LOT_SIZE)
};

struct SymbolFilters {
    // Ship-B P4 (D-106 source-authority): venue-DEFINED constants stored as DECIMAL
    // money — exact mirrors of the exchangeInfo strings (all <=8dp by venue contract).
    // Parse currently bridges through extract_double (exact for <=8dp via llround);
    // string-direct Money_FromString rides the .E.3 REST rework.
    Money lot_step_size;     // BTC: 0.00000100 — quantity must be multiple of this
    Money lot_min_qty;       // minimum order quantity
    Money lot_max_qty;       // maximum order quantity
    Money min_notional;      // minimum order value in quote asset (e.g. $10 USDT)
    int qty_decimals;        // decimal places for quantity formatting (derived from step_size)
    int loaded;              // 1 = filters fetched successfully
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — layout emitter cannot probe this block yet; quartet lands when the emitter covers it, D-327)
//======================================================================
// [END_STRUCT]_[SymbolFilters]
//======================================================================

//======================================================================
// [STRUCT]_[BinanceOrderAPI]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-thread REST instance — socket/SSL + credentials + clock offset + filters + reconnect rate-limit + staleness/error/rate-weight observability; NEVER shared across threads]
//======================================================================
// [CODE]
//======================================================================
struct BinanceOrderAPI {
    int sockfd;
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    char api_key[128];
    char api_secret[128];
    char host[64];
    char symbol[16];       // uppercase, e.g. "BTCUSDT"
    int connected;
    int64_t time_offset_ms; // local - server time difference
    SymbolFilters filters;
    int64_t last_reconnect_ms; // rate-limit reconnects to once per 5s
    int64_t last_request_ms;   // timestamp of last successful REST request (staleness detection)
    int last_error_code;       // Binance error code from last failed request (0 = none)
    int rate_limit_weight;     // X-MBX-USED-WEIGHT-1m from last response
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — layout emitter cannot probe this block yet; quartet lands when the emitter covers it, D-327)
//======================================================================
// [END_STRUCT]_[BinanceOrderAPI]
//======================================================================

//======================================================================
// [FUNCTION]_[binance_json_extract]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the helper family (current_ms / the shared-HMAC wrapper / extract_str / extract_double / round_qty / step_decimals ride) — flat-JSON extract; locale-immune parse_double_fast_n; exact-decimal quantize core in a double shell (.E.3)]
//======================================================================
// [CODE]
//======================================================================
static inline int64_t binance_current_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// v5.3.0 Phase B — thin wrapper over MemHeaders/HmacSha256.hpp's
// tt::hmac_sha256_hex. The shared primitive is the same openssl HMAC
// call this used to do inline; consolidating means stamp signing +
// Binance signing share one tested path.
static inline void binance_hmac_sha256(const char *key, const char *data, char *hex_out) {
    if (!tt::hmac_sha256_hex(key, data, hex_out)) {
        // Failure path: fill with zeros so a corrupt sig won't accidentally
        // collide with anything; caller will detect via REST 401.
        memset(hex_out, '0', 64);
        hex_out[64] = '\0';
    }
}

// simple JSON value extractor — finds "key":"value" or "key":number
// returns pointer to value start in buf, writes length to *out_len
// works for the flat JSON objects Binance returns
static inline const char* binance_json_extract(const char *json, const char *key,
                                                int *out_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) { *out_len = 0; return NULL; }
    pos += strlen(search);
    // skip whitespace
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos == '"') {
        // string value — find closing quote
        pos++; // skip opening quote
        const char *end = strchr(pos, '"');
        if (!end) { *out_len = 0; return NULL; }
        *out_len = (int)(end - pos);
        return pos;
    } else {
        // numeric value — read until comma/brace/bracket
        const char *end = pos;
        while (*end && *end != ',' && *end != '}' && *end != ']') end++;
        *out_len = (int)(end - pos);
        return pos;
    }
}

static inline void binance_json_extract_str(const char *json, const char *key,
                                             char *out, int out_size) {
    int len;
    const char *val = binance_json_extract(json, key, &len);
    if (val && len > 0 && len < out_size) {
        memcpy(out, val, len);
        out[len] = '\0';
    } else {
        out[0] = '\0';
    }
}

static inline double binance_json_extract_double(const char *json, const char *key) {
    int len;
    const char *val = binance_json_extract(json, key, &len);
    if (!val || len == 0) return 0.0;
    // v5.11.4.A — std::from_chars: locale-immune (no LC_NUMERIC dependency)
    // + branchless on well-formed inputs. parse_double_fast_n takes the
    // (val, len) span directly; no NUL-termination round-trip needed.
    return tt::parse_double_fast_n(val, (size_t)len);
}

// truncate quantity to exchange step size (always rounds down, no math.h needed)
// positive quantities only (always true for order sizing)
static inline double binance_round_qty(double qty, Money step_size) {
    // Ship-B P4: the quantize core is EXACT decimal (#6 Money_QuantizeToStep via the
    // certified divider) — the double shell remains for the .E.3 REST plumbing.
    if (step_size.v <= 0) return qty;
    Money q = Money{ money_from_double_payload(qty) };
    return Money_ToDouble(Money_QuantizeToStep(q, step_size));
}

// count decimal places in step size for quantity formatting
static inline int binance_step_decimals(double step_size) {
    int d = 0;
    while (step_size < 0.999999 && d < 10) { step_size *= 10.0; d++; }
    return d;
}

//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[binance_json_extract]
//======================================================================

//======================================================================
// [FUNCTION]_[binance_rest_request]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the transport family (rest_tcp_connect / rest_tls_setup / signed_request / retry_request ride) — keep-alive HTTP/1.1 over SSL with reconnect-and-resend; HMAC signing; 5xx/418/429 backoff retry]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[TCP + TLS — same pattern as BinanceCrypto.hpp]
//------------------------------------------------------------------
static inline int binance_rest_tcp_connect(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(host, port, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "[REST] getaddrinfo failed: %s\n", gai_strerror(err));
        return -1;
    }

    int sockfd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) continue;
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);
    if (sockfd == -1) {
        fprintf(stderr, "[REST] TCP connect failed to %s:%s\n", host, port);
        return -1;
    }

    // v5.11.0.C — Disable Nagle's algorithm. Order packets must hit the
    // wire immediately; default Linux TCP buffers up to 40ms for coalescing.
    // Audit: LATENCY_OPTIMIZATION_AUDIT.md Part 12.1.
    {
        int one = 1;
        if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
            fprintf(stderr, "[REST] setsockopt(TCP_NODELAY) failed: %s\n",
                    strerror(errno));
        }
    }

    // set socket read timeout — prevents SSL_read from blocking on keep-alive
    struct timeval tv = {5, 0}; // 5 second timeout
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return sockfd;
}

static inline int binance_rest_tls_setup(BinanceOrderAPI *api) {
    // ssl_ctx is persistent (created once at init, freed only in Cleanup)
    api->ssl = SSL_new(api->ssl_ctx);
    if (!api->ssl) return 0;

    SSL_set_fd(api->ssl, api->sockfd);
    SSL_set_tlsext_host_name(api->ssl, api->host);

    int ret = SSL_connect(api->ssl);
    if (ret != 1) {
        fprintf(stderr, "[REST] SSL_connect failed: %d\n", SSL_get_error(api->ssl, ret));
        SSL_free(api->ssl);
        api->ssl = NULL;
        return 0;
    }
    return 1;
}

//------------------------------------------------------------------
// [SECTION]_[HTTP REQUEST]
//------------------------------------------------------------------
// sends an HTTP/1.1 request over SSL and reads the response
// returns HTTP status code (200, 400, etc.) or -1 on error
// response body written to response_buf
static inline int binance_rest_request(BinanceOrderAPI *api,
                                        const char *method,
                                        const char *path,
                                        const char *query,
                                        char *response_buf, int buf_size) {
    // proactive staleness check: reconnect if idle too long
    // reconnect if needed
    if (!api->connected || !api->ssl) {
        if (api->ssl) { SSL_free(api->ssl); api->ssl = NULL; }
        if (api->sockfd >= 0) { close(api->sockfd); api->sockfd = -1; }

        api->sockfd = binance_rest_tcp_connect(api->host, "443");
        if (api->sockfd < 0) return -1;
        if (!binance_rest_tls_setup(api)) return -1;
        api->connected = 1;
        fprintf(stderr, "[REST] reconnected to %s\n", api->host);
    }

    // build request
    char request[4096];
    int req_len;

    if (strcmp(method, "POST") == 0) {
        req_len = snprintf(request, sizeof(request),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "X-MBX-APIKEY: %s\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: %d\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "%s",
            method, path, api->host, api->api_key, (int)strlen(query), query);
    } else {
        // GET or DELETE — query goes in URL
        req_len = snprintf(request, sizeof(request),
            "%s %s?%s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "X-MBX-APIKEY: %s\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            method, path, query, api->host, api->api_key);
    }

    int written = SSL_write(api->ssl, request, req_len);
    if (written <= 0) {
        int ssl_err = SSL_get_error(api->ssl, written);
        fprintf(stderr, "[REST] SSL_write failed (err=%d)\n", ssl_err);
        api->connected = 0;
        // retry with fresh connection immediately (don't return -1 on first attempt)
        if (api->ssl) { SSL_free(api->ssl); api->ssl = NULL; }
        if (api->sockfd >= 0) { close(api->sockfd); api->sockfd = -1; }
        api->sockfd = binance_rest_tcp_connect(api->host, "443");
        if (api->sockfd < 0) return -1;
        if (!binance_rest_tls_setup(api)) return -1;
        api->connected = 1;
        // resend on the fresh connection
        written = SSL_write(api->ssl, request, req_len);
        if (written <= 0) {
            fprintf(stderr, "[REST] SSL_write failed after reconnect\n");
            api->connected = 0;
            return -1;
        }
    }

    // read response — accumulate until headers + body complete
    char raw[8192];
    int total = 0;
    while (total < (int)sizeof(raw) - 1) {
        int n = SSL_read(api->ssl, raw + total, (int)sizeof(raw) - 1 - total);
        if (n <= 0) {
            if (total == 0) {
                fprintf(stderr, "[REST] SSL_read failed\n");
                api->connected = 0;
                return -1;
            }
            break; // got some data, server closed — use what we have
        }
        total += n;
        raw[total] = '\0';
        // check if response is complete (headers + body received)
        const char *hdr_end = strstr(raw, "\r\n\r\n");
        if (hdr_end) {
            // look for Content-Length to know when body is complete
            const char *cl = strstr(raw, "Content-Length: ");
            if (cl && cl < hdr_end) {
                int content_len = atoi(cl + 16);
                int body_start = (int)(hdr_end + 4 - raw);
                if (total - body_start >= content_len) break; // got full body
            } else {
                break; // no Content-Length, assume complete after first read with headers
            }
        }
    }
    raw[total] = '\0';

    // parse HTTP status
    int status = 0;
    if (strncmp(raw, "HTTP/1.1 ", 9) == 0)
        status = atoi(raw + 9);

    // find body (after \r\n\r\n)
    const char *body = strstr(raw, "\r\n\r\n");
    if (body) {
        body += 4;
        int body_len = total - (int)(body - raw);
        if (body_len >= buf_size) body_len = buf_size - 1;
        memcpy(response_buf, body, body_len);
        response_buf[body_len] = '\0';
    } else {
        response_buf[0] = '\0';
    }

    // Parse rate limit weight from response headers (before discarding them).
    // Binance sends: X-MBX-USED-WEIGHT-1m: 42
    const char* weight_hdr = strstr(raw, "X-MBX-USED-WEIGHT-1m: ");
    if (weight_hdr && weight_hdr < body) {
        api->rate_limit_weight = atoi(weight_hdr + 22);
    }

    // close connection after each request — ssl_ctx persists, only SSL object cycles
    // the same-tick guard in main.cpp prevents back-to-back calls that caused heap corruption
    if (api->ssl) { SSL_shutdown(api->ssl); SSL_free(api->ssl); api->ssl = NULL; }
    close(api->sockfd); api->sockfd = -1;
    api->connected = 0;

    return status;
}

//------------------------------------------------------------------
// [SECTION]_[SIGN + SEND HELPERS]
//------------------------------------------------------------------
static inline int binance_signed_request(BinanceOrderAPI *api,
                                          const char *method, const char *path,
                                          const char *params,
                                          char *response_buf, int buf_size) {
    // add recvWindow, timestamp, and signature
    char query[2048];
    int64_t ts = binance_current_ms() + api->time_offset_ms;
    if (params[0] != '\0')
        snprintf(query, sizeof(query), "%s&recvWindow=5000&timestamp=%lld", params, (long long)ts);
    else
        snprintf(query, sizeof(query), "recvWindow=5000&timestamp=%lld", (long long)ts);

    char signature[128];
    binance_hmac_sha256(api->api_secret, query, signature);

    char signed_query[2048];
    snprintf(signed_query, sizeof(signed_query), "%s&signature=%s", query, signature);

    return binance_rest_request(api, method, path, signed_query, response_buf, buf_size);
}

// retry wrapper — retries on 5xx/418/429, gives up on 4xx client errors
static inline int binance_retry_request(BinanceOrderAPI *api,
                                         const char *method, const char *path,
                                         const char *params,
                                         char *response_buf, int buf_size) {
    int delays[] = {0, 1, 2, 4};
    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt > 0) {
            fprintf(stderr, "[REST] retry %d/3 after %ds...\n", attempt, delays[attempt]);
            // Interruptible sleep — same pattern as BinanceStream_Reconnect.
            // Without this, an in-flight retry blocks engine shutdown for up
            // to 4 seconds. Polls g_binance_shutdown_flag every 100ms and
            // bails out early when set; caller (drainer) sees the failure
            // status and the outer loop exits via shutdown check.
            for (int s = 0; s < delays[attempt]; ++s) {
                for (int j = 0; j < 10; ++j) {
                    if (g_binance_shutdown_flag && *g_binance_shutdown_flag) {
                        return -1;
                    }
                    struct timespec ts = {0, 100000000};
                    nanosleep(&ts, NULL);
                }
            }
        }
        int status = binance_signed_request(api, method, path, params,
                                             response_buf, buf_size);
        if (status == 200) return status;
        if (status >= 400 && status < 500 && status != 418 && status != 429) {
            // client error — parse Binance error code for classification
            char msg[128];
            binance_json_extract_str(response_buf, "msg", msg, sizeof(msg));
            int code = (int)binance_json_extract_double(response_buf, "code");
            api->last_error_code = code;
            if (code != 0)
                fprintf(stderr, "[REST] Binance error %d: %s\n", code, msg);
            // timestamp errors are retryable (clock drift) — re-fetch
            // server time inline to resync. full SyncClock is defined later
            // in the file so we use the server time endpoint directly.
            if (code == BINANCE_TIMESTAMP) {
                fprintf(stderr, "[REST] timestamp error, resync + retry\n");
                char time_body[256];
                int ts = binance_rest_request(api, "GET", "/api/v3/time", "",
                                               time_body, sizeof(time_body));
                if (ts == 200) {
                    int64_t server_ms = (int64_t)binance_json_extract_double(time_body, "serverTime");
                    api->time_offset_ms = binance_current_ms() - server_ms;
                }
                continue;
            }
            // all other 4xx: don't retry (LOT_SIZE, insufficient balance, etc.)
            return status;
        }
        // 418/429 (rate limit): wait longer before retry
        if (status == 418 || status == 429) {
            fprintf(stderr, "[REST] rate limited (HTTP %d), weight=%d\n",
                    status, api->rate_limit_weight);
            // Interruptible — see comment above on the retry sleep.
            int wait_secs = delays[attempt] + 5;
            for (int s = 0; s < wait_secs; ++s) {
                for (int j = 0; j < 10; ++j) {
                    if (g_binance_shutdown_flag && *g_binance_shutdown_flag) {
                        return -1;
                    }
                    struct timespec ts = {0, 100000000};
                    nanosleep(&ts, NULL);
                }
            }
        }
    }
    fprintf(stderr, "[REST] all retries failed\n");
    return -1;
}

//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[binance_rest_request]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceOrderAPI_MarketBuy]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the public API family (Cleanup / MarketSell / CancelOrder / GetStatus / ServerTime / LoadFilters / GetBalance(s) / GetOpenOrders / GetMyTrades / SyncClock / Init ride) — market orders with step/min/max/notional clamps (D-175) + venue order_complete (A17); fills parse via the D-123 double bridge]
//======================================================================
// [CODE]
//======================================================================
static inline void BinanceOrderAPI_Cleanup(BinanceOrderAPI *api) {
    if (api->ssl) { SSL_shutdown(api->ssl); SSL_free(api->ssl); api->ssl = NULL; }
    if (api->ssl_ctx) { SSL_CTX_free(api->ssl_ctx); api->ssl_ctx = NULL; }
    if (api->sockfd >= 0) { close(api->sockfd); api->sockfd = -1; }
    api->connected = 0;
}

// place a market buy order — returns 1 on success, 0 on failure
// order_id_out receives the Binance order ID as a string
// fill_price_out/fill_qty_out receive actual execution values (NULL = don't care)
static inline int BinanceOrderAPI_MarketBuy(BinanceOrderAPI *api,
                                             double quantity,
                                             char *order_id_out,
                                             double *fill_price_out = NULL,
                                             double *fill_qty_out = NULL,
                                             uint64_t client_order_id = 0, int *order_complete_out = NULL) {
    // round quantity to exchange step size + clamp to the venue max (Ship-B P4
    // ratified pick, D-175: lot_max_qty was parsed-never-consumed — an oversized
    // order would be venue-REJECTED; clamping + warning keeps the entry alive).
    if (api->filters.loaded) {
        quantity = binance_round_qty(quantity, api->filters.lot_step_size);
        double max_q = Money_ToDouble(api->filters.lot_max_qty);
        if (max_q > 0.0 && quantity > max_q) {
            fprintf(stderr, "[order-api] qty %.8f CLAMPED to venue lot_max_qty %.8f\n",
                    quantity, max_q);
            quantity = binance_round_qty(max_q, api->filters.lot_step_size);
        }
    }

    char qty_str[32];
    snprintf(qty_str, sizeof(qty_str), "%.*f", api->filters.qty_decimals, quantity);

    char params[256];
    if (client_order_id != 0) {
        snprintf(params, sizeof(params),
                 "symbol=%s&side=BUY&type=MARKET&quantity=%s&newClientOrderId=oms_%llu",
                 api->symbol, qty_str, (unsigned long long)client_order_id);
    } else {
        snprintf(params, sizeof(params),
                 "symbol=%s&side=BUY&type=MARKET&quantity=%s",
                 api->symbol, qty_str);
    }

    char body[2048];
    int status = binance_retry_request(api, "POST", "/api/v3/order", params,
                                        body, sizeof(body));

    if (status == 200) {
        binance_json_extract_str(body, "orderId", order_id_out, 32);
        double exec_qty = binance_json_extract_double(body, "executedQty");
        double cum_quote = binance_json_extract_double(body, "cummulativeQuoteQty");
        double avg_price = (exec_qty > 0) ? cum_quote / exec_qty : 0.0;
        if (fill_price_out) *fill_price_out = avg_price;
        if (fill_qty_out) *fill_qty_out = exec_qty;
        // A17 (.E.0.10) + D-106 "let the venue decide": read the venue's OWN "status"
        // (FILLED/PARTIALLY_FILLED) — never re-derive completeness from exec_qty vs the
        // requested qty (lot-rounding-fragile). Mirrors GetStatus(:691) + the WS "X" SSoT.
        // Missing/unreadable -> 0 (assume partial; keep the slot alive, the WS default).
        if (order_complete_out) {
            char ostatus[32] = {};
            binance_json_extract_str(body, "status", ostatus, sizeof(ostatus));
            *order_complete_out = (strcmp(ostatus, "FILLED") == 0);
        }
        fprintf(stderr, "[REST] BUY filled: id=%s qty=%.8f price=%.2f\n",
                order_id_out, exec_qty, avg_price);
        return 1;
    } else {
        fprintf(stderr, "[REST] BUY failed (status %d): %s\n", status, body);
        order_id_out[0] = '\0';
        return 0;
    }
}

// place a market sell order
static inline int BinanceOrderAPI_MarketSell(BinanceOrderAPI *api,
                                              double quantity,
                                              char *order_id_out,
                                              double *fill_price_out = NULL,
                                              double *fill_qty_out = NULL,
                                              uint64_t client_order_id = 0, int *order_complete_out = NULL) {
    if (api->filters.loaded) {
        quantity = binance_round_qty(quantity, api->filters.lot_step_size);
        double max_q = Money_ToDouble(api->filters.lot_max_qty);   // D-175 ratified clamp (sell side)
        if (max_q > 0.0 && quantity > max_q) {
            fprintf(stderr, "[order-api] sell qty %.8f CLAMPED to venue lot_max_qty %.8f\n",
                    quantity, max_q);
            quantity = binance_round_qty(max_q, api->filters.lot_step_size);
        }
    }

    char qty_str[32];
    snprintf(qty_str, sizeof(qty_str), "%.*f", api->filters.qty_decimals, quantity);

    char params[256];
    if (client_order_id != 0) {
        snprintf(params, sizeof(params),
                 "symbol=%s&side=SELL&type=MARKET&quantity=%s&newClientOrderId=oms_%llu",
                 api->symbol, qty_str, (unsigned long long)client_order_id);
    } else {
        snprintf(params, sizeof(params),
                 "symbol=%s&side=SELL&type=MARKET&quantity=%s",
                 api->symbol, qty_str);
    }

    char body[2048];
    int status = binance_retry_request(api, "POST", "/api/v3/order", params,
                                        body, sizeof(body));

    if (status == 200) {
        binance_json_extract_str(body, "orderId", order_id_out, 32);
        double exec_qty = binance_json_extract_double(body, "executedQty");
        double cum_quote = binance_json_extract_double(body, "cummulativeQuoteQty");
        double avg_price = (exec_qty > 0) ? cum_quote / exec_qty : 0.0;
        if (fill_price_out) *fill_price_out = avg_price;
        if (fill_qty_out) *fill_qty_out = exec_qty;
        // A17 (.E.0.10) + D-106: the venue's own "status" terminal flag (mirror of MarketBuy).
        if (order_complete_out) {
            char ostatus[32] = {};
            binance_json_extract_str(body, "status", ostatus, sizeof(ostatus));
            *order_complete_out = (strcmp(ostatus, "FILLED") == 0);
        }
        fprintf(stderr, "[REST] SELL filled: id=%s qty=%.8f price=%.2f\n",
                order_id_out, exec_qty, avg_price);
        return 1;
    } else {
        fprintf(stderr, "[REST] SELL failed (status %d): %s\n", status, body);
        order_id_out[0] = '\0';
        return 0;
    }
}

// v5.14.4.0 — DELETE /api/v3/order. Cancels a working order by its
// exchange-side orderId. Reuses existing binance_signed_request +
// retry infrastructure (mirrors GetStatus's GET shape exactly).
//
// Returns:
//   1 on success (HTTP 200; order cancelled or already in terminal state
//     per Binance — the API returns 200 even if the order was already
//     filled before the cancel arrived; that's still a "successful no-op"
//     from operator's POV)
//   0 on failure (network error, invalid orderId, exchange refused)
//
// USED BY:
//   - v5.14.4.B Reconcile_AutoCancelStale (zombie-order cleanup at boot
//     when AUTO_SYNC mode + exchange has open orders not in our OMS)
//   - v5.14.7+ (TBD per master plan; manual operator cancel UI)
//
// Built ONCE here; both callers reuse — single source of truth for
// the cancel primitive (reuse-audit per CLAUDE.md item 16).
//
// Future-thinking: if v5.X+ adds bulk-cancel-by-symbol (DELETE
// /api/v3/openOrders), build that as a sister function alongside
// (NOT a special case of CancelOrder) — different endpoint shape +
// different operator semantics.
static inline int BinanceOrderAPI_CancelOrder(BinanceOrderAPI *api,
                                                const char *order_id) {
    if (!api || !order_id || order_id[0] == '\0') {
        fprintf(stderr, "[REST] CANCEL: invalid args (api=%p order_id=%s)\n",
                (void*)api, order_id ? order_id : "(null)");
        return 0;
    }
    char params[256];
    snprintf(params, sizeof(params), "symbol=%s&orderId=%s",
             api->symbol, order_id);

    char body[2048];
    int status = binance_signed_request(api, "DELETE", "/api/v3/order",
                                          params, body, sizeof(body));

    if (status == 200) {
        // Binance returns 200 + JSON body with the cancelled order's
        // final state. Operator-side: log the cancel for audit trail;
        // OMS state cleanup happens at caller (reconcile path).
        fprintf(stderr, "[REST] CANCEL ok: orderId=%s\n", order_id);
        return 1;
    } else {
        // 404 = order not found (already cancelled / never existed);
        // 400 = invalid params; 401 = auth; 5xx = exchange issue.
        // All non-200 → operator notification + caller decides
        // strict-mode action (refuse vs continue).
        fprintf(stderr, "[REST] CANCEL failed (status %d): orderId=%s body=%s\n",
                status, order_id, body);
        return 0;
    }
}

// check order status — returns ORDER_STATUS_* constant
// fills filled_qty and avg_price on success
static inline int BinanceOrderAPI_GetStatus(BinanceOrderAPI *api,
                                             const char *order_id,
                                             double *filled_qty,
                                             double *avg_price) {
    char params[256];
    snprintf(params, sizeof(params), "symbol=%s&orderId=%s", api->symbol, order_id);

    char body[2048];
    int status = binance_signed_request(api, "GET", "/api/v3/order", params,
                                         body, sizeof(body));

    if (status != 200) {
        fprintf(stderr, "[REST] status check failed (HTTP %d): %s\n", status, body);
        return ORDER_STATUS_UNKNOWN;
    }

    // extract status string
    char status_str[32];
    binance_json_extract_str(body, "status", status_str, sizeof(status_str));

    if (filled_qty)
        *filled_qty = binance_json_extract_double(body, "executedQty");
    if (avg_price) {
        // Binance returns cummulativeQuoteQty / executedQty = avg price
        double cumulative = binance_json_extract_double(body, "cummulativeQuoteQty");
        double executed = binance_json_extract_double(body, "executedQty");
        *avg_price = (executed > 0) ? cumulative / executed : 0.0;
    }

    if (strcmp(status_str, "NEW") == 0)              return ORDER_STATUS_NEW;
    if (strcmp(status_str, "PARTIALLY_FILLED") == 0) return ORDER_STATUS_PARTIALLY_FILLED;
    if (strcmp(status_str, "FILLED") == 0)           return ORDER_STATUS_FILLED;
    if (strcmp(status_str, "CANCELED") == 0)          return ORDER_STATUS_CANCELED;
    if (strcmp(status_str, "REJECTED") == 0)          return ORDER_STATUS_REJECTED;
    if (strcmp(status_str, "EXPIRED") == 0)            return ORDER_STATUS_EXPIRED;
    return ORDER_STATUS_UNKNOWN;
}

// get server time (milliseconds) — for clock calibration
static inline int64_t BinanceOrderAPI_ServerTime(BinanceOrderAPI *api) {
    char body[256];
    int status = binance_rest_request(api, "GET", "/api/v3/time", "", body, sizeof(body));
    if (status == 200)
        return (int64_t)binance_json_extract_double(body, "serverTime");
    return 0;
}

// load exchange filters for the configured symbol (LOT_SIZE, NOTIONAL)
// returns 1 on success, 0 on failure (caller should treat as fatal)
static inline int BinanceOrderAPI_LoadFilters(BinanceOrderAPI *api) {
    char query[64];
    snprintf(query, sizeof(query), "symbol=%s", api->symbol);

    char body[4096]; // exchangeInfo for one symbol is ~2KB
    int status = binance_rest_request(api, "GET", "/api/v3/exchangeInfo", query,
                                       body, sizeof(body));
    if (status != 200) {
        fprintf(stderr, "[REST] exchangeInfo failed (HTTP %d)\n", status);
        return 0;
    }

    // parse LOT_SIZE filter
    const char *lot = strstr(body, "LOT_SIZE");
    if (lot) {
        api->filters.lot_min_qty  = Money{ money_from_double_payload(binance_json_extract_double(lot, "minQty")) };
        api->filters.lot_max_qty  = Money{ money_from_double_payload(binance_json_extract_double(lot, "maxQty")) };
        api->filters.lot_step_size = Money{ money_from_double_payload(binance_json_extract_double(lot, "stepSize")) };
        api->filters.qty_decimals = binance_step_decimals(Money_ToDouble(api->filters.lot_step_size));
    }

    // parse NOTIONAL filter (or MIN_NOTIONAL for older API)
    const char *notional = strstr(body, "NOTIONAL");
    if (notional)
        api->filters.min_notional = Money{ money_from_double_payload(binance_json_extract_double(notional, "minNotional")) };

    api->filters.loaded = 1;
    fprintf(stderr, "[REST] filters: step=%.8f minQty=%.8f minNotional=%.2f decimals=%d\n",
            api->filters.lot_step_size, api->filters.lot_min_qty,
            api->filters.min_notional, api->filters.qty_decimals);
    return 1;
}

// query account balance for a specific asset (e.g. "USDT", "BTC")
// returns 1 on success, 0 on failure
static inline int BinanceOrderAPI_GetBalance(BinanceOrderAPI *api,
                                              const char *asset,
                                              double *free_balance) {
    char body[8192]; // account response can be large (many assets)
    int status = binance_retry_request(api, "GET", "/api/v3/account", "",
                                        body, sizeof(body));
    if (status != 200) {
        fprintf(stderr, "[REST] account query failed (HTTP %d)\n", status);
        return 0;
    }

    // find the asset in the balances array
    // format: "asset":"USDT","free":"1000000.00","locked":"0.00"
    char search[32];
    snprintf(search, sizeof(search), "\"asset\":\"%s\"", asset);
    const char *pos = strstr(body, search);
    if (!pos) {
        *free_balance = 0.0;
        return 0;
    }

    *free_balance = binance_json_extract_double(pos, "free");
    return 1;
}

// v5.2.1 (live reconciliation Phase 1): fetch open orders for the symbol.
// `body` receives raw JSON array. Caller parses (or passes to
// OrderManager_Reconcile which has the parser). Returns HTTP status.
//
// Why raw JSON here, parser in OrderManager: keeps the network layer
// thin + testable with mock JSON, and keeps the reconcile logic
// network-independent (testable without real REST calls).
static inline int BinanceOrderAPI_GetOpenOrders(BinanceOrderAPI *api,
                                                  char *body, size_t body_cap) {
    char params[128];
    snprintf(params, sizeof(params), "symbol=%s", api->symbol);
    return binance_retry_request(api, "GET", "/api/v3/openOrders", params,
                                  body, body_cap);
}

// v5.2.1: fetch recent trades (fills) for the symbol since `since_trade_id`.
// Pass since_trade_id=0 for "last 100 trades" (Binance default). Pass
// the last-known-processed trade id to catch only new fills.
static inline int BinanceOrderAPI_GetMyTrades(BinanceOrderAPI *api,
                                               int64_t since_trade_id,
                                               char *body, size_t body_cap) {
    char params[256];
    if (since_trade_id > 0) {
        snprintf(params, sizeof(params), "symbol=%s&fromId=%lld&limit=500",
                 api->symbol, (long long)since_trade_id);
    } else {
        snprintf(params, sizeof(params), "symbol=%s&limit=100", api->symbol);
    }
    return binance_retry_request(api, "GET", "/api/v3/myTrades", params,
                                  body, body_cap);
}

// query both USDT and BTC balances in a single API call
// returns 1 on success, 0 on failure
static inline int BinanceOrderAPI_GetBalances(BinanceOrderAPI *api,
                                               double *usdt_out,
                                               double *btc_out) {
    char body[8192];
    int status = binance_retry_request(api, "GET", "/api/v3/account", "",
                                        body, sizeof(body));
    if (status != 200) {
        fprintf(stderr, "[REST] account query failed (HTTP %d)\n", status);
        return 0;
    }

    *usdt_out = 0.0;
    *btc_out = 0.0;

    const char *pos = strstr(body, "\"asset\":\"USDT\"");
    if (pos) *usdt_out = binance_json_extract_double(pos, "free");

    pos = strstr(body, "\"asset\":\"BTC\"");
    if (pos) *btc_out = binance_json_extract_double(pos, "free");

    return 1;
}

// re-sync clock offset (call periodically or after reconnect)
static inline void BinanceOrderAPI_SyncClock(BinanceOrderAPI *api) {
    int64_t server_time = BinanceOrderAPI_ServerTime(api);
    if (server_time > 0) {
        int64_t local_time = binance_current_ms();
        int64_t old_offset = api->time_offset_ms;
        api->time_offset_ms = server_time - local_time;
        if (api->time_offset_ms != old_offset)
            fprintf(stderr, "[REST] clock re-synced: %lldms → %lldms\n",
                    (long long)old_offset, (long long)api->time_offset_ms);
    }
}

// init: connect to REST endpoint, calibrate clock, load exchange filters
// must be called after Cleanup, ServerTime, SyncClock, LoadFilters are defined
static inline int BinanceOrderAPI_Init(BinanceOrderAPI *api, const char *host,
                                        const char *api_key, const char *api_secret,
                                        const char *symbol) {
    memset(api, 0, sizeof(*api));
    api->sockfd = -1;
    strncpy(api->host, host, sizeof(api->host) - 1);
    strncpy(api->api_key, api_key, sizeof(api->api_key) - 1);
    strncpy(api->api_secret, api_secret, sizeof(api->api_secret) - 1);

    // uppercase symbol for REST API
    for (int i = 0; symbol[i] && i < (int)sizeof(api->symbol) - 1; i++)
        api->symbol[i] = (symbol[i] >= 'a' && symbol[i] <= 'z')
            ? symbol[i] - 32 : symbol[i];

    // create SSL context once — reused across all connections, freed only in Cleanup
    api->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!api->ssl_ctx) {
        fprintf(stderr, "[REST] SSL_CTX_new failed\n");
        return 0;
    }

    // connect
    api->sockfd = binance_rest_tcp_connect(host, "443");
    if (api->sockfd < 0) return 0;
    if (!binance_rest_tls_setup(api)) return 0;
    api->connected = 1;
    api->last_request_ms = binance_current_ms();

    // calibrate clock offset
    BinanceOrderAPI_SyncClock(api);

    // load exchange filters (LOT_SIZE, NOTIONAL) — fatal if fails
    if (!BinanceOrderAPI_LoadFilters(api)) {
        fprintf(stderr, "[REST] FATAL: could not load exchange filters for %s\n", api->symbol);
        BinanceOrderAPI_Cleanup(api);
        return 0;
    }

    return 1;
}

//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[BinanceOrderAPI_MarketBuy]
//======================================================================

//======================================================================
// [FUNCTION]_[LoadSecrets]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[api_key/api_secret from a key=value secrets file (engine.cfg format; inline #-comments stripped) — 1 only when BOTH present]
//======================================================================
// [CODE]
//======================================================================
static inline int LoadSecrets(const char *filepath, char *key_out, char *secret_out) {
    key_out[0] = '\0';
    secret_out[0] = '\0';

    FILE *f = fopen(filepath, "r");
    if (!f) {
        fprintf(stderr, "[SECRETS] file not found: %s\n", filepath);
        return 0;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        char *val = eq + 1;

        // strip inline comments
        char *comment = strchr(val, '#');
        if (comment) *comment = '\0';
        int vlen = strlen(val);
        while (vlen > 0 && (val[vlen-1] == ' ' || val[vlen-1] == '\t')) val[--vlen] = '\0';

        if (strcmp(key, "api_key") == 0)
            strncpy(key_out, val, 127);
        else if (strcmp(key, "api_secret") == 0)
            strncpy(secret_out, val, 127);
    }

    fclose(f);

    if (key_out[0] == '\0' || secret_out[0] == '\0') {
        fprintf(stderr, "[SECRETS] api_key or api_secret not found in %s\n", filepath);
        return 0;
    }
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[LoadSecrets]
//======================================================================

#endif // BINANCE_ORDER_API_HPP
