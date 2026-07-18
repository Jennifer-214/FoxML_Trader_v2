// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/BuddyAllocator.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[reserved buddy allocator (fox_ml::mem, NOT wired — zero includers) — 16B..1MB power-of-2 blocks, O(1) order lookup via free_list_bitmap; a KNOWN bitmap-index collision gap is flagged below]
// [CONTAINS]
//   - [STRUCT]_[BuddyAllocatorState]   (constants + BuddyFreeNode ride)
//   - [FUNCTION]_[buddy_alloc_bytes]   (+ init / free_ptr / freelist ops / internal helpers family)
//   - [FUNCTION]_[buddy_diag_snapshot]   (BuddyDiagSnapshot rides)
//======================================================================================================
//
// v5.11.13 (2026-05-07) — typo fixes + O(1) order lookup.
//
// Status: NOT WIRED INTO PRODUCTION YET. Engine uses PoolAllocator
// (bitmap-indexed slot pool) for orders + InitArena (bump-pointer
// over mmap) for slow-state. BuddyAllocator is reserved for future
// variable-size allocation (e.g. backtest-side scratch buffers).
//
// Known design gap (NOT fixed in v5.11.13 because no production
// caller exists): buddy_internal_bitmap_index(offset, order) uses
// `(offset >> order)` as the bitmap index, which collides across
// orders — order=4 offset=0 and order=5 offset=0 both index bit 0
// of the same bitmap. The right encoding is heap-tree-style,
// `(1u << (BUDDY_MAX_ORDER - order)) + (offset >> order) - 1`,
// which gives each (order, offset) pair a unique bit. Defer until
// the allocator is actually wired in production so the design
// intent is clear. Flagged here so the next person doesn't have
// to re-derive the bug.
//======================================================================================================
#ifndef BUDDY_ALLOCATOR_H
#define BUDDY_ALLOCATOR_H

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fox_ml {
namespace mem {
//======================================================================
// [STRUCT]_[BuddyAllocatorState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the 1MB in-struct pool + 17 per-order intrusive free lists + split/alloc bitmaps + stats + the v5.11.13 non-empty-list bitmap (constants + BuddyFreeNode ride)]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[CONSTANTS]
//------------------------------------------------------------------
static constexpr uint32_t BUDDY_MIN_ORDER       = 4;  // 16 bytes min block
static constexpr uint32_t BUDDY_MAX_ORDER       = 20; // 1MB max block
static constexpr uint32_t BUDDY_NUM_ORDERS      = BUDDY_MAX_ORDER - BUDDY_MIN_ORDER + 1;
static constexpr uint32_t BUDDY_POOL_SIZE_BYTES = (1u << BUDDY_MAX_ORDER);
static constexpr uint32_t BUDDY_SENTINEL        = 0xFFFFFFFFu;

//------------------------------------------------------------------
// [SECTION]_[TYPES]
//------------------------------------------------------------------
struct BuddyFreeNode {
    uint32_t next_offset;
    uint32_t prev_offset;
};

struct BuddyAllocatorState {
    alignas(64) uint8_t pool[BUDDY_POOL_SIZE_BYTES];
    uint32_t free_lists[BUDDY_NUM_ORDERS];
    uint8_t split_bitmap[BUDDY_POOL_SIZE_BYTES / (1 << BUDDY_MIN_ORDER) / 8];
    uint8_t alloc_bitmap[BUDDY_POOL_SIZE_BYTES / (1 << BUDDY_MIN_ORDER) / 8];
    uint64_t total_alloc_bytes;
    uint64_t total_free_bytes;
    uint32_t alloc_count;
    uint32_t free_count;
    // v5.11.13: bit N set ⇔ free_lists[N] non-empty. Maintained by
    // buddy_freelist_push/remove. Lets buddy_alloc_bytes skip the
    // O(BUDDY_NUM_ORDERS=17) scan for the lowest available order
    // ≥ target by masking the high bits and __builtin_ctz'ing.
    uint32_t free_list_bitmap;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — layout emitter cannot probe this block yet; header has ZERO includers (not wired), so the struct is in no TU's layout dump — quartet lands when wired, D-327)
//======================================================================
// [END_STRUCT]_[BuddyAllocatorState]
//======================================================================

//======================================================================
// [FUNCTION]_[buddy_alloc_bytes]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the allocator family (init / free_ptr / freelist push-remove-pop / internal helpers ride) — split-down allocate, coalesce-up free; O(1) order find via masked ctz]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[INTERNAL HELPERS] (buddy_internal_*)
//------------------------------------------------------------------
// v5.11.13: rewrote in C++17 idioms (__builtin_clz / power-of-2 round-up)
// — engine target is C++17. The pre-fix used <bit> (C++20 only),
// which compiled standalone but broke when the test included this
// header (controller_test compiled at -std=c++17).
[[nodiscard]] static inline uint32_t buddy_internal_size_to_order(size_t const size) noexcept {
    uint32_t const min_size = (size < (1u << BUDDY_MIN_ORDER)) ? (1u << BUDDY_MIN_ORDER) : static_cast<uint32_t>(size);
    // Round up to next power of 2 (C++17-friendly bit_ceil).
    // For min_size already a power of 2, returns min_size unchanged.
    // 1u << (32 - clz(x-1)) is the canonical idiom; guard min_size==1
    // since clz(0) is UB. (We already enforce min_size >= 16 above,
    // so the guard is defensive — keeps the helper safe in isolation.)
    uint32_t const rounded = (min_size <= 1u) ? 1u
                                              : (1u << (32u - static_cast<uint32_t>(__builtin_clz(min_size - 1u))));
    return static_cast<uint32_t>(__builtin_ctz(rounded));
}
// v5.11.13 (2026-05-07): typo cleanup. Pre-fix this function returned
// `1u < order` (a bool) instead of `1u << order` (the actual block
// size in bytes for the given order). Buddy_offset's helper call was
// also wrong: it took an order, but called size_to_order(order)
// which is the inverse. Both were caught simultaneously because
// they masked each other's badness in arithmetic — no production
// caller existed yet, so the bug was harmless until now.

[[nodiscard]] static inline uint32_t buddy_internal_order_to_size(uint32_t const order) noexcept {
    return 1u << order;
}

[[nodiscard]] static inline uint32_t buddy_internal_buddy_offset(uint32_t const offset, uint32_t const order) noexcept {
    return offset ^ buddy_internal_order_to_size(order);
}

[[nodiscard]] static inline uint32_t buddy_internal_bitmap_index(uint32_t const offset, uint32_t const order) noexcept {
    return (offset >> order);
}

// v5.11.13: was [[nodiscard]] uint32_t but body returns nothing — UB.
// The function only mutates; void is the correct shape and matches
// buddy_internal_bitmap_clear below.
static inline void buddy_internal_bitmap_set(uint8_t *const bitmap, uint32_t const idx) noexcept {
    bitmap[idx >> 3] |= static_cast<uint8_t>(1u << (idx & 7u));
}

static inline void buddy_internal_bitmap_clear(uint8_t *const bitmap, uint32_t const idx) noexcept {
    bitmap[idx >> 3] &= static_cast<uint8_t>(~(1u << (idx & 7u)));
}

[[nodiscard]] static inline bool buddy_internal_bitmap_test(uint8_t const *const bitmap, uint32_t const idx) noexcept {
    return (bitmap[idx >> 3] >> (idx & 7u)) & 1u;
}

//------------------------------------------------------------------
// [SECTION]_[FREE LIST OPS] (buddy_freelist_*)
//------------------------------------------------------------------
static inline void buddy_freelist_push(BuddyAllocatorState *const state, uint32_t const order, uint32_t const offset) noexcept {
    uint32_t const list_idx   = order - BUDDY_MIN_ORDER;
    BuddyFreeNode *const node = reinterpret_cast<BuddyFreeNode *>(state->pool + offset);

    node->prev_offset = BUDDY_SENTINEL;
    node->next_offset = state->free_lists[list_idx];

    if (state->free_lists[list_idx] != BUDDY_SENTINEL) {
        BuddyFreeNode *const head = reinterpret_cast<BuddyFreeNode *>(state->pool + state->free_lists[list_idx]);
        head->prev_offset         = offset;
    }

    state->free_lists[list_idx] = offset;
    // v5.11.13: maintain free_list_bitmap (set this list's bit since
    // it now has at least one entry).
    state->free_list_bitmap |= (1u << list_idx);
}

static inline void buddy_freelist_remove(BuddyAllocatorState *const state, uint32_t const order, uint32_t const offset) noexcept {
    uint32_t const list_idx   = order - BUDDY_MIN_ORDER;
    BuddyFreeNode *const node = reinterpret_cast<BuddyFreeNode *>(state->pool + offset);

    if (node->prev_offset != BUDDY_SENTINEL) {
        BuddyFreeNode *const prev = reinterpret_cast<BuddyFreeNode *>(state->pool + node->prev_offset);
        prev->next_offset         = node->next_offset;
    } else {
        state->free_lists[list_idx] = node->next_offset;
    }

    if (node->next_offset != BUDDY_SENTINEL) {
        BuddyFreeNode *const next = reinterpret_cast<BuddyFreeNode *>(state->pool + node->next_offset);

        next->prev_offset = node->prev_offset;
    }

    // v5.11.13: maintain free_list_bitmap (clear this list's bit if
    // it just became empty). A removal at a non-head position leaves
    // the head intact, so check the head pointer post-mutation.
    if (state->free_lists[list_idx] == BUDDY_SENTINEL) {
        state->free_list_bitmap &= ~(1u << list_idx);
    }
}

[[nodiscard]] static inline uint32_t buddy_freelist_pop(BuddyAllocatorState *const state, uint32_t const order) noexcept {
    uint32_t const list_idx = order - BUDDY_MIN_ORDER;
    uint32_t const offset   = state->free_lists[list_idx];

    if (offset == BUDDY_SENTINEL)
        return BUDDY_SENTINEL;

    buddy_freelist_remove(state, order, offset);
    return offset;
}

//------------------------------------------------------------------
// [SECTION]_[PUBLIC API]
//------------------------------------------------------------------
void buddy_init_state(BuddyAllocatorState *const state) noexcept {
    memset(state, 0, sizeof(BuddyAllocatorState));

    for (uint32_t i = 0; i < BUDDY_NUM_ORDERS; ++i) {
        state->free_lists[i] = BUDDY_SENTINEL;
    }

    buddy_freelist_push(state, BUDDY_MAX_ORDER, 0u);
    state->total_free_bytes = BUDDY_POOL_SIZE_BYTES;
}

[[nodiscard]] void *buddy_alloc_bytes(BuddyAllocatorState *const state, size_t const size) noexcept {
    if (size == 0 || size > BUDDY_POOL_SIZE_BYTES)
        return nullptr;

    uint32_t const target_order = buddy_internal_size_to_order(size);

    if (target_order > BUDDY_MAX_ORDER)
        return nullptr;

    // v5.11.13: O(1) lowest-available-order lookup via free_list_bitmap.
    // Pre-fix: linear scan of state->free_lists[target_order-MIN ..
    // MAX-MIN], O(BUDDY_NUM_ORDERS=17) worst-case. Post-fix: mask off
    // the bits below target_order's list_idx, then __builtin_ctz to
    // find the lowest set bit. ctz on 0 is UB, so we test for the
    // empty-mask case first (= no order ≥ target has any free block).
    uint32_t const target_list_idx = target_order - BUDDY_MIN_ORDER;
    uint32_t const masked_bitmap   = state->free_list_bitmap & ~((1u << target_list_idx) - 1u);
    if (masked_bitmap == 0u)
        return nullptr;
    uint32_t const found_list_idx  = static_cast<uint32_t>(__builtin_ctz(masked_bitmap));
    uint32_t found_order           = found_list_idx + BUDDY_MIN_ORDER;

    uint32_t offset = buddy_freelist_pop(state, found_order);

    while (found_order > target_order) {
        --found_order;
        uint32_t const buddy_off = offset + buddy_internal_order_to_size(found_order);

        buddy_internal_bitmap_set(state->split_bitmap, buddy_internal_bitmap_index(offset, found_order + 1));

        buddy_freelist_push(state, found_order, buddy_off);
    }

    buddy_internal_bitmap_set(state->alloc_bitmap, buddy_internal_bitmap_index(offset, target_order));

    uint32_t const block_size = buddy_internal_order_to_size(target_order);
    state->total_alloc_bytes += block_size;
    state->total_free_bytes -= block_size;
    ++state->alloc_count;

    return state->pool + offset;
}

void buddy_free_ptr(BuddyAllocatorState *const state, void *const ptr, size_t const size) noexcept {
    if (!ptr || size == 0)
        return;

    uint32_t offset = static_cast<uint32_t>(reinterpret_cast<uint8_t *>(ptr) - state->pool);

    uint32_t const target_order = buddy_internal_size_to_order(size);

    buddy_internal_bitmap_clear(state->alloc_bitmap, buddy_internal_bitmap_index(offset, target_order));

    uint32_t const block_size = buddy_internal_order_to_size(target_order);
    state->total_alloc_bytes -= block_size;
    state->total_free_bytes += block_size;
    ++state->free_count;

    uint32_t order = target_order;
    while (order < BUDDY_MAX_ORDER) {
        uint32_t const buddy_off = buddy_internal_buddy_offset(offset, order);

        uint32_t const parent_idx = buddy_internal_bitmap_index(offset & ~(buddy_internal_order_to_size(order)), order + 1);

        bool const parent_is_split = buddy_internal_bitmap_test(state->split_bitmap, parent_idx);

        if (!parent_is_split)
            break;

        uint32_t buddy_alloc_idx = buddy_internal_bitmap_index(buddy_off, order);
        if (buddy_internal_bitmap_test(state->alloc_bitmap, buddy_alloc_idx))
            break;

        buddy_freelist_remove(state, order, buddy_off);

        buddy_internal_bitmap_clear(state->split_bitmap, parent_idx);

        offset = (offset < buddy_off) ? offset : buddy_off;
        ++order;
    }

    buddy_freelist_push(state, order, offset);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[buddy_alloc_bytes]
//======================================================================

//======================================================================
// [STRUCT]_[BuddyDiagSnapshot]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the allocator diagnostics snapshot — total alloc/free bytes + alloc/free counts + per-order free-block counts]
//======================================================================
// [CODE]
//======================================================================
struct BuddyDiagSnapshot {
    uint64_t total_alloc_bytes;
    uint64_t total_free_bytes;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t free_blocks_per_order[BUDDY_NUM_ORDERS];
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//======================================================================
// [END_STRUCT]_[BuddyDiagSnapshot]
//======================================================================

//======================================================================
// [FUNCTION]_[buddy_diag_snapshot]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[stats + per-order free-block counts into a BuddyDiagSnapshot — O(free blocks) walk; diagnostics only]
//======================================================================
// [CODE]
//======================================================================

BuddyDiagSnapshot buddy_diag_snapshot(BuddyAllocatorState const *const state) noexcept {
    BuddyDiagSnapshot snap{};
    snap.total_alloc_bytes = state->total_alloc_bytes;
    snap.total_free_bytes  = state->total_free_bytes;
    snap.alloc_count       = state->alloc_count;
    snap.free_count        = state->free_count;

    for (uint32_t i = 0; i < BUDDY_NUM_ORDERS; ++i) {
        uint32_t count  = 0;
        uint32_t offset = state->free_lists[i];
        while (offset != BUDDY_SENTINEL) {
            ++count;
            BuddyFreeNode const *const node = reinterpret_cast<BuddyFreeNode const *>(state->pool + offset);
            offset                          = node->next_offset;
        }
        snap.free_blocks_per_order[i] = count;
    }
    return snap;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[buddy_diag_snapshot]
//======================================================================

} // namespace mem
} // namespace fox_ml

//------------------------------------------------------------------
// [SECTION]_[USAGE EXAMPLE]
//------------------------------------------------------------------
// fox_ml::mem::BuddyAllocatorState allocator;
// fox_ml::mem::buddy_init_state(&allocator);
//
// void* pos = fox_ml::mem::buddy_alloc_bytes(&allocator,
// sizeof(PositionState)); new (pos) PositionState{};
//
// use position
//
// static_cast<PositionState*>(pos)->~PositionState();
// fox_ml::mem::buddy_free_ptr(&allocator, pos, sizeof(PositionState));
//------------------------------------------------------------------
#endif // BUDDY_ALLOCATOR_H
