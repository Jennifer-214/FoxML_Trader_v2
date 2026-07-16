// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
#include "../Limits.hpp"
//======================================================================================================
// [FILE]_[CoreFrameworks/Portfolio.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [PERSISTENCE] [BITMAP_PACKED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[position tracking core — registry-generated Position + bitmap Portfolio + slot lifecycle + snapshot persistence]
// [CONTAINS]
//   - [STRUCT]_[Position]
//   - [STRUCT]_[Portfolio]
//   - [STRUCT]_[ExitRecord]
//   - [STRUCT]_[PositionEntryArgs]
//   - [FUNCTION]_[ExitBuffer_PendingProceeds]
//   - [FUNCTION]_[Position_Reset]
//   - [FUNCTION]_[Portfolio_AddPositionWithExits]
//   - [FUNCTION]_[Portfolio_OpenSlot]
//   - [FUNCTION]_[Money_FillGross]
//   - [FUNCTION]_[Portfolio_CloseSlot]
//   - [FUNCTION]_[Portfolio_ComputePnL]
//   - [FUNCTION]_[PositionExitGate]
//   - [FUNCTION]_[Portfolio_Save]
//   - [FUNCTION]_[Portfolio_Load]
//======================================================================================================
// this is basically just gonna track positions and stuff and be the core portfolio managment system, im not sure if ill actually add the rebaalncing logic and stuff here, but it should eventually just serve as the API call to get position deltas and stuff, it will be more robust that just the simple pool allocator i was attempting earlier
//======================================================================================================
#ifndef PORTFOLIO_HPP
#define PORTFOLIO_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include "../MemHeaders/PositionFieldRegistry.hpp"  // v5.15.5.C.4 Phase POS — FOREACH_POSITION_FIELD + PERSIST_KIND dispatch
#include <stdio.h>
#include <time.h>      // v5.11.65 — clock_gettime(CLOCK_REALTIME) for entry_timestamp_us
#include <cstddef>     // v5.15.5.C.4 Phase POS — offsetof for static_assert layout locks
//------------------------------------------------------------------------------------------------------
// [SECTION]_[structs]
//------------------------------------------------------------------------------------------------------
// Im not sure how many positions i really want to track here but for now im just gonna leave it at like 16 i think, there will probably be more advanced logic added later to have a model that watches performace and dynamically updates or something like i attempted to do in FoxML core, but this is a deepr dive so i can actually learn and understand the logic behind stuff, and i just think its cool as shit, like why learn java when stuff lke this exists lmao, also i get to make my own library so im not functioning off blackbox implementations where the end of the documentation is lke "Trust me bro", and i hate reading documentation, so id rather build my own
//======================================================================================================

//======================================================================
// [STRUCT]_[Position]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [DECIMAL] [PERSISTENCE]]
// [THREAD]_[[SLOW_WRITER] [SLOW_READER]]
// [REFERENCE]_[INVARIANT]_[[H9] [H12] [H21]]
// [REFERENCE]_[DESIGN_SPEC]_[hot-side-array-element-alignment-for-sparse-access]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the canonical position record — registry-generated fields, wire-persisted 128B prefix, layout-locked]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F> struct alignas(64) Position {
    // Auto-generated fields from FOREACH_POSITION_FIELD (PositionFieldRegistry.hpp):
    //   take_profit_price (Money)   ← hot; offset 0
    //   stop_loss_price   (Money)   ← hot; offset 16  (Ship-A 16B FPN_Binary, was 24)
    //   quantity          (Money)
    //   entry_price       (Money)
    //   entry_fee         (Money)
    //   original_tp       (Money)
    //   original_sl       (Money)
    //   entry_timestamp_us (uint64_t)
    //   pair_index        (int8_t)
    #define POSITION_EMIT_FIELD(name, type, init, persist_kind, doc) type name = init;
    FOREACH_POSITION_FIELD(POSITION_EMIT_FIELD)
    #undef POSITION_EMIT_FIELD

    // Manual padding to align Position to 8 bytes after int8_t pair_index.
    // Part of wire format (PORTFOLIO_SNAPSHOT_VERSION byte layout; Ship-A 16B FPN_Binary).
    // POSITION_PERSIST_BYTES = offsetof(_pad_pos) + sizeof(_pad_pos) = 128 (was 184).
    uint8_t _pad_pos[7] = {0};   // H12: reaches the wire via the 128B blob dump → MUST be zero-init (D-295)

    // SKIP_PERSIST fields would expand here. Empty today per C.5 revert.
    // Ship-A 16B FPN_Binary: PERSIST data fills 128B = 2 cache lines exact, so the
    // `alignas(64)` decorator adds NO trailing pad (was 8B at 24B FPN_Binary / 192B).
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]_[registry generation + C.5 layout history]
//----------------------------------------------------------------------
// v5.15.5.C.5 — Position struct generated from FOREACH_POSITION_FIELD registry
// + `alignas(64)` per `DESIGN_SPECS/hot-side-array-element-alignment-for-sparse-access.md`.
//
// Field order (controlled by FOREACH_POSITION_FIELD in PositionFieldRegistry.hpp)
// preserves the prior manual struct layout:
//   - Hot-path fields first (TP/SL — ExitGate reads every tick; fit in 1st cache line)
//   - Warm fields middle (quantity, entry_price, entry_fee — read at fill / P&L)
//   - Cold fields last (originals, timestamp, pair_index — slow-path-only)
//
// v5.15.5.C.5 changes vs C.4 (as-landed, 24B-FPN era; Ship-A later re-derived 192B->128B):
//   - SKIP_PERSIST fields (exit_fill_price, is_maker) REVERTED to OMS sibling arrays
//     (per `slot-state-foreach-registry-with-storage-routing.md` decision tree for
//     sparse-access ephemeral state)
//   - `alignas(64)` on Position struct → sizeof = 192B = 3 cache lines exact
//   - Per-slot hot-path access: guaranteed 1 cache line (TP+SL fit in first 64B
//     of each Position; each Position[N] starts on 64B boundary)
//   - 8B trailing alignas pad (cost: 128B per OMS); savings: ~50% reduction in
//     hot-path cache misses at sparse-iteration access patterns
//
// POS.2's SKIP_PERSIST infrastructure (FOREACH_POSITION_FIELD_SKIP_PERSIST registry +
// PERSIST_KIND filter dispatch in Portfolio_Save/Load) RETAINED as future-extension
// capacity. Empty registry today; available for future fields that warrant Position-
// locality co-access with PERSIST fields.
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//----------------------------------------------------------------------
// [SIZE]_[128B]
// [ALIGN]_[64]
// [CACHE_LINES]_[2]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[Position]
//======================================================================

