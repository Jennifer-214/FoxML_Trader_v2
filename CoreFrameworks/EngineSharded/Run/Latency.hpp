// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [EngineSharded/Run/Latency.hpp — order-latency singleton + per-core latency dump]
//======================================================================================================
// Sub-sub-file of CoreFrameworks/EngineSharded/Run.hpp (split per file-size-split-discipline.md
// at v5.15.5.F.4d.1.B.6 Phase B Step B.4.1; second-tier subfolder pattern to bring Run.hpp
// under the 1,500-line source-header threshold).
//
// Contains:
//   - g_sharded_order_lat — inline ShardedOrderLatency singleton (C++17 inline-variable
//     discipline; sister to Boot.hpp + Async.hpp inline globals). Read by ANSI TUI render
//     loop + EngineSharded_Run final dump + sampled by OrderManager_Tick (via pointer
//     handed to OrderManager_Init).
//   - EngineSharded_DumpLatency<F> — per-core latency table dump (used at engine shutdown).
//     Template; takes ExecutionCore<F>* + count + TSC GHz.
//
// **Atomic extraction unit (Phase B Step B.4.1 Decision):** g_sharded_order_lat +
// DumpLatency live together because the global is read by DumpLatency's sister code at
// shutdown (final order-latency dump in Run.hpp body) and the type they share lives in
// ShardedOrderLatency.hpp. Co-location simplifies cold-pickup ("where do shutdown
// latency dumps live?" → one file).
//
// **Hot-path discipline:** g_sharded_order_lat is sampled by OrderManager_Tick — a
// drainer-thread function, NOT hot path. DumpLatency runs at engine shutdown only.
// Header location has no latency impact.
//
// **C++17 inline-variable discipline:** g_sharded_order_lat declared `inline` (not
// `static`) for single shared storage across all TUs that include this header. CRITICAL:
// do NOT refactor back to `static` — that would give each TU its own private copy and
// the OrderManager_Init pointer wire-up + ANSI TUI read + shutdown dump would see
// DIFFERENT instances. Sister to Boot.hpp pattern per NEW DESIGN_SPEC
// cpp17-inline-variable-for-header-shared-state.md.
//======================================================================================================

#pragma once

#include <cstdio>

#include "../../ShardedOrderLatency.hpp"  // ShardedOrderLatency type + _Reset / _Sample
#include "../../ExecutionCore.hpp"        // ExecutionCore<F>
#include "../../CoreLatencyStats.hpp"     // CoreLatencyStats_Snapshot + CoreLatencySnapshot

// parent_index: CoreFrameworks/EngineSharded/Run.hpp

namespace tt {

//======================================================================================================
// [ORDER LATENCY STATS — file-shared inline singleton]
//======================================================================================================
// the type and helper functions live in CoreFrameworks/ShardedOrderLatency.hpp
// (extracted during the OMS phase 01 refactor so OrderManager.hpp can call
// Sample without circular includes). this file just owns the singleton instance
// the TUI render loop reads from. OrderManager_Init takes a pointer to it so
// the OMS can sample each REST round trip into the same counters the TUI
// already displays.
//
// v5.15.5.F.4d.1.B.6: converted from `static` to `inline` per C++17 inline-variable
// discipline (Decision C; sister to Boot.hpp pattern). Moved from EngineSharded.hpp
// to Run.hpp at Phase B Step B.4, then to Run/Latency.hpp at Phase B Step B.4.1 —
// co-located with DumpLatency + EngineSharded_Run final dump that reads it.
//======================================================================================================
inline ShardedOrderLatency g_sharded_order_lat;

//======================================================================================================
// [LATENCY DUMP]
//======================================================================================================
// Dumps per-core latency stats in a compact table after the run finishes.
// One row per core, all converted to ns via the calibrated TSC frequency.
//======================================================================================================
template <unsigned F>
static inline void EngineSharded_DumpLatency(const ExecutionCore<F>* cores,
                                              int num_cores, double tsc_ghz) {
    fprintf(stderr, "\n");
    fprintf(stderr, "================================================================\n");
    fprintf(stderr, "[sharded] PER-CORE LATENCY (samples are p-stats from 256 most recent ticks)\n");
    fprintf(stderr, "================================================================\n");
    fprintf(stderr, "core   samples       min        p50        p95        p99        max        avg\n");
    fprintf(stderr, "----   --------   --------   --------   --------   --------   --------   --------\n");
    for (int i = 0; i < num_cores; ++i) {
        CoreLatencySnapshot s = CoreLatencyStats_Snapshot(&cores[i].latency_stats, tsc_ghz);
        if (s.total_count == 0) {
            fprintf(stderr, " %2d        0     -          -          -          -          -          -\n", i);
            continue;
        }
        fprintf(stderr, " %2d   %8lu   %6.0f ns   %6.0f ns   %6.0f ns   %6.0f ns   %6.0f ns   %6.0f ns\n",
                i, (unsigned long)s.total_count,
                s.min_ns, s.p50_ns, s.p95_ns, s.p99_ns, s.max_ns, s.avg_ns);
    }
    fprintf(stderr, "================================================================\n");
    fprintf(stderr, "Note: rdtsc bracketing has a ~25-30ns floor on this CPU.\n");
    fprintf(stderr, "Subtract the floor from min/p50 for actual hot-path work cost.\n");
    fprintf(stderr, "Max outliers are usually kernel preemption (try chrt -f 90 +\n");
    fprintf(stderr, "isolcpus on a real production box for cleaner tails).\n");
    fprintf(stderr, "================================================================\n");
}

} // namespace tt
