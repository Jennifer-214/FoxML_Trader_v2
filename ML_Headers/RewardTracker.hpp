// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/RewardTracker.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-trade reward ring attributed to strategy arms — DrainCSV appends logging/reward_attribution.csv for offline bandit analysis]
// [CONTAINS]
//   - [STRUCT]_[RewardTracker]         (RewardRecord element shares the block)
//   - [FUNCTION]_[RewardTracker_Push]  (+ Init / DrainCSV share the file)
//======================================================================================================
// ring buffer of per-trade reward records attributed to strategy arms.
// DrainCSV appends to logging/reward_attribution.csv for offline analysis.
// essential for evaluating bandit decisions and strategy performance.
//======================================================================================================
#ifndef REWARD_TRACKER_HPP
#define REWARD_TRACKER_HPP

#include <cstdint>  // v5.15.5.F.4d TECH_DEBT-083 close — explicit IWYU for uintN_t (was transitively pulled)
#include <stdio.h>
#include <time.h>

//======================================================================
// [STRUCT]_[RewardTracker]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[256-slot reward ring + the RewardRecord element (timestamp/strategy/bps/prices/hold/reason)]
//======================================================================
// [CODE]
//======================================================================
#define REWARD_TRACKER_MAX 256

struct RewardRecord {
    time_t timestamp;        // exit time
    int strategy;            // entry_strategy index
    double reward_bps;       // P&L in basis points
    double entry_price;      // fill price
    double exit_price;       // exit price
    uint64_t hold_ticks;     // ticks held
    int exit_reason;         // 0 = TP, 1 = SL, 2 = time, etc.
};

struct RewardTracker {
    RewardRecord records[REWARD_TRACKER_MAX];
    int head;
    int count;
};

//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — layout emitter cannot probe this block yet; quartet lands when the emitter covers it, D-327)
//======================================================================
// [END_STRUCT]_[RewardTracker]
//======================================================================

//======================================================================
// [FUNCTION]_[RewardTracker_Push]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[ring push + the Init/DrainCSV (append + reset) siblings share this section]
//======================================================================
// [CODE]
//======================================================================
static inline void RewardTracker_Init(RewardTracker *rt) {
    rt->head = 0;
    rt->count = 0;
}

static inline void RewardTracker_Push(RewardTracker *rt, int strategy,
                                       double reward_bps, double entry_price,
                                       double exit_price, uint64_t hold_ticks,
                                       int exit_reason) {
    RewardRecord *r = &rt->records[rt->head];
    r->timestamp = time(NULL);
    r->strategy = strategy;
    r->reward_bps = reward_bps;
    r->entry_price = entry_price;
    r->exit_price = exit_price;
    r->hold_ticks = hold_ticks;
    r->exit_reason = exit_reason;
    rt->head = (rt->head + 1) % REWARD_TRACKER_MAX;
    if (rt->count < REWARD_TRACKER_MAX) rt->count++;
}

// append all pending records to CSV, then clear
static inline void RewardTracker_DrainCSV(RewardTracker *rt, const char *path) {
    if (rt->count == 0) return;

    FILE *f = fopen(path, "a");
    if (!f) return;

    // write header if file is empty
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0)
        fprintf(f, "timestamp,strategy,reward_bps,entry_price,exit_price,hold_ticks,exit_reason\n");

    int start = (rt->head - rt->count + REWARD_TRACKER_MAX) % REWARD_TRACKER_MAX;
    for (int i = 0; i < rt->count; i++) {
        int idx = (start + i) % REWARD_TRACKER_MAX;
        const RewardRecord *r = &rt->records[idx];
        fprintf(f, "%ld,%d,%.2f,%.2f,%.2f,%lu,%d\n",
                (long)r->timestamp, r->strategy, r->reward_bps,
                r->entry_price, r->exit_price,
                (unsigned long)r->hold_ticks, r->exit_reason);
    }

    fclose(f);
    rt->count = 0;
    rt->head = 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RewardTracker_Push]
//======================================================================

#endif // REWARD_TRACKER_HPP
