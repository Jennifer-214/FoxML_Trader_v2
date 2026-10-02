// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[Backtest/SuiteStartGates.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the suite's start gates — for each action that starts a suite run, the terms it must pass and the words that say which one failed, as a pure function of plain values and the lease; std-only, so the suite's cells pin every action's gate and a headless verb can refuse with the same policy the panel shows]
// [CONTAINS]
//   - [FUNCTION]_[StartGate_SelectedFiles]
//   - [FUNCTION]_[StartGate_RunBacktest]
//   - [FUNCTION]_[StartGate_GridSearch]
//   - [FUNCTION]_[StartGate_CollectFeatures]
//   - [FUNCTION]_[StartGate_CollectMultiHorizon]
//   - [FUNCTION]_[StartGate_TrainModel]
//   - [FUNCTION]_[StartGate_TrainMultiHorizon]
//   - [FUNCTION]_[StartGate_WalkForward]
//   - [FUNCTION]_[StartGate_HyperparamSweep]
//   - [FUNCTION]_[StartGate_FullValidation]
// [REFERENCE]_[DECISION]_[[D-503] [D-507]]
//======================================================================================================
//
// WHY PURE FUNCTIONS OF PLAIN VALUES (D-507; its review's F2): a gate the panel builds inline can be checked only by
// reading the panel, and the panel needs ImGui, so no test lane compiles it. Here each action's terms — which, in what
// order, at what thresholds, with which words and tone — are one call the cells drive with the lease free and held, and
// the panel keeps only the line that reads its fields into the call. Whether this build can train is an input like any
// other, so both answers run in every lane.
//
// THE ORDER, every gate (SuiteGate's rule): what this build can do, what the operator can fix now, whether the suite is
// free, what a run must produce first — so the reason a button shows is the next thing to do.
//
// The reasons are worded for the suite's panel ("(above)", "Set Model Path first"); a headless verb that reuses these
// gates decides its own wording at E.2's consult.

#pragma once

#include <cstdint>
#include "SuiteLease.hpp"

// Whether this build can train (XGBoost linked). The panel passes it; the cells pass both values.
#ifdef USE_XGBOOST
inline constexpr bool START_GATE_BUILD_TRAINS = true;
#else
inline constexpr bool START_GATE_BUILD_TRAINS = false;
#endif

// The samples each training action needs before its result can mean anything — each reason prints its own floor.
inline constexpr int START_GATE_TRAIN_MIN_SAMPLES = 10;
inline constexpr int START_GATE_WF_MIN_SAMPLES    = 50;
inline constexpr int START_GATE_HP_MIN_SAMPLES    = 100;
inline constexpr int START_GATE_FV_MIN_SAMPLES    = 50;

// Every gate FAILS CLOSED on an input the panel can never produce (a negative count, an unknown verdict): a caller's
// bug — a headless verb's included — refuses, it does not start.

// v5.11.40's broadcast-or-match rule for a per-horizon CSV: one value (or none) broadcasts to every horizon, N values
// map one per horizon; any other count — a negative one included — contradicts the horizons, and only an edit clears it.
inline bool StartGate_BroadcastsOrMatches(int n, int horizons) { return n == 0 || n == 1 || (n > 1 && n == horizons); }

//======================================================================
// [FUNCTION]_[StartGate_SelectedFiles]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the selected files among the files a scan found — the files term's input: only the first file_count flags count, never past capacity]
//======================================================================
// A selection left over from a scan that found more files, or none, cannot count: the Data panel used to keep this as
// a stored count its own recount skipped when a scan found nothing, so the count could open a gate onto zero files
// (D-507 review F7).
//======================================================================
// [CODE]
//======================================================================
inline int StartGate_SelectedFiles(const bool* selected, int file_count, int capacity) {
    if (!selected) return 0;
    const int n = file_count < capacity ? file_count : capacity;
    int count = 0;
    for (int i = 0; i < n; i++)
        count += selected[i] ? 1 : 0;
    return count;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_SelectedFiles]
