// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [COST MODEL]
//======================================================================================================
// port of FoxML/private LIVE_TRADING/arbitration/cost_model.py.
// estimates trading costs: spread + volatility timing + market impact.
// used to gate unprofitable trades (both in backtest analysis and live buy decisions).
//
// formula: cost = k1*spread + k2*vol*10000*sqrt(h/5) + k3*10*sqrt(participation/0.01)
//
// FoxML constants (from DEFAULT_CONFIG):
//   k1 = 1.0  (spread penalty — we use 0.5 default for crypto, tighter spreads)
//   k2 = 0.15 (volatility timing coefficient)
//   k3 = 1.0  (market impact coefficient)
//
// SHARED: used by both backtest suite (analytics) and live engine (buy gate).
//
// FUTURE HOOKS:
//   multi-symbol: add symbol_id param for per-symbol cost profiles
//     → see ~/FoxML/private/LIVE_TRADING/arbitration/horizon_arbiter.py
//   multi-horizon: cost varies by horizon via sqrt(h/5) term
//     → see ~/FoxML/private/LIVE_TRADING/arbitration/cost_model.py:estimate_all_horizons()
//======================================================================================================
#ifndef COST_MODEL_HPP
#define COST_MODEL_HPP

#include <math.h>

// default coefficients (from FoxML DEFAULT_CONFIG)
// k1 reduced from 1.0 to 0.5 for crypto (tighter spreads than equities)
#define COST_K1_DEFAULT  0.5    // spread penalty
#define COST_K2_DEFAULT  0.15   // volatility timing
#define COST_K3_DEFAULT  1.0    // market impact

struct TradingCosts {
    double spread_cost;     // spread component (bps)
    double timing_cost;     // volatility timing component (bps)
    double impact_cost;     // market impact component (bps)
    double total_cost;      // sum of all costs (bps)
};

//======================================================================================================
// cost = k1*spread + k2*vol*10000*sqrt(h/5) + k3*impact(q, ADV)
//
// parameters:
//   spread_bps:      current bid-ask spread in basis points
//   volatility:      volatility estimate (decimal, e.g. 0.02 = 2% daily vol)
//   horizon_minutes: trade horizon in minutes (default 5)
//   order_size:      order size in dollars (0 = skip impact)
//   adv:             average daily volume in dollars (0 = skip impact)
//   k1, k2, k3:     cost coefficients
//======================================================================================================
static inline TradingCosts CostModel_Estimate(double spread_bps, double volatility,
                                                double horizon_minutes,
                                                double order_size, double adv,
                                                double k1, double k2, double k3) {
    TradingCosts c;

    // spread cost (constant per trade, proportional to spread)
    c.spread_cost = k1 * spread_bps;

    // volatility timing cost (uncertainty of entry/exit over horizon)
    // increases with sqrt of horizon — longer holds have more timing risk
    double h = (horizon_minutes > 0.0) ? horizon_minutes : 5.0;
    c.timing_cost = k2 * volatility * 10000.0 * sqrt(h / 5.0);

    // market impact (square-root model: impact ∝ sqrt(participation))
    // calibrated to ~10 bps at 1% participation (from FoxML)
    c.impact_cost = 0.0;
    if (order_size > 0.0 && adv > 0.0) {
        double participation = order_size / adv;
        c.impact_cost = k3 * 10.0 * sqrt(participation / 0.01);
    }

    c.total_cost = c.spread_cost + c.timing_cost + c.impact_cost;
    return c;
}

// convenience: estimate with default coefficients
static inline TradingCosts CostModel_EstimateDefault(double spread_bps, double volatility,
                                                       double horizon_minutes,
                                                       double order_size, double adv) {
    return CostModel_Estimate(spread_bps, volatility, horizon_minutes,
                               order_size, adv,
                               COST_K1_DEFAULT, COST_K2_DEFAULT, COST_K3_DEFAULT);
}

// breakeven alpha: minimum expected return (decimal) to overcome costs
// cost is in bps, divide by 10000 to get decimal return
static inline double CostModel_Breakeven(double total_cost_bps) {
    return total_cost_bps / 10000.0;
}

// should we trade? returns 1 if expected alpha > breakeven
static inline int CostModel_ShouldTrade(double alpha_decimal, double total_cost_bps) {
    return (alpha_decimal > CostModel_Breakeven(total_cost_bps)) ? 1 : 0;
}

#endif // COST_MODEL_HPP
