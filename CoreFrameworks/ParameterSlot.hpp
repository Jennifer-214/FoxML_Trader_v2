// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [PARAMETER SLOT — SEQLOCK ATOMIC HANDOFF]
//======================================================================================================
//
// Wait-free producer, lock-free consumer, parameter handoff for the per-core
// sharded engine. The controller core writes parameter packs (gate thresholds,
// trade sizing, strategy state) and execution cores read them on every tick.
// The read MUST not return a torn buffer; the protocol MUST tolerate any
// producer rate without losing safety.
//
// Why seqlock and NOT triple buffering:
//
//   The original phase 05 plan recommended triple buffering. Pitfall P5.1
//   identified the rapid-write race and recommended triple buffering as the
//   fix. But triple buffering only protects against ONE in-flight producer
//   write per consumer read. Three writes during one read can wrap the rotor
//   back to the buffer the consumer is reading. Phase 05's stress test
//   reproduces this consistently when the producer rate approaches the
//   consumer rate (8000+ torn reads out of 1M reads at 1:10 producer:consumer).
//
//   The seqlock pattern handles arbitrary producer rates: the consumer reads
//   the version counter before and after copying the buffer, retries if it
//   changed. Wait-free producer (no retries on the write side). Lock-free
//   consumer (the retry loop is bounded only by producer activity, but the
//   producer is rate-limited by the slow-path cadence in production).
//
//   Phase 05 stress test shows zero torn reads with the seqlock at any rate.
//
// Cost on the hot path:
//
//   The consumer must MEMCPY the buffer into caller-provided storage (the
//   buffer can be overwritten while the caller is using fields off it, so we
//   can't return a reference). For GateParameters<64> that's a ~1KB memcpy
//   which is ~20-30ns on this hardware. Phase 05 ExecutionCore_Tick latency
//   measured at ~63ns avg, up from ~43ns before the seqlock. Still well
//   inside the 100ns budget.
//
//   In the steady state with no concurrent producer write, the read does:
//     1. acquire-load of seq (~1ns)
//     2. odd-bit check (1 cycle)
//     3. memcpy of T (~20ns for 1KB)
//     4. acquire-load of seq again (~1ns)
//     5. equality check (1 cycle)
//   Total: ~25ns. The retry path is essentially never taken in production.
//
// Memory ordering:
//
//   Producer:
//     seq.store(cur+1, release)         // mark in-progress, makes seq odd
//     buffers[idx] = new_params         // memcpy, non-atomic (safe, no reader)
//     seq.store(cur+2, release)         // mark done, advance version
//
//   Consumer:
//     do {
//       s1 = seq.load(acquire)          // catch the START of any in-flight write
//       if (s1 odd) continue            // mid-write, retry
//       *out = buffers[(s1 >> 1) & 1]   // copy out
//       s2 = seq.load(acquire)          // catch a write that started during the copy
//     } while (s1 != s2)                // value changed → retry
//
//   The two release stores on the producer side are essential. The first one
//   "fences off" the buffer write so the consumer can't see seq incrementing
//   without also seeing the upcoming write. The second one is the publication
//   barrier — the buffer is fully written before seq becomes "even" again.
//
//   The two acquire loads on the consumer side bracket the buffer copy. If
//   the producer started a write between them, the second load sees a
//   different value and we retry.
//
// Restrictions on T:
//
//   - must be trivially copyable (memcpy semantics)
//   - sizeof(T) is the per-tick memcpy cost; keep it small. GateParameters<64>
//     at ~1KB is the upper bound for what fits the budget.
//
// Usage:
//
//   ParameterSlot<GateParameters<64>> slot;
//   GateParameters<64> initial; GateParameters_Init(&initial);
//   ParameterSlot_Init(&slot, initial);
//
//   // producer (controller core, slow path):
//   GateParameters<64> new_params = ComputeFromStrategy();
//   ParameterSlot_Write(&slot, new_params);
//
//   // consumer (execution core, hot path):
//   GateParameters<64> params;
//   ParameterSlot_Read(&slot, &params);
//   bool fire = BG_Evaluate(tick, &params);
//
//======================================================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace tt {

