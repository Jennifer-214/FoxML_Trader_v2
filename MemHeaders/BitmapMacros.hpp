// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/BitmapMacros.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BITMAP_PACKED] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[THE H14 primitive header — BITMAP_* single-bit + BITMAP_ATOMIC_* + BIT/POPCOUNT/FIRST helpers + MBS_* K-state slot accessors; every flag registry builds on these]
// [CONTAINS]
//   - [MACRO]_[BITMAP_* (single-thread)]
//   - [MACRO]_[BITMAP_ATOMIC_*]
//   - [MACRO]_[BITMAP_BIT_* + POPCOUNT + FIRST]
//   - [MACRO]_[MBS_*]
// [REFERENCE]_[DESIGN_SPEC]_[[bitmap-flag-api] [multi-bit-state-encoding-pattern]]
// [REFERENCE]_[INVARIANT]_[H14]
//======================================================================================================
// Common ergonomic API for bit-packed boolean flags + counter/percent
// fields. Used by multiple registries to provide uniform access semantics:
//   - FOREACH_STAMP_BOUND_MODEL_CONST has_flags (v5.14.8.A.merged)
//   - FOREACH_FAILURE_MODE failure_flags (v5.14.8.B)
//   - the TECH_DEBT-013 sweep candidates as they land (PerNodeSnap state
//     flags LANDED as FOREACH_PER_NODE_STATE_FLAG at v5.14.9.B.2; the
//     NodeContext / OMS flag registries followed; FOREACH_FEATURE enabled
//     bitmap, engine-wide cfg flags, etc. remain candidates)
//
// PRECEDENT (the uint16_t Portfolio bitmap): Portfolio<uint16_t> bitmap +
// OrderManagerState.order_bitmap. This header generalizes the pattern
// + provides ergonomic accessors so future bitmap consumers don't
// reinvent.
//
// WHY THIS HEADER (data-oriented design + compile-time-elision disciplines):
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

//----------------------------------------------------------------------
// [MACRO]_[BITMAP_* (single-thread)]
// [TAG]_[[ENGINE] [BITMAP_PACKED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[single-thread bit accessors — IS_SET/SET/CLR/TOGGLE/ANY/ALL/NONE; 1-cycle ops; IS_SET returns bool (int-truncation bug class sidestep)]
//----------------------------------------------------------------------
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

//----------------------------------------------------------------------
// [MACRO]_[BITMAP_ATOMIC_*]
// [TAG]_[[ENGINE] [BITMAP_PACKED] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[cross-thread bit accessors — __atomic_* intrinsics, RELAXED default with _ORDER variants for stricter callers]
//----------------------------------------------------------------------
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

//----------------------------------------------------------------------
// [MACRO]_[BITMAP_BIT_* + POPCOUNT + FIRST]
// [TAG]_[[ENGINE] [BITMAP_PACKED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[width-typed helpers — BIT_U8..U64 mask builders (signed-promotion-safe), POPCOUNT set-bit counts, FIRST ctz iteration]
//----------------------------------------------------------------------

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

// Node -> portfolio-slot mask (uint16 active_bitmap domain). Under
// partial exits every node owns 2 slots (legs A+B at 2n / 2n+1);
// single-leg otherwise. Extracted 2026-08-22 from 12 identical open-coded
// copies (EngineCommon exit-submit / strategy exit paths / hot-swap +
// strategy-swap gates / rebuild ratchet + TP-retune sites) — the mask
// SHAPE is the SSoT here; the partial_on SOURCE stays per-site (cfg
// lifecycle flag at cfg-time sites, oms_state_flags at runtime sites).
// DELIBERATELY UNCHECKED (historical sites are loop-bounded); the
// bounds-checked wrapper is Sharded_NodeSlotMask (ControllerEventLoop.hpp),
// which delegates its shape here — one impl, two safety tiers.
#define BITMAP_NODE_SLOT_MASK(node, partial_on)                             \
    ((partial_on)                                                           \
        ? (uint16_t)((1u << ((node) * 2)) | (1u << ((node) * 2 + 1)))       \
        : (uint16_t)(1u << (node)))

