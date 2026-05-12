// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BITMAP MACROS — reusable bit-packed flag accessor API — v5.14.8.A.0.b]
//======================================================================================================
// Common ergonomic API for bit-packed boolean flags + counter/percent
// fields. Used by multiple registries to provide uniform access semantics:
//   - FOREACH_STAMP_BOUND_MODEL_CONST has_flags (v5.14.8.A.merged)
//   - FOREACH_FAILURE_MODE failure_flags (v5.14.8.B)
//   - Future TECH_DEBT-013 sweep candidates (PerCoreSnap state flags,
//     FOREACH_FEATURE enabled bitmap, engine-wide cfg flags, etc.)
//
// PRECEDENT (CLAUDE.md item 1): Portfolio<uint16_t> bitmap +
// OrderManagerState.order_bitmap. This header generalizes the pattern
// + provides ergonomic accessors so future bitmap consumers don't
// reinvent.
//
// WHY THIS HEADER (data-oriented design per CLAUDE.md item 1, 18):
//   1. Memory: 16/32/64 binary states in 2/4/8 bytes (vs N bytes byte-per-flag)
//   2. Branchless multi-flag check: (flags & (MASK_X | MASK_Y)) — one compare
//   3. Atomic multi-flag updates: __atomic_fetch_or vs N separate stores
//   4. Branchless "any flag set?": (flags != 0) — one compare
//   5. Single cache-line access for entire flag set
//   6. Predictable branch behavior (boolean test on bitmask result)
//
// USAGE PATTERN:
//   struct MyStruct {
//       uint64_t has_flags;  // or uint16_t/uint32_t depending on flag count
//       // ... other fields
//   };
//
//   // Bit position constants (typically auto-allocated by X-macro, but
//   // can also be manual constexpr declarations):
//   static constexpr uint64_t MASK_X = (1ULL << 0);
//   static constexpr uint64_t MASK_Y = (1ULL << 1);
//
//   // Accessors:
//   MyStruct s{};
//   BITMAP_SET(s.has_flags, MASK_X);
//   if (BITMAP_IS_SET(s.has_flags, MASK_X)) { ... }
//   if (BITMAP_ANY(s.has_flags, MASK_X | MASK_Y)) { ... }  // multi-flag
//   BITMAP_CLR(s.has_flags, MASK_X);
//
// LATENCY: All macros expand to 1-2 machine instructions. Mask AND/OR
// are 1-cycle ops. Atomic variants use a single __atomic_* intrinsic.
// No hot-path additions — these REPLACE byte-per-flag patterns at
// neutral or better cost.
//
// THREAD SAFETY: Non-atomic variants are NOT thread-safe; use atomic
// variants when bitmap is shared across threads (e.g., observability
// flags written by slow path, read by display thread).
//======================================================================================================
#ifndef BITMAP_MACROS_HPP
#define BITMAP_MACROS_HPP

//------------------------------------------------------------------------------------------------------
// [SINGLE-THREAD ACCESSORS]
//------------------------------------------------------------------------------------------------------
// Use when bitmap is only accessed from one thread (e.g., per-core slow
// path's own state) OR when surrounding synchronization (mutex, seqlock)
// already provides ordering.

// Read: is the masked bit (or any bit in mask_set) set?
//   Returns: bool (always 0 or 1; safe across all integer widths).
//   Cost: 1 cycle (AND + compare).
//   IMPORTANT: returns bool, not the masked value. If you need the
//   masked value (e.g., for further mask manipulation), use the raw
//   expression `(field) & (mask)`.
//
//   Why bool not bitmask: int-truncation bug class. If a uint64_t
//   bitmap has only its top bit set (e.g., 0x8000_0000_0000_0000)
//   and the result is used in an `int`-typed context (e.g.,
//   check(name, condition) where condition is int), the lower 32
//   bits of the value (which are zero) become the int — predicate
//   evaluates false. Returning bool sidesteps the entire class.
#define BITMAP_IS_SET(field, mask)  (((field) & (mask)) != 0)

// Set: turn ON the masked bit(s).
//   Cost: 1 cycle (OR + store).
#define BITMAP_SET(field, mask)     ((field) |= (mask))

// Clear: turn OFF the masked bit(s).
//   Cost: 1 cycle (AND-NOT + store).
#define BITMAP_CLR(field, mask)     ((field) &= ~(mask))

// Toggle: flip the masked bit(s).
//   Cost: 1 cycle (XOR + store).
#define BITMAP_TOGGLE(field, mask)  ((field) ^= (mask))

// Any: equivalent to IS_SET but spelled to emphasize multi-flag intent.
//   Useful for "any failure?" / "any drift?" checks.
//   Cost: 1 cycle (AND + compare).
#define BITMAP_ANY(field, mask_set) (((field) & (mask_set)) != 0)

// All: are ALL bits in mask_set currently set?
//   Cost: 1 cycle (AND + compare to mask).
#define BITMAP_ALL(field, mask_set) (((field) & (mask_set)) == (mask_set))

