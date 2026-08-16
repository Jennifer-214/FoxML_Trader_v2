// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/ReciprocalLUT.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP]]
// [REFERENCE]_[INVARIANT]_[H20]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[boot-precomputed 1/n table — FPN_Mul(sum, recip[n]) replaces per-Push division (~10ns vs ~50-100ns); <=1 LSB ULP drift documented, exact for power-of-2 n]
// [CONTAINS]
//   - [FUNCTION]_[GetReciprocalLUT]
//======================================================================================================
// v5.11.2.A — Replaces FPN_DivNoAssert(sum, n_fp) with FPN_Mul(sum, recip[n])
// where recip[n] = 1/n precomputed at engine boot.
//
// Audit: LATENCY_OPTIMIZATION_AUDIT.md Part 2.3.
// Discipline: plans/_cross-cutting/2026-05-06-latency-path-discipline.md Rule 8 (mask-blend
// over data-dependent branches; this LUT is the canonical "always do work,
// no branch" version of conditional division).
//
// ULP drift trade-off:
//   FPN_Binary<F> reciprocals for n in {3, 5, 6, 7, 9, 10, 11, ...} (non-power-of-2)
//   have a finite truncation in the FPN_Binary<F>=128-bit representation. The
//   resulting FPN_Mul(sum, recip[n]) differs from FPN_DivNoAssert(sum, n)
//   by at most 1 LSB in the result's magnitude.
//
//   Real-value impact: 1 LSB in FPN_Binary<64> = 1 / 2^64 ≈ 5.4e-20. Cascades to
//   ~1e-15 in derived values — well below all downstream consumer precision
//   (XGBoost float32 = ~1e-7, cfg-tuned thresholds = ~1e-4).
//
//   Replay-determinism (the memcmp replay test in tests/controller_test.cpp) uses memcmp on FPN_Binary
//   bytes — the LUT changes the output bytewise. Baseline is regenerated as
//   part of v5.11.2.A; documented in DOCS/PARITY_LIFECYCLE.md.
//
// For power-of-2 n (n in {2, 4, 8, 16, 32, 64, 128}), 1/n is exact in
// FPN_Binary<F=64> — no drift. Steady state of RollingStats has n=W (128 by default,
// power-of-2 enforced by static_assert) → exact reciprocal.
//
// Init cost: W FPN_DivNoAssert calls at engine boot (one-time, ~ms).
// Per-Push cost: 1 FPN_Mul (~10ns) replaces 1 FPN_DivNoAssert (~50-100ns).
//======================================================================================================
#pragma once
#include "../FixedPoint/FixedPointN.hpp"

namespace tt {

//======================================================================
// [FUNCTION]_[GetReciprocalLUT]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Meyer's-singleton const LUT (thread-safe C++11 init); values[0] is the never-used sentinel]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W>
struct ReciprocalLUT {
    FPN_Binary<F> values[W + 1];  // values[n] = 1.0 / n; values[0] = sentinel zero
};

// Meyer's singleton — thread-safe init in C++11+ (the underlying static
// initialization is guaranteed atomic by the standard). Returns const&
// so the LUT data is readonly after init.
template <unsigned F, unsigned W>
inline const ReciprocalLUT<F, W>& GetReciprocalLUT() {
    static const ReciprocalLUT<F, W> lut = []() {
        ReciprocalLUT<F, W> l{};
        FPN_Binary<F> one = FPN_FromDouble<F>(1.0);
        l.values[0] = FPN_Zero<F>();  // sentinel (n=0 should never be used)
        for (unsigned i = 1; i <= W; ++i) {
            l.values[i] = FPN_DivNoAssert(one, FPN_FromInt<F>((int64_t)i));
        }
        return l;
    }();
    return lut;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GetReciprocalLUT]
//======================================================================

} // namespace tt
