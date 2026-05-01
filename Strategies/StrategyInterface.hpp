// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [STRATEGY INTERFACE]
//======================================================================================================
// contract for strategy modules. each strategy implements these four functions with its own
// prefix (e.g. MeanReversion_Init, Momentum_Init). the engine dispatches to the active
// strategy on the slow path via strategy_id flag. hot path is unaffected - BuyGate and
// PositionExitGate just read the buy_conds struct, they dont know which strategy set it.
//
// strategy selection:
//   all strategies are compiled into the binary. a strategy_id flag (set on the slow path
//   by config or by a regime detector) determines which strategy's functions get called.
//   zero hot-path cost - the flag is read once per slow-path cycle.
//
// function signatures (replace PREFIX with strategy name, e.g. MeanReversion):
//
//   PREFIX_Init(PREFIXState<F> *state, const RollingStats<F> *rolling,
//               BuySideGateConditions<F> *buy_conds)
//     called once after warmup completes. sets initial buy conditions from rolling stats
//     and resets any internal tracking (regression feeders, etc). writes to buy_conds so
//     the engine can start running BuyGate immediately.
//
//   PREFIX_Adapt(PREFIXState<F> *state, FPN<F> current_price, FPN<F> portfolio_delta,
//               uint16_t active_bitmap, const BuySideGateConditions<F> *buy_conds,
//               const ControllerConfig<F> *cfg)
//     called every slow-path tick. adjusts adaptive filter parameters based on market
//     conditions and P&L feedback. does NOT set buy_conds - that's BuySignal's job.
//     stores regression results internally for BuySignal to apply.
//
//   PREFIX_BuySignal(PREFIXState<F> *state, const RollingStats<F> *rolling,
//                    const ControllerConfig<F> *cfg) -> BuySideGateConditions<F>
//     called every slow-path tick after Adapt. computes and returns buy gate conditions.
//     the engine writes the returned value to ctrl->buy_conds, which BuyGate reads on
//     the hot path. this is where the strategy's core signal logic lives.
//
//   PREFIX_ExitSignal (OPTIONAL - not required for v1)
//     per-position custom exit logic beyond TP/SL. not implemented yet - the default
//     PositionExitGate handles all exits via per-position TP/SL thresholds.
//
// state convention:
//   each strategy defines its own state struct (e.g. MeanReversionState<F>). the engine
//   holds one instance per strategy in the controller struct. the engine never looks inside
//   strategy state - it's opaque to the controller.
//
// call order on slow path:
//   1. engine: drain exits, push rolling stats, compute P&L
//   2. strategy: PREFIX_Adapt (adjust filters, process regression)
//   3. strategy: PREFIX_BuySignal (compute buy conditions from adjusted filters)
//   4. engine: ctrl->buy_conds = result from step 3
//======================================================================================================
#ifndef STRATEGY_INTERFACE_HPP
#define STRATEGY_INTERFACE_HPP

#include <cstdint>
#include <cstddef>

//======================================================================================================
// [STRATEGY REGISTRY — v5.8.0 X-macro]
//======================================================================================================
// Single source of truth for all public strategies. Adding a strategy:
//   1. cp DOCS/STRATEGY_TEMPLATE.hpp Strategies/<Name>.hpp; implement
//      4 lifecycle fns (_Init / _BuildParameters / _Adapt /
//      _ExitAdjustSharded) per DOCS/STRATEGY_INTERFACE.md.
//   2. Append one row to FOREACH_STRATEGY(X) below.
//   3. Add a strategy color to GUI/DashboardPanels.hpp's strat_colors[].
//   4. ./build.sh test
//
// Auto-generated from FOREACH_STRATEGY(X):
//   - STRATEGY_<ID> enum constants (compile-time IDs, contiguous from 0)
//   - NUM_STRATEGIES_REAL (count of registered real strategies)
//   - STRATEGY_SHORT_NAMES[] / STRATEGY_FULL_NAMES[] arrays
// Lifecycle dispatchers in StrategyLifecycle.hpp +
// Strategy_BuildParameters in StrategyParameters.hpp consume the same
// registry.
//
// Canonical signatures (do not deviate; see DOCS/EASY_ADDITIONS_INVARIANTS.md):
//   _Init(state, rolling, buy_conds)
//   _Adapt(state, current_price, portfolio_delta, active_bitmap, buy_conds, cfg)
//   _BuildParameters(rolling, config, allocated_balance, out [, state])
//   _ExitAdjustSharded(state, slot, strat_state, current_price, rolling, cfg)
//
// Drift (v5.8.0 audit):
//   - MLStrategy_Adapt takes `const void* cfg` (include-cycle workaround) →
//     X-macro references MLStrategy_Adapt_Canonical adapter
//   - ML_BuildParameters has wider sig (extra rolling_long + ml_ctx args) →
//     dispatcher uses case-block dispatch in StrategyParameters.hpp
//
// Public/private split: when private/EmaCross.hpp is absent, the
// EMA_CROSS row is omitted from the registry (#__has_include guard).
// STRATEGY_EMA_CROSS enum constant simply doesn't exist in that build —
// callers that reference it fail at compile time, which is the intended
// "private snapshot can't reference private code" behavior.
//======================================================================================================

