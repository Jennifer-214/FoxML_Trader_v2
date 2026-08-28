// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/SPSCRing.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CONCURRENCY] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[single-producer single-consumer lock-free ring — the engine's inter-thread transport primitive]
// [CONTAINS]
//   - [STRUCT]_[SPSCRing]
//   - [FUNCTION]_[SPSCRing_Init]
//   - [FUNCTION]_[SPSCRing_TryPush]
//   - [FUNCTION]_[SPSCRing_TryPop]
//   - [FUNCTION]_[SPSCRing_Depth]
//======================================================================================================

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace tt {

//======================================================================
// [STRUCT]_[SPSCRing]
//----------------------------------------------------------------------
// [TAG]_[[CONCURRENCY] [DATA_ORIENTED_DESIGN] [CRITICAL]]
// [THREAD]_[[PRODUCER_WRITER] [CONSUMER_WRITER]]
// [SYNC]_[SPSC]
// [SYNC]_[LOCK_FREE]
// [REFERENCE]_[INVARIANT]_[[H3] [H6]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[lock-free SPSC ring — producer/consumer counters on separate cache lines; power-of-2 slot array]
// [REGION]_[producer line — head + cached_tail]_[0..63]
// [THREAD]_[[PRODUCER_WRITER] [CONSUMER_READER]]
// [REGION]_[consumer line — tail + cached_head]_[64..127]
// [THREAD]_[[CONSUMER_WRITER] [PRODUCER_READER]]
// [REGION]_[slots array]_[128..]
// [THREAD]_[[PRODUCER_WRITER] [CONSUMER_READER]]
// [REFERENCE]_[DECISION]_[D-318]
//======================================================================
// [CODE]
//======================================================================
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
    //
    // Counters SELF-INITIALIZE ({0}) so every allocation shape — static/BSS, stack
    // local, arena — constructs an EMPTY ring. Found the hard way (E.1.3 P2-e): the
    // backtest driver's EventLoopState is a plain stack local; these counters were
    // indeterminate, head != tail garbage made TryPop "succeed" for up to 2^64 pops,
    // and the first consumer wired onto such a ring (the composer's drag drain) spun
    // the v5.9.2 parity battery for 38 minutes. Slots stay raw deliberately — an
    // empty ring never reads them, and zero-filling N slots per construction is waste.
    alignas(CACHE_LINE) std::atomic<uint64_t> head{0};
    uint64_t cached_tail = 0;

    // Consumer side: writes tail, reads cached head on the fast path.
    // alignas(CACHE_LINE) forces tail onto its own cache line, separate from head.
    // This is the load-bearing line in the whole header — without it, every push
    // and pop would bounce a cache line between producer and consumer cores.
    alignas(CACHE_LINE) std::atomic<uint64_t> tail{0};
    uint64_t cached_head = 0;

    // Storage. Aligned to a cache line so the array starts cleanly. Individual
    // slots use natural alignment for density.
    alignas(CACHE_LINE) T slots[N];
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]_[role + why this design]
//----------------------------------------------------------------------
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
//======================================================================
// [COMMENT]_[memory ordering]
//----------------------------------------------------------------------
// - Producer push: write the slot data first, then store head with release
//   ordering. The release forces the slot write to be globally visible BEFORE
//   any consumer can observe the head update via an acquire load.
// - Consumer pop: load head with acquire ordering first, check it, then read
//   the slot. The acquire ensures all writes that happened before the matching
//   release are visible to subsequent reads on this thread.
// - This is the standard release/acquire pair for SPSC. No fences needed,
//   no atomic operations needed beyond the head/tail loads/stores.
//======================================================================
// [COMMENT]_[backpressure policy]
//----------------------------------------------------------------------
// Backpressure policy: NOT enforced by the ring. TryPush returns false on full,
// TryPop returns false on empty. Caller decides what to do — drop, spin, retry,
// crash, etc. Different rings in the engine have different policies:
//   - Tick rings (market → execution cores): drop policy. Losing one tick is
//     acceptable, latency matters more than completeness.
//   - Event rings (execution cores → controller): spin policy. We cannot lose
//     trade events. Sized large enough that brief controller stalls don't cause
//     execution-core blocking.
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; layout facts are PER-INSTANTIATION for this
//              generic template and live tool-owned at each consumer struct, D-318/D-327)
//======================================================================
// [END_STRUCT]_[SPSCRing]
//======================================================================

