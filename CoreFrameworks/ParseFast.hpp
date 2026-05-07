// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [PARSE FAST — std::from_chars wrapper (v5.11.4.A)]
//======================================================================================================
// Locale-independent double parser. Replaces atof / strtod on parsing hot
// paths (Binance WS trade decoder, executionReport parser, REST response
// parsing).
//
// Why std::from_chars over atof:
//   - Locale-immune: atof depends on the global C locale's LC_NUMERIC. A
//     stray setlocale("de_DE") elsewhere in the process flips '.' to ','
//     and silently corrupts every parsed price. std::from_chars ALWAYS
//     uses the C locale per the standard.
//   - Faster: ~3-5× quicker for typical float strings on libstdc++ ≥11.
//     The implementation in libstdc++ is the same Lemire-style algorithm
//     as fast_float (which informed the C++17 standard).
//   - Branchless on common paths: well-formed inputs decode without
//     fallback branches; only malformed inputs take the error path.
//   - No exceptions: returns std::errc() on success.
//
// Why std::from_chars over the fast_float vendor:
//   - libstdc++ ships it; no vendoring, no compile-time include cost.
//   - Identical performance for our small-string trade-message workload.
//
// Audit: plans/2026-05-06-MASTER-v5.11-optimization-sprint.md v5.11.4 item 2
//        + LATENCY_OPTIMIZATION_AUDIT.md Part 4.1 / Part 8 / Part 10.2.
//======================================================================================================

#pragma once

#include <charconv>
#include <cstring>
#include <system_error>

namespace tt {

// Parse a NUL-terminated decimal string to double. Returns 0.0 on parse
// failure (matches atof's silent-failure semantic so existing callers
// don't need new error handling — but unlike atof, doesn't depend on
// LC_NUMERIC). Caller is responsible for NUL-termination.
static inline double parse_double_fast(const char *s) {
    if (s == nullptr || *s == '\0') return 0.0;
    size_t n = std::strlen(s);
    double d;
    auto r = std::from_chars(s, s + n, d);
    return (r.ec == std::errc()) ? d : 0.0;
}

// Length-aware variant — caller already knows the byte count, skipping
// the strlen scan. Use when parsing a slice of a JSON message buffer
// (e.g. extract returned a span with explicit length).
static inline double parse_double_fast_n(const char *s, size_t n) {
    if (s == nullptr || n == 0) return 0.0;
    double d;
    auto r = std::from_chars(s, s + n, d);
    return (r.ec == std::errc()) ? d : 0.0;
}

// Parse a non-negative integer from a NUL-terminated decimal string.
// Returns 0 on parse failure. Used for trade IDs, timestamps, etc.
static inline uint64_t parse_uint64_fast(const char *s) {
    if (s == nullptr || *s == '\0') return 0;
    size_t n = std::strlen(s);
    uint64_t v;
    auto r = std::from_chars(s, s + n, v);
    return (r.ec == std::errc()) ? v : 0;
}

} // namespace tt
