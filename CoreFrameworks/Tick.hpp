// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/Tick.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [HOT_PATH] [DATA_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[market-data tick record — producer fan-out -> per-core SPSC tick rings -> hot-path consumers]
// [CONTAINS]
//   - [STRUCT]_[Tick]
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"
#include <cstdint>
#include <type_traits>

namespace tt {

//======================================================================
// [STRUCT]_[Tick]
//----------------------------------------------------------------------
// [TAG]_[[HOT_PATH] [DATA_ORIENTED_DESIGN] [DECIMAL]]
// [THREAD]_[[PRODUCER_WRITER] [HOT_READER]]
// [SYNC]_[SPSC]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one market tick — 64B single-cache-line ring slot; DECIMAL Money price/volume; MARKET-time timestamp]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct alignas(64) Tick {
    Money           price;        // current trade price (DECIMAL money — Ship B P2b)
    Money           volume;       // current trade volume (DECIMAL money — Ship B P2b)
    uint64_t timestamp;    // market time, microseconds since epoch
    uint64_t sequence;     // monotonic sequence number from the exchange feed
    // v4.3 — Binance "m" field. 1 = buyer was the maker (seller aggressed),
    // 0 = buyer was the taker (buyer aggressed). Used by the cumulative
    // delta feature in the model pack. Default 0 = treat as buyer
    // aggression for synthetic paths (test/backtest with no flag).
    uint8_t  is_buyer_maker;
    uint8_t  _pad_v43[7];  // keep alignment / cache-line stability
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Wire format for market data ticks flowing from the market reader through
// per-core SPSC tick rings to execution cores.
//
// Design rules:
//   - Trivially copyable (memcpy-equivalent), enforced by static_assert
//   - alignas(64) so each tick occupies its own cache line (ring slot density vs
//     cache friendliness — padding to 64 is cheap)
//   - Tick.timestamp is in MARKET TIME (microseconds since epoch from the
//     exchange feed), NOT wall clock. This is the canonical time source for the
//     slow-path gate, snapshot timestamps, and CSV trade log timestamps.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[64B]
// [ALIGN]_[64]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[Tick]
//======================================================================

// Validate at compile time that Tick<F> can be used in SPSC rings (which require
// trivially copyable T) and that the cache line alignment is honored.
// [ASSERT]_[LAYOUT_LOCK]_[is_trivially_copyable<Tick<64>>]
// [WHY]_[SPSC ring slots are raw-copied (memcpy-equivalent); a non-trivial member would corrupt ring transport]
static_assert(std::is_trivially_copyable<Tick<64>>::value, "Tick<64> must be trivially copyable");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(Tick<64>) >= 64]
// [WHY]_[each ring slot owns its cache line — producer writes and consumer reads never false-share adjacent slots]
static_assert(alignof(Tick<64>) >= 64, "Tick<64> must be cache-line aligned");

}  // namespace tt
