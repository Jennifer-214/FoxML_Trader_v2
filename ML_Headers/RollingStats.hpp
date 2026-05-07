// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ROLLING MARKET STATISTICS]
//======================================================================================================
// tracks rolling averages, trends, and regression quality for price and volume
// used by the controller to set dynamic buy gate conditions and classify market regime
//
// outputs (recomputed every push via least-squares regression):
//   price_avg, price_slope, price_r_squared  — where is price, which direction, how consistent
//   price_stddev (range/4 approx)            — volatility proxy for spacing/TP/SL
//   price_variance                           — real variance for ratio comparisons
//   volume_avg, volume_slope                 — volume trend for filtering and confirmation
//   price_min, price_max                     — range bounds
//
// regression math follows LinearRegression3X_Fit (ordinary least squares, 5 sums)
// x-values are time indices 0..count-1, precomputable: sum_x = n(n-1)/2, sum_x2 = n(n-1)(2n-1)/6
//
// uses ring buffer with branchless power-of-2 wrap
// window size W is a template parameter, defaulting to 128
//======================================================================================================
#ifndef ROLLING_STATS_HPP
#define ROLLING_STATS_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include "ReciprocalLUT.hpp"  // v5.11.2.A — branchless 1/n via precomputed reciprocals
#include <cstddef>            // v5.11.2.B — offsetof() for cache-layout static_asserts

//======================================================================================================
// [ROLLING STATS STRUCT]
//======================================================================================================
// W must be power of 2 for branchless wrap with & (W - 1)
// default W=128: at BTC trade frequency this is roughly 10-30 seconds of market data
//======================================================================================================
template <unsigned F, unsigned W = 128> struct RollingStats {
    static_assert(W > 0 && (W & (W - 1)) == 0, "W must be power of 2");

    // ── READ-HEAVY OUTPUTS (cache lines 0..4) ──
    // v5.11.2.B layout reorder: outputs cluster at struct head; engine writes
    // them once per slow-path cycle. GUI thread + strategies + regime detector
    // read every cycle. Pre-v5.11.2.B: head/count interleaved with outputs at
    // offset 0-7 → cross-thread false sharing with GUI on every Push.
    // Post-v5.11.2.B: head/count moved past the 5-cache-line output cluster
    // via alignas(64). Engine's per-Push writes to head/count don't invalidate
    // the GUI's L1d copy of outputs.
    //
    // Audit: LATENCY_OPTIMIZATION_AUDIT.md Part 2.4
    // Discipline: plans/2026-05-06-latency-path-discipline.md Rule 1 + Rule 7
    FPN<F> price_avg;          // mean price over window
    FPN<F> price_slope;        // least-squares regression slope (positive = rising)
    FPN<F> price_r_squared;    // regression R² (0-1, trend consistency)
    FPN<F> price_variance;     // real variance (sum((p-avg)²)/n, no sqrt)
    FPN<F> price_stddev;       // range/4 approximation (kept for config compatibility)
    FPN<F> price_min;          // min price in window
    FPN<F> price_max;          // max price in window
    FPN<F> volume_avg;         // mean volume over window
    FPN<F> volume_slope;       // least-squares regression slope of volume
    FPN<F> volume_max;         // max volume in window (for spike detection)
    FPN<F> volume_delta;       // (buy - sell) / (buy + sell), range [-1.0, +1.0]
    FPN<F> vwap;               // pv_sum / vol_sum
    FPN<F> vwap_deviation;     // (price - vwap) / vwap (negative = below VWAP)
    // 13 × FPN<64>=24B = 312 bytes ≈ 5 cache lines (0-4)

    // ── WRITE-HEAVY INTERNAL STATE (cache-line-isolated from outputs) ──
    // alignas(64) on `head` forces it to start on a fresh cache line. The
    // running sums following stay clustered with head/count; engine mutates
    // all of these every Push, so co-locating them keeps write-side L1d
    // dirtying tight.
    alignas(64) int head;
    int count;
    FPN<F> buy_volume_sum;     // running sum of buyer-initiated volume
    FPN<F> sell_volume_sum;    // running sum of seller-initiated volume
    FPN<F> pv_sum;             // running sum(price * volume)
    FPN<F> vol_sum;            // running sum(volume) — separate from volume_sum in loop

    // ── RING BUFFERS (large; only iterated during periodic resync) ──
    FPN<F> price_buf[W];
    FPN<F> volume_buf[W];
    FPN<F> pv_buf[W];          // price*volume per sample (for eviction)
    int side_buf[W];           // is_buyer_maker flags for directional volume eviction
};

