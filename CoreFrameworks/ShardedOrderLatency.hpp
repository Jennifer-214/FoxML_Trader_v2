// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/ShardedOrderLatency.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[REST submission round-trip metrics — drainer-written atomics the TUI reads live]
// [CONTAINS]
//   - [STRUCT]_[ShardedOrderLatency]
//   - [FUNCTION]_[ShardedOrderLatency_Sample]
//======================================================================================================

#pragma once

#include <atomic>
#include <cstdint>

namespace tt {

//======================================================================
// [STRUCT]_[ShardedOrderLatency]
//----------------------------------------------------------------------
// [TAG]_[[MONITORING_PLANE] [CONCURRENCY] [LIVE_TRADING]]
// [THREAD]_[[DRAINER_WRITER] [GUI_READER]]
// [SYNC]_[ATOMIC]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[running min/avg/max of the venue REST round trip — atomics, cross-thread shared in place]
//======================================================================
// [CODE]
//======================================================================
struct ShardedOrderLatency {
    std::atomic<uint64_t> count;       // total orders submitted
    std::atomic<uint64_t> failures;    // count of REST failures
    std::atomic<uint64_t> total_us;    // running sum for avg
    std::atomic<uint64_t> min_us;
    std::atomic<uint64_t> max_us;
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]_[extraction note + original design note]
//----------------------------------------------------------------------
// Extracted from EngineSharded.hpp during the OMS phase 01 refactor so
// CoreFrameworks/OrderManager.hpp can call Sample without forming a
// circular include with EngineSharded.hpp. The instance still lives in
// EngineSharded.hpp as a file-static; this header only defines the type
// and the helpers. Jennifer's original comment block follows.
//
// drainer-thread metrics for the BinanceOrderAPI submission cost. brackets
// each MarketBuy/MarketSell with steady_clock and accumulates min/avg/max in
// microseconds. NOT a percentile (the order rate is too low to bother), just
// running scalars displayed in the live TUI.
//
// in paper mode this stays at zero. in live mode it shows the network round
// trip to binance, typically 50-200 ms on a US connection. that latency is
// the gap between "executor decided" and "order acknowledged" — separate from
// the per-core ExecutionCore_Tick latency which is unaffected.
//
// atomic because the drainer thread writes and the TUI render loop reads.
// (in phase 02 the writers may be multiple worker threads — the
// compare_exchange_weak loops on min/max already handle that case safely.)
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//----------------------------------------------------------------------
// [SIZE]_[40B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[ShardedOrderLatency]
//======================================================================

// Reset all counters to their initial state. Call once at engine startup
// before the first order can fire.
static inline void ShardedOrderLatency_Reset(ShardedOrderLatency* lat) {
    lat->count.store(0, std::memory_order_relaxed);
    lat->failures.store(0, std::memory_order_relaxed);
    lat->total_us.store(0, std::memory_order_relaxed);
    lat->min_us.store(UINT64_MAX, std::memory_order_relaxed);
    lat->max_us.store(0, std::memory_order_relaxed);
}

//======================================================================
// [FUNCTION]_[ShardedOrderLatency_Sample]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[record one REST round trip — relaxed adds + CAS-loop min/max, multi-writer-safe]
//======================================================================
// [CODE]
//======================================================================
// Record one sample. elapsed_us is the steady_clock duration of the REST
// round trip; success is the BinanceOrderAPI return value (1 = ok, 0 = fail).
// Updates count, failures (if !success), total_us, min_us, max_us. The
// min/max use compare-exchange-weak loops because multiple writers might
// race in a future thread-pool world; safe and cheap with one writer too.
static inline void ShardedOrderLatency_Sample(ShardedOrderLatency* lat,
                                              uint64_t elapsed_us, int success) {
    lat->count.fetch_add(1, std::memory_order_relaxed);
    if (!success) lat->failures.fetch_add(1, std::memory_order_relaxed);
    lat->total_us.fetch_add(elapsed_us, std::memory_order_relaxed);
    uint64_t cur_min = lat->min_us.load(std::memory_order_relaxed);
    while (elapsed_us < cur_min) {
        if (lat->min_us.compare_exchange_weak(
                cur_min, elapsed_us, std::memory_order_relaxed)) break;
    }
    uint64_t cur_max = lat->max_us.load(std::memory_order_relaxed);
    while (elapsed_us > cur_max) {
        if (lat->max_us.compare_exchange_weak(
                cur_max, elapsed_us, std::memory_order_relaxed)) break;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ShardedOrderLatency_Sample]
//======================================================================

}  // namespace tt
