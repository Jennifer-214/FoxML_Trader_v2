// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/RollingStats.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[rolling market statistics — O(1) sliding-window price/volume regression + monotonic min/max deques; feeds gate conditions + regime classification]
// [CONTAINS]
//   - [STRUCT]_[RollingStats]
//   - [FUNCTION]_[RollingStats_Init]
//   - [FUNCTION]_[RollingStats_Push]
//   - [FUNCTION]_[RollingStats_VolumeSignificant]
//   - [FUNCTION]_[RollingStats_EntrySpacing]
//   - [FUNCTION]_[RollingStats_BuyPrice]
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

//======================================================================
// [STRUCT]_[RollingStats]
//----------------------------------------------------------------------
// [TAG]_[[SLOW_PATH] [DATA_ORIENTED_DESIGN] [BINARY_FP] [CONCURRENCY]]
// [SCOPE]_[NODE]
// [THREAD]_[[SLOW_WRITER] [GUI_READER]]
// [INSTANTIATION]_[[128] [256] [512] [1024]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[rolling-window stats state — read-heavy outputs clustered at struct head, write-side head/count isolated on a fresh line via alignas(64), ring buffers + monotonic deques behind]
// [REFERENCE]_[INVARIANT]_[[H4] [H6]]
// [REFERENCE]_[AUDIT]_[latency-optimization-part-2.4]
// [REFERENCE]_[PLAN]_[2026-05-06-latency-path-discipline.md]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W = 128> struct RollingStats {
    static_assert(W > 0 && (W & (W - 1)) == 0, "W must be power of 2");

    // E.1.2.G — expose the window size so CONSUMERS can derive it instead of
    // re-typing it. FracDiffPriceCompute previously carried its own
    // `constexpr int W = 128`, a duplicated constant that would have silently
    // corrupted the walk if this instantiation ever changed: it indexes
    // price_buf with its own W, not the buffer's. Deriving closes that at the
    // source rather than once per consumer.
    static constexpr unsigned WINDOW = W;

    //------------------------------------------------------------------
    // [SECTION]_[READ-HEAVY OUTPUTS]
    //------------------------------------------------------------------
    // v5.11.2.B layout reorder: outputs cluster at struct head; engine writes
    // them once per slow-path cycle. GUI thread + strategies + regime detector
    // read every cycle. Pre-v5.11.2.B: head/count interleaved with outputs at
    // offset 0-7 → cross-thread false sharing with GUI on every Push.
    // Post-v5.11.2.B: head/count moved past the 5-cache-line output cluster
    // via alignas(64). Engine's per-Push writes to head/count don't invalidate
    // the GUI's L1d copy of outputs.
    //
    // Audit: LATENCY_OPTIMIZATION_AUDIT.md Part 2.4
    // Discipline: plans/_cross-cutting/2026-05-06-latency-path-discipline.md Rule 1 + Rule 7
    FPN_Binary<F> price_avg;          // mean price over window
    FPN_Binary<F> price_slope;        // least-squares regression slope (positive = rising)
    FPN_Binary<F> price_r_squared;    // regression R² (0-1, trend consistency)
    FPN_Binary<F> price_variance;     // real variance (sum((p-avg)²)/n, no sqrt)
    FPN_Binary<F> price_stddev;       // range/4 approximation (kept for config compatibility)
    FPN_Binary<F> price_min;          // min price in window
    FPN_Binary<F> price_max;          // max price in window
    FPN_Binary<F> volume_avg;         // mean volume over window
    FPN_Binary<F> volume_slope;       // least-squares regression slope of volume
    FPN_Binary<F> volume_max;         // max volume in window (for spike detection)
    FPN_Binary<F> volume_delta;       // (buy - sell) / (buy + sell), range [-1.0, +1.0]
    FPN_Binary<F> vwap;               // pv_sum / vol_sum
    FPN_Binary<F> vwap_deviation;     // (price - vwap) / vwap (negative = below VWAP)
    // 13 × FPN_Binary<64>=16B = 208 bytes ≈ 4 cache lines (0-3) (Ship-A 16B flip; was 24B/312B/5 lines)

    //------------------------------------------------------------------
    // [SECTION]_[WRITE-HEAVY INTERNAL STATE (cache-line-isolated from outputs)]
    //------------------------------------------------------------------
    // alignas(64) on `head` forces it to start on a fresh cache line. The
    // running sums following stay clustered with head/count; engine mutates
    // all of these every Push, so co-locating them keeps write-side L1d
    // dirtying tight.
    //
    // v5.11.2.C running sums: price_sum_running / price_sum_y2_running /
    // price_sum_xy_running / volume_sum_running / vol_sum_xy_running replace
    // the per-Push O(W) accumulator loop with O(1) slide-in/slide-out updates.
    // FPN_Binary<F=64> is exact integer math so no periodic resync is needed for drift
    // bounding (the standard floating-point concern doesn't apply).
    alignas(64) int head;
    int count;
    FPN_Binary<F> buy_volume_sum;     // running sum of buyer-initiated volume
    FPN_Binary<F> sell_volume_sum;    // running sum of seller-initiated volume
    FPN_Binary<F> pv_sum;             // running sum(price * volume)
    FPN_Binary<F> vol_sum;            // running sum(volume)
    FPN_Binary<F> price_sum_running;     // v5.11.2.C — running sum of prices
    FPN_Binary<F> price_sum_y2_running;  // v5.11.2.C — running sum of prices²
    FPN_Binary<F> price_sum_xy_running;  // v5.11.2.C — running sum of i * price[i] (i = window position)
    FPN_Binary<F> volume_sum_running;    // v5.11.2.C — running sum of volumes (separate from vol_sum which is for VWAP)
    FPN_Binary<F> vol_sum_xy_running;    // v5.11.2.C — running sum of i * volume[i]

    //------------------------------------------------------------------
    // [SECTION]_[RING BUFFERS (large; only read during eviction, not iterated per Push)]
    //------------------------------------------------------------------
    FPN_Binary<F> price_buf[W];
    FPN_Binary<F> volume_buf[W];
    // TECH_DEBT-293 T1 (delete pv_buf) — MEASURED AND DEFERRED, not forgotten.
    // Recomputing `FPN_Mul(oldest_price, oldest_volume)` at eviction is exactly
    // bytewise-identical (verified: all 4 window hashes matched) and saves
    // 16 B/slot = 30,720 B across the cohort. But it is NOT free: measured on
    // W=128 it costs p50 +358ns / p99 +794ns (~+5%/+7.5%) because a 128-bit
    // FPN_Mul is several instructions against one 16B load, and it also grew
    // RollingStats_Push by 94 instructions. On W=1024 it is free-to-better
    // (the smaller ring pays back in misses), so the trade is window-DEPENDENT.
    // Latency outranks memory in this codebase's priority order, so shipping a
    // measured ~5% regression on the smallest/hottest window to buy DRAM
    // footprint the tooling says is not L1d-relevant is an OPERATOR call, not an
    // agent one. Left in place; the decision is homed in TECH_DEBT-293.
    FPN_Binary<F> pv_buf[W];          // price*volume per sample (for eviction)
    //
    // TECH_DEBT-293 T2 — `int side_buf[W]` (4 B/slot) BIT-PACKED to one bit/slot.
    // Was gated on PARITY-047: while `is_buyer_maker` was never forwarded, every
    // bit here was provably zero, so a footprint pass would have read the array as
    // dead and deleted the only structure able to carry the fix — cementing
    // volume_delta at +1.0 permanently. PARITY-047 closed 2026-08-23; the bits now
    // carry real trade sides, so packing them is safe.
    // Manual word+bit arithmetic, never a C++ bitfield (H14: layout/packing-order
    // are implementation-defined, which conflicts with the size-pins below).
    // CEILING division, not W/64. My first cut asserted `W % 64 == 0` on the
    // grounds that every FOREACH_ROLLING_WINDOW row is 128/256/512/1024 — true of
    // the REGISTRY and false of the template's actual instantiation set: the suite
    // instantiates RollingStats<64,8> for a focused small-window test. Enumerating
    // the registry is not enumerating the callers, and the compiler caught it.
    uint64_t side_bits[(W + 63) / 64];   // bit i = is_buyer_maker for ring slot i

    //------------------------------------------------------------------
    // [SECTION]_[MONOTONIC DEQUES for O(1) sliding-window min/max (v5.11.2.C)]
    //------------------------------------------------------------------
    // Each deque holds slot indices into price_buf / volume_buf, ordered
    // chronologically (front = oldest). The min-deque maintains values
    // monotonically increasing front-to-back; max-deque monotonically
    // decreasing. Front always holds the min/max of the current window.
    //
    // Push amortizes O(1): each slot enters the deque exactly once and is
    // popped at most once. Worst-case burst (monotonically extreme input)
    // is O(W), but bounded — total work over N pushes stays O(N).
    //
    // Storage: 3 × (W ints + 2 ints) = 3W+6 ints. For W=128: 1560B ≈ 25
    // cache lines, slow-path-only access (engine slow + this struct's
    // own Push), so cold-on-arrival cost is the storage trade-off.
    int min_dq[W];      // price-min deque (slot indices)
    int min_dq_head;    // front index in ring (next pop-front position)
    int min_dq_size;    // valid element count
    int max_dq[W];      // price-max deque
    int max_dq_head;
    int max_dq_size;
    int vmax_dq[W];     // volume-max deque
    int vmax_dq_head;
    int vmax_dq_size;
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// W must be power of 2 for branchless wrap with & (W - 1)
// default W=128: at BTC trade frequency this is roughly 10-30 seconds of market data
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//----------------------------------------------------------------------
// [SIZE]_[65984B]
// [ALIGN]_[64]
// [CACHE_LINES]_[1031]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[RollingStats]
//======================================================================

