#pragma once
//======================================================================
// [FILE]_[Limits.hpp]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[centralized compile-time bounds SSoT — portfolio/candle/trade-history/data-file/log-buffer sizes, per-core sharding caps (execution nodes + per-node drain budget), OMS in-flight-order + Binance-worker caps; changed here only]
//======================================================================
// centralized compile-time constants
// change HERE only — all files include this for consistent bounds

#define MAX_PORTFOLIO_POSITIONS 16
#define CANDLE_HISTORY_MAX 4096
#define CANDLE_INTERVAL_DEFAULT 60
#define MAX_TRADE_HISTORY 512
#define MAX_DATA_FILES 2048  // multi-year tick datasets — 730 days/yr × 3 yr fits with headroom
                              // FUTURE: dynamic allocation per CLAUDE.md "Dynamic Sizing" rule
#define LOG_BUFFER_SIZE 32768

// per-core sharding (phase 04+)
// MAX_EXECUTION_NODES caps the number of execution cores the controller can
// register. one core per pinned CPU.
//
// ⚠ NODE AND SLOT ARE DIFFERENT INDEX SPACES. This comment used to claim
// "node_id maps directly to portfolio slot" — true ONLY when partial_exit_enabled=0.
// With partials ON a node owns portfolio slots 2N+0 and 2N+1, so the spaces diverge.
// That the two caps below are EQUAL is a coincidence, not an invariant: it is why a
// wrong-space index passes every bounds check silently (Class 61). Derive across the
// spaces via Sharded_SlotNode / Sharded_LegSlot, and prefer the typed tt::SlotIdx /
// tt::NodeIdx (CoreFrameworks/IndexSpaces.hpp, D-438) which make the mix-up a
// compile error.
// MAX_EVENTS_PER_DRAIN_PER_NODE caps how many events one drain pass will pull
// from a single core's event ring before moving to the next core. prevents one
// chatty core from starving the others (pitfall P4.1).
#define MAX_EXECUTION_NODES 16
#define MAX_EVENTS_PER_DRAIN_PER_NODE 16

// OMS (Order Management System, phase 01+)
// MAX_INFLIGHT_ORDERS caps the OrderManager in-flight order table. orders
// occupy a slot from submit until terminal state (FILLED / REJECTED /
// CANCELED / TIMEOUT). pinned at 16 so the bitmap stays uint16_t.
// MAX_BINANCE_WORKERS caps the BinanceAdapter worker thread pool. each
// worker owns its own BinanceOrderAPI instance (per-thread, not shared)
// because BinanceOrderAPI is not thread-safe — see plans/oms/master.md.
#define MAX_INFLIGHT_ORDERS 16
#define MAX_BINANCE_WORKERS 4

// ML flat-SoA tree walker (E.1.2.E leaf 1). Caps sized to the SERVING
// BUDGET, not today's model shape (R4): the walk cost is ~linear in
// nodes visited and the blob should stay L2-resident-ish. 128K nodes
// x 16B = 2MB blob ceiling; per-tree cap 2048 = depth ~10 headroom
// (today's models: depth 2, 7 nodes/tree). Parse REFUSES over-cap
// artifacts loudly — never truncates.
#define WALKER_MAX_TREES        4096
#define WALKER_MAX_NODES_PER_TREE 2048
#define WALKER_MAX_TOTAL_NODES  131072
#define WALKER_MAX_CLASSES      8
