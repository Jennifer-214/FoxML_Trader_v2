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
#include "../FixedPoint/FixedPointN.hpp"

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
    pool->slots    = (CurrentOrder<F> *)calloc(capacity, sizeof(CurrentOrder<F>));
    pool->bitmap   = 0;
    pool->capacity = capacity;
}

template <unsigned F> inline CurrentOrder<F> *OrderPool_Allocate(OrderPool<F> *pool) {
    uint32_t index = __builtin_ctzll(~pool->bitmap);
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
