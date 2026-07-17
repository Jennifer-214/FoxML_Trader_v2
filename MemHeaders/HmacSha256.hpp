// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/HmacSha256.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DETERMINISM] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[in-process HMAC-SHA256 + SHA-256 primitives over openssl EVP (no shell-out) — the digest kernel under stamp verify/write, sidecar integrity, and Binance signing]
// [CONTAINS]
//   - [FUNCTION]_[hmac_sha256_hex]
//   - [FUNCTION]_[sha256_file_hex_inproc]
//   - [FUNCTION]_[sha256_bytes]
// [REFERENCE]_[INVARIANT]_[H9]
//======================================================================================================
// Shared primitive for any code that needs HMAC-SHA256 or SHA-256 of a
// file. Uses openssl EVP — same library Binance signing already pulls in,
// so no new build dependency.
//
// Replaces two earlier shell-out paths (v5.2.0 Phase 1 hacks):
//   - ML_Headers/ModelInference.hpp `verify_model_stamp` invoked
//     `openssl dgst -sha256 -hmac '<secret>'` via popen.
//   - ML_Headers/ModelInference.hpp `sha256_file_hex` invoked
//     `sha256sum` via popen.
//
// In-process is faster, avoids shell-injection surface, and removes the
// dependency on /usr/bin/sha256sum + /usr/bin/openssl being on PATH.
//
// HMAC primitive matches `openssl dgst -sha256 -hmac` output (the retired
// bash stamper `tools/stamp_model.sh` used it — DELETED at .B.3 Path C;
// stamping is in-process via stamp_write_for_model / suite auto-stamp now).
// Bash-compat regression test in controller_test.cpp guards the equivalence.
//======================================================================================================
#ifndef HMAC_SHA256_HPP
#define HMAC_SHA256_HPP

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

namespace tt {

//======================================================================
// [FUNCTION]_[hmac_sha256_hex]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[HMAC-SHA256(secret, data) -> 64-char lowercase hex + NUL into a >=65B buffer — one-shot openssl HMAC over strlen-sized text bodies; 1 on success]
//======================================================================
// [CODE]
//======================================================================
static inline int hmac_sha256_hex(const char* secret, const char* data, char* hex_out) {
    if (!secret || !data || !hex_out) return 0;

    unsigned char raw[32];
    unsigned int  raw_len = 0;
    if (!HMAC(EVP_sha256(),
              secret, (int)strlen(secret),
              (const unsigned char*)data, strlen(data),
              raw, &raw_len)) {
        return 0;
    }
    if (raw_len != 32) return 0;

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        hex_out[2*i  ] = hex[raw[i] >> 4];
        hex_out[2*i+1] = hex[raw[i] & 0x0F];
    }
    hex_out[64] = '\0';
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// HMAC-SHA256(secret, data) → 64-byte lowercase hex digest.
// hex_out must point to a buffer of at least 65 bytes (64 hex digits + NUL).
// Returns 1 on success, 0 on openssl failure.
//
// `data` may contain arbitrary bytes; length is taken from strlen() since
// the canonical bodies this signs are NUL-terminated text. For binary
// data, use the lower-level HMAC() call directly.
//======================================================================
// [END_FUNCTION]_[hmac_sha256_hex]
//======================================================================

//======================================================================
// [FUNCTION]_[sha256_file_hex_inproc]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[SHA-256 of a file via EVP streaming in 64KB chunks (bounded memory for any file size) — 64-hex + NUL into a hex_cap>=65 buffer; 1 on success]
//======================================================================
// [CODE]
//======================================================================
static inline int sha256_file_hex_inproc(const char* path, char* hex_out, size_t hex_cap) {
    if (!path || !hex_out || hex_cap < 65) return 0;

    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        fclose(f);
        return 0;
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        fclose(f);
        return 0;
    }

    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (EVP_DigestUpdate(ctx, buf, n) != 1) {
            EVP_MD_CTX_free(ctx);
            fclose(f);
            return 0;
        }
    }
    fclose(f);

    unsigned char raw[32];
    unsigned int  raw_len = 0;
    if (EVP_DigestFinal_ex(ctx, raw, &raw_len) != 1 || raw_len != 32) {
        EVP_MD_CTX_free(ctx);
        return 0;
    }
    EVP_MD_CTX_free(ctx);

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        hex_out[2*i  ] = hex[raw[i] >> 4];
        hex_out[2*i+1] = hex[raw[i] & 0x0F];
    }
    hex_out[64] = '\0';
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// hex_out must point to a buffer of at least `hex_cap` bytes; this writes
// 64 lowercase hex digits + NUL. Returns 1 on success, 0 if file open
// failed, EVP setup failed, or hex_cap < 65.
//======================================================================
// [END_FUNCTION]_[sha256_file_hex_inproc]
//======================================================================

//======================================================================
// [FUNCTION]_[sha256_bytes]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[SHA-256 of an in-memory buffer -> raw 32B out — the FeatureStandardizer sidecar body-integrity primitive; 1 on success]
//======================================================================
// [CODE]
//======================================================================
static inline int sha256_bytes(const void* data, size_t n, unsigned char out[32]) {
    if (!data || !out) return 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        return 0;
    }
    if (n > 0 && EVP_DigestUpdate(ctx, data, n) != 1) {
        EVP_MD_CTX_free(ctx);
        return 0;
    }
    unsigned int raw_len = 0;
    if (EVP_DigestFinal_ex(ctx, out, &raw_len) != 1 || raw_len != 32) {
        EVP_MD_CTX_free(ctx);
        return 0;
    }
    EVP_MD_CTX_free(ctx);
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.9.3a — SHA-256 of an in-memory byte buffer.
// out must point to 32 bytes. Returns 1 on success, 0 on EVP failure.
// Used by FeatureStandardizer for sidecar body integrity check.
//======================================================================
// [END_FUNCTION]_[sha256_bytes]
//======================================================================

} // namespace tt

#endif // HMAC_SHA256_HPP