// v5.11.2.B layout invariants — compile-time enforced.
// Verified for W=128 (default); alignas(64) on `head` propagates the discipline
// to all W instantiations (W=256/512/1024 also instantiated for rolling_medium /
// rolling_long / rolling_baseline per StrategyParameters.hpp).
//
// offsetof() is a preprocessor macro that splits on commas — wrap the
// templated type in a using-alias so the comma stays inside the type.
namespace detail { using RollingStats_64_128 = RollingStats<64, 128>; }
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(head) % 64 == 0]
static_assert((offsetof(detail::RollingStats_64_128, head) % 64) == 0,
              "head must be cache-line-aligned (alignas(64) on field) — "
              "see plans/_cross-cutting/2026-05-06-latency-path-discipline.md Rule 1");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(head) >= 64*4 — head starts AFTER the output cluster]
static_assert(offsetof(detail::RollingStats_64_128, head) >= 64 * 4,
              "head must come AFTER the 4-cache-line output cluster (Ship-A 16B FPN_Binary; was 5 lines) — "
              "outputs (price_avg through vwap_deviation) read by GUI thread; "
              "head writes by engine must not share line with them");

// SIZE PINS (mechanical SSoT — replaces the hand-computed size comments that DRIFTED
// ~23x on PortfolioController.hpp:253-257; "~1.5MB" was rolling_baseline x16 cores at
// the OLD 24B FPN, mis-placed on a per-core field — see decision log D-229). sizeof has
// no "scope" assumption: these are the unambiguous per-INSTANCE sizes. A layout change
// is now a COMPILE error; recompute via tools/check_struct_size_budget.py and update the
// number here. Sister to the alignment asserts above + check_struct_alignment.py(c).
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(RollingStats<64,128>) == 8192]
static_assert(sizeof(RollingStats<64, 128>) == 8192, "RollingStats<64,128> size-pin (~8.0KB after TECH_DEBT-293 T2 side_buf bit-pack)");
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(RollingStats<64,256>) == 15872]
static_assert(sizeof(RollingStats<64, 256>) == 15872, "RollingStats<64,256> size-pin (~15.5KB after TECH_DEBT-293 T2 side_buf bit-pack)");
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(RollingStats<64,512>) == 31232]
static_assert(sizeof(RollingStats<64, 512>) == 31232, "RollingStats<64,512> size-pin (~30.5KB after TECH_DEBT-293 T2 side_buf bit-pack)");
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(RollingStats<64,1024>) == 62016]
static_assert(sizeof(RollingStats<64, 1024>) == 62016, "RollingStats<64,1024> size-pin (~60.6KB after TECH_DEBT-293 T2 side_buf bit-pack)");

