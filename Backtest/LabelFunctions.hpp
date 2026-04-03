// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [LABEL FUNCTIONS]
//======================================================================================================
// table-driven target label system for ML training data.
// adding a new label type = 1 function + 1 table entry.
//
// labels are computed by scanning forward through historical tick data from
// the sample point. this requires access to the full tick array (not just
// the current tick), so labels are computed in a post-processing pass
// after the backtest completes.
//
// barrier labels borrowed from FoxML/private target construction logic.
//======================================================================================================
#ifndef LABEL_FUNCTIONS_HPP
#define LABEL_FUNCTIONS_HPP

#include <stdint.h>

// HistoricalTick lives here (single definition point).
// BacktestEngine.hpp includes this file.
struct HistoricalTick {
    double price;
    double qty;
    int64_t timestamp_ms;
    int is_buyer_maker;
};

#define LABEL_WIN_LOSS     0   // 1 = next trade was profitable after fees, 0 = loss
#define LABEL_BARRIER      1   // 1 = price hits +tp% before -sl% (first-passage)
#define LABEL_FORWARD_PNL  2   // continuous: forward return over N ticks (regression target)
#define LABEL_REGIME       3   // regime that was active (multi-class)

//======================================================================================================
// [WIN/LOSS]
// looks forward to the next completed trade — was it profitable?
// simplest label: 1 = win, 0 = loss. uses the existing engine's exit logic.
// note: labels trades, not ticks. many ticks will have label=0 because
// no trade was entered at that point.
//======================================================================================================
static inline float Label_WinLoss(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                                   double sample_price, double tp_pct, double sl_pct,
                                   int /* forward_ticks */) {
    // scan forward: does price hit +tp% before -sl%?
    // this approximates whether a trade entered here would be profitable
    double tp_target = sample_price * (1.0 + tp_pct / 100.0);
    double sl_target = sample_price * (1.0 - sl_pct / 100.0);

    for (int j = tick_idx + 1; j < total_ticks; j++) {
        if (ticks[j].price >= tp_target) return 1.0f;  // hit TP first = win
        if (ticks[j].price <= sl_target) return 0.0f;   // hit SL first = loss
    }
    return 0.0f; // ran out of data = no exit = loss (conservative)
}

//======================================================================================================
// [BARRIER]
// first-passage label from FoxML/private: will price hit +X% before -Y%?
// tp_pct and sl_pct are the barrier sizes (e.g. 1.5 = 1.5%).
// same as win/loss but with configurable asymmetric barriers.
//======================================================================================================
static inline float Label_Barrier(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                                   double sample_price, double tp_pct, double sl_pct,
                                   int /* forward_ticks */) {
    double up_barrier   = sample_price * (1.0 + tp_pct / 100.0);
    double down_barrier = sample_price * (1.0 - sl_pct / 100.0);

    for (int j = tick_idx + 1; j < total_ticks; j++) {
        if (ticks[j].price >= up_barrier)   return 1.0f;  // hit up barrier first
        if (ticks[j].price <= down_barrier) return 0.0f;   // hit down barrier first
    }
    return 0.5f; // neither hit = neutral (useful for 3-class later)
}

//======================================================================================================
// [FORWARD P&L]
// continuous label: return over the next N ticks.
// useful for regression (predict magnitude, not just direction).
//======================================================================================================
static inline float Label_ForwardPnl(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                                      double sample_price, double /* tp_pct */, double /* sl_pct */,
                                      int forward_ticks) {
    int target_idx = tick_idx + forward_ticks;
    if (target_idx >= total_ticks) target_idx = total_ticks - 1;
    if (target_idx <= tick_idx) return 0.0f;

    double future_price = ticks[target_idx].price;
    return (float)((future_price - sample_price) / sample_price * 100.0); // % return
}

//======================================================================================================
// [REGIME]
// which regime was the engine in at this sample point?
// 0 = ranging, 1 = trending, 2 = volatile
// useful for training a regime classifier model.
//======================================================================================================
static inline float Label_Regime(const HistoricalTick * /* ticks */, int /* tick_idx */,
                                  int /* total_ticks */, double /* sample_price */,
                                  double /* tp_pct */, double /* sl_pct */,
                                  int regime_at_sample) {
    return (float)regime_at_sample;
}

//======================================================================================================
// [LABEL TABLE]
// table-driven: add new label = add 1 entry here + 1 function above
//======================================================================================================
typedef float (*LabelFn)(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                           double sample_price, double tp_pct, double sl_pct,
                           int extra_param);

struct LabelDef {
    int id;
    const char *name;
    const char *description;
    LabelFn fn;
};

static const LabelDef label_table[] = {
    { LABEL_WIN_LOSS,    "win_loss",    "Binary: 1=profitable entry, 0=loss",       Label_WinLoss    },
    { LABEL_BARRIER,     "barrier",     "First-passage: +tp% before -sl%",          Label_Barrier    },
    { LABEL_FORWARD_PNL, "forward_pnl", "Continuous: % return over N ticks",        Label_ForwardPnl },
    { LABEL_REGIME,      "regime",      "Multi-class: regime at sample point",      Label_Regime     },
};

static const int LABEL_COUNT = sizeof(label_table) / sizeof(label_table[0]);

#endif // LABEL_FUNCTIONS_HPP
