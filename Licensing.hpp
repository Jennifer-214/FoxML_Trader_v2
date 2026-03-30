#pragma once
// Licensing — startup license key validation
// checks key against a remote server, caches result locally for 24h
// NOT in the hot path — runs once at startup before websocket connect
//
// flow:
//   1. read key from ~/.foxml/license.key or engine.cfg
//   2. check local cache (~/.foxml/license.cache) — skip server if < 24h old
//   3. HTTPS GET to license server → valid/invalid
//   4. cache result, return bool

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

// configurable — change for your domain
#define LICENSE_SERVER_HOST "api.foxml.dev"
#define LICENSE_CHECK_PATH "/v1/license/check"
#define LICENSE_CACHE_HOURS 24

struct LicenseInfo {
    char key[64];
    char fingerprint[33]; // MD5 hex of machine ID
    int valid;
    char plan[16];       // "pro", "basic", etc.
    time_t expires;
    time_t last_check;
};

//==========================================================================
// MACHINE FINGERPRINT — prevents key sharing across devices
// uses hostname + /etc/machine-id (stable across reboots)
//==========================================================================
static inline void License_Fingerprint(char *out) {
    // combine hostname + machine-id for a stable fingerprint
    char raw[512] = {};
    int pos = 0;

    // hostname
    char hostname[128] = {};
    gethostname(hostname, sizeof(hostname));
    pos += snprintf(raw + pos, sizeof(raw) - pos, "%s:", hostname);

    // /etc/machine-id (systemd, stable across reboots)
    FILE *f = fopen("/etc/machine-id", "r");
    if (f) {
        char mid[64] = {};
        if (fgets(mid, sizeof(mid), f)) {
            // strip newline
            char *nl = strchr(mid, '\n');
            if (nl) *nl = '\0';
            pos += snprintf(raw + pos, sizeof(raw) - pos, "%s", mid);
        }
        fclose(f);
    }

    // simple hash → hex string (not crypto-secure, just a device ID)
    unsigned int hash = 5381;
    for (int i = 0; raw[i]; i++)
        hash = ((hash << 5) + hash) + raw[i];

    snprintf(out, 33, "%08x%08x%08x%08x",
             hash, hash ^ 0xDEADBEEF, hash * 2654435761u, hash ^ 0xCAFEBABE);
}

//==========================================================================
// READ LICENSE KEY from ~/.foxml/license.key
//==========================================================================
static inline int License_ReadKey(char *key_out, int max_len) {
    // try ~/.foxml/license.key first
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char path[512];
    snprintf(path, sizeof(path), "%s/.foxml/license.key", home);

    FILE *f = fopen(path, "r");
    if (!f) {
        // try ./license.key as fallback
        f = fopen("license.key", "r");
    }
    if (!f) return 0;

    if (!fgets(key_out, max_len, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);

    // strip whitespace/newlines
    int len = strlen(key_out);
    while (len > 0 && (key_out[len-1] == '\n' || key_out[len-1] == '\r' || key_out[len-1] == ' '))
        key_out[--len] = '\0';

    return len > 0;
}

//==========================================================================
// CHECK LOCAL CACHE
//==========================================================================
static inline int License_CheckCache(LicenseInfo *info) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char path[512];
    snprintf(path, sizeof(path), "%s/.foxml/license.cache", home);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[256];
    int found_valid = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "valid=", 6) == 0) {
            info->valid = atoi(line + 6);
            found_valid = 1;
        }
        if (strncmp(line, "plan=", 5) == 0) {
            strncpy(info->plan, line + 5, 15);
            info->plan[15] = '\0';
            // strip newline
            char *nl = strchr(info->plan, '\n');
            if (nl) *nl = '\0';
        }
        if (strncmp(line, "checked=", 8) == 0)
            info->last_check = (time_t)atol(line + 8);
        if (strncmp(line, "expires=", 8) == 0)
            info->expires = (time_t)atol(line + 8);
    }
    fclose(f);

    if (!found_valid) return 0;

    // check if cache is still fresh
    time_t now = time(NULL);
    double hours = difftime(now, info->last_check) / 3600.0;
    if (hours > LICENSE_CACHE_HOURS) return 0;  // stale

    return 1;  // cache hit
}

