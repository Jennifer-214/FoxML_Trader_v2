// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/BarrierGate.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[peak/valley classifier gate (FoxML barrier.py port) — soft gate = (1-p_peak)^gamma * (0.5+0.5*p_valley)^delta, hard block above 0.6]
// [CONTAINS]
//   - [STRUCT]_[BarrierGateResult]
//   - [FUNCTION]_[BarrierGate_Compute]
//======================================================================================================
// blocks entries before local price peaks using two binary classifiers:
//   P(will_peak)   — probability price is about to hit a local high
//   P(will_valley) — probability price is about to hit a local low (good entry)
//
// gate formula: g = max(g_min, (1 - p_peak)^gamma * (0.5 + 0.5*p_valley)^delta)
// hard block when p_peak > hard_block_threshold (default 0.6)
//
// ported from FoxML barrier.py — same formula, same defaults.
// models trained separately via LABEL_WILL_PEAK / LABEL_WILL_VALLEY in foxml_suite.
//======================================================================================================
#ifndef BARRIER_GATE_HPP
#define BARRIER_GATE_HPP

#include <math.h>

// tuning constants (FoxML defaults)
#define BARRIER_G_MIN              0.2    // minimum gate value (never fully block)
#define BARRIER_GAMMA              1.0    // peak penalty exponent
#define BARRIER_DELTA              0.5    // valley bonus exponent
#define BARRIER_HARD_BLOCK         0.6    // p_peak above this = hard block (gate = 0)

//======================================================================
// [STRUCT]_[BarrierGateResult]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the gate result — gate value [0,1] + the two model probabilities (p_peak/p_valley) + the hard-block flag]
//======================================================================
// [CODE]
//======================================================================
struct BarrierGateResult {
    double gate;       // [0, 1] — 0 = blocked, 1 = fully open
    double p_peak;     // model output: probability of imminent peak
    double p_valley;   // model output: probability of imminent valley
    int blocked;       // 1 if hard-blocked (p_peak > threshold)
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[32B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[BarrierGateResult]
//======================================================================

//======================================================================
// [FUNCTION]_[BarrierGate_Compute]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[hard block when p_peak > 0.6, else the soft gate formula floored at g_min]
//======================================================================
// [CODE]
//======================================================================

// compute barrier gate value from peak/valley predictions
static inline BarrierGateResult BarrierGate_Compute(double p_peak, double p_valley) {
    BarrierGateResult r;
    r.p_peak = p_peak;
    r.p_valley = p_valley;

    // hard block: high peak probability = don't enter
    if (p_peak > BARRIER_HARD_BLOCK) {
        r.gate = 0.0;
        r.blocked = 1;
        return r;
    }

    // soft gate: penalize peaks, reward valleys
    double peak_factor = pow(1.0 - p_peak, BARRIER_GAMMA);
    double valley_factor = pow(0.5 + 0.5 * p_valley, BARRIER_DELTA);
    r.gate = peak_factor * valley_factor;
    if (r.gate < BARRIER_G_MIN) r.gate = BARRIER_G_MIN;
    r.blocked = 0;
    return r;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[BarrierGate_Compute]
//======================================================================

#endif // BARRIER_GATE_HPP
