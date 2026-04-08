// test_spsc_ring.cpp — smoke test for the SPSCRing template
//
// What this validates:
//   1. Header compiles cleanly with -O3 -Wall -Wextra
//   2. static_asserts pass for power-of-2 sizes and trivially-copyable T
//   3. Init produces an empty ring (depth == 0)
//   4. Single push + single pop returns the same value (FIFO N=1 case)
//   5. Push N items, pop N items, FIFO order preserved
//   6. Push beyond capacity returns false
//   7. Pop from empty returns false
//   8. Wrap-around: push 4N items in batches of N, verify all returned correctly
//   9. Latency: tight push+pop loop, measure cycles per operation
//
// What it does NOT validate (deferred to phase 12 test suite):
//   - Multi-threaded producer/consumer correctness
//   - Stress testing under contention
//   - ThreadSanitizer clean
//   - Memory ordering correctness under concurrent access

#include "CoreFrameworks/SPSCRing.hpp"
#include "common/rdtsc.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace tt;
using namespace experiment;

// Simple POD payload for testing
struct TestPayload {
    uint64_t seq;
    uint64_t value;
    uint64_t timestamp;
    uint64_t _pad;  // pad to 32 bytes
};

static_assert(std::is_trivially_copyable<TestPayload>::value, "");
static_assert(sizeof(TestPayload) == 32, "");

static int failures = 0;

#define EXPECT(cond, msg)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);     \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static void test_init_empty() {
    SPSCRing<TestPayload, 8> ring;
    SPSCRing_Init(&ring);
    EXPECT(SPSCRing_Depth(&ring) == 0, "fresh ring depth should be 0");
    EXPECT(SPSCRing_Capacity(&ring) == 8, "capacity should equal template N");

    TestPayload out;
    EXPECT(!SPSCRing_TryPop(&ring, &out), "pop from empty should return false");
}

static void test_push_pop_one() {
    SPSCRing<TestPayload, 8> ring;
    SPSCRing_Init(&ring);

    TestPayload in = {1, 100, 200, 0};
    EXPECT(SPSCRing_TryPush(&ring, in), "push to empty should succeed");
    EXPECT(SPSCRing_Depth(&ring) == 1, "depth after one push should be 1");

    TestPayload out = {};
    EXPECT(SPSCRing_TryPop(&ring, &out), "pop should succeed");
    EXPECT(out.seq == 1, "seq should round-trip");
    EXPECT(out.value == 100, "value should round-trip");
    EXPECT(out.timestamp == 200, "timestamp should round-trip");
    EXPECT(SPSCRing_Depth(&ring) == 0, "depth after pop should be 0");
}

static void test_fifo_order() {
    constexpr size_t N = 8;
    SPSCRing<TestPayload, N> ring;
    SPSCRing_Init(&ring);

    // Push N items
    for (size_t i = 0; i < N; ++i) {
        TestPayload in = {i, i * 10, i * 100, 0};
        EXPECT(SPSCRing_TryPush(&ring, in), "push within capacity should succeed");
    }
    EXPECT(SPSCRing_Depth(&ring) == N, "depth after N pushes should be N");

    // One more push should fail
    TestPayload extra = {99, 99, 99, 0};
    EXPECT(!SPSCRing_TryPush(&ring, extra), "push beyond capacity should fail");

    // Pop N items in order
    for (size_t i = 0; i < N; ++i) {
        TestPayload out = {};
        EXPECT(SPSCRing_TryPop(&ring, &out), "pop within depth should succeed");
        EXPECT(out.seq == i, "FIFO seq order");
        EXPECT(out.value == i * 10, "FIFO value order");
    }

    // Ring should be empty
    EXPECT(SPSCRing_Depth(&ring) == 0, "depth after draining should be 0");
    TestPayload out;
    EXPECT(!SPSCRing_TryPop(&ring, &out), "pop from drained should fail");
}

static void test_wraparound() {
    constexpr size_t N = 16;
    SPSCRing<TestPayload, N> ring;
    SPSCRing_Init(&ring);

    // Push 4N items in batches of N, draining between each batch
    constexpr size_t TOTAL = 4 * N;
    uint64_t expected_seq = 0;
    for (size_t batch = 0; batch < 4; ++batch) {
        for (size_t i = 0; i < N; ++i) {
            TestPayload in = {expected_seq, expected_seq * 7, 0, 0};
            EXPECT(SPSCRing_TryPush(&ring, in), "wraparound push");
            ++expected_seq;
        }
        for (size_t i = 0; i < N; ++i) {
            TestPayload out = {};
            EXPECT(SPSCRing_TryPop(&ring, &out), "wraparound pop");
            uint64_t expected = expected_seq - N + i;
            EXPECT(out.seq == expected, "wraparound seq order");
            EXPECT(out.value == expected * 7, "wraparound value order");
        }
    }

    EXPECT(expected_seq == TOTAL, "all items processed");
}