//======================================================================================================
// [PARAMETER SLOT STRUCT]
//======================================================================================================
// two buffers + a version counter. seq encodes both the version (high bits)
// and the active buffer index (bit 1) and the in-progress flag (bit 0).
//
// seq layout:
//   bit 0:  0 = stable (write complete), 1 = in-progress (mid-write)
//   bit 1:  active buffer index (0 or 1) when stable
//   bits 2+: monotonic version, used by the consumer's retry check
//
// the producer increments seq by 1 to start a write (parity flips to odd) and
// by 1 again to commit (parity flips back to even, version advances by 2 per
// write). the active buffer alternates: 0, 1, 0, 1, ...
//
// alignas(64) keeps each slot on its own cache line. arrays of slots don't
// share lines because of the explicit pad to a full cache line.
//======================================================================================================
template <typename T>
struct alignas(64) ParameterSlot {
    static_assert(std::is_trivially_copyable<T>::value,
                  "ParameterSlot<T> requires T to be trivially copyable");

    T buffers[2];
    std::atomic<uint64_t> seq;
    // v5.12.1.B — paired publish_tick per buffer. Writer stores the slow-
    // path tick counter alongside buffers[next_idx]; reader returns it
    // through the optional out-param. Hot-path uses (now_tick -
    // publish_tick) > cfg.param_max_age_ticks to gate stale params via
    // SHALT_PARAM_STALE. Same seqlock barrier protects both arrays —
    // bytewise paired with the buffer at the same index.
    uint64_t publish_ticks[2];
    // pad the slot itself to a multiple of 64 so adjacent slots in arrays
    // (e.g. inside ExecutionCore[]) don't share cache lines (pitfall P5.5).
    // Pad shrunk from 56 → 40 to absorb the 16 bytes of publish_ticks[2]
    // without growing the struct beyond its existing 64-byte stride.
    uint8_t _pad[64 - sizeof(std::atomic<uint64_t>) - sizeof(uint64_t) * 2];
};

//======================================================================================================
// [INIT]
//======================================================================================================
// install the initial value in both buffers, set seq to 0 (even = stable,
// active idx = 0). a consumer that reads before the first write sees the
// initial value rather than uninitialized memory.
//======================================================================================================
template <typename T>
inline void ParameterSlot_Init(ParameterSlot<T>* slot, const T& initial) {
    slot->buffers[0] = initial;
    slot->buffers[1] = initial;
    // v5.12.1.B — both publish_ticks start at 0 (warmup sentinel; hot-path
    // staleness gate treats 0 as "no tick stamped, never gate" via the
    // existing pre-warmup escape).
    slot->publish_ticks[0] = 0;
    slot->publish_ticks[1] = 0;
    slot->seq.store(0, std::memory_order_release);
}

//======================================================================================================
// [WRITE — PRODUCER, CONTROLLER SIDE]
//======================================================================================================
// classic seqlock write:
//   1. read current seq (relaxed — single producer, only we write this)
//   2. compute next buffer index (alternates 0/1)
//   3. release-store seq+1 (parity now odd, "mid-write" signal)
//   4. memcpy the new value into the next buffer
//   5. release-store seq+2 (parity back to even, version advanced by 2,
//      active idx now points to the just-written buffer)
//
// the two release stores form a "fence" around the non-atomic memcpy. any
// consumer that sees seq+1 will retry; any consumer that sees seq+2 is
// guaranteed to read the new buffer (the memcpy happened-before the release
// store, so it's visible to any acquire-loading reader).
//
// wait-free: no retries on the write side. the producer always makes
// progress in O(1) operations.
//
// TSan note: the `slot->buffers[next_idx] = new_params` line is, at the byte
// level, a data race against any concurrent ParameterSlot_Read. that race is
// the entire POINT of the seqlock — the version counter dance ensures the
// reader detects torn reads and retries, so the byte-level race is benign by
// construction. ThreadSanitizer correctly reports it as a race; we tag both
// functions with no_sanitize("thread") so TSan doesn't false-flag the
// pattern. this is the same approach the Linux kernel uses for seqcount_t.
// the functional stress test (zero torn reads in 100k+ writes) is what
// actually validates correctness.
//======================================================================================================
template <typename T>
__attribute__((no_sanitize("thread")))
inline void ParameterSlot_Write(ParameterSlot<T>* slot, const T& new_params,
                                 uint64_t publish_tick = 0) {
    uint64_t s = slot->seq.load(std::memory_order_relaxed);
    // Active idx is bit 1 (since bit 0 is the parity). Next write goes to the
    // opposite buffer. The XOR with 1 alternates 0 → 1 → 0 → 1.
    uint64_t next_idx = ((s >> 1) & 1ULL) ^ 1ULL;
    // Mark in-progress: parity flips to odd. This invalidates any consumer
    // mid-read of the OLD buffer (they'll see odd seq and retry).
    slot->seq.store(s + 1, std::memory_order_release);
    // Non-atomic memcpy into the inactive buffer. Safe — no consumer reads
    // this buffer until we publish.
    slot->buffers[next_idx] = new_params;
    // v5.12.1.B — publish_tick paired with buffer in the same seqlock window.
    // Reader sees consistent (params, tick) tuple. publish_tick=0 (default)
    // disables hot-path staleness gate for legacy callers.
    slot->publish_ticks[next_idx] = publish_tick;
    // Mark done: parity flips back to even, version advanced by 2, active idx
    // bit now reflects the new buffer. Consumers that sampled seq+1 will retry
    // and see seq+2 with the new buffer ready.
    slot->seq.store(s + 2, std::memory_order_release);
}

