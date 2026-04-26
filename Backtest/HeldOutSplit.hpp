// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [HELD-OUT SPLIT — temporal train+val / test partitioning with lock-token discipline]
//======================================================================================================
// purpose: separate a final-test held-out portion BEFORE any tuning so the
// generalization-gap measurement is unbiased. ML training "discipline" — code
// refuses to use the test indices unless explicitly unlocked, with audit-log
// at unlock time. friction, not security: a determined peeker can edit memory.
// the goal is "make accidental peeking impossible, intentional peeking
// auditable."
//
// Phase 7prep ships the primitive (struct + Make/Unlock/TestAccessAllowed).
// Backtest_RunFullValidation (Phase 7prep c2) is the consumer that runs WF on
// the train+val portion, then unlocked-test eval, then reports the gap.
//
// lock_token: 32 hex chars from a 128-bit FNV-1a hash of (total_samples +
// trainval_end + a small entropy seed). The plan originally said "SHA256-hex"
// but the field is 33 bytes — that's only space for 32 hex chars (truncated
// SHA256 = ~128 bits). To keep it dependency-free + simple, we use double-
// FNV-1a-64 instead. Same friction-grade properties; not cryptographic.
//======================================================================================================
#ifndef HELD_OUT_SPLIT_HPP
#define HELD_OUT_SPLIT_HPP

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

// 5%–30% range for the held-out fraction. Below 5% = too small to estimate
// generalization meaningfully; above 30% = too little data left for training
// + walk-forward CV. Out-of-range inputs are clamped (not rejected) so the
// suite can recover gracefully if the user types an extreme value.
#define HELDOUT_FRACTION_MIN 0.05
#define HELDOUT_FRACTION_MAX 0.30

struct HeldOutSplit {
    int total_samples;
    int trainval_end_idx;    // [0, trainval_end_idx) = train + walk-forward
    int test_start_idx;      // [test_start_idx, total_samples) = held-out test
    int locked;              // 1 = test set inaccessible, 0 = unlocked
    char lock_token[33];     // 32 hex chars + null terminator
};

//======================================================================================================
// Two FNV-1a-64 hashes with distinct seeds, concatenated as 16+16 hex chars.
// Friction-grade: hard to guess (~128 bits entropy), trivial to forge with
// source access. That's by design.
static inline uint64_t HeldOutSplit_FNV1a64(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = seed ? seed : 0xcbf29ce484222325ULL;
    while (len--) {
        h ^= *p++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static inline void HeldOutSplit_GenToken(char out[33],
                                          int total, int trainval_end,
                                          uint64_t entropy) {
    uint8_t buf[24];
    memcpy(buf,      &total,        4);
    memcpy(buf + 4,  &trainval_end, 4);
    memcpy(buf + 8,  &entropy,      8);
    // pad with timestamp for unique-per-split even with same shape inputs
    int64_t ts = (int64_t)time(NULL);
    memcpy(buf + 16, &ts,           8);

    uint64_t h1 = HeldOutSplit_FNV1a64(buf, sizeof(buf), 0);
    uint64_t h2 = HeldOutSplit_FNV1a64(buf, sizeof(buf), 0x9e3779b97f4a7c15ULL);
    snprintf(out, 33, "%016llx%016llx",
             (unsigned long long)h1, (unsigned long long)h2);
}

//======================================================================================================
static inline HeldOutSplit HeldOutSplit_Make(int total_samples, double fraction) {
    HeldOutSplit s = {};
    s.total_samples = total_samples;

    if (total_samples <= 0) {
        // degenerate input — return locked, all-zero split (caller should check)
        s.locked = 1;
        s.lock_token[0] = '\0';
        return s;
    }

    // clamp fraction to sane range
    if (fraction < HELDOUT_FRACTION_MIN) fraction = HELDOUT_FRACTION_MIN;
    if (fraction > HELDOUT_FRACTION_MAX) fraction = HELDOUT_FRACTION_MAX;

    int test_count = (int)(total_samples * fraction + 0.5);
    if (test_count < 1) test_count = 1;
    if (test_count >= total_samples) test_count = total_samples - 1;

    s.trainval_end_idx = total_samples - test_count;
    s.test_start_idx   = s.trainval_end_idx;
    s.locked = 1;

    // entropy seed: a pointer address (process-relative randomness) — good
    // enough to make tokens differ across runs even with identical shape.
    uint64_t entropy = (uint64_t)(uintptr_t)&s;
    HeldOutSplit_GenToken(s.lock_token, s.total_samples, s.trainval_end_idx, entropy);
    return s;
}

//======================================================================================================
static inline int HeldOutSplit_TestAccessAllowed(const HeldOutSplit *s) {
    return s && s->locked == 0;
}

//======================================================================================================
// Returns 1 if unlock succeeded, 0 if token doesn't match (still locked).
// Logs unlock event to stderr — caller can also Notify_Send for audit trail.
static inline int HeldOutSplit_Unlock(HeldOutSplit *s, const char *token) {
    if (!s || !token) return 0;
    if (strncmp(s->lock_token, token, sizeof(s->lock_token)) != 0) {
        fprintf(stderr, "[HELDOUT] unlock REJECTED — token mismatch\n");
        return 0;
    }
    s->locked = 0;
    fprintf(stderr, "[HELDOUT] unlocked — held-out test eval permitted (samples [%d, %d))\n",
            s->test_start_idx, s->total_samples);
    return 1;
}

//======================================================================================================
// Convenience: re-lock for a fresh evaluation cycle (e.g., new model).
// Generates a NEW token, so any code holding the old one can't unlock again.
static inline void HeldOutSplit_Relock(HeldOutSplit *s) {
    if (!s) return;
    s->locked = 1;
    uint64_t entropy = (uint64_t)(uintptr_t)s;
    HeldOutSplit_GenToken(s->lock_token, s->total_samples, s->trainval_end_idx, entropy);
}

#endif // HELD_OUT_SPLIT_HPP
