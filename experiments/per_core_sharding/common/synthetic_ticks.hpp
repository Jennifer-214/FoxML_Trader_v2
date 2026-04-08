// synthetic_ticks.hpp — pre-generated tick stream for the bench harness
//
// We need a deterministic, reproducible stream of ticks that:
//   1. Has realistic-looking price moves (random walk + small jumps) so the
//      buy gate has something to evaluate
//   2. Occasionally fires the BG (price below threshold) so we measure the
//      cost of the rare-branch path too, not just the steady-state no-fire
//   3. Doesn't drift unboundedly during long runs (the BG/SG thresholds are
//      fixed, so the price needs to stay in a ~5% band around its starting point)
//   4. Is pre-generated into a flat array so the generation cost is OUTSIDE
//      the timed region — we measure the gate evaluation, not the prng
//
// We use xorshift64* for the random walk because it's branch-free, fast, and
// reproducible across runs (same seed → same stream).
//
// Volume is constant for now. The bench doesn't exercise volume-dependent
// signals because none of the four scenarios use them (the BG stub uses a
// price-only condition, the production BG path uses rolling stats which we'd
// have to feed separately).

#pragma once

#include "../../../CoreFrameworks/Tick.hpp"
#include "../../../FixedPoint/FixedPointN.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace experiment {

// xorshift64* — fast, branchless, deterministic prng. Same seed → same output.
static inline uint64_t xorshift64_next(uint64_t* state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

// Map a uint64 to a double in [-1.0, +1.0). Used for the random walk step.
static inline double xorshift_to_unit(uint64_t r) {
    // Top 53 bits → double in [0, 1), then center around 0.
    double u = (double)(r >> 11) * (1.0 / (double)(1ULL << 53));
    return (u * 2.0) - 1.0;
}

// Generate a vector of ticks with a random walk around start_price.
//   - count       : number of ticks to generate
//   - start_price : center price of the walk (e.g. 60000.0)
//   - step_pct    : max single-tick move as fraction of start_price (e.g. 0.0005 = 0.05%)
//   - drift_pull  : 0..1, how strongly the walk is pulled back toward start_price
//                   each tick. 0.0 = pure random walk, 1.0 = always reset to mean.
//                   Use ~0.02 to keep the price in a tight band over long runs.
//   - seed        : prng seed for reproducibility
//
// Tick volumes are fixed at 1000.0 since none of the bench scenarios use them.
// Timestamps are monotonic at 1µs intervals from t=1'000'000.
template <unsigned F>
static std::vector<tt::Tick<F>> generate_random_walk(
    size_t count,
    double start_price,
    double step_pct,
    double drift_pull,
    uint64_t seed
) {
    std::vector<tt::Tick<F>> ticks;
    ticks.reserve(count);

    uint64_t state = seed ? seed : 0xC0FFEE12345ULL;
    double price = start_price;
    double max_step = start_price * step_pct;

    for (size_t i = 0; i < count; ++i) {
        // Random walk step: move ±max_step, then mean-revert toward start_price.
        double r = xorshift_to_unit(xorshift64_next(&state));
        double drift = (start_price - price) * drift_pull;
        price += (r * max_step) + drift;

        tt::Tick<F> t;
        t.price     = FPN_FromDouble<F>(price);
        t.volume    = FPN_FromDouble<F>(1000.0);
        t.timestamp = 1'000'000ULL + (uint64_t)i;
        t.sequence  = (uint64_t)i;
        ticks.push_back(t);
    }

    return ticks;
}

// Quick stats on the generated stream — useful for sanity-checking that the
// walk doesn't drift out of range during a long bench run.
template <unsigned F>
static inline void describe_tick_stream(const std::vector<tt::Tick<F>>& ticks) {
    if (ticks.empty()) {
        printf("(empty tick stream)\n");
        return;
    }
    double min_p = 1e18, max_p = -1e18, sum_p = 0;
    for (const auto& t : ticks) {
        double p = FPN_ToDouble(t.price);
        if (p < min_p) min_p = p;
        if (p > max_p) max_p = p;
        sum_p += p;
    }
    double avg_p = sum_p / (double)ticks.size();
    printf("tick stream: count=%zu  min=%.2f  avg=%.2f  max=%.2f  range=%.2f (%.2f%%)\n",
           ticks.size(), min_p, avg_p, max_p, max_p - min_p,
           100.0 * (max_p - min_p) / avg_p);
}

}  // namespace experiment
