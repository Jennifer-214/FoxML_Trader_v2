// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [TICK]
//
// Wire format for market data ticks flowing from the market reader through
// per-core SPSC tick rings to execution cores.
//
// Design rules:
//   - Trivially copyable (memcpy-equivalent), enforced by static_assert
//   - alignas(64) so each tick occupies its own cache line (ring slot density vs
//     cache friendliness — at 32-48 bytes per tick, padding to 64 is cheap)
//   - Tick.timestamp is in MARKET TIME (microseconds since epoch from the
//     exchange feed), NOT wall clock. This is the canonical time source for the
//     slow-path gate, snapshot timestamps, and CSV trade log timestamps.
//
// FPN<F> price and volume use the engine's existing fixed-point math for
// consistency with the rest of the hot path.
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"
#include <cstdint>
#include <type_traits>

namespace tt {

template <unsigned F>
struct alignas(64) Tick {
    FPN<F>   price;        // current trade price (FPN)
    FPN<F>   volume;       // current trade volume (FPN)
    uint64_t timestamp;    // market time, microseconds since epoch
    uint64_t sequence;     // monotonic sequence number from the exchange feed
    // v4.3 — Binance "m" field. 1 = buyer was the maker (seller aggressed),
    // 0 = buyer was the taker (buyer aggressed). Used by the cumulative
    // delta feature in the model pack. Default 0 = treat as buyer
    // aggression for synthetic paths (test/backtest with no flag).
    uint8_t  is_buyer_maker;
    uint8_t  _pad_v43[7];  // keep alignment / cache-line stability
};

// Validate at compile time that Tick<F> can be used in SPSC rings (which require
// trivially copyable T) and that the cache line alignment is honored.
static_assert(std::is_trivially_copyable<Tick<64>>::value, "Tick<64> must be trivially copyable");
static_assert(alignof(Tick<64>) >= 64, "Tick<64> must be cache-line aligned");

}  // namespace tt