//======================================================================================================
// [READ — CONSUMER, EXECUTION CORE HOT PATH]
//======================================================================================================
// classic seqlock read:
//   1. acquire-load seq into s1
//   2. if s1 is odd (mid-write), pause and retry
//   3. compute idx from s1
//   4. memcpy buffers[idx] into caller storage
//   5. acquire-load seq into s2
//   6. if s1 != s2 (a write completed during the copy), retry
//
// the two acquire loads bracket the copy. if the producer didn't write at
// all during the copy, s1 == s2 and we're done. if the producer wrote, the
// version advanced by 2, s1 != s2, we retry. retries are bounded only by
// producer activity, which in production is ~1 write per ~3 seconds (slow
// path). the steady state always succeeds on the first try.
//
// the result is written to caller-provided storage. we can't return a
// reference because the buffer may be overwritten while the caller is using
// it. the caller can stack-allocate T cheaply.
//
// always_inline so the read is folded into the calling tick path. the
// retry loop becomes one iteration in the steady state, the rest is dead code
// the branch predictor handles trivially.
//======================================================================================================
template <typename T>
__attribute__((always_inline))
__attribute__((no_sanitize("thread")))
static inline void ParameterSlot_Read(const ParameterSlot<T>* slot, T* out,
                                       uint64_t* publish_tick_out = nullptr) {
    uint64_t s1, s2;
    for (;;) {
        s1 = slot->seq.load(std::memory_order_acquire);
        if ((s1 & 1ULL) != 0) {
            // mid-write, pause briefly and retry. on x86 the pause hint
            // reduces power and improves performance under spin contention.
            __builtin_ia32_pause();
            continue;
        }
        uint64_t idx = (s1 >> 1) & 1ULL;
        *out = slot->buffers[idx];
        // v5.12.1.B — paired publish_tick read inside the same seqlock
        // bracket. Caller passes nullptr to skip (legacy call sites pay
        // zero — branch eliminated by the compiler).
        if (publish_tick_out) {
            *publish_tick_out = slot->publish_ticks[idx];
        }
        s2 = slot->seq.load(std::memory_order_acquire);
        if (s1 == s2) return;
        // version changed during the copy — a write completed. retry to get
        // a self-consistent snapshot.
    }
}

//======================================================================================================
// [INTROSPECTION HELPERS — for tests and diagnostics]
//======================================================================================================
template <typename T>
inline uint64_t ParameterSlot_Sequence(const ParameterSlot<T>* slot) {
    return slot->seq.load(std::memory_order_relaxed);
}

template <typename T>
inline uint8_t ParameterSlot_ActiveIndex(const ParameterSlot<T>* slot) {
    uint64_t s = slot->seq.load(std::memory_order_relaxed);
    return (uint8_t)((s >> 1) & 1ULL);
}

}  // namespace tt