// v5.15.5.C.4 Phase POS — static_assert layout locks per the design spec
// `function-struct-alignment-for-single-mov-access.md` + wire-format byte
// preservation discipline. Catches accidental field-reorder that would
// invalidate PORTFOLIO_SNAPSHOT_VERSION=6 wire format.
//
// Reference layout (Ship-A 16B Money; PORTFOLIO_SNAPSHOT_VERSION=6; was 24B/v5):
//   offset 0:   take_profit_price   (16B)
//   offset 16:  stop_loss_price     (16B)
//   offset 32:  quantity            (16B)
//   offset 48:  entry_price         (16B)
//   offset 64:  entry_fee           (16B)
//   offset 80:  original_tp         (16B)
//   offset 96:  original_sl         (16B)
//   offset 112: entry_timestamp_us  (8B)
//   offset 120: pair_index          (1B)
//   offset 121: _pad_pos            (7B)
//   total:      128 bytes
// Ship-A 16B FPN_Binary — sizeof locked at 128B (= 2 cache lines exact; NO alignas trailing pad).
// PERSIST prefix = 128 bytes; v5 (184B/position) snapshots version-rejected (H21/D-144).
//
// Per DESIGN_SPECS/hot-side-array-element-alignment-for-sparse-access.md:
// - sizeof(Position) % 64 == 0 → each Position[N] starts on cache-line boundary
// - Hot-path read (TP + SL at offsets 0, 16) fits in first cache line of each
//   Position[N] — guaranteed 1 cache line per slot access, regardless of N
//
// Ship A (16B FPN_Binary): Position re-derived 192B→128B. HOT fields (TP@0, SL@16) still share cache-line 0;
// COLD (original_tp@80, original_sl@96) in line 1. 128B = 2 cache lines exact, no trailing pad.
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(Position<64>) == 128]
// [WHY]_[wire format (PORTFOLIO_SNAPSHOT_VERSION) + 2-cache-lines-exact hot-slot access — a silent grow/reorder is a snapshot break, H21]
static_assert(sizeof(Position<64>) == 128,
              "Position<64> size must be 128B (Ship A 16B FPN_Binary: 2 cache lines exact, no trailing pad; was 192B at 24B FPN_Binary); "
              "see DESIGN_SPECS/hot-side-array-element-alignment-for-sparse-access.md");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(Position<64>) == 64]
// [WHY]_[each Position[N] starts on a cache-line boundary — single-line-per-slot hot access]
static_assert(alignof(Position<64>) == 64,
              "Position<64> must have 64B alignment for hot-path single-cache-line-per-slot access");
static_assert(offsetof(Position<64>, take_profit_price)  == 0,   "Position layout: take_profit_price offset (HOT)");
static_assert(offsetof(Position<64>, stop_loss_price)    == 16,  "Position layout: stop_loss_price offset (HOT)");
static_assert(offsetof(Position<64>, quantity)           == 32,  "Position layout: quantity offset");
static_assert(offsetof(Position<64>, entry_price)        == 48,  "Position layout: entry_price offset");
static_assert(offsetof(Position<64>, entry_fee)          == 64,  "Position layout: entry_fee offset");
static_assert(offsetof(Position<64>, original_tp)        == 80,  "Position layout: original_tp offset");
static_assert(offsetof(Position<64>, original_sl)        == 96,  "Position layout: original_sl offset");
static_assert(offsetof(Position<64>, entry_timestamp_us) == 112, "Position layout: entry_timestamp_us offset");
static_assert(offsetof(Position<64>, pair_index)         == 120, "Position layout: pair_index offset");