//======================================================================
// [FUNCTION]_[SPSCRing_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[initialize the ring — call once before any use, on either thread]
//======================================================================
// [CODE]
//======================================================================
template <typename T, size_t N>
static inline void SPSCRing_Init(SPSCRing<T, N>* r) {
    r->head.store(0, std::memory_order_relaxed);
    r->tail.store(0, std::memory_order_relaxed);
    r->cached_tail = 0;
    r->cached_head = 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SPSCRing_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[SPSCRing_TryPush]
//----------------------------------------------------------------------
// [TAG]_[[HOT_PATH] [CONCURRENCY]]
// [REFERENCE]_[INVARIANT]_[H20]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-160]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[producer push — true on success, false on full; cached-counter fast path skips the cross-core load]
//======================================================================
// [CODE]
//======================================================================
template <typename T, size_t N>
__attribute__((always_inline))
static inline bool SPSCRing_TryPush(SPSCRing<T, N>* r, const T& item) {
    uint64_t head = r->head.load(std::memory_order_relaxed);
    uint64_t next_head = head + 1;

    // Fast path: cached_tail says we have space. Hot in normal load — annotate
    // with __builtin_expect to keep the slow-path body out-of-line.
    if (__builtin_expect(next_head - r->cached_tail > N, 0)) {
        // Cached value is stale. Refresh from the consumer side.
        r->cached_tail = r->tail.load(std::memory_order_acquire);
        if (__builtin_expect(next_head - r->cached_tail > N, 0)) {
            return false;  // genuinely full
        }
    }

    // Write the slot first, then publish via head store with release ordering.
    // The release ensures the slot write is visible before the head update is.
    //
    // KNOWN-FP (TECH_DEBT-160, verified 2026-06-09): GCC's gui-lane build emits 8
    // -Wstringop-overflow "writing 1 byte into a region of size 0" instances for this write,
    // via IPA/.isra clones of the submit inline chain (BinanceAdapter_Submit* ->
    // OrderManager_Submit -> OMS_DrainSubmit). The write is in-bounds BY CONSTRUCTION:
    // MASK = N-1 with (N & (N-1)) == 0 static_assert'd at the struct => (head & MASK) < N,
    // always. "Region of SIZE 0" = the clone's base-pointer PROVENANCE is degenerate — an
    // index-side fact cannot fix it, and both remedies were tried + verified ineffective:
    // (1) #pragma GCC diagnostic ignored — IGNORED by the late IPA passes that emit this;
    // (2) __builtin_unreachable() range hint on the masked index — no effect (the glitch is
    // the object model, not the index range). Disposition: documented verified-FP; do NOT
    // chase these 8 lines again — but a stringop warning at ANY OTHER site is real signal.
    r->slots[head & SPSCRing<T, N>::MASK] = item;
    r->head.store(next_head, std::memory_order_release);
    return true;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Producer: try to push one item. Returns true on success, false if ring is full.
//
// Cost in the fast path (cached_tail says we have space): one atomic relaxed load,
// one comparison, one slot write, one atomic release store. ~3-5ns on modern x86.
//
// Cost in the slow path (cached_tail is stale, ring might be full): adds one
// cross-core load of tail with acquire ordering. ~20-50ns depending on whether
// the consumer's cache line is contested. Only happens when the ring approaches
// full, so amortized cost is small under normal load.
//
// v5.11.20 (2026-05-07): branch prediction hints. The "ring not full" fast
// path is overwhelmingly the common case — typical engine usage runs the ring
// at <50% utilization (we size for peak burst with 4-8x headroom). Pre-fix
// the compiler had no info about which branch was hot and emitted the canonical
// "fall through on condition true, jump on false" — which puts the slow path
// in-line with the fast path's i-cache footprint AND lets the predictor
// occasionally mispredict during ramp-up. Post-fix __builtin_expect(..., 0)
// on both the cache-stale check and the genuinely-full check tells GCC to
// emit jump-rare; compiled output puts the slow-path body OUT OF the fast
// path's i-cache line. Saves ~1-3ns of mispredict cost in steady state +
// keeps the fast path tight under i-cache pressure.
//
// NOTE: a "branchless mask blend" version (computing both fast + slow path
// then masking the result) was considered but REJECTED — it would force an
// unconditional cross-core load of r->tail, defeating the cached-counter
// optimization (#3 in the struct's design notes) and regressing the steady-
// state cost from ~3ns to ~20-30ns. Branch-prediction hints are the right
// answer for SPSC ring fast paths.
//======================================================================
// [END_FUNCTION]_[SPSCRing_TryPush]
//======================================================================

//======================================================================
// [FUNCTION]_[SPSCRing_TryPop]
//----------------------------------------------------------------------
// [TAG]_[[HOT_PATH] [CONCURRENCY]]
// [REFERENCE]_[INVARIANT]_[H20]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[consumer pop into *out — true on success, false on empty; same cached-counter fast path as push]
//======================================================================
// [CODE]
//======================================================================
template <typename T, size_t N>
__attribute__((always_inline))
static inline bool SPSCRing_TryPop(SPSCRing<T, N>* r, T* out) {
    uint64_t tail = r->tail.load(std::memory_order_relaxed);

    // Fast path: cached_head says we have data.
    if (__builtin_expect(tail >= r->cached_head, 0)) {
        r->cached_head = r->head.load(std::memory_order_acquire);
        if (__builtin_expect(tail >= r->cached_head, 0)) {
            return false;  // genuinely empty
        }
    }

    // Read the slot AFTER the acquire load of head. The acquire ordering ensures
    // we see the slot data the producer wrote before its release store.
    *out = r->slots[tail & SPSCRing<T, N>::MASK];
    r->tail.store(tail + 1, std::memory_order_release);
    return true;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Consumer: try to pop one item into *out. Returns true on success, false if ring is empty.
//
// Cost in the fast path: one atomic relaxed load, one comparison, one slot read,
// one atomic release store. ~3-5ns on modern x86.
//
// Cost in the slow path (cached_head is stale, ring might be empty): adds one
// cross-core load of head with acquire ordering.
//
// v5.11.20: same branch-hint discipline as TryPush. The "ring has data" fast
// path is hot under steady-state engine load; the "genuinely empty" path
// fires only briefly between bursts. See TryPush comment block for the
// branchless-mask rejection rationale.
//======================================================================
// [END_FUNCTION]_[SPSCRing_TryPop]
//======================================================================

//======================================================================
// [FUNCTION]_[SPSCRing_Depth]
//----------------------------------------------------------------------
// [TAG]_[[MONITORING_PLANE] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[informational depth (entries waiting for consumer) — monitoring/TUI only, never a sync decision]
//======================================================================
// [CODE]
//======================================================================
template <typename T, size_t N>
static inline size_t SPSCRing_Depth(const SPSCRing<T, N>* r) {
    uint64_t head = r->head.load(std::memory_order_acquire);
    uint64_t tail = r->tail.load(std::memory_order_acquire);
    return (size_t)(head - tail);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Informational: current depth (entries waiting for consumer). Not safe to act
// on for synchronization decisions — by the time you read this it may already
// be stale. Only use for monitoring / logging / TUI display.
//======================================================================
// [END_FUNCTION]_[SPSCRing_Depth]
//======================================================================

// Informational: total capacity. Constant per instance, useful for monitoring
// "depth / capacity" utilization ratios.
template <typename T, size_t N>
static inline constexpr size_t SPSCRing_Capacity(const SPSCRing<T, N>*) {
    return N;
}

}  // namespace tt
