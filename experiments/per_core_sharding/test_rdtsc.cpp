// test_rdtsc.cpp — smoke test for the TSC-based measurement foundation
//
// What this validates:
//   1. TSC frequency calibration produces a sensible number (1-4 GHz range)
//   2. rdtsc_start / rdtsc_end with proper fences gives stable measurements
//   3. The cost of an empty measurement window is small and consistent (this
//      is the floor for anything we measure — anything at this floor is "the
//      cost of measuring nothing")
//   4. A trivial loop body produces a stable measurement above the empty floor
//
// What "stable" means: percentiles cluster (p50, p95, p99 within ~2x of each
// other) and the minimum is reproducible across runs. If we see wild outliers
// or the floor varies by 10x between runs, the measurement is not trustworthy
// and we need to investigate (kernel preemption, thermal throttling, frequency
// scaling) before building the real harness on top.

#include "common/rdtsc.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace experiment;

// Sample N times, return sorted vector for percentile reporting.
template <typename Fn>
static std::vector<uint64_t> sample(int n, Fn&& f) {
    std::vector<uint64_t> samples;
    samples.reserve(n);
    for (int i = 0; i < n; ++i) {
        uint64_t start = rdtsc_start();
        f(i);
        uint64_t end = rdtsc_end();
        samples.push_back(end - start);
    }
    std::sort(samples.begin(), samples.end());
    return samples;
}

static void report(const char* name, const std::vector<uint64_t>& samples, uint64_t tsc_hz) {
    if (samples.empty()) return;
    auto pct = [&](double p) -> uint64_t {
        size_t idx = (size_t)((double)samples.size() * p);
        if (idx >= samples.size()) idx = samples.size() - 1;
        return samples[idx];
    };
    uint64_t min  = samples.front();
    uint64_t p50  = pct(0.50);
    uint64_t p95  = pct(0.95);
    uint64_t p99  = pct(0.99);
    uint64_t p999 = pct(0.999);
    uint64_t max  = samples.back();

    printf("%-30s  min=%4lu  p50=%4lu  p95=%4lu  p99=%4lu  p99.9=%5lu  max=%6lu cycles\n",
           name, min, p50, p95, p99, p999, max);
    printf("%-30s  min=%5.1f p50=%5.1f p95=%5.1f p99=%5.1f p99.9=%6.1f max=%7.1f ns\n",
           "  ↳ ns",
           cycles_to_ns(min, tsc_hz),
           cycles_to_ns(p50, tsc_hz),
           cycles_to_ns(p95, tsc_hz),
           cycles_to_ns(p99, tsc_hz),
           cycles_to_ns(p999, tsc_hz),
           cycles_to_ns(max, tsc_hz));
}

int main() {
    printf("=== rdtsc smoke test ===\n\n");

    // Calibrate TSC. Single calibration is fine for smoke testing; the real
    // harness will average a few calibrations and report the variance.
    uint64_t tsc_hz = calibrate_tsc_hz(100'000'000ULL); // 100ms
    if (tsc_hz == 0) {
        printf("FAIL: TSC calibration returned 0. Either nanosleep failed or\n");
        printf("      the TSC is not advancing. This CPU may not support rdtsc-\n");
        printf("      based timing; switch to clock_gettime(CLOCK_MONOTONIC_RAW).\n");
        return 1;
    }
    double tsc_ghz = (double)tsc_hz / 1e9;
    printf("TSC frequency: %.4f GHz  (%.4f ns/tick)\n", tsc_ghz, 1.0 / tsc_ghz);

    if (tsc_ghz < 0.5 || tsc_ghz > 5.0) {
        printf("WARNING: TSC frequency outside expected 0.5-5.0 GHz range.\n");
        printf("         Continuing, but the conversion to nanoseconds may be wrong.\n");
    }
    printf("\n");

    constexpr int N = 100'000;
    volatile uint64_t sink = 0; // prevents the optimizer from removing our work

    // Test 1: empty measurement window. This is the floor cost of rdtsc itself
    // (the fences + the read). Every other measurement includes this overhead,
    // so we want it small and stable.
    auto empty = sample(N, [&](int) { /* nothing */ });
    report("empty window (overhead floor)", empty, tsc_hz);
    printf("\n");

    // Test 2: trivial loop body — single integer add. Should be a few cycles
    // above the empty floor. If this is way more than empty + 1-2 cycles,
    // either the compiler isn't reordering away the volatile or our fences
    // are leaking.
    auto trivial = sample(N, [&](int i) { sink += (uint64_t)i; });
    report("trivial add (sink += i)", trivial, tsc_hz);
    printf("\n");

    // Test 3: a few dependent integer ops. This is what a tight inner loop
    // looks like — should still be small (under 20 cycles).
    auto deps = sample(N, [&](int i) {
        uint64_t x = (uint64_t)i;
        x ^= x << 7;
        x += 0x9e3779b97f4a7c15ULL;
        x *= 0xbf58476d1ce4e5b9ULL;
        sink += x;
    });
    report("4 dependent int ops", deps, tsc_hz);
    printf("\n");

    // Sanity check: prevent the compiler from concluding sink is dead.
    if (sink == 0xdeadbeef) {
        printf("(unreachable, just keeping sink alive: %lu)\n", sink);
    }

    printf("=== smoke test done ===\n");
    printf("\nIf the empty floor is ~20-40 cycles and the trivial add is\n");
    printf("within 5-10 cycles of that, the measurement infrastructure is\n");
    printf("calibrated. Next step: BG-shape anchor test against the known\n");
    printf("40ns BuyGate floor from the production engine.\n");

    return 0;
}
