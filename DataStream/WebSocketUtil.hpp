// SPDX-License-Identifier: AGPL-3.0-or-later

//======================================================================================================
// [WEBSOCKET UTILITIES]
//======================================================================================================
// shared TCP, SSL, and WebSocket functions used by both BinanceCrypto and BinanceDepth
// extracted to avoid duplicating connection/framing code across stream types
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

//======================================================================================================
// [BASE64]
//======================================================================================================
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
    out[j] = '\0';
}

//======================================================================================================
// [TCP CONNECT]
//======================================================================================================
static inline int ws_tcp_connect(const char *host, int port) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo hints = {}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    return fd;
}

//======================================================================================================
// [SSL SETUP]
//======================================================================================================
static inline int ws_ssl_setup(SSL_CTX **ctx_out, SSL **ssl_out, int sockfd, const char *host) {
    *ctx_out = SSL_CTX_new(TLS_client_method());
    if (!*ctx_out) return -1;
    *ssl_out = SSL_new(*ctx_out);
    if (!*ssl_out) { SSL_CTX_free(*ctx_out); return -1; }
    SSL_set_fd(*ssl_out, sockfd);
    SSL_set_tlsext_host_name(*ssl_out, host);  // SNI required by Binance
    if (SSL_connect(*ssl_out) <= 0) {
        SSL_free(*ssl_out); SSL_CTX_free(*ctx_out); return -1;
    }
    return 0;
}

//======================================================================================================
// [WEBSOCKET HANDSHAKE]
//======================================================================================================
static inline int ws_handshake(SSL *ssl, const char *host, const char *path) {
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
    if (SSL_write(ssl, req, n) <= 0) return -1;

    char resp[1024];
    int total = 0;
    while (total < (int)sizeof(resp) - 1) {
        int r = SSL_read(ssl, resp + total, sizeof(resp) - 1 - total);
        if (r <= 0) return -1;
        total += r;
        resp[total] = '\0';
        if (strstr(resp, "\r\n\r\n")) break;
    }
    return strstr(resp, "101") ? 0 : -1;
}

//======================================================================================================
// [WEBSOCKET FRAME READER]
//======================================================================================================
static inline int ws_read_frame(SSL *ssl, char *out, int max_len, int *opcode) {
    unsigned char hdr[2];
    int r = SSL_read(ssl, hdr, 2);
    if (r <= 0) return -1;
    *opcode = hdr[0] & 0x0F;
    int masked = (hdr[1] >> 7) & 1;
    int plen = hdr[1] & 0x7F;

    if (plen == 126) {
        unsigned char ext[2];
        if (SSL_read(ssl, ext, 2) != 2) return -1;
        plen = (ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        unsigned char ext[8];
        if (SSL_read(ssl, ext, 8) != 8) return -1;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
    }

    unsigned char mask[4] = {};
    if (masked && SSL_read(ssl, mask, 4) != 4) return -1;
    if (plen > max_len) return -1;

    int total = 0;
    while (total < plen) {
        r = SSL_read(ssl, out + total, plen - total);
        if (r <= 0) return -1;
        total += r;
    }
    if (masked) for (int i = 0; i < plen; i++) out[i] ^= mask[i & 3];
    out[plen] = '\0';
    return plen;
}

//======================================================================================================
// [WEBSOCKET CLOSE]
//======================================================================================================
static inline void ws_close(SSL *ssl, SSL_CTX *ctx, int sockfd) {
    unsigned char close_frame[6] = {0x88, 0x80, 0, 0, 0, 0};
    SSL_write(ssl, close_frame, 6);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(sockfd);
}

//======================================================================================================
// [PONG RESPONSE]
//======================================================================================================
static inline void ws_send_pong(SSL *ssl) {
    unsigned char pong[2] = {0x8A, 0x00};
    SSL_write(ssl, pong, 2);
}

#endif // WEBSOCKET_UTIL_HPP