// Inverse direction — the logical node that owns portfolio slot `slot`.
// partial_on ∈ {0,1} IS the shift count (legs A/B at 2c/2c+1 → node c under
// partials; identity in single-slot mode — the D-294 lesson: an UNGATED >>1
// halved the node in single mode). Raw sibling of BITMAP_NODE_SLOT_MASK above
// (one SSoT family, two directions); the canonical checked accessor is
// Sharded_SlotNode (ControllerEventLoop.hpp), which delegates its shape here
// — same two-tier split as Sharded_NodeSlotMask.
#define BITMAP_SLOT_NODE(slot, partial_on) ((slot) >> (uint32_t)(partial_on))  // SLOT_DERIVE_OK: the raw SSoT shape (D-296)

//----------------------------------------------------------------------
// [MACRO]_[MBS_*]
// [TAG]_[[ENGINE] [BITMAP_PACKED] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[K-state N-bit slot accessors — GET/SET/EQ per width + ATOMIC_GET (no atomic SET: partial-word RMW needs a caller CAS loop) + SLOT_WIDTH/SLOT_MAX helpers]
//----------------------------------------------------------------------
// Per DESIGN_SPECS/multi-bit-state-encoding-pattern.md. Generalizes the
// single-bit BITMAP_* primitives above to K-state fields (K=2..16) packed
// into N-bit slots (N=ceil(log2(K))) within a shared bitmap word.
//
// First codebase application of generic MBS_* primitives (v5.15.5.C.3 Phase 3b).
// Pre-existing per-registry multi-bit accessors (OmsExitPredictorMetaRegistry's
// OMS_META_GET_REGIME / GET_ARM / IS_VALID / PACK / CLEAR — v5.15.5.C.2.1) were
// the FIRST APPLICATION pattern; this header now codifies the primitives so
// future K-state fields don't reinvent. Reference: oms_state_flags hybrid
// (3 single-bit flags + EVENT_LOG_MODE 2-bit slot) in v5.15.5.C.3.
//
// Slot identified by (mask, shift). Width is derivable from mask via
// __builtin_popcount(mask) when needed. Shift can also derive from mask
// via __builtin_ctz(mask) — but explicit shift at call sites is clearer
// (operator sees the slot position without mental masking).
//
// USAGE:
//   uint8_t flags = 0;
//   static constexpr uint8_t MASK_MODE  = 0x0C;  // bits 2-3 (2-bit slot, K=4 states)
//   static constexpr int     SHIFT_MODE = 2;
//
//   MBS_SET_U8(flags, MASK_MODE, SHIFT_MODE, 2);            // sets slot to value 2
//   uint8_t mode = MBS_GET_U8(flags, MASK_MODE, SHIFT_MODE); // reads slot value (0..3)
//   if (MBS_EQ_U8(flags, MASK_MODE, SHIFT_MODE, 1)) { ... }  // branchless equality
//   if (BITMAP_ANY(flags, MASK_MODE)) { ... }                // slot non-zero (any 1-bit set in slot)
//
// LATENCY (single-thread):
//   MBS_GET = 1 AND + 1 SHR (often fused into BMI BEXTR by compiler)
//   MBS_SET = 1 AND-NOT + 1 SHL + 1 OR + 1 store
//   MBS_EQ  = 1 AND + 1 compare-to-immediate (single-cycle when val is compile-time const)
// No branches; ILP-friendly (parallel decode of multiple slots in same field).
//
// THREAD SAFETY: MBS_SET is read-modify-write (3 ops). NOT atomic. Use MBS_ATOMIC_GET
// for cross-thread read (snapshot consistent with __ATOMIC_RELAXED ordering); use
// CAS loop if cross-thread MUTATION of multi-bit slots is required (no single-
// instruction atomic for partial-word updates).

// ---- Extract slot value (1 cycle: AND + SHR) ----
#define MBS_GET_U8(field, mask, shift) \
    ((uint8_t)(((field) & (mask)) >> (shift)))
#define MBS_GET_U16(field, mask, shift) \
    ((uint16_t)(((field) & (mask)) >> (shift)))
#define MBS_GET_U32(field, mask, shift) \
    ((uint32_t)(((field) & (mask)) >> (shift)))