// PERSIST byte count — first 128 bytes of Position go to wire format (Ship A 16B FPN_Binary; was 184B).
// Save/Load writes exactly POSITION_PERSIST_BYTES per position (16 positions × 128 = 2048 bytes payload).
// PORTFOLIO_SNAPSHOT_VERSION bumped 5→6 (D-144) — old 184B-per-Position snapshots are version-rejected.
// At 16B the PERSIST data fills the 2 cache lines exactly → NO trailing alignas pad.
template <unsigned F>
constexpr size_t POSITION_PERSIST_BYTES() {
    // 9 PERSIST value fields (112B FPN_Binary + 8B uint64 + 1B int8) + 7B _pad_pos = 128B (Ship A 16B FPN_Binary)
    return offsetof(Position<F>, _pad_pos) + 7;
}
static_assert(POSITION_PERSIST_BYTES<64>() == 128,
              "Position PERSIST byte count must equal 128 — wire format (PORTFOLIO_SNAPSHOT_VERSION=6, Ship A 16B) byte-identical");
static_assert(sizeof(Position<64>) - POSITION_PERSIST_BYTES<64>() == 0,
              "16B FPN_Binary: PERSIST fills 128B = 2 cache lines exact, NO trailing alignas pad (was 8B at 24B FPN_Binary)");
