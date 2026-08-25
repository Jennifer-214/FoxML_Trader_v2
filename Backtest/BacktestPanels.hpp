// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================
// [FILE]_[Backtest/BacktestPanels.hpp]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the foxml_suite backtest GUI — Data Browser, Run Control, Results, Comparison, Past Runs, Optimizer, and the big Training panel (WF / held-out / multi-horizon train+stamp); each panel = a state struct + worker threads + an ImGui render fn, and the GUI only ever reads display structs, never calls engine fns directly]
//======================================================================
// follows the panel pattern from DashboardPanels.hpp:
//   - each panel is a standalone ImGui window (dockable, rearrangeable)
//   - state structs are separate from render functions
//   - GUI never calls engine functions directly (reads display structs only)
//   - long-running work (backtest / WF / training) runs on a pthread worker;
//     the render fn reads a thread-safe snapshot when the worker finishes
//======================================================================
#ifndef BACKTEST_PANELS_HPP
#define BACKTEST_PANELS_HPP

#include "imgui.h"
#include "BacktestEngine.hpp"
#include "BacktestSharded.hpp"  // phase 13: per-core sharded backtest path
#include "../ML_Headers/ModelPathSchema.hpp"  // D-431 nested layout — the path-grammar SSoT
#include "../MemHeaders/DirCreate.hpp"        // D-431 — FoxDir_CreateParents (family+horizon chain)
#include "Fingerprint.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <ftw.h>          // v5.11.51 — nftw() for recursive directory delete
#include <unistd.h>       // v5.15.5 — fork() / execlp() / _exit() for Open Folder Path
#include <omp.h>          // v5.11.44 hotfix — omp_set_num_threads(1) in
                          // per-horizon parallel workers to cap libgomp's
                          // thread pool. Without this, multiple pthreads
                          // each running XGBoost (which uses libgomp
                          // internally) cause OpenMP team collisions in
                          // RowsWiseBuildHistKernel → segfault.

// scan cap for the Data panel — must be ≥ MAX_DATA_FILES so the GUI doesn't
// silently truncate before the run_config buffer fills. paired with Limits.hpp.
#define DATA_MAX_FILES 2048

//======================================================================
// [STRUCT]_[DataPanelState]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[state for the Data Browser panel — the recursive CSV scan results + per-file selection]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
struct DataPanelState {
    char data_dir[256];
    // discovered files
    char files[DATA_MAX_FILES][256];
    int file_count;
    // selection
    bool selected[DATA_MAX_FILES];
    int selected_count;
    // scan state
    bool scanned;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[526604B]
// [ALIGN]_[4]
// [CACHE_LINES]_[8229]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[DataPanelState]
//======================================================================

//======================================================================
// [FUNCTION]_[DataPanel_Init]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[init the Data Browser state with the default data dir]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void DataPanel_Init(DataPanelState *state) {
    memset(state, 0, sizeof(*state));
    strncpy(state->data_dir, "data/", sizeof(state->data_dir) - 1);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[DataPanel_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[DataPanel_Scan]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[recursively scan data_dir for .csv files, filename-sorted (chronological for YYYY-MM-DD)]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void DataPanel_Scan(DataPanelState *state) {
    state->file_count = 0;
    state->scanned = true;

    // scan data_dir recursively for .csv files
    DIR *dir = opendir(state->data_dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && state->file_count < DATA_MAX_FILES) {
        // check subdirectories (data/BTCUSDT/*.csv)
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
            char subdir[512];
            snprintf(subdir, sizeof(subdir), "%s%s/", state->data_dir, entry->d_name);
            DIR *sub = opendir(subdir);
            if (!sub) continue;
            struct dirent *subentry;
            while ((subentry = readdir(sub)) != NULL && state->file_count < DATA_MAX_FILES) {
                int len = strlen(subentry->d_name);
                if (len > 4 && strcmp(subentry->d_name + len - 4, ".csv") == 0) {
                    snprintf(state->files[state->file_count], 256, "%s%s",
                             subdir, subentry->d_name);
                    state->file_count++;
                }
            }
            closedir(sub);
        }
        // also check .csv files directly in data_dir
        int len = strlen(entry->d_name);
        if (len > 4 && strcmp(entry->d_name + len - 4, ".csv") == 0) {
            snprintf(state->files[state->file_count], 256, "%s%s",
                     state->data_dir, entry->d_name);
            state->file_count++;
        }
    }
    closedir(dir);

    // sort by filename (chronological for YYYY-MM-DD names)
    for (int i = 0; i < state->file_count - 1; i++)
        for (int j = i + 1; j < state->file_count; j++)
            if (strcmp(state->files[i], state->files[j]) > 0) {
                char tmp[256];
                memcpy(tmp, state->files[i], 256);
                memcpy(state->files[i], state->files[j], 256);
                memcpy(state->files[j], tmp, 256);
            }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[DataPanel_Scan]
//======================================================================

//======================================================================
// [STRUCT]_[SamplesSnapshot]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[thread-safe label-distribution display struct — the worker writes it once post-run, the GUI reads it when running==0 (kills the labels[] realloc-race)]
//======================================================================
// Worker thread writes to this ONCE at end of Backtest_Run (after the label
// post-pass populates results->labels[]). GUI thread reads from this when
// rendering — never iterates results->labels[] directly, eliminating the
// realloc-race that crashed the suite on 2026-04-25.
//
// Thread safety: worker writes all fields, then sets running=0 last.
// GUI reads only when running==0. The volatile running flag prevents
// compiler reordering of the loads/stores around it on x86.
//
// All three label-kind branches (binary/multiclass/regression) populate
// the appropriate subset; the rest stay zero. label_kind tells the GUI
// which subset to display.
//======================================================================
// [CODE]
//======================================================================
struct SamplesSnapshot {
    int sample_count;        // 0 = no completed run yet
    int label_type;          // LABEL_* id used during the run
    int label_kind;          // 0 = binary, 1 = regression, 2 = multiclass
    int num_classes;         // ≥2 for multiclass; 0 otherwise

    // binary
    int pos_count;
    int neg_count;
    int neutral_count;

    // multiclass — class_counts[c] = number of samples in class c
    int class_counts[16];

    // regression
    float lmin;
    float lmax;
    float lmean;
    float lstddev;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[108B]
// [ALIGN]_[4]
// [CACHE_LINES]_[2]
// [STRADDLE]_[class_counts@28]
//======================================================================
// [END_STRUCT]_[SamplesSnapshot]
//======================================================================

//======================================================================
// [STRUCT]_[RunControlState]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [THREAD]_[[RUN_WORKER_WRITER] [GUI_READER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[state for the Run Control panel — the worker thread, run config + results, snapshot, and candle feed]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
struct RunControlState {
    volatile int running;
    volatile int progress_pct;
    volatile int cancel_flag;
    volatile int complete;
    pthread_t worker_tid;
    BacktestRunConfig run_config;
    BacktestResults results;
    CandleAccumulator *candle_acc;
    TUISnapshot *snapshot;       // populated by worker after run completes
    SamplesSnapshot stats_snapshot; // distribution stats — see comment above struct
    char config_path[256];
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-20]
// [SIZE]_[631616B]
// [ALIGN]_[64]
// [CACHE_LINES]_[9869]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[RunControlState]
//======================================================================

//======================================================================
// [FUNCTION]_[RunControl_Init]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[init Run Control state + allocate the BacktestResults buffers]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void RunControl_Init(RunControlState *state) {
    memset(state, 0, sizeof(*state));
    strncpy(state->config_path, "backtest.cfg", sizeof(state->config_path) - 1);
    BacktestResults_Init(&state->results);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RunControl_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[SamplesSnapshot_Compute]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[compute the kind-aware label distribution into a SamplesSnapshot — worker-thread only, after labels are populated and before running=0]
//======================================================================
// Compute distribution stats from results->labels[] into a SamplesSnapshot.
// MUST only be called when no other thread is writing to results->labels —
// i.e. by the worker thread AFTER Backtest_Run has populated labels in the
// post-pass, BEFORE running=0 is set. The GUI thread reads the snapshot
// only when running==0, giving a safe happens-before relationship.
//======================================================================
// [CODE]
//======================================================================
static inline void SamplesSnapshot_Compute(SamplesSnapshot *snap,
                                             const BacktestResults *r,
                                             int label_type) {
    memset(snap, 0, sizeof(*snap));
    snap->label_type = label_type;
    int K = LabelType_NumClasses(label_type);
    snap->num_classes = K;
    snap->label_kind  = (K == 0) ? 0 : (K == 1 ? 1 : 2);

    if (r->sample_count <= 0 || !r->labels) return;
    snap->sample_count = r->sample_count;

    if (snap->label_kind == 1) {
        // regression: range / mean / σ
        float lmin = r->labels[0], lmax = r->labels[0];
        double sum = 0.0, sum_sq = 0.0;
        for (int i = 0; i < r->sample_count; i++) {
            float v = r->labels[i];
            sum += v; sum_sq += (double)v * v;
            if (v < lmin) lmin = v;
            if (v > lmax) lmax = v;
        }
        double mean = sum / r->sample_count;
        double var  = (sum_sq / r->sample_count) - mean * mean;
        snap->lmin    = lmin;
        snap->lmax    = lmax;
        snap->lmean   = (float)mean;
        snap->lstddev = (var > 0.0) ? (float)sqrt(var) : 0.0f;
    } else if (snap->label_kind == 2) {
        // multiclass: per-class histogram
        int Kc = K > 16 ? 16 : K;
        for (int i = 0; i < r->sample_count; i++) {
            int c = (int)(r->labels[i] + 0.5f);
            if (c >= 0 && c < Kc) snap->class_counts[c]++;
        }
    } else {
        // binary: +/-/neutral
        for (int i = 0; i < r->sample_count; i++) {
            float v = r->labels[i];
            if (v > 0.5f) snap->pos_count++;
            else if (v < 0.5f) snap->neg_count++;
            else snap->neutral_count++;
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SamplesSnapshot_Compute]
//======================================================================

// worker thread function
struct BacktestWorkerArgs {
    RunControlState *state;
};

//======================================================================
// [FUNCTION]_[backtest_worker_fn]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[background thread: run a backtest, then compute the samples snapshot]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void *backtest_worker_fn(void *arg) {
    BacktestWorkerArgs *args = (BacktestWorkerArgs *)arg;
    RunControlState *state = args->state;
    free(args);

    Backtest_Run(&state->results, &state->run_config,
                 &state->progress_pct, &state->cancel_flag,
                 state->candle_acc, state->snapshot);

    // Compute display snapshot after labels are populated by Backtest_Run's
    // post-pass. Done BEFORE running=0 so the GUI never sees a stale
    // snapshot when it next reads (running=0 is the happens-before edge).
    SamplesSnapshot_Compute(&state->stats_snapshot, &state->results,
                              state->run_config.label_type);

    state->complete = 1;
    state->running = 0;
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[backtest_worker_fn]
//======================================================================

// v5.11.24 — multi-horizon Collect Features. Mirrors Train Multi-Horizon's
// pattern: snap horizons at click time, collect features ONCE, then loop
// recomputing labels per horizon and fprintf'ing valid-sample counts to
// stderr (engine.log → operator's LogViewer panel).
//
// Final state: results->labels[] contains the LAST horizon's labels.
// Operator who wants per-horizon training next clicks Train Multi-Horizon
// which recomputes labels per horizon during training (no data loss).
//
// The point of this button isn't per-horizon label persistence (that's
// what Train Multi-Horizon does) — it's giving operator a quick way to
// see label class distribution for each candidate horizon BEFORE
// committing to a multi-horizon train run.
//======================================================================
// [STRUCT]_[CollectMultiHorizonWorkerArgs]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[worker-thread args for the multi-horizon label-collect job — run_control + the snapped horizon list + parallel per-horizon TP/SL barrier arrays]
//======================================================================
// [CODE]
//======================================================================
struct CollectMultiHorizonWorkerArgs {
    RunControlState *run_control;
    int snap_horizon_count;
    int snap_horizons[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    // v5.11.40 — per-horizon TP/SL. Snap-time arrays parallel to
    // snap_horizons[]. snap_tp_pct[h] is the TP barrier for horizon h.
    // For broadcast (single-value) mode, the click handler fills all
    // entries with the same value. Arrays are always horizon_count
    // wide; aligned 1:1 with snap_horizons.
    float snap_tp_pct[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    float snap_sl_pct[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-22]
// [SIZE]_[112B]
// [ALIGN]_[8]
// [CACHE_LINES]_[2]
// [STRADDLE]_[snap_tp_pct@44]
//======================================================================
// [END_STRUCT]_[CollectMultiHorizonWorkerArgs]
//======================================================================

//======================================================================
// [FUNCTION]_[collect_multi_horizon_worker_fn]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[background thread: collect features once for a multi-horizon training run]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void *collect_multi_horizon_worker_fn(void *arg) {
    auto *args = (CollectMultiHorizonWorkerArgs *)arg;
    RunControlState *rc = args->run_control;
    int horizon_count = args->snap_horizon_count;
    int horizons[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    float tp_pcts[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    float sl_pcts[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    memcpy(horizons, args->snap_horizons, sizeof(horizons));
    memcpy(tp_pcts,  args->snap_tp_pct,   sizeof(tp_pcts));
    memcpy(sl_pcts,  args->snap_sl_pct,   sizeof(sl_pcts));
    free(args);

    // 1. Collect features ONCE. label_forward_ticks at this point is
    //    whatever was set when the button was clicked — we'll overwrite
    //    labels per horizon afterwards.
    Backtest_Run(&rc->results, &rc->run_config,
                  &rc->progress_pct, &rc->cancel_flag,
                  rc->candle_acc, rc->snapshot);

    // 2. Per-horizon label diagnostic — ONE batched walk (E.1.2.D leaf 5),
    //    was one full-corpus walk PER horizon. All targets share the
    //    collect-time label_type (exactly what the old loop did — it only
    //    rotated fwd/tp/sl); the LAST target writes rc->results.labels
    //    directly so the post-loop state (SamplesSnapshot below reads it)
    //    is bytewise what the old last iteration left behind. rc->run_config
    //    label fields are no longer mutated, so the old save/restore is gone
    //    with the mutation itself.
    // v5.11.40 — label_tp_pct/label_sl_pct rotate per horizon (broadcast or
    //            per-horizon CSV from operator); double-typed, percent
    //            pass-through without /100.
    if (!rc->cancel_flag && horizon_count > 0) {
        LabelBatchTarget bt[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
        float *tmp_bufs[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX] = {0};
        int bt_ok = 1;
        for (int h = 0; h < horizon_count; ++h) {
            bt[h] = LabelBatchTarget{};
            bt[h].label_type    = rc->run_config.label_type;
            bt[h].tp_pct        = (double)tp_pcts[h];
            bt[h].sl_pct        = (double)sl_pcts[h];
            bt[h].forward_ticks = horizons[h];
            if (h == horizon_count - 1) {
                bt[h].out_labels = rc->results.labels;   // post-state = last horizon
            } else {
                tmp_bufs[h] = (float *)malloc(
                    (size_t)rc->results.sample_count * sizeof(float));
                if (!tmp_bufs[h]) { bt_ok = 0; break; }
                bt[h].out_labels = tmp_bufs[h];
            }
        }
        if (!bt_ok) {
            // Pathological small-alloc failure: keep the post-state contract
            // (last horizon into results.labels) and drop the earlier
            // horizons' diagnostics rather than the whole collect.
            fprintf(stderr, "[collect-mh] batch buffer alloc failed; "
                            "labeling last horizon only\n");
            Backtest_ComputeLabelsBatch(&rc->results, &rc->run_config,
                                        &bt[horizon_count - 1], 1);
        } else {
            Backtest_ComputeLabelsBatch(&rc->results, &rc->run_config,
                                        bt, horizon_count);
        }
        // Legacy accumulate-semantics: the old loop's every per-horizon walk
        // folded its NaN counters into results.stats. Same totals, one fold.
        for (int h = 0; h < horizon_count; ++h) {
            if (!bt_ok && h < horizon_count - 1) continue;  // never computed
            rc->results.stats.nan_labels_total   += bt[h].nan_total;
            rc->results.stats.nan_labels_dropped += bt[h].nan_dropped;
        }
        for (int h = 0; h < horizon_count; ++h) {
            if (rc->cancel_flag) {
                fprintf(stderr, "[collect-mh] cancelled at horizon %d/%d\n",
                        h, horizon_count);
                break;
            }
            if (!bt_ok && h < horizon_count - 1) continue;  // no buffer to read
            const float *labs = bt[h].out_labels;
            int n_valid = 0, n_pos = 0, n_neg = 0;
            int n_stable = 0, n_peak = 0, n_valley = 0;   // 3-class (PVS) view
            for (int s = 0; s < rc->results.sample_count; ++s) {
                float lab = labs[s];
                if (isnan(lab) || isinf(lab)) continue;
                n_valid++;
                if (lab > 0.5f) n_pos++;
                else if (lab < 0.5f) n_neg++;
                if      (lab < 0.5f) n_stable++;
                else if (lab < 1.5f) n_peak++;
                else                 n_valley++;
            }
            // E.1.2.C GUI polish (c) — the binary pos/neg split lumped peak(1)
            // + valley(2) together as "pos" and printed stable as "neg" for the
            // 3-class PVS label; print per-class counts for that kind instead.
            if (rc->run_config.label_type == LABEL_PEAK_VALLEY_STABLE) {
                fprintf(stderr, "[collect-mh] horizon=%d ticks tp=%.3f%% sl=%.3f%%: "
                                "%d valid samples (%d stable, %d peak, %d valley) of %d total\n",
                        horizons[h], tp_pcts[h], sl_pcts[h], n_valid, n_stable,
                        n_peak, n_valley, rc->results.sample_count);
            } else {
                fprintf(stderr, "[collect-mh] horizon=%d ticks tp=%.3f%% sl=%.3f%%: "
                                "%d valid samples (%d pos, %d neg, %d neutral) of %d total\n",
                        horizons[h], tp_pcts[h], sl_pcts[h], n_valid, n_pos, n_neg,
                        n_valid - n_pos - n_neg, rc->results.sample_count);
            }
        }
        for (int h = 0; h < horizon_count; ++h) free(tmp_bufs[h]);
    } else if (rc->cancel_flag) {
        fprintf(stderr, "[collect-mh] cancelled at horizon 0/%d\n", horizon_count);
    }

    // 3. Final SamplesSnapshot from whatever the last horizon's labels are
    //    (operator's "current" view — Train Multi-Horizon will recompute
    //    per-horizon during training so this just reflects the last loop
    //    iteration's distribution).
    SamplesSnapshot_Compute(&rc->stats_snapshot, &rc->results,
                              rc->run_config.label_type);

    rc->complete = 1;
    rc->running = 0;
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[collect_multi_horizon_worker_fn]
//======================================================================

//======================================================================
// [FUNCTION]_[RunControl_Start]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[spawn the backtest worker thread for the selected files + config]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void RunControl_Start(RunControlState *state, DataPanelState *data) {
    if (state->running) return;

    // build run config from data panel selection
    state->run_config.num_data_files = 0;
    for (int i = 0; i < data->file_count && state->run_config.num_data_files < MAX_DATA_FILES; i++) {
        if (data->selected[i]) {
            strncpy(state->run_config.data_paths[state->run_config.num_data_files],
                    data->files[i], 255);
            state->run_config.num_data_files++;
        }
    }

    if (state->run_config.num_data_files == 0) return;

    strncpy(state->run_config.config_path, state->config_path, 255);
    state->run_config.use_config_override = 0;
    state->run_config.collect_features = 0;

    // reset state
    state->progress_pct = 0;
    state->cancel_flag = 0;
    state->complete = 0;
    memset(&state->stats_snapshot, 0, sizeof(state->stats_snapshot));
    state->running = 1;

    // reset candle accumulator if present
    if (state->candle_acc)
        CandleAccumulator_Init(state->candle_acc, 60);

    // spawn worker
    BacktestWorkerArgs *args = (BacktestWorkerArgs *)malloc(sizeof(BacktestWorkerArgs));
    args->state = state;
    pthread_create(&state->worker_tid, NULL, backtest_worker_fn, args);
    pthread_detach(state->worker_tid);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RunControl_Start]
//======================================================================

//======================================================================
// [FUNCTION]_[GUI_Panel_DataBrowser]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[render the Data Browser panel — the discovered-file list + selection]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void GUI_Panel_DataBrowser(DataPanelState *state) {
    ImGui::Begin("Data");

    ImGui::InputText("Directory", state->data_dir, sizeof(state->data_dir));
    ImGui::SameLine();
    if (ImGui::Button("Scan") || !state->scanned)
        DataPanel_Scan(state);

    if (state->file_count == 0) {
        ImGui::TextDisabled("No CSV files found in %s", state->data_dir);
        // a relative dir resolves against the process cwd — show it so a
        // wrong-launch-directory (or a mistyped absolute path) is
        // self-diagnosing instead of reading as a broken trainer
        // (2026-08-20 operator report: scan of /data/BTCUSDT at fs root).
        char cwd[512];
        if (state->data_dir[0] != '/' && getcwd(cwd, sizeof(cwd)))
            ImGui::TextDisabled("(relative to cwd: %s)", cwd);
        ImGui::TextDisabled("Place Binance aggTrades CSVs or TickRecorder output here.");
        ImGui::End();
        return;
    }

    // basic select / clear
    if (ImGui::Button("Select All")) {
        for (int i = 0; i < state->file_count; i++) state->selected[i] = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Select None")) {
        for (int i = 0; i < state->file_count; i++) state->selected[i] = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Invert")) {
        for (int i = 0; i < state->file_count; i++) state->selected[i] = !state->selected[i];
    }

    // quick presets — files are sorted alphabetically (YYYY-MM-DD), so
    // "Last N" = N most recent days. fast iteration patterns:
    //   Last 30   = single month for fast smoke test
    //   Last 90   = quarter, typical first training run
    //   Last 365  = full year for production training
    auto select_last_n = [&](int n) {
        for (int i = 0; i < state->file_count; i++) state->selected[i] = false;
        int start = state->file_count - n;
        if (start < 0) start = 0;
        for (int i = start; i < state->file_count; i++) state->selected[i] = true;
    };
    auto select_first_n = [&](int n) {
        for (int i = 0; i < state->file_count; i++) state->selected[i] = false;
        int end = n < state->file_count ? n : state->file_count;
        for (int i = 0; i < end; i++) state->selected[i] = true;
    };
    if (ImGui::Button("Last 30"))   select_last_n(30);
    ImGui::SameLine(); if (ImGui::Button("Last 90"))   select_last_n(90);
    ImGui::SameLine(); if (ImGui::Button("Last 180"))  select_last_n(180);
    ImGui::SameLine(); if (ImGui::Button("Last 365"))  select_last_n(365);
    ImGui::SameLine(); if (ImGui::Button("Last 730"))  select_last_n(730);

    // custom range — input N, Apply selects last N or first N
    static int n_custom = 90;
    static bool from_end = true;
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##n_custom", &n_custom, 0, 0);
    if (n_custom < 1) n_custom = 1;
    if (n_custom > state->file_count) n_custom = state->file_count;
    ImGui::SameLine();
    ImGui::Checkbox("from end (newest)", &from_end);
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        if (from_end) select_last_n(n_custom);
        else          select_first_n(n_custom);
    }

    // count selected
    state->selected_count = 0;
    for (int i = 0; i < state->file_count; i++)
        if (state->selected[i]) state->selected_count++;

    ImGui::Text("%d files, %d selected", state->file_count, state->selected_count);
    ImGui::Separator();

    // file list with checkboxes
    ImGui::BeginChild("FileList", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (int i = 0; i < state->file_count; i++) {
        // show just the filename, not full path
        const char *name = strrchr(state->files[i], '/');
        name = name ? name + 1 : state->files[i];

        ImGui::Checkbox(name, &state->selected[i]);

        // show file size on hover
        if (ImGui::IsItemHovered()) {
            struct stat st;
            if (stat(state->files[i], &st) == 0) {
                double mb = st.st_size / (1024.0 * 1024.0);
                ImGui::SetItemTooltip("%s\n%.1f MB", state->files[i], mb);
            }
        }
    }
    ImGui::EndChild();

    ImGui::End();
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GUI_Panel_DataBrowser]
//======================================================================

//======================================================================
// [FUNCTION]_[GUI_Panel_RunControl]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[render the Run Control panel — start/cancel, progress, and the post-run snapshot stats]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void GUI_Panel_RunControl(RunControlState *state, DataPanelState *data) {
    ImGui::Begin("Run Control");

    ImGui::InputText("Config", state->config_path, sizeof(state->config_path));

    if (state->running) {
        // progress bar
        ImGui::ProgressBar(state->progress_pct / 100.0f, ImVec2(-1, 0));
        if (ImGui::Button("Cancel")) {
            state->cancel_flag = 1;
        }
    } else {
        // run button
        bool can_run = data->selected_count > 0;
        if (!can_run) ImGui::BeginDisabled();
        if (ImGui::Button("Run Backtest")) {
            RunControl_Start(state, data);
        }
        ImGui::SetItemTooltip(
            "Replays selected files through the engine, computes stats only.\n"
            "Use this for quick performance evaluation (Sharpe, DD, win rate).\n\n"
            "If you want to TRAIN an ML model, use \"Collect Features\" in the\n"
            "Training panel instead — it runs the same backtest plus gathers\n"
            "the feature/label samples XGBoost needs.");
        if (!can_run) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("Select data files first");
        }
    }

    if (state->complete) {
        ImGui::Separator();
        BacktestStats *s = &state->results.stats;
        ImGui::Text("Completed in %.1f ms (%lu ticks)", s->elapsed_ms, s->ticks_processed);
        ImGui::Text("Trades: %u  |  Win Rate: %.1f%%", s->total_trades, s->win_rate);
    }

    ImGui::End();
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GUI_Panel_RunControl]
//======================================================================

//======================================================================
// [FUNCTION]_[ResultsPnlColor]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[pick a P&L cell color from the value sign]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline ImVec4 ResultsPnlColor(double v) {
    return v >= 0.0 ? ImVec4(0.55f, 0.76f, 0.51f, 1.0f)    // foxml green
                    : ImVec4(0.82f, 0.47f, 0.47f, 1.0f);    // foxml red
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ResultsPnlColor]
//======================================================================

//======================================================================
// [FUNCTION]_[GUI_Panel_Results]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[render the Results panel — the backtest stats table + equity curve]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void GUI_Panel_Results(const BacktestResults *results) {
    ImGui::Begin("Results");

    if (results->stats.total_trades == 0) {
        ImGui::TextDisabled("No backtest results yet. Run a backtest first.");
        ImGui::End();
        return;
    }

    const BacktestStats *s = &results->stats;

    // P&L header
    ImGui::TextColored(ResultsPnlColor(s->total_pnl), "P&L: $%.2f  (%.2f%%)",
                       s->total_pnl, s->return_pct);
    ImGui::Separator();

    // stats table
    if (ImGui::BeginTable("stats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        auto row = [](const char *label, const char *fmt, ...) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", label);
            ImGui::TableNextColumn();
            va_list args;
            va_start(args, fmt);
            char buf[64]; vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);
            ImGui::Text("%s", buf);
        };

        row("Trades",         "%u", s->total_trades);
        row("Wins / Losses",  "%u / %u", s->wins, s->losses);
        row("Win Rate",       "%.1f%%", s->win_rate);
        row("Profit Factor",  "%.2f", s->profit_factor);
        row("Expectancy",     "$%.2f", s->expectancy);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("Avg Win");
        ImGui::TableNextColumn();
        ImGui::TextColored(ResultsPnlColor(s->avg_win), "$%.2f", s->avg_win);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("Avg Loss");
        ImGui::TableNextColumn();
        ImGui::TextColored(ResultsPnlColor(-1), "$%.2f", s->avg_loss);

        row("Max Drawdown",   "$%.2f (%.2f%%)", s->max_drawdown, s->max_drawdown_pct);
        row("Sharpe Ratio",   "%.2f", s->sharpe_ratio);
        row("Total Fees",     "$%.2f", s->total_fees);
        row("Avg Hold (ticks)", "%.0f", s->avg_hold_ticks);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Separator();
        ImGui::TableNextColumn(); ImGui::Separator();

        row("Ticks Processed", "%lu", s->ticks_processed);
        row("Elapsed",         "%.1f ms", s->elapsed_ms);
        double tps = s->elapsed_ms > 0 ? s->ticks_processed / (s->elapsed_ms / 1000.0) : 0;
        row("Throughput",      "%.0f ticks/sec", tps);

        if (results->sample_count > 0)
            row("ML Samples",  "%d", results->sample_count);

        if (s->nan_labels_total > 0) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "NaN/Inf Labels");
            ImGui::TableNextColumn();
            if (s->nan_labels_dropped > 0) {
                ImGui::Text("%u total (%u multiclass dropped)",
                            s->nan_labels_total, s->nan_labels_dropped);
            } else {
                ImGui::Text("%u (replaced with neutral default)",
                            s->nan_labels_total);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Label generators produced NaN/Inf for these samples.\n"
                                  "Binary → 0.5, regression → 0.0, multiclass → skipped.\n"
                                  "Non-zero count usually indicates degenerate input data\n"
                                  "(zero prices, missing forward window, etc.).");
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GUI_Panel_Results]
//======================================================================

#define COMPARISON_MAX_RUNS 8

//======================================================================
// [STRUCT]_[ComparisonState]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[state for the Comparison panel — saved run slots for side-by-side compare]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
struct ComparisonState {
    BacktestStats stats[COMPARISON_MAX_RUNS];
    double *equity_curves[COMPARISON_MAX_RUNS];   // dynamic per-run snapshots
    int     equity_counts[COMPARISON_MAX_RUNS];
    char    labels[COMPARISON_MAX_RUNS][64];
    int run_count;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[1768B]
// [ALIGN]_[8]
// [CACHE_LINES]_[28]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[ComparisonState]
//======================================================================

//==========================================================================
// PAST RUNS VIEWER (v4.3) — scan models/{run_name}/ subdirs, parse the
// summary.txt + expected.cfg in each, render a sortable table for easy
// comparison across saved runs. Differs from ComparisonState (in-memory
// equity curves only) — Past Runs persists across restarts, captures ML
// metrics specifically (accuracy, val acc, label kind, hyperparams).
//==========================================================================
#define PAST_RUNS_MAX 64

//======================================================================
// [STRUCT]_[PastRun]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one loaded past-run record — kind-aware metrics + fingerprint + horizon metadata]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
struct PastRun {
    char dir_name[128];          // run directory name under models/
    // from summary.txt
    char role[32];
    float train_accuracy;        // % (in-sample at train time)
    int   label_type;
    int   expected_num_classes;  // 0=binary, 1=regression, ≥2=multiclass
    int   max_depth;
    float learning_rate;
    int   n_estimators;
    // from expected.cfg
    float ml_buy_threshold;
    float ml_tp_pct;             // engine deployment TP (decimal)
    float ml_sl_pct;             // engine deployment SL (decimal)
    float held_out_fraction;
    float gap_acceptable_threshold;
    // v4.3 — LABEL barriers from Training panel (what the model was trained
    // to predict). These are the values shown in the Past Runs table, not
    // ml_tp_pct/ml_sl_pct (which are engine deployment thresholds, often
    // different from the label barriers).
    float label_tp_pct;          // % stored as float (e.g. 0.150 means 0.15%)
    float label_sl_pct;
    int   label_lookahead_ticks;
    // from summary.txt v2 (post-v4.3) — optional, zeroed when missing
    int   label_kind;            // 0=binary/multiclass, 1=regression (drives display formatting)
    float val_accuracy;          // walk-forward mean (for binary/multiclass)
    float val_stddev;
    float val_correlation;       // for regression
    float val_mse;               // for regression
    float train_val_gap;
    int   overfit_folds;
    int   has_wf_results;        // 1 = WF metrics present, 0 = old-format file
    // v5.8.9 — held-out + auto-stamp metadata. Populated when summary.txt
    // contains held_out_metric (Run Full Validation produced it) and when
    // a .stamp file exists alongside the saved model. Operator-visible
    // signals: "is this run deploy-ready?" (held-out gap < threshold +
    // signed stamp present + matches current build).
    float held_out_metric;
    int   has_held_out;
    int   has_stamp;             // 1 = .stamp file exists in run dir
    char  stamp_verify_msg[128]; // populated by Verify Stamp button — empty if not verified yet
    int   stamp_verify_state;    // 0=unverified, 1=ok, -1=fail
    // v5.9.5d — full verify result stored when stamp_verify_state==1.
    // Renders as expandable "Stamp details" tree below the OK/FAIL line.
    // Pre-v5.9.5d the operator only saw "OK — engine=X registry=Y" without
    // the recorded inference cfg / training metrics / scaler binding /
    // model_num_outputs that were ALL just stamped (v5.9.5b/c). This makes
    // those values actually visible for cross-cfg audit.
    ModelStampResult stamp_verify_full;
    int   stamp_verify_has_full;  // 1 = stamp_verify_full populated
    // v5.11.51 — Date column + multi-horizon grouping. mtime_sec = directory
    // mtime (sortable + display). prefix + horizon_ticks let the renderer
    // detect multi-horizon siblings (dirs matching <prefix>_horizon_<H>) and
    // collapse them into a single visual row.
    time_t mtime_sec;            // directory mtime; 0 if stat failed
    char   prefix[128];          // dir_name with "_horizon_<N>" stripped (or full dir_name)
    int    horizon_ticks;        // parsed from dir_name; 0 if not multi-horizon row
    char   full_path[400];       // full path under models/ (e.g. "models/classification/foo");
                                  // populated by PastRuns_LoadOne. Used by Delete button.
    // v5.11.54 — multi-horizon visual grouping. group_size=N means this row
    // is part of a cluster of N rows sharing the same prefix (all multi-
    // horizon siblings). group_idx=0 = first/header; >=1 = continuation
    // (rendered with indent). group_size=1 = singleton (not a group).
    int    group_size;
    int    group_idx;
    // v5.15.5 — training sample count from summary.txt. 0 = older run
    // pre-v5.15.5 that didn't capture the field. Rendered as "Samples"
    // column in both classification + regression tables; lets operator
    // gauge training data scale at a glance.
    int    n_train_samples;
    // E.1.2.D D-e — which side's record this row displays (0 = entry /
    // legacy summary.txt, 1 = exit), and whether the OTHER side's summary
    // also exists in the dir (rendered as a [+exit]/[+entry] tag so
    // neither record is invisible).
    int    summary_side;
    int    has_other_side;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-23]
// [SIZE]_[6272B]
// [ALIGN]_[16]
// [CACHE_LINES]_[98]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[PastRun]
//======================================================================

//======================================================================
// [STRUCT]_[PastRunsState]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[state for the Past Runs panel — the scanned run-directory list + selection]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
struct PastRunsState {
    PastRun runs[PAST_RUNS_MAX];
    int     count;
    int     selected;            // index of last clicked row (for inspector / actions)
    char    status_msg[256];     // last action status (e.g., "loaded", "deleted")
    int     sort_column;         // 0..N-1, which column to sort by
    int     sort_descending;     // 0 = asc, 1 = desc
    // v5.9.5i — stamp audit filter. 0 = all, 1 = stamped only, 2 = OK only,
    // 3 = FAIL only, 4 = unstamped only.
    int     stamp_filter;
    // v5.10.0a — Compare-to-Baseline slots. Operator picks two runs from
    // dropdowns; Compare button opens a modal showing metric deltas.
    // -1 = unselected. Persists across rescans (modal closes on rescan
    // for safety; indices may shift).
    int     compare_baseline_idx;
    int     compare_candidate_idx;
    int     compare_modal_open;  // 1 = render modal next frame
    // v5.15.5 — Delete confirm modal hoisted out of per-row popup
    // (popup-inside-table-cell rendering issue: button clicked → peach
    // flash → popup never appeared because ImGui popup ID gets scoped
    // to the row's transient context). Track pending row index here;
    // single modal renders at window scope after EndTabBar.
    int     pending_delete_idx;  // -1 = no delete pending
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-23]
// [SIZE]_[401712B]
// [ALIGN]_[16]
// [CACHE_LINES]_[6277]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[PastRunsState]
//======================================================================

//======================================================================
// [FUNCTION]_[PastRuns_Init]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[init the Past Runs state]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void PastRuns_Init(PastRunsState *s) {
    memset(s, 0, sizeof(*s));
    s->selected = -1;
    s->pending_delete_idx = -1;
    s->sort_column = 6;          // default sort by val_accuracy descending
    s->sort_descending = 1;
    // v5.10.0a — Compare slots default to "unselected".
    s->compare_baseline_idx = -1;
    s->compare_candidate_idx = -1;
    s->compare_modal_open = 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[PastRuns_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[parse_kv_line]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[parse one key=value line from a run's metadata file]
//======================================================================
// helper: parse a key=value line into a (key, value) pair via simple split.
// returns 1 on success, 0 if line doesn't contain '='.
//======================================================================
// [CODE]
//======================================================================
static inline int parse_kv_line(const char *line, char *key, size_t key_size,
                                  char *val, size_t val_size) {
    const char *eq = strchr(line, ':');
    const char *eq2 = strchr(line, '=');
    if (!eq || (eq2 && eq2 < eq)) eq = eq2;
    if (!eq) return 0;
    size_t klen = (size_t)(eq - line);
    if (klen >= key_size) klen = key_size - 1;
    memcpy(key, line, klen);
    key[klen] = '\0';
    // trim trailing whitespace from key
    while (klen > 0 && (key[klen-1] == ' ' || key[klen-1] == '\t')) key[--klen] = '\0';
    // skip ':' or '=' and following whitespace
    const char *vstart = eq + 1;
    while (*vstart == ' ' || *vstart == '\t') vstart++;
    strncpy(val, vstart, val_size - 1);
    val[val_size - 1] = '\0';
    // trim trailing newline / whitespace from value
    size_t vlen = strlen(val);
    while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r' ||
                         val[vlen-1] == ' '  || val[vlen-1] == '\t' ||
                         val[vlen-1] == '%')) val[--vlen] = '\0';
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[parse_kv_line]
//======================================================================

//======================================================================
// [FUNCTION]_[PastRuns_LoadOne]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[load one past-run record from its run directory]
//======================================================================
// scan one run directory's metadata files
//======================================================================
// [CODE]
//======================================================================
static inline int PastRuns_LoadOne(PastRun *r, const char *run_dir) {
    memset(r, 0, sizeof(*r));
    const char *base = strrchr(run_dir, '/');
    base = base ? base + 1 : run_dir;
    strncpy(r->dir_name, base, sizeof(r->dir_name) - 1);
    // v5.11.51 — full path for Delete button
    strncpy(r->full_path, run_dir, sizeof(r->full_path) - 1);

    char path[400];
    char line[512];

    // E.1.2.D D-e — side-suffixed summaries (writer above). Preference:
    // entry > legacy summary.txt > exit, so a dir carrying both sides
    // shows its ENTRY record by default; the other side's presence is
    // flagged (has_other_side) so neither record is invisible. This is
    // also the listing gate: PastRuns_ScanOneDir lists a dir iff this
    // returns 1, so any of the three names qualifies a run dir.
    static const char* summary_names[] = {
        "summary_entry.txt", "summary.txt", "summary_exit.txt" };
    FILE *f = NULL;
    int summary_idx = -1;
    for (int si = 0; si < 3 && !f; ++si) {
        snprintf(path, sizeof(path), "%s/%s", run_dir, summary_names[si]);
        f = fopen(path, "r");
        if (f) summary_idx = si;
    }
    if (!f) return 0;  // no summary of any side = not a run bundle
    r->summary_side = (summary_idx == 2) ? 1 : 0;   // 1 = the row shows the EXIT record
    {
        struct stat ost;
        char opath[400];
        snprintf(opath, sizeof(opath), "%s/%s", run_dir,
                 (summary_idx == 2) ? "summary_entry.txt" : "summary_exit.txt");
        r->has_other_side = (stat(opath, &ost) == 0) ? 1 : 0;
        if (!r->has_other_side && summary_idx == 2) {
            // an exit row whose dir also carries a LEGACY entry summary
            snprintf(opath, sizeof(opath), "%s/summary.txt", run_dir);
            r->has_other_side = (stat(opath, &ost) == 0) ? 1 : 0;
        }
    }
    while (fgets(line, sizeof(line), f)) {
        char k[64], v[256];
        if (!parse_kv_line(line, k, sizeof(k), v, sizeof(v))) continue;
        if      (strcmp(k, "role") == 0)                 strncpy(r->role, v, sizeof(r->role) - 1);
        else if (strcmp(k, "accuracy") == 0)             r->train_accuracy = (float)atof(v);
        else if (strcmp(k, "label_type") == 0)           r->label_type = atoi(v);
        else if (strcmp(k, "expected_num_classes") == 0) r->expected_num_classes = atoi(v);
        else if (strcmp(k, "max_depth") == 0)            r->max_depth = atoi(v);
        else if (strcmp(k, "learning_rate") == 0)        r->learning_rate = (float)atof(v);
        else if (strcmp(k, "n_estimators") == 0)         r->n_estimators = atoi(v);
        else if (strcmp(k, "val_accuracy") == 0)       { r->val_accuracy = (float)atof(v); r->has_wf_results = 1; }
        else if (strcmp(k, "val_stddev") == 0)           r->val_stddev = (float)atof(v);
        else if (strcmp(k, "val_correlation") == 0)    { r->val_correlation = (float)atof(v); r->has_wf_results = 1; }
        else if (strcmp(k, "val_mse") == 0)              r->val_mse = (float)atof(v);
        else if (strcmp(k, "label_kind") == 0)           r->label_kind = atoi(v);
        else if (strcmp(k, "train_val_gap") == 0)        r->train_val_gap = (float)atof(v);
        else if (strcmp(k, "overfit_folds") == 0)        r->overfit_folds = atoi(v);
        else if (strcmp(k, "label_tp_pct") == 0)         r->label_tp_pct = (float)atof(v);
        else if (strcmp(k, "label_sl_pct") == 0)         r->label_sl_pct = (float)atof(v);
        else if (strcmp(k, "label_lookahead_ticks") == 0) r->label_lookahead_ticks = atoi(v);
        // v5.8.9 — held-out + auto-stamp summary fields (optional, missing
        // for older runs).
        else if (strcmp(k, "held_out_metric") == 0)    { r->held_out_metric = (float)atof(v); r->has_held_out = 1; }
        // v5.15.5 — training data scale; missing on older runs = 0.
        else if (strcmp(k, "n_train_samples") == 0)      r->n_train_samples = atoi(v);
    }
    fclose(f);

    // v5.8.9 — check for a .stamp file alongside the model. PastRuns
    // doesn't parse the stamp body itself (too expensive — would compute
    // SHA-256 of every saved model on Rescan). Verify Stamp button fires
    // verify_model_stamp on demand.
    {
        char model_path[400];
        const char *src_ext = ".json";  // most common; verifier checks .bin path with .stamp suffix
        // Try role-specific filenames in priority order
        // E.1.2.D D-e — the badge follows the RECORD the row displays: when
        // the summary declares its role, ONLY that role's stamp counts. The
        // old any-role loop let an exit row wear the buy model's badge —
        // measured on run_1: rows read "[stamped] exit" off barrier.json.stamp
        // while zero exit stamps existed on disk.
        if (r->role[0]) {
            char stamp_path[420];
            snprintf(stamp_path, sizeof(stamp_path), "%s/%s.json.stamp",
                     run_dir, r->role);
            struct stat sst;
            r->has_stamp = (stat(stamp_path, &sst) == 0) ? 1 : 0;
            if (!r->has_stamp) {  // stamps ride .json; .xgb roles verified via .xgb.stamp
                snprintf(stamp_path, sizeof(stamp_path), "%s/%s.xgb.stamp",
                         run_dir, r->role);
                r->has_stamp = (stat(stamp_path, &sst) == 0) ? 1 : 0;
            }
        } else {
            // Legacy summary with no role: field — the old any-role probe.
            const char *role_files[] = {"barrier.json", "buy_signal.json", "regime.json", "exit.json", "exit.xgb",  /* E.1.2.C — exit-blindness fix */
                                         "barrier.xgb",  "buy_signal.xgb",  "regime.xgb",
                                         NULL};
            for (int i = 0; role_files[i]; ++i) {
                snprintf(model_path, sizeof(model_path), "%s/%s", run_dir, role_files[i]);
                struct stat mst;
                if (stat(model_path, &mst) != 0) continue;
                char stamp_path[420];
                snprintf(stamp_path, sizeof(stamp_path), "%s.stamp", model_path);
                struct stat sst;
                if (stat(stamp_path, &sst) == 0) {
                    r->has_stamp = 1;
                    break;
                }
            }
        }
        (void)src_ext;
    }

    // expected.cfg (optional; older runs may not have all fields)
    snprintf(path, sizeof(path), "%s/expected.cfg", run_dir);
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#') continue;
            char k[64], v[256];
            if (!parse_kv_line(line, k, sizeof(k), v, sizeof(v))) continue;
            if      (strcmp(k, "ml_buy_threshold") == 0)         r->ml_buy_threshold = (float)atof(v);
            else if (strcmp(k, "ml_tp_pct") == 0)                 r->ml_tp_pct = (float)atof(v);
            else if (strcmp(k, "ml_sl_pct") == 0)                 r->ml_sl_pct = (float)atof(v);
            else if (strcmp(k, "held_out_fraction") == 0)         r->held_out_fraction = (float)atof(v);
            else if (strcmp(k, "gap_acceptable_threshold") == 0)  r->gap_acceptable_threshold = (float)atof(v);
        }
        fclose(f);
    }
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[PastRuns_LoadOne]
//======================================================================

//======================================================================
// [FUNCTION]_[past_runs_unlink_cb]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[nftw unlink callback for recursive run-directory deletion]
//======================================================================
// v4.3 — scan one directory for run subdirs containing summary.txt. Used
// recursively for the two-level models/{kind}/{run_name}/ layout AND for
// backward compat with flat models/{run_name}/ runs from before v4.3.
// v5.11.51 — recursive directory delete via nftw. Used by Past Runs
// "Delete" button. Returns 0 on success, -1 on any error.
//======================================================================
// [CODE]
//======================================================================
static inline int past_runs_unlink_cb(const char *fpath, const struct stat *sb,
                                          int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (typeflag == FTW_DP || typeflag == FTW_D) {
        return rmdir(fpath);
    }
    return unlink(fpath);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[past_runs_unlink_cb]
//======================================================================

//======================================================================
// [FUNCTION]_[PastRuns_DeleteDir]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[recursively delete a run directory via nftw]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline int PastRuns_DeleteDir(const char *path) {
    // FTW_DEPTH = post-order traversal so files deleted before parent dir
    // FTW_PHYS = don't follow symlinks (avoid accidentally walking into other
    //            dirs if operator has bizarre symlink configuration)
    return nftw(path, past_runs_unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[PastRuns_DeleteDir]
//======================================================================

//======================================================================
// [FUNCTION]_[PastRun_ParseHorizon]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[parse a horizon prefix + label out of a run-directory name]
//======================================================================
// v5.11.51 — parse "<prefix>_horizon_<N>" from dir_name. Returns 1 if it
// matches the multi-horizon pattern (sets out_prefix + out_horizon_ticks).
// Returns 0 if not a multi-horizon dir (out_prefix gets dir_name copy,
// out_horizon_ticks = 0).
//======================================================================
// [CODE]
//======================================================================
static inline int PastRun_ParseHorizon(const char *dir_name, char *out_prefix,
                                          size_t out_prefix_size, int *out_horizon_ticks) {
    *out_horizon_ticks = 0;
    out_prefix[0] = '\0';
    const char *match = strstr(dir_name, "_horizon_");
    if (!match) {
        // Not a multi-horizon dir; whole name is the prefix.
        size_t n = strnlen(dir_name, out_prefix_size - 1);
        memcpy(out_prefix, dir_name, n);
        out_prefix[n] = '\0';
        return 0;
    }
    // Verify suffix is purely digits
    const char *digits = match + 9;  // strlen("_horizon_")
    if (!*digits) {
        // "_horizon_" with nothing after; treat as not-multi-horizon
        size_t n = strnlen(dir_name, out_prefix_size - 1);
        memcpy(out_prefix, dir_name, n);
        out_prefix[n] = '\0';
        return 0;
    }
    char *end = nullptr;
    long h = strtol(digits, &end, 10);
    if (end == digits || *end != '\0' || h <= 0) {
        // Not pure digits or 0/negative; treat as not-multi-horizon.
        size_t n = strnlen(dir_name, out_prefix_size - 1);
        memcpy(out_prefix, dir_name, n);
        out_prefix[n] = '\0';
        return 0;
    }
    // Match — copy prefix (up to but not including "_horizon_")
    size_t prefix_len = (size_t)(match - dir_name);
    if (prefix_len >= out_prefix_size) prefix_len = out_prefix_size - 1;
    memcpy(out_prefix, dir_name, prefix_len);
    out_prefix[prefix_len] = '\0';
    *out_horizon_ticks = (int)h;
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[PastRun_ParseHorizon]
//======================================================================

//======================================================================
// [FUNCTION]_[PastRuns_ScanOneDir]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[scan one directory for past-run records]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void PastRuns_ScanOneDir(PastRunsState *s, const char *path) {
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && s->count < PAST_RUNS_MAX) {
        if (entry->d_name[0] == '.') continue;
        char sub[400];
        snprintf(sub, sizeof(sub), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (PastRuns_LoadOne(&s->runs[s->count], sub)) {
            // v5.11.51 — capture mtime + parse multi-horizon prefix
            // (the RETIRED flat form keeps listing here pre-migration —
            // old dirs stay visible, never silently vanish; D-431).
            PastRun *r = &s->runs[s->count];
            r->mtime_sec = st.st_mtime;
            PastRun_ParseHorizon(entry->d_name, r->prefix, sizeof(r->prefix),
                                   &r->horizon_ticks);
            s->count++;
        } else {
            // D-431 NESTED — a summary-less dir may be a FAMILY node whose
            // horizon_<N> children hold the run records one level down.
            // Family name comes from the PARENT dir; the horizon from the
            // child (schema matcher); the existing sort+adjacency grouping
            // then renders them as one family block.
            DIR *fd = opendir(sub);
            if (!fd) continue;
            struct dirent *ce;
            while ((ce = readdir(fd)) != NULL && s->count < PAST_RUNS_MAX) {
                long h = ModelPath_ParseHorizonChild(ce->d_name);
                if (h < 0) continue;
                char hsub[440];
                snprintf(hsub, sizeof(hsub), "%s/%s", sub, ce->d_name);
                struct stat hst;
                if (stat(hsub, &hst) != 0 || !S_ISDIR(hst.st_mode)) continue;
                if (PastRuns_LoadOne(&s->runs[s->count], hsub)) {
                    PastRun *r = &s->runs[s->count];
                    r->mtime_sec = hst.st_mtime;
                    snprintf(r->prefix, sizeof(r->prefix), "%s", entry->d_name);
                    r->horizon_ticks = (int)h;
                    s->count++;
                }
            }
            closedir(fd);
        }
    }
    closedir(d);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[PastRuns_ScanOneDir]
//======================================================================

//======================================================================
// [FUNCTION]_[PastRuns_Scan]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[scan the runs root for every past-run record]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void PastRuns_Scan(PastRunsState *s) {
    s->count = 0;
    s->status_msg[0] = '\0';
    // v4.3 — walk the kind-organized subdirs first
    PastRuns_ScanOneDir(s, "models/classification");
    PastRuns_ScanOneDir(s, "models/regression");
    // Backward compat: also scan models/ directly for runs saved before v4.3
    // (those still have summary.txt at models/{run_name}/).
    PastRuns_ScanOneDir(s, "models");

    // v5.11.51 — sort runs by mtime descending (newest first). v5.11.54
    // refines: secondary sort key = (prefix asc, horizon_ticks asc) so
    // multi-horizon siblings cluster together within their mtime cohort.
    // Group-leader's mtime determines the group's position in the list.
    for (int i = 0; i < s->count - 1; ++i) {
        for (int j = i + 1; j < s->count; ++j) {
            // Primary: mtime desc (newer first)
            int swap = 0;
            if (s->runs[j].mtime_sec > s->runs[i].mtime_sec + 60) {
                // 60s grace = treat near-simultaneous as same cohort, then
                // sort by prefix within cohort
                swap = 1;
            } else if (s->runs[i].mtime_sec > s->runs[j].mtime_sec + 60) {
                swap = 0;
            } else {
                // Same cohort — sort by prefix asc, horizon_ticks asc
                int cmp = strcmp(s->runs[j].prefix, s->runs[i].prefix);
                if (cmp < 0) {
                    swap = 1;
                } else if (cmp == 0 &&
                           s->runs[j].horizon_ticks < s->runs[i].horizon_ticks) {
                    swap = 1;
                }
            }
            if (swap) {
                PastRun tmp = s->runs[i];
                s->runs[i] = s->runs[j];
                s->runs[j] = tmp;
            }
        }
    }

    // v5.11.54 — compute group_size + group_idx for visual grouping in
    // render. Walk runs, group consecutive same-prefix multi-horizon
    // siblings. Singletons (horizon_ticks=0 OR no siblings) get
    // group_size=1, group_idx=0 (rendered as standalone row).
    for (int i = 0; i < s->count; ) {
        int group_end = i + 1;
        // Only multi-horizon runs (horizon_ticks > 0) form groups
        if (s->runs[i].horizon_ticks > 0) {
            while (group_end < s->count &&
                   s->runs[group_end].horizon_ticks > 0 &&
                   strcmp(s->runs[group_end].prefix, s->runs[i].prefix) == 0) {
                group_end++;
            }
        }
        int gsize = group_end - i;
        for (int g = i; g < group_end; ++g) {
            s->runs[g].group_size = gsize;
            s->runs[g].group_idx  = g - i;
        }
        i = group_end;
    }

    snprintf(s->status_msg, sizeof(s->status_msg),
             "scanned %d run(s) in models/{classification,regression,...} (newest first; multi-horizon grouped)",
             s->count);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[PastRuns_Scan]
//======================================================================

//======================================================================
// [FUNCTION]_[PastRun_MetricLabel]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the metric-label string for a run's label kind (accuracy vs correlation)]
//======================================================================
// label-type-aware metric label
//======================================================================
// [CODE]
//======================================================================
static inline const char* PastRun_MetricLabel(int expected_num_classes) {
    if (expected_num_classes == 1) return "Corr (r)";   // regression
    if (expected_num_classes >= 2) return "Acc (multi)";// multiclass
    return "Acc (bin)";                                  // binary (0)
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[PastRun_MetricLabel]
//======================================================================

//======================================================================
// [FUNCTION]_[GUI_Panel_PastRuns]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[render the Past Runs panel — the run table + per-run detail + delete/compare actions]
//======================================================================
// v5.11.57 — `cfg_for_verify` exposes ControllerConfig (typically
// &run_control->results.config_used) so Verify Stamp can use the
// real cfg.auto_stamp_secret for HMAC-verification (not just devmode).
// Pass NULL to keep pre-v5.11.57 behavior (devmode-only).
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-4]
//======================================================================
// [CODE]
//======================================================================
static inline void GUI_Panel_PastRuns(PastRunsState *s,
                                        const ControllerConfig<BACKTEST_FP> *cfg_for_verify = nullptr) {
    ImGui::Begin("Past Runs");
    SectionHeader("PAST RUNS");

    if (ImGui::Button("Rescan")) PastRuns_Scan(s);
    ImGui::SameLine();
    if (s->status_msg[0])
        ImGui::TextColored(FoxmlColors::comment, "(%s)", s->status_msg);

    // v5.9.5i — Stamp audit filter. Operator can isolate runs by stamp
    // status (all / stamped / OK only / FAIL / unstamped) for audit
    // workflows. Per /plan-check 2026-05-02: dedicated Stamps panel
    // would duplicate Past Runs's scan logic; filter inside Past Runs
    // gives the same operator audit value with no panel duplication.
    {
        static const char* filter_names[] = {
            "All", "Stamped", "Stamp OK", "Stamp FAIL", "Unstamped"
        };
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::Combo("##stamp_filter", &s->stamp_filter, filter_names, 5);
        ImGui::SetItemTooltip("Filter runs by stamp status:\n"
                              "All: every run\n"
                              "Stamped: has .stamp file (any verify state)\n"
                              "Stamp OK: Verify Stamp returned valid=1\n"
                              "Stamp FAIL: Verify Stamp returned valid=0\n"
                              "Unstamped: no .stamp file\n\n"
                              "Click 'Verify Stamp' on a row to populate\n"
                              "OK/FAIL state (default is unverified).");
    }

    if (s->count == 0) {
        ImGui::TextDisabled("No saved runs found in models/. "
                            "Train a model and click 'Save Run' in the Training panel.");
        ImGui::End();
        return;
    }

    // v5.10.0a — Compare-to-Baseline. Two dropdowns + Compare button on
    // the same line as Rescan; modal pops up showing metric deltas. Value:
    // validates v5.10.0 perf optimizations didn't change model behavior
    // (pick foundation-baseline vs post-fix run; metrics should match
    // within tolerance). Headless backdoor: existing summary.txt files
    // are diff-able with `diff models/A/summary.txt models/B/summary.txt`.
    {
        ImGui::Separator();
        ImGui::TextDisabled("Compare:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        if (ImGui::BeginCombo("Baseline",
                              s->compare_baseline_idx >= 0 && s->compare_baseline_idx < s->count
                                ? s->runs[s->compare_baseline_idx].dir_name
                                : "(pick a run)")) {
            for (int i = 0; i < s->count; ++i) {
                bool sel = (i == s->compare_baseline_idx);
                if (ImGui::Selectable(s->runs[i].dir_name, sel))
                    s->compare_baseline_idx = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        if (ImGui::BeginCombo("Candidate",
                              s->compare_candidate_idx >= 0 && s->compare_candidate_idx < s->count
                                ? s->runs[s->compare_candidate_idx].dir_name
                                : "(pick a run)")) {
            for (int i = 0; i < s->count; ++i) {
                bool sel = (i == s->compare_candidate_idx);
                if (ImGui::Selectable(s->runs[i].dir_name, sel))
                    s->compare_candidate_idx = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        bool can_compare = (s->compare_baseline_idx >= 0 &&
                             s->compare_candidate_idx >= 0 &&
                             s->compare_baseline_idx != s->compare_candidate_idx);
        if (!can_compare) ImGui::BeginDisabled();
        if (ImGui::Button("Compare")) {
            s->compare_modal_open = 1;
            ImGui::OpenPopup("Compare to Baseline");
        }
        if (!can_compare) ImGui::EndDisabled();

        // Modal: render unconditionally (ImGui no-ops when not open). Inside:
        // pull both rows, render metric deltas with color-coded thresholds.
        // bool proxy so the int compare_modal_open can drive ImGui's bool*
        // signature; sync back after the modal returns.
        bool modal_open_b = (s->compare_modal_open != 0);
        if (ImGui::BeginPopupModal("Compare to Baseline", &modal_open_b,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {
            if (s->compare_baseline_idx >= 0 && s->compare_baseline_idx < s->count &&
                s->compare_candidate_idx >= 0 && s->compare_candidate_idx < s->count) {
                const PastRun *base = &s->runs[s->compare_baseline_idx];
                const PastRun *cand = &s->runs[s->compare_candidate_idx];

                ImGui::Text("Baseline:  %s  (%s, %d-class)",
                            base->dir_name, base->role,
                            base->expected_num_classes);
                ImGui::Text("Candidate: %s  (%s, %d-class)",
                            cand->dir_name, cand->role,
                            cand->expected_num_classes);
                if (base->expected_num_classes != cand->expected_num_classes) {
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f),
                        "WARN: different class counts; metrics may not be directly comparable");
                }
                ImGui::Separator();

                // Helper: render one metric row. delta_pp = candidate - baseline
                // in percentage points. green = candidate beats by >2pp,
                // red = candidate worse by >2pp, yellow within ±2pp.
                auto metric_row = [](const char* label, float base_val,
                                      float cand_val, const char* fmt,
                                      bool higher_is_better, double tol_pp) {
                    double delta = (double)cand_val - (double)base_val;
                    ImVec4 col;
                    if (fabs(delta) <= tol_pp) col = ImVec4(0.85f, 0.80f, 0.50f, 1.0f); // yellow within tol
                    else if ((delta > 0 && higher_is_better) ||
                             (delta < 0 && !higher_is_better))
                        col = ImVec4(0.55f, 0.76f, 0.51f, 1.0f); // green better
                    else
                        col = ImVec4(0.95f, 0.35f, 0.35f, 1.0f); // red worse
                    char base_buf[32], cand_buf[32];
                    snprintf(base_buf, sizeof(base_buf), fmt, base_val);
                    snprintf(cand_buf, sizeof(cand_buf), fmt, cand_val);
                    ImGui::Text("%-22s  base=%-8s  cand=%-8s",
                                label, base_buf, cand_buf);
                    ImGui::SameLine();
                    ImGui::TextColored(col, " (Δ %+.3f)", delta);
                };

                ImGui::TextDisabled("Performance metrics");
                metric_row("Train accuracy:",
                           base->train_accuracy, cand->train_accuracy,
                           "%.1f%%", true, 2.0);
                if (base->has_wf_results && cand->has_wf_results) {
                    if (base->expected_num_classes == 1 || cand->expected_num_classes == 1) {
                        metric_row("Val correlation (r):",
                                   base->val_correlation, cand->val_correlation,
                                   "%.3f", true, 0.05);
                        metric_row("Val MSE:",
                                   base->val_mse, cand->val_mse,
                                   "%.5f", false, 0.001);
                    } else {
                        metric_row("Val accuracy:",
                                   base->val_accuracy, cand->val_accuracy,
                                   "%.1f%%", true, 2.0);
                    }
                    metric_row("Train/val gap:",
                               base->train_val_gap, cand->train_val_gap,
                               "%.4f", false, 0.02);
                    ImGui::Text("Overfit folds:        base=%-8d  cand=%-8d",
                                base->overfit_folds, cand->overfit_folds);
                } else if (!base->has_wf_results || !cand->has_wf_results) {
                    ImGui::TextColored(ImVec4(0.85f, 0.80f, 0.50f, 1.0f),
                        "WF metrics missing on %s%s%s — only train accuracy compared.",
                        !base->has_wf_results ? "baseline" : "",
                        (!base->has_wf_results && !cand->has_wf_results) ? " + " : "",
                        !cand->has_wf_results ? "candidate" : "");
                }

                ImGui::Separator();
                ImGui::TextDisabled("Hyperparams (training-time)");
                ImGui::Text("Max depth:            base=%-8d  cand=%-8d",
                            base->max_depth, cand->max_depth);
                ImGui::Text("Learning rate:        base=%-8.3f  cand=%-8.3f",
                            base->learning_rate, cand->learning_rate);
                ImGui::Text("N estimators:         base=%-8d  cand=%-8d",
                            base->n_estimators, cand->n_estimators);

                ImGui::Separator();
                ImGui::TextDisabled("Label config (sweep / drift detection)");
                metric_row("Label TP %:",
                           base->label_tp_pct, cand->label_tp_pct,
                           "%.3f", true, 0.001);
                metric_row("Label SL %:",
                           base->label_sl_pct, cand->label_sl_pct,
                           "%.3f", true, 0.001);
                ImGui::Text("Lookahead ticks:      base=%-8d  cand=%-8d",
                            base->label_lookahead_ticks, cand->label_lookahead_ticks);

                ImGui::Separator();
                if (ImGui::Button("Close")) {
                    s->compare_modal_open = 0;
                    ImGui::CloseCurrentPopup();
                }
            } else {
                ImGui::Text("(invalid selection)");
                if (ImGui::Button("Close")) {
                    s->compare_modal_open = 0;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        // Sync proxy back to int storage (ImGui clears modal_open_b when
        // operator clicks the X button on the modal).
        s->compare_modal_open = modal_open_b ? 1 : 0;
    }

    // Split runs by label kind so each tab has its own column set.
    // Classification tab: binary + multiclass (label_kind != 1).
    // Regression tab: label_kind == 1.
    int n_class = 0, n_regr = 0;
    for (int i = 0; i < s->count; ++i) {
        if (s->runs[i].label_kind == 1) n_regr++;
        else                              n_class++;
    }

    ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;

    // Helper: render one selectable row's leading "Run" cell. Shared between
    // both tabs since selection is global across runs.
    // v5.11.54 — Multi-horizon visual grouping. When this row is part of a
    // group (group_size > 1), render differently:
    //   - First row of group (group_idx == 0): show prefix + " [N horizons]"
    //     badge so operator sees the group at a glance
    //   - Continuation rows (group_idx > 0): indent with "  └ horizon <H>"
    //     so the cluster visually groups under the header row
    // Singleton rows (group_size == 1) render with full dir_name as before.
    auto render_run_cell = [&](int i) {
        PastRun *r = &s->runs[i];
        ImGui::TableSetColumnIndex(0);
        char rowid[200];
        const char *stamp_tag = r->has_stamp ? "[stamped] " : "";
        // E.1.2.D D-e — a dir carrying BOTH sides' summaries shows which
        // record this row is and that the other exists (the exit run used
        // to silently eclipse the buy record entirely).
        const char *side_tag = r->has_other_side
            ? (r->summary_side ? "[exit·+entry] " : "[+exit] ")
            : (r->summary_side ? "[exit] " : "");
        char tag_buf[40];
        snprintf(tag_buf, sizeof(tag_buf), "%s%s", stamp_tag, side_tag);
        stamp_tag = tag_buf;
        if (r->group_size > 1 && r->group_idx == 0) {
            // Group header — show prefix + count badge
            snprintf(rowid, sizeof(rowid),
                     "%s%s [%d horizons]##run%d",
                     stamp_tag, r->prefix, r->group_size, i);
        } else if (r->group_size > 1 && r->group_idx > 0) {
            // Continuation — indented
            snprintf(rowid, sizeof(rowid),
                     "%s    └ horizon %d##run%d",
                     stamp_tag, r->horizon_ticks, i);
        } else {
            // Singleton (single-horizon or non-multi run)
            snprintf(rowid, sizeof(rowid), "%s%s##run%d",
                     stamp_tag, r->dir_name, i);
        }
        bool sel = (s->selected == i);
        if (ImGui::Selectable(rowid, sel, ImGuiSelectableFlags_SpanAllColumns)) {
            s->selected = i;
        }
    };

    if (ImGui::BeginTabBar("##past_runs_tabs")) {
        // ============================================================
        // CLASSIFICATION TAB — binary + multiclass models
        // ============================================================
        char class_label[64];
        snprintf(class_label, sizeof(class_label), "Classification (%d)", n_class);
        if (ImGui::BeginTabItem(class_label)) {
            if (n_class == 0) {
                ImGui::TextDisabled("No classification runs saved yet.");
            } else if (ImGui::BeginTable("past_runs_class", 16, flags)) {  // v5.11.51: +Date +Delete cols; v5.15.5.E.bugfix: 15→16 (Samples col added but BeginTable count missed; ImGui asserted on 16th TableSetupColumn)
                ImGui::TableSetupColumn("Run",        ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 220);
                ImGui::TableSetupColumn("Date",       ImGuiTableColumnFlags_WidthFixed, 100);  // v5.11.51
                ImGui::TableSetupColumn("Role",       ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Label",      ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("Classes",    ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("TP bps",     ImGuiTableColumnFlags_WidthFixed, 75);
                ImGui::TableSetupColumn("SL bps",     ImGuiTableColumnFlags_WidthFixed, 75);
                ImGui::TableSetupColumn("Lookahead",  ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Samples",    ImGuiTableColumnFlags_WidthFixed, 80);  // v5.15.5
                ImGui::TableSetupColumn("Train Acc",  ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Val Acc",    ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Gap",        ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Overfit",    ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Depth/LR/N", ImGuiTableColumnFlags_WidthFixed, 120);
                ImGui::TableSetupColumn("Stamp",      ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("",           ImGuiTableColumnFlags_WidthFixed, 30);  // v5.11.51 Delete
                ImGui::TableHeadersRow();

                for (int i = 0; i < s->count; ++i) {
                    PastRun *r = &s->runs[i];
                    if (r->label_kind == 1) continue;  // skip regression runs
                    // v5.9.5i — stamp filter
                    if (s->stamp_filter == 1 && !r->has_stamp) continue;
                    if (s->stamp_filter == 2 && r->stamp_verify_state != 1) continue;
                    if (s->stamp_filter == 3 && r->stamp_verify_state != -1) continue;
                    if (s->stamp_filter == 4 && r->has_stamp) continue;
                    ImGui::TableNextRow();

                    render_run_cell(i);
                    // v5.11.51 — Date column (2nd col); shows "MM-DD HH:MM" in local time
                    ImGui::TableNextColumn();
                    if (r->mtime_sec > 0) {
                        char dbuf[24];
                        struct tm tm_buf;
                        localtime_r(&r->mtime_sec, &tm_buf);
                        strftime(dbuf, sizeof(dbuf), "%m-%d %H:%M", &tm_buf);
                        ImGui::TextDisabled("%s", dbuf);
                    } else {
                        ImGui::TextDisabled("-");
                    }
                    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r->role);
                    ImGui::TableNextColumn(); ImGui::Text("%d", r->label_type);
                    ImGui::TableNextColumn();
                    if (r->expected_num_classes == 0)      ImGui::Text("binary");
                    else                                    ImGui::Text("%d-class", r->expected_num_classes);

                    ImGui::TableNextColumn();
                    if (r->label_tp_pct > 0.0f) ImGui::Text("%.1f", r->label_tp_pct * 100.0f);
                    else                         ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    if (r->label_sl_pct > 0.0f) ImGui::Text("%.1f", r->label_sl_pct * 100.0f);
                    else                         ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    if (r->label_lookahead_ticks > 0) ImGui::Text("%d", r->label_lookahead_ticks);
                    else                               ImGui::TextDisabled("-");

                    // v5.15.5 — Samples column (training data scale).
                    ImGui::TableNextColumn();
                    if (r->n_train_samples > 0) {
                        if (r->n_train_samples >= 1000000)
                            ImGui::Text("%.1fM", r->n_train_samples / 1e6);
                        else if (r->n_train_samples >= 1000)
                            ImGui::Text("%.1fk", r->n_train_samples / 1e3);
                        else
                            ImGui::Text("%d", r->n_train_samples);
                    } else {
                        ImGui::TextDisabled("-");
                    }

                    ImGui::TableNextColumn(); ImGui::Text("%.1f%%", r->train_accuracy);

                    ImGui::TableNextColumn();
                    if (r->has_wf_results) {
                        // Color thresholds depend on chance level: 3-class
                        // baseline ~33%, binary ~50%, plus majority-class
                        // dominance can shift these. Keep simple bands.
                        float thresh_low  = (r->expected_num_classes >= 2) ? 35.0f : 50.0f;
                        float thresh_good = (r->expected_num_classes >= 2) ? 50.0f : 60.0f;
                        ImVec4 vcol = (r->val_accuracy < thresh_low)  ? FoxmlColors::red
                                    : (r->val_accuracy < thresh_good) ? FoxmlColors::yellow
                                                                        : FoxmlColors::green;
                        ImGui::TextColored(vcol, "%.1f%%", r->val_accuracy);
                    } else {
                        ImGui::TextDisabled("-");
                    }

                    ImGui::TableNextColumn();
                    if (r->has_wf_results) {
                        ImVec4 gcol = (r->train_val_gap > 0.20f) ? FoxmlColors::red
                                    : (r->train_val_gap > 0.10f) ? FoxmlColors::yellow
                                                                  : FoxmlColors::green;
                        ImGui::TextColored(gcol, "%.3f", r->train_val_gap);
                    } else {
                        ImGui::TextDisabled("-");
                    }

                    ImGui::TableNextColumn();
                    if (r->has_wf_results) {
                        if (r->overfit_folds > 0)
                            ImGui::TextColored(FoxmlColors::red, "%d", r->overfit_folds);
                        else
                            ImGui::Text("0");
                    } else {
                        ImGui::TextDisabled("-");
                    }
                    // v5.11.55 — Overfit column tooltip
                    ImGui::SetItemTooltip(
                        "Number of WF folds where train_accuracy - val_accuracy\n"
                        "exceeded the overfit threshold (~3-5%% gap).\n\n"
                        "  0      = no folds overfit (model generalizes well)\n"
                        "  1      = 1 fold overfit (mostly generalizes; minor concern)\n"
                        "  2-3    = multiple folds overfit (concerning; review hyperparams)\n"
                        "  4-5    = ALL folds overfit (model memorized; lower n_estimators\n"
                        "           or max_depth, or add subsample/colsample regularization)\n\n"
                        "Red = >0 (any overfit). Goal: 0 across all folds.\n"
                        "See WalkForwardFoldResult.overfit_count in BacktestEngine.hpp.");

                    ImGui::TableNextColumn();
                    ImGui::Text("%d/%.2f/%d", r->max_depth, r->learning_rate, r->n_estimators);

                    // v5.9.5h #19 — stamp_ok column. Four states:
                    //   - missing:   dim "—" (no .stamp file in run dir)
                    //   - unverified: yellow "?" (present, not yet verified)
                    //   - verified OK: green "✓"
                    //   - verified FAIL: red "✗" (Verify Stamp shows reason)
                    ImGui::TableNextColumn();
                    if (!r->has_stamp) {
                        ImGui::TextDisabled("—");
                    } else if (r->stamp_verify_state == 1) {
                        ImGui::TextColored(ImVec4(0.55f, 0.76f, 0.51f, 1.0f), "✓");
                        ImGui::SetItemTooltip("Stamp verified OK\n"
                                              "Click 'Verify Stamp' below for full details.");
                    } else if (r->stamp_verify_state == -1) {
                        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "✗");
                        ImGui::SetItemTooltip("Stamp verification FAILED\n"
                                              "Click 'Verify Stamp' below for reason.");
                    } else {
                        ImGui::TextColored(ImVec4(0.85f, 0.80f, 0.50f, 1.0f), "?");
                        ImGui::SetItemTooltip("Stamp present, not yet verified\n"
                                              "Click 'Verify Stamp' below to check.");
                    }

                    // v5.15.5 — Delete button: set pending idx + open hoisted
                    // modal at window scope. Popup body lives below EndTabBar
                    // so it isn't scoped to this row's transient context.
                    ImGui::TableNextColumn();
                    ImGui::PushID(i);
                    if (ImGui::SmallButton("X")) {
                        s->pending_delete_idx = i;
                        // v5.15.5.F.6 — OpenPopup MOVED outside table scope (see
                        // below at "Hoisted OpenPopup" comment). Calling OpenPopup
                        // inside PushID(i) + BeginTable scope made the popup ID
                        // hash with the row's pushed ID + table context; the
                        // matching BeginPopupModal at parent-window scope computed
                        // a DIFFERENT hash → popup never appeared. Click handler
                        // now only sets pending state; the outer-scope hoist
                        // fires OpenPopup once when state becomes non-negative.
                    }
                    ImGui::SetItemTooltip("Delete this run (recursive)");
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // ============================================================
        // REGRESSION TAB — continuous-target models (forward P&L, etc.)
        // ============================================================
        char regr_label[64];
        snprintf(regr_label, sizeof(regr_label), "Regression (%d)", n_regr);
        if (ImGui::BeginTabItem(regr_label)) {
            if (n_regr == 0) {
                ImGui::TextDisabled("No regression runs saved yet.");
            } else if (ImGui::BeginTable("past_runs_regr", 15, flags)) {  // v5.11.55: +Date +Delete cols; v5.15.5.E.bugfix: 14→15 (Samples col added but BeginTable count missed; same off-by-one as past_runs_class)
                ImGui::TableSetupColumn("Run",        ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 220);
                ImGui::TableSetupColumn("Date",       ImGuiTableColumnFlags_WidthFixed, 100);  // v5.11.55
                ImGui::TableSetupColumn("Role",       ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Label",      ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("TP bps",     ImGuiTableColumnFlags_WidthFixed, 75);
                ImGui::TableSetupColumn("SL bps",     ImGuiTableColumnFlags_WidthFixed, 75);
                ImGui::TableSetupColumn("Lookahead",  ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Samples",    ImGuiTableColumnFlags_WidthFixed, 80);  // v5.15.5
                ImGui::TableSetupColumn("Train r",    ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Val r",      ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Val MSE",    ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableSetupColumn("Gap (r)",    ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Depth/LR/N", ImGuiTableColumnFlags_WidthFixed, 120);
                ImGui::TableSetupColumn("Stamp",      ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("",           ImGuiTableColumnFlags_WidthFixed, 30);  // v5.11.55 Delete
                ImGui::TableHeadersRow();

                for (int i = 0; i < s->count; ++i) {
                    PastRun *r = &s->runs[i];
                    if (r->label_kind != 1) continue;  // only regression
                    // v5.9.5i — stamp filter
                    if (s->stamp_filter == 1 && !r->has_stamp) continue;
                    if (s->stamp_filter == 2 && r->stamp_verify_state != 1) continue;
                    if (s->stamp_filter == 3 && r->stamp_verify_state != -1) continue;
                    if (s->stamp_filter == 4 && r->has_stamp) continue;
                    ImGui::TableNextRow();

                    render_run_cell(i);
                    // v5.11.55 — Date column for regression tab (parity with classification)
                    ImGui::TableNextColumn();
                    if (r->mtime_sec > 0) {
                        char dbuf[24];
                        struct tm tm_buf;
                        localtime_r(&r->mtime_sec, &tm_buf);
                        strftime(dbuf, sizeof(dbuf), "%m-%d %H:%M", &tm_buf);
                        ImGui::TextDisabled("%s", dbuf);
                    } else {
                        ImGui::TextDisabled("-");
                    }
                    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r->role);
                    ImGui::TableNextColumn(); ImGui::Text("%d", r->label_type);

                    ImGui::TableNextColumn();
                    if (r->label_tp_pct > 0.0f) ImGui::Text("%.1f", r->label_tp_pct * 100.0f);
                    else                         ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    if (r->label_sl_pct > 0.0f) ImGui::Text("%.1f", r->label_sl_pct * 100.0f);
                    else                         ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    if (r->label_lookahead_ticks > 0) ImGui::Text("%d", r->label_lookahead_ticks);
                    else                               ImGui::TextDisabled("-");

                    // v5.15.5 — Samples column (training data scale).
                    ImGui::TableNextColumn();
                    if (r->n_train_samples > 0) {
                        if (r->n_train_samples >= 1000000)
                            ImGui::Text("%.1fM", r->n_train_samples / 1e6);
                        else if (r->n_train_samples >= 1000)
                            ImGui::Text("%.1fk", r->n_train_samples / 1e3);
                        else
                            ImGui::Text("%d", r->n_train_samples);
                    } else {
                        ImGui::TextDisabled("-");
                    }

                    // Train r — for regression, train_accuracy field stores
                    // the in-sample correlation already (since training code
                    // sets state->train_correlation; "accuracy" field stays 0).
                    // Older runs may not have separate train_correlation
                    // captured — we show train_accuracy for now as a proxy.
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", r->train_accuracy / 100.0f);

                    ImGui::TableNextColumn();
                    if (r->has_wf_results) {
                        // Pearson r threshold bands for crypto-tick
                        // regression: |r|>0.10 is meaningful at this scale.
                        float ar = fabsf(r->val_correlation);
                        ImVec4 vcol = (ar < 0.05f) ? FoxmlColors::red
                                    : (ar < 0.10f) ? FoxmlColors::yellow
                                                    : FoxmlColors::green;
                        ImGui::TextColored(vcol, "%.3f", r->val_correlation);
                    } else {
                        ImGui::TextDisabled("-");
                    }

                    ImGui::TableNextColumn();
                    if (r->has_wf_results) ImGui::Text("%.5f", r->val_mse);
                    else                    ImGui::TextDisabled("-");

                    ImGui::TableNextColumn();
                    if (r->has_wf_results) ImGui::Text("%.3f", r->train_val_gap);
                    else                    ImGui::TextDisabled("-");

                    ImGui::TableNextColumn();
                    ImGui::Text("%d/%.2f/%d", r->max_depth, r->learning_rate, r->n_estimators);

                    // v5.9.5h #19 — stamp_ok column. Four states:
                    //   - missing:   dim "—" (no .stamp file in run dir)
                    //   - unverified: yellow "?" (present, not yet verified)
                    //   - verified OK: green "✓"
                    //   - verified FAIL: red "✗" (Verify Stamp shows reason)
                    ImGui::TableNextColumn();
                    if (!r->has_stamp) {
                        ImGui::TextDisabled("—");
                    } else if (r->stamp_verify_state == 1) {
                        ImGui::TextColored(ImVec4(0.55f, 0.76f, 0.51f, 1.0f), "✓");
                        ImGui::SetItemTooltip("Stamp verified OK\n"
                                              "Click 'Verify Stamp' below for full details.");
                    } else if (r->stamp_verify_state == -1) {
                        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "✗");
                        ImGui::SetItemTooltip("Stamp verification FAILED\n"
                                              "Click 'Verify Stamp' below for reason.");
                    } else {
                        ImGui::TextColored(ImVec4(0.85f, 0.80f, 0.50f, 1.0f), "?");
                        ImGui::SetItemTooltip("Stamp present, not yet verified\n"
                                              "Click 'Verify Stamp' below to check.");
                    }

                    // v5.15.5 — Delete button column (parity with classification);
                    // shares the hoisted modal at parent window scope.
                    ImGui::TableNextColumn();
                    ImGui::PushID(i);
                    if (ImGui::SmallButton("X")) {
                        s->pending_delete_idx = i;
                        // v5.15.5.F.6 — OpenPopup MOVED outside table scope (see
                        // below at "Hoisted OpenPopup" comment). Calling OpenPopup
                        // inside PushID(i) + BeginTable scope made the popup ID
                        // hash with the row's pushed ID + table context; the
                        // matching BeginPopupModal at parent-window scope computed
                        // a DIFFERENT hash → popup never appeared. Click handler
                        // now only sets pending state; the outer-scope hoist
                        // fires OpenPopup once when state becomes non-negative.
                    }
                    ImGui::SetItemTooltip("Delete this run (recursive)");
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // v5.15.5 — Hoisted delete-confirm modal (shared by both tabs).
    // Sits at parent window scope so it isn't scoped to a table cell's
    // transient context (which caused the v5.11.51/v5.11.55 "peach flash,
    // no popup" bug).
    //
    // v5.15.5.F.6 — Hoisted OpenPopup too. The v5.11.51 fix only hoisted the
    // popup BODY; the OpenPopup call stayed inside PushID(i) + BeginTable
    // scope where it hashed with a DIFFERENT ID context than the
    // BeginPopupModal at this outer window scope. Net: click "X", state
    // updates, popup never opens. Hoisting OpenPopup here (same scope as
    // BeginPopupModal) makes the IDs match.
    //
    // Idempotent: ImGui::OpenPopup is a no-op when the popup is already open
    // (IsPopupOpen guard not strictly needed, but explicit guards make the
    // single-shot semantic obvious).
    if (s->pending_delete_idx >= 0 && !ImGui::IsPopupOpen("##DeleteConfirmModal")) {
        ImGui::OpenPopup("##DeleteConfirmModal");
    }
    if (ImGui::BeginPopupModal("##DeleteConfirmModal", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (s->pending_delete_idx >= 0 && s->pending_delete_idx < s->count) {
            PastRun *dr = &s->runs[s->pending_delete_idx];
            ImGui::Text("Delete %s?", dr->dir_name);
            ImGui::TextDisabled("(removes %s recursively)", dr->full_path);
            ImGui::Separator();
            if (ImGui::Button("Delete")) {
                int rc = PastRuns_DeleteDir(dr->full_path);
                if (rc == 0) {
                    snprintf(s->status_msg, sizeof(s->status_msg),
                             "deleted: %s", dr->full_path);
                } else {
                    snprintf(s->status_msg, sizeof(s->status_msg),
                             "delete FAILED: %s (errno=%d)",
                             dr->full_path, errno);
                }
                PastRuns_Scan(s);
                s->pending_delete_idx = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                s->pending_delete_idx = -1;
                ImGui::CloseCurrentPopup();
            }
        } else {
            // pending_delete_idx invalidated by a rescan — just close
            s->pending_delete_idx = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // detail / action area for the selected run
    if (s->selected >= 0 && s->selected < s->count) {
        PastRun *r = &s->runs[s->selected];
        ImGui::Separator();
        ImGui::TextColored(FoxmlColors::primary, "Selected: %s", r->dir_name);
        ImGui::TextColored(FoxmlColors::comment,
            "Role=%s  label_type=%d  num_classes=%d  held_out=%.2f  gap_threshold=%.2f",
            r->role, r->label_type, r->expected_num_classes,
            r->held_out_fraction, r->gap_acceptable_threshold);

        // Path hint for engine.cfg — v5.11.55 use full_path so kind-
        // organized subdirs (classification/, regression/) appear correctly.
        ImGui::TextColored(FoxmlColors::sand,
            "To use in engine: set node_N_model_dir=%s/ in engine.cfg",
            r->full_path);

        // v5.15.5 — Open Folder Path actually opens the directory via
        // xdg-open. Pre-fix only wrote the path to status_msg, leaving
        // the operator to copy/paste manually. The path comes from a
        // filesystem scan (PastRuns_LoadOne stat'd it) so shell injection
        // risk is bounded; still pass via fork+exec rather than system()
        // so spaces / quotes in pathnames don't need escaping.
        if (ImGui::Button("Open Folder Path")) {
            pid_t pid = fork();
            if (pid == 0) {
                execlp("xdg-open", "xdg-open", r->full_path, (char*)nullptr);
                _exit(127);  // exec failed
            } else if (pid > 0) {
                snprintf(s->status_msg, sizeof(s->status_msg),
                         "opened: %s/", r->full_path);
            } else {
                snprintf(s->status_msg, sizeof(s->status_msg),
                         "fork() failed for xdg-open %s/", r->full_path);
            }
        }
        ImGui::SameLine();
        // v5.15.5 — Copy Path replaces the redundant "Delete (manual)"
        // button (X column on the row already does real delete-with-confirm
        // via PastRuns_DeleteDir). Copy is useful for dropping the path
        // into a cfg, terminal, or `rm -r` manually.
        if (ImGui::Button("Copy Path")) {
            ImGui::SetClipboardText(r->full_path);
            snprintf(s->status_msg, sizeof(s->status_msg),
                     "copied to clipboard: %s", r->full_path);
        }

        // v5.8.9 — Verify Stamp: runs verify_model_stamp on the saved
        // model's .stamp file using the current build's
        // FEATURE_REGISTRY_HASH() so the operator can confirm match
        // (signature valid + format version + drift hash) before
        // deploying. Same code path the live engine fires at boot.
        if (r->has_stamp) {
            ImGui::SameLine();
            if (ImGui::Button("Verify Stamp")) {
                // v5.11.55 — use r->full_path (the actual scanned dir under
                // models/<class>/...) instead of synthesizing "models/<dir_name>".
                // Pre-fix synthesized path missed the "classification/" or
                // "regression/" intermediate dir that v4.3+ Save Run +
                // v5.11.41.A Multi-Horizon use, so Verify Stamp always
                // failed with "no model file found in models/<dir>/" for
                // any kind-organized run.
                // E.1.2.D leaf 11 (S2-F7) — verify EVERY role present, not the
                // first found. Post-E.1.2.C exit models land CO-LOCATED, so a
                // buy+exit dir is the norm; the old first-match `break`
                // verified barrier.json and never looked at exit.json's
                // stamp, while the picker's Settings_VerifyBundleStamps loops
                // ALL roles — the two verify surfaces disagreed on the very
                // same dir. Rules now match the picker's: .json roles verify;
                // an .xgb-only role is counted-skipped (stamps ride the .json
                // convention); the row verdict AGGREGATES across roles, and
                // the details expansion carries the first valid role's stamp.
                static const char *vs_json_roles[] = {
                    "barrier.json", "buy_signal.json", "regime.json", "exit.json" };
                static const char *vs_xgb_roles[] = {
                    "barrier.xgb",  "buy_signal.xgb",  "regime.xgb",  "exit.xgb" };
                // v5.11.57 — use cfg.auto_stamp_secret if available
                // (caller passed cfg_for_verify). Empty fallback =
                // devmode (accepts any signature). Operator's engine
                // load uses the same secret; matching path here means
                // suite-side verify reflects what the engine will do.
                const char *verify_secret =
                    (cfg_for_verify && cfg_for_verify->auto_stamp_secret[0])
                        ? cfg_for_verify->auto_stamp_secret
                        : "";
                char model_path[640];
                int n_checked = 0, n_ok = 0, n_xgb_only = 0;
                char fail_role[20] = {0};
                char fail_reason[80] = {0};
                ModelStampResult first_ok_vr{};
                int have_first_ok = 0;
                for (int i = 0; i < 4; ++i) {
                    snprintf(model_path, sizeof(model_path), "%s/%s",
                             r->full_path, vs_json_roles[i]);
                    struct stat mst;
                    if (stat(model_path, &mst) != 0) {
                        snprintf(model_path, sizeof(model_path), "%s/%s",
                                 r->full_path, vs_xgb_roles[i]);
                        if (stat(model_path, &mst) == 0) n_xgb_only++;
                        continue;
                    }
                    ModelStampResult vr = verify_model_stamp(
                        model_path, /*secret=*/verify_secret,
                        /*gap_threshold=*/(double)r->gap_acceptable_threshold > 0.0
                            ? (double)r->gap_acceptable_threshold : 0.05,
                        /*expected_format_version=*/MODEL_FORMAT_VERSION,
                        /*expected_feature_registry_hash=*/FEATURE_REGISTRY_HASH(),
                        /*expected_label_registry_hash=*/LABEL_REGISTRY_HASH());  // v5.10.1.A — close Finding #1 consume side (UI Verify Stamp)
                    n_checked++;
                    if (vr.valid == 1) {
                        n_ok++;
                        // v5.9.5d — details expansion payload (first valid
                        // role; barrier/buy probe first, so this is the
                        // primary in practice).
                        if (!have_first_ok) { first_ok_vr = vr; have_first_ok = 1; }
                    } else if (!fail_role[0]) {
                        snprintf(fail_role, sizeof(fail_role), "%s", vs_json_roles[i]);
                        snprintf(fail_reason, sizeof(fail_reason), "%s", vr.reason);
                    }
                }
                if (n_checked == 0) {
                    if (n_xgb_only) {
                        snprintf(r->stamp_verify_msg, sizeof(r->stamp_verify_msg),
                            "no verifiable .json role in %s/ (%d .xgb-only)",
                            r->full_path, n_xgb_only);
                    } else {
                        snprintf(r->stamp_verify_msg, sizeof(r->stamp_verify_msg),
                            "no model file found in %s/", r->full_path);
                    }
                    r->stamp_verify_state = -1;
                    r->stamp_verify_has_full = 0;
                } else if (n_ok == n_checked) {
                    r->stamp_verify_state = 1;
                    r->stamp_verify_full = first_ok_vr;
                    r->stamp_verify_has_full = 1;
                    // v5.11.57 — secret-aware OK message; devmode caveat so
                    // operator knows engine load is the real gate.
                    const char *mode_str = verify_secret[0]
                        ? "signature verified"
                        : "devmode, signature UNVERIFIED — set auto_stamp_secret in engine.cfg";
                    snprintf(r->stamp_verify_msg, sizeof(r->stamp_verify_msg),
                        "OK %d/%d roles (%s) — engine=%s registry=%016lx",
                        n_ok, n_checked, mode_str,
                        first_ok_vr.engine_version[0] ? first_ok_vr.engine_version
                                                      : "unknown",
                        (unsigned long)first_ok_vr.feature_registry_hash);
                } else {
                    r->stamp_verify_state = 0;
                    r->stamp_verify_has_full = 0;  // FAIL reason IS the message
                    snprintf(r->stamp_verify_msg, sizeof(r->stamp_verify_msg),
                        "%d/%d roles OK; %s FAIL — %s",
                        n_ok, n_checked, fail_role, fail_reason);
                }
            }
        }

        // Render verify result if button has been pressed for this run.
        if (r->stamp_verify_msg[0]) {
            ImVec4 vc = (r->stamp_verify_state == 1)
                ? ImVec4(0.55f, 0.76f, 0.51f, 1.0f)
                : ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
            ImGui::TextColored(vc, "%s", r->stamp_verify_msg);

            // v5.9.5d — Stamp details expansion. Renders all recorded
            // body fields when the stamp verifies. Lets operator audit
            // recorded inference cfg vs runtime cfg without manually
            // reading the .stamp file. Only shown on successful verify
            // (FAIL case is already self-explanatory via the reason).
            if (r->stamp_verify_has_full) {
                const ModelStampResult &v = r->stamp_verify_full;
                char tree_id[64];
                snprintf(tree_id, sizeof(tree_id), "Stamp details##%s",
                         r->dir_name);
                if (ImGui::TreeNode(tree_id)) {
                    // Generalization metrics
                    ImGui::Text("gap:               %.4f  (threshold: %.4f)",
                                v.generalization_gap, v.gap_threshold);
                    ImGui::Text("model_format_ver:  %d  (stamp_schema: %d)",
                                v.model_format_version, v.stamp_format_version);
                    // Cross-build identifiers
                    ImGui::Separator();
                    ImGui::Text("engine_version:    %s",
                                v.engine_version[0] ? v.engine_version : "(unknown)");
                    ImGui::Text("registry_hash:     %016lx",
                                (unsigned long)v.feature_registry_hash);
                    if (v.cross_major_engine) {
                        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f),
                                           "  (cross-major-engine WARN)");
                    }
                    // v5.9.4a model_num_outputs (output dimension)
                    if (STAMP_HAS(v, model_num_outputs)) {
                        ImGui::Text("model_num_outputs: %d", v.model_num_outputs);
                    }
                    // v5.9.4a training_poll_interval (cadence)
                    if (STAMP_HAS(v, training_poll_interval)) {
                        ImGui::Text("training_poll:    %u",
                                    (unsigned)v.training_poll_interval);
                    }
                    // v5.9.3a scaler binding
                    if (STAMP_HAS(v, scaler)) {
                        ImGui::Separator();
                        ImGui::Text("scaler_present:    %d",
                                    v.feature_scaler_present);
                        if (v.feature_scaler_present && v.scaler_sha256[0]) {
                            ImGui::Text("scaler_sha256:");
                            ImGui::SameLine();
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                ImVec4(0.85f, 0.80f, 0.60f, 1.0f));
                            ImGui::PushItemWidth(-1);
                            char tmp_id[80];
                            snprintf(tmp_id, sizeof(tmp_id), "##sha_%s",
                                     r->dir_name);
                            // Cast away const — InputText with ReadOnly is
                            // display-only, doesn't mutate.
                            ImGui::InputText(tmp_id,
                                              (char*)v.scaler_sha256,
                                              sizeof(v.scaler_sha256),
                                              ImGuiInputTextFlags_ReadOnly);
                            ImGui::PopItemWidth();
                            ImGui::PopStyleColor();
                        }
                    }
                    // v5.9.2b inference cfg block
                    if (STAMP_HAS(v, inference_cfg)) {
                        ImGui::Separator();
                        ImGui::TextColored(FoxmlColors::comment,
                                           "Recorded cfg at training time:");
                        ImGui::Text("  confidence_threshold_scale:       %.4g",
                                    v.confidence_threshold_scale);
                        ImGui::Text("  barrier_gate_enabled:             %d",
                                    v.barrier_gate_enabled);
                        ImGui::Text("  confidence_hard_block_threshold:  %.4g",
                                    v.confidence_hard_block_threshold);
                        ImGui::Text("  held_out_fraction:                %.3f",
                                    v.held_out_fraction);
                        // v5.14.9.D — DELETED freshness_tau display
                        // (TECH_DEBT-004 close); registry entry + struct field
                        // deleted; stamp body line no longer emitted.
                        // 2026-08-17 (D-426) — the bandit_blend_ratio display was REMOVED with its
                        // wire key, for the SAME reason the fee-rate display below it was: it
                        // rendered a permanently-zero field as the model's training-time setting.
                        // Every model stamped with bandit_enabled=1 showed `bandit_blend_ratio: 0`
                        // here while the truthful cfg-derived value said otherwise. Found while
                        // SCOPING the row deletion, not by the sweep that removed the fee-rate
                        // twin — the third repeat of one pattern on this surface (fees emit ->
                        // bandit emit -> bandit DISPLAY), which is why the display half now gets
                        // enumerated with the emit half rather than after it.
                        // 2026-08-16 — the fee-rate display was REMOVED with the `fees`
                        // group. It rendered two permanently-zero fields as the model's
                        // training-time fees; the panel showed 0.00000 / 0.00000 while the
                        // same stamp body carried the real rates under the canonical
                        // cfg-derived keys. Showing nothing beats showing a confident zero.
                    }
                    // v5.9.5h — XGBoost hyperparameter group. Renders when
                    // stamp had has_xgb_hyperparams=1 (post-v5.9.5h stamps).
                    if (STAMP_HAS(v, xgb_hyperparams)) {
                        ImGui::Separator();
                        ImGui::TextColored(FoxmlColors::comment,
                                           "XGBoost hyperparams at training time:");
                        ImGui::Text("  max_depth:          %d",   v.xgb_max_depth);
                        ImGui::Text("  learning_rate:      %.4f", v.xgb_learning_rate);
                        ImGui::Text("  n_estimators:       %d",   v.xgb_n_estimators);
                        ImGui::Text("  subsample:          %.2f", v.xgb_subsample);
                        ImGui::Text("  colsample_bytree:   %.2f", v.xgb_colsample_bytree);
                        ImGui::Text("  min_child_weight:   %d",   v.xgb_min_child_weight);
                        ImGui::Text("  seed:               %d",   v.xgb_seed);
                        ImGui::Text("  tree_method:        %s",
                                    v.xgb_tree_method[0] ? v.xgb_tree_method : "(unknown)");
                    }
                    ImGui::TreePop();
                }
            }
        }
    }

    ImGui::End();
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GUI_Panel_PastRuns]
//======================================================================

//======================================================================
// [FUNCTION]_[Comparison_Init]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[init the Comparison state]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void Comparison_Init(ComparisonState *state) {
    memset(state, 0, sizeof(*state));
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Comparison_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[Comparison_Free]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[free the Comparison saved-run buffers]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void Comparison_Free(ComparisonState *state) {
    for (int i = 0; i < COMPARISON_MAX_RUNS; i++) {
        free(state->equity_curves[i]);
        state->equity_curves[i] = NULL;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Comparison_Free]
//======================================================================

//======================================================================
// [FUNCTION]_[Comparison_SaveRun]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[save the current results into a Comparison slot]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void Comparison_SaveRun(ComparisonState *state, const BacktestResults *results,
                                       const char *label) {
    if (state->run_count >= COMPARISON_MAX_RUNS) {
        // drop oldest, shift the rest down. free the oldest's buffer first
        // so we don't leak when the slot gets overwritten.
        free(state->equity_curves[0]);
        memmove(&state->stats[0], &state->stats[1],
                (COMPARISON_MAX_RUNS - 1) * sizeof(BacktestStats));
        memmove(&state->equity_curves[0], &state->equity_curves[1],
                (COMPARISON_MAX_RUNS - 1) * sizeof(state->equity_curves[0]));
        memmove(&state->equity_counts[0], &state->equity_counts[1],
                (COMPARISON_MAX_RUNS - 1) * sizeof(int));
        memmove(&state->labels[0], &state->labels[1],
                (COMPARISON_MAX_RUNS - 1) * sizeof(state->labels[0]));
        // tail is now duplicated by the memmove; clear the old tail pointer
        state->equity_curves[COMPARISON_MAX_RUNS - 1] = NULL;
        state->run_count = COMPARISON_MAX_RUNS - 1;
    }
    int idx = state->run_count;
    state->stats[idx] = results->stats;
    int ec = results->equity_count;
    // free any previous snapshot in this slot, then allocate fresh of exact size
    free(state->equity_curves[idx]);
    state->equity_curves[idx] = NULL;
    if (ec > 0) {
        state->equity_curves[idx] = (double *)malloc(ec * sizeof(double));
        if (state->equity_curves[idx]) {
            memcpy(state->equity_curves[idx], results->equity_curve, ec * sizeof(double));
        } else {
            ec = 0;
        }
    }
    state->equity_counts[idx] = ec;
    strncpy(state->labels[idx], label, 63);
    state->labels[idx][63] = '\0';
    state->run_count++;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Comparison_SaveRun]
//======================================================================

//======================================================================
// [FUNCTION]_[GUI_Panel_Comparison]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[render the Comparison panel — side-by-side saved runs]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void GUI_Panel_Comparison(ComparisonState *state, const BacktestResults *current) {
    ImGui::Begin("Comparison");

    // save current run
    if (current->stats.total_trades > 0) {
        static char save_label[64] = "Run";
        ImGui::InputText("Label", save_label, sizeof(save_label));
        ImGui::SameLine();
        if (ImGui::Button("Save Run")) {
            // auto-number if label is default
            char label[64];
            if (strcmp(save_label, "Run") == 0)
                snprintf(label, sizeof(label), "Run %d", state->run_count + 1);
            else
                strncpy(label, save_label, sizeof(label) - 1);
            Comparison_SaveRun(state, current, label);
        }
    }

    if (state->run_count == 0) {
        ImGui::TextDisabled("No saved runs yet. Complete a backtest and click Save Run.");
        ImGui::End();
        return;
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear All")) {
        Comparison_Init(state);
        ImGui::End();
        return;
    }

    ImGui::Separator();

    // equity curve overlay
    static const ImVec4 run_colors[] = {
        {0.55f, 0.76f, 0.51f, 1.0f},  // green
        {0.53f, 0.66f, 0.82f, 1.0f},  // blue
        {0.82f, 0.62f, 0.47f, 1.0f},  // orange
        {0.76f, 0.51f, 0.76f, 1.0f},  // purple
        {0.82f, 0.82f, 0.47f, 1.0f},  // yellow
        {0.47f, 0.82f, 0.82f, 1.0f},  // cyan
        {0.82f, 0.47f, 0.47f, 1.0f},  // red
        {0.75f, 0.75f, 0.55f, 1.0f},  // sand
    };

    if (ImPlot::BeginPlot("Equity Comparison", ImVec2(-1, 200))) {
        ImPlot::SetupAxes("Trade #", "$");
        // single reusable x-axis buffer — sized to the largest run, grown as needed.
        // static so we don't malloc/free on every redraw frame.
        static double *xs = NULL;
        static int xs_capacity = 0;
        int xs_needed = 0;
        for (int r = 0; r < state->run_count; r++) {
            if (state->equity_counts[r] > xs_needed) xs_needed = state->equity_counts[r];
        }
        if (xs_needed > xs_capacity) {
            xs = (double *)realloc(xs, xs_needed * sizeof(double));
            xs_capacity = xs ? xs_needed : 0;
            // (re)fill x-axis identity values up to new capacity
            for (int i = 0; i < xs_capacity; i++) xs[i] = (double)i;
        }
        for (int r = 0; r < state->run_count; r++) {
            int n = state->equity_counts[r];
            if (n < 2 || !state->equity_curves[r] || !xs) continue;
            ImPlotSpec ls;
            ls.LineColor = run_colors[r % 8];
            ls.LineWeight = 2.0f;
            ImPlot::PlotLine(state->labels[r], xs, state->equity_curves[r], n, ls);
        }
        ImPlot::EndPlot();
    }

    ImGui::Separator();

    // stats comparison table
    if (ImGui::BeginTable("cmp", state->run_count + 1,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV |
                          ImGuiTableFlags_ScrollX)) {
        ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthFixed, 110);
        for (int r = 0; r < state->run_count; r++)
            ImGui::TableSetupColumn(state->labels[r], ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableHeadersRow();

        auto cmp_row = [&](const char *label, auto fn) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", label);
            for (int r = 0; r < state->run_count; r++) {
                ImGui::TableNextColumn();
                fn(r);
            }
        };

        cmp_row("P&L", [&](int r) {
            ImGui::TextColored(ResultsPnlColor(state->stats[r].total_pnl),
                               "$%.2f", state->stats[r].total_pnl);
        });
        cmp_row("Return %", [&](int r) {
            ImGui::TextColored(ResultsPnlColor(state->stats[r].return_pct),
                               "%.2f%%", state->stats[r].return_pct);
        });
        cmp_row("Trades", [&](int r) {
            ImGui::Text("%u", state->stats[r].total_trades);
        });
        cmp_row("Win Rate", [&](int r) {
            ImGui::Text("%.1f%%", state->stats[r].win_rate);
        });
        cmp_row("PF", [&](int r) {
            ImGui::Text("%.2f", state->stats[r].profit_factor);
        });
        cmp_row("Expectancy", [&](int r) {
            ImGui::TextColored(ResultsPnlColor(state->stats[r].expectancy),
                               "$%.2f", state->stats[r].expectancy);
        });
        cmp_row("Max DD", [&](int r) {
            ImGui::Text("%.2f%%", state->stats[r].max_drawdown_pct);
        });
        cmp_row("Sharpe", [&](int r) {
            ImGui::Text("%.2f", state->stats[r].sharpe_ratio);
        });
        cmp_row("Fees", [&](int r) {
            ImGui::Text("$%.2f", state->stats[r].total_fees);
        });

        ImGui::EndTable();
    }

    ImGui::End();
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GUI_Panel_Comparison]
//======================================================================

//======================================================================
// [STRUCT]_[OptimizerPanelState]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [THREAD]_[[OPT_WORKER_WRITER] [GUI_READER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[state for the Optimizer panel — the two sweep ranges + the results grid + the worker]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
struct OptimizerPanelState {
    OptimizerRange ranges[OPT_MAX_PARAMS];
    int num_params;
    int metric_idx;
    OptimizerResults results;
    volatile int running;
    volatile int current_run;
    volatile int total_runs;
    volatile int cancel_flag;
    volatile int complete;
    pthread_t worker_tid;
    // copies for the worker thread
    BacktestRunConfig run_config;
    char config_path[256];
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-20]
// [SIZE]_[959104B]
// [ALIGN]_[64]
// [CACHE_LINES]_[14986]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[OptimizerPanelState]
//======================================================================

//======================================================================
// [FUNCTION]_[OptimizerPanel_Init]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[init the Optimizer panel state with default sweep ranges]
//======================================================================
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-123]
//======================================================================
// [CODE]
//======================================================================
static inline void OptimizerPanel_Init(OptimizerPanelState *state) {
    memset(state, 0, sizeof(*state));
    state->num_params = 1;
    state->metric_idx = OPT_METRIC_PNL;
    strncpy(state->ranges[0].key, "take_profit_pct", 31);
    state->ranges[0].lo = 1.0; state->ranges[0].hi = 5.0; state->ranges[0].step = 0.5;
    strncpy(state->ranges[1].key, "stop_loss_pct", 31);
    state->ranges[1].lo = 0.5; state->ranges[1].hi = 3.0; state->ranges[1].step = 0.5;
    // v5.15.5.F.4d.1.B.3 Step 6.9 (2026-05-24) — closes foxml_suite Optimizer-vs-RunControl
    // divergence. Pre-fix: OptimizerPanel defaulted to "engine.cfg" while RunControl_Init:160
    // defaulted to "backtest.cfg" — two suite-internal panels loaded DIFFERENT cfg files.
    // foxml_suite agent CRIT-3 finding 2026-05-24. Fix: 1-line "engine.cfg" → "backtest.cfg"
    // restores parity between suite panels. (Note: backtest.cfg/engine.cfg structural drift
    // closes separately at v5.15.6.A/B/C per TECH_DEBT-123.)
    strncpy(state->config_path, "backtest.cfg", sizeof(state->config_path) - 1);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OptimizerPanel_Init]
//======================================================================

struct OptWorkerArgs {
    OptimizerPanelState *state;
};

//======================================================================
// [FUNCTION]_[optimizer_worker_fn]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[background thread: run a parameter sweep]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void *optimizer_worker_fn(void *arg) {
    OptWorkerArgs *args = (OptWorkerArgs *)arg;
    OptimizerPanelState *state = args->state;
    free(args);

    Backtest_RunSweep(&state->results, &state->run_config,
                       state->ranges, state->num_params, state->metric_idx,
                       &state->current_run, &state->total_runs, &state->cancel_flag);

    state->complete = 1;
    state->running = 0;
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[optimizer_worker_fn]
//======================================================================

//======================================================================
// [FUNCTION]_[GUI_Panel_Optimizer]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[render the Optimizer panel — sweep ranges, the results grid, and the best cell]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void GUI_Panel_Optimizer(OptimizerPanelState *state, DataPanelState *data) {
    ImGui::Begin("Optimizer");

    // parameter config
    static const char *metric_names[] = {"Sharpe", "Profit Factor", "Expectancy", "Return %", "P&L $"};
    ImGui::Combo("Metric", &state->metric_idx, metric_names, 5);

    ImGui::SliderInt("Parameters", &state->num_params, 1, 2);

    // v5.10.0a — sweepable cfg key picker. Supports the cfg fields recognized
    // by ConfigField_Set (BacktestEngine.hpp). Operator selects from
    // dropdown to fill state->ranges[p].key; InputText still editable for
    // operators who know which obscure key they want.
    static const char* sweep_keys[] = {
        // Existing pre-v5.10
        "take_profit_pct", "stop_loss_pct", "fee_rate", "entry_offset_pct",
        "slippage_pct", "max_exposure_pct", "risk_pct", "max_drawdown_pct",
        "offset_stddev_mult", "spacing_multiplier",
        "momentum_breakout_mult", "momentum_tp_mult", "momentum_sl_mult",
        "tp_hold_score", "tp_trail_mult", "sl_trail_mult", "no_trade_band_mult",
        "ml_buy_threshold", "danger_warn_stddevs", "danger_crash_stddevs",
        "poll_interval", "warmup_ticks", "max_hold_ticks", "sl_cooldown_base",
        // v5.10.0a — XGBoost hyperparam sweeping (cfg-bound since v5.9.5h
        // + v5.10.0D thread counts). Common operator targets:
        //   xgb_subsample / xgb_colsample_bytree → tree regularization
        //   xgb_min_child_weight → leaf-purity gate
        //   xgb_seed → reproducibility check (sweep multiple seeds, look at
        //              variance to detect overfit-to-seed)
        "xgb_subsample", "xgb_colsample_bytree", "xgb_min_child_weight",
        "xgb_seed", "xgb_train_nthread", "xgb_eval_nthread"
    };
    constexpr int sweep_keys_count = (int)(sizeof(sweep_keys) / sizeof(sweep_keys[0]));

    for (int p = 0; p < state->num_params; p++) {
        ImGui::PushID(p);
        char hdr[32]; snprintf(hdr, sizeof(hdr), "Param %d", p + 1);
        if (ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen)) {
            // v5.10.0a — dropdown that fills the Key InputText on click.
            // Compute current selection by matching state->ranges[p].key
            // against the dropdown list; if not found, sentinel "(custom)"
            // shows the operator they typed something off-list.
            int sel_idx = -1;
            for (int k = 0; k < sweep_keys_count; ++k) {
                if (strcmp(state->ranges[p].key, sweep_keys[k]) == 0) {
                    sel_idx = k;
                    break;
                }
            }
            int combo_idx = sel_idx;  // -1 indicates custom / unmatched
            if (ImGui::BeginCombo("Quick-pick",
                                  sel_idx >= 0 ? sweep_keys[sel_idx] : "(custom)",
                                  0)) {
                for (int k = 0; k < sweep_keys_count; ++k) {
                    bool sel = (k == combo_idx);
                    if (ImGui::Selectable(sweep_keys[k], sel)) {
                        strncpy(state->ranges[p].key, sweep_keys[k],
                                sizeof(state->ranges[p].key) - 1);
                        state->ranges[p].key[sizeof(state->ranges[p].key) - 1] = '\0';
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip(
                "Pre-filled list of cfg fields ConfigField_Set knows.\n"
                "Selecting fills the Key field below; operator can also\n"
                "type any cfg field name that ConfigField_Set recognizes.");

            ImGui::InputText("Key", state->ranges[p].key, 32);
            ImGui::InputDouble("Min", &state->ranges[p].lo, 0.1, 1.0, "%.2f");
            ImGui::InputDouble("Max", &state->ranges[p].hi, 0.1, 1.0, "%.2f");
            ImGui::InputDouble("Step", &state->ranges[p].step, 0.1, 0.5, "%.2f");
            int steps = state->ranges[p].steps();
            ImGui::Text("%d steps", steps);
        }
        ImGui::PopID();
    }

    int total_combos = state->ranges[0].steps() * (state->num_params > 1 ? state->ranges[1].steps() : 1);
    ImGui::Text("Total combinations: %d", total_combos);

    ImGui::Separator();

    if (state->running) {
        float pct = state->total_runs > 0 ? (float)state->current_run / state->total_runs : 0.0f;
        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%d / %d", (int)state->current_run, (int)state->total_runs);
        ImGui::ProgressBar(pct, ImVec2(-1, 0), overlay);
        if (ImGui::Button("Cancel"))
            state->cancel_flag = 1;
    } else {
        bool can_run = data->selected_count > 0 && total_combos > 0 && total_combos <= OPT_MAX_GRID;
        if (!can_run) ImGui::BeginDisabled();
        if (ImGui::Button("Run Grid Search")) {
            // build run config from data selection
            state->run_config.num_data_files = 0;
            for (int i = 0; i < data->file_count && state->run_config.num_data_files < MAX_DATA_FILES; i++) {
                if (data->selected[i]) {
                    strncpy(state->run_config.data_paths[state->run_config.num_data_files],
                            data->files[i], 255);
                    state->run_config.num_data_files++;
                }
            }
            strncpy(state->run_config.config_path, state->config_path, 255);
            state->run_config.use_config_override = 0;
            state->run_config.collect_features = 0;

            state->cancel_flag = 0;
            state->complete = 0;
            state->running = 1;

            OptWorkerArgs *args = (OptWorkerArgs *)malloc(sizeof(OptWorkerArgs));
            args->state = state;
            pthread_create(&state->worker_tid, NULL, optimizer_worker_fn, args);
            pthread_detach(state->worker_tid);
        }
        if (!can_run) {
            ImGui::EndDisabled();
            if (data->selected_count == 0)
                ImGui::SameLine(), ImGui::TextDisabled("Select data files first");
            else if (total_combos > OPT_MAX_GRID)
                ImGui::SameLine(), ImGui::TextDisabled("Too many combos (max %d)", OPT_MAX_GRID);
        }
    }

    // results
    if (state->complete && state->results.total_runs > 0) {
        ImGui::Separator();
        OptimizerResults *r = &state->results;

        // best result header
        int bi = r->best_idx;
        ImGui::TextColored(ResultsPnlColor(r->stats[bi].total_pnl),
                           "Best: %s=%.2f", state->ranges[0].key,
                           r->param_vals[0][bi / r->dims[1]]);
        if (r->num_params > 1)
            ImGui::SameLine(), ImGui::Text(" %s=%.2f", state->ranges[1].key,
                                            r->param_vals[1][bi % r->dims[1]]);
        ImGui::Text("P&L $%.2f  |  Sharpe %.2f  |  WR %.1f%%  |  PF %.2f",
                     r->stats[bi].total_pnl, r->stats[bi].sharpe_ratio,
                     r->stats[bi].win_rate, r->stats[bi].profit_factor);

        // 1D: bar chart
        if (r->num_params == 1) {
            if (ImPlot::BeginPlot("Sweep", ImVec2(-1, 200))) {
                ImPlot::SetupAxes(state->ranges[0].key, metric_names[state->metric_idx]);
                ImPlot::PlotBars("##metric", r->param_vals[0], r->metric, r->dims[0], 0.6);
                ImPlot::EndPlot();
            }
        }

        // 2D: heatmap
        if (r->num_params == 2) {
            if (ImPlot::BeginPlot("Heatmap", ImVec2(-1, 250))) {
                ImPlot::SetupAxes(state->ranges[0].key, state->ranges[1].key);
                ImPlot::PlotHeatmap("##heat", r->metric, r->dims[1], r->dims[0],
                                     0, 0, NULL,
                                     ImPlotPoint(r->param_vals[0][0], r->param_vals[1][0]),
                                     ImPlotPoint(r->param_vals[0][r->dims[0]-1],
                                                 r->param_vals[1][r->dims[1]-1]));
                ImPlot::EndPlot();
            }
        }

        // top-N table
        ImGui::Separator();
        ImGui::Text("Top Results:");
        if (ImGui::BeginTable("opt_results", 5 + r->num_params,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV |
                              ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY,
                              ImVec2(0, 200))) {
            ImGui::TableSetupColumn(state->ranges[0].key, ImGuiTableColumnFlags_WidthFixed, 70);
            if (r->num_params > 1)
                ImGui::TableSetupColumn(state->ranges[1].key, ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("P&L", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("WR%", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("PF", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Sharpe", ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Trades", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableHeadersRow();

            // sort by metric (descending)
            int sorted[OPT_MAX_GRID];
            for (int i = 0; i < r->total_runs; i++) sorted[i] = i;
            for (int i = 0; i < r->total_runs - 1; i++)
                for (int j = i + 1; j < r->total_runs; j++)
                    if (r->metric[sorted[j]] > r->metric[sorted[i]]) {
                        int tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp;
                    }

            int show = r->total_runs < 20 ? r->total_runs : 20;
            for (int si = 0; si < show; si++) {
                int idx = sorted[si];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", r->param_vals[0][idx / r->dims[1]]);
                if (r->num_params > 1) {
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", r->param_vals[1][idx % r->dims[1]]);
                }
                ImGui::TableNextColumn();
                ImGui::TextColored(ResultsPnlColor(r->stats[idx].total_pnl),
                                   "$%.2f", r->stats[idx].total_pnl);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", r->stats[idx].win_rate);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", r->stats[idx].profit_factor);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", r->stats[idx].sharpe_ratio);
                ImGui::TableNextColumn(); ImGui::Text("%u", r->stats[idx].total_trades);
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GUI_Panel_Optimizer]
//======================================================================

//======================================================================
// [STRUCT]_[TrainingPanelState]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [THREAD]_[[TRAIN_WORKER_WRITER] [GUI_READER]]
// [STRADDLE_EXEMPT]_[mh_horizon_ticks]_[GUI-thread-only display snapshot (click-write + render-read; the train worker never touches it) on cold per-frame UI cadence — E.1.2.C GUI polish (a) 2026-08-20]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[state for the Training panel — every training / validation / multi-horizon knob and worker handle]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
struct TrainingPanelState {
    // XGBoost hyperparameters
    int max_depth;
    float learning_rate;
    int n_estimators;
    // v5.9.5h — additional cfg-tunable XGBoost hyperparams. Defaults match
    // pre-v5.9.5h hardcoded values bytewise; non-tuning operators get
    // identical training output. UI exposes these as advanced tuning;
    // operator edits affect Train Model worker output only (worker captures
    // a snapshot at entry, like max_depth/lr/n_estimators above). Backtest
    // WF/HeldOut paths read from cfg directly.
    float ui_subsample;          // 0.5-1.0
    float ui_colsample_bytree;   // 0.5-1.0
    int   ui_min_child_weight;   // 1-50
    int   ui_seed;               // any int
    int   ui_tree_method_idx;    // 0=hist, 1=exact, 2=approx, 3=auto
    int label_type;
    float label_tp_pct;
    float label_sl_pct;
    // s5 leaf-15 — venue-general round-trip cost the label's WIN threshold must
    // clear (percent, like its siblings: 0.2 = 0.2%). Standalone knob by operator
    // decision: it generalizes across venues instead of tracking one engine fee
    // cfg. 0 = fee-blind labels (pre-s5 behavior).
    float label_roundtrip_fee_pct;
    int label_forward_ticks;
    // results
    float feature_importance[MODEL_MAX_FEATURES];
    char feature_names[MODEL_MAX_FEATURES][32];
    char model_path[256];
    float train_accuracy;            // binary/multiclass: classification accuracy (0..1)
    // regression-only metrics (valid when label kind == regression)
    float train_mse;                 // mean squared error
    float train_correlation;         // Pearson r between predictions and labels
    float train_label_min;           // observed label min/max for context
    float train_label_max;
    float train_label_mean;
    float train_label_stddev;
    int positive_count, negative_count;
    char status_msg[128];
    // v5.9.5d — scaler SHA-256 hex (64 chars + null) for GUI display.
    // Populated by the DELETED train_model_worker_fn (D-d) — dead at HEAD;
    // retained pending the field-hygiene sweep (leaf-13-adjacent);
    // empty when scaler not persisted. Single-writer (worker) → reader (UI
    // render after tm_complete=1 flips). Pre-v5.9.5d the SHA was only
    // logged via stderr — operator couldn't see it in foxml_suite.
    char scaler_sha256_hex[80];
    // (v5.9.5j tm_auto_stamp_* result fields DELETED at D-d with their only
    // writer; the mh path's RFV auto-stamp is the live mechanism.)
    // walk-forward validation (Phase 6A — A7 GUI rework)
    int wf_n_splits;          // number of temporal folds (default 5)
    // s5 leaf-16: 0 = AUTO (max of the Horizons CSV, resolved at every use via
    // Training_ResolvePurgeHorizon); nonzero = explicit operator override.
    // NOT ini-persisted — it re-defaults every launch, which is exactly why the
    // old literal-1000 default silently leaked on every fresh session.
    int wf_horizon_ticks;     // label horizon for purge gap calc (0 = auto-derive)
    int wf_buffer_ticks;      // extra purge buffer (default 512)
    int wf_min_train;         // min training samples per fold (default 500)
    volatile int wf_running;  // 1 = walk-forward in progress
    volatile int wf_progress; // 0-100 progress
    volatile int wf_cancel;   // 1 = user requested cancel
    volatile int wf_complete; // 1 = run finished
    pthread_t wf_tid;
    WalkForwardResults wf_results;
    bool wf_has_results;      // true after first completed walk-forward run
    // save run (bundles config + model for deployment)
    char run_name[64];
    char save_msg[128];
    // v5.8.7 — Full Validation (held-out + auto-stamp). Replaces the
    // hand-wired multi-button workflow with a single integrated path
    // that exercises Backtest_RunFullValidation, which is the function
    // carrying the v5.8.6 auto-stamp wiring (FEATURE_REGISTRY_HASH +
    // engine_version embedded in stamp body).
    volatile int fv_running;
    volatile int fv_progress;
    volatile int fv_cancel;
    volatile int fv_complete;
    pthread_t fv_tid;
    FullValidationResults fv_results;
    bool fv_has_results;
    char fv_auto_stamp_secret[128];   // HMAC secret (empty = devmode, signs but accepts any sig)
    float fv_held_out_fraction;       // 0.05 .. 0.30; clamped by HeldOutSplit_Make
    float fv_gap_threshold;           // gap threshold for stamp accept/refuse
    char fv_status_msg[256];          // post-run summary + auto-stamp result
    // v5.9.0c — Train Model worker thread state (V5_9_AUDIT-#7).
    // Pre-v5.9.0c, Train Model ran synchronously and froze the GUI 5-30s.
    // Worker pattern mirrors fv_* above. State is single-writer (worker
    // thread) → main UI reads after volatile completion flag flips.
    volatile int tm_running;
    volatile int tm_complete;
    volatile int tm_cancel;     // v5.9.0d: polled between XGBoost iterations
    pthread_t tm_tid;
    // v5.11.25 — XGBoost iteration progress (operator-flagged 2026-05-07).
    // Worker writes tm_progress_iter (current_iter+1, 1..n_estimators) and
    // tm_progress_total (snapshotted n_estimators) every iteration; GUI
    // renders ImGui::ProgressBar(tm_progress_iter / tm_progress_total).
    // Both reset to 0 on entry; written single-writer (worker thread)
    // with volatile to prevent compiler reordering. Pre-v5.11.25 the
    // progress bar was indeterminate (-1 fed into ImGui as a pulse).
    volatile int tm_progress_iter;
    volatile int tm_progress_total;
    // v5.11.29 — post-iter phase indicator. After the iter loop completes
    // (tm_progress_iter == tm_progress_total), the worker still does
    // significant work: XGBoosterSaveModel (slow JSON serialization for
    // 400+ trees, 1-5s), train-set predict + accuracy, scaler compute +
    // persist + SHA-256, optional auto-stamp. Pre-v5.11.29 the GUI
    // showed "iter 400/400" stuck at 100% for several seconds — operator
    // couldn't tell if it was hung. Worker now writes a short phase
    // string here at each post-iter transition; GUI uses it as the
    // ProgressBar overlay during the post-iter window. Empty string =
    // either pre-iter or iter-running (overlay falls back to iter count).
    char tm_phase_msg[64];
    // v5.10.0a.E — Hyperparam Sweep state. Mirrors wf_* / tm_* worker
    // pattern. Operator clicks Run Hyperparam Sweep → spawn worker that
    // calls Backtest_RunHyperparamTrainSweep using already-collected
    // feature_matrix.
    OptimizerRange  hp_ranges[OPT_MAX_PARAMS];
    int             hp_num_params;          // 1 or 2 active params
    OptimizerResults hp_results;
    volatile int    hp_running;
    volatile int    hp_progress;            // current cell index
    volatile int    hp_total;                // total cells (set by worker)
    volatile int    hp_cancel;
    volatile int    hp_complete;
    pthread_t       hp_tid;
    bool            hp_has_results;
    // v5.10.0a.G.1 — Multi-Horizon training state. Operator clicks Train
    // Multi-Horizon button; worker trains N models, one per horizon.
    // v5.10.0a-bugfix2 — horizons editable IN PANEL via CSV input
    // (operator no longer needs to edit cfg.horizon_list + reload).
    // ui_horizon_csv is the operator-typed string (e.g. "100,500,1000");
    // ui_horizon_list/_count are parsed on each render. cfg.horizon_list
    // still works as a fallback if ui_horizon_csv is empty (back-compat
    // for operators who already set cfg).
    volatile int    mh_running;
    volatile int    mh_progress;            // 1..N as horizons complete
    volatile int    mh_total;                // N (set by worker)
    volatile int    mh_current_horizon;     // current horizon ticks
    volatile int    mh_cancel;
    volatile int    mh_complete;
    pthread_t       mh_tid;
    char            ui_horizon_csv[128];    // operator-typed; parsed → ui_horizon_*
    // E.1.2.D leaf 13 (S3-F10) — the panel's per-horizon arrays were literal
    // [8]; bind them to the cfg grid cap so a future HORIZON_LIST_MAX bump
    // cannot silently shear the panel arrays off the grid.
    static constexpr int PANEL_HORIZON_MAX =
        ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX;
    int             ui_horizon_list[PANEL_HORIZON_MAX];
    int             ui_horizon_count;
    // v5.11.40 — per-horizon TP/SL CSV (operator-flagged 2026-05-07).
    // Broadcast-or-match rule: 1 value applies to all horizons; N values
    // map positionally where N == ui_horizon_count; anything else is
    // misaligned and disables the Multi-Horizon button with a hint.
    //
    // Backward compat: when CSV is empty OR parses to 1 value, the
    // existing single-value label_tp_pct/label_sl_pct fields are used
    // (single-horizon Train Model + cfg load + Save Run output paths
    // all read from those float fields). Per-horizon arrays here drive
    // the multi-horizon worker only.
    char            ui_tp_pct_csv[64];   // e.g. "0.030" or "0.020,0.030,0.040"
    char            ui_sl_pct_csv[64];
    float           ui_tp_per_horizon[PANEL_HORIZON_MAX];   // parsed values (broadcast or positional)
    float           ui_sl_per_horizon[PANEL_HORIZON_MAX];
    int             ui_tp_per_horizon_count;  // 0 = empty/use single field; 1 = broadcast; N = positional
    int             ui_sl_per_horizon_count;
    // v5.11.41 — per-horizon FullValidationResults (one per horizon, max PANEL_HORIZON_MAX).
    // Multi-horizon worker writes mh_horizon_fv[h] for each h in [0..N-1)
    // by calling Backtest_RunFullValidation per horizon (replacing the
    // previous "train+save only" inline XGBoost path). GUI reads
    // mh_horizon_status[h] for live render. mh_horizon_progress[h] is
    // the current horizon's WF + held-out % (0..100). mh_horizon_complete[h]
    // = 1 when that horizon's FV pipeline finished (or failed).
    FullValidationResults  mh_horizon_fv[PANEL_HORIZON_MAX];
    volatile int           mh_horizon_complete[PANEL_HORIZON_MAX];
    alignas(64) volatile int mh_horizon_progress[PANEL_HORIZON_MAX];  // H6 (Stage-5.5): cross-thread, was straddling a line
    char                   mh_horizon_status[PANEL_HORIZON_MAX][128];
    // E.1.2.C GUI polish (a) — click-time snapshot of the run's horizon
    // ticks for the per-horizon results table. The live ui_horizon_list
    // re-parses ui_horizon_csv EVERY frame, so reading it from the table
    // relabeled rows whenever the operator edited the CSV mid/post-run
    // (and showed nothing on the cfg.horizon_list fallback path). GUI
    // thread writes at click + reads at render — no volatile needed.
    int                    mh_horizon_ticks[PANEL_HORIZON_MAX];

    // v5.13.1.A — sell-side training. Routes Multi-Horizon output to a
    // side-specific subdirectory: side=0 (buy) leaves the existing
    // models/<run_subdir>/<run>/horizon_<N>/ path; side=1 (exit) emits the
    // CO-LOCATED exit role file in the SAME per-horizon dirs (E.1.2.C — the
    // retired models/exit/ tree was never walked by any loader; the engine
    // auto-discovers exit.json siblings under node_N_model_dir).
    int             ui_training_side;  // 0=buy (default), 1=exit

    // v5.13.1.B — per-horizon label_kind CSV (operator-flagged 2026-05-08).
    // Broadcast-or-match rule mirrors ui_tp_pct_csv. Empty/single value
    // falls back to state->label_type (existing behavior). N values map
    // positionally where N == ui_horizon_count; misalignment disables
    // Train Multi-Horizon button.
    //
    // Format: integer label_type values per LABEL_* enum (LabelFunctions.hpp).
    // Operator types e.g. "0,2,1" → horizon_0=binary, horizon_1=multi,
    // horizon_2=regression.
    char            ui_label_kind_csv[64];
    alignas(64) int ui_label_kind_per_horizon[PANEL_HORIZON_MAX];   // parsed (broadcast or positional); H6-aligned (Stage-5.5 straddle)
    int             ui_label_kind_per_horizon_count; // 0=empty; 1=broadcast; N=positional
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-22]
// [SIZE]_[500096B]
// [ALIGN]_[64]
// [CACHE_LINES]_[7814]
// [STRADDLE]_[run_name@12705 · tm_phase_msg@24904 · ui_tp_pct_csv@406148 · ui_sl_pct_csv@406212 · ui_sl_per_horizon@406308 · ui_label_kind_csv@499908]
//======================================================================
// [END_STRUCT]_[TrainingPanelState]
//======================================================================

//======================================================================
// [FUNCTION]_[Training_AnyWorkerRunning]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE "is a suite worker live" predicate — hoisted so the COLLECT gates can consult it, not just the train gates]
//======================================================================
// [CODE]
//======================================================================
static inline bool Training_AnyWorkerRunning(const TrainingPanelState *st) {
    return st && (st->tm_running || st->wf_running ||
                  st->fv_running || st->hp_running || st->mh_running);
}

//======================================================================
// [FUNCTION]_[Training_ResolvePurgeHorizon]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[s5 leaf-16 — the ONE purge-horizon resolver: explicit override, else DERIVED max(Horizons CSV). Kills the two-unlinked-fields leakage class.]
//======================================================================
// [CODE]
//======================================================================
// The purge gap between train and test folds must cover the LONGEST label's
// forward reach, or training samples near the boundary carry outcomes that
// peek into test — silent leakage that INFLATES every WF / sweep / Full
// Validation number.
//
// Before s5 leaf-16, `wf_horizon_ticks` was an independent manual field
// defaulting to 1000 with NOTHING binding it to the horizons the operator
// actually collected: a 67,500-tick label grid with the box left at 1000
// purged ~1.5k ticks instead of ~68k. Two fields that must agree, with no
// mechanism making them agree — the same shape as the label fee knob that
// wasn't there (Class-55-adjacent dual-source).
//
// Resolution: 0 = AUTO (derive max over the effective horizon list); any
// nonzero value is an explicit operator override and is honored verbatim
// (escape hatch preserved — an operator experimenting with a deliberately
// short purge can still ask for one). Falls back to the legacy 1000 only
// when auto is requested and NO horizon list exists to derive from.
static inline int Training_ResolvePurgeHorizon(const TrainingPanelState *st) {
    if (!st) return 1000;
    if (st->wf_horizon_ticks > 0) return st->wf_horizon_ticks;  // explicit override
    int mx = 0;
    for (int i = 0; i < st->ui_horizon_count
                    && i < TrainingPanelState::PANEL_HORIZON_MAX; ++i) {
        if (st->ui_horizon_list[i] > mx) mx = st->ui_horizon_list[i];
    }
    if (mx > 0) return mx;
    if (st->label_forward_ticks > 0) return st->label_forward_ticks;  // single-horizon flows
    return 1000;  // no horizons known — the historical default
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Training_ResolvePurgeHorizon]
//======================================================================
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// The predicate already existed, but INLINE and computed AFTER the collect
// gates, so `can_collect` / `mh_can_collect` could not see it — they consulted
// only `run_control->running`. That left a real hazard: Collect Features runs
// Backtest_Run, which REALLOCs results->feature_matrix and results->labels, and
// a realloc MOVES those buffers. A training worker holding a shallow copy of
// `results` then reads freed memory. The 2026-04-25 segfault this file's
// comments describe was mitigated for the DISPLAY path only; the worker path
// stayed open.
//
// Hoisted to a named function so a future gate cannot silently re-derive a
// different answer — the same reason Training_ResolveRole was extracted.
//======================================================================
// [END_FUNCTION]_[Training_AnyWorkerRunning]
//======================================================================

//======================================================================
// [FUNCTION]_[Training_SnapshotHyperparams]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE panel-state -> XGBHyperparams mapping — click-time snapshot for every worker entry point, so no two buttons can describe different architectures]
//======================================================================
// [CODE]
//======================================================================
static inline tt::XGBHyperparams Training_SnapshotHyperparams(const TrainingPanelState *st) {
    if (!st) return tt::XGBHyperparams_Defaults();
    // E.1.2.D leaf 14 — the ONE value-mapper (was a hand-copy of the mapping)
    return tt::XGBHyperparams_FromRaw(st->max_depth, st->learning_rate,
                                      st->n_estimators, st->ui_subsample,
                                      st->ui_colsample_bytree,
                                      st->ui_min_child_weight, st->ui_seed,
                                      st->ui_tree_method_idx);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// E.1.2.C follow-up (2026-08-22). The Stage-6.5.4 review found the standalone
// Run-Full-Validation button training at XGBHyperparams_Defaults() while the
// Train path used the panel's values — one architecture measured, a different
// one shipped, and the signed stamp overwritten with the wrong numbers. Wiring
// the two remaining entry points fixed the SYMPTOM; this helper fixes the
// SHAPE, because the fix as first written left two character-identical copies
// of the mapping (an eighth hyperparameter would have to be added to both, and
// the one that got missed would fail exactly as silently).
//
// This is the panel-state adapter. Two more constructions of the same mapping
// live on the multi-horizon worker path (from `snap_*` locals at the
// booster call, and from `args->snap_*` after the capture-before-free); those
// take a different SOURCE, so folding all four onto one value-mapper is its own
// leaf rather than a close-out drive-by — homed at E.1.2.D leaf 14, together
// with the `tree_method` stamp split-brain (M1) that shares this surface.
//======================================================================
// [END_FUNCTION]_[Training_SnapshotHyperparams]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingPanel_Init]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[init the Training panel state — defaults for every training/validation knob]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void TrainingPanel_Init(TrainingPanelState *state) {
    memset(state, 0, sizeof(*state));
    state->max_depth = 4;
    state->learning_rate = 0.1f;
    state->n_estimators = 100;
    // v5.9.5h — defaults match XGBHyperparams_Defaults bytewise
    state->ui_subsample          = 0.8f;
    state->ui_colsample_bytree   = 0.8f;
    state->ui_min_child_weight   = 5;
    state->ui_seed               = 42;
    state->ui_tree_method_idx    = 0;  // 0 = "hist"
    state->label_type = LABEL_WIN_LOSS;
    state->label_tp_pct = 1.5f;
    state->label_sl_pct = 1.0f;
    // s5 leaf-15: 0 = fee-blind (bytewise-identical to pre-s5 labels). The
    // operator sets the venue's round trip explicitly — no silent default that
    // would change every existing run's labels on upgrade.
    state->label_roundtrip_fee_pct = 0.0f;
    // v5.11.40 — CSV-aware TP/SL per-horizon. Default: empty CSV =
    // single-value mode (uses label_tp_pct/_sl_pct directly). Operator
    // types comma-separated values to opt in to per-horizon.
    state->ui_tp_pct_csv[0] = '\0';
    state->ui_sl_pct_csv[0] = '\0';
    // v5.13.1 — sell-side training defaults: side=buy, empty CSV
    // (broadcast state->label_type to all horizons).
    state->ui_training_side               = 0;  // 0 = buy
    state->ui_label_kind_csv[0]           = '\0';
    state->ui_label_kind_per_horizon_count = 0;
    for (int i = 0; i < TrainingPanelState::PANEL_HORIZON_MAX; ++i)
        state->ui_label_kind_per_horizon[i] = LABEL_WIN_LOSS;
    for (int i = 0; i < TrainingPanelState::PANEL_HORIZON_MAX; ++i)
        state->ui_tp_per_horizon[i] = 0.0f;
    for (int i = 0; i < TrainingPanelState::PANEL_HORIZON_MAX; ++i)
        state->ui_sl_per_horizon[i] = 0.0f;
    state->ui_tp_per_horizon_count = 0;
    state->ui_sl_per_horizon_count = 0;
    // v5.11.48 — default "run" instead of "run_01". Operator typically
    // overrides with their own prefix (e.g. "btc_5min", "regime_v2"); the
    // generic "run" surfaces less misleading than a specific-looking number.
    // Worker appends "_horizon_<H>" so even default produces "run_horizon_*".
    strncpy(state->run_name, "run", sizeof(state->run_name) - 1);
    state->label_forward_ticks = 1000;
    strncpy(state->model_path, "models/buy_signal.json", sizeof(state->model_path) - 1);
    // feature names from ModelInference.hpp constants
    strncpy(state->feature_names[FEAT_SHORT_SLOPE],    "short_slope", 31);
    strncpy(state->feature_names[FEAT_SHORT_R2],       "short_r2", 31);
    strncpy(state->feature_names[FEAT_SHORT_VARIANCE], "short_var", 31);
    strncpy(state->feature_names[FEAT_LONG_SLOPE],     "long_slope", 31);
    strncpy(state->feature_names[FEAT_LONG_R2],        "long_r2", 31);
    strncpy(state->feature_names[FEAT_LONG_VARIANCE],  "long_var", 31);
    strncpy(state->feature_names[FEAT_VOL_RATIO],      "vol_ratio", 31);
    strncpy(state->feature_names[FEAT_ROR_SLOPE],      "ror_slope", 31);
    strncpy(state->feature_names[FEAT_VOLUME_SLOPE],   "vol_slope", 31);
    strncpy(state->feature_names[FEAT_VOLUME_DELTA],   "vol_delta", 31);
    strncpy(state->feature_names[FEAT_EMA_SMA_SPREAD], "ema_sma", 31);
    strncpy(state->feature_names[FEAT_VWAP_DEV],       "vwap_dev", 31);
    strncpy(state->feature_names[FEAT_PRICE_STDDEV],   "stddev", 31);
    strncpy(state->feature_names[FEAT_PRICE_AVG],      "price_avg", 31);
    strncpy(state->feature_names[FEAT_VOLUME_AVG],     "vol_avg", 31);
    strncpy(state->feature_names[FEAT_EMA_ABOVE_SMA],  "ema>sma", 31);
    // walk-forward defaults (FoxML battle-tested values)
    state->wf_n_splits = 5;
    // s5 leaf-16: 0 = AUTO (derive max(Horizons CSV) at use — see
    // Training_ResolvePurgeHorizon). The old literal 1000 default is what let a
    // 67.5k-tick label grid run a ~1.5k-tick purge gap and leak into test.
    state->wf_horizon_ticks = 0;
    state->wf_buffer_ticks = PURGE_BUFFER_DEFAULT;
    state->wf_min_train = 500;
    state->wf_running = 0;
    state->wf_progress = 0;
    state->wf_cancel = 0;
    state->wf_complete = 0;
    state->wf_has_results = false;
    memset(&state->wf_results, 0, sizeof(state->wf_results));
    // v5.8.7 — full validation defaults (mirrors the cfg defaults so the
    // suite UI is usable out-of-the-box without editing engine.cfg).
    state->fv_running = 0;
    state->fv_progress = 0;
    state->fv_cancel = 0;
    state->fv_complete = 0;
    state->fv_has_results = false;
    memset(&state->fv_results, 0, sizeof(state->fv_results));
    state->fv_auto_stamp_secret[0] = '\0';   // devmode by default
    state->fv_held_out_fraction = 0.20f;      // matches HELDOUT_FRACTION default
    state->fv_gap_threshold = 0.05f;          // matches gap_acceptable_threshold default
    state->fv_status_msg[0] = '\0';
    // v5.9.0c — Train Model worker init
    state->tm_running = 0;
    state->tm_complete = 0;
    state->tm_cancel = 0;
    state->tm_progress_iter = 0;   // v5.11.25
    state->tm_progress_total = 0;  // v5.11.25
    state->tm_phase_msg[0] = '\0'; // v5.11.29 — clear post-iter phase indicator
    // v5.10.0a.E — Hyperparam Sweep init. Default param 0 = sweep
    // xgb_subsample 0.5 .. 0.9 step 0.1 (5 cells).
    strncpy(state->hp_ranges[0].key, "xgb_subsample", sizeof(state->hp_ranges[0].key) - 1);
    state->hp_ranges[0].key[sizeof(state->hp_ranges[0].key) - 1] = '\0';
    state->hp_ranges[0].lo   = 0.5;
    state->hp_ranges[0].hi   = 0.9;
    state->hp_ranges[0].step = 0.1;
    state->hp_ranges[1].key[0] = '\0';
    state->hp_ranges[1].lo   = 0.0;
    state->hp_ranges[1].hi   = 0.0;
    state->hp_ranges[1].step = 0.0;
    state->hp_num_params  = 1;
    state->hp_running     = 0;
    state->hp_progress    = 0;
    state->hp_total       = 0;
    state->hp_cancel      = 0;
    state->hp_complete    = 0;
    state->hp_has_results = false;
    memset(&state->hp_results, 0, sizeof(state->hp_results));
    // v5.10.0a.G.1 — Multi-Horizon training state init
    state->mh_running         = 0;
    state->mh_progress        = 0;
    state->mh_total           = 0;
    state->mh_current_horizon = 0;
    state->mh_cancel          = 0;
    state->mh_complete        = 0;
    memset(state->mh_horizon_ticks, 0, sizeof(state->mh_horizon_ticks));
    // v5.10.0a-bugfix2 — UI horizon list defaults empty; operator types
    // CSV (or leaves blank to fall back to cfg.horizon_list). Pre-fill
    // with a sensible suggestion that matches the original Idea #4 spec
    // example so operators see what shape the field expects.
    strncpy(state->ui_horizon_csv, "100,500,1000",
            sizeof(state->ui_horizon_csv) - 1);
    state->ui_horizon_csv[sizeof(state->ui_horizon_csv) - 1] = '\0';
    for (int i = 0; i < TrainingPanelState::PANEL_HORIZON_MAX; ++i)
        state->ui_horizon_list[i] = 0;
    state->ui_horizon_count = 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingPanel_Init]
//======================================================================

// walk-forward worker thread
//======================================================================
// [STRUCT]_[WalkForwardWorkerArgs]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[worker-thread args for the standalone Walk-Forward button — panel state + data + the click-time snapshot (WF split params, label kind from run_config, XGB hyperparams) this struct went without until E.1.2.C]
//======================================================================
// [CODE]
//======================================================================
struct WalkForwardWorkerArgs {
    TrainingPanelState *state;
    const BacktestResults *data;
    // E.1.2.C follow-up (2026-08-22) — this struct had NO snap block at all, so
    // the worker read six operator-editable fields LIVE off `state->` (S1-F6),
    // and it is the THIRD entry point into Backtest_RunWalkForward: it passed
    // neither cfg_override nor hp_override, so the standalone Walk-Forward button
    // measured XGBHyperparams_Defaults() regardless of the panel. Found by
    // applying the Stage-6.5.4 review's own lesson — enumerate a threaded call
    // chain's ENTRY POINTS, not just its consumers. `wf_horizon_ticks` is the
    // temporal-leakage purge gap, so a live read there is a correctness surface,
    // not just a tidiness one.
    int    snap_wf_n_splits;
    int    snap_wf_horizon_ticks;
    int    snap_wf_buffer_ticks;
    int    snap_wf_min_train;
    int    snap_label_type;
    tt::XGBHyperparams snap_hp;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-22]
// [SIZE]_[80B]
// [ALIGN]_[8]
// [CACHE_LINES]_[2]
// [STRADDLE]_[snap_hp@36]
//======================================================================
// [END_STRUCT]_[WalkForwardWorkerArgs]
//======================================================================

//======================================================================
// [FUNCTION]_[walkforward_worker_fn]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[background thread: run walk-forward CV]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void *walkforward_worker_fn(void *arg) {
    WalkForwardWorkerArgs *args = (WalkForwardWorkerArgs *)arg;
    TrainingPanelState *state = args->state;
    const BacktestResults *data = args->data;
    // E.1.2.C follow-up — capture BEFORE free(args). This is the Class-13
    // capture-before-free discipline the sister worker structs already follow;
    // this one had no snap block to capture from until now.
    int    snap_wf_n_splits      = args->snap_wf_n_splits;
    int    snap_wf_horizon_ticks = args->snap_wf_horizon_ticks;
    int    snap_wf_buffer_ticks  = args->snap_wf_buffer_ticks;
    int    snap_wf_min_train     = args->snap_wf_min_train;
    int    snap_label_type       = args->snap_label_type;
    tt::XGBHyperparams snap_hp   = args->snap_hp;
    free(args);

    Backtest_RunWalkForward(&state->wf_results, data,
                             snap_wf_n_splits, snap_wf_horizon_ticks,
                             snap_wf_buffer_ticks, snap_wf_min_train,
                             &state->wf_progress, &state->wf_cancel,
                             snap_label_type,
                             /*cfg_override=*/nullptr, /*hp_override=*/&snap_hp);

    state->wf_has_results = true;
    state->wf_complete = 1;
    state->wf_running = 0;
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[walkforward_worker_fn]
//======================================================================

// v5.10.0a.E — Hyperparam Sweep worker thread. Mirrors walkforward_worker_fn
// but calls Backtest_RunHyperparamTrainSweep — trains N XGBoosters per cell
// using the shared feature_matrix, varies xgb_* hyperparams via cfg_override
// path. Operator must Collect Features first; data->config_used carries the
// base cfg used at collect-time.
//
// Click-time snapshot: copies hp_ranges + hp_num_params + WF tuning fields
// into worker args at click time (matches v5.10.0E pattern). Operator can
// keep editing the input ranges while sweep runs without affecting the
// in-flight cells.
//======================================================================
// [STRUCT]_[HyperparamSweepWorkerArgs]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[worker-thread args for the hyperparameter grid sweep — panel state + result data + the swept OptimizerRange set + the walk-forward split params (all click-time snapshots)]
//======================================================================
// [CODE]
//======================================================================
struct HyperparamSweepWorkerArgs {
    TrainingPanelState *state;
    const BacktestResults *data;
    OptimizerRange snap_ranges[OPT_MAX_PARAMS];
    int snap_num_params;
    int snap_label_type;
    int snap_wf_n_splits;
    int snap_wf_horizon_ticks;
    int snap_wf_buffer_ticks;
    int snap_wf_min_train;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[152B]
// [ALIGN]_[8]
// [CACHE_LINES]_[3]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[HyperparamSweepWorkerArgs]
//======================================================================

//======================================================================
// [FUNCTION]_[hp_sweep_worker_fn]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[background thread: run a hyperparam training sweep]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void *hp_sweep_worker_fn(void *arg) {
    HyperparamSweepWorkerArgs *args = (HyperparamSweepWorkerArgs *)arg;
    TrainingPanelState *state = args->state;
    const BacktestResults *data = args->data;

    // Local copies — args struct freed below.
    OptimizerRange ranges[OPT_MAX_PARAMS];
    memcpy(ranges, args->snap_ranges, sizeof(ranges));
    int num_params = args->snap_num_params;
    int label_type = args->snap_label_type;
    int wf_n_splits = args->snap_wf_n_splits;
    int wf_horizon  = args->snap_wf_horizon_ticks;
    int wf_buffer   = args->snap_wf_buffer_ticks;
    int wf_min_train = args->snap_wf_min_train;
    free(args);

    memset(&state->hp_results, 0, sizeof(state->hp_results));
    state->hp_progress = 0;
    state->hp_total = 0;

#ifdef USE_XGBOOST
    Backtest_RunHyperparamTrainSweep(
        &state->hp_results, data, ranges, num_params,
        label_type,
        wf_n_splits, wf_horizon, wf_buffer, wf_min_train,
        &state->hp_progress, &state->hp_total,
        &state->hp_cancel);
    state->hp_has_results = (state->hp_results.total_runs > 0);
#else
    fprintf(stderr, "[hpsweep] XGBoost not compiled in — sweep skipped\n");
    state->hp_has_results = false;
#endif

    state->hp_complete = 1;
    state->hp_running = 0;
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[hp_sweep_worker_fn]
//======================================================================

// v5.8.7 — full-validation worker thread. Mirrors walkforward_worker_fn but
// calls Backtest_RunFullValidation, which carries the v5.8.6 auto-stamp
// wiring (FEATURE_REGISTRY_HASH + engine_version embedded in stamp body).
//======================================================================
// [STRUCT]_[FullValidationWorkerArgs]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[worker-thread args for Run Full Validation — panel state + data + click-time snapshots of model path / stamp secret / label params (capture-at-click defeats the ImGui edit race)]
//======================================================================
// [CODE]
//======================================================================
struct FullValidationWorkerArgs {
    TrainingPanelState *state;
    const BacktestResults *data;
    // v5.10.0E — snapshot operator-editable fields at click time, not in
    // the worker. The ImGui input fields write into state->model_path /
    // state->fv_auto_stamp_secret CONCURRENTLY with worker execution; if
    // operator clicks Run Full Validation with empty model_path then
    // types it AFTER, the worker reads empty (skip copy → auto_stamp_path
    // empty), then status build reads the post-typed value, producing
    // "model_path='X' did not propagate (worker race)" diagnostic.
    // Capture-at-click eliminates the race.
    char snap_model_path[256];
    char snap_fv_auto_stamp_secret[64];
    // v5.11.41 — capture label params from run_control->run_config at click
    // time so RFV can stamp them into the body. Live in BacktestRunConfig,
    // not in ControllerConfig (= data->config_used) so RFV can't reach
    // them otherwise. Closes /parity-check 2026-05-07-stamp CRITICAL-1.
    int     snap_label_forward_ticks;
    double  snap_label_tp_pct;
    double  snap_label_sl_pct;
    // E.1.2.C — the remaining fields RFV was reading LIVE off `state->` from the
    // worker thread. `snap_label_type` is deliberately sourced from
    // run_control->run_config (the field that actually produced results->labels[]),
    // NOT from state->label_type: the combo can be changed BETWEEN the Collect
    // click and the Run-Full-Validation click, with no race required, and the
    // worker then trained WF/held-out on one objective while stamping another.
    // When the class counts differ the engine REFUSES at load
    // (NodeModelZoo.hpp: "stamp claims model_num_outputs=N but handle=M"); when
    // they match — WIN_LOSS / BARRIER / VOL_BARRIER / WILL_PEAK are all binary —
    // nothing catches it and the stamp simply records a label the model never
    // trained on. Sourcing from run_config makes the stamp describe the labels.
    int     snap_label_type;
    int     snap_wf_n_splits;
    int     snap_wf_horizon_ticks;
    int     snap_wf_buffer_ticks;
    int     snap_wf_min_train;
    float   snap_fv_gap_threshold;
    float   snap_fv_held_out_fraction;
    // E.1.2.C follow-up (2026-08-22) — the SECOND entry point into
    // Backtest_RunFullValidation. The multi-horizon Train path was threaded with
    // the click-time hyperparameters at f99e102; THIS one was missed, so the
    // standalone "Run Full Validation" button still trained WF folds + held-out at
    // XGBHyperparams_Defaults() AND overwrote the target model's signed stamp with
    // 6/0.1/200 — silently undoing, on disk, the thing f99e102 fixed. The
    // `= nullptr` default that makes hp_override safe for un-updated callers is
    // exactly what made the omission invisible: no compile error, no warning.
    // Caught by the Stage-6.5.4 adversarial handoff review, not by the author.
    tt::XGBHyperparams snap_hp;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-22]
// [SIZE]_[432B]
// [ALIGN]_[8]
// [CACHE_LINES]_[7]
// [STRADDLE]_[snap_fv_auto_stamp_secret@272]
//======================================================================
// [END_STRUCT]_[FullValidationWorkerArgs]
//======================================================================

//======================================================================
// [FUNCTION]_[fullvalidation_worker_fn]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[background thread: run full validation (WF + held-out gap)]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void *fullvalidation_worker_fn(void *arg) {
    FullValidationWorkerArgs *args = (FullValidationWorkerArgs *)arg;
    TrainingPanelState *state = args->state;
    const BacktestResults *data = args->data;
    // v5.10.0E — local copies of click-time snapshots. Free args
    // immediately so caller-side memory hygiene matches the legacy worker
    // pattern (free as early as practical).
    char model_path_snap[256];
    char fv_auto_stamp_secret_snap[64];
    // v5.11.41 — local copies of label params (live in BacktestRunConfig
    // which is operator-mutable; capture at click time avoids race).
    int    snap_label_forward_ticks = args->snap_label_forward_ticks;
    double snap_label_tp_pct        = args->snap_label_tp_pct;
    // E.1.2.C — capture the rest of the click-time snapshot before free(args).
    // Every one of these was a live `state->` read further down.
    int    snap_label_type          = args->snap_label_type;
    int    snap_wf_n_splits         = args->snap_wf_n_splits;
    int    snap_wf_horizon_ticks    = args->snap_wf_horizon_ticks;
    int    snap_wf_buffer_ticks     = args->snap_wf_buffer_ticks;
    int    snap_wf_min_train        = args->snap_wf_min_train;
    float  snap_fv_gap_threshold    = args->snap_fv_gap_threshold;
    float  snap_fv_held_out_fraction = args->snap_fv_held_out_fraction;
    tt::XGBHyperparams snap_hp       = args->snap_hp;   // E.1.2.C follow-up
    double snap_label_sl_pct        = args->snap_label_sl_pct;
    {
        size_t n = strnlen(args->snap_model_path,
                           sizeof(args->snap_model_path));
        if (n >= sizeof(model_path_snap)) n = sizeof(model_path_snap) - 1;
        memcpy(model_path_snap, args->snap_model_path, n);
        model_path_snap[n] = '\0';
    }
    {
        size_t n = strnlen(args->snap_fv_auto_stamp_secret,
                           sizeof(args->snap_fv_auto_stamp_secret));
        if (n >= sizeof(fv_auto_stamp_secret_snap))
            n = sizeof(fv_auto_stamp_secret_snap) - 1;
        memcpy(fv_auto_stamp_secret_snap, args->snap_fv_auto_stamp_secret, n);
        fv_auto_stamp_secret_snap[n] = '\0';
    }
    free(args);

    // Build held-out split and unlock immediately. The friction-grade lock
    // exists to make held-out access a deliberate operator action; the
    // suite UI's Run Full Validation button is exactly that deliberate
    // action, so unlocking here is correct.
    // E.1.2.C — the click-time fraction. This determined the held-out split size,
    // hence held_out_metric, hence the generalization gap that gates auto-stamp —
    // off a live slider read from a worker thread.
    HeldOutSplit split = HeldOutSplit_Make(data->sample_count,
                                            (double)snap_fv_held_out_fraction);
    char unlock_token[33];
    memcpy(unlock_token, split.lock_token, sizeof(unlock_token));
    HeldOutSplit_Unlock(&split, unlock_token);

    // Pre-populate auto-stamp request fields. Backtest_RunFullValidation
    // gates the stamp_write_for_model call on auto_stamp_path being non-empty
    // AND ran_held_out=1; both are met here when training succeeds.
    //
    // v5.8.10 — gate path-setting on the cfg's auto_stamp_on_held_out flag.
    // When the operator runs the suite with auto_stamp_on_held_out=0 (originally:
    // for manual stamping via tools/stamp_model.sh; bash CLI DELETED at .B.3 Path C
    // 2026-05-24; =0 now only meaningful for v5.16+ cmdline-invocable training per
    // decoupling-endgoal-roadmap), the FV button still runs held-out validation
    // but skips the stamp write. Honors operator intent.
    memset(&state->fv_results, 0, sizeof(state->fv_results));
    int auto_stamp_enabled = data->config_used.auto_stamp_on_held_out;
    if (auto_stamp_enabled) {
        // v5.10.0E — read from local snapshot (captured at click time),
        // not from state->model_path which may have changed since.
        size_t n = strnlen(model_path_snap, sizeof(model_path_snap));
        if (n >= sizeof(state->fv_results.auto_stamp_path))
            n = sizeof(state->fv_results.auto_stamp_path) - 1;
        memcpy(state->fv_results.auto_stamp_path, model_path_snap, n);
        // E.1.2.C 3-role (F1, per the D2 verdict) — the FV re-stamp was the ONE
        // production emit path that omitted expected_role (fv_results is memset
        // above, req_role stayed ""). Derive it from the model basename stem
        // when it exactly names a role file; otherwise leave empty (legacy).
        {
            const char* base = strrchr(model_path_snap, '/');
            base = base ? base + 1 : model_path_snap;
            static const char* kRoles[4] = {"barrier", "regime", "exit", "buy_signal"};
            for (int ri = 0; ri < 4; ++ri) {
                size_t rl = strlen(kRoles[ri]);
                if (strncmp(base, kRoles[ri], rl) == 0 && base[rl] == '.') {
                    snprintf(state->fv_results.req_role,
                             sizeof(state->fv_results.req_role), "%s", kRoles[ri]);
                    break;
                }
            }
        }
        state->fv_results.auto_stamp_path[n] = '\0';
    }
    {
        // v5.10.0E — same race-free snapshot read for secret.
        size_t n = strnlen(fv_auto_stamp_secret_snap,
                           sizeof(fv_auto_stamp_secret_snap));
        if (n >= sizeof(state->fv_results.auto_stamp_secret))
            n = sizeof(state->fv_results.auto_stamp_secret) - 1;
        memcpy(state->fv_results.auto_stamp_secret, fv_auto_stamp_secret_snap, n);
        state->fv_results.auto_stamp_secret[n] = '\0';
    }
    state->fv_results.auto_stamp_format_version = 0;  // 0 = use MODEL_FORMAT_VERSION

    // v5.11.41 — populate per-horizon label params from snap'd BacktestRunConfig
    // (captured at click time alongside model_path + secret). Single-horizon
    // path; multi-horizon worker populates these per horizon directly.
    // Closes pre-existing schema gap so single-horizon stamps also gain
    // forensic horizon record.
    state->fv_results.req_label_lookahead_ticks = snap_label_forward_ticks;
    state->fv_results.req_label_tp_pct = snap_label_tp_pct;
    state->fv_results.req_label_sl_pct = snap_label_sl_pct;

    // E.1.2.C — every argument here is now the CLICK-TIME snapshot. These were
    // live `state->` reads from a worker thread; the label_type one needed no
    // race at all (change the combo between Collect and this click).
    Backtest_RunFullValidation(&state->fv_results, data, &split,
                                snap_wf_n_splits, snap_wf_horizon_ticks,
                                snap_wf_buffer_ticks, snap_wf_min_train,
                                &state->fv_progress, &state->fv_cancel,
                                snap_label_type, snap_fv_gap_threshold,
                                /*hp_override=*/&snap_hp);

    // Build a one-line status summary the UI can render in fv_status_msg.
    if (state->fv_results.auto_stamp_attempted) {
        if (state->fv_results.auto_stamp_ok) {
            snprintf(state->fv_status_msg, sizeof(state->fv_status_msg),
                "Stamp written: %s", state->fv_results.auto_stamp_path_written);
        } else {
            snprintf(state->fv_status_msg, sizeof(state->fv_status_msg),
                "Stamp REFUSED: %s", state->fv_results.auto_stamp_error);
        }
    } else if (state->fv_results.ran_held_out) {
        if (!auto_stamp_enabled) {
            snprintf(state->fv_status_msg, sizeof(state->fv_status_msg),
                "Held-out OK; auto-stamp disabled (cfg auto_stamp_on_held_out=0)");
        } else {
            // v5.9.4a — improved diagnostic. Pre-v5.9.4a always said
            // "model_path empty?" — operator had no info to debug.
            // v5.10.0E — read model_path from local SNAPSHOT (captured at
            // click time), not from state->model_path which the operator
            // may have edited since worker started. This makes the
            // diagnostic accurate ("at click time, model_path was empty"
            // vs the false "did not propagate" message we used to print
            // when operator typed model_path AFTER clicking).
            if (model_path_snap[0] == '\0') {
                snprintf(state->fv_status_msg, sizeof(state->fv_status_msg),
                    "Held-out OK; auto-stamp skipped — model_path was empty "
                    "AT CLICK TIME (set Model Path field BEFORE clicking "
                    "Run Full Validation; current value: '%s')",
                    state->model_path);
            } else if (state->fv_results.auto_stamp_path[0] == '\0') {
                // Truly unexpected — snapshot was non-empty but copy didn't
                // populate. Code path shouldn't fire post-v5.10.0E.
                snprintf(state->fv_status_msg, sizeof(state->fv_status_msg),
                    "Held-out OK; auto-stamp skipped — model_path='%s' "
                    "snapshot non-empty but auto_stamp_path empty "
                    "(internal copy failure; report bug)",
                    model_path_snap);
            } else {
                snprintf(state->fv_status_msg, sizeof(state->fv_status_msg),
                    "Held-out OK; auto-stamp skipped — Backtest_RunFullValidation "
                    "did not fire stamp_write (auto_stamp_path='%s'; check "
                    "ran_held_out flag + path validity)",
                    state->fv_results.auto_stamp_path);
            }
        }
    } else {
        snprintf(state->fv_status_msg, sizeof(state->fv_status_msg),
            "Held-out did not complete (cancel or shape error?)");
    }

    state->fv_has_results = true;
    state->fv_complete = 1;
    state->fv_running = 0;
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[fullvalidation_worker_fn]
//======================================================================

// D-d (2026-08-22, operator-decided) — train_model_worker_fn + TrainModelWorkerArgs DELETED (~470 lines).
// Dead since v5.11.44 routed Train Model through the multi-horizon worker: zero
// pthread_create sites tree-wide (scan-1 NEW-3 / scan-2 NEW-4/W2, both re-derived
// 2026-08-22). It was compiled-in dead capital-adjacent code (H21 discipline) that
// DIVERGED from the live path (it neutral-filtered + class-weighted where the live
// trainer does not — scan-1 NEW-2's evidence), held the only
// FeatureStandardizer_Persist caller (the .scaler capability is hereby dormant-by-
// decision), and kept `model_trained` semantics alive (S1-F8). The expected.cfg
// producer — the one part worth keeping — was PORTED into mh_run_one_horizon_fv.


//======================================================================================================
// [v5.10.0a.G.1 — MULTI-HORIZON TRAINING WORKER]
//======================================================================================================
// Trains N models, one per horizon in cfg.horizon_list, sharing the
// feature_matrix collected once. Each horizon recomputes its labels
// (Backtest_ComputeLabelsFromSamples with override forward_ticks),
// then trains an XGBooster on (features, this-horizon's labels), then
// saves to a per-horizon dir.
//
// Per-horizon save path (D-431 nested): models/<class>/<run>/horizon_<H>/<role>.json
// Per-horizon summary:                    .../horizon_<H>/summary_{entry|exit}.txt (D-e)
//
// Operator workflow:
//   1. Set cfg.horizon_list=100,500,1000 (CSV)
//   2. Click Collect Features (single feature collect)
//   3. Click Train Multi-Horizon — N models trained sequentially
//   4. (Future) cfg.horizon_list non-empty + Run Engine = ensemble
//      inference (G.4)
//
// LITE caveats:
//   - Models trained sequentially within this worker (each horizon
//     uses cfg.xgb_train_nthread for per-booster threads)
//   - No ensemble training-time discipline check — operator must
//     manually pick which to deploy OR rely on G.4 ensemble inference
//   - Save Run for multi-horizon: writes per-horizon dirs; Past Runs
//     panel rescan picks them up as N separate rows
//
// Click-time snapshot mirrors v5.10.0E pattern.
//======================================================================================================
//======================================================================
// [STRUCT]_[MultiHorizonWorkerArgs]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[worker-thread args for the multi-horizon training run — panel/run state + click-snapshots of run name / model path / XGBoost hyperparams / per-horizon TP-SL + WF/held-out/auto-stamp params + sell-side routing]
//======================================================================
// [CODE]
//======================================================================
struct MultiHorizonWorkerArgs {
    TrainingPanelState *state;
    RunControlState *run_control;
    char snap_run_name[64];
    char snap_model_path[256];
    int  snap_label_type;
    int  snap_max_depth;
    float snap_learning_rate;
    int  snap_n_estimators;
    float snap_subsample;
    float snap_colsample_bytree;
    int  snap_min_child_weight;
    int  snap_seed;
    int  snap_tree_method_idx;
    int  snap_horizon_count;
    int  snap_horizons[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    // v5.11.40 — per-horizon TP/SL. snap_tp_pct[h] / snap_sl_pct[h]
    // are the barrier values for horizon h (broadcast or positional
    // per the operator's CSV input + the broadcast-or-match rule).
    float snap_tp_pct[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    float snap_sl_pct[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    // v5.11.41 — Backtest_RunFullValidation needs WF + held-out + auto-stamp
    // params snap'd at click time (operator-mutable in GUI; race-free
    // capture). Mirrors FullValidationWorkerArgs pattern.
    char  snap_auto_stamp_secret[128];     // from cfg.auto_stamp_secret
    int   snap_auto_stamp_enabled;          // from cfg.auto_stamp_on_held_out
    int   snap_n_splits;                    // from state->wf_n_splits
    int   snap_buffer_ticks;                // from state->wf_buffer_ticks
    int   snap_min_train;                   // from state->wf_min_train
    float snap_gap_threshold;               // from state->fv_gap_threshold
    float snap_held_out_fraction;           // from state->fv_held_out_fraction
    // v5.13.1 — sell-side training routing + per-horizon label_kind.
    // snap_training_side = 0 (buy) leaves existing path bytewise; 1 (exit)
    // historically routed models/exit/... — that side tree is RETIRED
    // (PARITY-044); side flips the ROLE FILE, co-located in the family.
    // snap_label_kind_per_horizon[h] overrides snap_label_type per horizon
    // when its source CSV had >1 entries; otherwise broadcasts the single
    // value (back-compat with single-uniform Label Type combo).
    int  snap_training_side;
    int  snap_label_kind_per_horizon[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-22]
// [SIZE]_[664B]
// [ALIGN]_[8]
// [CACHE_LINES]_[11]
// [STRADDLE]_[snap_run_name@16 · snap_horizons@376 · snap_sl_pct@440 · snap_label_kind_per_horizon@628]
//======================================================================
// [END_STRUCT]_[MultiHorizonWorkerArgs]
//======================================================================

//======================================================================
// [FUNCTION]_[mh_run_one_horizon_fv]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[run full validation for one horizon of a multi-horizon grid + emit its stamp]
//======================================================================
// v5.11.41 — per-horizon FV helper. Extracted from train_multi_horizon_worker_fn
// for both serial-mode (called from for-loop) AND parallel-mode (called from
// per-horizon pthreads). Writes mh_horizon_fv[h] / mh_horizon_complete[h] /
// mh_horizon_status[h] / per-horizon summary.txt + fires
// Backtest_RunFullValidation (which itself fires WF + held-out + auto-stamp).
//
// Parameters:
//   state        — for status writes
//   results      — feature matrix + labels[]; in PARALLEL mode each thread
//                  passes its own isolated copy with own labels[] buffer
//   h            — horizon index (0..N-1)
//   horizon_ticks/tp_pct/sl_pct — per-horizon overrides
//   local_run_cfg — mutable copy of BacktestRunConfig; mutated per horizon
//                   to drive Backtest_ComputeLabelsFromSamples. In SERIAL,
//                   this is &run_control->run_config (shared mutate). In
//                   PARALLEL, each thread passes its own local copy.
// [REFERENCE]_[PARITY]_[PARITY-21]
//======================================================================
// [CODE]
//======================================================================
static inline void mh_run_one_horizon_fv(
    TrainingPanelState *state,
    BacktestResults *results,
    // E.1.2.C — the click-time hyperparameter snapshot. Replaces eight live
    // `state->` reads and is handed to Backtest_RunFullValidation so the booster,
    // the WF folds, the held-out model and the stamp all describe ONE architecture.
    const tt::XGBHyperparams &snap_hp,
    int h,
    int horizon_ticks,
    float tp_pct, float sl_pct,
    int label_type,
    const char *run_name,
    int snap_n_splits, int snap_buffer_ticks, int snap_min_train,
    float snap_gap_threshold, float snap_held_out_fraction,
    int snap_auto_stamp_enabled,
    const char *snap_auto_stamp_secret,
    BacktestRunConfig *local_run_cfg,
    // D-431 nested layout (S2-F4 close) — the RUN's primary label kind
    // decides the class tree ONCE for the whole family, so a mixed
    // Label-Kind CSV can no longer fragment one family across
    // classification/ + regression/ as two same-named trees. Per-horizon
    // label_type still drives role file, objective, labels and stamp.
    // NO default (AR-20 — every caller states it).
    int primary_label_type,
    // E.1.2.D leaf 5 — 1 = results->labels already filled by the caller's
    // Backtest_ComputeLabelsBatch (ONE corpus walk covers every horizon);
    // 0 = do the single-target walk here (legacy per-horizon behavior).
    // Deliberately NO default: every caller states its choice — leaf 4b is
    // the record of how a defaulted new param keeps an unwired entry point
    // invisible (AR-20).
    int labels_precomputed,
    // v5.13.1 — sell-side training. Default 0 preserves pre-v5.13.1 path
    // for legacy callers. 1 → prepend "exit/" to run_subdir routing
    // output co-located: side flips the ROLE FILE in the same nested
    // horizon dirs (the models/exit/ side tree is RETIRED, PARITY-044).
    int training_side = 0,
    // v5.15.3.B.2 — total horizon count in this multi-horizon sweep
    // (default 1 = single-horizon caller; multi-horizon callers pass N).
    // Closes PARITY-021: stamp body grid_member_count + grid_member_idx
    // were orphan-placeholder fields; this plumbs the real values.
    int horizon_count = 1)
{
    snprintf(state->mh_horizon_status[h], 128,
             "h=%d: computing labels...", horizon_ticks);

    local_run_cfg->label_forward_ticks = horizon_ticks;
    local_run_cfg->label_tp_pct        = (double)tp_pct;
    local_run_cfg->label_sl_pct        = (double)sl_pct;
    // E.1.2.C — the label KIND belongs in this per-horizon mutation set too,
    // and its absence was not cosmetic. Backtest_ComputeLabelsFromSamples
    // picks the label leaf from local_run_cfg->label_type
    // (the label pass reads local_run_cfg->label_type), which nothing here
    // wrote — so every horizon
    // recomputed labels from whatever the last COLLECT click left behind,
    // while `label_type` (the parameter) drove num_classes, the XGB
    // objective, the role file, the run_subdir and the stamp. Same run,
    // two different labels.
    //
    // That silently voided the "Label Kind CSV" feature outright: :4885
    // writes the per-horizon kind into job->label_type, so it reached the
    // objective and the stamp but NEVER the labels the model actually
    // trained on. Every horizon trained on the collect-time label and was
    // then stamped as something else.
    //
    // Safe to mutate: local_run_cfg is a per-JOB copy of saved_run_cfg
    // (:4907), documented at :4284 as "mutated per horizon" — this is
    // exactly the mutation set it exists for.
    local_run_cfg->label_type          = label_type;
    // The mutation set above stays UNCONDITIONAL even when labels arrive
    // precomputed: Backtest_RunFullValidation + the stamp read label_type /
    // fwd / tp / sl off local_run_cfg downstream. Only the corpus walk is
    // skipped — the batch already produced these exact bytes (memcmp oracle).
    if (!labels_precomputed) {
        Backtest_ComputeLabelsFromSamples(results, local_run_cfg);
    }

    int n_valid = 0;
    for (int s = 0; s < results->sample_count; ++s) {
        if (!isnan(results->labels[s]) && !isinf(results->labels[s]))
            n_valid++;
    }
    if (n_valid < 50) {
        fprintf(stderr, "[mh-train] horizon %d: only %d valid labels; skip\n",
                horizon_ticks, n_valid);
        snprintf(state->mh_horizon_status[h], 128,
                 "h=%d FAILED: only %d valid labels (need >= 50)",
                 horizon_ticks, n_valid);
        state->mh_horizon_complete[h] = 1;
        return;
    }

    int num_classes = (label_type >= 0 && label_type < LABEL_COUNT)
                      ? label_table[label_type].num_classes : 0;
    int is_multiclass = (num_classes >= 2);
    int is_regression = (num_classes == 1);
    // E.1.2.C 3-role — side selects the ROLE FILE via the extracted helper
    // (side=1 => "exit", saved CO-LOCATED; label kind stays free per (b)).
    const char* role = Training_ResolveRole(label_type, training_side);
    // D-431 nested layout — run_subdir derives from the RUN's PRIMARY kind
    // (S2-F4 close: one family = one class tree; the old per-horizon
    // derivation fragmented a mixed-CSV family across two trees).
    int primary_nc = (primary_label_type >= 0 && primary_label_type < LABEL_COUNT)
                     ? label_table[primary_label_type].num_classes : 0;
    const char* run_subdir = (primary_label_type == LABEL_PEAK_VALLEY_STABLE
                              || primary_label_type == LABEL_REGIME
                              || primary_nc >= 2)
        ? "classification" : (primary_nc == 1 ? "regression" : "classification");
    // E.1.2.C 3-retire (2026-08-20) — the models/exit/ SIDE TREE is RETIRED:
    // no loader ever walked it (PARITY-044); exit models land CO-LOCATED in
    // the same per-horizon dirs (side flips the ROLE FILE, next commit).
    //
    // D-431 — the FAMILY node is the unit: models/<class>/<family>/ holds
    // horizon_<N> children + the bundle-scoped state files. Every future
    // family is born with its bundle node (the treadmill's structural end);
    // FoxDir_CreateParents builds the whole chain.
    char family_dir[340];
    snprintf(family_dir, sizeof(family_dir), "models/%s/%s",
             run_subdir, run_name);
    char horizon_dir[360];
    ModelPath_HorizonDir(horizon_dir, sizeof(horizon_dir),
                         family_dir, (long)horizon_ticks);
    FoxDir_CreateParents(horizon_dir);

    HeldOutSplit split = HeldOutSplit_Make(results->sample_count,
                                            (double)snap_held_out_fraction);
    char unlock_token[33];
    memcpy(unlock_token, split.lock_token, sizeof(unlock_token));
    HeldOutSplit_Unlock(&split, unlock_token);

    FullValidationResults *fv = &state->mh_horizon_fv[h];
    memset(fv, 0, sizeof(*fv));
    // v5.11.47 — ALWAYS stamp. Was gated on snap_auto_stamp_enabled
    // (= cfg.auto_stamp_on_held_out which defaults to 1 but could be
    // 0 OR uninitialized if Run Control never loaded a cfg). Operator
    // wants stamps unconditionally — they're cheap, and unstamped
    // models lose load-time safety checks (label_registry_hash,
    // feature_registry_hash, model_num_outputs, etc.). Removed the
    // conditional; auto_stamp_path is always set.
    snprintf(fv->auto_stamp_path, sizeof(fv->auto_stamp_path),
             "%s/%s.json", horizon_dir, role);
    size_t n = strnlen(snap_auto_stamp_secret, 128);
    if (n >= sizeof(fv->auto_stamp_secret))
        n = sizeof(fv->auto_stamp_secret) - 1;
    memcpy(fv->auto_stamp_secret, snap_auto_stamp_secret, n);
    fv->auto_stamp_secret[n] = '\0';
    fv->auto_stamp_format_version = 0;
    (void)snap_auto_stamp_enabled;  // kept in args for back-compat; not gating now
    fv->req_label_lookahead_ticks = horizon_ticks;
    // s5-F12 — record the EFFECTIVE barrier, i.e. the one the labels were
    // actually built against, not the raw operator input. Same resolver the
    // label walk uses (Label_ResolveEffectiveTp), so train-time and stamp-time
    // cannot disagree. Before this the stamp carried the raw tp while the
    // labels carried tp+fee, and the served bracket inherited the stamp's
    // value — an M5 train-serve parity break with no observable symptom
    // (the fee is not in the stamp body either, so nothing could catch it).
    fv->req_label_tp_pct          = Label_ResolveEffectiveTp(
                                        label_type, (double)tp_pct,
                                        local_run_cfg ? local_run_cfg->label_roundtrip_fee_pct : 0.0);
    fv->req_label_sl_pct          = (double)sl_pct;
    // v5.15.3.B.2 — PARITY-021 close. Grid identification plumbed from
    // multi-horizon worker through FullValidationResults → StampArgs.
    // grid_member_count = horizon_count (total horizons), member_idx = h
    // (this horizon's index 0..N-1). Single-horizon callers (Train Model
    // button) leave defaults at 1/0/1 via the function-arg default.
    fv->req_grid_member_count = horizon_count;
    fv->req_grid_member_idx   = h;
    fv->req_horizon_count     = horizon_count;
    // v5.15.3.B.2 — also plumb expected_role from label_type so Stamp_
    // AssembleAndEmit emits args.req_role correctly. Pre-v5.15.3 this
    // came via inf.expected_role manual setter at RFV; helper expects
    // it via out->req_role.
    snprintf(fv->req_role, sizeof(fv->req_role), "%s", role);

    // v5.11.52 — train + save the FINAL deployable model BEFORE calling
    // RFV. RFV computes WF + held-out metrics + auto-stamps the file at
    // fv->auto_stamp_path, but it doesn't save a model itself (it trains
    // boosters internally for WF folds + held-out then discards). The
    // pre-v5.11.41.A worker had this train+save inline; my v5.11.41.A
    // refactor dropped it when replacing inline XGB with RFV. Without
    // a saved model file, stamp_write_for_model failed at SHA256 of
    // model_path → "could not sha256 ..." status. Restoring train+save
    // here closes that bug.
    //
    // Model trained on full feature_matrix (NaN-filtered labels). This
    // matches the held-out training's training portion ([0, trainval_end))
    // closely enough for deployment purposes — operator gets a model
    // that learned from the most data possible.
#ifdef USE_XGBOOST
    {
        snprintf(state->mh_horizon_status[h], 128,
                 "h=%d: training final model for save+stamp...", horizon_ticks);

        // count valid (non-NaN, non-Inf) labels
        int n_valid = 0;
        for (int s = 0; s < results->sample_count; ++s) {
            if (!isnan(results->labels[s]) && !isinf(results->labels[s]))
                n_valid++;
        }
        if (n_valid >= 50) {
            float *train_features = (float*)malloc((size_t)n_valid * MODEL_NUM_FEATURES * sizeof(float));
            float *train_labels   = (float*)malloc((size_t)n_valid * sizeof(float));
            if (train_features && train_labels) {
                int j = 0;
                for (int s = 0; s < results->sample_count; ++s) {
                    if (isnan(results->labels[s]) || isinf(results->labels[s])) continue;
                    memcpy(&train_features[(size_t)j * MODEL_NUM_FEATURES],
                           &results->feature_matrix[(size_t)s * MODEL_NUM_FEATURES],
                           MODEL_NUM_FEATURES * sizeof(float));
                    train_labels[j] = results->labels[s];
                    j++;
                }
                DMatrixHandle dtrain = nullptr;
                XGDMatrixCreateFromMat(train_features, n_valid, MODEL_NUM_FEATURES,
                                        std::numeric_limits<float>::quiet_NaN(), &dtrain);
                XGDMatrixSetFloatInfo(dtrain, "label", train_labels, n_valid);
                BoosterHandle booster = nullptr;
                XGBoosterCreate(&dtrain, 1, &booster);
                // E.1.2.C — the click-time snapshot, not eight live widget reads.
                tt::XGBHyperparams hp = snap_hp;
                int eval_nthread = results->config_used.xgb_eval_nthread > 0
                                 ? results->config_used.xgb_eval_nthread : 1;
                tt::XGBHyperparams_Apply(booster, hp, eval_nthread);
                int K_classes = (label_type >= 0 && label_type < LABEL_COUNT)
                              ? label_table[label_type].num_classes : 0;
                int is_multi  = (K_classes >= 2);
                int is_regr   = (K_classes == 1);
                if (is_multi) {
                    XGBoosterSetParam(booster, "objective", "multi:softprob");
                    char nc_s[8]; snprintf(nc_s, 8, "%d", K_classes);
                    XGBoosterSetParam(booster, "num_class", nc_s);
                } else if (is_regr) {
                    XGBoosterSetParam(booster, "objective", "reg:squarederror");
                } else {
                    XGBoosterSetParam(booster, "objective", "binary:logistic");
                }
                // TECH_DEBT-301a (2026-08-25) — THE SHIPPED MODEL WAS TRAINED UNWEIGHTED.
                // The WF folds and the held-out eval both applied class balance; this site — the
                // one whose artifact is actually SAVED and STAMPED — applied none. So the two
                // numbers the stamp certifies described boosters this model is not, and on a
                // 61.5/34.4/4.1 split an unweighted booster leans to the majority and rarely
                // emits the rare class, which is the buy signal. Routed through the SAME producer
                // as the other two so the three cannot drift apart again.
                float *ship_w = XGBoost_ApplyClassBalance(booster, dtrain, train_labels, n_valid,
                                                           K_classes, is_regr, is_multi,
                                                           "mh-train shipped");
                int it_completed = 0;   // E.1.2.D (scan-2 NEW-1) — real rounds only
                for (int it = 0; it < hp.n_estimators; ++it) {
                    if (state->mh_cancel) break;
                    if (XGBoosterUpdateOneIter(booster, it, dtrain) != 0) break;
                    it_completed++;
                }
                // E.1.2.D (scan-2 NEW-1) — NEVER save a zero-tree husk over a
                // real artifact. A cancel at round 0 / a first-round failure /
                // n_estimators==0 fell through to an unconditional save,
                // writing a valid-but-empty XGBoost JSON ("num_trees":"0")
                // that silently REPLACED the previous model at this path and
                // predicted base_score forever — unstamped, so every load-time
                // check was vacuous on it (S2-F6). The 516-byte
                // twins_horizon_7500/exit.json husk is the live instance.
                if (ship_w) { free(ship_w); ship_w = nullptr; }   // TECH_DEBT-301a — DMatrix copied it
                if (it_completed > 0) {
                    int save_rc = XGBoosterSaveModel(booster, fv->auto_stamp_path);
                    if (save_rc != 0) {
                        fprintf(stderr, "[mh-train] horizon %d: SaveModel(%s) failed: %s\n",
                                horizon_ticks, fv->auto_stamp_path,
                                XGBGetLastError() ? XGBGetLastError() : "(null)");
                    }
                } else {
                    fprintf(stderr, "[mh-train] horizon %d: 0 boosting rounds "
                            "completed (%s) — NOT saving over %s\n",
                            horizon_ticks,
                            state->mh_cancel ? "cancelled"
                                             : "first round failed or n_estimators==0",
                            fv->auto_stamp_path);
                }
                XGBoosterFree(booster);
                XGDMatrixFree(dtrain);
            }
            free(train_features);
            free(train_labels);
        }
    }
#endif

    snprintf(state->mh_horizon_status[h], 128,
             "h=%d: WF + held-out (%d folds)...",
             horizon_ticks, snap_n_splits);

    // E.1.2.C — hand the SAME snapshot to validation. Without this the WF folds
    // trained at 6/0.1/200 + four cfg overrides, the held-out model at pure
    // defaults, and the stamp recorded a third story — while the booster above
    // used the operator's values. Four descriptions of one run.
    Backtest_RunFullValidation(fv, results, &split,
                                snap_n_splits, horizon_ticks,
                                snap_buffer_ticks, snap_min_train,
                                &state->mh_horizon_progress[h],
                                &state->mh_cancel,
                                label_type, snap_gap_threshold,
                                /*hp_override=*/&snap_hp);

    state->mh_horizon_complete[h] = 1;

    // E.1.2.D (scan-1 NEW-6) — `label_kind == 2` was the wrong discriminant:
    // FullValidationResults.label_kind IS num_classes (0=binary, 1=regression,
    // >=2=multiclass) and no FOREACH_TARGET row has num_classes==2, so that
    // branch was unreachable and every REGRESSION horizon displayed/recorded
    // accuracy 0.00 instead of its correlation. Route through the kind SSoT.
    double wf_metric = LabelType_IsRegression(label_type)
        ? fv->walkforward.mean_val_correlation
        : fv->walkforward.mean_val_accuracy;
    double ho_metric = LabelType_IsRegression(label_type)
        ? fv->held_out_correlation : fv->held_out_metric;

    if (fv->auto_stamp_attempted && fv->auto_stamp_ok) {
        snprintf(state->mh_horizon_status[h], 128,
                 "h=%d OK: WF=%.3f HO=%.3f gap=%.3f stamped",
                 horizon_ticks, wf_metric, ho_metric,
                 fv->wf_to_held_out_gap);
    } else if (fv->ran_held_out) {
        // v5.11.47 — distinguish WHY stamp was skipped:
        //  - auto_stamp_attempted=0 → path was empty (cfg.auto_stamp_on_held_out=0
        //    OR snap was 0 at click time; OR Run Control hasn't loaded a cfg)
        //  - auto_stamp_attempted=1 + auto_stamp_ok=0 → write failed
        //    (auto_stamp_error has the reason)
        const char* skip_reason =
            !fv->auto_stamp_attempted
                ? "auto_stamp_on_held_out=0 in cfg"
                : (fv->auto_stamp_error[0] ? fv->auto_stamp_error
                                           : "unknown write error");
        snprintf(state->mh_horizon_status[h], 128,
                 "h=%d OK: WF=%.3f HO=%.3f gap=%.3f (stamp skipped: %s)",
                 horizon_ticks, wf_metric, ho_metric,
                 fv->wf_to_held_out_gap, skip_reason);
    } else if (state->mh_cancel) {
        snprintf(state->mh_horizon_status[h], 128,
                 "h=%d CANCELLED mid-validation", horizon_ticks);
    } else {
        snprintf(state->mh_horizon_status[h], 128,
                 "h=%d FAILED: held-out did not complete",
                 horizon_ticks);
    }

    char dst_summary[400];
    // E.1.2.D D-e (operator-decided) — SIDE-SUFFIXED summaries end the
    // buy/exit collision: the exit run used to OVERWRITE summary.txt,
    // destroying the buy record (measured twice — twins and run_1 both
    // lost their entry metrics to it; D4's accept+document disposition
    // failed in practice). Entry runs write summary_entry.txt, exit runs
    // summary_exit.txt; legacy summary.txt on old dirs stays readable via
    // the PastRuns_LoadOne preference chain.
    snprintf(dst_summary, sizeof(dst_summary), "%s/%s", horizon_dir,
             training_side == 1 ? "summary_exit.txt" : "summary_entry.txt");
    FILE *sf = fopen(dst_summary, "w");
    if (sf) {
        // v5.11.50 — use CANONICAL summary.txt field names that past_runs
        // reads (in PastRuns_LoadOne). Pre-fix v5.11.41.A used made-up wf_*
        // names; past_runs ignored them, leaving Train Acc/Val Acc/Gap
        // columns blank. NOW uses the same names Save Run uses so
        // multi-horizon runs render the same as single-horizon Save Run
        // bundles. Plus expected_num_classes (v5.11.49) for Classes col.
        fprintf(sf, "run: %s/horizon_%d\n", run_name, horizon_ticks);  // D-431 nested (display-only; verified unparsed)
        fprintf(sf, "role: %s\n", role);
        fprintf(sf, "model: %s/%s.json\n", horizon_dir, role);
        fprintf(sf, "label_type: %d\n", label_type);
        fprintf(sf, "expected_num_classes: %d\n", num_classes);
        // E.1.2.C — report what actually TRAINED. These were a SECOND live read of
        // the same widgets, minutes after the first, so an edit in between made
        // summary.txt disagree with the model sitting beside it — and PastRuns
        // displays the summary value as truth.
        fprintf(sf, "max_depth: %d\n", snap_hp.max_depth);
        fprintf(sf, "learning_rate: %.3f\n", (double)snap_hp.learning_rate);
        fprintf(sf, "n_estimators: %d\n", snap_hp.n_estimators);
        fprintf(sf, "label_tp_pct: %.4f\n", (double)tp_pct);
        fprintf(sf, "label_sl_pct: %.4f\n", (double)sl_pct);
        // s5 leaf-15 — lineage: which round trip this run's win threshold cleared.
        // Sourced from local_run_cfg (the config that produced these labels), the
        // same click-time-snapshot discipline as the hyperparameters above.
        fprintf(sf, "label_roundtrip_fee_pct: %.4f\n",
                local_run_cfg ? local_run_cfg->label_roundtrip_fee_pct : 0.0);
        fprintf(sf, "label_lookahead_ticks: %d\n", horizon_ticks);
        fprintf(sf, "n_train_samples: %d\n", results->sample_count);
        fprintf(sf, "label_kind: %d\n", fv->label_kind);
        fprintf(sf, "valid_folds: %d\n", fv->walkforward.valid_folds);
        // val_accuracy / val_correlation: pick whichever fits the kind.
        // past_runs reader sets has_wf_results=1 when EITHER is read.
        if (LabelType_IsRegression(label_type)) {  // regression (E.1.2.D NEW-6 — was the unreachable == 2)
            fprintf(sf, "val_correlation: %.4f\n",
                    (double)fv->walkforward.mean_val_correlation);
            fprintf(sf, "val_mse: %.6f\n",
                    (double)fv->walkforward.mean_val_mse);
        } else {  // binary or multiclass
            fprintf(sf, "val_accuracy: %.2f\n",
                    100.0 * (double)fv->walkforward.mean_val_accuracy);
            fprintf(sf, "val_stddev: %.2f\n",
                    100.0 * (double)fv->walkforward.std_val_accuracy);
        }
        fprintf(sf, "train_val_gap: %.4f\n",
                (double)fv->wf_to_held_out_gap);
        fprintf(sf, "overfit_folds: %d\n", fv->walkforward.overfit_count);
        fprintf(sf, "held_out_metric: %.4f\n", (double)fv->held_out_metric);
        fprintf(sf, "held_out_count: %d\n", fv->held_out_count);
        // accuracy = train accuracy. Past_runs reads this as `r->train_accuracy`.
        // Use mean_train_accuracy from WF folds (closest analog).
        if (fv->label_kind != 2) {
            fprintf(sf, "accuracy: %.2f\n",
                    100.0 * (double)fv->walkforward.mean_train_accuracy);
        }
        // Bookkeeping (operator may grep for these even though past_runs
        // doesn't use them):
        fprintf(sf, "ran_held_out: %d\n", fv->ran_held_out);
        fprintf(sf, "auto_stamp_attempted: %d\n", fv->auto_stamp_attempted);
        fprintf(sf, "auto_stamp_ok: %d\n", fv->auto_stamp_ok);
        if (fv->auto_stamp_ok) {
            fprintf(sf, "auto_stamp_path_written: %s\n",
                    fv->auto_stamp_path_written);
        } else if (fv->auto_stamp_attempted) {
            fprintf(sf, "auto_stamp_error: %s\n", fv->auto_stamp_error);
        }
        fclose(sf);
    }

    // D-d (2026-08-22, operator-decided) — expected.cfg gains its FIRST live
    // producer, ported from the deleted Save Run block: the mh path emits it
    // per horizon dir, so the load-side VerifyExpected + the cd9c2c7
    // label-direction check stop being vacuous (register #22 / Class 51).
    // NEW-8 dies in the port: num_classes comes from label_table (computed
    // above), never a hand-switch; hyperparams record the CLICK-TIME SNAPSHOT
    // (the dead writer recorded live panel state).
    {
        char dst_expected[400];
        snprintf(dst_expected, sizeof(dst_expected), "%s/expected.cfg", horizon_dir);
        FILE *ef = fopen(dst_expected, "w");
        if (ef) {
            fprintf(ef, "# auto-generated by foxml_suite multi-horizon train — DO NOT EDIT\n");
            fprintf(ef, "# the engine compares these against engine.cfg at load time.\n");
            fprintf(ef, "# mismatch → warning (default) or failure (model_verify_strict=1).\n\n");
            fprintf(ef, "expected_role = %s\n", role);
            fprintf(ef, "expected_label_type = %d\n", label_type);
            fprintf(ef, "expected_num_classes = %d\n", num_classes);
            fprintf(ef, "\n# ML config the model was trained against. live engine should match.\n");
            fprintf(ef, "ml_buy_threshold = %.3f\n",
                    FPN_ToDouble(results->config_used.ml_buy_threshold));
            fprintf(ef, "ml_tp_pct = %.6f\n", Money_ToDouble(results->config_used.ml_tp_pct));
            fprintf(ef, "ml_sl_pct = %.6f\n", Money_ToDouble(results->config_used.ml_sl_pct));
            fprintf(ef, "ml_backend = %d\n", results->config_used.ml_backend);
            fprintf(ef, "expected_poll_interval = %u\n",
                    results->config_used.poll_interval);
            fprintf(ef, "expected_feature_format_version = %u\n",
                    (unsigned)MODEL_FORMAT_VERSION);
            fprintf(ef, "expected_num_features = %u\n", (unsigned)MODEL_NUM_FEATURES);
            fprintf(ef, "held_out_fraction = %.4f\n",
                    FPN_ToDouble(results->config_used.held_out_fraction));
            fprintf(ef, "gap_acceptable_threshold = %.4f\n",
                    FPN_ToDouble(results->config_used.gap_acceptable_threshold));
            fprintf(ef, "\n# click-time training hyperparameters (informational)\n");
            fprintf(ef, "# max_depth = %d\n", snap_hp.max_depth);
            fprintf(ef, "# learning_rate = %.3f\n", (double)snap_hp.learning_rate);
            fprintf(ef, "# n_estimators = %d\n", snap_hp.n_estimators);
            fclose(ef);
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[mh_run_one_horizon_fv]
//======================================================================

// v5.11.41 — per-horizon parallel worker. Each thread runs ONE horizon's
// full FV pipeline against its own isolated_results (shallow copy of shared
// BacktestResults + own labels[] buffer to avoid race with other threads
// recomputing labels concurrently). Forces config_used.xgb_train_nthread=1
// for bytewise-determinism parity with serial-mode-with-nthread=1.
//======================================================================
// [STRUCT]_[MultiHorizonParallelJob]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one per-horizon parallel job — an isolated result copy + horizon index/barriers + WF/held-out/auto-stamp snapshots + local run cfg + grid-member identification (PARITY-021)]
// [REFERENCE]_[PARITY]_[PARITY-21]
//======================================================================
// [CODE]
//======================================================================
struct MultiHorizonParallelJob {
    TrainingPanelState *state;
    BacktestResults isolated_results;  // shallow copy + own labels[]
    int h;
    int horizon_ticks;
    float tp_pct, sl_pct;
    int label_type;
    char run_name[64];
    int snap_n_splits, snap_buffer_ticks, snap_min_train;
    float snap_gap_threshold, snap_held_out_fraction;
    int snap_auto_stamp_enabled;
    char snap_auto_stamp_secret[128];
    BacktestRunConfig local_run_cfg;
    // v5.13.1 — sell-side training routing. Defaults to 0 (buy) so legacy
    // parallel-mode callers preserve bytewise output paths.
    int training_side;
    // v5.15.3.B.2 — grid identification (PARITY-021 close). horizon_count = N
    // total horizons in this multi-horizon sweep. h is the per-job index
    // (already present). Plumbed into fv->req_grid_* before RFV call so
    // Stamp_AssembleAndEmit emits grid_member_count + grid_member_idx fields
    // (previously orphan-placeholder fields in FOREACH_STAMP_BOUND_MODEL_CONST
    // that no production caller populated).
    int horizon_count;
    // E.1.2.C — the click-time hyperparameter snapshot rides the job, because the
    // parallel worker cannot reach the outer scope's copy. Without it the parallel
    // path would keep reading `state->` live from N threads at once while ImGui
    // wrote the same non-atomic ints.
    tt::XGBHyperparams snap_hp;
    // E.1.2.D leaf 5 — 1 = isolated_results.labels was filled by the ONE
    // batched corpus walk before spawn (worker skips its own walk); 0 = the
    // batch fell back (buffer alloc failure) and the worker does the legacy
    // per-horizon walk itself.
    int labels_precomputed;
    // D-431 — the RUN's primary label kind (class-tree derivation is
    // once-per-family, S2-F4; per-horizon label_type above still drives
    // role/objective/labels/stamp).
    int primary_label_type;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//======================================================================
// [END_STRUCT]_[MultiHorizonParallelJob]
//======================================================================

//======================================================================
// [FUNCTION]_[mh_per_horizon_parallel_worker]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[parallel per-horizon worker for the multi-horizon sweep (caps libgomp to 1 thread)]
//======================================================================
// [REFERENCE]_[PARITY]_[PARITY-21]
//======================================================================
// [CODE]
//======================================================================
static inline void *mh_per_horizon_parallel_worker(void *arg) {
    MultiHorizonParallelJob *job = (MultiHorizonParallelJob *)arg;
    // v5.15.3.C — libgomp pthread-race landmine FIXED at process entry
    // via setenv("OMP_NUM_THREADS", "1", ...) in foxml_suite.cpp:main.
    // Per-pthread omp_set_num_threads(1) here is now defensive (process-
    // global env already set, but cheap belt-and-suspenders against any
    // accidental nested omp_set_num_threads call elsewhere in libgomp/
    // XGBoost init).
    omp_set_num_threads(1);
    omp_set_dynamic(0);
    // PROGRESS PUBLISH (2026-08-25, operator: the bar read "horizon 0/3 (current: 0 ticks)" for a
    // whole run). mh_progress / mh_current_horizon were written ONLY by the SERIAL path, and
    // parallel became the default when E.1.2.D deleted the multi_horizon_max_threads=1 override —
    // so in parallel mode the bar sat at 0 until the join, then jumped straight to N. That made a
    // running job indistinguishable from a hung one, which is also why Cancel felt inert: there
    // was no signal that anything had happened. Publish the horizon on entry; the completion
    // counter is bumped atomically below because N workers finish out of order.
    __atomic_store_n(&job->state->mh_current_horizon, job->horizon_ticks, __ATOMIC_RELAXED);
    mh_run_one_horizon_fv(
        job->state,
        &job->isolated_results,
        job->snap_hp,
        job->h,
        job->horizon_ticks,
        job->tp_pct, job->sl_pct,
        job->label_type,
        job->run_name,
        job->snap_n_splits, job->snap_buffer_ticks, job->snap_min_train,
        job->snap_gap_threshold, job->snap_held_out_fraction,
        job->snap_auto_stamp_enabled, job->snap_auto_stamp_secret,
        &job->local_run_cfg,
        job->primary_label_type,   // D-431 — class tree from the RUN's primary kind
        job->labels_precomputed,   // E.1.2.D leaf 5 — batch filled labels pre-spawn
        job->training_side,
        job->horizon_count);  // v5.15.3.B.2 PARITY-021
    // Completion tick — atomic because N workers finish out of order and a plain `volatile`
    // increment from several threads loses counts. Display-only, so RELAXED is sufficient.
    __atomic_add_fetch(&job->state->mh_progress, 1, __ATOMIC_RELAXED);
    free(job->isolated_results.labels);
    free(job);
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[mh_per_horizon_parallel_worker]
//======================================================================

//======================================================================
// [FUNCTION]_[train_multi_horizon_worker_fn]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[background thread: train a multi-horizon model grid, serial or parallel]
//======================================================================
// [REFERENCE]_[PARITY]_[PARITY-21]
//======================================================================
// [CODE]
//======================================================================
static inline void *train_multi_horizon_worker_fn(void *arg) {
    MultiHorizonWorkerArgs *args = (MultiHorizonWorkerArgs *)arg;
    TrainingPanelState *state = args->state;
    RunControlState *run_control = args->run_control;

    // Local snapshots
    char run_name[64];
    char model_path[256];
    {
        size_t n = strnlen(args->snap_run_name, sizeof(args->snap_run_name));
        if (n >= sizeof(run_name)) n = sizeof(run_name) - 1;
        memcpy(run_name, args->snap_run_name, n);
        run_name[n] = '\0';
    }
    {
        size_t n = strnlen(args->snap_model_path, sizeof(args->snap_model_path));
        if (n >= sizeof(model_path)) n = sizeof(model_path) - 1;
        memcpy(model_path, args->snap_model_path, n);
        model_path[n] = '\0';
    }
    int label_type = args->snap_label_type;
    int horizon_count = args->snap_horizon_count;
    int horizons[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    float tp_pcts[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    float sl_pcts[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    memcpy(horizons, args->snap_horizons, sizeof(horizons));
    memcpy(tp_pcts,  args->snap_tp_pct,   sizeof(tp_pcts));
    memcpy(sl_pcts,  args->snap_sl_pct,   sizeof(sl_pcts));
    // v5.11.41 — local copies of FV / auto-stamp params before free(args)
    char  snap_auto_stamp_secret[128];
    {
        size_t n = strnlen(args->snap_auto_stamp_secret,
                           sizeof(args->snap_auto_stamp_secret));
        if (n >= sizeof(snap_auto_stamp_secret))
            n = sizeof(snap_auto_stamp_secret) - 1;
        memcpy(snap_auto_stamp_secret, args->snap_auto_stamp_secret, n);
        snap_auto_stamp_secret[n] = '\0';
    }
    int   snap_auto_stamp_enabled = args->snap_auto_stamp_enabled;
    int   snap_n_splits           = args->snap_n_splits;
    int   snap_buffer_ticks       = args->snap_buffer_ticks;
    int   snap_min_train          = args->snap_min_train;
    float snap_gap_threshold      = args->snap_gap_threshold;
    float snap_held_out_fraction  = args->snap_held_out_fraction;
    // E.1.2.C — capture the EIGHT hyperparameter snaps that the click handlers have
    // populated since v5.11.41 and that NOTHING has ever read. The worker was reading
    // `state->` live instead, from a worker thread, so an operator edit during the
    // label pass (seconds to minutes) silently changed the model that got saved; the
    // same fields were read AGAIN at summary-write time, so summary.txt could
    // disagree with the model beside it; and in parallel mode N threads read
    // non-atomic ints while ImGui wrote them. Capturing them here makes the snap
    // block do the job it was written for, and gives us ONE value to hand to both
    // the booster and the validation trainers so they cannot describe different
    // architectures. Class 13, "snap block complete, consumer bypasses it".
    // E.1.2.D leaf 14 — the ONE value-mapper (was the second hand-copy)
    tt::XGBHyperparams snap_hp = tt::XGBHyperparams_FromRaw(
        args->snap_max_depth, (float)args->snap_learning_rate,
        args->snap_n_estimators, args->snap_subsample,
        args->snap_colsample_bytree, args->snap_min_child_weight,
        args->snap_seed, args->snap_tree_method_idx);
    // v5.13.5.B (parity-check audit gap-close 2026-05-08) — copy NEW
    // v5.13.5 snap fields to stack BEFORE free(args). Without this,
    // subsequent reads at the parallel-job populate +
    // the serial-mode call would dereference freed
    // memory → undefined label_kind in stamp + wrong/random training_side
    // path routing. Same pattern as horizons/tp_pcts/sl_pcts above.
    int snap_label_kind_per_horizon[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    memcpy(snap_label_kind_per_horizon,
           args->snap_label_kind_per_horizon,
           sizeof(snap_label_kind_per_horizon));
    int snap_training_side = args->snap_training_side;
    free(args);

    BacktestResults *results = &run_control->results;

#ifdef USE_XGBOOST
    if (horizon_count <= 0) {
        snprintf(state->status_msg, sizeof(state->status_msg),
                 "Multi-horizon: cfg.horizon_list empty; set horizons first.");
        state->mh_complete = 1;
        state->mh_running = 0;
        return NULL;
    }
    if (results->sample_count <= 0) {
        snprintf(state->status_msg, sizeof(state->status_msg),
                 "Multi-horizon: Collect Features first.");
        state->mh_complete = 1;
        state->mh_running = 0;
        return NULL;
    }

    fprintf(stderr, "[mh-train] starting multi-horizon train: %d horizons, "
                    "%d samples, run_name='%s'\n",
            horizon_count, results->sample_count, run_name);

    // Save the original RunConfig + restore at end (we mutate
    // label_forward_ticks per horizon).
    BacktestRunConfig saved_run_cfg = run_control->run_config;

    // v5.11.41 — clear per-horizon state arrays before the loop. operator
    // may have run Multi-Horizon previously; stale mh_horizon_* arrays
    // would confuse the GUI panel.
    for (int h = 0; h < TrainingPanelState::PANEL_HORIZON_MAX; ++h) {
        memset(&state->mh_horizon_fv[h], 0, sizeof(FullValidationResults));
        state->mh_horizon_complete[h] = 0;
        state->mh_horizon_progress[h] = 0;
        state->mh_horizon_status[h][0] = '\0';
    }
    state->mh_total = horizon_count;

    // v5.15.3.C — libgomp landmine FIXED at process entry (setenv
    // OMP_NUM_THREADS=1 in foxml_suite.cpp:main). XGBoost trainings across
    // pthreads no longer race on libgomp's shared parallel-region state
    // because the process-global single-thread mode is set before any
    // libgomp init. Per-pthread workers can now safely run concurrent
    // XGBoost without the v5.11.44 omp_set_num_threads(1) per-pthread
    // workaround (kept defensively) or the v5.11.45 forced-serial clamp
    // (REMOVED in this ship). cfg.multi_horizon_max_threads now honors
    // operator's actual setting: 0 = auto (= horizon_count, fully
    // parallel), N = cap to N concurrent.
    int mh_max_threads = results->config_used.multi_horizon_max_threads;
    if (mh_max_threads <= 0) {
        mh_max_threads = horizon_count;  // 0 = auto = fully parallel
    }
    int n_parallel = horizon_count < mh_max_threads ? horizon_count : mh_max_threads;
    int parallel_mode = (n_parallel >= 2 && horizon_count >= 2);

    int trained = 0;
    int saved_count = 0;
    int validated = 0;

    // E.1.2.D leaf 5 — ONE batched corpus walk labels every horizon up front
    // (was: one full walk per horizon, and in parallel mode N of them running
    // SIMULTANEOUSLY against the same disk). Targets carry the per-horizon
    // (kind, fwd, tp, sl); both modes consume the vectors below and skip the
    // in-place walk. Buffer-alloc failure degrades to the legacy per-horizon
    // walks (mh_batch_ok=0), never to wrong labels. A corpus abort keeps
    // precomputed=1: the NAN prefill makes every horizon refuse on 0 valid
    // labels — the legacy path would have re-walked the broken corpus N more
    // times and, worse, counted whatever stale labels sat in results->labels.
    float *mh_label_bufs[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX] = {0};
    int mh_batch_ok = 1;
    {
        LabelBatchTarget bt[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
        for (int h = 0; h < horizon_count; ++h) {
            mh_label_bufs[h] = (float *)malloc(
                (size_t)results->sample_count * sizeof(float));
            if (!mh_label_bufs[h]) { mh_batch_ok = 0; break; }
            bt[h] = LabelBatchTarget{};
            bt[h].label_type    = snap_label_kind_per_horizon[h];
            bt[h].tp_pct        = (double)tp_pcts[h];
            bt[h].sl_pct        = (double)sl_pcts[h];
            bt[h].forward_ticks = horizons[h];
            bt[h].out_labels    = mh_label_bufs[h];
        }
        if (mh_batch_ok) {
            int labeled = Backtest_ComputeLabelsBatch(results, &saved_run_cfg,
                                                      bt, horizon_count);
            if (labeled < 0) {
                fprintf(stderr, "[mh-train] batched label pass aborted; "
                                "horizons will refuse on 0 valid labels\n");
            } else if (!parallel_mode) {
                // Serial-mode legacy stats parity: each per-horizon walk used
                // to fold its NaN counters into results->stats (the S3-F3
                // accumulate semantics — leaf 13 owns re-thinking them);
                // parallel mode folded into discarded isolated copies. Both
                // preserved (serial folds all-up-front vs per-iteration; the
                // post-run totals are identical).
                for (int h = 0; h < horizon_count; ++h) {
                    results->stats.nan_labels_total   += bt[h].nan_total;
                    results->stats.nan_labels_dropped += bt[h].nan_dropped;
                }
            }
        } else {
            fprintf(stderr, "[mh-train] label batch buffer alloc failed; "
                            "falling back to per-horizon label walks\n");
        }
    }

    if (parallel_mode) {
        fprintf(stderr, "[mh-train] parallel mode: %d horizons across %d threads "
                        "(xgb_train_nthread pinned to 1 per thread for parity)\n",
                horizon_count, n_parallel);

        // v5.11.41.C — spawn one pthread per horizon. Each thread:
        //   - shallow-copies BacktestResults (shared read-only feature_matrix)
        //   - allocates own labels[] buffer (avoids race with concurrent
        //     Backtest_ComputeLabelsFromSamples calls in other threads)
        //   - shallow-copies cfg with xgb_train_nthread=1 forced (parity
        //     contract — same value as serial mode would produce when cfg
        //     has nthread=1, and bytewise-deterministic across mode).
        pthread_t tids[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX] = {0};
        int spawned[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX] = {0};
        for (int h = 0; h < horizon_count; ++h) {
            if (state->mh_cancel) break;
            MultiHorizonParallelJob *job =
                (MultiHorizonParallelJob *)malloc(sizeof(MultiHorizonParallelJob));
            if (!job) {
                snprintf(state->mh_horizon_status[h], 128,
                         "h=%d FAILED: malloc job arg", horizons[h]);
                state->mh_horizon_complete[h] = 1;
                continue;
            }
            job->state = state;
            job->snap_hp = snap_hp;   // E.1.2.C — one snapshot, every worker
            // Shallow-copy results; labels[] = the horizon's batch-filled
            // vector (ownership TRANSFERS to the job — the worker frees it;
            // E.1.2.D leaf 5). Legacy own-malloc only on batch fallback.
            job->isolated_results = *results;
            if (mh_batch_ok) {
                job->isolated_results.labels = mh_label_bufs[h];
                mh_label_bufs[h] = NULL;   // consumed — post-join free skips it
            } else {
                job->isolated_results.labels =
                    (float *)malloc((size_t)results->sample_count * sizeof(float));
            }
            if (!job->isolated_results.labels) {
                snprintf(state->mh_horizon_status[h], 128,
                         "h=%d FAILED: malloc labels[]", horizons[h]);
                state->mh_horizon_complete[h] = 1;
                free(job);
                continue;
            }
            job->labels_precomputed = mh_batch_ok;
            // Pin xgb_train_nthread=1 + xgb_eval_nthread=1 in the isolated
            // cfg for parity vs serial-mode-with-nthread=1 AND for parallel-
            // mode safety (WF folds inside RFV use xgb_eval_nthread). Both
            // must be 1 to prevent libgomp OpenMP team collisions across
            // pthreads (v5.11.44 hotfix: also paired with omp_set_num_threads(1)
            // in the worker entry).
            job->isolated_results.config_used.xgb_train_nthread = 1;
            job->isolated_results.config_used.xgb_eval_nthread  = 1;
            job->h = h;
            job->horizon_count = horizon_count;  // v5.15.3.B.2 PARITY-021
            job->horizon_ticks = horizons[h];
            job->tp_pct = tp_pcts[h];
            job->sl_pct = sl_pcts[h];
            // v5.13.1.B — per-horizon label_kind. Click-handler snap
            // populates each slot with either the per-horizon CSV value
            // (when N>1 entries given) or the broadcast (single value /
            // empty CSV). Worker reads array directly.
            // v5.13.5.B (parity-check gap-close 2026-05-08) — read from
            // stack-local snap (post-free(args)), not args->* (freed).
            job->label_type = snap_label_kind_per_horizon[h];
            // D-431 — the RUN's primary kind rides the job so the class tree
            // derives once per family (S2-F4), not per horizon.
            job->primary_label_type = label_type;
            // v5.13.1.A — per-job training_side (stack-local copy)
            job->training_side = snap_training_side;
            {
                size_t n = strnlen(run_name, sizeof(job->run_name) - 1);
                memcpy(job->run_name, run_name, n);
                job->run_name[n] = '\0';
            }
            job->snap_n_splits          = snap_n_splits;
            job->snap_buffer_ticks      = snap_buffer_ticks;
            job->snap_min_train         = snap_min_train;
            job->snap_gap_threshold     = snap_gap_threshold;
            job->snap_held_out_fraction = snap_held_out_fraction;
            job->snap_auto_stamp_enabled = snap_auto_stamp_enabled;
            {
                size_t n = strnlen(snap_auto_stamp_secret,
                                   sizeof(job->snap_auto_stamp_secret) - 1);
                memcpy(job->snap_auto_stamp_secret, snap_auto_stamp_secret, n);
                job->snap_auto_stamp_secret[n] = '\0';
            }
            // Local cfg copy — per-thread mutation of label_*; restored
            // implicitly when thread exits (job is freed).
            job->local_run_cfg = saved_run_cfg;

            int rc = pthread_create(&tids[h], NULL,
                                     mh_per_horizon_parallel_worker, job);
            if (rc != 0) {
                snprintf(state->mh_horizon_status[h], 128,
                         "h=%d FAILED: pthread_create rc=%d", horizons[h], rc);
                state->mh_horizon_complete[h] = 1;
                free(job->isolated_results.labels);
                free(job);
                continue;
            }
            spawned[h] = 1;
        }
        // Join all spawned threads. Throttle concurrency by limiting
        // simultaneous joins is not necessary — pthread_join just blocks
        // until each completes. Total wall time is max-per-horizon time.
        for (int h = 0; h < horizon_count; ++h) {
            if (spawned[h]) pthread_join(tids[h], NULL);
        }
        // Tally counters from per-horizon FV results
        for (int h = 0; h < horizon_count; ++h) {
            if (!state->mh_horizon_complete[h]) continue;
            FullValidationResults *fv = &state->mh_horizon_fv[h];
            trained++;
            if (fv->ran_held_out) validated++;
            if (fv->auto_stamp_ok) saved_count++;
        }
        // Workers already counted themselves up; this is the terminal pin (a cancelled or
        // failed worker may not have ticked, and the bar must still read complete at the end).
        state->mh_progress = horizon_count;
    } else {
        // Serial mode (existing v5.11.41.A behavior; now via helper)
        fprintf(stderr, "[mh-train] serial mode: %d horizons sequential "
                        "(xgb_train_nthread=%d from cfg)\n",
                horizon_count, results->config_used.xgb_train_nthread);
        for (int h = 0; h < horizon_count; ++h) {
            if (state->mh_cancel) {
                fprintf(stderr, "[mh-train] cancelled at horizon %d/%d\n",
                        h, horizon_count);
                break;
            }
            state->mh_current_horizon = horizons[h];
            state->mh_progress = h + 1;

            // v5.13.1.B — per-horizon label_kind from snap (broadcast
            // already applied at click time when CSV had ≤1 entries).
            // v5.13.5.B (parity-check gap-close) — read stack-local snap
            // (args is freed by the worker before these reads).
            int per_horizon_lk = snap_label_kind_per_horizon[h];
            // E.1.2.D leaf 5 — consume the batch vector (bytewise what the
            // in-place walk produced; memcmp-pinned) instead of re-walking
            // the corpus for every horizon.
            if (mh_batch_ok) {
                memcpy(results->labels, mh_label_bufs[h],
                       (size_t)results->sample_count * sizeof(float));
            }
            mh_run_one_horizon_fv(
                state, results, snap_hp, h,
                horizons[h], tp_pcts[h], sl_pcts[h],
                per_horizon_lk, run_name,
                snap_n_splits, snap_buffer_ticks, snap_min_train,
                snap_gap_threshold, snap_held_out_fraction,
                snap_auto_stamp_enabled, snap_auto_stamp_secret,
                &run_control->run_config,
                label_type,        // primary_label_type (D-431 — the run's snap primary)
                mh_batch_ok,       // labels_precomputed (E.1.2.D leaf 5)
                snap_training_side,
                horizon_count);  // v5.15.3.B.2 PARITY-021

            FullValidationResults *fv = &state->mh_horizon_fv[h];
            trained++;
            if (fv->ran_held_out) validated++;
            if (fv->auto_stamp_ok) saved_count++;
        }
    }

    // E.1.2.D leaf 5 — release batch vectors (parallel transferred consumed
    // slots to jobs and NULLed them; serial memcpys and keeps ownership;
    // free(NULL) is a no-op for every consumed/never-allocated slot).
    for (int h = 0; h < horizon_count; ++h) free(mh_label_bufs[h]);

    // Restore RunConfig
    run_control->run_config = saved_run_cfg;

    snprintf(state->status_msg, sizeof(state->status_msg),
             "Multi-horizon: %d/%d horizons trained, %d validated (held-out), "
             "%d stamped. Models in models/<class>/%s_horizon_*/.",
             trained, horizon_count, validated, saved_count, run_name);
#else
    snprintf(state->status_msg, sizeof(state->status_msg),
             "Multi-horizon: XGBoost not compiled in (build with -DUSE_XGBOOST=ON)");
    (void)results;
    (void)label_type;
    (void)horizon_count;
    (void)horizons;
    (void)run_name;
    (void)model_path;
#endif

    state->mh_complete = 1;
    state->mh_running = 0;
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[train_multi_horizon_worker_fn]
//======================================================================

//======================================================================
// [FUNCTION]_[GUI_Panel_Training]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[render the Training panel — collect features, WF, held-out, optimizer, multi-horizon, and model training/stamping]
//======================================================================
// [REFERENCE]_[INVARIANT]_[[H1] [H2]]
//======================================================================
// [CODE]
//======================================================================
static inline void GUI_Panel_Training(TrainingPanelState *state,
                                       RunControlState *run_control,
                                       DataPanelState *data) {
    ImGui::Begin("Training");

    // label config — display names derived from label_table (single source of truth).
    // adding a label = 1 entry in LabelFunctions.hpp::label_table[]; this dropdown auto-updates.
    static const char *label_names[LABEL_COUNT];
    static bool label_names_built = false;
    if (!label_names_built) {
        for (int i = 0; i < LABEL_COUNT; i++) {
            label_names[i] = label_table[i].display_name;
        }
        label_names_built = true;
    }
    // E.1.2.C — sell-side training selects the ROLE FILE, not a side tree:
    // side=1 emits exit.json CO-LOCATED with the buy roles (next commit),
    // where the engine's ensemble loader already looks. No cfg pointing step.
    static const char* side_names[2] = {"Buy (entry signals)", "Exit (sell signals)"};
    int prev_training_side = state->ui_training_side;
    ImGui::Combo("Training Side", &state->ui_training_side, side_names, 2);
    // E.1.2.C 3-role (b) — flipping to the exit side defaults the label to
    // WILL_PEAK (P(peak) is exactly what the exit_threshold consumer wants);
    // the operator can still override to PVS etc. below. Mirrors the
    // label-combo retarget pattern; broadcast flows at click time.
    if (state->ui_training_side != prev_training_side) {
        if (state->ui_training_side == 1) state->label_type = LABEL_WILL_PEAK;
        // E.1.2.C — CLEAR the per-horizon Label-Kind CSV on a side flip, or the
        // retarget above is a lie on the multi-horizon path. The MH click reads
        // `bcast_lk = (ui_label_kind_per_horizon_count > 0)
        //             ? ui_label_kind_per_horizon[0] : state->label_type`
        // (see the snap block below), so a NON-EMPTY CSV makes `state->label_type`
        // unreachable — the combo would display "Will Peak" while every horizon
        // trained on the buy-side kind the CSV still held. The single-horizon
        // click reads the combo directly and was never affected, which is why
        // this only bit the multi-horizon path.
        //
        // Clearing (rather than retargeting) is the honest choice: a per-horizon
        // label set typed for the ENTRY side carries no meaning on the exit side,
        // and an emptied CSV makes the visible combo authoritative again. It also
        // keeps the CSV consistent with its own render gate, which hides the input
        // entirely for WILL_PEAK — a retarget would leave a populated, invisible,
        // un-editable CSV driving training.
        //
        // Only fires on an actual side CHANGE, so a deliberately-typed exit-side
        // CSV survives every subsequent click.
        state->ui_label_kind_csv[0] = '\0';
        state->ui_label_kind_per_horizon_count = 0;
        snprintf(state->model_path, sizeof(state->model_path), "models/%s.json",
                 Training_ResolveRole(state->label_type, state->ui_training_side));
    }
    // E.1.2.C 3-role (F3) — the side x label gate, enforced at the PRODUCER
    // where label truth lives (no wire key can see it): side=1 with an
    // entry-goodness label would train a semantically INVERTED exit model.
    //   0 = REFUSE   1 = WARN (allowed, yellow hint)   2 = OK
    //
    // The tier rule itself now lives in Training_SideLabelGate (LabelFunctions.hpp),
    // extracted from the lambda that used to sit here so the ANSI test TU can drive
    // the REAL function — this was the one leg of the D2 verdict without a pin, and
    // an inline replica is the Class-51 shape the plan's OUT-list replica died of.
    //
    // AGGREGATION over the EFFECTIVE label set (E.1.2.C): the gate used to read
    // state->label_type alone, so the per-horizon "Label Kind CSV" walked straight
    // past it — an OK combo selection could carry a REFUSE-tier horizon in behind
    // it. That hole widened the moment the CSV started reaching the labels the model
    // actually trains on. Worst (numerically lowest) tier across the set wins.
    int side_gate = Training_SideLabelGate(state->label_type, state->ui_training_side);
    for (int lk = 0; lk < state->ui_label_kind_per_horizon_count && lk < 8; ++lk) {
        int t = Training_SideLabelGate(state->ui_label_kind_per_horizon[lk],
                                       state->ui_training_side);
        if (t < side_gate) side_gate = t;
    }
    if (side_gate == 0) {
        ImGui::TextColored(FoxmlColors::red,
            "exit side: label '%s' trains an ENTRY-goodness objective — inverted as an exit "
            "signal. Use Will Peak (default) or Peak/Valley/Stable.",
            label_table[state->label_type].display_name);
    } else if (side_gate == 1) {
        ImGui::TextColored(FoxmlColors::yellow,
            "exit side: label '%s' is untriaged for exit semantics — proceed deliberately.",
            label_table[state->label_type].display_name);
    }
    ImGui::SetItemTooltip(
        "Buy: trains entry-signal models (default). Output:\n"
        "  models/<run_subdir>/<run>/horizon_<N>/<role>.json\n\n"
        "Exit: trains sell-point models for the exit_predictor slots.\n"
        "Output (CO-LOCATED, auto-discovered by the engine):\n"
        "  models/<run_subdir>/<run>/horizon_<N>/exit.json\n\n"
        "No cfg step needed: the engine walks exit.json siblings under\n"
        "node_N_model_dir automatically (E.1.2.C).");

    int prev_label_type = state->label_type;
    ImGui::Combo("Label Type", &state->label_type, label_names, LABEL_COUNT);
    // v4.2.2: when label type changes, retarget the default Model Path so it
    // matches the role this label trains. Pre-patch, the path stayed at the
    // legacy "models/buy_signal.json" regardless of label type — confusing
    // since 3-class barrier models would then save under a binary-role name
    // (Save Run still rewrote it to barrier.json on bundle, but the in-progress
    // training output had the wrong filename). Now the field tracks the role.
    if (state->label_type != prev_label_type) {
        const char* role = Training_ResolveRole(state->label_type,
                                                 state->ui_training_side);  // E.1.2.C 3-role
        snprintf(state->model_path, sizeof(state->model_path), "models/%s.json", role);
    }
    ImGui::SetItemTooltip("How to label each sample for ML training:\n"
                          "  Win/Loss: 1 if price hits TP%% first, 0 if SL%% first\n"
                          "  Barrier: same but returns 0.5 (neutral) if neither hit within horizon\n"
                          "  Forward P&L: 1 if price is higher N ticks later, 0 if lower\n"
                          "  Regime: labels by detected regime (multi-class)\n"
                          "  Vol Barrier: k * rolling_vol barriers (FoxML formulation)\n"
                          "  Will Peak / Will Valley: binary classifiers for legacy 2-model BarrierGate\n"
                          "  Peak/Valley/Stable: 3-class softmax (PRIMARY for BarrierGate) —\n"
                          "    saves to barrier.json for zoo auto-discovery");

    // TP/SL barriers — used by win_loss, barrier, vol_barrier, peak_valley_stable
    if (state->label_type == LABEL_WIN_LOSS || state->label_type == LABEL_BARRIER ||
        state->label_type == LABEL_VOL_BARRIER || state->label_type == LABEL_PEAK_VALLEY_STABLE) {
        // v5.11.40 — TP/SL fields now accept comma-separated values for
        // per-horizon mapping (broadcast-or-match rule). Single value
        // (e.g. "0.030") works as before — applies to every horizon.
        // CSV "0.020,0.030,0.040" maps positionally where N matches
        // the horizon count. Misalignment disables the Multi-Horizon
        // button with a hint (see render below).
        //
        // Backward compat: state->label_tp_pct / _sl_pct floats remain
        // the source-of-truth for single-horizon Train Model + cfg I/O
        // + Save Run output. The parser keeps them in sync with
        // ui_tp_pct_csv[0] / ui_sl_pct_csv[0] (first parsed value).
        //
        // First-time render: if CSV string is empty, seed it from the
        // existing label_tp_pct float (operator's stored cfg value).
        if (state->ui_tp_pct_csv[0] == '\0') {
            snprintf(state->ui_tp_pct_csv, sizeof(state->ui_tp_pct_csv),
                     "%.3f", state->label_tp_pct);
        }
        if (state->ui_sl_pct_csv[0] == '\0') {
            snprintf(state->ui_sl_pct_csv, sizeof(state->ui_sl_pct_csv),
                     "%.3f", state->label_sl_pct);
        }

        ImGui::InputText("TP Barrier %",
                         state->ui_tp_pct_csv, sizeof(state->ui_tp_pct_csv));
        ImGui::SetItemTooltip("UNIT: percent of price — 0.5 = 0.5%% (= 50 bps).\n"
                              "Take-profit barrier as %% of price\n"
                              "label = 1 (or VALLEY for 3-class) if price moves up this much before SL is hit\n"
                              "wider = fewer but higher-confidence labels\n"
                              "tip: 0.050 = 5 bps. For short horizons (~1k ticks) at BTC scale,\n"
                              "0.05-0.10%% gives balanced labels; 0.3+ usually = 99%% \"stable\".\n\n"
                              "v5.11.40 multi-horizon: comma-separated values map positionally\n"
                              "to Horizons (CSV), e.g. '0.020,0.030,0.040' for 3 horizons.\n"
                              "A single value (e.g. '0.030') broadcasts to all horizons.");
        ImGui::InputText("SL Barrier %",
                         state->ui_sl_pct_csv, sizeof(state->ui_sl_pct_csv));
        ImGui::SetItemTooltip("UNIT: percent of price — 0.5 = 0.5%% (= 50 bps).\n"
                              "Stop-loss barrier as %% of price\n"
                              "label = 0 (or PEAK for 3-class) if price drops this much before TP is hit\n"
                              "wider = fewer but higher-confidence labels\n\n"
                              "v5.11.40 multi-horizon: same CSV format as TP — single value\n"
                              "broadcasts; N values map positionally to Horizons (CSV).");
        // s5 leaf-15 — the round-trip cost the WIN threshold must clear.
        ImGui::InputFloat("Round-trip Fee %", &state->label_roundtrip_fee_pct,
                          0.01f, 0.05f, "%.3f");
        if (state->label_roundtrip_fee_pct < 0.0f) state->label_roundtrip_fee_pct = 0.0f;
        ImGui::SetItemTooltip("UNIT: percent of price — 0.2 = 0.2%% (= 20 bps).\n"
                              "The ROUND TRIP (entry + exit) cost a winning trade must clear.\n"
                              "Added to the TP barrier ONLY when labeling: a move that hits\n"
                              "TP but not TP+fee is not a win you could have banked.\n\n"
                              "Venue-general on purpose — set it from YOUR venue's taker\n"
                              "schedule (Binance taker 0.1%% both sides = 0.2), not from the\n"
                              "engine's fee cfg. 0 = fee-blind labels (pre-2026-08-23 behavior).\n\n"
                              "SL is NOT adjusted: it is a price-level stop — fees deepen the\n"
                              "realized loss but do not move where it fires.\n"
                              "Recorded in the run summary + the model stamp for lineage.");

        // Parse CSVs every render frame (cheap; max 8 entries, bounded loop).
        // Same shape as the horizons-CSV parser. After parsing, sync
        // ui_*_per_horizon[] arrays + index-0 → label_*_pct floats.
        auto parse_pct_csv = [](const char* csv, float* out, int* n_out) {
            int n = 0;
            const char* p = csv;
            while (*p && n < 8) {
                while (*p == ' ' || *p == '\t' || *p == ',') p++;
                if (!*p) break;
                char* end = nullptr;
                float v = strtof(p, &end);
                if (end == p) break;
                if (v >= 0.0f && v <= 100.0f) out[n++] = v;
                p = end;
            }
            *n_out = n;
        };
        parse_pct_csv(state->ui_tp_pct_csv,
                      state->ui_tp_per_horizon, &state->ui_tp_per_horizon_count);
        parse_pct_csv(state->ui_sl_pct_csv,
                      state->ui_sl_per_horizon, &state->ui_sl_per_horizon_count);

        // v5.13.1.B — per-horizon label_kind CSV input. Mirrors TP/SL
        // CSV pattern: empty → broadcast state->label_type combo;
        // single value → broadcast that value to all horizons; N values
        // → positional map to Horizons (CSV) with broadcast-or-match
        // alignment. Format: integer label_type values per LABEL_*
        // (LabelFunctions.hpp). Operator types e.g. "0,2,1" →
        // horizon_0=binary, horizon_1=multiclass, horizon_2=regression.
        ImGui::InputText("Label Kind CSV",
                         state->ui_label_kind_csv,
                         sizeof(state->ui_label_kind_csv));
        // Tooltip iterates label_table[] live so adding a new label
        // (1 row in FOREACH_TARGET) auto-updates the lookup.
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(
                "Per-horizon label_kind (integer LABEL_* enum values).\n\n"
                "Empty: all horizons use the Label Type combo above.\n"
                "Single value: broadcasts to all horizons.\n"
                "N values: positional map to Horizons CSV.\n"
                "  N must equal Horizons count or Train Multi-Horizon disables.\n\n"
                "Trains heterogeneous mixed-output ensembles in ONE click;\n"
                "v5.12.3.B+E mixed-output normalizer blends them at inference.");
            ImGui::Separator();
            ImGui::TextUnformatted("Lookup (auto-synced from FOREACH_TARGET):");
            for (int i = 0; i < LABEL_COUNT; i++)
                ImGui::Text("  %2d  %s", i, label_table[i].display_name);
            ImGui::EndTooltip();
        }

        // Parse the label_kind CSV (same pattern as TP/SL CSV).
        auto parse_int_csv = [](const char* csv, int* out, int* n_out) {
            int n = 0;
            const char* p = csv;
            while (*p && n < 8) {
                while (*p == ' ' || *p == '\t' || *p == ',') p++;
                if (!*p) break;
                char* end = nullptr;
                long v = strtol(p, &end, 10);
                if (end == p) break;
                if (v >= 0 && v < LABEL_COUNT) out[n++] = (int)v;
                p = end;
            }
            *n_out = n;
        };
        parse_int_csv(state->ui_label_kind_csv,
                      state->ui_label_kind_per_horizon,
                      &state->ui_label_kind_per_horizon_count);
        // Keep label_tp_pct / _sl_pct in sync with index 0 — the
        // backward-compat path for single-horizon Train Model.
        if (state->ui_tp_per_horizon_count > 0)
            state->label_tp_pct = state->ui_tp_per_horizon[0];
        if (state->ui_sl_per_horizon_count > 0)
            state->label_sl_pct = state->ui_sl_per_horizon[0];
    }
    // v5.11.43 — Forward Ticks / Lookahead Ticks inputs DELETED. Horizons CSV
    // (rendered below) is the single source for label_forward_ticks. For
    // single-horizon mode (1 entry in CSV), horizons[0] is used as
    // label_forward_ticks. For multi-horizon (N entries), each horizon
    // gets its own label_forward_ticks during the per-horizon worker loop.
    // Sync logic in click handlers: state->label_forward_ticks =
    //   state->ui_horizon_list[0] when ui_horizon_count >= 1.

    // v5.11.43 — parse horizon CSV early (was later, post-Train-Model).
    // Collect Features + Train Model buttons need ui_horizon_count to decide
    // whether to render single-mode or multi-horizon-mode variant. Parsing
    // here makes ui_horizon_count fresh BEFORE any button conditional fires.
    // (The same parser ran later in v5.11.40 — duplicating here doesn't hurt;
    // CSV parse is microseconds.)
    {
        int n = 0;
        const char* p = state->ui_horizon_csv;
        while (*p && n < 8) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (!*p) break;
            char* end = nullptr;
            long v = strtol(p, &end, 10);
            if (end == p) break;
            if (v > 0 && v <= 1000000)
                state->ui_horizon_list[n++] = (int)v;
            p = end;
        }
        state->ui_horizon_count = n;
    }
    // v5.11.43 — sync label_forward_ticks from horizons[0] when single-horizon
    // mode (so Collect Features + Train Model use horizons[0] without needing
    // a separate Lookahead Ticks input). Multi-horizon mode (N>1) overrides
    // per horizon inside the worker loop.
    if (state->ui_horizon_count >= 1) {
        state->label_forward_ticks = state->ui_horizon_list[0];
    }

    // v5.11.43 — compute effective horizon count up-front. When operator's
    // UI CSV is empty, fall back to cfg.horizon_list (back-compat). This
    // value drives the auto-routing button visibility (single_horizon_mode)
    // so legacy cfg-driven multi-horizon flows still work even when the
    // operator hasn't typed anything in the UI input.
    int panel_eff_horizon_count = state->ui_horizon_count > 0
                                 ? state->ui_horizon_count
                                 : run_control->results.config_used.horizon_count;

    ImGui::Separator();

    // collect features button — disabled if no data selected OR a backtest is
    // already running (mirrors the Walk-Forward pattern). prevents the
    // "click button N times because nothing visibly happens" UX trap that
    // fires N parallel backtests each writing to the same log file.
    //
    // v5.11.43 — auto-route by horizon count. Operator types horizons in
    // Horizons CSV; only the matching button is rendered. <=1 horizon =
    // "Collect Features" (single-mode worker). >1 = "Collect Multi-Horizon"
    // (multi-horizon worker). Both still write to results->feature_matrix.
    bool has_data = data->selected_count > 0;
    // E.1.2.C — `&& !Training_AnyWorkerRunning(state)` closes the realloc-under-worker
    // hazard: Collect runs Backtest_Run, which reallocs (and therefore MOVES)
    // feature_matrix/labels while a training worker holds a shallow copy.
    bool can_collect = has_data && !run_control->running && side_gate != 0
                       && !Training_AnyWorkerRunning(state);  // E.1.2.C F3
    // v5.11.43 — uses panel_eff_horizon_count (UI takes priority, falls back
    // to cfg.horizon_list). 0 or 1 = single mode; >1 = multi-horizon mode.
    bool single_horizon_mode = (panel_eff_horizon_count <= 1);

    if (single_horizon_mode) {
    if (!can_collect) ImGui::BeginDisabled();
    if (ImGui::Button("Collect Features")) {
        // clear previous training/walk-forward results on re-collect
        state->status_msg[0] = '\0';
        state->wf_has_results = false;
        // set up run config with feature collection enabled
        run_control->run_config.num_data_files = 0;
        for (int i = 0; i < data->file_count && run_control->run_config.num_data_files < MAX_DATA_FILES; i++) {
            if (data->selected[i]) {
                strncpy(run_control->run_config.data_paths[run_control->run_config.num_data_files],
                        data->files[i], 255);
                run_control->run_config.num_data_files++;
            }
        }
        strncpy(run_control->run_config.config_path, run_control->config_path, 255);
        run_control->run_config.use_config_override = 0;
        run_control->run_config.collect_features = 1;
        run_control->run_config.label_type = state->label_type;
        run_control->run_config.label_tp_pct = state->label_tp_pct;
        run_control->run_config.label_sl_pct = state->label_sl_pct;
        // s5 leaf-15 — the fee rides the same click-time copy as its siblings.
        run_control->run_config.label_roundtrip_fee_pct = state->label_roundtrip_fee_pct;
        run_control->run_config.label_forward_ticks = state->label_forward_ticks;

        // start the run
        run_control->progress_pct = 0;
        run_control->cancel_flag = 0;
        run_control->complete = 0;
        memset(&run_control->stats_snapshot, 0, sizeof(run_control->stats_snapshot));
        run_control->running = 1;

        if (run_control->candle_acc)
            CandleAccumulator_Init(run_control->candle_acc, 60);

        BacktestWorkerArgs *args = (BacktestWorkerArgs *)malloc(sizeof(BacktestWorkerArgs));
        args->state = run_control;
        pthread_create(&run_control->worker_tid, NULL, backtest_worker_fn, args);
        pthread_detach(run_control->worker_tid);
    }
    ImGui::SetItemTooltip(
        "Runs a backtest AND gathers ML training samples (features + labels)\n"
        "for every slow-path cycle. Required before Train Model.\n\n"
        "Output goes to results->feature_matrix (in-memory). The dataset\n"
        "rebuilds every time you click — use Run Control's Run Backtest if\n"
        "you only need stats and want to skip the sample collection cost.");
    if (!can_collect) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (run_control->running) {
            ImGui::TextColored(FoxmlColors::yellow, "running... (%d%%)", run_control->progress_pct);
        } else {
            ImGui::TextDisabled("Select data files first");
        }
    } else if (run_control->running) {
        // safety belt: if running flag flipped while button was enabled (race), still warn
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::yellow, "running... (%d%%)", run_control->progress_pct);
    }
    } // end single_horizon_mode (Collect Features)

    // v5.11.24 — Collect Multi-Horizon button. Mirrors Train Multi-Horizon's
    // pattern (uses state->ui_horizon_csv populated by the input field below).
    // Disabled when no horizons configured OR a backtest is already running.
    // Clicking spawns collect_multi_horizon_worker_fn which collects features
    // ONCE then loops over horizons recomputing labels + logging valid-sample
    // counts to engine.log. Final state: last horizon's labels in
    // results->labels[] (Train Multi-Horizon will recompute per horizon
    // during training, so no data loss).
    // v5.11.43 — only render Collect Multi-Horizon when N>1 horizons typed.
    // Single horizon → operator sees "Collect Features" only (rendered above).
    // N>1 → operator sees "Collect Multi-Horizon" only (rendered here).
    int mh_collect_horizon_count = state->ui_horizon_count;
    // v5.11.40 — broadcast-or-match alignment for per-horizon TP/SL.
    // Allowed: single value (broadcasts) OR N values where N matches
    // horizon count. Anything else disables the Multi-Horizon button
    // with a hint. tp_aligned + sl_aligned both must be true.
    int tp_n = state->ui_tp_per_horizon_count;
    int sl_n = state->ui_sl_per_horizon_count;
    bool tp_aligned = (tp_n <= 1) || (tp_n == mh_collect_horizon_count);
    bool sl_aligned = (sl_n <= 1) || (sl_n == mh_collect_horizon_count);
    bool mh_can_collect = has_data && !run_control->running
                          && !Training_AnyWorkerRunning(state)   // E.1.2.C — see can_collect
                          && mh_collect_horizon_count > 0
                          && tp_aligned && sl_aligned
                          && side_gate != 0;  // E.1.2.C F3
    if (!single_horizon_mode) {
    if (!mh_can_collect) ImGui::BeginDisabled();
    if (ImGui::Button("Collect Multi-Horizon")) {
        // clear previous training/walk-forward results
        state->status_msg[0] = '\0';
        state->wf_has_results = false;
        // build run_config (mirrors single-horizon Collect Features above)
        run_control->run_config.num_data_files = 0;
        for (int i = 0; i < data->file_count
                          && run_control->run_config.num_data_files < MAX_DATA_FILES; i++) {
            if (data->selected[i]) {
                strncpy(run_control->run_config.data_paths[run_control->run_config.num_data_files],
                        data->files[i], 255);
                run_control->run_config.num_data_files++;
            }
        }
        strncpy(run_control->run_config.config_path, run_control->config_path, 255);
        run_control->run_config.use_config_override = 0;
        run_control->run_config.collect_features = 1;
        run_control->run_config.label_type = state->label_type;
        run_control->run_config.label_tp_pct = state->label_tp_pct;
        run_control->run_config.label_sl_pct = state->label_sl_pct;
        // s5 leaf-15 — the fee rides the same click-time copy as its siblings.
        run_control->run_config.label_roundtrip_fee_pct = state->label_roundtrip_fee_pct;
        run_control->run_config.label_forward_ticks = state->label_forward_ticks;

        run_control->progress_pct = 0;
        run_control->cancel_flag = 0;
        run_control->complete = 0;
        memset(&run_control->stats_snapshot, 0, sizeof(run_control->stats_snapshot));
        run_control->running = 1;

        if (run_control->candle_acc)
            CandleAccumulator_Init(run_control->candle_acc, 60);

        auto *args = (CollectMultiHorizonWorkerArgs *)malloc(
            sizeof(CollectMultiHorizonWorkerArgs));
        args->run_control = run_control;
        args->snap_horizon_count = mh_collect_horizon_count;
        // v5.11.40 — snap per-horizon TP/SL using broadcast-or-match.
        // single value (count==1) broadcasts to all horizons; N values
        // map positionally. Single label_tp_pct/_sl_pct float as
        // ultimate fallback (CSV totally empty).
        float bcast_tp = (state->ui_tp_per_horizon_count > 0)
            ? state->ui_tp_per_horizon[0] : state->label_tp_pct;
        float bcast_sl = (state->ui_sl_per_horizon_count > 0)
            ? state->ui_sl_per_horizon[0] : state->label_sl_pct;
        for (int i = 0; i < ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX; ++i) {
            args->snap_horizons[i] = (i < mh_collect_horizon_count)
                ? state->ui_horizon_list[i] : 0;
            args->snap_tp_pct[i] = (state->ui_tp_per_horizon_count > 1
                                    && i < state->ui_tp_per_horizon_count)
                ? state->ui_tp_per_horizon[i] : bcast_tp;
            args->snap_sl_pct[i] = (state->ui_sl_per_horizon_count > 1
                                    && i < state->ui_sl_per_horizon_count)
                ? state->ui_sl_per_horizon[i] : bcast_sl;
        }
        pthread_t tid;
        pthread_create(&tid, NULL, collect_multi_horizon_worker_fn, args);
        pthread_detach(tid);
    }
    if (!mh_can_collect) ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        "v5.11.24 — Collects features ONCE, then loops over each horizon\n"
        "in 'Horizons (CSV)' (below) recomputing labels per horizon.\n\n"
        "Useful for inspecting per-horizon label class distribution\n"
        "BEFORE committing to a multi-horizon train run. Per-horizon\n"
        "valid-sample counts go to engine.log.\n\n"
        "Final results->labels[] holds the LAST horizon's labels;\n"
        "Train Multi-Horizon will recompute per horizon during\n"
        "training, so nothing is lost.");

    // v5.11.40 — TP/SL misalignment hint. When operator typed a
    // multi-value TP/SL CSV that doesn't broadcast or match horizon
    // count, surface an explicit reason next to the disabled button.
    if (mh_collect_horizon_count > 0 && (!tp_aligned || !sl_aligned)) {
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::yellow,
            "(misaligned: TP=%d, SL=%d, horizons=%d — need 1 or %d each)",
            tp_n, sl_n, mh_collect_horizon_count, mh_collect_horizon_count);
    }
    } // end !single_horizon_mode (Collect Multi-Horizon)

    // v5.11.43 — Horizons (CSV) input ALWAYS visible. Single source of
    // truth for both Collect/Train mode auto-routing AND single-horizon
    // label_forward_ticks. Type "1000" for single-horizon mode (one
    // training run); type "1000,7500,15000" for multi-horizon (N parallel
    // trainings). v5.11.28 rendered this at the top to mirror the train
    // side; v5.11.43 dropped the train-side mirror so this is now the
    // ONLY Horizons input.
    ImGui::PushItemWidth(220);
    ImGui::InputText("Horizons (CSV)##collect",
                     state->ui_horizon_csv,
                     sizeof(state->ui_horizon_csv));
    ImGui::PopItemWidth();
    ImGui::SetItemTooltip(
        "Comma-separated forward-tick horizons. Single source of truth.\n\n"
        "  '1000'              → single-horizon mode\n"
        "                        (Collect Features + Train Model render)\n"
        "  '1000,7500,15000'   → multi-horizon mode (N parallel trainings)\n"
        "                        (Collect Multi-Horizon + Train Multi-Horizon\n"
        "                         render; Train auto-spawns N pthreads)\n\n"
        "Empty falls back to cfg.horizon_list. Max 8 horizons,\n"
        "each 1..1,000,000 ticks.");
    ImGui::SameLine();
    ImGui::TextDisabled("(%d horizon%s parsed)",
                        state->ui_horizon_count,
                        state->ui_horizon_count == 1 ? "" : "s");

    // results pointer for Train Model + Walk-Forward sections below — they
    // need sample_count + feature_matrix + labels, all of which are safe to
    // read by the time those sections run (Train Model runs synchronously
    // on the UI thread; Walk-Forward worker is its own thread that doesn't
    // collide with backtest_worker_fn).
    BacktestResults *results = &run_control->results;

    // show feature collection status — display reads from a worker-written
    // snapshot, NOT from results->labels[] directly.
    //
    // Why: results->labels (+ sample_count, sample_capacity) is written by
    // the worker thread during collection and realloc'd as the buffer grows.
    // GUI rendering at 60fps that iterated those buffers raced with worker
    // reallocs → use-after-free → segfault on a 2.25M-sample run on
    // 2026-04-25. Snapshot pattern: worker computes the distribution stats
    // ONCE after Backtest_Run completes (in backtest_worker_fn) and sets
    // running=0 last. GUI reads the snapshot when running==0. The volatile
    // flag prevents compiler reordering of the loads/stores, giving a
    // happens-before edge.
    //
    // Bonus: the diagnostic compute happens once per run, not every render
    // frame. Iterating millions of labels every frame was wasteful even
    // when it didn't crash.
    const SamplesSnapshot *snap = &run_control->stats_snapshot;
    if (snap->sample_count > 0) {
        // FoxML colors for diagnostics
        const ImVec4 diag_green  = ImVec4(0.55f, 0.76f, 0.51f, 1.0f);
        const ImVec4 diag_yellow = ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
        const ImVec4 diag_red    = ImVec4(0.95f, 0.35f, 0.35f, 1.0f);

        if (snap->label_kind == 1) {
            // regression: continuous labels — show distribution stats, not +/- counts.
            float lmin = snap->lmin, lmax = snap->lmax;
            float mean = snap->lmean, stddev = snap->lstddev;
            // expose to state for downstream display
            state->train_label_min    = lmin;
            state->train_label_max    = lmax;
            state->train_label_mean   = mean;
            state->train_label_stddev = stddev;
            ImGui::Text("Samples: %d  |  range: [%.4f, %.4f]  |  mean: %.4f  |  σ: %.4f",
                         snap->sample_count, lmin, lmax, mean, stddev);
            ImGui::SetItemTooltip("Regression labels — continuous target (e.g. forward %% return).\n"
                                  "range: min and max observed values\n"
                                  "mean: average label value (close to 0 for return-style targets)\n"
                                  "σ: standard deviation — wider σ = more spread, more learnable signal\n\n"
                                  "If σ ≈ 0, all samples have nearly the same label and the model\n"
                                  "has nothing to predict. Larger σ relative to typical XGBoost\n"
                                  "step size (~eta × leaf_value) means the model can fit something.");

            // Diagnosis: interpret the distribution for the user
            const ImVec4 *dcol = &diag_green;
            const char *dtext = "looks normal — distribution shape is reasonable for continuous returns";
            const char *dtip  = "Sanity checks all pass: σ is meaningful, range isn't pathological,\n"
                                "and the asymmetry isn't extreme. Move on to training and read the\n"
                                "Pearson r in walk-forward.";
            if (stddev < 0.0001) {
                dcol = &diag_red;
                dtext = "σ ≈ 0 — labels are essentially constant, nothing for model to predict";
                dtip  = "All samples have nearly the same label. The model can only output a\n"
                        "single constant value, which is useless. Check the label generator —\n"
                        "this is usually a bug (e.g. all samples computed against the same\n"
                        "reference price).";
            } else {
                float abs_min = lmin < 0 ? -lmin : lmin;
                float abs_max = lmax < 0 ? -lmax : lmax;
                float asym = (abs_max > 0.001f && abs_min > 0.001f)
                             ? (abs_min > abs_max ? abs_min / abs_max : abs_max / abs_min)
                             : 1.0f;
                if (asym > 10.0f) {
                    dcol = &diag_red;
                    dtext = "range is wildly asymmetric — likely a label-generator bug";
                    dtip  = "abs(min) and abs(max) differ by >10x. For unbiased forward returns\n"
                            "on a roughly stationary asset, this should not happen. Most common\n"
                            "cause: label generator with a reference-price bug, file-boundary\n"
                            "edge case, or division-by-near-zero. Check label_table[t].fn.";
                } else if (abs_min > 50.0f || abs_max > 50.0f) {
                    dcol = &diag_yellow;
                    dtext = "range has very large values — verify label units (% vs raw)";
                    dtip  = "Label values exceed ±50. If labels are %% returns, BTC moving 50%%\n"
                            "in the lookahead window is implausible at tick scale → likely a bug.\n"
                            "If labels are raw $ deltas, this is fine but XGBoost may want a\n"
                            "scaled feature.";
                }
            }
            ImGui::TextColored(*dcol, "Diagnosis: %s", dtext);
            ImGui::SetItemTooltip("%s", dtip);
        } else if (snap->label_kind == 2) {
            // multiclass: per-class histogram from snapshot.
            int K = snap->num_classes > 16 ? 16 : snap->num_classes;
            // build display string: "Samples: N  |  c0: X (Y%)  |  c1: ..."
            char buf[256];
            int off = snprintf(buf, sizeof(buf), "Samples: %d  ", snap->sample_count);
            for (int k = 0; k < K && off < (int)sizeof(buf) - 1; k++) {
                off += snprintf(buf + off, sizeof(buf) - off, "|  c%d: %d (%.1f%%)  ",
                                k, snap->class_counts[k],
                                snap->sample_count > 0
                                    ? 100.0f * snap->class_counts[k] / snap->sample_count : 0.0f);
            }
            ImGui::TextUnformatted(buf);
            ImGui::SetItemTooltip("Multiclass labels — per-class sample counts.\n"
                                  "c0..cK-1 = class index (e.g. for Peak/Valley/Stable:\n"
                                  "  c0=stable, c1=peak, c2=valley)\n\n"
                                  "Heavy imbalance (one class >90%%) means the model can\n"
                                  "trivially predict that class for high accuracy. Consider\n"
                                  "class-rebalanced training (per-sample weights) or different\n"
                                  "label parameters (barrier widths, lookahead horizon).");

            // Diagnosis for multiclass: check imbalance
            const ImVec4 *mc_col = &diag_green;
            const char *mc_text = "balanced — classes have similar populations, model can learn each";
            const char *mc_tip  = "No class dominates >70%%. The per-sample inverse-frequency\n"
                                  "weights (already applied during training) should let the model\n"
                                  "differentiate. Read multiclass accuracy + per-class precision\n"
                                  "carefully.";
            int max_count = 0;
            int max_class = 0;
            int counts_used = 0;
            for (int k = 0; k < K; k++) {
                if (snap->class_counts[k] > max_count) {
                    max_count = snap->class_counts[k]; max_class = k;
                }
                if (snap->class_counts[k] > 0) counts_used++;
            }
            float max_pct = snap->sample_count > 0
                ? 100.0f * max_count / snap->sample_count : 0.0f;
            if (counts_used <= 1) {
                mc_col = &diag_red;
                mc_text = "all samples in one class — labels are degenerate";
                mc_tip  = "Every sample got the same class. Likely a label-generator bug or\n"
                          "extreme barrier widths / horizons that prevent any other class\n"
                          "from triggering. Try different label parameters or check the\n"
                          "label function.";
            } else if (max_pct > 95.0f) {
                mc_col = &diag_red;
                mc_text = "extreme imbalance — model will trivially predict majority class";
                mc_tip  = "One class is >95%% of samples. Per-sample weights help but the\n"
                          "model has very few minority-class samples to actually learn from.\n"
                          "Consider tighter barrier widths (more decisive labels), longer\n"
                          "lookahead, or a different label scheme entirely (try Forward P&L\n"
                          "regression — sidesteps class imbalance).";
            } else if (max_pct > 70.0f) {
                mc_col = &diag_yellow;
                mc_text = "moderate imbalance — minority classes underrepresented";
                mc_tip  = "Largest class is >70%%. Per-sample weights compensate in the loss,\n"
                          "but if minority-class signal is what you want to capture, fewer\n"
                          "training examples = noisier learning. Watch the per-class accuracy\n"
                          "after training, not just overall.";
            }
            ImGui::TextColored(*mc_col, "Diagnosis: c%d dominates at %.1f%% — %s",
                               max_class, max_pct, mc_text);
            ImGui::SetItemTooltip("%s", mc_tip);
        } else {
            // binary: +/-/neutral counts from snapshot
            state->positive_count = snap->pos_count;
            state->negative_count = snap->neg_count;
            int neutral_count = snap->neutral_count;
            int labeled = snap->pos_count + snap->neg_count;
            ImGui::Text("Samples: %d  |  +: %d  |  -: %d  |  neutral: %d  |  Ratio: %.1f%%",
                         snap->sample_count, snap->pos_count, snap->neg_count,
                         neutral_count,
                         labeled > 0
                             ? (float)snap->pos_count / labeled * 100.0f : 0.0f);
            ImGui::SetItemTooltip("Binary labels.\n"
                                  "+: labeled as buy signal (price hit TP barrier first)\n"
                                  "-: labeled as no-buy (price hit SL barrier first)\n"
                                  "neutral: neither barrier hit within horizon (excluded from training)\n"
                                  "Ratio: +/(+ + -) — class balance among non-neutral labels\n\n"
                                  "50%% ratio is ideal for balanced training\n"
                                  "high neutral %% is normal with barrier labels + tight barriers\n"
                                  "extreme imbalance (<5%%) → classifier trivially predicts majority,\n"
                                  "scale_pos_weight is auto-applied to compensate.");

            // Diagnosis for binary: check ratio + neutral fraction
            const ImVec4 *bn_col = &diag_green;
            const char *bn_text = "well-balanced for training — model has both classes to learn from";
            const char *bn_tip  = "Ratio is in [30%%, 70%%] and neutral fraction is reasonable.\n"
                                  "Ready to train. After training, read walk-forward val accuracy.";
            float ratio = labeled > 0 ? 100.0f * snap->pos_count / labeled : 0.0f;
            float neutral_pct = snap->sample_count > 0
                ? 100.0f * neutral_count / snap->sample_count : 0.0f;
            if (labeled == 0) {
                bn_col = &diag_red;
                bn_text = "all samples are neutral — no class labels to train on";
                bn_tip  = "100%% neutral means neither TP nor SL was hit within the horizon\n"
                          "for any sample. Either widen the horizon (Lookahead Ticks), tighten\n"
                          "the barriers (TP%% / SL%%), or check the label generator.";
            } else if (ratio < 5.0f || ratio > 95.0f) {
                bn_col = &diag_red;
                bn_text = "extreme imbalance — scale_pos_weight will compensate but minority class is sparse";
                bn_tip  = "+/(+ + -) is outside [5%%, 95%%]. The auto-applied scale_pos_weight\n"
                          "rebalances the loss, but the minority class still has very few\n"
                          "training examples. Consider symmetric barriers (TP=SL) or a\n"
                          "different label.";
            } else if (ratio < 20.0f || ratio > 80.0f) {
                bn_col = &diag_yellow;
                bn_text = "skewed — scale_pos_weight active, watch val accuracy";
                bn_tip  = "Ratio outside [20%%, 80%%]. scale_pos_weight is doing real work\n"
                          "here. Walk-forward val_accuracy is more meaningful than raw\n"
                          "training accuracy in this regime.";
            } else if (neutral_pct > 95.0f) {
                bn_col = &diag_yellow;
                bn_text = "very high neutral fraction — most samples excluded from training";
                bn_tip  = "Less than 5%% of samples got a definitive label. The model only\n"
                          "trains on the resolved minority. Ratio looks OK but n_valid is\n"
                          "small. Consider longer horizon or wider barriers if walk-forward\n"
                          "shows high variance across folds.";
            }
            ImGui::TextColored(*bn_col, "Diagnosis: %s", bn_text);
            ImGui::SetItemTooltip("%s", bn_tip);
        }
    }

    ImGui::Separator();

    // XGBoost hyperparameters
    ImGui::Text("XGBoost Parameters");
    ImGui::InputInt("Max Depth", &state->max_depth, 1, 2);
    ImGui::SetItemTooltip("Max tree depth — controls model complexity\n"
                          "lower = simpler model, less overfitting\n"
                          "2-3: conservative, 4-6: moderate, 8+: high risk of memorization");
    ImGui::InputFloat("Learning Rate", &state->learning_rate, 0.01f, 0.1f, "%.3f");
    ImGui::SetItemTooltip("How much each tree contributes (eta)\n"
                          "lower = needs more estimators but generalizes better\n"
                          "0.01-0.05: conservative, 0.1: moderate, 0.3+: aggressive");
    ImGui::InputInt("Estimators", &state->n_estimators, 10, 50);
    ImGui::SetItemTooltip("Number of boosting rounds (trees)\n"
                          "more trees + low learning rate = better but slower\n"
                          "too many = overfitting (check walk-forward gap)");

    // v5.9.5h — advanced hyperparameter section. Defaults match
    // pre-v5.9.5h hardcoded values; non-tuning operators don't need to
    // touch these. Operators wanting to tune get cfg-bound fields with
    // stamp recording for drift forensics.
    if (ImGui::CollapsingHeader("Advanced (v5.9.5h)")) {
        ImGui::SliderFloat("Subsample", &state->ui_subsample, 0.5f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("Row subsample per tree (0.5-1.0)\n"
                              "Lower = more variance reduction, less overfitting\n"
                              "Default 0.8 (matches pre-v5.9.5h hardcoded)");
        ImGui::SliderFloat("ColSample/Tree", &state->ui_colsample_bytree, 0.5f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("Column subsample per tree (0.5-1.0)\n"
                              "Lower = less feature-importance bias\n"
                              "Default 0.8 (matches pre-v5.9.5h hardcoded)");
        ImGui::InputInt("Min Child Weight", &state->ui_min_child_weight, 1, 5);
        if (state->ui_min_child_weight < 1) state->ui_min_child_weight = 1;
        if (state->ui_min_child_weight > 50) state->ui_min_child_weight = 50;
        ImGui::SetItemTooltip("Min sum-of-weights per leaf (1-50)\n"
                              "Higher = more regularization\n"
                              "Default 5 (matches pre-v5.9.5h hardcoded)");
        ImGui::InputInt("Seed", &state->ui_seed, 1, 100);
        ImGui::SetItemTooltip("RNG seed for reproducible runs\n"
                              "Same seed + same data + same hyperparams = same model\n"
                              "Default 42 (matches pre-v5.9.5h hardcoded)");
        // E.1.2.D leaf 14 — render the SHARED table (was the fourth hand-copy)
        ImGui::Combo("Tree Method", &state->ui_tree_method_idx,
                     const_cast<const char**>(tt::XGB_TREE_METHOD_NAMES),
                     tt::XGB_TREE_METHOD_COUNT);
        ImGui::SetItemTooltip("XGBoost tree construction algorithm\n"
                              "hist (default): fast histogram, recommended\n"
                              "exact: slow but precise (small datasets only)\n"
                              "approx: histogram alternative\n"
                              "auto: XGBoost picks (varies by version)");
    }

    // v5.11.48 — only show Model Path in single-horizon mode. In multi-mode
    // the worker auto-generates save paths from run_name + horizon dir, so
    // Model Path is unused noise. Run Walk-Forward worker doesn't use it
    // either (operates on results->* in memory). Run Full Validation worker
    // uses it for auto_stamp_path output, but Multi-Horizon worker has its
    // own per-horizon stamp path already.
    if (single_horizon_mode) {
        ImGui::InputText("Model Path (Save Run output)", state->model_path, sizeof(state->model_path));
        ImGui::SetItemTooltip(
            "Output path for Run Full Validation auto-stamp.\n"
            "Single-horizon mode only.\n\n"
            "NOT used by Train Model / Train Multi-Horizon — those auto-generate\n"
            "save paths from Run Name + horizon dir naming convention\n"
            "(models/<class>/<run_name>_horizon_<H>/<role>.json).");
    }

    // v5.11.48 — Run Name prefix input rendered HERE (before Train buttons)
    // so operator sees + sets it BEFORE clicking train. The same field is
    // also rendered post-train near Save Run (legacy location)
    // so operator can rename for Save Run if needed. Both edit the same
    // state->run_name buffer.
    ImGui::InputText("Run Name (prefix)", state->run_name, sizeof(state->run_name));
    ImGui::SetItemTooltip(
        "Prefix for save paths. Worker auto-appends \"_horizon_<H>\" per horizon.\n\n"
        "Example: Run Name \"btc_5min\" + horizons 1000,7500,15000 →\n"
        "  models/<class>/btc_5min_horizon_1000/barrier.json\n"
        "  models/<class>/btc_5min_horizon_7500/barrier.json\n"
        "  models/<class>/btc_5min_horizon_15000/barrier.json\n\n"
        "Re-running with the same prefix overwrites previous results — pick\n"
        "a unique name per experiment (e.g. btc_5min_v1, btc_5min_v2, ...).");
    // Live preview of what dirs will be created
    if (state->run_name[0] != '\0' && state->ui_horizon_count > 0) {
        // E.1.2.C — this was the FOURTH and last hand-copy of the label->role rule,
        // and the only operator-FACING one, so it was the one that lied to a human.
        // It ignored ui_training_side, so with Training Side = Exit it advertised
        // "barrier.json" / "buy_signal.json" while the worker wrote exit.json. Now
        // calls the ONE extracted rule, same as the trainer, Save Run and the boot
        // walk. (Its three siblings were closed earlier in E.1.2.C; this completes
        // the class rather than leaving the visible one wrong.)
        const char* role_preview =
            Training_ResolveRole(state->label_type, state->ui_training_side);
        // ...and this ternary returned "classification" in BOTH arms, so a
        // regression label advertised the classification tree while :4364-4367
        // routed the write to models/regression/. Derive it the same way the
        // writer does: num_classes == 1 means regression.
        const int nclass_preview = (state->label_type >= 0 && state->label_type < LABEL_COUNT)
                                 ? label_table[state->label_type].num_classes : 0;
        const char* class_preview = (nclass_preview == 1) ? "regression" : "classification";
        ImGui::TextDisabled("Will write to: models/%s/%s_horizon_<%s>/%s.json",
                            class_preview, state->run_name,
                            state->ui_horizon_count == 1 ? "H" : "H1,H2,...",
                            role_preview);
    }

    // v5.9.0d worker lineage — the original train_model_worker_fn was DELETED
    // at D-d (2026-08-22); Train Model routes through the multi-horizon worker
    // (v5.11.44) whose per-horizon results table is the live display.
    //
    // v5.10.0a-bugfix1 — cross-worker mutual exclusion. Pre-bugfix, operator
    // could click Run Walk-Forward then Run Full Validation, spawning two
    // concurrent training pthreads. XGBoost's internal global state +
    // PhaseTimer_Global() singleton are NOT safe under that concurrency on
    // some builds; result was a segfault when GUI thread tried to read
    // worker-mutating state on click-back. Fix: gate all training buttons
    // on a single "any_worker_running" predicate so only ONE worker runs
    // at a time. Operator-friendly: button is disabled with a tooltip
    // "(another training task is running)" rather than crashing.
    // E.1.2.C — the one predicate, hoisted; see Training_AnyWorkerRunning.
    bool any_worker_running = Training_AnyWorkerRunning(state);
    // E.1.2.C — `&& side_gate != 0` is the half F3 was missing. The tier's own
    // comment claimed "buttons disabled", but side_gate reached only the two
    // COLLECT predicates, so a REFUSE-tier label could still be TRAINED from
    // samples a previous collect had left behind: collect at side=Buy, flip to
    // Exit, pick any label, Train. The gate rendered red and stopped nothing.
    // E.1.2.D (scan-1 NEW-5) — `!run_control->running` closes the REVERSE
    // direction of the leaf-6 exclusion: a collect/backtest reallocs + MOVES
    // (or Reset()s) the shared results buffers, so no trainer may start while
    // Run Control is live. Leaf 6 gated collect-during-train; this gates
    // train-during-collect. mh_can_train derives from can_train and inherits.
    bool can_train = results->sample_count >= 10 && !any_worker_running
                     && !run_control->running
                     && side_gate != 0;
#ifndef USE_XGBOOST
    can_train = false;
#endif

    // v5.11.44 — single-mode now routes through Multi-Horizon worker. We
    // gate the legacy tm_running progress bar on tm_running specifically
    // (legacy worker still exists for back-compat callers). Multi-Horizon
    // running state is shown by the per-horizon table + mh progress bar
    // further down. New mh_running covers BOTH single (N=1) + multi (N>1).
    if (state->tm_running) {
        // v5.11.25 — real per-iteration progress bar. Pre-fix used a
        // pulsing indeterminate bar (`-1.0f * GetTime()`) as a "still
        // alive" signal because the XGBoost C-API had no progress
        // callback hook — but this file's per-iteration training loop
        // already uses XGBoosterUpdateOneIter
        // (added for cancel support, v5.9.0d), so the worker can
        // publish current_iter to a volatile field cheaply. Operator
        // sees actual % done + iter count.
        // v5.11.29 — post-iter phases. After iter loop completes the
        // worker still does 1-5s of save-model + scaler + auto-stamp;
        // tm_phase_msg gets updated at each transition. GUI shows the
        // phase msg as the overlay when set, else falls back to iter
        // count.
        int p_total = state->tm_progress_total;
        int p_iter  = state->tm_progress_iter;
        const char* phase = (const char*)state->tm_phase_msg;
        bool have_phase = phase[0] != '\0';
        if (p_total > 0) {
            float frac = (float)p_iter / (float)p_total;
            char overlay[96];
            if (have_phase) {
                // post-iter phase active — keep bar full + show phase
                snprintf(overlay, sizeof(overlay),
                         "Training XGBoost... %s", phase);
                ImGui::ProgressBar(1.0f, ImVec2(-1, 0), overlay);
            } else {
                snprintf(overlay, sizeof(overlay),
                         "Training XGBoost... iter %d / %d", p_iter, p_total);
                ImGui::ProgressBar(frac, ImVec2(-1, 0), overlay);
            }
        } else {
            // Pre-loop: still allocating dtrain, no iters started yet.
            const char* preparing = have_phase ? phase : "(preparing)";
            char overlay[96];
            snprintf(overlay, sizeof(overlay),
                     "Training XGBoost... %s", preparing);
            ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(),
                               ImVec2(-1, 0), overlay);
        }
        if (ImGui::Button("Cancel Training")) {
            state->tm_cancel = 1;
        }
    } else {
        // v5.11.43 — auto-route by horizon count. Single-horizon (count<=1)
        // shows "Train Model"; multi-horizon (count>1) shows "Train Multi-Horizon".
        if (single_horizon_mode) {
        if (!can_train) ImGui::BeginDisabled();
        if (ImGui::Button("Train Model")) {
            // v5.11.44 — route Train Model through the Multi-Horizon worker
            // with N=1. This makes single-horizon training run the same
            // train+WF+held-out+stamp pipeline that Multi-Horizon does, in
            // ONE click (no separate Run Walk-Forward / Run Full Validation
            // needed). Per-horizon results table renders 1 row.
            state->status_msg[0] = '\0';
            state->wf_has_results = false;
            memset(&state->wf_results, 0, sizeof(state->wf_results));
            state->mh_running = 1;
            state->mh_progress = 0;
            state->mh_total = 1;  // N=1 in single-horizon mode
            state->mh_current_horizon = 0;
            state->mh_cancel = 0;
            state->mh_complete = 0;

            // Build MultiHorizonWorkerArgs (same as Train Multi-Horizon
            // click handler below, but with N=1 horizon).
            int single_h = (state->ui_horizon_count >= 1)
                         ? state->ui_horizon_list[0]
                         : (state->label_forward_ticks > 0
                            ? state->label_forward_ticks : 1000);
            float single_tp = (state->ui_tp_per_horizon_count > 0)
                            ? state->ui_tp_per_horizon[0] : state->label_tp_pct;
            float single_sl = (state->ui_sl_per_horizon_count > 0)
                            ? state->ui_sl_per_horizon[0] : state->label_sl_pct;

            // E.1.2.C GUI polish (a) — click-time horizon snapshot for the
            // per-horizon results table (the render must never read the
            // live-reparsed ui_horizon_list).
            state->mh_horizon_ticks[0] = single_h;
            for (int i = 1; i < TrainingPanelState::PANEL_HORIZON_MAX; ++i)
                state->mh_horizon_ticks[i] = 0;

            MultiHorizonWorkerArgs *mh_args =
                (MultiHorizonWorkerArgs *)malloc(sizeof(MultiHorizonWorkerArgs));
            mh_args->state = state;
            mh_args->run_control = run_control;
            {
                size_t n = strnlen(state->run_name, sizeof(state->run_name));
                if (n >= sizeof(mh_args->snap_run_name))
                    n = sizeof(mh_args->snap_run_name) - 1;
                memcpy(mh_args->snap_run_name, state->run_name, n);
                mh_args->snap_run_name[n] = '\0';
            }
            {
                size_t n = strnlen(state->model_path, sizeof(state->model_path));
                if (n >= sizeof(mh_args->snap_model_path))
                    n = sizeof(mh_args->snap_model_path) - 1;
                memcpy(mh_args->snap_model_path, state->model_path, n);
                mh_args->snap_model_path[n] = '\0';
            }
            mh_args->snap_label_type     = state->label_type;
            mh_args->snap_max_depth      = state->max_depth;
            mh_args->snap_learning_rate  = state->learning_rate;
            mh_args->snap_n_estimators   = state->n_estimators;
            mh_args->snap_subsample        = state->ui_subsample;
            mh_args->snap_colsample_bytree = state->ui_colsample_bytree;
            mh_args->snap_min_child_weight = state->ui_min_child_weight;
            mh_args->snap_seed             = state->ui_seed;
            mh_args->snap_tree_method_idx  = state->ui_tree_method_idx;
            mh_args->snap_horizon_count = 1;
            for (int i = 0; i < ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX; ++i) {
                mh_args->snap_horizons[i] = (i == 0) ? single_h : 0;
                mh_args->snap_tp_pct[i]   = single_tp;
                mh_args->snap_sl_pct[i]   = single_sl;
                // v5.13.5.A — populate new snap fields. Without this,
                // malloc'd MultiHorizonWorkerArgs leaves snap_label_kind_
                // per_horizon[] uninitialized → undefined label_type
                // passed to mh_run_one_horizon_fv. Single-horizon click
                // mirrors broadcast: label_type combo for h=0, 0 elsewhere.
                mh_args->snap_label_kind_per_horizon[i] =
                    (i == 0) ? state->label_type : 0;
            }
            // v5.13.5.A — single-horizon training_side. Operator's UI
            // toggle applies even in single-horizon mode (lets them
            // train one exit-side model without per-horizon CSV).
            mh_args->snap_training_side = state->ui_training_side;
            // v5.11.47 — same cfg-fallback for secret as Multi-Horizon path.
            {
                const char* secret_src = state->fv_auto_stamp_secret;
                if (secret_src[0] == '\0') {
                    secret_src = run_control->results.config_used.auto_stamp_secret;
                }
                size_t n = strnlen(secret_src, sizeof(state->fv_auto_stamp_secret));
                if (n >= sizeof(mh_args->snap_auto_stamp_secret))
                    n = sizeof(mh_args->snap_auto_stamp_secret) - 1;
                memcpy(mh_args->snap_auto_stamp_secret, secret_src, n);
                mh_args->snap_auto_stamp_secret[n] = '\0';
            }
            mh_args->snap_auto_stamp_enabled  =
                run_control->results.config_used.auto_stamp_on_held_out;
            mh_args->snap_n_splits            = state->wf_n_splits;
            mh_args->snap_buffer_ticks        = state->wf_buffer_ticks;
            mh_args->snap_min_train           = state->wf_min_train;
            mh_args->snap_gap_threshold       = state->fv_gap_threshold;
            mh_args->snap_held_out_fraction   = state->fv_held_out_fraction;
            pthread_create(&state->mh_tid, NULL, train_multi_horizon_worker_fn, mh_args);
            pthread_detach(state->mh_tid);
        }
        if (!can_train) {
            ImGui::EndDisabled();
#ifndef USE_XGBOOST
            ImGui::SameLine();
            ImGui::TextDisabled("Build with -DUSE_XGBOOST=ON");
#else
            if (results->sample_count < 10) {
                ImGui::SameLine();
                ImGui::TextDisabled("Collect features first (need 10+ samples)");
            }
#endif
        }
        } // end single_horizon_mode (Train Model)

        // v5.10.0a.G.1 — Train Multi-Horizon button. Adjacent to Train
        // Model so operators see both options. Gated on horizons being
        // configured (in-panel CSV input OR cfg.horizon_list fallback).
        // v5.10.0a-bugfix2: in-panel CSV editor — operator no longer
        // needs to edit cfg.horizon_list + reload to multi-horizon train.
        // v5.11.43 — only render in multi-horizon mode (single mode shows
        // Train Model, above).
        const auto& mh_cfg = run_control->results.config_used;

        // Parse the operator's CSV input on each render. Cheap (typically
        // 1-8 entries; bounded loop). Updates state->ui_horizon_list/_count
        // so the click handler reads from a stable snapshot.
        {
            int n = 0;
            const char* p = state->ui_horizon_csv;
            while (*p && n < 8) {
                while (*p == ' ' || *p == '\t' || *p == ',') p++;
                if (!*p) break;
                char* end = nullptr;
                long v = strtol(p, &end, 10);
                if (end == p) break;
                if (v > 0 && v <= 1000000)
                    state->ui_horizon_list[n++] = (int)v;
                p = end;
            }
            state->ui_horizon_count = n;
        }

        // Effective horizons: operator's UI input takes priority; if
        // empty (CSV doesn't parse to any valid horizon), fall back to
        // cfg.horizon_list (back-compat for operators who already wired
        // the cfg).
        int eff_horizon_count = state->ui_horizon_count > 0
                              ? state->ui_horizon_count
                              : mh_cfg.horizon_count;
        const int *eff_horizons = state->ui_horizon_count > 0
                                 ? state->ui_horizon_list
                                 : mh_cfg.horizon_list;

        // v5.11.43 — second Horizons CSV InputText DELETED. Single source of
        // truth lives at the top of the panel (rendered always, near
        // Collect Features / Collect Multi-Horizon). Operator types horizons
        // there; auto-routing renders the matching Train button here.

        // v5.11.40 — broadcast-or-match for TP/SL on the train side too.
        // Same validation as Collect Multi-Horizon (above). When eff_horizon
        // came from cfg.horizon_list fallback (operator didn't type a CSV),
        // ui_horizon_count is 0; in that case alignment uses
        // eff_horizon_count for the match.
        int train_tp_n = state->ui_tp_per_horizon_count;
        int train_sl_n = state->ui_sl_per_horizon_count;
        bool train_tp_aligned = (train_tp_n <= 1) || (train_tp_n == eff_horizon_count);
        bool train_sl_aligned = (train_sl_n <= 1) || (train_sl_n == eff_horizon_count);
        // v5.13.1.B — alignment check for per-horizon label_kind CSV.
        // Same broadcast-or-match rule as TP/SL CSV.
        int train_lk_n = state->ui_label_kind_per_horizon_count;
        bool train_lk_aligned = (train_lk_n <= 1) || (train_lk_n == eff_horizon_count);
        bool mh_can_train = can_train && (eff_horizon_count > 0)
                            && train_tp_aligned && train_sl_aligned
                            && train_lk_aligned;
        if (!single_horizon_mode) {
        if (!mh_can_train) ImGui::BeginDisabled();
        // v5.13.6.D — tooltip for the click target. Note: SetItemTooltip
        // attaches to the LAST item; ImGui::Button must be issued first
        // for the tooltip to bind to it. Render order matters here.
        bool mh_clicked = ImGui::Button("Train Multi-Horizon");
        ImGui::SetItemTooltip(
            "Train N models in one click — one per horizon in Horizons CSV.\n"
            "\n"
            "Per-horizon TP/SL via the CSV inputs above (broadcast-or-match\n"
            "rule: empty/single value broadcasts; N values map positional).\n"
            "\n"
            "v5.13.5 — per-horizon Label Kind via 'Label Kind CSV' input:\n"
            "  Empty: all horizons use the Label Type combo above\n"
            "  Single value: broadcasts to all horizons\n"
            "  N values: positional map to Horizons CSV\n"
            "  Misalignment disables this button (count != horizons count)\n"
            "Hover the 'Label Kind CSV' input for the integer→name lookup.\n"
            "Trains heterogeneous mixed-output ensembles in ONE click;\n"
            "v5.12.3.B+E mixed-output normalizer blends them at inference.\n"
            "\n"
            "Training Side combo at top of panel selects the ROLE FILE\n"
            "(co-located; E.1.2.C):\n"
            "  Buy:  models/<run_subdir>/<run>/horizon_<N>/<role>.json\n"
            "  Exit: models/<run_subdir>/<run>/horizon_<N>/exit.json\n"
            "No cfg step: the engine auto-discovers exit.json siblings\n"
            "under node_N_model_dir automatically (E.1.2.C).\n"
            "\n"
            "Each model gets full WF + held-out + auto-stamp. Per-horizon\n"
            "results table renders below.");
        if (mh_clicked) {
            state->mh_running = 1;
            state->mh_progress = 0;
            state->mh_total = eff_horizon_count;
            state->mh_current_horizon = 0;
            state->mh_cancel = 0;
            state->mh_complete = 0;
            state->status_msg[0] = '\0';

            MultiHorizonWorkerArgs *mh_args =
                (MultiHorizonWorkerArgs *)malloc(sizeof(MultiHorizonWorkerArgs));
            mh_args->state = state;
            mh_args->run_control = run_control;
            // Snapshot fields at click time (v5.10.0E pattern)
            {
                size_t n = strnlen(state->run_name, sizeof(state->run_name));
                if (n >= sizeof(mh_args->snap_run_name))
                    n = sizeof(mh_args->snap_run_name) - 1;
                memcpy(mh_args->snap_run_name, state->run_name, n);
                mh_args->snap_run_name[n] = '\0';
            }
            {
                size_t n = strnlen(state->model_path, sizeof(state->model_path));
                if (n >= sizeof(mh_args->snap_model_path))
                    n = sizeof(mh_args->snap_model_path) - 1;
                memcpy(mh_args->snap_model_path, state->model_path, n);
                mh_args->snap_model_path[n] = '\0';
            }
            mh_args->snap_label_type     = state->label_type;
            mh_args->snap_max_depth      = state->max_depth;
            mh_args->snap_learning_rate  = state->learning_rate;
            mh_args->snap_n_estimators   = state->n_estimators;
            mh_args->snap_subsample        = state->ui_subsample;
            mh_args->snap_colsample_bytree = state->ui_colsample_bytree;
            mh_args->snap_min_child_weight = state->ui_min_child_weight;
            mh_args->snap_seed             = state->ui_seed;
            mh_args->snap_tree_method_idx  = state->ui_tree_method_idx;
            // v5.10.0a-bugfix2 — snapshot effective horizons (UI takes
            // priority over cfg fallback at click time).
            mh_args->snap_horizon_count = eff_horizon_count;
            // v5.11.40 — snap per-horizon TP/SL (broadcast-or-match
            // rule). Single-value broadcasts; N values map positional.
            // Empty CSV falls back to label_tp_pct/_sl_pct float.
            float bcast_tp = (state->ui_tp_per_horizon_count > 0)
                ? state->ui_tp_per_horizon[0] : state->label_tp_pct;
            float bcast_sl = (state->ui_sl_per_horizon_count > 0)
                ? state->ui_sl_per_horizon[0] : state->label_sl_pct;
            // v5.13.1.B — broadcast-or-match for per-horizon label_kind.
            // Empty CSV / single value → broadcast state->label_type
            // (which is the existing UI Label Type combo). N values map
            // positional. Mirrors TP/SL CSV pattern.
            int bcast_lk = (state->ui_label_kind_per_horizon_count > 0)
                ? state->ui_label_kind_per_horizon[0] : state->label_type;
            for (int i = 0; i < ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX; ++i) {
                mh_args->snap_horizons[i] = (i < eff_horizon_count)
                    ? eff_horizons[i] : 0;
                mh_args->snap_tp_pct[i] = (state->ui_tp_per_horizon_count > 1
                                           && i < state->ui_tp_per_horizon_count)
                    ? state->ui_tp_per_horizon[i] : bcast_tp;
                mh_args->snap_sl_pct[i] = (state->ui_sl_per_horizon_count > 1
                                           && i < state->ui_sl_per_horizon_count)
                    ? state->ui_sl_per_horizon[i] : bcast_sl;
                mh_args->snap_label_kind_per_horizon[i] =
                    (state->ui_label_kind_per_horizon_count > 1
                     && i < state->ui_label_kind_per_horizon_count)
                        ? state->ui_label_kind_per_horizon[i] : bcast_lk;
            }
            // E.1.2.C GUI polish (a) — click-time horizon snapshot for the
            // per-horizon results table (arrays sized PANEL_HORIZON_MAX =
            // HORIZON_LIST_MAX since E.1.2.D leaf 13, so they track the grid).
            for (int i = 0; i < TrainingPanelState::PANEL_HORIZON_MAX; ++i)
                state->mh_horizon_ticks[i] =
                    (i < eff_horizon_count) ? eff_horizons[i] : 0;
            // v5.13.1.A — snapshot side at click time (race-free).
            mh_args->snap_training_side = state->ui_training_side;
            // v5.11.41 — snap FV/auto-stamp params at click time. Closes
            // the gap where Train Multi-Horizon trained but didn't run WF
            // / held-out / stamp. Now mirrors single-horizon RFV behavior.
            // v5.11.47 — secret falls back to cfg.auto_stamp_secret when
            // GUI text input (state->fv_auto_stamp_secret) is empty. Lets
            // operator set secret once in cfg without re-typing per session.
            // per horizon (sequential; v5.11.41.C adds parallelism).
            // v5.11.47 — if GUI text input is empty, fall back to cfg's
            // auto_stamp_secret (lets operator set it once in cfg).
            {
                const char* secret_src = state->fv_auto_stamp_secret;
                if (secret_src[0] == '\0') {
                    secret_src = run_control->results.config_used.auto_stamp_secret;
                }
                size_t n = strnlen(secret_src, sizeof(state->fv_auto_stamp_secret));
                if (n >= sizeof(mh_args->snap_auto_stamp_secret))
                    n = sizeof(mh_args->snap_auto_stamp_secret) - 1;
                memcpy(mh_args->snap_auto_stamp_secret, secret_src, n);
                mh_args->snap_auto_stamp_secret[n] = '\0';
            }
            // v5.11.47 — kept for back-compat in args struct; worker no
            // longer gates on this (always stamps).
            mh_args->snap_auto_stamp_enabled  =
                run_control->results.config_used.auto_stamp_on_held_out;
            mh_args->snap_n_splits            = state->wf_n_splits;
            mh_args->snap_buffer_ticks        = state->wf_buffer_ticks;
            mh_args->snap_min_train           = state->wf_min_train;
            mh_args->snap_gap_threshold       = state->fv_gap_threshold;
            mh_args->snap_held_out_fraction   = state->fv_held_out_fraction;
            pthread_create(&state->mh_tid, NULL, train_multi_horizon_worker_fn, mh_args);
            pthread_detach(state->mh_tid);
        }
        if (!mh_can_train) {
            ImGui::EndDisabled();
            if (eff_horizon_count == 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("(set Horizons CSV above OR cfg.horizon_list to enable)");
            } else if (!train_tp_aligned || !train_sl_aligned) {
                // v5.11.40 — same misalignment hint as Collect side
                ImGui::SameLine();
                ImGui::TextColored(FoxmlColors::yellow,
                    "(misaligned: TP=%d, SL=%d, horizons=%d — need 1 or %d each)",
                    train_tp_n, train_sl_n, eff_horizon_count, eff_horizon_count);
            }
        }
        ImGui::SetItemTooltip(
            "Trains N XGBoost models, one per horizon in 'Horizons (CSV)' above.\n"
            "Each horizon recomputes labels with that label_forward_ticks value,\n"
            "trains a separate model, saves to:\n"
            "  models/<class>/<run_name>_horizon_<H>/<role>.json\n\n"
            "Operator manually picks which horizon to deploy (or relies on\n"
            "v5.10.0a.G.4 ensemble inference once engine wiring lands). Past\n"
            "Runs panel treats each horizon as a separate row for\n"
            "Compare-to-Baseline.");

        // Multi-horizon progress bar (rendered when worker is running)
        if (state->mh_running) {
            float pct = state->mh_total > 0
                ? (float)state->mh_progress / state->mh_total : 0.0f;
            char overlay[96];
            snprintf(overlay, sizeof(overlay), "horizon %d/%d (current: %d ticks)",
                     (int)state->mh_progress, (int)state->mh_total,
                     (int)state->mh_current_horizon);
            ImGui::ProgressBar(pct, ImVec2(-1, 0), overlay);
            if (ImGui::Button("Cancel Multi-Horizon"))
                state->mh_cancel = 1;
        }

        // v5.11.41 — per-horizon results table. Renders during run AND
        // post-completion so operator can review metrics without scrolling
        // through stderr. Each row = one horizon's WF + held-out + stamp
        // status. Columns:
        //   Horizon  | Progress  | Status   | Metrics
        // (status string is built by the worker's per-horizon block).
        // Empty when no Multi-Horizon run has fired yet.
        if (state->mh_total > 0) {
            ImGui::Separator();
            ImGui::TextDisabled("Per-horizon results");
            if (ImGui::BeginTable("mh_horizons", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Horizon");
                ImGui::TableSetupColumn("WF %");
                ImGui::TableSetupColumn("State");
                ImGui::TableSetupColumn("Metrics");
                ImGui::TableHeadersRow();

                int n_show = state->mh_total < 8 ? state->mh_total : 8;
                for (int h = 0; h < n_show; ++h) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    // E.1.2.C GUI polish (a) — the click-time snapshot, never
                    // the live-reparsed ui_horizon_list (editing the CSV
                    // mid/post-run relabeled these rows).
                    ImGui::Text("%d", state->mh_horizon_ticks[h]);

                    ImGui::TableNextColumn();
                    int prog = state->mh_horizon_progress[h];
                    if (prog > 0) ImGui::Text("%d%%", prog);
                    else          ImGui::TextDisabled("--");

                    ImGui::TableNextColumn();
                    if (state->mh_horizon_complete[h]) {
                        ImGui::TextColored(FoxmlColors::green, "DONE");
                    } else if (state->mh_running && h == (state->mh_progress - 1)) {
                        ImGui::TextColored(FoxmlColors::yellow, "running");
                    } else {
                        ImGui::TextDisabled("waiting");
                    }

                    ImGui::TableNextColumn();
                    if (state->mh_horizon_status[h][0] != '\0') {
                        ImGui::TextWrapped("%s", state->mh_horizon_status[h]);
                    } else {
                        ImGui::TextDisabled("--");
                    }
                }
                ImGui::EndTable();
            }
        }
        } // end !single_horizon_mode (Train Multi-Horizon block)
    }

    // training results — kind-appropriate display.
    // D-d (2026-08-22, operator-decided) — the ~300-line results+Save-Run block
    // that rendered here was gated on `model_trained`, whose only true-writers
    // lived in the deleted train_model_worker_fn: permanently-invisible UI at
    // HEAD (S1-F8, the Class-44 shape). The MH results table above is the live
    // results view; the one living signal (the completion status line) now
    // renders whenever it has content:
    if (state->status_msg[0]) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.55f, 0.76f, 0.51f, 1.0f), "%s", state->status_msg);
    }

    //==================================================================
    // WALK-FORWARD VALIDATION (Phase 6A — the REAL performance metric)
    //==================================================================
    ImGui::Separator();
    ImGui::Text("Walk-Forward Validation");
    ImGui::SetItemTooltip("Tests if the model generalizes to unseen data\n"
                          "splits data chronologically into train/test folds\n"
                          "trains a fresh model per fold and measures accuracy on the test portion\n\n"
                          "this is the REAL performance metric — train accuracy means nothing\n"
                          "val > 55%% with low gap = real signal, val ~50%% = coin flip");

    // parameters
    ImGui::InputInt("Folds", &state->wf_n_splits, 1, 2);
    if (state->wf_n_splits < 2) state->wf_n_splits = 2;
    if (state->wf_n_splits > 20) state->wf_n_splits = 20;
    ImGui::SetItemTooltip("Number of temporal train/test splits\n"
                          "each fold trains on earlier data, tests on later data\n"
                          "more folds = more reliable estimate but slower\n"
                          "5 is standard, 3 for fast iteration");
    ImGui::InputInt("Horizon Ticks", &state->wf_horizon_ticks, 100, 500);
    if (state->wf_horizon_ticks < 0) state->wf_horizon_ticks = 0;
    // s5 leaf-16 — show the operator what AUTO actually resolved to, so the
    // derived value is visible rather than implied.
    if (state->wf_horizon_ticks == 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(auto = %d)", Training_ResolvePurgeHorizon(state));
    }
    ImGui::SetItemTooltip("Label forward window in ticks — drives the purge gap\n"
                          "between train and test folds (prevents labels that look\n"
                          "into the future from leaking across the boundary).\n\n"
                          "0 = AUTO: derives max(Horizons CSV) — the longest label's\n"
                          "reach, which is the value the purge gap actually needs.\n"
                          "Nonzero = explicit override, honored verbatim.\n\n"
                          "Why auto is the default: this field used to sit at 1000 with\n"
                          "nothing linking it to the horizons you collected, so a 67,500-\n"
                          "tick grid purged ~1.5k ticks and silently inflated every\n"
                          "validation number.");
    ImGui::InputInt("Purge Buffer", &state->wf_buffer_ticks, 64, 256);
    ImGui::SetItemTooltip("Extra safety margin added to the purge gap\n"
                          "accounts for feature lookback windows (rolling stats etc.)\n"
                          "default 512 — increase if features use long lookback periods");
    ImGui::InputInt("Min Train", &state->wf_min_train, 100, 500);
    ImGui::SetItemTooltip("Minimum training samples required per fold\n"
                          "folds with fewer are skipped (usually fold 1)\n"
                          "higher = more reliable per-fold training but may skip more folds");

    // run / cancel button
    {
        // v5.10.0a-bugfix1 — re-evaluate any_worker_running for WF gate
        // (state may have flipped since the can_train computation above
        // — e.g. if the operator clicked Train Model then renders fired
        // before tm_running flipped). Recompute here for safety.
        bool any_worker_running_wf =
            state->tm_running ||
            state->fv_running ||
            state->hp_running ||
            state->mh_running;  // intentionally exclude wf_running so WF can show its own cancel button
        bool can_wf = results->sample_count >= 50 && !any_worker_running_wf
                      && !run_control->running;  // E.1.2.D NEW-5 — no train-during-collect
#ifndef USE_XGBOOST
        can_wf = false;
#endif
        if (state->wf_running) {
            ImGui::ProgressBar(state->wf_progress / 100.0f, ImVec2(-1, 0), "Walk-forward...");
            if (ImGui::Button("Cancel Walk-Forward"))
                state->wf_cancel = 1;
        } else {
            if (!can_wf) ImGui::BeginDisabled();
            if (ImGui::Button("Run Walk-Forward")) {
                state->wf_running = 1;
                state->wf_progress = 0;
                state->wf_cancel = 0;
                state->wf_complete = 0;
                state->wf_has_results = false;

                WalkForwardWorkerArgs *wf_args = (WalkForwardWorkerArgs *)malloc(sizeof(WalkForwardWorkerArgs));
                wf_args->state = state;
                wf_args->data = results;
                // E.1.2.C follow-up — snapshot at click, on the GUI thread, which is
                // the only moment the panel is coherent. label_type comes from
                // run_config (the field that produced results->labels[]), same
                // resolver-SSoT choice as the Run-Full-Validation path.
                wf_args->snap_wf_n_splits      = state->wf_n_splits;
                wf_args->snap_wf_horizon_ticks = Training_ResolvePurgeHorizon(state);   // s5 leaf-16
                wf_args->snap_wf_buffer_ticks  = state->wf_buffer_ticks;
                wf_args->snap_wf_min_train     = state->wf_min_train;
                wf_args->snap_label_type       = run_control->run_config.label_type;
                wf_args->snap_hp = Training_SnapshotHyperparams(state);
                pthread_create(&state->wf_tid, NULL, walkforward_worker_fn, wf_args);
                pthread_detach(state->wf_tid);
            }
            if (!can_wf) {
                ImGui::EndDisabled();
#ifndef USE_XGBOOST
                ImGui::SameLine();
                ImGui::TextDisabled("Build with -DUSE_XGBOOST=ON");
#else
                if (results->sample_count < 50) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Need 50+ samples");
                }
#endif
            }
        }
    }

    // walk-forward results display — kind-aware (label-type-aware metric invariant)
    if (state->wf_has_results) {
        WalkForwardResults *wf = &state->wf_results;
        bool wf_is_regression = (wf->label_kind == 1);

        // aggregate metrics — the metrics that actually matter
        ImGui::Separator();
        {
            ImVec4 val_color = (wf->overfit_count > 0)
                ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f)   // red: overfit detected
                : ImVec4(0.55f, 0.76f, 0.51f, 1.0f);   // green: clean

            if (wf_is_regression) {
                // load-bearing metric for regression: mean Pearson r across folds.
                // MSE is shown alongside but doesn't tell you if the model has signal —
                // a model predicting always-zero gets low MSE on small targets.
                ImGui::TextColored(val_color, "Val Pearson r: %.4f",
                                   wf->mean_val_correlation);
                ImGui::SetItemTooltip("Mean Pearson correlation between predictions and labels\n"
                                      "across all walk-forward folds. THIS is the metric that\n"
                                      "tells you if the model has signal:\n\n"
                                      "  |r| < 0.05 → no signal (predictions uncorrelated with truth)\n"
                                      "  |r| 0.05-0.2 → weak but real signal\n"
                                      "  |r| > 0.2 → strong signal at tick scale\n"
                                      "  |r| > 0.99 → memorization (flagged as overfit)\n\n"
                                      "Negative r means the model is anti-predictive — fitting\n"
                                      "noise that happens to be inverted. Still a memorization risk.");
                ImGui::SameLine();
                ImGui::TextDisabled("(train: %.4f, MSE: %.6f)",
                                    wf->mean_train_correlation, wf->mean_val_mse);
                ImGui::SetItemTooltip("train: in-sample correlation\n"
                                      "MSE: mean squared error on validation\n"
                                      "MSE alone is not a signal indicator — read it alongside r.");
            } else {
                ImGui::TextColored(val_color, "Val Accuracy: %.1f%% +/- %.1f%%",
                                   wf->mean_val_accuracy * 100.0f, wf->std_val_accuracy * 100.0f);
                ImGui::SetItemTooltip("Mean accuracy on unseen test data across all folds\n"
                                      "+/- shows consistency (lower = more stable)\n\n"
                                      "> 55%%: model has real predictive signal\n"
                                      "~ 50%%: no better than random (coin flip)\n"
                                      "< 50%%: model is anti-predictive (inverted signal)");
                ImGui::SameLine();
                ImGui::TextDisabled("(train: %.1f%%)", wf->mean_train_accuracy * 100.0f);
                ImGui::SetItemTooltip("Training accuracy — how well the model fits the data it trained on\n"
                                      "high train + low val = overfitting (memorizing noise)\n"
                                      "the gap between train and val is what matters");
            }
        }

        // overfit warning — same structure for both kinds, reason text is kind-specific
        if (wf->overfit_count > 0) {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f),
                "WARNING: %d/%d folds flagged as overfit", wf->overfit_count, wf->valid_folds);
            ImGui::SetItemTooltip("Folds where the model memorized training data\n"
                                  "Classification: flagged when train accuracy >= 99%% or\n"
                                  "  train-val gap >= 20%%\n"
                                  "Regression: flagged when |train_corr| >= 0.99 or\n"
                                  "  train_corr - val_corr >= 0.20\n\n"
                                  "try: fewer estimators, lower max depth, more data,\n"
                                  "different label parameters (barriers, lookahead)");
        }

        // Walk-Forward diagnosis — interpret the result for the user.
        // This is the load-bearing line: it tells you what just happened
        // in plain language so you don't have to mentally translate metrics.
        {
            const ImVec4 wf_green  = ImVec4(0.55f, 0.76f, 0.51f, 1.0f);
            const ImVec4 wf_yellow = ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
            const ImVec4 wf_red    = ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
            const ImVec4 *wd_col = &wf_yellow;
            const char *wd_text = "result unclear";
            const char *wd_tip  = "Couldn't classify the result. Check per-fold table for details.";

            if (wf_is_regression) {
                float val_r = wf->mean_val_correlation;
                float train_r = wf->mean_train_correlation;
                float abs_val_r = val_r < 0 ? -val_r : val_r;
                float abs_train_r = train_r < 0 ? -train_r : train_r;
                if (wf->overfit_count > 0 && abs_val_r < 0.05f) {
                    wd_col = &wf_red;
                    wd_text = "memorization without generalization — model learned training noise";
                    wd_tip  = "Train correlation is high (the overfit detector flagged it),\n"
                              "but val correlation is ~0. Model memorized training samples\n"
                              "without learning anything that transfers. Reduce capacity:\n"
                              "lower max_depth (try 2-3), fewer estimators, or more data.";
                } else if (abs_val_r < 0.05f && abs_train_r < 0.05f) {
                    wd_col = &wf_red;
                    wd_text = "no signal — features don't predict returns at this horizon";
                    wd_tip  = "Both train and val correlations are near zero. The model\n"
                              "couldn't learn anything from the features even on training data.\n"
                              "This means: features genuinely lack predictive content for this\n"
                              "label, OR the labels are degenerate (check sample-panel\n"
                              "Diagnosis for label sanity), OR the horizon is wrong (try\n"
                              "shorter or longer Forward Ticks).";
                } else if (abs_val_r >= 0.20f) {
                    wd_col = &wf_green;
                    wd_text = "STRONG signal — verify no leakage before trusting";
                    wd_tip  = "Mean val Pearson r >= 0.20 is unusually strong for tick-scale\n"
                              "BTC prediction. Before celebrating: check that purge gap is\n"
                              "respected (no train/test temporal overlap), that labels don't\n"
                              "leak future info into features, and that the fingerprint matches\n"
                              "expected. If it survives those checks, this is real edge.";
                } else if (abs_val_r >= 0.05f) {
                    wd_col = &wf_green;
                    wd_text = "weak but real signal — worth optimizing";
                    wd_tip  = "Mean val Pearson r in [0.05, 0.20]. Real edge but small.\n"
                              "Hyperparameter sweep can extract more (longer training,\n"
                              "different max_depth, different label horizons). Compare\n"
                              "across folds for stability — high variance = brittle.";
                } else {
                    wd_col = &wf_yellow;
                    wd_text = "marginal — barely above noise";
                    wd_tip  = "abs(val r) is between 0.0 and 0.05. Could be real weak signal\n"
                              "or just noise. Run again with different folds or longer training\n"
                              "to see if it's stable.";
                }
            } else {
                // binary or multiclass — both report accuracy. v5.9.4a:
                // baseline-aware bands. Pre-v5.9.4a hardcoded 0.52/0.55
                // (binary 50%+spread); for K-class with imbalanced classes
                // baseline can be much higher (e.g. 47% for the
                // PEAK_VALLEY_STABLE 5.9/47.4/46.7 distribution from
                // 2026-05-02 paper test). Diagnosis was misleading for
                // multiclass; now uses actual majority-class baseline.
                float val_acc = wf->mean_val_accuracy;
                float train_acc = wf->mean_train_accuracy;
                int K = (wf->num_classes >= 2) ? wf->num_classes : 2;
                float baseline = multiclass_baseline_accuracy(
                    K, snap->class_counts, snap->sample_count);
                // "fee-overhead" threshold: 3 percentage points above
                // baseline. Empirical — covers ~0.1% × 2 sides for
                // typical small-to-mid TP barriers.
                const float fee_band = 0.03f;
                const float marginal_band = 0.01f;  // within 1pp of baseline = marginal
                static char tip_buf[1024];

                if (wf->overfit_count >= wf->valid_folds && wf->valid_folds > 0) {
                    wd_col = &wf_red;
                    wd_text = "every fold flagged as memorization — model trivially fits training";
                    wd_tip  = "All valid folds were flagged. Most common cause: extreme class\n"
                              "imbalance where 'predict majority' gets 99%+ training accuracy\n"
                              "but val accuracy converges to the prior rate — looks high\n"
                              "but means nothing. Check sample panel ratio. Or reduce\n"
                              "model capacity (max_depth 2-3).";
                } else if (val_acc <= baseline + marginal_band) {
                    wd_col = &wf_red;
                    wd_text = "no edge — val accuracy at or below the always-predict-best baseline";
                    snprintf(tip_buf, sizeof(tip_buf),
                             "Mean val accuracy %.1f%% <= baseline %.1f%% (for %d-class with "
                             "current distribution). Predicting the majority class would do "
                             "equally well. Features aren't separating the classes at this "
                             "label/horizon.\n\n"
                             "Try: different label (Forward P&L regression sidesteps class-\n"
                             "balance issues), tighter/wider barriers, different lookahead.",
                             val_acc * 100.0f, baseline * 100.0f, K);
                    wd_tip = tip_buf;
                } else if (val_acc >= baseline + fee_band) {
                    wd_col = &wf_green;
                    wd_text = "real edge — val accuracy above baseline + fee overhead";
                    snprintf(tip_buf, sizeof(tip_buf),
                             "Mean val accuracy %.1f%% > baseline %.1f%% + 3%% fee buffer "
                             "(for %d-class). After fees of ~0.1%% × 2 sides, this regime can "
                             "plausibly produce positive expectancy. Verify fold-to-fold "
                             "stability (low std), no leakage, and that training distribution "
                             "matches deployment regime.",
                             val_acc * 100.0f, baseline * 100.0f, K);
                    wd_tip = tip_buf;
                } else {
                    wd_col = &wf_yellow;
                    wd_text = "marginal — val accuracy above baseline but inside fee buffer";
                    snprintf(tip_buf, sizeof(tip_buf),
                             "Mean val accuracy %.1f%% is in [%.1f%%, %.1f%%] (baseline + 1-3%% "
                             "for %d-class). Real signal but might not overcome fees + slippage "
                             "in live trading. Worth optimizing if you can also reduce trading "
                             "costs (maker rebates, longer holding period to amortize fees).",
                             val_acc * 100.0f,
                             (baseline + marginal_band) * 100.0f,
                             (baseline + fee_band) * 100.0f, K);
                    wd_tip = tip_buf;
                }
                (void)train_acc; // available for future train/val gap diagnostics
            }
            ImGui::TextColored(*wd_col, "Diagnosis: %s", wd_text);
            ImGui::SetItemTooltip("%s", wd_tip);
        }

        // fingerprint
        if (wf->fingerprint[0] != '\0') {
            char short_fp[13];
            Fingerprint_Short(wf->fingerprint, short_fp, 12);
            ImGui::TextDisabled("Fingerprint: %s  (%.0f ms)", short_fp, wf->elapsed_ms);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Full: %s\nReproducible: same config + data = same hash", wf->fingerprint);
        }

        // per-fold table — column headers + values depend on label kind
        if (wf->valid_folds > 0 && ImGui::TreeNode("Per-Fold Results")) {
            ImGui::BeginTable("folds", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg);
            ImGui::TableSetupColumn("Fold", ImGuiTableColumnFlags_WidthFixed, 40);
            if (wf_is_regression) {
                ImGui::TableSetupColumn("Train r",  ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Val r",    ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Val MSE",  ImGuiTableColumnFlags_WidthFixed, 90);
            } else {
                ImGui::TableSetupColumn("Train",    ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Val",      ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Gap",      ImGuiTableColumnFlags_WidthFixed, 60);
            }
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            if (wf_is_regression) {
                ImGui::SetItemTooltip("Train r: in-sample Pearson correlation\n"
                                      "Val r: out-of-sample correlation (the real test)\n"
                                      "Val MSE: mean squared error on validation\n"
                                      "Status: overfit detection (corr-based for regression)");
            } else {
                ImGui::SetItemTooltip("Train: accuracy on data the model saw during training\n"
                                      "Val: accuracy on future data it never saw (the real test)\n"
                                      "Gap: train - val (lower is better, >20%% = overfitting)\n"
                                      "Status: overfit detection (memorization, high gap, etc.)");
            }

            for (int i = 0; i < wf->num_folds; i++) {
                if (!wf->folds[i].valid) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", i + 1);

                if (wf_is_regression) {
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.4f", wf->folds[i].train_correlation);
                    ImGui::TableSetColumnIndex(2);
                    // color val_r: green if signal-like, yellow weak, red near zero or memorized
                    float vr = wf->folds[i].val_correlation;
                    float abs_vr = vr < 0 ? -vr : vr;
                    ImVec4 vr_color = (abs_vr > 0.20f) ? ImVec4(0.55f, 0.76f, 0.51f, 1.0f)
                                    : (abs_vr > 0.05f) ? ImVec4(0.95f, 0.75f, 0.30f, 1.0f)
                                                       : ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
                    ImGui::TextColored(vr_color, "%.4f", vr);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.6f", wf->folds[i].val_mse);
                } else {
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.1f%%", wf->folds[i].train_accuracy * 100.0f);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.1f%%", wf->folds[i].val_accuracy * 100.0f);
                    ImGui::TableSetColumnIndex(3);
                    float gap = wf->folds[i].train_accuracy - wf->folds[i].val_accuracy;
                    ImVec4 gap_color = (gap > 0.20f) ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f)
                                     : (gap > 0.10f) ? ImVec4(0.95f, 0.75f, 0.30f, 1.0f)
                                                      : ImVec4(0.55f, 0.76f, 0.51f, 1.0f);
                    ImGui::TextColored(gap_color, "%.1f%%", gap * 100.0f);
                }

                ImGui::TableSetColumnIndex(4);
                if (wf->folds[i].overfit.is_overfit) {
                    ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s",
                                       wf->folds[i].overfit.reason);
                } else {
                    ImGui::TextColored(ImVec4(0.55f, 0.76f, 0.51f, 1.0f), "clean");
                }
            }
            ImGui::EndTable();
            ImGui::TreePop();
        }
    }

    //==================================================================
    // FULL VALIDATION (v5.8.7) — held-out + auto-stamp
    //==================================================================
    // The "shippable model" gate: train on [0, trainval_end) with the same
    // hyperparameters as a WF fold, evaluate on the locked held-out portion,
    // and (if gap_threshold met) auto-write a signed stamp alongside the
    // model file. The stamp embeds:
    //   - feature_registry_hash (FEATURE_REGISTRY_HASH() — current build)
    //   - engine_version       (ENGINE_VERSION_STRING — current build)
    //   - model_format_version (MODEL_FORMAT_VERSION — wire format)
    // so the live engine's NodeModelZoo_TryLoadRole can refuse to load a
    // model trained against a different feature set or engine version.
    //
    // This button is the ONLY UI path that exercises Backtest_RunFullValidation
    // (and therefore the v5.8.6 auto-stamp wiring). Pre-v5.8.7 the function
    // existed but was unreachable from the suite.
    ImGui::Separator();
    // ============================================================
    // v5.10.0a.E — Hyperparam Sweep block. Placed between WF and FV
    // since it logically follows WF (operator runs WF on best hyperparams
    // they found via sweep). Disabled when no features collected yet.
    // ============================================================
    ImGui::Separator();
    ImGui::Text("Hyperparam Sweep (XGBoost grid search over training)");
    ImGui::SetItemTooltip(
        "Trains N XGBoost models with different hyperparam values,\n"
        "runs walk-forward on each, picks the best by val accuracy.\n\n"
        "Workflow:\n"
        "  1. Click Collect Features (above) to populate the dataset\n"
        "  2. Pick 1-2 cfg fields + ranges below\n"
        "  3. Click Run Hyperparam Sweep — trains all cells\n"
        "  4. Inspect results table; best cell highlighted\n\n"
        "Sweepable fields (ConfigField_Set whitelist):\n"
        "  xgb_subsample / xgb_colsample_bytree / xgb_min_child_weight\n"
        "  / xgb_seed / xgb_train_nthread / xgb_eval_nthread");
    {
        ImGui::PushItemWidth(-180);
        ImGui::SliderInt("Sweep params (1 or 2)", &state->hp_num_params, 1, OPT_MAX_PARAMS);
        for (int p = 0; p < state->hp_num_params; ++p) {
            ImGui::PushID(p);
            char hdr[32]; snprintf(hdr, sizeof(hdr), "Sweep Param %d", p + 1);
            if (ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::InputText("Key", state->hp_ranges[p].key, 32);
                ImGui::InputDouble("Min", &state->hp_ranges[p].lo, 0.05, 0.5, "%.3f");
                ImGui::InputDouble("Max", &state->hp_ranges[p].hi, 0.05, 0.5, "%.3f");
                ImGui::InputDouble("Step", &state->hp_ranges[p].step, 0.05, 0.25, "%.3f");
                int steps = state->hp_ranges[p].steps();
                ImGui::Text("%d steps", steps);
            }
            ImGui::PopID();
        }
        int hp_total_cells = state->hp_ranges[0].steps()
                            * (state->hp_num_params > 1 ? state->hp_ranges[1].steps() : 1);
        ImGui::Text("Total cells: %d", hp_total_cells);
        ImGui::PopItemWidth();

        const BacktestResults *hp_data = &run_control->results;
        // v5.10.0a-bugfix1 — exclude other workers (recompute fresh)
        bool any_worker_running_hp =
            state->tm_running ||
            state->wf_running ||
            state->fv_running ||
            state->mh_running;
        bool can_hp =
#ifdef USE_XGBOOST
            hp_data->sample_count >= 100 && hp_total_cells > 0
            && hp_total_cells <= OPT_MAX_GRID && !any_worker_running_hp
            && !run_control->running;  // E.1.2.D NEW-5 — no train-during-collect
#else
            false;
#endif

        if (state->hp_running) {
            float pct = state->hp_total > 0
                ? (float)state->hp_progress / state->hp_total : 0.0f;
            char overlay[64];
            snprintf(overlay, sizeof(overlay), "%d / %d cells",
                     (int)state->hp_progress, (int)state->hp_total);
            ImGui::ProgressBar(pct, ImVec2(-1, 0), overlay);
            if (ImGui::Button("Cancel Hyperparam Sweep"))
                state->hp_cancel = 1;
        } else {
            if (!can_hp) ImGui::BeginDisabled();
            if (ImGui::Button("Run Hyperparam Sweep")) {
                state->hp_running = 1;
                state->hp_progress = 0;
                state->hp_total = 0;
                state->hp_cancel = 0;
                state->hp_complete = 0;
                state->hp_has_results = false;
                memset(&state->hp_results, 0, sizeof(state->hp_results));

                HyperparamSweepWorkerArgs *hp_args =
                    (HyperparamSweepWorkerArgs *)malloc(sizeof(HyperparamSweepWorkerArgs));
                hp_args->state = state;
                hp_args->data = hp_data;
                memcpy(hp_args->snap_ranges, state->hp_ranges, sizeof(hp_args->snap_ranges));
                hp_args->snap_num_params = state->hp_num_params;
                // E.1.2.D (scan-1 NEW-4) — the labels' own producer, not the
                // combo (leaf 7's rule at the third sibling): the sweep must
                // rank cells against the kind the samples were actually
                // labeled with, or a post-collect combo flip silently trains
                // every cell on mismatched label semantics.
                hp_args->snap_label_type = run_control->run_config.label_type;
                hp_args->snap_wf_n_splits = state->wf_n_splits;
                hp_args->snap_wf_horizon_ticks = Training_ResolvePurgeHorizon(state);   // s5 leaf-16
                hp_args->snap_wf_buffer_ticks = state->wf_buffer_ticks;
                hp_args->snap_wf_min_train = state->wf_min_train;
                pthread_create(&state->hp_tid, NULL, hp_sweep_worker_fn, hp_args);
                pthread_detach(state->hp_tid);
            }
            if (!can_hp) {
                ImGui::EndDisabled();
#ifndef USE_XGBOOST
                ImGui::SameLine();
                ImGui::TextDisabled("Build with -DUSE_XGBOOST=ON");
#else
                if (hp_data->sample_count < 100) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Need 100+ samples (Collect Features first)");
                } else if (hp_total_cells == 0 || hp_total_cells > OPT_MAX_GRID) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Cell count out of range (1..%d)", OPT_MAX_GRID);
                }
#endif
            }
        }

        // Results table — kind-aware (WF metric: accuracy or correlation)
        if (state->hp_has_results && state->hp_results.total_runs > 0) {
            const OptimizerResults *opt = &state->hp_results;
            ImGui::Separator();
            ImGui::Text("Best cell: %s=%.3f",
                        state->hp_ranges[0].key,
                        opt->param_vals[0][opt->best_idx / opt->dims[1]]);
            if (opt->num_params > 1) {
                ImGui::SameLine();
                ImGui::Text(" %s=%.3f", state->hp_ranges[1].key,
                            opt->param_vals[1][opt->best_idx % opt->dims[1]]);
            }
            ImGui::Text("Metric (val accuracy or correlation): %.4f",
                        opt->metric[opt->best_idx]);

            ImGuiTableFlags flags = ImGuiTableFlags_Borders |
                                     ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingFixedFit;
            if (ImGui::BeginTable("hp_sweep_results",
                                   opt->num_params == 1 ? 3 : 4, flags)) {
                ImGui::TableSetupColumn("Cell", ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn(state->hp_ranges[0].key,
                                         ImGuiTableColumnFlags_WidthFixed, 120);
                if (opt->num_params > 1) {
                    ImGui::TableSetupColumn(state->hp_ranges[1].key,
                                             ImGuiTableColumnFlags_WidthFixed, 120);
                }
                ImGui::TableSetupColumn("Metric",
                                         ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableHeadersRow();

                for (int idx = 0; idx < opt->total_runs; ++idx) {
                    int i0 = idx / opt->dims[1];
                    int i1 = idx % opt->dims[1];
                    bool is_best = (idx == opt->best_idx);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (is_best) {
                        ImGui::TextColored(ImVec4(0.55f, 0.76f, 0.51f, 1.0f),
                                           "%d ★", idx);
                    } else {
                        ImGui::Text("%d", idx);
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.3f", opt->param_vals[0][i0]);
                    if (opt->num_params > 1) {
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%.3f", opt->param_vals[1][i1]);
                        ImGui::TableSetColumnIndex(3);
                    } else {
                        ImGui::TableSetColumnIndex(2);
                    }
                    ImGui::Text("%.4f", opt->metric[idx]);
                }
                ImGui::EndTable();
            }
        }
    }

    ImGui::Separator();
    ImGui::Text("Full Validation (held-out + auto-stamp)");
    ImGui::SetItemTooltip("Trains on [0, trainval_end), evaluates on locked\n"
                          "held-out tail, and (if held_out gap < threshold)\n"
                          "auto-writes a signed stamp alongside the model.\n\n"
                          "Stamp embeds engine_version + feature_registry_hash\n"
                          "so the live engine refuses to load a model trained\n"
                          "against a drifted build.");
    {
        ImGui::PushItemWidth(-180);
        ImGui::SliderFloat("Held-out fraction",
                           &state->fv_held_out_fraction, 0.05f, 0.30f, "%.2f");
        ImGui::SliderFloat("Gap threshold",
                           &state->fv_gap_threshold, 0.01f, 0.20f, "%.2f");
        ImGui::InputText("HMAC secret (empty = devmode)",
                         state->fv_auto_stamp_secret,
                         sizeof(state->fv_auto_stamp_secret));
        ImGui::PopItemWidth();
        ImGui::SetItemTooltip("Empty secret = dev mode. Stamp is written but the\n"
                              "verifier accepts any signature on load (with a stderr\n"
                              "warn). Production: set a non-empty secret in BOTH the\n"
                              "suite (here) and engine.cfg (held_out_stamp_secret),\n"
                              "and flip held_out_gate_strict=1 in engine.cfg to refuse\n"
                              "unsigned loads.");

        const BacktestResults *fv_data = &run_control->results;
        // v5.10.0a-bugfix1 — exclude other workers (recompute fresh)
        bool any_worker_running_fv =
            state->tm_running ||
            state->wf_running ||
            state->hp_running ||
            state->mh_running;
        bool can_fv =
#ifdef USE_XGBOOST
            fv_data->sample_count >= 50 && state->model_path[0] != '\0'
            && !any_worker_running_fv
            && !run_control->running;  // E.1.2.D NEW-5 — no train-during-collect
#else
            false;
#endif

        if (state->fv_running) {
            ImGui::ProgressBar(state->fv_progress / 100.0f, ImVec2(-1, 0),
                               "Full validation...");
            if (ImGui::Button("Cancel Full Validation"))
                state->fv_cancel = 1;
        } else {
            if (!can_fv) ImGui::BeginDisabled();
            if (ImGui::Button("Run Full Validation")) {
                state->fv_running = 1;
                state->fv_progress = 0;
                state->fv_cancel = 0;
                state->fv_complete = 0;
                state->fv_has_results = false;
                state->fv_status_msg[0] = '\0';

                FullValidationWorkerArgs *fv_args =
                    (FullValidationWorkerArgs *)malloc(sizeof(FullValidationWorkerArgs));
                fv_args->state = state;
                fv_args->data = fv_data;
                // v5.10.0E — snapshot operator-editable fields at click
                // time, not in worker. Fixes the auto_stamp_path race
                // where typing model_path AFTER clicking produced a
                // misleading "worker race or copy failure" diagnostic.
                {
                    size_t n = strnlen(state->model_path, sizeof(state->model_path));
                    if (n >= sizeof(fv_args->snap_model_path))
                        n = sizeof(fv_args->snap_model_path) - 1;
                    memcpy(fv_args->snap_model_path, state->model_path, n);
                    fv_args->snap_model_path[n] = '\0';
                }
                {
                    size_t n = strnlen(state->fv_auto_stamp_secret,
                                       sizeof(state->fv_auto_stamp_secret));
                    if (n >= sizeof(fv_args->snap_fv_auto_stamp_secret))
                        n = sizeof(fv_args->snap_fv_auto_stamp_secret) - 1;
                    memcpy(fv_args->snap_fv_auto_stamp_secret,
                           state->fv_auto_stamp_secret, n);
                    fv_args->snap_fv_auto_stamp_secret[n] = '\0';
                }
                // v5.11.41 — snap label params from BacktestRunConfig at click time.
                // Live in run_control->run_config (BacktestRunConfig) which is
                // operator-mutable. Worker uses these to populate fv_results.req_label_*
                // before calling Backtest_RunFullValidation, which embeds them in
                // the stamp body. Closes /parity-check 2026-05-07-stamp CRITICAL-1.
                fv_args->snap_label_forward_ticks = run_control->run_config.label_forward_ticks;
                fv_args->snap_label_tp_pct        = run_control->run_config.label_tp_pct;
                fv_args->snap_label_sl_pct        = run_control->run_config.label_sl_pct;
                // E.1.2.C — label_type from run_config (the labels' own source), the
                // rest from the panel at click time. See the struct comment.
                fv_args->snap_label_type          = run_control->run_config.label_type;
                fv_args->snap_wf_n_splits         = state->wf_n_splits;
                fv_args->snap_wf_horizon_ticks    = Training_ResolvePurgeHorizon(state);   // s5 leaf-16
                fv_args->snap_wf_buffer_ticks     = state->wf_buffer_ticks;
                fv_args->snap_wf_min_train        = state->wf_min_train;
                fv_args->snap_fv_gap_threshold    = state->fv_gap_threshold;
                fv_args->snap_fv_held_out_fraction = state->fv_held_out_fraction;
                // E.1.2.C follow-up — the same click-time snapshot the Train path
                // builds, so BOTH entry points describe one architecture.
                fv_args->snap_hp = Training_SnapshotHyperparams(state);
                pthread_create(&state->fv_tid, NULL, fullvalidation_worker_fn, fv_args);
                pthread_detach(state->fv_tid);
            }
            if (!can_fv) {
                ImGui::EndDisabled();
#ifndef USE_XGBOOST
                ImGui::SameLine();
                ImGui::TextDisabled("Build with -DUSE_XGBOOST=ON");
#else
                ImGui::SameLine();
                if (state->model_path[0] == '\0')
                    ImGui::TextDisabled("Set Model Path first");
                else if (fv_data->sample_count < 50)
                    ImGui::TextDisabled("Need 50+ samples");
#endif
            }
        }
    }

    if (state->fv_has_results) {
        const FullValidationResults *fv = &state->fv_results;
        ImGui::Separator();

        // Held-out metric line — the load-bearing "did the model generalize?"
        // signal. Color: green when held_out >= wf_mean (no degradation),
        // yellow when small gap, red when above threshold.
        double wf_metric = (fv->label_kind == 1)
            ? fv->walkforward.mean_val_correlation
            : fv->walkforward.mean_val_accuracy;
        double gap = wf_metric - (double)fv->held_out_metric;
        if (gap < 0) gap = -gap;
        ImVec4 ho_col = (gap > (double)fv->gap_threshold)
            ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f)
            : (gap > (double)fv->gap_threshold * 0.5)
                ? ImVec4(0.95f, 0.75f, 0.30f, 1.0f)
                : ImVec4(0.55f, 0.76f, 0.51f, 1.0f);
        if (fv->ran_held_out) {
            ImGui::TextColored(ho_col,
                "Held-out: %.4f (WF mean: %.4f, gap: %.4f, threshold: %.4f)",
                (double)fv->held_out_metric, wf_metric, gap, (double)fv->gap_threshold);
        } else {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f),
                "Held-out did NOT complete");
        }

        // Auto-stamp result line. Sourced from the worker's status_msg.
        // v5.11.36 — operator-flagged 2026-05-07: long status lines (e.g.
        // "Held-out OK; auto-stamp skipped — model_path='...' snapshot
        // non-empty but auto_stamp_path empty (internal copy failure;
        // report bug)") ran off the panel right edge on 1080p. Use
        // PushStyleColor + TextWrapped instead of TextColored so the
        // text wraps at panel width with color preserved.
        if (state->fv_status_msg[0]) {
            ImVec4 stamp_col = fv->auto_stamp_attempted && fv->auto_stamp_ok
                ? ImVec4(0.55f, 0.76f, 0.51f, 1.0f)
                : ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, stamp_col);
            ImGui::TextWrapped("%s", state->fv_status_msg);
            ImGui::PopStyleColor();
        }
    }

    // v5.10.0 Item A — per-phase timer breakdown. Read singleton state
    // populated by the latest backtest run. Header is collapsing — most
    // operators don't want it open by default. Only render when populated;
    // a never-run state has nothing useful to show.
    if (tt::PhaseTimer_Global().populated &&
        ImGui::CollapsingHeader("Phase Timing (last run)")) {
        const auto& pt = tt::PhaseTimer_Global();
        double total_ms = pt.total_ns / 1.0e6;
        if (total_ms > 0.0) {
            auto row = [&](const char* label, uint64_t ns, bool nested = false) {
                if (ns == 0) return;
                double ms = ns / 1.0e6;
                double pct = 100.0 * (double)ns / (double)pt.total_ns;
                ImGui::Text("%s%-18s %8.1f ms  (%5.1f%%)",
                            nested ? "  " : "",
                            label, ms, pct);
            };
            row("parse:",           pt.parse_ns);
            row("fan_out_hot:",     pt.fan_out_hot_ns);
            row("feature_collect:", pt.feature_collect_ns);
            row("label_compute:",   pt.label_compute_ns);
            row("wf_eval:",         pt.wf_eval_ns);
            row("xgboost_train:",   pt.xgboost_train_ns, /*nested=*/true);
            row("held_out_eval:",   pt.held_out_eval_ns);
            row("stamp_emit:",      pt.stamp_emit_ns);
            ImGui::Separator();
            ImGui::Text("%-18s %8.1f ms", "total:", total_ms);
        }
    }

    ImGui::End();
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GUI_Panel_Training]
//======================================================================

#endif // BACKTEST_PANELS_HPP