//======================================================================
// [FUNCTION]_[RollingStats_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[zero-init every output, running sum, ring slot + deque — call once before first Push]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W = 128> inline RollingStats<F, W> RollingStats_Init() {
    RollingStats<F, W> rs;
    for (int i = 0; i < (int)((W + 63) / 64); i++) rs.side_bits[i] = 0ULL;   // T2
    for (int i = 0; i < (int)W; i++) {
        rs.price_buf[i]  = FPN_Zero<F>();
        rs.volume_buf[i] = FPN_Zero<F>();
        rs.pv_buf[i]     = FPN_Zero<F>();
        rs.min_dq[i]     = 0;
        rs.max_dq[i]     = 0;
        rs.vmax_dq[i]    = 0;
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
    rs.pv_sum          = FPN_Zero<F>();
    rs.vol_sum         = FPN_Zero<F>();
    rs.vwap            = FPN_Zero<F>();
    rs.vwap_deviation  = FPN_Zero<F>();
    rs.price_sum_running    = FPN_Zero<F>();
    rs.price_sum_y2_running = FPN_Zero<F>();
    rs.price_sum_xy_running = FPN_Zero<F>();
    rs.volume_sum_running   = FPN_Zero<F>();
    rs.vol_sum_xy_running   = FPN_Zero<F>();
    rs.min_dq_head  = 0;
    rs.min_dq_size  = 0;
    rs.max_dq_head  = 0;
    rs.max_dq_size  = 0;
    rs.vmax_dq_head = 0;
    rs.vmax_dq_size = 0;
    return rs;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RollingStats_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[RollingStats_Push]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP]]