//======================================================================
// [STRUCT]_[Portfolio]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [BITMAP_PACKED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[bitmap-slot portfolio — active_bitmap + Position[16]; ctz walks skip cleared slots automatically]
//======================================================================
// [CODE]
//======================================================================
// bitmap-based like OrderPool - same __builtin_ctz pattern, no array shifting on removal,
// hot-path exit gate only walks set bits so cleared positions are skipped automatically
template <unsigned F> struct Portfolio {
    uint16_t active_bitmap; // hot: read first every tick by ExitGate (cache line 0)
    uint16_t _pad0;
    uint32_t _pad1;         // align positions to 8 bytes
    Position<F> positions[MAX_PORTFOLIO_POSITIONS];
    static_assert(MAX_PORTFOLIO_POSITIONS <= 16,
                  "active_bitmap is uint16_t (16 bits); MAX_PORTFOLIO_POSITIONS must fit it — raising it "
                  "past 16 silently truncates the bitmap (bitmap-overflow-protection-discipline; H21-sibling).");
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//----------------------------------------------------------------------
// [SIZE]_[2112B]
// [ALIGN]_[64]
// [CACHE_LINES]_[33]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[Portfolio]
//======================================================================

//======================================================================
// [STRUCT]_[ExitRecord]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [DECIMAL]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[exit-time snapshot of all position data — immune to slot reuse before DrainExits runs]
//======================================================================
// [CODE]
//======================================================================
// ExitRecord snapshots all position data at exit time — slot may be reused by a
// new fill before DrainExits runs, so nothing downstream should read from the slot
template <unsigned F> struct ExitRecord {
    uint32_t position_index;    // slot index (for entry_ticks/entry_strategy lookup only)
    int reason;                 // 0 = take profit, 1 = stop loss
    uint64_t tick;
    Money exit_price;
    // position data snapshot — captured at exit time, immune to slot reuse
    Money entry_price;
    Money quantity;
    Money entry_fee;
    int8_t pair_index;
    uint8_t _pad_rec[7];
};
//======================================================================
// [END_CODE]
//======================================================================
// [END_STRUCT]_[ExitRecord]
//======================================================================

template <unsigned F> struct ExitBuffer {
    uint32_t count;             // hot: read first every tick (cache line 0 with records[0])
    uint32_t _pad0;             // align records to 8 bytes
    ExitRecord<F> records[16];
};

template <unsigned F> inline void ExitBuffer_Init(ExitBuffer<F> *buf) {
    buf->count = 0;
}

template <unsigned F> inline void ExitBuffer_Clear(ExitBuffer<F> *buf) {
    buf->count = 0;
}

//======================================================================
// [FUNCTION]_[ExitBuffer_PendingProceeds]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [DECIMAL]]
// [REFERENCE]_[INVARIANT]_[H4]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[exact net proceeds pending in the exit buffer — slippage + fee applied per record]
//======================================================================
// [CODE]
//======================================================================
// exact net proceeds pending in exit buffer — matches what DrainExits/RecordExit will credit
// reads ALL data from ExitRecord (not position slots — those may have been reused)
template <unsigned F>
inline Money ExitBuffer_PendingProceeds(const ExitBuffer<F> *buf,
                                          Money fee_rate, Money slippage_pct) {
    Money total = Money_Zero();
    for (uint32_t i = 0; i < buf->count; i++) {
        Money exit_price = buf->records[i].exit_price;
        if (!Money_IsZero(slippage_pct)) {
            Money slip = Money_Mul(exit_price, slippage_pct);
            exit_price = Money_Sub(exit_price, slip);
        }
        Money gross = Money_Mul(exit_price, buf->records[i].quantity);
        Money fee = Money_Mul(gross, fee_rate);
        total = Money_Add(total, Money_Sub(gross, fee));
    }
    return total;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ExitBuffer_PendingProceeds]
//======================================================================

//------------------------------------------------------------------------------------------------------
// [SECTION]_[functions]
//------------------------------------------------------------------------------------------------------
// similar to the pool allocator, will need more work and im not sure if i want the rebalancing adn stuff here or in another header, probably another header for the actual managment, because these are just the basic functions to add and manipulate the actual opsitions, tnd are dependent on the buy/sell gates
//======================================================================================================

//======================================================================
// [FUNCTION]_[Position_Reset]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [CRITICAL]]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-182]
// [REFERENCE]_[DECISION]_[D-295]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the SINGLE source for clearing a slot to defaults — closes the subset-zeroing class (A19/A28)]
//======================================================================
// [CODE]
//======================================================================
// A28/TD-182 (sister A19): full-struct Position reset — the SINGLE source for clearing a slot to defaults.
// The subset-zeroing class recurs when each clear site hand-lists fields (A19 = ratchet_tp never cleared;
// A28 = original_tp/original_sl/pair_index/entry_timestamp_us never cleared → stale trail anchor + mis-paired
// legs on slot reuse). Every reset site (Init/ClearPositions) calls this. pair_index defaults to -1 (unpaired),
// NOT 0. Sets values only — no Position layout change, so no PORTFOLIO_SNAPSHOT_VERSION/H21 concern.
template <unsigned F> inline void Position_Reset(Position<F>* p) {
    // Full-struct clear to defaults, single-sourced off FOREACH_POSITION_FIELD's `init`
    // column (the DMIs) + zeroes _pad_pos (H12: the pad reaches the wire via the blob dump).
    // `{}` is correct HERE because Position is a flat blob-serialized POD — every field is
    // value-initializable and reset-default == construct-default; drift-proof (covers new
    // fields AND the manual pad) + fails-safe. SWITCH to an OMS-style registry-walker +
    // per-field reset ONLY if Position stops being a blob-POD: a field whose reset-value ≠
    // construct-default, a SKIP_RESET (preserve-across-reuse) field, or a non-`{}`-able
    // member (atomic / ring / RAII). (D-295)
    *p = Position<F>{};
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Position_Reset]
//======================================================================
template <unsigned F> inline void Portfolio_Init(Portfolio<F> *portfolio) {
    for (int i = 0; i < 16; i++) Position_Reset(&portfolio->positions[i]);   // A28: full reset (was a 5-field subset → original_*/pair_index/ts stale)
    portfolio->active_bitmap = 0;
}
//======================================================================================================
template <unsigned F> inline int Portfolio_IsFull(const Portfolio<F> *portfolio) {
    return portfolio->active_bitmap == 0xFFFF;
}
//======================================================================================================
template <unsigned F> inline int Portfolio_CountActive(const Portfolio<F> *portfolio) {
    return __builtin_popcount(portfolio->active_bitmap);
}
//======================================================================================================
// find position by entry price - walks active bits, returns index or -1
//======================================================================================================
template <unsigned F> inline int Portfolio_FindByPrice(const Portfolio<F> *portfolio, Money entry_price) {
    uint16_t active = portfolio->active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);
        if (Money_Eq(portfolio->positions[idx].entry_price, entry_price)) {
            return idx;
        }
        active &= active - 1;
    }
    return -1;
}
//======================================================================================================
// add quantity to existing position at index (consolidation)
//======================================================================================================
template <unsigned F> inline void Portfolio_AddQuantity(Portfolio<F> *portfolio, int index, Money quantity) {
    portfolio->positions[index].quantity = Money_Add(portfolio->positions[index].quantity, quantity);
}
//======================================================================
// [FUNCTION]_[Portfolio_AddPositionWithExits]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[add new position with pre-computed exit prices — returns slot index or -1 if full]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int Portfolio_AddPositionWithExits(Portfolio<F> *portfolio, Money quantity, Money entry_price,
                                          Money take_profit_price, Money stop_loss_price,
                                          Money entry_fee = Money_Zero()) {
    if (portfolio->active_bitmap == 0xFFFF) return -1;
    int idx                                       = __builtin_ctz(~portfolio->active_bitmap);
    portfolio->positions[idx].quantity             = quantity;
    portfolio->positions[idx].entry_price          = entry_price;
    portfolio->positions[idx].entry_fee            = entry_fee;
    portfolio->positions[idx].take_profit_price    = take_profit_price;
    portfolio->positions[idx].stop_loss_price      = stop_loss_price;
    portfolio->positions[idx].original_tp          = take_profit_price;   // A28/TD-182: was unset → stale trail anchor on slot reuse (mirror Portfolio_OpenSlot)
    portfolio->positions[idx].original_sl          = stop_loss_price;
    portfolio->positions[idx].entry_timestamp_us   = 0;
    portfolio->positions[idx].pair_index           = -1;
    portfolio->active_bitmap |= (1 << idx);
    return idx;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Portfolio_AddPositionWithExits]