#if __has_include("private/EmaCross.hpp")
#  define FOREACH_STRATEGY_EMACROSS(X) \
    X(EMA_CROSS, "EMA",  "EmaCross",      EmaCrossState, \
       EmaCross_Init,        EmaCross_BuildParameters, \
       EmaCross_Adapt,       EmaCross_ExitAdjustSharded)
#else
#  define FOREACH_STRATEGY_EMACROSS(X) /* private/EmaCross.hpp absent */
#endif

#define FOREACH_STRATEGY(X) \
    X(MEAN_REVERSION, "MR",   "MeanReversion", MeanReversionState, \
       MeanReversion_Init,   MeanReversion_BuildParameters, \
       MeanReversion_Adapt,  MeanReversion_ExitAdjustSharded) \
    X(MOMENTUM,       "MOM",  "Momentum",      MomentumState, \
       Momentum_Init,        Momentum_BuildParameters, \
       Momentum_Adapt,       Momentum_ExitAdjustSharded) \
    X(SIMPLE_DIP,     "DIP",  "SimpleDip",     SimpleDipState, \
       SimpleDip_Init,       SimpleDip_BuildParameters, \
       SimpleDip_Adapt,      SimpleDip_ExitAdjustSharded) \
    X(ML,             "ML",   "ML",            MLStrategyState, \
       MLStrategy_Init,      ML_BuildParameters, \
       MLStrategy_Adapt_Canonical, MLStrategy_ExitAdjustSharded) \
    FOREACH_STRATEGY_EMACROSS(X)

// IDs — auto-generated. Order matches the historical assignment
// (MR=0, MOM=1, DIP=2, ML=3, EMA=4) so cfg files / stamps survive the
// refactor unchanged.
enum StrategyId : uint8_t {
#define X(id, ...) STRATEGY_##id,
    FOREACH_STRATEGY(X)
#undef X
    NUM_STRATEGIES_REAL,                            // count of registered real strategies
    STRATEGY_AUTO    = NUM_STRATEGIES_REAL,         // v4.0.3 sentinel: regime-driven auto-select
    NUM_STRATEGIES   = NUM_STRATEGIES_REAL + 1,     // includes AUTO
    STRATEGY_NONE    = 0xFF                         // per-core "no strategy assigned"
};

// Names for display, indexed by strategy ID. Real strategies' names
// auto-generate from the X-macro; AUTO appended manually.
#define X(id, short_name, full_name, ...) short_name,
static const char *STRATEGY_SHORT_NAMES[] = {
    FOREACH_STRATEGY(X)
    "AUTO"
};
#undef X

#define X(id, short_name, full_name, ...) full_name,
static const char *STRATEGY_FULL_NAMES[] = {
    FOREACH_STRATEGY(X)
    "Auto-Regime"
};
#undef X

static_assert(sizeof(STRATEGY_SHORT_NAMES) / sizeof(*STRATEGY_SHORT_NAMES) == NUM_STRATEGIES,
              "STRATEGY_SHORT_NAMES out of sync with NUM_STRATEGIES");
static_assert(sizeof(STRATEGY_FULL_NAMES) / sizeof(*STRATEGY_FULL_NAMES) == NUM_STRATEGIES,
              "STRATEGY_FULL_NAMES out of sync with NUM_STRATEGIES");

