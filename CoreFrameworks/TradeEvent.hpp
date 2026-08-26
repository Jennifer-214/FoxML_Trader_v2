// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/TradeEvent.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [HOT_PATH] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[trade-event record — hot-path cores -> per-core SPSC event rings -> controller fill handling]
// [CONTAINS]
//   - [STRUCT]_[TradeEvent]
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"
#include <cstdint>
#include <type_traits>

namespace tt {

constexpr uint8_t TRADE_EVENT_ENTRY = 0x01;
constexpr uint8_t TRADE_EVENT_EXIT  = 0x02;

// Partial exits P.2 (2026-04-27): leg index for the two-position-per-core
// model. 0 = leg A (or single position when partial_exit_enabled=0); 1 =
// leg B. Drainer maps to portfolio slot via Sharded_LegSlot(node_id, leg,
// partial_exit_enabled). Lives here because TradeEvent.leg uses these
// values; ControllerEventLoop.hpp re-exports under the same names for
// readability at slow-path call sites.
constexpr int PARTIAL_LEG_A = 0;
constexpr int PARTIAL_LEG_B = 1;

//======================================================================
// [STRUCT]_[TradeEvent]
//----------------------------------------------------------------------
// [TAG]_[[HOT_PATH] [CAPITAL_BEARING] [CONCURRENCY]]
// [THREAD]_[[HOT_WRITER] [SLOW_READER]]
// [SYNC]_[SPSC]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one hot-path trade state-change — ENTRY/EXIT bitmask + leg + fill price; controller drains on its slow path]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct alignas(64) TradeEvent {
    Money           price;        // fill price for entry or exit (DECIMAL money — Ship B P2b)
    uint64_t timestamp;    // market time of the originating tick (microseconds)
    uint16_t node_id;      // which execution NODE fired this event — SINGLE-space
                           // since the D1/Class-61 close (2026-08-26): the trade-log
                           // Record fns take the SLOT as an explicit tt::SlotIdx
                           // param, so nothing smuggles a slot through this field
                           // any more (the OMS synth events used to)
    uint8_t  type;         // bitmask: TRADE_EVENT_ENTRY and/or TRADE_EVENT_EXIT
    // P.2 (partial exits, 2026-04-27): leg index for the two-position-per-
    // core model. 0 = leg A (or single position when partial_exit_enabled=0);
    // 1 = leg B. Drainer maps to portfolio slot via Sharded_LegSlot(node_id,
    // leg, partial_exit_enabled). When partial_exit_enabled=0, leg is always
    // 0 — preserves pre-Wave-1 behavior.
    uint8_t  leg;
    uint8_t  _pad[4];      // explicit padding for layout stability
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Wire format for trade events flowing from execution cores back to the controller
// via per-core SPSC event rings.
//
// One TradeEvent per state change. The controller reads events from each core's
// event ring on its own slow path, runs them through the existing PortfolioController
// fill-handling logic, and updates the canonical Position records.
//
// Type encoding (single byte):
//   bit 0 (0x01) = ENTRY: execution core just opened a position at .price
//   bit 1 (0x02) = EXIT:  execution core just closed a position at .price
//
// Both bits set (0x03) is logically invalid (same-tick entry+exit) — the branchless
// ExecutionCore_Tick computes can_enter and can_exit from mutually exclusive states
// so this should never occur. Controllers SHOULD assert against type=3 to catch
// logic bugs.
//
// Design rules:
//   - Trivially copyable, fits in one cache line, alignas(64)
//   - timestamp is MARKET TIME (from the originating tick), not wall clock. The
//     controller may process this event microseconds or milliseconds after it
//     happened; the timestamp captures when it actually occurred.
//   - node_id identifies which execution core fired the event, used by the
//     controller to look up the canonical Position slot for this trade.
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
// [END_STRUCT]_[TradeEvent]
//======================================================================

// [ASSERT]_[LAYOUT_LOCK]_[is_trivially_copyable<TradeEvent<64>>]
// [WHY]_[SPSC ring slots are raw-copied (memcpy-equivalent); a non-trivial member would corrupt ring transport]
static_assert(std::is_trivially_copyable<TradeEvent<64>>::value, "TradeEvent<64> must be trivially copyable");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(TradeEvent<64>) >= 64]
// [WHY]_[each ring slot owns its cache line — hot-path writes and controller reads never false-share adjacent slots]
static_assert(alignof(TradeEvent<64>) >= 64, "TradeEvent<64> must be cache-line aligned");

}  // namespace tt
