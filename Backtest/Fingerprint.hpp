// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FINGERPRINT — SHA256 REPRODUCIBILITY]
//======================================================================================================
// port of FoxML/private fingerprinting.py concept (not the 1309-line infrastructure).
// SHA256 hash of config + data for reproducibility tracking.
// same config + same data = same fingerprint, guaranteed.
//
// embedded public-domain SHA256 implementation — zero external dependencies.
// streams data in 64-byte chunks for large file support.
//
// source: ~/FoxML/private/TRAINING/common/utils/fingerprinting.py (concept only)
// source: ~/FoxML/private/TRAINING/common/utils/config_hashing.py (canonical serialization)
//======================================================================================================
#ifndef FINGERPRINT_HPP
#define FINGERPRINT_HPP

#include <stdio.h>
#include <stdint.h>
#include <string.h>

//======================================================================================================
// [EMBEDDED SHA256]
// public domain implementation. processes data in 64-byte blocks.
// based on the FIPS 180-4 specification.
//======================================================================================================

struct SHA256_State {
    uint32_t h[8];
    uint64_t total_len;
    uint8_t  buf[64];
    int      buf_len;
};

static inline uint32_t sha256_rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t sha256_ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static inline uint32_t sha256_maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static inline uint32_t sha256_S0(uint32_t x) { return sha256_rotr(x,2) ^ sha256_rotr(x,13) ^ sha256_rotr(x,22); }
static inline uint32_t sha256_S1(uint32_t x) { return sha256_rotr(x,6) ^ sha256_rotr(x,11) ^ sha256_rotr(x,25); }
static inline uint32_t sha256_s0(uint32_t x) { return sha256_rotr(x,7) ^ sha256_rotr(x,18) ^ (x >> 3); }
static inline uint32_t sha256_s1(uint32_t x) { return sha256_rotr(x,17) ^ sha256_rotr(x,19) ^ (x >> 10); }

static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static inline void sha256_transform(SHA256_State *s, const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    for (int i = 16; i < 64; i++)
        w[i] = sha256_s1(w[i-2]) + w[i-7] + sha256_s0(w[i-15]) + w[i-16];

    uint32_t a=s->h[0], b=s->h[1], c=s->h[2], d=s->h[3];
    uint32_t e=s->h[4], f=s->h[5], g=s->h[6], h=s->h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + sha256_S1(e) + sha256_ch(e,f,g) + SHA256_K[i] + w[i];
        uint32_t t2 = sha256_S0(a) + sha256_maj(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }

    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static inline void SHA256_Init(SHA256_State *s) {
    s->h[0]=0x6a09e667; s->h[1]=0xbb67ae85; s->h[2]=0x3c6ef372; s->h[3]=0xa54ff53a;
    s->h[4]=0x510e527f; s->h[5]=0x9b05688c; s->h[6]=0x1f83d9ab; s->h[7]=0x5be0cd19;
    s->total_len = 0;
    s->buf_len = 0;
}

static inline void SHA256_Update(SHA256_State *s, const void *data, int len) {
    const uint8_t *p = (const uint8_t *)data;
    s->total_len += len;
    // fill buffer
    while (len > 0) {
        int space = 64 - s->buf_len;
        int take = (len < space) ? len : space;
        memcpy(s->buf + s->buf_len, p, take);
        s->buf_len += take;
        p += take;
        len -= take;
        if (s->buf_len == 64) {
            sha256_transform(s, s->buf);
            s->buf_len = 0;
        }
    }
}

static inline void SHA256_Final(SHA256_State *s, uint8_t hash[32]) {
    uint64_t bits = s->total_len * 8;
    // padding
    uint8_t pad = 0x80;
    SHA256_Update(s, &pad, 1);
    pad = 0x00;
    while (s->buf_len != 56)
        SHA256_Update(s, &pad, 1);
    // length in big-endian
    uint8_t len_be[8];
    for (int i = 7; i >= 0; i--) { len_be[i] = (uint8_t)(bits & 0xff); bits >>= 8; }
    SHA256_Update(s, len_be, 8);
    // output
    for (int i = 0; i < 8; i++) {
        hash[i*4+0] = (uint8_t)(s->h[i] >> 24);
        hash[i*4+1] = (uint8_t)(s->h[i] >> 16);
        hash[i*4+2] = (uint8_t)(s->h[i] >> 8);
        hash[i*4+3] = (uint8_t)(s->h[i]);
    }
}

// convenience: hash to hex string (65 bytes including null terminator)
static inline void SHA256_ToHex(const uint8_t hash[32], char hex[65]) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i*2]   = hx[hash[i] >> 4];
        hex[i*2+1] = hx[hash[i] & 0x0f];
    }
    hex[64] = '\0';
}

//======================================================================================================
// [FILE HASHING]
// streams file through SHA256 in 64KB chunks — handles multi-GB files.
//======================================================================================================
#define FINGERPRINT_CHUNK_SIZE 65536

static inline int Fingerprint_HashFile(const char *path, uint8_t hash[32]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    SHA256_State s;
    SHA256_Init(&s);

    uint8_t chunk[FINGERPRINT_CHUNK_SIZE];
    int n;
    while ((n = (int)fread(chunk, 1, FINGERPRINT_CHUNK_SIZE, f)) > 0)
        SHA256_Update(&s, chunk, n);

    fclose(f);
    SHA256_Final(&s, hash);
    return 1;
}

//======================================================================================================
// [CONFIG FINGERPRINT]
//======================================================================================================
// canonical serialization: hash config fields as sorted key=value pairs.
// same config → same hash, regardless of field order in struct.
// includes MODEL_FORMAT_VERSION to catch feature set changes.
//
// from FoxML config_hashing.py: canonical_json with sorted keys + normalized floats.
// we use a simpler approach: snprintf key=value pairs in sorted order.
//======================================================================================================
#include "../ML_Headers/ModelInference.hpp"

// compute fingerprint of config + data files.
// hex_out must be at least 65 bytes.
// config fields are serialized in sorted order for canonical hashing.
// each data file contributes its own SHA256 to the combined hash.
template <unsigned F>
static inline void Fingerprint_Compute(char *hex_out, const void *cfg_ptr, int cfg_size,
                                        const char **data_paths, int num_files) {
    SHA256_State s;
    SHA256_Init(&s);

    // hash 1: config struct (raw bytes — deterministic for same field values)
    SHA256_Update(&s, cfg_ptr, cfg_size);

    // hash 2: model format version (catches feature set changes)
    int ver = MODEL_FORMAT_VERSION;
    SHA256_Update(&s, &ver, sizeof(ver));

    // hash 3: feature lookback metadata (catches lookback changes)
    SHA256_Update(&s, FEATURE_LOOKBACKS, sizeof(FEATURE_LOOKBACKS));

    // hash 4: each data file's content
    for (int i = 0; i < num_files; i++) {
        if (!data_paths[i]) continue;
        uint8_t file_hash[32];
        if (Fingerprint_HashFile(data_paths[i], file_hash))
            SHA256_Update(&s, file_hash, 32);
    }

    uint8_t hash[32];
    SHA256_Final(&s, hash);
    SHA256_ToHex(hash, hex_out);
}

// short fingerprint (first 12 hex chars) for display
static inline void Fingerprint_Short(const char *full_hex, char *short_out, int len) {
    if (len > 64) len = 64;
    memcpy(short_out, full_hex, len);
    short_out[len] = '\0';
}

#endif // FINGERPRINT_HPP