//======================================================================================================
// [REGIME CONSTANTS]
//======================================================================================================
#define REGIME_RANGING       0
#define REGIME_TRENDING      1  // uptrend — momentum (buy breakouts above)
#define REGIME_VOLATILE      2
#define REGIME_TRENDING_DOWN 3  // downtrend — pause buying (future: short strategy)
#define REGIME_MILD_TREND    4  // mild uptrend — EMA Cross (buy dips in uptrend)
#define NUM_REGIMES          5

// regime info lookup table — single source of truth for display
// adding a regime = one line here, zero display edits elsewhere
struct RegimeInfo { const char *short_name; const char *full_name; };
static const RegimeInfo REGIME_INFO[] = {
    {"RANGE", "RANGING"},        // 0
    {"TREND", "TRENDING"},       // 1
    {"VOLAT", "VOLATILE"},       // 2
    {"TR_DN", "TRENDING_DOWN"},  // 3
    {"EMACR", "MILD_TREND"},     // 4
};

// regime-to-strategy mapping table — branchless lookup
static const int REGIME_STRATEGY_TABLE[] = {
    STRATEGY_MEAN_REVERSION,  // RANGING (0)
    STRATEGY_MOMENTUM,        // TRENDING (1)
    STRATEGY_SIMPLE_DIP,      // VOLATILE (2)
    STRATEGY_MEAN_REVERSION,  // TRENDING_DOWN (3)
    STRATEGY_EMA_CROSS,       // MILD_TREND (4)
};

//======================================================================================================
// [STRATEGY HALT REASON CODES — v5.6.2]
//======================================================================================================
// Distinct from CoreContext::halt_reason (controller-level halts:
// spacing, vwap, long-slope, vol-delta, min-stddev, sl-cooldown,
// warmup, core-budget, core-kill, imbalance — set in
// ControllerEventLoop.hpp:1812-1814).
//
// strategy_halt_reason captures STRATEGY-INTERNAL vetoes — the
// reasons a strategy zero-gates or sets BUY_BLOCKED before the
// controller-level checks run. Each strategy's _BuildParameters
// MUST set strategy_halt_reason before Gate_Zero or BUY_BLOCKED
// is set in `out`. Reset to SHALT_OK at the top of each rebuild.
//
// Display priority in the GUI: controller halt_reason > 0 wins;
// else strategy_halt_reason > 0 wins; else gate_flags & BUY_BLOCKED;
// else "no signal".
//
// v5.8.2 — registry-driven via FOREACH_SHALT(X). Adding a new SHALT
// code is a single-line change here; the GUI mirror in
// DashboardPanels.hpp now reads SHALT_SHORT_NAMES directly. Bound
// assertion in controller_test.cpp uses NUM_SHALT_CODES.
//
// Row format: X(<id>, <short_name>, <description>)
// IDs are append-only; never reorder or remove (existing trade logs
// reference numeric values).
//======================================================================================================
#define FOREACH_SHALT(X) \
    X(OK,               "ok",             "all strategies pass") \
    X(NO_UPTREND,       "no-uptrend",     "EmaCross / MeanReversion long-trend gate") \
    X(NO_MEAN_REV,      "no-mean-rev",    "MeanReversion vwap_ok / long_ok / delta_ok") \
    X(FEE_FLOOR,        "fee-floor",      "dispatcher fee-floor BUY_BLOCKED (TP < 3 * fee_taker)") \
    X(COST_GATE,        "cost-gate",      "dispatcher cost-gate BUY_BLOCKED (CostModel)") \
    X(STDDEV_ZERO,      "stddev-zero",    "any strategy on dead market (rolling.stddev == 0)") \
    X(NO_BREAKOUT,      "no-breakout",    "Momentum: price hasn't broken above threshold") \
    X(ML_NO_PRED,       "ml-no-pred",     "ML: zoo unloaded or no inference output") \
    X(ML_BELOW_THR,     "ml-below-thr",   "ML: prediction below trigger threshold") \
    X(LOW_CONFIDENCE,   "low-confidence", "ConfidenceScorer veto") \
    X(NO_SIGNAL,        "no-signal",      "catch-all for strategy zero-gates without specific code") \
    X(MOM_TP_TOO_TIGHT, "mom:tp-tight",   "momentum_min_tp_margin_pct unmet") \
    X(MOM_NO_FLOW,      "mom:no-flow",    "momentum_min_buy_delta_recent unmet") \
    X(MOM_LOW_R2,       "mom:low-r2",     "momentum_min_r2 unmet") \
    X(MOM_LAST_LOST,    "mom:last-lost",  "momentum_require_last_win + last exit was loss")