// v5.11.2.B layout invariants — compile-time enforced.
// Verified for W=128 (default); alignas(64) on `head` propagates the discipline
// to all W instantiations (W=256/512/1024 also instantiated for rolling_medium /
// rolling_long / rolling_baseline per StrategyParameters.hpp).
//
// offsetof() is a preprocessor macro that splits on commas — wrap the
// templated type in a using-alias so the comma stays inside the type.
namespace detail { using RollingStats_64_128 = RollingStats<64, 128>; }
static_assert((offsetof(detail::RollingStats_64_128, head) % 64) == 0,
              "head must be cache-line-aligned (alignas(64) on field) — "
              "see plans/2026-05-06-latency-path-discipline.md Rule 1");
static_assert(offsetof(detail::RollingStats_64_128, head) >= 64 * 5,
              "head must come AFTER the 5-cache-line output cluster — "
              "outputs (price_avg through vwap_deviation) read by GUI thread; "
              "head writes by engine must not share line with them");

//======================================================================================================
// [INIT]
//======================================================================================================
template <unsigned F, unsigned W = 128> inline RollingStats<F, W> RollingStats_Init() {
    RollingStats<F, W> rs;
    for (int i = 0; i < (int)W; i++) {
        rs.price_buf[i]  = FPN_Zero<F>();
        rs.volume_buf[i] = FPN_Zero<F>();
    }
    rs.head            = 0;
    rs.count           = 0;
    rs.price_avg       = FPN_Zero<F>();
    rs.price_slope     = FPN_Zero<F>();
    rs.price_r_squared = FPN_Zero<F>();
    rs.price_variance  = FPN_Zero<F>();
    rs.price_stddev    = FPN_Zero<F>();
    rs.price_min       = FPN_Zero<F>();
    rs.price_max       = FPN_Zero<F>();
    rs.volume_avg      = FPN_Zero<F>();
    rs.volume_slope    = FPN_Zero<F>();
    rs.volume_max      = FPN_Zero<F>();
    rs.buy_volume_sum  = FPN_Zero<F>();
    rs.sell_volume_sum = FPN_Zero<F>();
    rs.volume_delta    = FPN_Zero<F>();
    for (int i = 0; i < (int)W; i++) rs.side_buf[i] = 0;
    for (int i = 0; i < (int)W; i++) rs.pv_buf[i] = FPN_Zero<F>();
    rs.pv_sum          = FPN_Zero<F>();
    rs.vol_sum         = FPN_Zero<F>();
    rs.vwap            = FPN_Zero<F>();
    rs.vwap_deviation  = FPN_Zero<F>();
    return rs;
}

