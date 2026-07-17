#pragma once
//======================================================================================================
// [FILE]_[ML_Headers/BuildFlags.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DETERMINISM] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[compile-time build-flags fingerprint — stamp-emitted hash catches cross-build deploy drift (-O2 vs -O3 / USE_NATIVE_128); WARN-only at load]
// [CONTAINS]
//   - [FUNCTION]_[BUILD_FLAGS_HASH]
//======================================================================================================
// Build flags fingerprint (v5.9.5h Phase 10 / v5.10 Idea #10)
//======================================================================================================
// Detects cross-build deploy mistakes: train on dev box compiled with -O2,
// deploy to prod compiled with -O3 + -DUSE_NATIVE_128 → silent feature
// distribution drift (IEEE-754 reordering across optimization levels +
// FMA availability differences).
//
// Mechanism:
//   - Constexpr hash over canonical flag string (compile-time)
//   - Stamp body emit: `build_flags_hash=<hex>` (Surface G `has_*` flag)
//   - Engine load-time WARN if stamp's hash differs from current build's
//     (suppressible via acknowledge_cross_binary_version_drift=1)
//   - WARN-only (no refuse): operator may genuinely deploy across builds;
//     drift is observable, not catastrophic
//
// Canonical flag list — order locked. Adding a flag bumps the hash;
// every existing model needs retraining. Adding flags conservatively.
//======================================================================================================

#include <cstdint>

namespace tt {

//======================================================================
// [FUNCTION]_[BUILD_FLAGS_HASH]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [DETERMINISM]]
// [REFERENCE]_[INVARIANT]_[H21]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[constexpr FNV-1a over the ORDER-LOCKED canonical flag string — appending preserves existing semantics, reordering flips everyone (append-only discipline)]
//======================================================================
// [CODE]
//======================================================================
// FNV-1a constexpr — matches the helper in FeatureRegistry.hpp pattern.
// Local copy so this header is self-contained (no include dependency on
// FeatureRegistry.hpp from NodeModelZoo / EngineSharded which load early).
constexpr uint64_t BUILD_FLAGS_FNV_OFFSET = 0xcbf29ce484222325ULL;
constexpr uint64_t BUILD_FLAGS_FNV_PRIME  = 0x100000001b3ULL;

constexpr uint64_t bf_fnv1a(const char* s, uint64_t h = BUILD_FLAGS_FNV_OFFSET) {
    return (*s == '\0') ? h : bf_fnv1a(s + 1, (h ^ (uint64_t)(unsigned char)(*s)) * BUILD_FLAGS_FNV_PRIME);
}

// Canonical flag string — composed at compile time from preprocessor
// definitions. Each segment "FLAG=value;" or "FLAG;" (when boolean).
// Order is LOCKED: appending preserves hash for existing flags + their
// values; reordering existing flags flips hash for everyone.
//
// Current flags tracked:
//   USE_NATIVE_128 — `-DUSE_NATIVE_128=ON` for 128-bit FPN_Binary math
//   USE_XGBOOST    — XGBoost availability
//   __OPTIMIZE__   — compiler optimization (set when -O > 0)
//
// NOT tracked (observed unreliable across compilers / glibc / etc.):
//   __FAST_MATH__, __VERSION__, glibc version
//
// Operator can append more flags by editing this list; hash flips →
// existing stamps refuse via the standard cross-binary-drift WARN.
constexpr const char* BUILD_FLAGS_CANONICAL =
#ifdef USE_NATIVE_128
    "USE_NATIVE_128=1;"
#else
    "USE_NATIVE_128=0;"
#endif
#ifdef USE_XGBOOST
    "USE_XGBOOST=1;"
#else
    "USE_XGBOOST=0;"
#endif
#ifdef __OPTIMIZE__
    "OPT=1;"
#else
    "OPT=0;"
#endif
    "";  // terminating empty literal allows trailing-comma-style maintenance

inline constexpr uint64_t BUILD_FLAGS_HASH() {
    return bf_fnv1a(BUILD_FLAGS_CANONICAL);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[BUILD_FLAGS_HASH]
//======================================================================

}  // namespace tt
