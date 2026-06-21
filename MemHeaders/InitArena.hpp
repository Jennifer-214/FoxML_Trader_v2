// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [INIT ARENA — unified init-time mmap-backed bump allocator (v5.11.6.A)]
//======================================================================================================
// Replaces the per-struct `malloc(sizeof(...))` / `new T()` pattern at engine
// boot with a single mmap(MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE) backing
// store. All init-time allocations (RollingStats long/medium/baseline,
// CumDeltaState, per-core NodeSlowState, strategy state factories) bump-pointer
// from this arena.
//
// Properties:
//   - Single mmap → single munmap. No per-allocation free, no fragmentation.
//   - MAP_POPULATE pre-faults all pages at engine boot so the first slow-path
//     cycle never hits a page-fault tail.
//   - All engine state packed in contiguous physical memory → improves spatial
//     locality vs scattered malloc() pointers.
//   - Lifetime matches engine — the arena is destroyed when the engine
//     terminates (after worker threads have joined).
//
// Audit: LATENCY_OPTIMIZATION_AUDIT.md Part 7 Item 2 (init allocations).
// Plan: plans/2026-05-07-v5.11.6-allocator-eradication.md Phase A.
//
// Design rules:
//   - Bump pointer; alloc never fails when there's space, never frees an
//     individual alloc.
//   - Caller-provided alignment per call (typical: alignof(T)).
//   - On mmap failure, the engine logs WARN + falls back to a malloc-backed
//     buffer of the same logical capacity. Behavior is identical except the
//     pre-fault guarantee is lost.
//
// SINGLE-WRITER REQUIREMENT: InitArena is NOT thread-safe. All allocations
// must happen before worker threads are spawned. Engine boot is the only
// caller. Failure to honor this WILL race; we don't add a mutex because
// the call sites are init-time-only by construction.
//======================================================================================================

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <sys/mman.h>
#include <errno.h>

namespace tt {

struct InitArena {
    uint8_t* base;       // start of the mmap'd region (or malloc'd fallback)
    size_t   capacity;   // total bytes available
    size_t   used;       // bump-pointer offset
    int      is_mmap;    // 1 = base was mmap'd; 0 = malloc fallback
};

// Create a new arena of exactly `bytes` capacity. Pre-faults all pages via
// MAP_POPULATE so subsequent allocations + first-write never page-fault.
// On mmap failure, falls back to malloc with a stderr WARN; the caller can
// continue but loses the pre-fault guarantee.
//
// v5.11.22 — extra_flags lets the caller request additional mmap flags
// beyond the baseline (MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE).
// Currently used for MAP_HUGETLB (operator-gated via cfg.init_arena_use_hugepages).
// On MAP_HUGETLB failure (no hugepages reserved), retry without HUGETLB
// before falling back to malloc — operator missing the OS-level
// reservation gets a non-fatal degraded-but-functional path.
inline InitArena InitArena_Create(size_t bytes, int extra_flags = 0) {
    InitArena a;
    a.capacity = bytes;
    a.used     = 0;
    a.is_mmap  = 0;
    a.base     = nullptr;
    int base_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE;
    int flags      = base_flags | extra_flags;
    void* mem = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (mem == MAP_FAILED && extra_flags != 0) {
        // v5.11.22 — retry without extra_flags (most likely cause is
        // MAP_HUGETLB asked but no hugepages reserved at the OS level).
        // WARN so operator can see the fallback happened.
        std::fprintf(stderr, "[InitArena] WARN: mmap(%zu, flags=0x%x) failed (%s); "
                     "retrying without extra_flags=0x%x — likely missing OS-level "
                     "hugepage reservation. See DOCS/OPERATOR_DEPLOYMENT.md.\n",
                     bytes, flags, std::strerror(errno), extra_flags);
        mem = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, base_flags, -1, 0);
    }
    if (mem != MAP_FAILED) {
        a.base = (uint8_t*)mem;
        a.is_mmap = 1;
    } else {
        std::fprintf(stderr, "[InitArena] WARN: mmap(%zu, MAP_POPULATE) failed (%s), "
                     "falling back to malloc — pre-fault guarantee lost\n",
                     bytes, std::strerror(errno));
        a.base = (uint8_t*)std::malloc(bytes);
        if (!a.base) {
            std::fprintf(stderr, "[InitArena] FATAL: malloc fallback also failed for %zu bytes\n",
                         bytes);
            a.capacity = 0;
        } else {
            // Zero-fill to match MAP_ANONYMOUS semantics so callers can
            // assume initial-zero memory.
            std::memset(a.base, 0, bytes);
        }
    }
    return a;
}

