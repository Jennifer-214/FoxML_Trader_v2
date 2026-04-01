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

#define STRATEGY_MEAN_REVERSION 0
#define STRATEGY_MOMENTUM      1
#define STRATEGY_SIMPLE_DIP    2
#define STRATEGY_ML            3
#define STRATEGY_EMA_CROSS     4
#define NUM_STRATEGIES         5

// strategy short names for display (indexed by strategy ID)
static const char *STRATEGY_SHORT_NAMES[] = {"MR", "MOM", "DIP", "ML", "EMA"};

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

#endif // STRATEGY_INTERFACE_HPP