//======================================================================
//======================================================================================================
// legacy add - for backward compatibility with existing tests, no exit prices
//======================================================================================================
template <unsigned F> inline void Portfolio_AddPosition(Portfolio<F> *portfolio, Money quantity, Money entry_price) {
    if (portfolio->active_bitmap == 0xFFFF) return;
    int idx                                       = __builtin_ctz(~portfolio->active_bitmap);
    Position_Reset(&portfolio->positions[idx]);   // A28/F3: full-struct reset SSoT → no stale original_tp/sl/entry_ts/pair_index on slot reuse (was subset-zeroing 5 of 9 fields, sister to Init/Clear/AddPositionWithExits)
    portfolio->positions[idx].quantity             = quantity;
    portfolio->positions[idx].entry_price          = entry_price;
    portfolio->active_bitmap |= (1 << idx);
}
//======================================================================================================
// remove - just clears the bit, data stays in slot for controller to read
//======================================================================================================
template <unsigned F> inline void Portfolio_RemovePosition(Portfolio<F> *portfolio, int index) {
    portfolio->active_bitmap &= ~(1 << index);
}
//------------------------------------------------------------------------------------------------------
// [SECTION]_[per-core sharding slot helpers]
//------------------------------------------------------------------------------------------------------
// per-core sharding (phase 04+) binds each execution core to a fixed portfolio
// slot. slot index == node_id directly. these helpers open and close a slot by
// index instead of the auto-assigning __builtin_ctz path used by the legacy
// hot path. controller core calls these from PortfolioController_OnEvent when
// it processes a TradeEvent from a per-core event ring.
//
// open: writes the position fields and sets the active bit. caller must
//   ensure the slot isn't already active (assertion in debug, undefined in
//   release — controller's job to track per-core state).
//
// close: clears the active bit and returns gross P&L = (exit - entry) * qty.
//   does NOT touch balance or fees — controller's OnEvent does that, this
//   function just snapshots the gross.
//======================================================================================================

//======================================================================
// [STRUCT]_[PositionEntryArgs]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [STRUCTURAL_FIX]]
// [REFERENCE]_[CLASS]_[18]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE shape both live-entry and replay populate — closes the Class-18 mirror between the two paths]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct PositionEntryArgs {
    Money   entry_price;
    Money   quantity;
    Money   take_profit_price;
    Money   stop_loss_price;
    Money   entry_fee          = Money_Zero();
    uint64_t entry_timestamp_us = 0;   // 0 = use clock_gettime(REALTIME); non-zero = preserve
    int      pair_index         = -1;  // -1 = unpaired; >=0 = paired-leg slot index (partial-exit)
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.F.2 — Position entry args struct. Bundles ALL fields that
// Portfolio_OpenSlot writes to a Position so live-entry path + replay path
// populate from the same shape. Closes the Class-18 mirror between:
//   - Live entry: drainer's HandleFill builds args from a TradeEvent
//   - Replay:     Portfolio_FromEventLog builds args from an OrderEvent
//
// Adding a NEW Position field that needs preservation across replay =
// ONE line in this struct + populate at both sites. Eliminates the
// recurring "I forgot to preserve X across replay" bug class — see
// CLAUDE.md item 19 + DESIGN_SPECS/structural-fix-preferred-decision-
// framework.md.
//
// Field semantics:
//   - entry_price/quantity/take_profit_price/stop_loss_price/entry_fee:
//     always populated; no defaults safe (would compute wrong).
//   - entry_timestamp_us = 0 → Portfolio_OpenSlot calls clock_gettime(
//     CLOCK_REALTIME) (live-entry semantics). Non-zero → preserve as-is
//     (replay path: passes OrderEvent.timestamp_us so hold-time survives
//     engine restart).
//   - pair_index = -1 → no pairing (single-position mode or unpaired leg).
//     Non-negative → paired leg index for partial-exit semantics (.F.1.B
//     audit candidate when partial-exit replay needs leg-A/leg-B pairing
//     preservation).
//======================================================================
// [END_STRUCT]_[PositionEntryArgs]
//======================================================================

//======================================================================
// [FUNCTION]_[Portfolio_OpenSlot]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[write position fields + set the active bit for a fixed per-core slot — args-struct primary overload]
//======================================================================
// [CODE]
//======================================================================
// v5.15.5.F.2 — Portfolio_OpenSlot now takes a const-ref PositionEntryArgs.
// Live-entry path + replay path populate identically (struct-shape parity).
// Old multi-arg signature shim (next overload) preserves backward compat
// for existing call sites; deprecated but functional.
template <unsigned F>
inline void Portfolio_OpenSlot(Portfolio<F>* portfolio, int slot,
                                const PositionEntryArgs<F>& args) {
    portfolio->positions[slot].entry_price       = args.entry_price;
    portfolio->positions[slot].quantity          = args.quantity;
    portfolio->positions[slot].entry_fee         = args.entry_fee;
    portfolio->positions[slot].take_profit_price = args.take_profit_price;
    portfolio->positions[slot].stop_loss_price   = args.stop_loss_price;
    portfolio->positions[slot].original_tp       = args.take_profit_price;
    portfolio->positions[slot].original_sl       = args.stop_loss_price;
    portfolio->positions[slot].pair_index        = args.pair_index;
    // v5.11.65 — wall-clock entry timestamp for cross-restart hold tracking.
    // CLOCK_REALTIME (not _MONOTONIC) so the value survives engine restart.
    if (args.entry_timestamp_us != 0) {
        portfolio->positions[slot].entry_timestamp_us = args.entry_timestamp_us;
    } else {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
            portfolio->positions[slot].entry_timestamp_us =
                (uint64_t)ts.tv_sec * 1000000ULL +
                (uint64_t)ts.tv_nsec / 1000ULL;
        } else {
            portfolio->positions[slot].entry_timestamp_us = 0;
        }
    }
    portfolio->active_bitmap |= (uint16_t)(1 << slot);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Portfolio_OpenSlot]
//======================================================================

