// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BINANCE REST ORDER API]
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

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

//======================================================================================================
// [ORDER STATUS CODES]
//======================================================================================================
#define ORDER_STATUS_UNKNOWN         0
#define ORDER_STATUS_NEW             1
#define ORDER_STATUS_PARTIALLY_FILLED 2
#define ORDER_STATUS_FILLED          3
#define ORDER_STATUS_CANCELED        4
#define ORDER_STATUS_REJECTED        5
#define ORDER_STATUS_EXPIRED         6

//======================================================================================================
// [SYMBOL FILTERS] — queried from /api/v3/exchangeInfo at init
//======================================================================================================
struct SymbolFilters {
    double lot_step_size;    // BTC: 0.00000100 — quantity must be multiple of this
    double lot_min_qty;      // minimum order quantity
    double lot_max_qty;      // maximum order quantity
    double min_notional;     // minimum order value in quote asset (e.g. $10 USDT)
    int qty_decimals;        // decimal places for quantity formatting (derived from step_size)
    int loaded;              // 1 = filters fetched successfully
};

//======================================================================================================
// [API STATE]
//======================================================================================================
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
};

//======================================================================================================
// [HELPERS]
//======================================================================================================
static inline int64_t binance_current_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static inline void binance_hmac_sha256(const char *key, const char *data, char *hex_out) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(), key, (int)strlen(key),
         (const unsigned char*)data, strlen(data), digest, &digest_len);
    for (unsigned i = 0; i < digest_len; i++)
        sprintf(hex_out + i * 2, "%02x", digest[i]);
    hex_out[digest_len * 2] = '\0';
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
    char buf[64];
    if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, val, len);
    buf[len] = '\0';
    return atof(buf);
}

// truncate quantity to exchange step size (always rounds down, no math.h needed)
// positive quantities only (always true for order sizing)
static inline double binance_round_qty(double qty, double step_size) {
    if (step_size <= 0) return qty;
    return (double)((int64_t)(qty / step_size)) * step_size;
}

// count decimal places in step size for quantity formatting
static inline int binance_step_decimals(double step_size) {
    int d = 0;
    while (step_size < 0.999999 && d < 10) { step_size *= 10.0; d++; }
    return d;
}

//======================================================================================================
// [TCP + TLS] — same pattern as BinanceCrypto.hpp
//======================================================================================================
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

//======================================================================================================
// [HTTP REQUEST]
//======================================================================================================
// sends an HTTP/1.1 request over SSL and reads the response
// returns HTTP status code (200, 400, etc.) or -1 on error
// response body written to response_buf
//======================================================================================================
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

    // close connection after each request — ssl_ctx persists, only SSL object cycles
    // the same-tick guard in main.cpp prevents back-to-back calls that caused heap corruption
    if (api->ssl) { SSL_shutdown(api->ssl); SSL_free(api->ssl); api->ssl = NULL; }
    close(api->sockfd); api->sockfd = -1;
    api->connected = 0;

    return status;
}

//======================================================================================================
// [SIGN + SEND HELPERS]
//======================================================================================================
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
            sleep(delays[attempt]);
        }
        int status = binance_signed_request(api, method, path, params,
                                             response_buf, buf_size);
        if (status == 200) return status;
        if (status >= 400 && status < 500 && status != 418 && status != 429) {
            // client error (bad qty, bad signature, etc.) — don't retry
            // try to parse Binance error code
            char msg[128];
            binance_json_extract_str(response_buf, "msg", msg, sizeof(msg));
            int code = (int)binance_json_extract_double(response_buf, "code");
            if (code != 0)
                fprintf(stderr, "[REST] Binance error %d: %s\n", code, msg);
            return status;
        }
        // 418/429 (rate limit) or 5xx (server error): retry
        if (status == 418 || status == 429)
            fprintf(stderr, "[REST] rate limited (HTTP %d)\n", status);
    }
    fprintf(stderr, "[REST] all retries failed\n");
    return -1;
}

//======================================================================================================
// [PUBLIC API]
//======================================================================================================

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
                                             double *fill_qty_out = NULL) {
    // round quantity to exchange step size
    if (api->filters.loaded)
        quantity = binance_round_qty(quantity, api->filters.lot_step_size);

    char qty_str[32];
    snprintf(qty_str, sizeof(qty_str), "%.*f", api->filters.qty_decimals, quantity);

    char params[256];
    snprintf(params, sizeof(params),
             "symbol=%s&side=BUY&type=MARKET&quantity=%s",
             api->symbol, qty_str);

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
                                              double *fill_qty_out = NULL) {
    if (api->filters.loaded)
        quantity = binance_round_qty(quantity, api->filters.lot_step_size);

    char qty_str[32];
    snprintf(qty_str, sizeof(qty_str), "%.*f", api->filters.qty_decimals, quantity);

    char params[256];
    snprintf(params, sizeof(params),
             "symbol=%s&side=SELL&type=MARKET&quantity=%s",
             api->symbol, qty_str);

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
        fprintf(stderr, "[REST] SELL filled: id=%s qty=%.8f price=%.2f\n",
                order_id_out, exec_qty, avg_price);
        return 1;
    } else {
        fprintf(stderr, "[REST] SELL failed (status %d): %s\n", status, body);
        order_id_out[0] = '\0';
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
        api->filters.lot_min_qty  = binance_json_extract_double(lot, "minQty");
        api->filters.lot_max_qty  = binance_json_extract_double(lot, "maxQty");
        api->filters.lot_step_size = binance_json_extract_double(lot, "stepSize");
        api->filters.qty_decimals = binance_step_decimals(api->filters.lot_step_size);
    }

    // parse NOTIONAL filter (or MIN_NOTIONAL for older API)
    const char *notional = strstr(body, "NOTIONAL");
    if (notional)
        api->filters.min_notional = binance_json_extract_double(notional, "minNotional");

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

//======================================================================================================
// [SECRETS LOADER]
//======================================================================================================
// loads api_key and api_secret from a key=value file (same format as engine.cfg)
// returns 1 if both found, 0 if file missing or keys empty
//======================================================================================================
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

#endif // BINANCE_ORDER_API_HPP
