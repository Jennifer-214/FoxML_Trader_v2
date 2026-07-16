// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/ParseFast.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [PARSER] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the locale-immune std::from_chars parse family — the H5 replacement for atof/strtod on parsing paths]
// [CONTAINS]
//   - [FUNCTION]_[parse_double_fast]
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
#include <cstdint>      // uint64_t (parse_uint64_fast at line ~64); explicit per IWYU discipline
#include <system_error>

namespace tt {

//======================================================================
// [FUNCTION]_[parse_double_fast]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PARSER] [DETERMINISM] [CRITICAL]]
// [REFERENCE]_[INVARIANT]_[H5]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[THE H5 canonical — locale-immune NUL-terminated double parse; atof's silent-0.0 semantic without its LC_NUMERIC hazard]
//======================================================================
// [CODE]
//======================================================================
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[parse_double_fast]
//======================================================================

// Checked variant of parse_double_fast: same value AND the same locale-immune std::from_chars backend,
// plus it reports whether a NON-EMPTY value failed to parse. Empty/NULL stays clean (0.0, malformed=false)
// — the cfg "empty = inherit/default" convention; only a non-empty unparseable string (e.g. "banana") is
// malformed. Lets a caller distinguish a real 0 from a malformed→0 silent default WITHOUT falling back to
// atof (whose LC_NUMERIC dependence is the determinism reason this family exists). The returned value is
// byte-identical to parse_double_fast for every input (③ D-256(b) per-node RAW-override flag-capture).
static inline double parse_double_fast_checked(const char *s, bool *malformed_out) {
    if (s == nullptr || *s == '\0') { if (malformed_out) *malformed_out = false; return 0.0; }
    size_t n = std::strlen(s);
    double d;
    auto r = std::from_chars(s, s + n, d);
    bool ok = (r.ec == std::errc());
    if (malformed_out) *malformed_out = !ok;
    return ok ? d : 0.0;
}

// Checked base-10 integer parse (locale-immune std::from_chars). Returns the value; sets *malformed_out
// true iff a NON-EMPTY value failed to parse OR wasn't fully consumed. Empty/NULL = clean (0, not
// malformed) — the "empty = default" cfg convention. Lets a caller refuse a malformed int instead of the
// atoi/atol silent→0 coercion (③ config-compiler reuse for non-registry int cfg parsers, e.g. BinanceConfig
// venue selectors where →0 is the DANGEROUS value: use_testnet=0 = PRODUCTION).
static inline long parse_int_checked(const char *s, bool *malformed_out) {
    if (s == nullptr || *s == '\0') { if (malformed_out) *malformed_out = false; return 0; }
    size_t n = std::strlen(s);
    long v;
    auto r = std::from_chars(s, s + n, v);
    bool ok = (r.ec == std::errc()) && (r.ptr == s + n);
    if (malformed_out) *malformed_out = !ok;
    return ok ? v : 0;
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

// Parse a double + return how many bytes were consumed via *end_out.
// Locale-immune drop-in for strtod (which depends on LC_NUMERIC). On
// parse failure leaves *end_out == p (no progress) and returns 0.0 —
// matches strtod's "no number consumed" sentinel via the same pointer
// equality test used by callers.
static inline double parse_double_fast_advance(const char *p, const char **end_out) {
    if (p == nullptr) { if (end_out) *end_out = nullptr; return 0.0; }
    size_t n = std::strlen(p);
    double v;
    auto r = std::from_chars(p, p + n, v);
    if (end_out) *end_out = (r.ec == std::errc()) ? r.ptr : p;
    return (r.ec == std::errc()) ? v : 0.0;
}

} // namespace tt
