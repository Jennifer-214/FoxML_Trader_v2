// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.


//======================================================================================================
// [POOL ALLOCATOR]
//======================================================================================================
#ifndef POOL_ALLOCATOR_H
#define POOL_ALLOCATOR_H

#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>     // memset for mmap-backed zero-init
#include <sys/mman.h>   // v5.11.17 — mmap(MAP_POPULATE) backing under FOXML_POOL_USE_MMAP
#include "../FixedPoint/FixedPointN.hpp"

// v5.11.17 (2026-05-07) — backing-allocator switch.
//
// Pool backing was historically calloc(). 31 sites across tests +
// production called `free(pool.slots)` directly to clean up. The
// v5.11.6.B attempt to flip backing to mmap(MAP_POPULATE) regressed
// because `free()` is UB on mmap'd pointers — that ship reverted.
//
// This re-attempt introduces `OrderPool_DestroyBacking` as the SOLE
// cleanup entry point. All 31 prior `free(pool.slots)` callers are
// migrated to it (boundary-stable refactor: callers see a stable API,
// implementation can switch backing freely).
//
// Backing default stays `calloc` for v5.11.17 to keep behavior
// bytewise-identical at the same tag. Define FOXML_POOL_USE_MMAP at
// build time to flip to mmap(MAP_POPULATE | MAP_PRIVATE | MAP_ANONYMOUS).
// v5.11.22 (MAP_HUGETLB) will lift this default once operator gates
// hugepage availability via cfg.

//======================================================================================================
// [CURRENT ORDER STRUCTURE]
//======================================================================================================
// these are the current structs, theyll probably change but idk, just consider these more like intial jsut to lay ground work, these are almost definitly gonna change now that i think about it lol
//======================================================================================================
// [EDIT [14-03-26]]
//======================================================================================================
// price and quantity are now FP32 - deterministic fixed-point all the way through
//======================================================================================================
// [EDIT [14-03-26]]
//======================================================================================================
// templated on FP precision - engine code picks the width with e.g. CurrentOrder<64>
//======================================================================================================
template <unsigned F> struct CurrentOrder {
    uint64_t order_id;
    FPN<F> price;
    FPN<F> quantity;
};

template <unsigned F> struct OrderPool {
    CurrentOrder<F> *slots;
    uint64_t bitmap;
    uint32_t capacity;
};
//======================================================================================================
// [POOL ALLOCATOR FUNCTION PROTOTYPES]
//======================================================================================================
// current working code, subject to chaaanggggeeeee
//======================================================================================================
template <unsigned F> inline void OrderPool_init(OrderPool<F> *pool, uint32_t capacity) {
#ifdef FOXML_POOL_USE_MMAP
    // v5.11.17 — mmap(MAP_POPULATE) backing. MAP_POPULATE pre-faults the
    // pages so the first access on the hot path doesn't pay a fault.
    // MAP_PRIVATE | MAP_ANONYMOUS = no file backing, copy-on-write isolation.
    // Caller MUST use OrderPool_DestroyBacking — `free()` on this pointer
    // is UB. Size is rounded up to the page boundary by the kernel.
    size_t bytes = (size_t)capacity * sizeof(CurrentOrder<F>);
    void *backing = mmap(nullptr, bytes,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                          -1, 0);
    if (backing == MAP_FAILED) {
        pool->slots = nullptr;
    } else {
        // mmap(MAP_ANONYMOUS) zero-fills by the kernel on Linux, but explicit
        // memset is portable + costs nothing on top of MAP_POPULATE pre-fault.
        memset(backing, 0, bytes);
        pool->slots = (CurrentOrder<F> *)backing;
    }
#else
    pool->slots    = (CurrentOrder<F> *)calloc(capacity, sizeof(CurrentOrder<F>));
#endif
    pool->bitmap   = 0;
    pool->capacity = capacity;
}

// v5.11.17 — sole cleanup entry point for pool backing. Routes through
// the matching deallocator for whichever backing OrderPool_init used.
// Idempotent on a zeroed/uninitialized pool (no-op if slots is nullptr).
template <unsigned F> inline void OrderPool_DestroyBacking(OrderPool<F> *pool) {
    if (!pool || !pool->slots) return;
#ifdef FOXML_POOL_USE_MMAP
    munmap((void *)pool->slots, (size_t)pool->capacity * sizeof(CurrentOrder<F>));
#else
    free(pool->slots);
#endif
    pool->slots    = nullptr;
    pool->bitmap   = 0;
    pool->capacity = 0;
}

template <unsigned F> inline CurrentOrder<F> *OrderPool_Allocate(OrderPool<F> *pool) {
    uint64_t free_mask = ~pool->bitmap;
    if (!free_mask) return NULL; // pool full
    uint32_t index = __builtin_ctzll(free_mask);
    pool->bitmap |= (1ULL << index);
    return &pool->slots[index];
}

template <unsigned F> inline void OrderPool_Free(OrderPool<F> *pool, CurrentOrder<F> *slot_ptr) {
    uint32_t index = (uint32_t)(slot_ptr - pool->slots);
    pool->bitmap &= ~(1ULL << index);
}

template <unsigned F> inline uint32_t OrderPool_CountActive(const OrderPool<F> *pool) {
    uint32_t popcount = __builtin_popcountll(pool->bitmap);
    return popcount;
}
//======================================================================================================
#endif // POOL_ALLOCATOR_H