// v5.15.5.F.2 — Back-compat shim for existing call sites that pass individual
// args. New code should construct a PositionEntryArgs + call the primary
// overload directly. Deprecation path: convert all callers, then delete this.
template <unsigned F>
inline void Portfolio_OpenSlot(Portfolio<F>* portfolio, int slot,
                                Money entry_price, Money quantity,
                                Money take_profit_price, Money stop_loss_price,
                                Money entry_fee = Money_Zero()) {
    PositionEntryArgs<F> args;
    args.entry_price        = entry_price;
    args.quantity           = quantity;
    args.take_profit_price  = take_profit_price;
    args.stop_loss_price    = stop_loss_price;
    args.entry_fee          = entry_fee;
    // entry_timestamp_us + pair_index = struct defaults (0 / -1; live-entry semantics)
    Portfolio_OpenSlot(portfolio, slot, args);
}

//======================================================================
// [FUNCTION]_[Money_FillGross]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [DECIMAL] [CRITICAL]]
// [REFERENCE]_[DECISION]_[D-190]
// [REFERENCE]_[INVARIANT]_[H4]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[THE canonical price-diff gross — 1-mul form, single-sourced so per-core/OMS/replay accounting cannot drift]
//======================================================================
// [CODE]
//======================================================================
// Canonical fill P&L gross — the SINGLE SOURCE so the per-core, OMS-aggregate, and replay
// accounting paths cannot drift (D-190; feedback_single_source_the_computation_not_just_the_mode).
// 1-mul form: round the price DIFFERENCE once, then scale by qty (matches the authoritative OMS
// books). NB round((exit−entry)×qty) != round(exit×qty)−round(entry×qty) under decimal half-even —
// open-coding the 2-mul form at any site (was DrainPostFill :1536) re-introduces a 1-ULP divergence.
inline Money Money_FillGross(Money entry_price, Money exit_price, Money quantity) {
    return Money_Mul(Money_Sub(exit_price, entry_price), quantity);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Money_FillGross]
//======================================================================

//======================================================================
// [FUNCTION]_[Portfolio_CloseSlot]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[clear the slot's active bit + return gross P&L via the Money_FillGross SSoT — balance/fees are the caller's]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline Money Portfolio_CloseSlot(Portfolio<F> *portfolio, int slot, Money exit_price) {
    Money gross = Money_FillGross(portfolio->positions[slot].entry_price, exit_price,
                                  portfolio->positions[slot].quantity);
    portfolio->active_bitmap &= ~(uint16_t)(1 << slot);
    return gross;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Portfolio_CloseSlot]
//======================================================================

template <unsigned F>
inline int Portfolio_SlotActive(const Portfolio<F> *portfolio, int slot) {
    return (portfolio->active_bitmap >> slot) & 1;
}
//======================================================================================================
template <unsigned F> inline void Portfolio_ClearPositions(Portfolio<F> *portfolio) {
    for (int i = 0; i < 16; i++) Position_Reset(&portfolio->positions[i]);   // A28/TD-182: full reset (sister Portfolio_Init)
    portfolio->active_bitmap = 0;
}
//======================================================================================================
template <unsigned F>
inline void Portfolio_UpdatePosition(Portfolio<F> *portfolio, int index, Money new_quantity, Money new_entry_price) {
    portfolio->positions[index].quantity    = new_quantity;
    portfolio->positions[index].entry_price = new_entry_price;
}
//------------------------------------------------------------------------------------------------------
// [SECTION]_[p&l functions]
//------------------------------------------------------------------------------------------------------

//======================================================================
// [FUNCTION]_[Portfolio_ComputePnL]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [DECIMAL]]
// [REFERENCE]_[DECISION]_[D-190]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[unrealized P&L across active positions — the regression signal; marks via the Money_FillGross SSoT]
//======================================================================
// [CODE]
//======================================================================
// unrealized P&L: for each active position, (current_price - entry_price) * quantity
// this is the signal the controller feeds to regression - measures whether current
// gate conditions are producing positions that are making money
template <unsigned F> inline Money Portfolio_ComputePnL(const Portfolio<F> *portfolio, Money current_price) {
    Money total = Money_Zero();
    uint16_t active  = portfolio->active_bitmap;
    while (active) {
        int idx        = __builtin_ctz(active);
        // D-190 single-source: unrealized = the same (mark − entry) × qty gross, via Money_FillGross
        // (valued at current_price instead of an exit fill). Keeps all price-diff gross 1-mul + drift-proof.
        Money pnl  = Money_FillGross(portfolio->positions[idx].entry_price, current_price,
                                     portfolio->positions[idx].quantity);
        total           = Money_Add(total, pnl);
        active &= active - 1;
    }
    return total;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Portfolio_ComputePnL]
