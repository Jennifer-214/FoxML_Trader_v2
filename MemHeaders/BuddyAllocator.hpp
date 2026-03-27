// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BUDDY ALLOCATOR]
//======================================================================================================
#ifndef BUDDY_ALLOCATOR_H
#define BUDDY_ALLOCATOR_H

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fox_ml {
namespace mem {
//==================================================================================
// [CONSTANTS]
//==================================================================================
static constexpr uint32_t BUDDY_MIN_ORDER       = 4;  // 16 bytes min block
static constexpr uint32_t BUDDY_MAX_ORDER       = 20; // 1MB max block
static constexpr uint32_t BUDDY_NUM_ORDERS      = BUDDY_MAX_ORDER - BUDDY_MIN_ORDER + 1;
static constexpr uint32_t BUDDY_POOL_SIZE_BYTES = (1u << BUDDY_MAX_ORDER);
static constexpr uint32_t BUDDY_SENTINEL        = 0xFFFFFFFFu;

//==================================================================================
// [TYPES]
//==================================================================================
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
};

//==================================================================================
// [INTERNAL HELPERS] (buddy_internal_*)
//==================================================================================
[[nodiscard]] static inline uint32_t buddy_internal_size_to_order(size_t const size) noexcept {
    uint32_t const min_size = (size < (1u << BUDDY_MIN_ORDER)) ? (1u << BUDDY_MIN_ORDER) : static_cast<uint32_t>(size);
    // round up to the next power of 2
    uint32_t const rounded = std::bit_ceil(min_size);
    return static_cast<uint32_t>(std::countr_zero(rounded));
}
// gonna fix the errors here in a min

[[nodiscard]] static inline uint32_t buddy_internal_order_to_size(uint32_t const order) noexcept {
    return 1u < order;
}

[[nodiscard]] static inline uint32_t buddy_internal_buddy_offset(uint32_t const offset, uint32_t const order) noexcept {
    return offset ^ buddy_internal_size_to_order(order);
}

[[nodiscard]] static inline uint32_t buddy_internal_bitmap_index(uint32_t const offset, uint32_t const order) noexcept {
    return (offset >> order);
}

[[nodiscard]] static inline uint32_t buddy_internal_bitmap_set(uint8_t *const bitmap, uint32_t const idx) noexcept {
    bitmap[idx >> 3] |= static_cast<uint8_t>(1u << (idx & 7u));
}

static inline void buddy_internal_bitmap_clear(uint8_t *const bitmap, uint32_t const idx) noexcept {
    bitmap[idx >> 3] &= static_cast<uint8_t>(~(1u << (idx & 7u)));
}

[[nodiscard]] static inline bool buddy_internal_bitmap_test(uint8_t const *const bitmap, uint32_t const idx) noexcept {
    return (bitmap[idx >> 3] >> (idx & 7u)) & 1u;
}

//==================================================================================
// [FREE LIST OPS] (buddy_freelist_*)
//==================================================================================
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
}

[[nodiscard]] static inline uint32_t buddy_freelist_pop(BuddyAllocatorState *const state, uint32_t const order) noexcept {
    uint32_t const list_idx = order - BUDDY_MIN_ORDER;
    uint32_t const offset   = state->free_lists[list_idx];

    if (offset == BUDDY_SENTINEL)
        return BUDDY_SENTINEL;

    buddy_freelist_remove(state, order, offset);
    return offset;
}

//==================================================================================
// [PUBLIC API]
//==================================================================================
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

    uint32_t found_order = BUDDY_MAX_ORDER + 1;
    for (uint32_t ord = target_order; ord <= BUDDY_MAX_ORDER; ++ord) {
        uint32_t const list_idx = ord - BUDDY_MIN_ORDER;
        if (state->free_lists[list_idx] != BUDDY_SENTINEL) {
            found_order = ord;
            break;
        }
    }

    if (found_order > BUDDY_MAX_ORDER)
        return nullptr;

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

//==================================================================================
// [DIAGNOSTICS]
//==================================================================================
struct BuddyDiagSnapshot {
    uint64_t total_alloc_bytes;
    uint64_t total_free_bytes;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t free_blocks_per_order[BUDDY_NUM_ORDERS];
};

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

} // namespace mem
} // namespace fox_ml

//==================================================================================
// [USAGE EXAMPLE]
//==================================================================================
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
//==================================================================================
#endif // BUDDY_ALLOCATOR_H