static void test_interleaved_push_pop() {
    constexpr size_t N = 16;
    SPSCRing<TestPayload, N> ring;
    SPSCRing_Init(&ring);

    // Push 3, pop 1, push 3, pop 1, ... — keeps the ring partially full and
    // exercises the cached counter refresh paths.
    uint64_t pushed = 0;
    uint64_t popped = 0;
    for (size_t round = 0; round < 100; ++round) {
        for (int i = 0; i < 3; ++i) {
            TestPayload in = {pushed, pushed * 13, 0, 0};
            if (SPSCRing_TryPush(&ring, in)) {
                ++pushed;
            }
        }
        TestPayload out = {};
        if (SPSCRing_TryPop(&ring, &out)) {
            EXPECT(out.seq == popped, "interleaved seq order");
            ++popped;
        }
    }
    // Drain remaining
    TestPayload out;
    while (SPSCRing_TryPop(&ring, &out)) {
        EXPECT(out.seq == popped, "drain seq order");
        ++popped;
    }
    EXPECT(pushed == popped, "all pushed items popped");
}

static void measure_latency(uint64_t tsc_hz) {
    constexpr size_t N = 1024;
    SPSCRing<TestPayload, N> ring;
    SPSCRing_Init(&ring);

    constexpr int ITERS = 100'000;
    TestPayload in = {1, 2, 3, 0};
    TestPayload out;

    uint64_t push_total = 0;
    uint64_t pop_total = 0;
    uint64_t push_min = UINT64_MAX;
    uint64_t pop_min = UINT64_MAX;

    for (int i = 0; i < ITERS; ++i) {
        in.seq = (uint64_t)i;
        uint64_t s1 = rdtsc_start();
        SPSCRing_TryPush(&ring, in);
        uint64_t s2 = rdtsc_end();
        SPSCRing_TryPop(&ring, &out);
        uint64_t s3 = rdtsc_end();

        uint64_t push_cycles = s2 - s1;
        uint64_t pop_cycles = s3 - s2;
        push_total += push_cycles;
        pop_total += pop_cycles;
        if (push_cycles < push_min) push_min = push_cycles;
        if (pop_cycles < pop_min) pop_min = pop_cycles;
    }

    double push_avg = (double)push_total / ITERS;
    double pop_avg = (double)pop_total / ITERS;
    printf("\n--- single-threaded latency (ring capacity %zu) ---\n", N);
    printf("TryPush: min %lu cycles (%.1f ns), avg %.1f cycles (%.1f ns)\n",
           push_min, cycles_to_ns(push_min, tsc_hz),
           push_avg, cycles_to_ns((uint64_t)push_avg, tsc_hz));
    printf("TryPop:  min %lu cycles (%.1f ns), avg %.1f cycles (%.1f ns)\n",
           pop_min, cycles_to_ns(pop_min, tsc_hz),
           pop_avg, cycles_to_ns((uint64_t)pop_avg, tsc_hz));
    printf("Note: same-thread measurements include the ~25ns rdtsc overhead floor.\n");
}

int main() {
    printf("=== SPSCRing smoke test ===\n\n");

    uint64_t tsc_hz = calibrate_tsc_hz();
    if (tsc_hz == 0) {
        fprintf(stderr, "TSC calibration failed\n");
        return 1;
    }
    printf("TSC frequency: %.4f GHz\n", tsc_hz / 1e9);

    // Layout sanity checks (compile-time assertions inside the template, plus runtime offsets)
    SPSCRing<TestPayload, 1024> ring;
    SPSCRing_Init(&ring);
    uintptr_t head_addr = (uintptr_t)&ring.head;
    uintptr_t tail_addr = (uintptr_t)&ring.tail;
    printf("head offset: %lu, tail offset: %lu, separation: %lu bytes\n",
           head_addr - (uintptr_t)&ring,
           tail_addr - (uintptr_t)&ring,
           tail_addr - head_addr);
    EXPECT(tail_addr - head_addr >= 64, "head and tail must be on separate cache lines");
    EXPECT((head_addr & 63) == 0, "head must be cache-line aligned");
    EXPECT((tail_addr & 63) == 0, "tail must be cache-line aligned");

    printf("\n--- functional tests ---\n");
    test_init_empty();
    printf("init_empty           ok\n");
    test_push_pop_one();
    printf("push_pop_one         ok\n");
    test_fifo_order();
    printf("fifo_order           ok\n");
    test_wraparound();
    printf("wraparound           ok\n");
    test_interleaved_push_pop();
    printf("interleaved_push_pop ok\n");

    measure_latency(tsc_hz);

    printf("\n=== %s (%d failures) ===\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