//======================================================================
//======================================================================================================
// total portfolio value: sum of current_price * quantity across active positions
//======================================================================================================
template <unsigned F> inline Money Portfolio_ComputeValue(const Portfolio<F> *portfolio, Money current_price) {
    Money total = Money_Zero();
    uint16_t active  = portfolio->active_bitmap;
    while (active) {
        int idx        = __builtin_ctz(active);
        Money val = Money_Mul(current_price, portfolio->positions[idx].quantity);
        total          = Money_Add(total, val);
        active &= active - 1;
    }
    return total;
}
//======================================================================
// [FUNCTION]_[PositionExitGate]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [SUPPORTIVE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[v1 per-tick exit gate — bitmap walk, TP/SL compares, exit-buffer append; sharded path uses ExecutionCore SG instead]
//======================================================================
// [CODE]
//======================================================================
// runs every tick - walks active bitmap, checks each position's TP/SL against current price
// branchless comparisons, writes to exit buffer using count += should_exit pattern
// clears position bit immediately on exit so next tick's gate skips it
template <unsigned F>
inline void PositionExitGate(Portfolio<F> *portfolio, Money current_price, ExitBuffer<F> *exit_buf, uint64_t tick) {
    uint16_t active = portfolio->active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);

        // positive-FPN_Binary comparison; crypto prices always positive so sign check skipped. 16B two's-comp →
        // native .v compares: value-equivalent to the old MSW-to-LSW magnitude compare for non-negative values
        // (== triggers exit, matching the old all-words-equal case). Branchless, replaces the word-loop short-circuit.
        const Money &tp = portfolio->positions[idx].take_profit_price;
        const Money &sl = portfolio->positions[idx].stop_loss_price;
        int hit_tp = (current_price.v >= tp.v);   // price >= TP
        int hit_sl = (current_price.v <= sl.v);   // price <= SL

        // skip positions with no exit prices set (legacy adds, zero TP/SL)
        int has_exits = !Money_IsZero(tp);
        int should_exit = (hit_tp | hit_sl) & has_exits;

        // conditional write: exits are rare (~1/1000 ticks), well-predicted branch
        // saves ~8ns/position vs unconditional 24-byte write every tick
        if (should_exit && exit_buf->count < 16) {
            ExitRecord<F> *rec = &exit_buf->records[exit_buf->count];
            rec->position_index = idx;
            rec->exit_price     = current_price;
            rec->tick            = tick;
            rec->reason          = hit_sl & (!hit_tp); // 0=TP, 1=SL (TP takes priority)
            // snapshot position data BEFORE clearing bitmap — slot may be reused
            rec->entry_price     = portfolio->positions[idx].entry_price;
            rec->quantity        = portfolio->positions[idx].quantity;
            rec->entry_fee       = portfolio->positions[idx].entry_fee;
            rec->pair_index      = portfolio->positions[idx].pair_index;
            exit_buf->count++;
            portfolio->active_bitmap &= ~(1 << idx);
        }

        active &= active - 1;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[PositionExitGate]
//======================================================================

//------------------------------------------------------------------------------------------------------
// [SECTION]_[persistence]
//------------------------------------------------------------------------------------------------------
// binary snapshot of portfolio state - written on slow path, read once at startup
// includes a magic number and version so we dont load garbage or stale formats
// also saves realized P&L and adaptive filter state alongside the portfolio
//======================================================================================================
#define PORTFOLIO_SNAPSHOT_MAGIC 0x4B434954  // "TICK" in little-endian
#define PORTFOLIO_SNAPSHOT_VERSION 7   // Ship-B DECIMAL epoch: money re-encoded 2^64->10^8 at identical 16B layout; v6 (16B binary, H21 tombstone) + earlier version-rejected. Was: // Ship-A 16B FPN_Binary: Position PERSIST 184->128 B; v5 snapshots version-rejected (H21/D-144)

// Ship-B P2 epoch guard (S-4/D-174 #14): Position persists money fields RAW — a 16B->16B decimal
// re-encoding changes the VALUE SEMANTICS at identical layout, so no sizeof/offset assert can see
// it. This trait-keyed tripwire red-builds the flip commit until the version bumps past the
// binary-era 6 (-> 7) in the SAME commit (old snapshots must version-reject, never load misscaled).
// [ASSERT]_[EPOCH_TRIPWIRE]_[is_fp_decimal_v<Position money> => PORTFOLIO_SNAPSHOT_VERSION >= 7]
// [WHY]_[a 16B-to-16B encoding flip is invisible to sizeof/offset asserts — the trait-keyed guard forces the H21 version bump in the same commit]
static_assert(!is_fp_decimal_v<decltype(Position<64>::entry_price)>
                  || PORTFOLIO_SNAPSHOT_VERSION >= 7,
              "Ship-B epoch: Position money fields flipped to decimal — bump "
              "PORTFOLIO_SNAPSHOT_VERSION to 7 (H21 tombstone v6) in THIS commit, or pre-epoch "
              "snapshots load with money misread x1.8e11.");