//======================================================================================================
// [PUSH + RECOMPUTE]
//======================================================================================================
// adds a new price/volume sample and recomputes all rolling statistics
// this runs on the slow path (every poll_interval ticks), not every tick
// the computation is O(W) which at default 128 is well within the slow-path budget
//
// single pass computes 5 regression sums (following LinearRegression3X_Fit):
//   sum_y (price), sum_y2 (price²), sum_xy (index*price), sum_vol, sum_vol_xy
// x-values are time indices 0..count-1, so sum_x and sum_x2 are computed from count alone
//======================================================================================================
template <unsigned F, unsigned W>
inline void RollingStats_Push(RollingStats<F, W> *rs, FPN<F> price, FPN<F> volume, int is_buyer_maker = 0) {
    // evict oldest directional volume before overwriting ring buffer slot
    if (rs->count >= (int)W) {
        int old_side = rs->side_buf[rs->head];
        FPN<F> old_vol = rs->volume_buf[rs->head];
        if (old_side) rs->sell_volume_sum = FPN_SubSat(rs->sell_volume_sum, old_vol);
        else          rs->buy_volume_sum  = FPN_SubSat(rs->buy_volume_sum,  old_vol);
    }

    // accumulate new directional volume
    if (is_buyer_maker) rs->sell_volume_sum = FPN_AddSat(rs->sell_volume_sum, volume);
    else                rs->buy_volume_sum  = FPN_AddSat(rs->buy_volume_sum,  volume);
    rs->side_buf[rs->head] = is_buyer_maker;

    // VWAP running sums: evict oldest pv, accumulate new
    if (rs->count >= (int)W) {
        rs->pv_sum  = FPN_SubSat(rs->pv_sum, rs->pv_buf[rs->head]);
        rs->vol_sum = FPN_SubSat(rs->vol_sum, rs->volume_buf[rs->head]);
    }
    FPN<F> pv = FPN_Mul(price, volume);
    rs->pv_buf[rs->head] = pv;
    rs->pv_sum  = FPN_AddSat(rs->pv_sum, pv);
    rs->vol_sum = FPN_AddSat(rs->vol_sum, volume);

    // recompute VWAP and deviation
    if (!FPN_IsZero(rs->vol_sum)) {
        rs->vwap = FPN_DivNoAssert(rs->pv_sum, rs->vol_sum);
        rs->vwap_deviation = FPN_DivNoAssert(FPN_Sub(price, rs->vwap), rs->vwap);
    }

    // write to ring buffer
    rs->price_buf[rs->head]  = price;
    rs->volume_buf[rs->head] = volume;
    rs->head  = (rs->head + 1) & ((int)W - 1);
    rs->count += (rs->count < (int)W);

    // compute volume delta: (buy - sell) / (buy + sell), range [-1.0, +1.0]
    FPN<F> total_dir_vol = FPN_AddSat(rs->buy_volume_sum, rs->sell_volume_sum);
    if (!FPN_IsZero(total_dir_vol))
        rs->volume_delta = FPN_DivNoAssert(FPN_Sub(rs->buy_volume_sum, rs->sell_volume_sum), total_dir_vol);
    else
        rs->volume_delta = FPN_Zero<F>();

    if (rs->count < 2) return; // need at least 2 samples for meaningful stats

    int n = rs->count;

    // single pass: accumulate sums for regression, averages, and min/max
    FPN<F> price_sum    = FPN_Zero<F>();
    FPN<F> volume_sum   = FPN_Zero<F>();
    FPN<F> price_sum_xy = FPN_Zero<F>();   // sum(i * price[i])
    FPN<F> price_sum_y2 = FPN_Zero<F>();   // sum(price[i]²)
    FPN<F> vol_sum_xy   = FPN_Zero<F>();   // sum(i * volume[i])

    FPN<F> p_min = rs->price_buf[(rs->head - n + (int)W) & ((int)W - 1)];
    FPN<F> p_max = p_min;
    FPN<F> v_max = rs->volume_buf[(rs->head - n + (int)W) & ((int)W - 1)];

    for (int i = 0; i < n; i++) {
        int idx = (rs->head - n + i + (int)W) & ((int)W - 1);
        FPN<F> p = rs->price_buf[idx];
        FPN<F> v = rs->volume_buf[idx];
        FPN<F> i_fp = FPN_FromInt<F>(i);  // v5.10.0b.1: integer constructor (no double round-trip)

        price_sum    = FPN_AddSat(price_sum, p);
        volume_sum   = FPN_AddSat(volume_sum, v);
        price_sum_xy = FPN_AddSat(price_sum_xy, FPN_Mul(i_fp, p));
        price_sum_y2 = FPN_AddSat(price_sum_y2, FPN_Mul(p, p));
        vol_sum_xy   = FPN_AddSat(vol_sum_xy, FPN_Mul(i_fp, v));

        p_min = FPN_Min(p_min, p);
        p_max = FPN_Max(p_max, p);
        v_max = FPN_Max(v_max, v);
    }

    // precompute x-sums from count (x = 0, 1, ..., n-1)
    // sum_x  = n*(n-1)/2
    // sum_x2 = n*(n-1)*(2n-1)/6
    // v5.10.0b.1: pure integer math (no IEEE-754 reordering across builds);
    // n is bounded by W=128 so sum_x2 max = 128*127*255/6 = 691,520 — fits in int64
    FPN<F> n_fp   = FPN_FromInt<F>(n);
    int64_t n_l = (int64_t)n;
    FPN<F> sum_x  = FPN_FromInt<F>(n_l * (n_l - 1) / 2);
    FPN<F> sum_x2 = FPN_FromInt<F>(n_l * (n_l - 1) * (2 * n_l - 1) / 6);

    // v5.11.2.A — Branchless averaging via precomputed reciprocal LUT.
    // FPN_Mul(sum, recip[n]) replaces FPN_DivNoAssert(sum, n_fp).
    // ULP drift bounded ≤ 1 LSB for non-power-of-2 n; replay-determinism
    // baseline regenerated. See ReciprocalLUT.hpp for full trade-off context.
    const auto& recip = tt::GetReciprocalLUT<F, W>();
    rs->price_avg  = FPN_Mul(price_sum, recip.values[n]);
    rs->volume_avg = FPN_Mul(volume_sum, recip.values[n]);
    rs->volume_max = v_max;
    rs->price_min  = p_min;
    rs->price_max  = p_max;

    // stddev approximation: range / 4 = range >> 2 (exact, no LUT needed since 4 is power-of-2)
    FPN<F> range = FPN_Sub(p_max, p_min);
    rs->price_stddev = FPN_DivNoAssert(range, FPN_FromDouble<F>(4.0));

    // real variance: var = (n*sum_y2 - sum_y²) / n²
    // Use recip[n] * recip[n] for the n² division (one extra FPN_Mul, ULP drift
    // already captured by the per-n drift; no compounding past the LSB).
    FPN<F> ss_total = FPN_SubSat(FPN_Mul(n_fp, price_sum_y2), FPN_Mul(price_sum, price_sum));
    FPN<F> recip_n_sq = FPN_Mul(recip.values[n], recip.values[n]);
    rs->price_variance = FPN_Mul(ss_total, recip_n_sq);

    // price slope via ordinary least squares (same formula as LinearRegression3X_Fit)
    // slope = (n*sum_xy - sum_x*sum_y) / (n*sum_x2 - sum_x²)
    FPN<F> numerator   = FPN_Sub(FPN_Mul(n_fp, price_sum_xy), FPN_Mul(sum_x, price_sum));
    FPN<F> denominator = FPN_Sub(FPN_Mul(n_fp, sum_x2), FPN_Mul(sum_x, sum_x));

    int denom_nonzero = !FPN_IsZero(denominator);
    FPN<F> safe_denom  = denom_nonzero ? denominator : FPN_FromDouble<F>(1.0);
    FPN<F> raw_slope   = FPN_DivNoAssert(numerator, safe_denom);
    rs->price_slope    = denom_nonzero ? raw_slope : FPN_Zero<F>();

    // R² = slope * numerator / ss_total
    // splits the fraction to avoid squaring large values (same trick as LinearRegression3X_Fit)
    int total_nonzero   = (!FPN_IsZero(ss_total)) & denom_nonzero;
    FPN<F> safe_total   = total_nonzero ? ss_total : FPN_FromDouble<F>(1.0);
    FPN<F> raw_r2       = FPN_Mul(rs->price_slope, FPN_DivNoAssert(numerator, safe_total));
    rs->price_r_squared = total_nonzero ? raw_r2 : FPN_Zero<F>();

    // volume slope via same formula
    FPN<F> vol_num   = FPN_Sub(FPN_Mul(n_fp, vol_sum_xy), FPN_Mul(sum_x, volume_sum));
    FPN<F> vol_slope = FPN_DivNoAssert(vol_num, safe_denom);
    rs->volume_slope = denom_nonzero ? vol_slope : FPN_Zero<F>();
}