// Auto-generated SHALT_<id> constants. Underlying type uint8_t for
// compact storage in TradeEvent / per-core snapshot fields.
enum : uint8_t {
#define X(id, name, desc) SHALT_##id,
    FOREACH_SHALT(X)
#undef X
    NUM_SHALT_CODES
};

// Highest valid code. Kept for back-compat with existing test bounds.
// Equivalent to NUM_SHALT_CODES - 1.
constexpr uint8_t SHALT_MAX = NUM_SHALT_CODES - 1;

// Names for display, indexed by SHALT_*. Single source of truth —
// DashboardPanels.hpp references this directly (no mirror).
static const char* SHALT_SHORT_NAMES[] = {
#define X(id, name, desc) name,
    FOREACH_SHALT(X)
#undef X
};

static_assert(sizeof(SHALT_SHORT_NAMES) / sizeof(*SHALT_SHORT_NAMES) == NUM_SHALT_CODES,
              "SHALT_SHORT_NAMES out of sync with NUM_SHALT_CODES");

//======================================================================================================
// [HALT REASONS — controller-level halt codes — v5.8.3 X-macro registry]
//======================================================================================================
// These are set by the slow-path gate rebuild loop (ControllerEventLoop)
// when a cross-cutting filter (spacing, vwap, vol-delta, book-imbalance,
// etc.) zero-gates a core. SHALT_* (above) is the strategy-internal
// equivalent set by individual strategies.
//
// IDs are append-only — never reorder or remove. Trade logs and per-core
// snapshots persist this value as a raw integer; reordering breaks
// historical decode.
//
// HALT_WARMUP (=7) is reserved-but-unused: warmup state is gated via
// `permission=0` upstream of the zero_gate path (see EngineSharded.hpp
// init), so no halt_reason=7 is ever written by current code. Kept in
// the registry for back-compat with older trade logs that may have
// recorded it before the permission mechanism took over.
//
// Row format: X(<id>, <short_name>, <description>)
//======================================================================================================
#define FOREACH_HALT_REASON(X) \
    X(OK,           "ok",           "all gates pass") \
    X(SPACING,      "spacing",      "proposed entry too close to last entry") \
    X(VWAP,         "vwap",         "entry above VWAP threshold (anti-pump)") \
    X(LONG_SLOPE,   "long-slope",   "long-window slope below floor (downtrend)") \
    X(VOL_DELTA,    "vol-delta",    "buy/sell volume delta below floor (heavy dump)") \
    X(MIN_STDDEV,   "min-stddev",   "rolling stddev below floor (dead market)") \
    X(SL_COOLDOWN,  "sl-cooldown",  "stop-loss cooldown active") \
    X(WARMUP,       "warmup",       "reserved — warmup state via permission=0 instead") \
    X(CORE_BUDGET,  "core-budget",  "core open_notional >= allocated") \
    X(CORE_KILL,    "core-kill",    "per-core kill switch tripped") \
    X(IMBALANCE,    "imbalance",    "book imbalance below threshold (Track E.3)")

// Auto-generated HALT_<id> constants. Underlying type uint8_t — matches
// the existing `halt_reason` field width on EventLoopState::cores[].
enum : uint8_t {
#define X(id, name, desc) HALT_##id,
    FOREACH_HALT_REASON(X)
#undef X
    NUM_HALT_REASONS
};

// Highest valid code. Equivalent to NUM_HALT_REASONS - 1.
constexpr uint8_t HALT_MAX = NUM_HALT_REASONS - 1;

// Names for display, indexed by HALT_*. Single source of truth —
// DashboardPanels.hpp reads this directly (no mirror).
static const char* HALT_NAMES[] = {
#define X(id, name, desc) name,
    FOREACH_HALT_REASON(X)
#undef X
};

static_assert(sizeof(HALT_NAMES) / sizeof(*HALT_NAMES) == NUM_HALT_REASONS,
              "HALT_NAMES out of sync with NUM_HALT_REASONS");

#endif // STRATEGY_INTERFACE_HPP