//==========================================================================
// SAVE CACHE
//==========================================================================
static inline void License_SaveCache(const LicenseInfo *info) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    // ensure ~/.foxml/ directory exists
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.foxml", home);
    mkdir(dir, 0700);

    char path[512];
    snprintf(path, sizeof(path), "%s/.foxml/license.cache", home);

    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "valid=%d\n", info->valid);
    fprintf(f, "plan=%s\n", info->plan);
    fprintf(f, "checked=%ld\n", (long)info->last_check);
    fprintf(f, "expires=%ld\n", (long)info->expires);
    fclose(f);
}

//==========================================================================
// HTTPS LICENSE CHECK (single request, self-contained)
//==========================================================================
static inline int License_ServerCheck(const char *key, LicenseInfo *info) {
    // TCP connect
    struct addrinfo hints = {}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(LICENSE_SERVER_HOST, "443", &hints, &res) != 0) {
        fprintf(stderr, "[LICENSE] DNS lookup failed for %s\n", LICENSE_SERVER_HOST);
        return -1;  // network error — use cache or fail gracefully
    }

    int sockfd = -1;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) continue;
        // 5 second connect timeout
        struct timeval tv = {5, 0};
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);

    if (sockfd == -1) {
        fprintf(stderr, "[LICENSE] TCP connect failed\n");
        return -1;
    }

    // TLS
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sockfd);
    SSL_set_tlsext_host_name(ssl, LICENSE_SERVER_HOST);

    if (SSL_connect(ssl) != 1) {
        fprintf(stderr, "[LICENSE] TLS handshake failed\n");
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(sockfd);
        return -1;
    }

    // HTTP GET with fingerprint
    char fp[33] = {};
    License_Fingerprint(fp);

    char request[512];
    int req_len = snprintf(request, sizeof(request),
        "GET %s?key=%s&fingerprint=%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "User-Agent: foxml-trader/1.0\r\n"
        "\r\n",
        LICENSE_CHECK_PATH, key, fp, LICENSE_SERVER_HOST);

    SSL_write(ssl, request, req_len);

    // read response
    char response[4096] = {};
    int total = 0;
    while (total < (int)sizeof(response) - 1) {
        int n = SSL_read(ssl, response + total, sizeof(response) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    response[total] = '\0';

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(sockfd);

    // parse response — look for "valid":true/false in JSON body
    const char *body = strstr(response, "\r\n\r\n");
    if (!body) return -1;
    body += 4;

    info->valid = (strstr(body, "\"valid\":true") != NULL) ? 1 : 0;
    info->last_check = time(NULL);

    // parse plan
    const char *plan_pos = strstr(body, "\"plan\":\"");
    if (plan_pos) {
        plan_pos += 8;
        int i = 0;
        while (*plan_pos && *plan_pos != '"' && i < 15)
            info->plan[i++] = *plan_pos++;
        info->plan[i] = '\0';
    } else {
        strcpy(info->plan, "unknown");
    }

    // parse expires (unix timestamp)
    const char *exp_pos = strstr(body, "\"expires\":");
    if (exp_pos) {
        info->expires = (time_t)atol(exp_pos + 10);
    }

    return 0;  // request succeeded (check info->valid for result)
}

//==========================================================================
// MAIN ENTRY POINT — call once at startup
//==========================================================================
// returns: 1 = licensed, 0 = unlicensed, -1 = check failed (no network)
static inline int License_Validate(LicenseInfo *info) {
    memset(info, 0, sizeof(*info));

    // read key
    if (!License_ReadKey(info->key, sizeof(info->key))) {
        fprintf(stderr, "[LICENSE] no license key found\n");
        fprintf(stderr, "[LICENSE] place your key in ~/.foxml/license.key\n");
        return 0;
    }

    fprintf(stderr, "[LICENSE] key: %.8s...\n", info->key);  // log first 8 chars only

    // check cache first
    if (License_CheckCache(info)) {
        fprintf(stderr, "[LICENSE] cached: %s (plan: %s)\n",
                info->valid ? "valid" : "invalid", info->plan);
        return info->valid;
    }

    // server check
    fprintf(stderr, "[LICENSE] checking with server...\n");
    int result = License_ServerCheck(info->key, info);

    if (result < 0) {
        // network error — if we had a previous valid cache, be generous
        fprintf(stderr, "[LICENSE] server unreachable — using offline grace period\n");
        info->valid = 1;  // grace period when offline
        strcpy(info->plan, "offline");
    }

    // save cache
    License_SaveCache(info);

    fprintf(stderr, "[LICENSE] %s (plan: %s)\n",
            info->valid ? "valid" : "INVALID", info->plan);

    return info->valid;
}
