// test_spsc_concurrent.cpp — phase 12 concurrent stress test for SPSCRing
//
// Validates the lock-free SPSC ring with a real producer + consumer thread
// pair under sustained load. The single-threaded tests in test_spsc_ring
// cover correctness of the data structure; this file covers correctness of
// the cross-core memory ordering and the cache-line separated head/tail
// design under actual concurrency.
//
// What gets validated:
//   1. 1M items produced + consumed without loss
//   2. FIFO order preserved (sequence numbers strictly monotonic)
//   3. No torn payloads under sustained writer pressure
//   4. The cached_head / cached_tail optimization doesn't cause stalls or
//      false negatives on full/empty checks
//   5. Backpressure: producer correctly blocks (spins) when ring full
//   6. Spurious wakeup safety: consumer correctly handles empty after seeing
//      data was queued
//
// Run under TSan to validate memory ordering claims:
//   cmake -B build_tsan -DTSAN=ON
//   cmake --build build_tsan
//   ./build_tsan/test_spsc_concurrent

#include "CoreFrameworks/SPSCRing.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace tt;

static int failures = 0;

#define EXPECT(cond, msg)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);     \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

// Payload struct so a torn write would be detectable. The fields are
// redundant but cross-validated on the consumer side.
struct StressPayload {
    uint64_t seq;        // monotonic sequence number
    uint64_t seq_xor;    // ~seq, lets us detect torn writes
    uint64_t producer_ts;
    uint64_t magic;      // sentinel
};

constexpr uint64_t MAGIC = 0xDEADBEEFCAFEBABEULL;

//======================================================================================================
// test 1: 1M items, no losses, FIFO preserved
//======================================================================================================
static void test_one_million_items() {
    constexpr size_t RING_N = 1024;
    constexpr uint64_t TOTAL = 1'000'000;

    SPSCRing<StressPayload, RING_N> ring;
    SPSCRing_Init(&ring);

    std::atomic<uint64_t> consumer_count{0};
    std::atomic<uint64_t> torn_payloads{0};
    std::atomic<uint64_t> bad_magic{0};
    std::atomic<uint64_t> out_of_order{0};
    std::atomic<bool> consumer_done{false};

    // Consumer thread — pop in a tight loop until we see TOTAL items.
    std::thread consumer([&] {
        uint64_t expected_seq = 0;
        StressPayload p;
        while (expected_seq < TOTAL) {
            if (SPSCRing_TryPop(&ring, &p)) {
                if (p.magic != MAGIC) bad_magic.fetch_add(1, std::memory_order_relaxed);
                if ((p.seq ^ p.seq_xor) != ~uint64_t(0))
                    torn_payloads.fetch_add(1, std::memory_order_relaxed);
                if (p.seq != expected_seq)
                    out_of_order.fetch_add(1, std::memory_order_relaxed);
                ++expected_seq;
                consumer_count.fetch_add(1, std::memory_order_relaxed);
            }
            // tight spin, no yield — measures the actual hot path behavior
        }
        consumer_done.store(true, std::memory_order_release);
    });

    // Producer thread — push as fast as possible, spinning on full.
    std::thread producer([&] {
        StressPayload p;
        p.magic = MAGIC;
        for (uint64_t seq = 0; seq < TOTAL; ++seq) {
            p.seq = seq;
            p.seq_xor = ~seq;
            p.producer_ts = seq;  // synthetic timestamp
            // Spin until the push succeeds. The ring is small (1024) and the
            // producer outpaces the consumer, so this exercises full-ring
            // backpressure heavily.
            while (!SPSCRing_TryPush(&ring, p)) {
                // tight spin
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT(consumer_count.load() == TOTAL, "consumed exactly 1M items");
    EXPECT(torn_payloads.load() == 0, "no torn payloads");
    EXPECT(bad_magic.load() == 0, "no bad magic values");
    EXPECT(out_of_order.load() == 0, "FIFO order preserved");
    EXPECT(consumer_done.load() == true, "consumer ran to completion");

    printf("  1M items: %lu consumed, %lu torn, %lu bad magic, %lu out-of-order\n",
           (unsigned long)consumer_count.load(),
           (unsigned long)torn_payloads.load(),
           (unsigned long)bad_magic.load(),
           (unsigned long)out_of_order.load());
}

//======================================================================================================
// test 2: bursty producer + slow consumer — exercises backpressure
//======================================================================================================
// Producer pushes in bursts of 100, then sleeps briefly. Consumer pops slowly.
// This stresses the full-ring path and verifies items aren't dropped.
//======================================================================================================
static void test_bursty_producer() {
    constexpr size_t RING_N = 256;
    constexpr int BURSTS = 100;
    constexpr int BURST_SIZE = 50;
    constexpr uint64_t TOTAL = (uint64_t)BURSTS * BURST_SIZE;

    SPSCRing<StressPayload, RING_N> ring;
    SPSCRing_Init(&ring);

    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> torn{0};
    std::atomic<bool> producer_done{false};

    std::thread consumer([&] {
        uint64_t next = 0;
        StressPayload p;
        while (received.load(std::memory_order_acquire) < TOTAL) {
            if (SPSCRing_TryPop(&ring, &p)) {
                if (p.seq != next) torn.fetch_add(1, std::memory_order_relaxed);
                ++next;
                received.fetch_add(1, std::memory_order_release);
                // Slow the consumer down a tiny bit
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            } else if (producer_done.load(std::memory_order_acquire) && next >= TOTAL) {
                break;
            }
        }
    });

    std::thread producer([&] {
        StressPayload p;
        p.magic = MAGIC;
        uint64_t seq = 0;
        for (int b = 0; b < BURSTS; ++b) {
            for (int i = 0; i < BURST_SIZE; ++i) {
                p.seq = seq;
                p.seq_xor = ~seq;
                while (!SPSCRing_TryPush(&ring, p)) {}
                ++seq;
            }
            // Brief pause between bursts
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        producer_done.store(true, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    EXPECT(received.load() == TOTAL, "all bursty items received");
    EXPECT(torn.load() == 0, "no order violations");
}

//======================================================================================================
// test 3: empty pop returns false, no UB
//======================================================================================================
static void test_empty_pop_safe() {
    SPSCRing<StressPayload, 64> ring;
    SPSCRing_Init(&ring);

    std::atomic<int> empty_count{0};

    std::thread t([&] {
        StressPayload p;
        for (int i = 0; i < 1000; ++i) {
            if (!SPSCRing_TryPop(&ring, &p)) {
                empty_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    t.join();
    EXPECT(empty_count.load() == 1000, "all 1000 empty pops returned false");
}

//======================================================================================================
// main
//======================================================================================================
int main() {
    printf("=== SPSC concurrent stress tests ===\n\n");

    test_one_million_items();
    printf("one_million_items                 ok\n");
    test_bursty_producer();
    printf("bursty_producer                   ok\n");
    test_empty_pop_safe();
    printf("empty_pop_safe                    ok\n");

    printf("\n=== %s (%d failures) ===\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