// None: are NO bits in mask_set set?
//   Cost: 1 cycle (AND + compare to 0).
#define BITMAP_NONE(field, mask_set) (((field) & (mask_set)) == 0)

//------------------------------------------------------------------------------------------------------
// [ATOMIC ACCESSORS — cross-thread visibility]
//------------------------------------------------------------------------------------------------------
// Use when bitmap is shared across threads (e.g., observability flags
// written by slow path, read by display thread; or per-core failure
// flags read by other cores' panel rendering).
//
// Memory order: __ATOMIC_RELAXED is sufficient for observability flags
// (no happens-before constraint with other data). Caller can pass
// stricter ordering if needed for synchronization.

// Atomic load — reads field with relaxed ordering.
#define BITMAP_ATOMIC_LOAD(field) \
    __atomic_load_n(&(field), __ATOMIC_RELAXED)

// Atomic load with explicit memory order.
#define BITMAP_ATOMIC_LOAD_ORDER(field, order) \
    __atomic_load_n(&(field), (order))

// Atomic OR — turn ON the masked bit(s); returns prior value.
//   Equivalent to atomic SET. 1 instruction on modern x86 (lock or).
#define BITMAP_ATOMIC_SET(field, mask) \
    __atomic_fetch_or(&(field), (mask), __ATOMIC_RELAXED)

#define BITMAP_ATOMIC_SET_ORDER(field, mask, order) \
    __atomic_fetch_or(&(field), (mask), (order))

// Atomic AND-NOT — turn OFF the masked bit(s); returns prior value.
#define BITMAP_ATOMIC_CLR(field, mask) \
    __atomic_fetch_and(&(field), ~(mask), __ATOMIC_RELAXED)

#define BITMAP_ATOMIC_CLR_ORDER(field, mask, order) \
    __atomic_fetch_and(&(field), ~(mask), (order))

// Atomic XOR — flip the masked bit(s); returns prior value.
#define BITMAP_ATOMIC_TOGGLE(field, mask) \
    __atomic_fetch_xor(&(field), (mask), __ATOMIC_RELAXED)

// Atomic IS_SET — load + AND. For polling / display-thread observation.
//   Returns: bool (consistent with single-thread BITMAP_IS_SET).
#define BITMAP_ATOMIC_IS_SET(field, mask) \
    ((__atomic_load_n(&(field), __ATOMIC_RELAXED) & (mask)) != 0)

// Atomic ANY — multi-flag truthy check on a snapshot of the bitmap.
#define BITMAP_ATOMIC_ANY(field, mask_set) \
    ((__atomic_load_n(&(field), __ATOMIC_RELAXED) & (mask_set)) != 0)

//------------------------------------------------------------------------------------------------------
// [HELPERS]
//------------------------------------------------------------------------------------------------------

// Bit-position helper: build a mask for bit N.
//   Use the type-aware variant to avoid signed-int promotion bugs:
//     BITMAP_BIT_U8(0)  = (uint8_t)1
//     BITMAP_BIT_U16(0) = (uint16_t)1
//     BITMAP_BIT_U32(0) = 1u
//     BITMAP_BIT_U64(0) = 1ULL
#define BITMAP_BIT_U8(n)  ((uint8_t)((uint8_t)1u << (n)))
#define BITMAP_BIT_U16(n) ((uint16_t)((uint16_t)1u << (n)))
#define BITMAP_BIT_U32(n) ((uint32_t)(1u << (n)))
#define BITMAP_BIT_U64(n) (1ULL << (n))

// Population count: how many bits are set?
//   Built-in popcount; portable across GCC/Clang.
//   Useful for "how many failure modes fired?" queries.
#define BITMAP_POPCOUNT_U8(field)  (__builtin_popcount((unsigned)(field) & 0xFFu))
#define BITMAP_POPCOUNT_U16(field) (__builtin_popcount((unsigned)(field) & 0xFFFFu))
#define BITMAP_POPCOUNT_U32(field) (__builtin_popcount((unsigned)(field)))
#define BITMAP_POPCOUNT_U64(field) (__builtin_popcountll((unsigned long long)(field)))

// First-set-bit index (0-based). Returns 0 if no bits set
// (use BITMAP_ANY to disambiguate "none set" from "bit 0 set").
//   Useful for iteration: bit_idx = BITMAP_FIRST_U64(remaining);
//                          remaining &= ~BITMAP_BIT_U64(bit_idx);
#define BITMAP_FIRST_U8(field)  ((unsigned)__builtin_ctz((unsigned)(field) & 0xFFu))
#define BITMAP_FIRST_U16(field) ((unsigned)__builtin_ctz((unsigned)(field) & 0xFFFFu))
#define BITMAP_FIRST_U32(field) ((unsigned)__builtin_ctz((unsigned)(field)))
#define BITMAP_FIRST_U64(field) ((unsigned)__builtin_ctzll((unsigned long long)(field)))

#endif // BITMAP_MACROS_HPP