// [COMPLEXITY]_[THROUGHPUT O(1) amortized (running sums + monotonic deques); a SINGLE push is O(W) when a monotone run collapses — an amortized bound does NOT discharge H8's p99 TAIL budget (TECH_DEBT-292)]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[add one price/volume sample + recompute ALL rolling outputs — 9 mask-blend phases, no per-push window loop since v5.11.2.C]
// [REFERENCE]_[INVARIANT]_[[H4] [H8] [H11]]
//======================================================================
// [CODE]
//======================================================================
// PARITY-047 — `is_buyer_maker` is DELIBERATELY NOT DEFAULTED. It carried
// `= 0` from v5.10.X until 2026-08-23, and the sharded call sites simply omitted
// it: the default absorbed the omission at COMPILE time, so all volume routed to
// buy, `sell_volume_sum` never left zero, and `volume_delta` computed buy/buy =
// exactly +1.0 for the life of the process. That silently disabled a
// falling-knife capital gate, two halt reasons, a GUI readout and an ML feature —
// for roughly five sprints, through a full green suite.
//
// It is not a hypothetical: on 2026-08-23 a stale editor buffer re-dropped the
// argument at all four live call sites and the suite still passed 3982/0. No test
// can see a swallowed argument. Requiring it makes the next omission a COMPILE
// ERROR at every call site, which is the only thing that would have caught either
// event. Do not restore the default (H21 sister: TECH_DEBT-288's defaulted-tail
// cohort; `feedback_guards_compound_enforcement_is_leverage`).
template <unsigned F, unsigned W>
inline void RollingStats_Push(RollingStats<F, W> *rs, FPN_Binary<F> price, FPN_Binary<F> volume, int is_buyer_maker) {
    // ── v5.11.2.C — O(1) Push: running sums + monotonic deques replace the
    //               per-Push O(W) accumulator loop. See RollingStats struct
    //               docstring for the running-sum + deque storage rationale.
    //
    // Update order matters:
    //   1. Capture old state (oldest sample, count_old, sum snapshots)
    //   2. Update directional volumes + pv_sum + vol_sum (use mask-blend
    //      for warmup/full eviction term)
    //   3. Compute VWAP outputs (mask-blend on vol_sum != 0)
    //   4. Write new sample to ring buffers
    //   5. Update sum_xy + vol_sum_xy using sum_y_snapshot (BEFORE updating
    //      sum_y), then update sum_y / sum_y2 / sum_v
    //   6. Update min/max deques (slot-based eviction + back-pop while-loops)
    //   7. Increment head + count
    //   8. Compute volume_delta (mask-blend on total_dir_vol != 0)
    //   9. Recompute regression outputs from running sums (no loop)

    //------------------------------------------------------------
    // [SECTION]_[1. Old state capture]
    //------------------------------------------------------------
    int evict_int = (rs->count >= (int)W);
    uint64_t evict_mask = -(uint64_t)evict_int;
    int slot = rs->head;
    FPN_Binary<F> oldest_price  = rs->price_buf[slot];   // garbage when count==0; masked to zero by evict_mask
    FPN_Binary<F> oldest_volume = rs->volume_buf[slot];
    FPN_Binary<F> oldest_pv     = rs->pv_buf[slot];
    // T2: one-bit read, branchless — yields exactly 0 or 1, which is what the
    // sell/buy evict masks below require.
    int oldest_side      = (int)((rs->side_bits[slot >> 6] >> (slot & 63)) & 1ULL);
    FPN_Binary<F> sum_y_snapshot = rs->price_sum_running;   // for sum_xy formula (uses pre-update sum_y)
    FPN_Binary<F> sum_v_snapshot = rs->volume_sum_running;  // for vol_sum_xy formula
    int count_old = rs->count;

    //------------------------------------------------------------
    // [SECTION]_[2. Common eviction terms (zero when warmup, real when full)]
    //------------------------------------------------------------
    FPN_Binary<F> evict_p   = FPN_BlendOnMask(oldest_price,  FPN_Zero<F>(), evict_mask);
    FPN_Binary<F> evict_v   = FPN_BlendOnMask(oldest_volume, FPN_Zero<F>(), evict_mask);
    FPN_Binary<F> evict_pv  = FPN_BlendOnMask(oldest_pv,     FPN_Zero<F>(), evict_mask);
    FPN_Binary<F> oldest_p2 = FPN_Mul(oldest_price, oldest_price);
    FPN_Binary<F> evict_p2  = FPN_BlendOnMask(oldest_p2, FPN_Zero<F>(), evict_mask);

    // Directional volume eviction: mask is (evict & matching_side).
    int sell_evict_int = evict_int & oldest_side;
    int buy_evict_int  = evict_int & (1 - oldest_side);
    FPN_Binary<F> sell_evict_v = FPN_BlendOnMask(oldest_volume, FPN_Zero<F>(), -(uint64_t)sell_evict_int);
    FPN_Binary<F> buy_evict_v  = FPN_BlendOnMask(oldest_volume, FPN_Zero<F>(), -(uint64_t)buy_evict_int);

    // Directional volume add: mask is is_buyer_maker (sell side gets the volume when set).
    uint64_t is_sell_mask = -(uint64_t)is_buyer_maker;
    FPN_Binary<F> add_to_sell = FPN_BlendOnMask(volume,        FPN_Zero<F>(), is_sell_mask);
    FPN_Binary<F> add_to_buy  = FPN_BlendOnMask(FPN_Zero<F>(), volume,        is_sell_mask);

    rs->buy_volume_sum  = FPN_AddSat(FPN_SubSat(rs->buy_volume_sum,  buy_evict_v),  add_to_buy);
    rs->sell_volume_sum = FPN_AddSat(FPN_SubSat(rs->sell_volume_sum, sell_evict_v), add_to_sell);

    // ── pv_sum + vol_sum ──
    FPN_Binary<F> new_pv = FPN_Mul(price, volume);
    rs->pv_sum  = FPN_AddSat(FPN_SubSat(rs->pv_sum,  evict_pv), new_pv);
    rs->vol_sum = FPN_AddSat(FPN_SubSat(rs->vol_sum, evict_v),  volume);

    //------------------------------------------------------------
    // [SECTION]_[3. VWAP outputs (mask-blend on vol_sum != 0)]
    //------------------------------------------------------------
    uint64_t vol_nz_mask = -(uint64_t)(!FPN_IsZero(rs->vol_sum));
    FPN_Binary<F> safe_vol = FPN_BlendOnMask(rs->vol_sum, FPN_FromDouble<F>(1.0), vol_nz_mask);
    FPN_Binary<F> new_vwap = FPN_DivNoAssert(rs->pv_sum, safe_vol);
    FPN_Binary<F> safe_vwap = FPN_BlendOnMask(new_vwap, FPN_FromDouble<F>(1.0), vol_nz_mask);
    FPN_Binary<F> new_vwap_dev = FPN_DivNoAssert(FPN_Sub(price, new_vwap), safe_vwap);
    rs->vwap           = FPN_BlendOnMask(new_vwap,     rs->vwap,           vol_nz_mask);
    rs->vwap_deviation = FPN_BlendOnMask(new_vwap_dev, rs->vwap_deviation, vol_nz_mask);

    //------------------------------------------------------------
    // [SECTION]_[4. Write new sample to ring buffer (slot stays valid through deque updates)]
    //------------------------------------------------------------
    rs->price_buf[slot]  = price;
    rs->volume_buf[slot] = volume;
    rs->pv_buf[slot]     = new_pv;
    // T2: branchless set-or-clear of bit `slot` — no data-dependent branch (H7/H20).
    {
        const uint64_t bit = 1ULL << (slot & 63);
        const uint64_t on  = -(uint64_t)(is_buyer_maker != 0);
        uint64_t* w = &rs->side_bits[slot >> 6];
        *w = (*w & ~bit) | (bit & on);
    }

    //------------------------------------------------------------
    // [SECTION]_[5. Running sums for regression]
    //------------------------------------------------------------
    // Position of new sample in the window: full → W-1; warmup → count_old.
    // Branchless: position = count_old + evict_int * (W - 1 - count_old).
    int position_int = count_old + evict_int * ((int)W - 1 - count_old);
    FPN_Binary<F> position_fp = FPN_FromInt<F>((int64_t)position_int);

    // sum_xy update via formula: sum_xy_new = sum_xy_old - subtracted_term + position * new
    //   warmup: subtracted_term = 0,                  position = count_old
    //   full:   subtracted_term = sum_y_old - oldest, position = W - 1
    FPN_Binary<F> sum_y_excl  = FPN_SubSat(sum_y_snapshot, oldest_price);
    FPN_Binary<F> evict_xy    = FPN_BlendOnMask(sum_y_excl, FPN_Zero<F>(), evict_mask);
    FPN_Binary<F> contrib_xy  = FPN_Mul(position_fp, price);
    rs->price_sum_xy_running = FPN_AddSat(FPN_SubSat(rs->price_sum_xy_running, evict_xy), contrib_xy);

    FPN_Binary<F> sum_v_excl    = FPN_SubSat(sum_v_snapshot, oldest_volume);
    FPN_Binary<F> evict_v_xy    = FPN_BlendOnMask(sum_v_excl, FPN_Zero<F>(), evict_mask);
    FPN_Binary<F> contrib_v_xy  = FPN_Mul(position_fp, volume);
    rs->vol_sum_xy_running = FPN_AddSat(FPN_SubSat(rs->vol_sum_xy_running, evict_v_xy), contrib_v_xy);

    // sum_y / sum_y2 / sum_v: standard slide-in/slide-out
    rs->price_sum_running    = FPN_AddSat(FPN_SubSat(rs->price_sum_running,    evict_p),  price);
    FPN_Binary<F> new_p2 = FPN_Mul(price, price);
    rs->price_sum_y2_running = FPN_AddSat(FPN_SubSat(rs->price_sum_y2_running, evict_p2), new_p2);
    rs->volume_sum_running   = FPN_AddSat(FPN_SubSat(rs->volume_sum_running,   evict_v),  volume);

    //------------------------------------------------------------
    // [SECTION]_[6. Monotonic deque min/max updates]
    //------------------------------------------------------------
    // (a) Pop expired entries from front. The old slot value is being aged out
    //     iff this slot is being evicted (count >= W) AND the deque front's
    //     stored slot index equals this slot. (Deque only references in-window
    //     slots by design; the only way a stored slot becomes out-of-window is
    //     via this exact eviction.)
    int min_front_expires = evict_int & (rs->min_dq_size > 0)
                          & (rs->min_dq[rs->min_dq_head] == slot);
    rs->min_dq_head  = (rs->min_dq_head + min_front_expires) & ((int)W - 1);
    rs->min_dq_size -= min_front_expires;

    int max_front_expires = evict_int & (rs->max_dq_size > 0)
                          & (rs->max_dq[rs->max_dq_head] == slot);
    rs->max_dq_head  = (rs->max_dq_head + max_front_expires) & ((int)W - 1);
    rs->max_dq_size -= max_front_expires;

    int vmax_front_expires = evict_int & (rs->vmax_dq_size > 0)
                           & (rs->vmax_dq[rs->vmax_dq_head] == slot);
    rs->vmax_dq_head  = (rs->vmax_dq_head + vmax_front_expires) & ((int)W - 1);
    rs->vmax_dq_size -= vmax_front_expires;

    // (b) Pop "worse" entries from back. While-loop is data-dependent but
    //     amortizes O(1) — each slot enters once and is popped at most once,
    //     so total work over N pushes is O(N).
    while (rs->min_dq_size > 0) {
        int back_pos  = (rs->min_dq_head + rs->min_dq_size - 1) & ((int)W - 1);
        int back_slot = rs->min_dq[back_pos];
        if (FPN_GreaterThan(rs->price_buf[back_slot], price)) rs->min_dq_size--;
        else break;
    }
    rs->min_dq[(rs->min_dq_head + rs->min_dq_size) & ((int)W - 1)] = slot;
    rs->min_dq_size++;

    while (rs->max_dq_size > 0) {
        int back_pos  = (rs->max_dq_head + rs->max_dq_size - 1) & ((int)W - 1);
        int back_slot = rs->max_dq[back_pos];
        if (FPN_LessThan(rs->price_buf[back_slot], price)) rs->max_dq_size--;
        else break;
    }
    rs->max_dq[(rs->max_dq_head + rs->max_dq_size) & ((int)W - 1)] = slot;
    rs->max_dq_size++;

    while (rs->vmax_dq_size > 0) {
        int back_pos  = (rs->vmax_dq_head + rs->vmax_dq_size - 1) & ((int)W - 1);
        int back_slot = rs->vmax_dq[back_pos];
        if (FPN_LessThan(rs->volume_buf[back_slot], volume)) rs->vmax_dq_size--;
        else break;
    }
    rs->vmax_dq[(rs->vmax_dq_head + rs->vmax_dq_size) & ((int)W - 1)] = slot;
    rs->vmax_dq_size++;

    // (c) Output current min/max from deque fronts (always valid post-update)
    rs->price_min  = rs->price_buf[rs->min_dq[rs->min_dq_head]];
    rs->price_max  = rs->price_buf[rs->max_dq[rs->max_dq_head]];
    rs->volume_max = rs->volume_buf[rs->vmax_dq[rs->vmax_dq_head]];

    //------------------------------------------------------------
    // [SECTION]_[7. Advance head + count]
    //------------------------------------------------------------
    rs->head  = (rs->head + 1) & ((int)W - 1);
    rs->count += (rs->count < (int)W);

    //------------------------------------------------------------
    // [SECTION]_[8. Volume delta = (buy - sell) / (buy + sell), branchless via mask-blend]
    //------------------------------------------------------------
    FPN_Binary<F> total_dir_vol = FPN_AddSat(rs->buy_volume_sum, rs->sell_volume_sum);
    uint64_t total_nz_mask = -(uint64_t)(!FPN_IsZero(total_dir_vol));
    FPN_Binary<F> safe_total_dir = FPN_BlendOnMask(total_dir_vol, FPN_FromDouble<F>(1.0), total_nz_mask);
    FPN_Binary<F> diff = FPN_Sub(rs->buy_volume_sum, rs->sell_volume_sum);
    FPN_Binary<F> raw_delta = FPN_DivNoAssert(diff, safe_total_dir);
    rs->volume_delta = FPN_BlendOnMask(raw_delta, FPN_Zero<F>(), total_nz_mask);

    if (rs->count < 2) return;  // need at least 2 samples for regression

    //------------------------------------------------------------
    // [SECTION]_[9. Regression outputs from running sums (no loop)]
    //------------------------------------------------------------
    int n = rs->count;
    FPN_Binary<F> n_fp = FPN_FromInt<F>(n);
    int64_t n_l = (int64_t)n;
    FPN_Binary<F> sum_x  = FPN_FromInt<F>(n_l * (n_l - 1) / 2);
    FPN_Binary<F> sum_x2 = FPN_FromInt<F>(n_l * (n_l - 1) * (2 * n_l - 1) / 6);

    // v5.11.2.A: branchless 1/n via reciprocal LUT (FPN_Mul, not FPN_DivNoAssert)
    const auto& recip = tt::GetReciprocalLUT<F, W>();
    rs->price_avg  = FPN_Mul(rs->price_sum_running,  recip.values[n]);
    rs->volume_avg = FPN_Mul(rs->volume_sum_running, recip.values[n]);

    // stddev approximation: range / 4
    FPN_Binary<F> range = FPN_Sub(rs->price_max, rs->price_min);
    rs->price_stddev = FPN_DivNoAssert(range, FPN_FromDouble<F>(4.0));

    // real variance: var = (n*sum_y2 - sum_y²) / n²
    FPN_Binary<F> ss_total = FPN_SubSat(FPN_Mul(n_fp, rs->price_sum_y2_running),
                                  FPN_Mul(rs->price_sum_running, rs->price_sum_running));
    FPN_Binary<F> recip_n_sq = FPN_Mul(recip.values[n], recip.values[n]);
    rs->price_variance = FPN_Mul(ss_total, recip_n_sq);

    // OLS slope = (n*sum_xy - sum_x*sum_y) / (n*sum_x2 - sum_x²); R² = slope * num / ss_total
    FPN_Binary<F> numerator   = FPN_Sub(FPN_Mul(n_fp, rs->price_sum_xy_running),
                                  FPN_Mul(sum_x, rs->price_sum_running));
    FPN_Binary<F> denominator = FPN_Sub(FPN_Mul(n_fp, sum_x2), FPN_Mul(sum_x, sum_x));

    uint64_t denom_nz_mask = -(uint64_t)(!FPN_IsZero(denominator));
    FPN_Binary<F> safe_denom = FPN_BlendOnMask(denominator, FPN_FromDouble<F>(1.0), denom_nz_mask);
    FPN_Binary<F> raw_slope  = FPN_DivNoAssert(numerator, safe_denom);
    rs->price_slope   = FPN_BlendOnMask(raw_slope, FPN_Zero<F>(), denom_nz_mask);

    uint64_t total_xn_mask = denom_nz_mask & -(uint64_t)(!FPN_IsZero(ss_total));
    FPN_Binary<F> safe_total = FPN_BlendOnMask(ss_total, FPN_FromDouble<F>(1.0), total_xn_mask);
    FPN_Binary<F> raw_r2     = FPN_Mul(rs->price_slope, FPN_DivNoAssert(numerator, safe_total));
    rs->price_r_squared = FPN_BlendOnMask(raw_r2, FPN_Zero<F>(), total_xn_mask);

    FPN_Binary<F> vol_num   = FPN_Sub(FPN_Mul(n_fp, rs->vol_sum_xy_running),
                                FPN_Mul(sum_x, rs->volume_sum_running));
    FPN_Binary<F> vol_slope = FPN_DivNoAssert(vol_num, safe_denom);
    rs->volume_slope = FPN_BlendOnMask(vol_slope, FPN_Zero<F>(), denom_nz_mask);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// adds a new price/volume sample and recomputes all rolling statistics
// this runs on the slow path (every poll_interval ticks), not every tick
//
// running sums maintain the 5 regression sums (following LinearRegression3X_Fit):
//   sum_y (price), sum_y2 (price²), sum_xy (index*price), sum_vol, sum_vol_xy
// x-values are time indices 0..count-1, so sum_x and sum_x2 are computed from count alone
//======================================================================
// [END_FUNCTION]_[RollingStats_Push]
//======================================================================

//======================================================================
// [FUNCTION]_[RollingStats_VolumeSignificant]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[volume >= multiplier * rolling_avg -> 1 else 0 — mask value, AND-able with other gate conditions]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W>
inline int RollingStats_VolumeSignificant(const RollingStats<F, W> *rs, FPN_Binary<F> tick_volume, FPN_Binary<F> multiplier) {
    FPN_Binary<F> threshold = FPN_Mul(rs->volume_avg, multiplier);
    return FPN_GreaterThanOrEqual(tick_volume, threshold);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// returns 1 if the given volume is >= multiplier * rolling_avg, 0 otherwise
// branchless - produces a mask value the caller can AND with other conditions
//======================================================================
// [END_FUNCTION]_[RollingStats_VolumeSignificant]
//======================================================================

//======================================================================
// [FUNCTION]_[RollingStats_EntrySpacing]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[volatility-scaled minimum entry spacing = max(stddev * mult, 0.03% of avg price floor)]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W>
inline FPN_Binary<F> RollingStats_EntrySpacing(const RollingStats<F, W> *rs, FPN_Binary<F> spacing_multiplier) {
    // floor: at least 0.03% of avg price — prevents tight clustering when stddev is low
    // (e.g. right after warmup or during calm markets with compressed volatility)
    // 0.03% of $70k = ~$21, comparable to steady-state spacing with stddev ~$10
    FPN_Binary<F> vol_spacing = FPN_Mul(rs->price_stddev, spacing_multiplier);
    FPN_Binary<F> min_floor = FPN_Mul(rs->price_avg, FPN_FromDouble<F>(0.0003));
    return FPN_Max(vol_spacing, min_floor);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// computes the minimum price distance between entries based on rolling volatility
// spacing = stddev * spacing_multiplier
// returns the FPN_Binary spacing value - caller compares against nearest existing position
//======================================================================
// [END_FUNCTION]_[RollingStats_EntrySpacing]
//======================================================================

//======================================================================
// [FUNCTION]_[RollingStats_BuyPrice]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[buy gate price = rolling_avg - (rolling_avg * offset_pct) — buy only on a dip below the mean]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W>
inline FPN_Binary<F> RollingStats_BuyPrice(const RollingStats<F, W> *rs, FPN_Binary<F> offset_pct) {
    FPN_Binary<F> offset = FPN_Mul(rs->price_avg, offset_pct);
    return FPN_Sub(rs->price_avg, offset);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// computes the buy gate price offset from rolling mean
// buy_price = rolling_avg - (rolling_avg * offset_pct)
// this means "only buy when price dips offset_pct below the rolling average"
//======================================================================
// [END_FUNCTION]_[RollingStats_BuyPrice]
//======================================================================
#endif // ROLLING_STATS_HPP
