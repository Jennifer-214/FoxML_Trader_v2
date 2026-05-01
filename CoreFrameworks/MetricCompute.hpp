// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [METRIC COMPUTE HELPERS — v5.8.4c]
//======================================================================================================
// Single source of truth for backtest + live performance metrics.
// Called from BacktestStats_Compute / BacktestStats_ComputeFromEquity
// AND from the live TUI/snapshot path (DataStream/EngineTUI.hpp,
// CoreFrameworks/ShardedSnapshot.hpp).
//
// Pre-v5.8.4c each metric was inlined at 3-4 sites with subtle drift:
//   - profit_factor: 4 different zero-guards (epsilon 0.0001 vs 0.001
//     vs none vs -1.0 sentinel)
//   - expectancy: fabs(avg_loss) in BacktestEngine, missing in TUI +
//     ShardedSnapshot
//   - max_drawdown: two independent implementations (post-hoc walk +
//     incremental per-tick)
//
// Extraction reconciles the divergences to single canonical forms.
// Decisions documented in plans/2026-05-01-v5.8-easy-additions.md
// (Phase 5c reconciliation):
//   - profit_factor: epsilon=0.0001 guard, returns 0.0 for no-losses
//     (caller sets all_wins_run=1 separately for display semantics —
//     pre-v5.8.4c's -1.0 sentinel was packed into profit_factor itself
//     and read by OPT_METRIC_PF, conflating display + math).
//   - expectancy: keeps fabs(avg_loss) — defensive against avg_loss
//     invariant break, equally accurate when invariant holds.
//   - max_drawdown: shared inner-update helper called from both
//     post-hoc walk (BacktestStats_ComputeFromEquity) AND incremental
//     per-tick path (BacktestSharded.hpp). Same code path, same FP
//     ops → bytewise identical by construction. No regression test
//     needed since equivalence is structural.
//
// Layering: this header lives in CoreFrameworks/ so both runtime
// (EngineTUI, ShardedSnapshot) and backtest (BacktestEngine,
// BacktestSharded) paths can include it without crossing the runtime
// → backtest-suite layering boundary.
//======================================================================================================
#pragma once

#include <cstdint>
#include <cmath>

static inline double Compute_ProfitFactor(double gross_wins, double gross_losses) {
    return (gross_losses > 0.0001) ? gross_wins / gross_losses : 0.0;
}

// Returns 1 if the run had wins but zero losses (display-only flag —
// numerical profit_factor is 0.0 in this case, separately).
static inline int Compute_AllWinsRun(double gross_wins, double gross_losses) {
    return (gross_wins > 0.0001 && gross_losses <= 0.0001) ? 1 : 0;
}

static inline double Compute_Expectancy(uint32_t total_trades, uint32_t wins,
                                         double avg_win, double avg_loss) {
    if (total_trades == 0) return 0.0;
    double wr = (double)wins / (double)total_trades;
    double lr = 1.0 - wr;
    // avg_loss is invariant-negative (losses are signed money outflows).
    // fabs is defensive-redundant: protects against future sign-flip bugs
    // and clarifies the contract for readers; behavior identical when
    // invariant holds.
    return (wr * avg_win) - (lr * fabs(avg_loss));
}

static inline double Compute_WinRate(uint32_t wins, uint32_t total_trades) {
    return (total_trades > 0)
        ? (double)wins / (double)total_trades * 100.0
        : 0.0;
}

static inline double Compute_AvgHoldTicks(uint64_t total_hold_ticks, uint32_t total_trades) {
    return (total_trades > 0)
        ? (double)total_hold_ticks / (double)total_trades
        : 0.0;
}

static inline double Compute_ReturnPct(double total_pnl, double starting_balance) {
    return (starting_balance > 0.0)
        ? total_pnl / starting_balance * 100.0
        : 0.0;
}

// Shared inner-update for max_drawdown — called from BOTH post-hoc walk
// (BacktestStats_ComputeFromEquity loops it over equity[]) AND incremental
// per-tick (BacktestSharded.hpp invokes once per equity sample). Same
// inputs → same outputs. Bytewise FP identity by construction; no
// regression test needed.
static inline void MaxDrawdown_UpdateIncremental(
    double cur_equity,
    double *peak,
    double *max_dd,
    double *max_dd_pct) {
    if (cur_equity > *peak) *peak = cur_equity;
    double dd = *peak - cur_equity;
    if (dd > *max_dd) *max_dd = dd;
    double dd_pct = (*peak > 0.0) ? dd / *peak : 0.0;
    if (dd_pct > *max_dd_pct) *max_dd_pct = dd_pct;
}

//======================================================================================================
// [BACKTEST METRIC REGISTRY — v5.8.4c X-macro]
//======================================================================================================
// Display-side registry: each row pairs a metric name (for stamps,
// JSONL output, GUI labels) with a printf format string. Adding a new
// metric to display panels is a 1-line append.
//
// IDs are append-only — never reorder. The values match the offset of
// the corresponding field on BacktestStats; tests pin a few for
// stability but most metrics are accessed via name lookup not enum ID,
// so reordering is structurally safer than for SHALT/HALT/REGIME
// (which have persisted integer codes).
//
// Compute helpers above are NOT in this registry — they take varying
// input shapes (equity arrays vs scalar inputs) and aren't dispatched
// uniformly. This registry is for METADATA (name + format), not for
// dispatch.
//
// Row format: X(<id>, <name>, <printf_format>)
//======================================================================================================
#define FOREACH_BACKTEST_METRIC(X) \
    X(SHARPE_RATIO,     "sharpe_ratio",     "%.3f") \
    X(PROFIT_FACTOR,    "profit_factor",    "%.2f") \
    X(EXPECTANCY,       "expectancy",       "$%+.2f") \
    X(MAX_DRAWDOWN,     "max_drawdown",     "$%.2f") \
    X(MAX_DRAWDOWN_PCT, "max_drawdown_pct", "%.2f%%") \
    X(WIN_RATE,         "win_rate",         "%.1f%%") \
    X(RETURN_PCT,       "return_pct",       "%.2f%%") \
    X(AVG_HOLD_TICKS,   "avg_hold_ticks",   "%.0f")

// Auto-generated METRIC_<id> constants. Order matches FOREACH row order.
enum {
#define X(id, name, fmt) METRIC_##id,
    FOREACH_BACKTEST_METRIC(X)
#undef X
    NUM_BACKTEST_METRICS
};

static const char* BACKTEST_METRIC_NAMES[] = {
#define X(id, name, fmt) name,
    FOREACH_BACKTEST_METRIC(X)
#undef X
};

static const char* BACKTEST_METRIC_FORMATS[] = {
#define X(id, name, fmt) fmt,
    FOREACH_BACKTEST_METRIC(X)
#undef X
};

static_assert(sizeof(BACKTEST_METRIC_NAMES) / sizeof(*BACKTEST_METRIC_NAMES) == NUM_BACKTEST_METRICS,
              "BACKTEST_METRIC_NAMES out of sync with NUM_BACKTEST_METRICS");
static_assert(sizeof(BACKTEST_METRIC_FORMATS) / sizeof(*BACKTEST_METRIC_FORMATS) == NUM_BACKTEST_METRICS,
              "BACKTEST_METRIC_FORMATS out of sync with NUM_BACKTEST_METRICS");