// Bump-pointer allocate `bytes` aligned to `align`. Returns nullptr if the
// arena is exhausted (caller should fall back to malloc + log).
//
// `align` must be a power of two (alignof(T) always is). Typical values
// are 8 (for plain structs) or 64 (for cache-line-aligned hot structures).
inline void* InitArena_Alloc(InitArena* a, size_t bytes, size_t align) {
    if (!a || !a->base || bytes == 0) return nullptr;
    // Round up `used` to alignment boundary.
    size_t aligned_used = (a->used + align - 1) & ~(align - 1);
    if (aligned_used + bytes > a->capacity) {
        // Not actually fatal — caller falls back to malloc/new, losing
        // the pre-fault guarantee for this allocation but preserving
        // engine functionality. WARN once per call site implicitly via
        // the raw fprintf (no rate limiting; arena exhaustion at boot
        // is bounded by the number of init-time alloc sites).
        std::fprintf(stderr, "[InitArena] WARN: capacity exhausted "
                     "(req=%zu align=%zu, used=%zu/%zu); caller will "
                     "fall back to malloc — bump arena size to silence\n",
                     bytes, align, a->used, a->capacity);
        return nullptr;
    }
    void* p = a->base + aligned_used;
    a->used = aligned_used + bytes;
    return p;
}

// Type-safe convenience wrapper. Allocates space for one T with proper
// alignment. Caller is responsible for placement-new construction
// (bump-pointer arena does not run constructors).
template <typename T>
inline T* InitArena_AllocOne(InitArena* a) {
    return (T*)InitArena_Alloc(a, sizeof(T), alignof(T));
}

// Destroy the arena. Safe to call on a never-Created arena (zeroed
// struct). After Destroy, the arena is empty and capacity=0.
inline void InitArena_Destroy(InitArena* a) {
    if (!a || !a->base) return;
    if (a->is_mmap) {
        if (::munmap(a->base, a->capacity) != 0) {
            std::fprintf(stderr, "[InitArena] WARN: munmap failed (%s)\n",
                         std::strerror(errno));
        }
    } else {
        std::free(a->base);
    }
    a->base     = nullptr;
    a->capacity = 0;
    a->used     = 0;
    a->is_mmap  = 0;
}

// Introspection: how many bytes have been allocated from the arena so far.
inline size_t InitArena_Used(const InitArena* a) {
    return a ? a->used : 0;
}

// Introspection: how many bytes remain available (after alignment rounding
// is unknown; this is an upper bound).
inline size_t InitArena_Remaining(const InitArena* a) {
    return (a && a->capacity > a->used) ? (a->capacity - a->used) : 0;
}

// Pointer-ownership check: true if `p` falls within this arena's mapped
// range. Used by per-struct Free routines to decide whether to call free()
// (malloc-fallback) or skip (arena-owned, lifetime managed by the arena).
//
// Usage pattern (PortfolioController_Free etc.):
//   if (ctrl->rolling_long && !InitArena_Owns(InitArena_Global(), ctrl->rolling_long))
//       free(ctrl->rolling_long);
//   ctrl->rolling_long = nullptr;
inline int InitArena_Owns(const InitArena* a, const void* p) {
    if (!a || !a->base || !p) return 0;
    const uint8_t* pb = (const uint8_t*)p;
    return (pb >= a->base) && (pb < a->base + a->capacity);
}

// Global arena pointer accessor (Meyer's singleton). Engine boot points this
// at the engine's InitArena instance; tests + foxml_suite leave it nullptr
// and get the malloc fallback path. The mutability of the slot means this
// doubles as the SETTER too:
//
//   tt::InitArena_Global() = &my_arena;   // engine boot (single-writer)
//   tt::InitArena_Global() = nullptr;     // engine shutdown
//   if (auto* a = tt::InitArena_Global()) { ... }  // consumer
//
// Single-writer requirement: only one thread (the engine boot thread) ever
// SETS this. Reads can happen from any thread but happen-before is
// guaranteed because the writes occur strictly before worker threads
// spawn and after they join.
inline InitArena*& InitArena_Global() {
    static InitArena* g = nullptr;
    return g;
}

} // namespace tt