#define MBS_GET_U64(field, mask, shift) \
    ((uint64_t)(((field) & (mask)) >> (shift)))

// ---- Insert value into slot (clears slot, then writes shifted value) ----
// VALUE masked to slot width to prevent overflow into adjacent slots.
#define MBS_SET_U8(field, mask, shift, value) do {                                   \
    (field) = (uint8_t)(((field) & (uint8_t)~(mask)) |                                \
                        (((uint8_t)(value) << (shift)) & (mask)));                    \
} while (0)
#define MBS_SET_U16(field, mask, shift, value) do {                                  \
    (field) = (uint16_t)(((field) & (uint16_t)~(mask)) |                              \
                         (((uint16_t)(value) << (shift)) & (mask)));                  \
} while (0)
#define MBS_SET_U32(field, mask, shift, value) do {                                  \
    (field) = (uint32_t)(((field) & ~(mask)) |                                        \
                         (((uint32_t)(value) << (shift)) & (mask)));                  \
} while (0)
#define MBS_SET_U64(field, mask, shift, value) do {                                  \
    (field) = (uint64_t)(((field) & ~(mask)) |                                        \
                         (((uint64_t)(value) << (shift)) & (mask)));                  \
} while (0)

// ---- Branchless equality (1 cycle when val is compile-time const) ----
// Compiles to AND + compare-to-immediate. No shift needed at runtime — the
// compiler pre-computes (val << shift & mask) at the call site.
// Returns: bool (consistent with BITMAP_IS_SET — avoids int-truncation hazard).
#define MBS_EQ_U8(field, mask, shift, val) \
    (((field) & (mask)) == (uint8_t)(((uint8_t)(val) << (shift)) & (mask)))
#define MBS_EQ_U16(field, mask, shift, val) \
    (((field) & (mask)) == (uint16_t)(((uint16_t)(val) << (shift)) & (mask)))
#define MBS_EQ_U32(field, mask, shift, val) \
    (((field) & (mask)) == (uint32_t)(((uint32_t)(val) << (shift)) & (mask)))
#define MBS_EQ_U64(field, mask, shift, val) \
    (((field) & (mask)) == (uint64_t)(((uint64_t)(val) << (shift)) & (mask)))

// ---- Atomic GET (cross-thread read with __ATOMIC_RELAXED snapshot) ----
// No MBS_ATOMIC_SET — multi-bit slot mutation needs CAS loop (not single-
// instruction atomic). If cross-thread multi-bit MUTATION is required,
// implement explicit __atomic_compare_exchange_n loop at call site.
#define MBS_ATOMIC_GET_U8(field, mask, shift) \
    ((uint8_t)((__atomic_load_n(&(field), __ATOMIC_RELAXED) & (mask)) >> (shift)))
#define MBS_ATOMIC_GET_U16(field, mask, shift) \
    ((uint16_t)((__atomic_load_n(&(field), __ATOMIC_RELAXED) & (mask)) >> (shift)))
#define MBS_ATOMIC_GET_U32(field, mask, shift) \
    ((uint32_t)((__atomic_load_n(&(field), __ATOMIC_RELAXED) & (mask)) >> (shift)))
#define MBS_ATOMIC_GET_U64(field, mask, shift) \
    ((uint64_t)((__atomic_load_n(&(field), __ATOMIC_RELAXED) & (mask)) >> (shift)))

// ---- Slot-width helper (compile-time popcount of mask) ----
// Useful for static_asserts validating slot capacity. Example:
//   static_assert(EVENT_LOG_MODE_COUNT <= (1u << MBS_SLOT_WIDTH(MASK_EVENT_LOG_MODE)),
//                 "EVENT_LOG_MODE values exceed multi-bit slot capacity");
#define MBS_SLOT_WIDTH(mask) (__builtin_popcount((unsigned)(mask)))

// ---- Slot-max-value helper (max representable value in a slot) ----
// For a 2-bit slot (mask=0x0C), max value = 3 (binary 11).
#define MBS_SLOT_MAX(mask) (((unsigned)(mask) >> __builtin_ctz((unsigned)(mask))))

#endif // BITMAP_MACROS_HPP
