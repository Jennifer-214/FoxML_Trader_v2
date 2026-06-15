// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BARRIER VALIDATION — v5.15.5.E.0.10 A6 ingress (D-221)]
//======================================================================================================
// SSoT predicate for detecting a CORRUPT model barrier (label_tp_pct / label_sl_pct).
// Applied at BOTH seams of the SAME correctness invariant (adjacency by structure+function,
// not file-location):
//   - CONSUMER (ingress): ezoo_set_per_arm_barrier (CoreModelZoo.hpp) refuses a corrupt
//     arm at load → full-disable + majority-SHALT.
//   - PRODUCER (trainer emit): Stamp_AssembleAndEmit (StampHelper.hpp) refuses to write a
//     corrupt barrier into a stamp.
//
// CONVENTION: label_*_pct is a FRACTION (0.03 = 3%), applied as price·pct at ExecutionCore
// (live_tp = price + price·tp_pct). Equity-agnostic — a PERCENTAGE, not a price → ONE bound
// across all symbols. 0 is LEGITIMATE (the absolute sg_* target fallback).
//
// SANE bounds (D-221, separate per side): SL <= 100% (can't lose >100% on a spot long);
// TP generous (a >100% long take-profit is rare-but-legit). The exact TP cap + the
// bounded-barrier bit-pack opportunity are TECH_DEBT-199 (refine post-paper-test).
//======================================================================================================
#ifndef BARRIER_VALIDATION_HPP
#define BARRIER_VALIDATION_HPP

#include <cmath>  // std::isfinite

namespace tt {

static constexpr double BARRIER_SANE_MAX_SL = 1.0;    // 100% — economically hard for a spot SL
static constexpr double BARRIER_SANE_MAX_TP = 10.0;   // 1000% — generous; absurd garbage (1e6) still caught

// Returns true if a stamp-bound barrier is CORRUPT: negative, non-finite (NaN / +Inf — the
// +sat-laundering class a sign-only clamp misses), or absurdly out-of-range. Inputs are the
// raw stamp DOUBLE (pre-Money — validation-only; H4: double is correct on the raw artifact).
inline bool barrier_is_corrupt(double tp_pct, double sl_pct) {
    return !std::isfinite(tp_pct)   || !std::isfinite(sl_pct)
        || tp_pct < 0.0             || sl_pct < 0.0
        || tp_pct > BARRIER_SANE_MAX_TP || sl_pct > BARRIER_SANE_MAX_SL;
}

}  // namespace tt

#endif  // BARRIER_VALIDATION_HPP