//======================================================================================================
// [VOLUME FILTER]
//======================================================================================================
// returns 1 if the given volume is >= multiplier * rolling_avg, 0 otherwise
// branchless - produces a mask value the caller can AND with other conditions
//======================================================================================================
template <unsigned F, unsigned W>
inline int RollingStats_VolumeSignificant(const RollingStats<F, W> *rs, FPN<F> tick_volume, FPN<F> multiplier) {
    FPN<F> threshold = FPN_Mul(rs->volume_avg, multiplier);
    return FPN_GreaterThanOrEqual(tick_volume, threshold);
}

//======================================================================================================
// [ENTRY SPACING]
//======================================================================================================
// computes the minimum price distance between entries based on rolling volatility
// spacing = stddev * spacing_multiplier
// returns the FPN spacing value - caller compares against nearest existing position
//======================================================================================================
template <unsigned F, unsigned W>
inline FPN<F> RollingStats_EntrySpacing(const RollingStats<F, W> *rs, FPN<F> spacing_multiplier) {
    // floor: at least 0.03% of avg price — prevents tight clustering when stddev is low
    // (e.g. right after warmup or during calm markets with compressed volatility)
    // 0.03% of $70k = ~$21, comparable to steady-state spacing with stddev ~$10
    FPN<F> vol_spacing = FPN_Mul(rs->price_stddev, spacing_multiplier);
    FPN<F> min_floor = FPN_Mul(rs->price_avg, FPN_FromDouble<F>(0.0003));
    return FPN_Max(vol_spacing, min_floor);
}

//======================================================================================================
// [ENTRY OFFSET]
//======================================================================================================
// computes the buy gate price offset from rolling mean
// buy_price = rolling_avg - (rolling_avg * offset_pct)
// this means "only buy when price dips offset_pct below the rolling average"
//======================================================================================================
template <unsigned F, unsigned W>
inline FPN<F> RollingStats_BuyPrice(const RollingStats<F, W> *rs, FPN<F> offset_pct) {
    FPN<F> offset = FPN_Mul(rs->price_avg, offset_pct);
    return FPN_Sub(rs->price_avg, offset);
}

//======================================================================================================
//======================================================================================================
#endif // ROLLING_STATS_HPP
