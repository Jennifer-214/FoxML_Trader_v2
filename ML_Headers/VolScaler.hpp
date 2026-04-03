// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [VOL SCALER — VOLATILITY-INVERSE POSITION SIZING]
//======================================================================================================
// port of FoxML/private LIVE_TRADING/sizing/vol_scaling.py.
// converts an alpha signal (expected return) to a position weight by dividing
// by volatility — a Sharpe-ratio-like scaling. higher vol = smaller position.
//
// formula:
//   z = clip(alpha / vol, -z_max, z_max)
//   weight = z * (max_weight / z_max)
//
// FoxML constants (from DEFAULT_CONFIG):
//   z_max = 3.0      (Sharpe cap — clips extreme signals)
//   max_weight = 0.05 (5% max position — for single-symbol, maps to exposure fraction)
//
// SHARED: used by both backtest suite (signal analysis) and live engine (position sizing).
//
// FUTURE HOOKS:
//   multi-symbol: weight = per-symbol, normalize across portfolio
//     → see ~/FoxML/private/LIVE_TRADING/sizing/position_sizer.py
//   turnover management: no-trade band to prevent excessive rebalancing
//     → see ~/FoxML/private/LIVE_TRADING/sizing/turnover.py (no_trade_band = 0.008)
//======================================================================================================
#ifndef VOL_SCALER_HPP
#define VOL_SCALER_HPP

// default parameters (from FoxML DEFAULT_CONFIG)
#define VOL_SCALER_Z_MAX_DEFAULT       3.0    // z-score clipping threshold
#define VOL_SCALER_MAX_WEIGHT_DEFAULT  0.05   // 5% max position weight

// scale alpha signal to position weight via volatility
//
// parameters:
//   alpha:      expected return (decimal, e.g. 0.01 = 1% expected return)
//   volatility: volatility estimate (decimal, e.g. 0.02 = 2%)
//   z_max:      clipping threshold (default 3.0)
//   max_weight: maximum position weight (default 0.05)
//
// returns: target weight in [-max_weight, +max_weight]
//   positive = long, negative = short (we only go long in current engine)
static inline double VolScaler_Size(double alpha, double volatility,
                                     double z_max, double max_weight) {
    if (volatility <= 0.0) return 0.0;
    if (z_max <= 0.0) z_max = VOL_SCALER_Z_MAX_DEFAULT;

    double z = alpha / volatility;

    // clip to [-z_max, z_max]
    if (z > z_max) z = z_max;
    if (z < -z_max) z = -z_max;

    return z * (max_weight / z_max);
}

// convenience: scale with default parameters
static inline double VolScaler_SizeDefault(double alpha, double volatility) {
    return VolScaler_Size(alpha, volatility,
                           VOL_SCALER_Z_MAX_DEFAULT,
                           VOL_SCALER_MAX_WEIGHT_DEFAULT);
}

// inverse: given a weight and volatility, recover the implied alpha
// useful for: "what alpha does this position size imply?"
static inline double VolScaler_InverseAlpha(double weight, double volatility,
                                              double z_max, double max_weight) {
    if (max_weight <= 0.0 || z_max <= 0.0) return 0.0;
    double z = weight * (z_max / max_weight);
    return z * volatility;
}

// raw z-score without clipping (for analytics / display)
static inline double VolScaler_RawZ(double alpha, double volatility) {
    if (volatility <= 0.0) return 0.0;
    return alpha / volatility;
}

#endif // VOL_SCALER_HPP
