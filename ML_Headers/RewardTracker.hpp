// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [REWARD TRACKER]
//======================================================================================================
// ring buffer of per-trade reward records attributed to strategy arms.
// DrainCSV appends to logging/reward_attribution.csv for offline analysis.
// essential for evaluating bandit decisions and strategy performance.
//======================================================================================================
#ifndef REWARD_TRACKER_HPP
#define REWARD_TRACKER_HPP

#include <stdio.h>
#include <time.h>

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

#endif // REWARD_TRACKER_HPP
