// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [SPSC RING]
//
// Single-producer single-consumer lock-free ring buffer for inter-core communication.
//
// Used by the per-core sharded engine for tick fan-out (market reader → execution
// cores) and trade event delivery (execution cores → controller). One producer
// thread, one consumer thread per ring instance — no MPMC support, no MPSC, no SPMC.
//
// Why this design:
//   1. Lock-free: producer and consumer never block each other on coordination,
//      only on the ring being full / empty. No mutexes, no syscalls, no kernel
//      crossings on the hot path.
//
//   2. Cache-line separated head and tail: producer writes head, consumer writes
//      tail. If they shared a cache line every push and pop would invalidate the
//      other side's L1 line via cache coherence traffic, costing ~80ns per op.
//      We force them onto independent 64-byte cache lines via alignas(CACHE_LINE).
//
//   3. Cached counters: the producer keeps a local copy of the consumer's tail,
//      only refreshed when the ring appears full from the producer's local view.
//      This means the common case (ring not full) doesn't load tail from the
//      consumer's cache line at ALL. Same trick on the consumer side for head.
//      This is the key optimization that makes SPSC fast — without it the design
//      would be no better than MPMC.
//
//   4. Power-of-2 size: enables the bitmask trick `index & (N-1)` instead of
//      modulo, which is a few cycles faster and predictable.
//
//   5. Trivially copyable T: slot writes are memcpy-equivalent, no constructors,
//      no destructors, no exceptions on the hot path. Enforced by static_assert.
//
// Memory ordering:
//   - Producer push: write the slot data first, then store head with release
//     ordering. The release forces the slot write to be globally visible BEFORE
//     any consumer can observe the head update via an acquire load.
//   - Consumer pop: load head with acquire ordering first, check it, then read
//     the slot. The acquire ensures all writes that happened before the matching
//     release are visible to subsequent reads on this thread.
//   - This is the standard release/acquire pair for SPSC. No fences needed,
//     no atomic operations needed beyond the head/tail loads/stores.
//
// Backpressure policy: NOT enforced by the ring. TryPush returns false on full,
// TryPop returns false on empty. Caller decides what to do — drop, spin, retry,
// crash, etc. Different rings in the engine have different policies:
//   - Tick rings (market → execution cores): drop policy. Losing one tick is
//     acceptable, latency matters more than completeness.
//   - Event rings (execution cores → controller): spin policy. We cannot lose
//     trade events. Sized large enough that brief controller stalls don't cause
//     execution-core blocking.
//======================================================================================================

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace tt {

template <typename T, size_t N>
struct SPSCRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static_assert(N >= 2, "N must be at least 2");
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");

    static constexpr size_t MASK = N - 1;
    static constexpr size_t CACHE_LINE = 64;

    // Producer side: writes head, reads cached tail on the fast path.
    // alignas(CACHE_LINE) forces the next field to start at a cache line boundary,
    // preventing false sharing with whatever lives before the struct in memory.
    alignas(CACHE_LINE) std::atomic<uint64_t> head;
    uint64_t cached_tail;

    // Consumer side: writes tail, reads cached head on the fast path.
    // alignas(CACHE_LINE) forces tail onto its own cache line, separate from head.
    // This is the load-bearing line in the whole header — without it, every push
    // and pop would bounce a cache line between producer and consumer cores.
    alignas(CACHE_LINE) std::atomic<uint64_t> tail;
    uint64_t cached_head;

    // Storage. Aligned to a cache line so the array starts cleanly. Individual
    // slots use natural alignment for density.
    alignas(CACHE_LINE) T slots[N];
};

// Initialize the ring. Call once before any use, on either thread.
template <typename T, size_t N>
static inline void SPSCRing_Init(SPSCRing<T, N>* r) {
    r->head.store(0, std::memory_order_relaxed);
    r->tail.store(0, std::memory_order_relaxed);
    r->cached_tail = 0;
    r->cached_head = 0;
}

// Producer: try to push one item. Returns true on success, false if ring is full.
//
// Cost in the fast path (cached_tail says we have space): one atomic relaxed load,
// one comparison, one slot write, one atomic release store. ~3-5ns on modern x86.
//
// Cost in the slow path (cached_tail is stale, ring might be full): adds one
// cross-core load of tail with acquire ordering. ~20-50ns depending on whether
// the consumer's cache line is contested. Only happens when the ring approaches
// full, so amortized cost is small under normal load.
template <typename T, size_t N>
__attribute__((always_inline))
static inline bool SPSCRing_TryPush(SPSCRing<T, N>* r, const T& item) {
    uint64_t head = r->head.load(std::memory_order_relaxed);
    uint64_t next_head = head + 1;

    // Fast path: cached_tail says we have space.
    if (next_head - r->cached_tail > N) {
        // Cached value is stale. Refresh from the consumer side.
        r->cached_tail = r->tail.load(std::memory_order_acquire);
        if (next_head - r->cached_tail > N) {
            return false;  // genuinely full
        }
    }

    // Write the slot first, then publish via head store with release ordering.
    // The release ensures the slot write is visible before the head update is.
    r->slots[head & SPSCRing<T, N>::MASK] = item;
    r->head.store(next_head, std::memory_order_release);
    return true;
}

// Consumer: try to pop one item into *out. Returns true on success, false if ring is empty.
//
// Cost in the fast path: one atomic relaxed load, one comparison, one slot read,
// one atomic release store. ~3-5ns on modern x86.
//
// Cost in the slow path (cached_head is stale, ring might be empty): adds one
// cross-core load of head with acquire ordering.
template <typename T, size_t N>
__attribute__((always_inline))
static inline bool SPSCRing_TryPop(SPSCRing<T, N>* r, T* out) {
    uint64_t tail = r->tail.load(std::memory_order_relaxed);

    // Fast path: cached_head says we have data.
    if (tail >= r->cached_head) {
        r->cached_head = r->head.load(std::memory_order_acquire);
        if (tail >= r->cached_head) {
            return false;  // genuinely empty
        }
    }

    // Read the slot AFTER the acquire load of head. The acquire ordering ensures
    // we see the slot data the producer wrote before its release store.
    *out = r->slots[tail & SPSCRing<T, N>::MASK];
    r->tail.store(tail + 1, std::memory_order_release);
    return true;
}

// Informational: current depth (entries waiting for consumer). Not safe to act
// on for synchronization decisions — by the time you read this it may already
// be stale. Only use for monitoring / logging / TUI display.
template <typename T, size_t N>
static inline size_t SPSCRing_Depth(const SPSCRing<T, N>* r) {
    uint64_t head = r->head.load(std::memory_order_acquire);
    uint64_t tail = r->tail.load(std::memory_order_acquire);
    return (size_t)(head - tail);
}

// Informational: total capacity. Constant per instance, useful for monitoring
// "depth / capacity" utilization ratios.
template <typename T, size_t N>
static inline constexpr size_t SPSCRing_Capacity(const SPSCRing<T, N>*) {
    return N;
}

}  // namespace tt