//======================================================================
// [FUNCTION]_[Portfolio_Save]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [CAPITAL_BEARING] [SLOW_PATH]]
// [REFERENCE]_[INVARIANT]_[[H9] [H21]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[binary portfolio snapshot write — magic + version + bitmap + 128B-per-position PERSIST prefix + money tail]
// ---- the snapshot byte layout (matches the fwrite sequence below) ----
// [WIRE_FIELD]_[magic]_[4 bytes — "TICK" little-endian]
// [WIRE_FIELD]_[version]_[4 bytes — PORTFOLIO_SNAPSHOT_VERSION; mismatch = reject-and-ignore]
// [WIRE_FIELD]_[active_bitmap]_[2 bytes + 2 bytes padding]
// [WIRE_FIELD]_[positions]_[16 x POSITION_PERSIST_BYTES (128B PERSIST prefix per position)]
// [WIRE_FIELD]_[realized_pnl]_[sizeof(Money)]
// [WIRE_FIELD]_[live_offset_pct]_[sizeof(Money)]
// [WIRE_FIELD]_[live_vol_mult]_[sizeof(Money)]
// [WIRE_FIELD]_[live_stddev_mult]_[sizeof(Money)]
// [WIRE_FIELD]_[balance]_[sizeof(Money)]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
static inline int Portfolio_Save(const Portfolio<F> *portfolio, Money realized_pnl,
                                  Money live_offset_pct, Money live_vol_mult,
                                  Money live_stddev_mult, Money balance,
                                  const char *filepath) {
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        fprintf(stderr, "[SNAPSHOT] failed to open %s for writing\n", filepath);
        return 0;
    }

    uint32_t magic   = PORTFOLIO_SNAPSHOT_MAGIC;
    uint32_t version = PORTFOLIO_SNAPSHOT_VERSION;

    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&portfolio->active_bitmap, 2, 1, f);
    uint16_t pad = 0;
    fwrite(&pad, 2, 1, f);
    // v5.15.5.C.4 Phase POS.2 — write only PERSIST prefix (128 bytes per position; Ship-A 16B FPN_Binary, was 184).
    // SKIP_PERSIST fields (exit_fill_price, is_maker) live in the Position struct
    // for cache locality but are NOT in the wire format. Per-position loop with
    // explicit POSITION_PERSIST_BYTES() count defines the PORTFOLIO_SNAPSHOT_VERSION
    // wire layout (v5 184B-per-position snapshots are version-rejected, H21/D-144).
    constexpr size_t pos_persist_bytes = POSITION_PERSIST_BYTES<F>();
    for (int i = 0; i < 16; i++) {
        fwrite(&portfolio->positions[i], pos_persist_bytes, 1, f);
    }
    fwrite(&realized_pnl, sizeof(Money), 1, f);
    fwrite(&live_offset_pct, sizeof(Money), 1, f);
    fwrite(&live_vol_mult, sizeof(Money), 1, f);
    fwrite(&live_stddev_mult, sizeof(Money), 1, f);
    fwrite(&balance, sizeof(Money), 1, f);

    fflush(f);
    fclose(f);
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Portfolio_Save]
//======================================================================

//======================================================================
// [FUNCTION]_[Portfolio_Load]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [CAPITAL_BEARING] [BOOT_TIME]]
// [REFERENCE]_[INVARIANT]_[[H9] [H21]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[snapshot read at startup — magic/version gates reject garbage + stale formats; mirrors Save's byte layout]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
static inline int Portfolio_Load(Portfolio<F> *portfolio, Money *realized_pnl,
                                  Money *live_offset_pct, Money *live_vol_mult,
                                  Money *live_stddev_mult, Money *balance,
                                  const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        // no snapshot file is normal on first run
        return 0;
    }

    uint32_t magic, version;
    if (fread(&magic, 4, 1, f) != 1 || magic != PORTFOLIO_SNAPSHOT_MAGIC) {
        fprintf(stderr, "[SNAPSHOT] bad magic in %s - ignoring\n", filepath);
        fclose(f);
        return 0;
    }
    if (fread(&version, 4, 1, f) != 1 || version != PORTFOLIO_SNAPSHOT_VERSION) {
        fprintf(stderr, "[SNAPSHOT] version mismatch in %s - ignoring\n", filepath);
        fclose(f);
        return 0;
    }

    uint16_t bitmap;
    uint16_t pad;
    if (fread(&bitmap, 2, 1, f) != 1) { fclose(f); return 0; }
    if (fread(&pad, 2, 1, f) != 1) { fclose(f); return 0; }
    // v5.15.5.C.4 Phase POS.2 — read only PERSIST prefix (POSITION_PERSIST_BYTES per position).
    // SKIP_PERSIST fields (exit_fill_price, is_maker) are NOT in wire format;
    // they stay at default-init (zero) after load. Per-position loop matches
    // Save's PERSIST-filtered write; legacy snapshots load byte-identical.
    constexpr size_t pos_persist_bytes = POSITION_PERSIST_BYTES<F>();
    for (int i = 0; i < 16; i++) {
        if (fread(&portfolio->positions[i], pos_persist_bytes, 1, f) != 1) {
            fclose(f);
            return 0;
        }
    }

    portfolio->active_bitmap = bitmap;

    if (fread(realized_pnl, sizeof(Money), 1, f) != 1) { fclose(f); return 0; }
    if (fread(live_offset_pct, sizeof(Money), 1, f) != 1) { fclose(f); return 0; }
    if (fread(live_vol_mult, sizeof(Money), 1, f) != 1) { fclose(f); return 0; }
    if (fread(live_stddev_mult, sizeof(Money), 1, f) != 1) { fclose(f); return 0; }
    if (fread(balance, sizeof(Money), 1, f) != 1) { fclose(f); return 0; }

    fclose(f);

    int count = __builtin_popcount(bitmap);
    fprintf(stderr, "[SNAPSHOT] loaded %d positions from %s\n", count, filepath);
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Portfolio_Load]
//======================================================================

//======================================================================================================
#endif