//======================================================================

// The terms more than one gate shares — each worded once.
inline void StartGate_NeedBuildTrains(SuiteGate* g, bool build_trains) {
    SuiteGate_Need(g, build_trains, "Build with -DUSE_XGBOOST=ON");
}
inline void StartGate_NeedFiles(SuiteGate* g, int selected_files) {
    SuiteGate_Need(g, selected_files > 0, "Select data files first");
}
// side_gate is Training_SideLabelGate's verdict over the label set (0 refuse, 1 warn, 2 ok); the panel says why above.
// Anything but a warn or an ok refuses.
inline void StartGate_NeedSideAccepts(SuiteGate* g, int side_gate) {
    SuiteGate_Need(g, side_gate > 0, "label refused for this training side (above)");
}
inline void StartGate_NeedTrainSamples(SuiteGate* g, int samples) {
    SuiteGate_Need(g, samples >= START_GATE_TRAIN_MIN_SAMPLES, "Collect features first (need %d+ samples)",
                   START_GATE_TRAIN_MIN_SAMPLES);
}
inline void StartGate_NeedAlignedTpSl(SuiteGate* g, int horizons, int tp_n, int sl_n) {
    SuiteGate_NeedFix(g, StartGate_BroadcastsOrMatches(tp_n, horizons) && StartGate_BroadcastsOrMatches(sl_n, horizons),
                      "(misaligned: TP=%d, SL=%d, horizons=%d — need 1 or %d each)", tp_n, sl_n, horizons, horizons);
}

//======================================================================
// [FUNCTION]_[StartGate_RunBacktest]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Run Backtest's gate — the files to replay, then the suite free]
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_RunBacktest(int selected_files) {
    SuiteGate g;
    StartGate_NeedFiles(&g, selected_files);
    SuiteGate_NeedLease(&g);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_RunBacktest]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_GridSearch]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Grid Search's gate — the files, every axis with at least one step, the cells within the grid's cap, then the suite free]
//======================================================================
// Each axis is checked on its own: two empty (inverted) ranges multiply to a positive count, and the sweep would then
// index its parameter values at a negative position. The cells multiply in 64 bits — two large axes overflow an int.
// steps_b is 1 for a one-parameter grid.
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_GridSearch(int selected_files, int steps_a, int steps_b, int max_cells) {
    SuiteGate g;
    StartGate_NeedFiles(&g, selected_files);
    SuiteGate_Need(&g, steps_a >= 1 && steps_b >= 1, "An axis has no steps — check its Min / Max / Step");
    SuiteGate_Need(&g, (int64_t)steps_a * steps_b <= max_cells, "Too many combos (max %d)", max_cells);
    SuiteGate_NeedLease(&g);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_GridSearch]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_CollectFeatures]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Collect Features' gate — the files, the training side's verdict on the label (E.1.2.C F3), then the suite free]
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_CollectFeatures(int selected_files, int side_gate) {
    SuiteGate g;
    StartGate_NeedFiles(&g, selected_files);
    StartGate_NeedSideAccepts(&g, side_gate);
    SuiteGate_NeedLease(&g);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_CollectFeatures]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_CollectMultiHorizon]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Collect Multi-Horizon's gate — the files, the panel's horizons, TP / SL counts that agree with them, the training side's verdict on the label, then the suite free]
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_CollectMultiHorizon(int selected_files, int horizons, int tp_n, int sl_n, int side_gate) {
    SuiteGate g;
    StartGate_NeedFiles(&g, selected_files);
    SuiteGate_Need(&g, horizons > 0, "(type the horizons in Horizons (CSV) to collect)");
    StartGate_NeedAlignedTpSl(&g, horizons, tp_n, sl_n);
    StartGate_NeedSideAccepts(&g, side_gate);
    SuiteGate_NeedLease(&g);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_CollectMultiHorizon]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_TrainModel]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Train Model's gate — a build that trains, the training side's verdict on the label, the suite free, then the samples a collect must produce]
//======================================================================
// E.1.2.C — the side term is the half F3 was missing: the verdict used to reach only the two COLLECT gates, so a
// REFUSE-tier label could still be TRAINED from samples an earlier collect had left behind (collect at side=Buy, flip
// to Exit, pick any label, Train) — the tier rendered red and stopped nothing.
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_TrainModel(bool build_trains, int side_gate, int samples) {
    SuiteGate g;
    StartGate_NeedBuildTrains(&g, build_trains);
    StartGate_NeedSideAccepts(&g, side_gate);
    SuiteGate_NeedLease(&g);
    StartGate_NeedTrainSamples(&g, samples);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_TrainModel]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_TrainMultiHorizon]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Train Multi-Horizon's gate — Train Model's, with the horizons and their TP / SL and Label Kind counts between the side's verdict and the suite]
//======================================================================
// v5.13.1.B — the Label Kind CSV follows the same broadcast-or-match rule as TP / SL; its misalignment used to grey the
// button with no reason shown.
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_TrainMultiHorizon(bool build_trains, int side_gate, int horizons, int tp_n, int sl_n,
                                             int lk_n, int samples) {
    SuiteGate g;
    StartGate_NeedBuildTrains(&g, build_trains);
    StartGate_NeedSideAccepts(&g, side_gate);
    SuiteGate_Need(&g, horizons > 0, "(set Horizons CSV above OR cfg.horizon_list to enable)");
    StartGate_NeedAlignedTpSl(&g, horizons, tp_n, sl_n);
    SuiteGate_NeedFix(&g, StartGate_BroadcastsOrMatches(lk_n, horizons),
                      "(misaligned: Label Kind=%d, horizons=%d — need 1 or %d)", lk_n, horizons, horizons);
    SuiteGate_NeedLease(&g);
    StartGate_NeedTrainSamples(&g, samples);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_TrainMultiHorizon]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_WalkForward]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Run Walk-Forward's gate — a build that trains, the suite free, then the samples]
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_WalkForward(bool build_trains, int samples) {
    SuiteGate g;
    StartGate_NeedBuildTrains(&g, build_trains);
    SuiteGate_NeedLease(&g);
    SuiteGate_Need(&g, samples >= START_GATE_WF_MIN_SAMPLES, "Need %d+ samples", START_GATE_WF_MIN_SAMPLES);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_WalkForward]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_HyperparamSweep]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Run Hyperparam Sweep's gate — a build that trains, every axis with a step and the cells within the cap, the suite free, then the samples]
//======================================================================
// Each axis is checked on its own and the cells multiply in 64 bits, for the reasons StartGate_GridSearch gives.
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_HyperparamSweep(bool build_trains, int steps_a, int steps_b, int max_cells, int samples) {
    SuiteGate g;
    StartGate_NeedBuildTrains(&g, build_trains);
    SuiteGate_Need(&g, steps_a >= 1 && steps_b >= 1 && (int64_t)steps_a * steps_b <= max_cells,
                   "Cell count out of range (1..%d)", max_cells);
    SuiteGate_NeedLease(&g);
    SuiteGate_Need(&g, samples >= START_GATE_HP_MIN_SAMPLES, "Need %d+ samples (Collect Features first)",
                   START_GATE_HP_MIN_SAMPLES);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_HyperparamSweep]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_FullValidation]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Run Full Validation's gate — a build that trains, the model to validate, the suite free, then the samples]
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_FullValidation(bool build_trains, bool has_model_path, int samples) {
    SuiteGate g;
    StartGate_NeedBuildTrains(&g, build_trains);
    SuiteGate_Need(&g, has_model_path, "Set Model Path first");
    SuiteGate_NeedLease(&g);
    SuiteGate_Need(&g, samples >= START_GATE_FV_MIN_SAMPLES, "Need %d+ samples", START_GATE_FV_MIN_SAMPLES);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_FullValidation]
//======================================================================
