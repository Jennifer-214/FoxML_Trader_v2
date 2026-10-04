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
// what the panels draw with, included here rather than left to the suite's include order — a header that parses on its
// own is one the layout gate's isolate probe and the editor can read (D-414 register, hole 12)
#include "../GUI/FoxmlTheme.hpp"        // FoxmlColors
#include "../GUI/DashboardPanels.hpp"   // SectionHeader
#include "BacktestEngine.hpp"
#include "TrainingWorkers.hpp"   // E.1.3 MP-1 — the ML producer core the Train buttons drive
#include "SuiteLease.hpp"        // E.1.3 MP-6 — the suite run lease, the launch funnel and SuiteJob (D-503 / D-506)
#include "SuiteStartGates.hpp"   // E.1.3 MP-6 — every start button's gate, ImGui-free and cell-tested (D-507)
#include "SuiteModal.hpp"        // E.1.3 MP-6 — every suite modal, one way + the ONE launch-failure surface (D-507)
#include "BacktestSharded.hpp"  // phase 13: per-core sharded backtest path
#include "../ML_Headers/ModelPathSchema.hpp"  // D-431 nested layout — the path-grammar SSoT
#include <errno.h>   // 2026-09-03 — the data-file sidecar writer fails LOUD with errno (path-schema discipline 5)
#include "../MemHeaders/DirCreate.hpp"        // D-431 — FoxDir_CreateParents (family+horizon chain)
#include "../CoreFrameworks/CfgPaths.hpp"     // the cfg-filename SSoT (the panels' default cfg)
#include "Fingerprint.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <ftw.h>          // v5.11.51 — nftw() for recursive directory delete
#include <unistd.h>       // v5.15.5 — fork() / execlp() / _exit() for Open Folder Path

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
    // selection (its count is DataPanel_SelectedCount — computed, never stored, so it cannot outlive a scan)
    bool selected[DATA_MAX_FILES];
    // scan state
    bool scanned;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-02]
// [SIZE]_[526600B]
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
    // the selection follows the FILE across a rescan, never its index (D-507 review F7): remember the selected names,
    // then re-mark them in the new, sorted list (StartGate_RekeySelection — cell-tested). Static, not 512 KB of stack;
    // the Data panel runs on the GUI thread only.
    static char kept[DATA_MAX_FILES][256];
    int kept_n = 0;
    for (int i = 0; i < state->file_count && i < DATA_MAX_FILES; i++)
        if (state->selected[i]) memcpy(kept[kept_n++], state->files[i], sizeof(kept[0]));

    state->file_count = 0;
    state->scanned = true;

    // scan data_dir recursively for .csv files
    DIR *dir = opendir(state->data_dir);
    if (!dir) {   // a scan that found nothing selects nothing
        memset(state->selected, 0, sizeof(state->selected));
        return;
    }

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

    StartGate_RekeySelection(kept, kept_n, state->files, state->file_count, DATA_MAX_FILES, state->selected);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[DataPanel_Scan]
//======================================================================

//======================================================================
// [FUNCTION]_[DataPanel_SelectedCount]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the files selected among the files the last scan found — the panel's fields read into the cell-tested StartGate_SelectedFiles; computed on every read, never stored, so it cannot outlive a scan]
//======================================================================
// [CODE]
//======================================================================
static inline int DataPanel_SelectedCount(const DataPanelState *state) {
    return StartGate_SelectedFiles(state->selected, state->file_count, DATA_MAX_FILES);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[DataPanel_SelectedCount]
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
// Thread safety: the worker writes all fields, then publishes its job's
// result (SuiteJob_Publish — a release); the GUI reads only once the job
// shows no run (SuiteJob_Running — an acquire on the trampoline's release).
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
    // TECH_DEBT-302 (b) — WHICH horizon this distribution belongs to. The snapshot is computed from
    // the single results->labels[] array, which under a multi-horizon collect holds whatever the
    // LAST loop iteration wrote. The code always knew that (see the collect worker's comment);
    // the PANEL did not say so, and presented one horizon's class split as if it were the run's.
    // 0 = single-horizon run / unknown, and the panel omits the qualifier in that case.
    int horizon_ticks;
    int label_kind;          // 0 = binary, 1 = regression, 2 = multiclass
    int num_classes;         // ≥2 for multiclass; 0 otherwise

    // binary
    int pos_count;
    int neg_count;
    int neutral_count;

    // classification (binary + multiclass) — class_counts[c] = samples in
    // class c over the BASELINE population (Backtest_LabelClassCounts SSoT:
    // binary excludes neutrals, matching WF pre-compaction), baseline_total =
    // that population's size. Baseline calls divide by baseline_total, NEVER
    // sample_count (which includes neutrals/NaN and understates the majority).
    int class_counts[16];
    int baseline_total;

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
// [UPDATED]_[2026-08-26]
// [SIZE]_[116B]
// [ALIGN]_[4]
// [CACHE_LINES]_[2]
// [STRADDLE]_[class_counts@32]
//======================================================================
// [END_STRUCT]_[SamplesSnapshot]
//======================================================================

//======================================================================
// [STRUCT]_[RunControlState]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [THREAD]_[[RUN_WORKER_WRITER] [GUI_READER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[state for the Run Control panel — the worker's display flags, run config + results, snapshot, and candle feed]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
struct RunControlState {
    // Run Backtest, Collect Features and Collect Multi-Horizon share ONE job: they write the same results, run_config
    // and snapshots (D-506 — the funnel owns its start and its end; progress is a percent)
    SuiteJob job;
    // the last run's record: the request it started from, written by its worker at its start, under the lease (D-507 —
    // never by a click); a multi-horizon collect's label pass then names the labels it left in the results
    // (BacktestRunConfig_RecordLabels). The GUI reads it only at rest — in a start's click behind its gate (the lease
    // free: only this thread takes it), and through RunControl_AtRest (the purge display, Full Validation's labels'
    // horizon) — so no worker is writing it then
    BacktestRunConfig run_config;
    BacktestResults results;
    CandleAccumulator *candle_acc;
    TUISnapshot *snapshot;       // filled by the worker at its run's end (no seqlock) — the panels draw the GUI's copy (RunControl_AdoptSnapshot)
    SamplesSnapshot stats_snapshot; // distribution stats — see comment above struct
    // E.1.2.G — per-horizon collect distributions (operator ask 2026-09-01: "can
    // we display the breakdown per horizon instead of just the last one"). The
    // collect worker fills [0..count) then writes mh_collect_snap_count LAST;
    // the GUI reads only once the job shows no run — the same happens-before
    // edge stats_snapshot already rides. tp/sl are the click-time echo for the
    // table; the label KIND rides inside each snapshot (heterogeneous under a
    // Label Kind CSV override).
    SamplesSnapshot mh_collect_snap[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    // alignas: the tp/sl echo pair fills exactly one 64B line (8+8 floats) —
    // H6 on a [THREAD]-tagged struct; the layout gate flagged the unaligned
    // first cut straddling 9879→9880.
    alignas(64) float mh_collect_tp[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    float mh_collect_sl[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    volatile int mh_collect_snap_count;   // 0 = none yet / single-horizon collect
    char config_path[256];
    // D-483 C (2026-09-04) — the Run Control PRODUCER for BacktestRunConfig::bandit_state_prior_path
    // (the field existed since v5.10.0a.next.1 with a test-only writer — Class 12). With the backtest
    // now fresh-only (it binds no state dir, loads no learned state from the model tree), an explicit
    // prior FILE is its only learned-state input. Empty = start uniform.
    char bandit_state_prior_path[400];
    // the last run's BacktestRunStatus — the worker writes it before its job publishes; read once the job is done
    uint8_t last_status;
};
// Over-aligned through its alignas(64) members: it lives in foxml_suite.cpp's static storage, never malloc / calloc
// (Check K — an over-aligned type from bare malloc is misaligned, UB).
static_assert(alignof(RunControlState) == 64, "RunControlState's alignment changed: re-check how it is allocated");
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-02]
// [SIZE]_[633728B]
// [ALIGN]_[64]
// [CACHE_LINES]_[9902]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[RunControlState]
//======================================================================

// A finished Run Control run whose backtest actually ran — the ONE predicate the results displays go by (the panels'
// result lines, the dashboard, the trade refreshes, the window title, Past Runs' save): a refused or failed run is done
// but has no results to show. The panels that draw the run's SNAPSHOT draw the GUI's copy instead, which every run's
// start retires (RunControl_ForgetDisplay) — so after a run that did not happen they show nothing either.
static inline bool RunControl_HasRun(const RunControlState *rc) {
    return SuiteJob_Done(&rc->job) && rc->last_status == BACKTEST_RUN_DONE;
}

// Run Control's outputs AT REST — true while no Run Control run can be writing them (F8; E.1.3 MP-6 step 10.3): its
// results, its run's record (run_config), its snapshot, the collect's per-horizon rows and the samples line. Its two
// workers (the backtest, the collect) are their only writers — every other worker reads the dataset through a const
// pointer (CS-271) and writes none of them — and the job's `running` is set on this thread before the spawn and cleared
// after the worker's last write (SuiteWorker_ReleaseOnExit), so it is exact. EVERY read of those outputs that is not a
// results display (those read RunControl_HasRun) goes through it: a gate's sample count, the purge and the labels'
// horizon, the collect's table, the GUI's copies of the snapshot and of Verify Stamp's secret. Evaluated at each read,
// never kept for a frame: a start clicked earlier in the same frame has already begun its job. A run of another job (a
// training run, a sweep) leaves them at rest — it writes none of them, so nothing it does hides them.
static inline bool RunControl_AtRest(const RunControlState *rc) {
    return !SuiteJob_Running(&rc->job);
}

// The collected dataset's sample count for a start gate drawn every frame — 0 while Run Control's outputs are not at
// rest. Every gate checks the lease before its samples, so the 0 never changes the reason a button shows.
static inline int RunControl_DatasetSamples(const RunControlState *rc) {
    return RunControl_AtRest(rc) ? rc->results.sample_count : 0;
}

// The GUI's copy of the run's snapshot — what every panel that draws it reads (F4 of the step-10 review; Class 63): the
// worker fills Run Control's snapshot at its run's end, a memset and then field writes with no seqlock, so a panel
// reading it meanwhile could draw half of each. Copied each frame while Run Control's outputs are at rest, held through
// a run (63 KB a frame — far less than the panels' own draw).
static inline void RunControl_AdoptSnapshot(TUISnapshot *view, const RunControlState *rc) {
    if (view && rc->snapshot && RunControl_AtRest(rc)) memcpy(view, rc->snapshot, sizeof(*view));
}

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
    strncpy(state->config_path, CFG_PATH_BACKTEST_CFG, sizeof(state->config_path) - 1);
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
// E.1.2.G — the array-taking core. The BacktestResults overload below delegates
// here; the per-horizon collect table calls this directly with each horizon's
// own label vector (results->labels only ever holds the LAST horizon's — the
// TECH_DEBT-302 (b) footnote this refactor exists to retire from the display).
static inline void SamplesSnapshot_ComputeFromLabels(SamplesSnapshot *snap,
                                                     const float *labels, int n,
                                                     int label_type) {
    memset(snap, 0, sizeof(*snap));
    snap->label_type = label_type;
    int K = LabelType_NumClasses(label_type);
    snap->num_classes = K;
    snap->label_kind  = (K == 0) ? 0 : (K == 1 ? 1 : 2);

    if (n <= 0 || !labels) return;
    snap->sample_count = n;

    if (snap->label_kind == 1) {
        // regression: range / mean / σ
        float lmin = labels[0], lmax = labels[0];
        double sum = 0.0, sum_sq = 0.0;
        for (int i = 0; i < n; i++) {
            float v = labels[i];
            sum += v; sum_sq += (double)v * v;
            if (v < lmin) lmin = v;
            if (v > lmax) lmax = v;
        }
        double mean = sum / n;
        double var  = (sum_sq / n) - mean * mean;
        snap->lmin    = lmin;
        snap->lmax    = lmax;
        snap->lmean   = (float)mean;
        snap->lstddev = (var > 0.0) ? (float)sqrt(var) : 0.0f;
    } else {
        // classification (binary + multiclass): baseline histogram via the ONE
        // population rule the stamp gate's skill floor also uses (Class-62/F4
        // close — previously the multiclass histogram was open-coded here and
        // binary left class_counts all-zero, so the panel's binary baseline
        // degenerated to a flat 0.5 while the gate refused against the real one).
        snap->baseline_total = Backtest_LabelClassCounts(labels, n,
                                                         label_type, snap->class_counts);
        if (snap->label_kind == 0) {
            // binary display counters: +/-/neutral over ALL samples (the
            // neutral share is exactly what the panel must show)
            for (int i = 0; i < n; i++) {
                float v = labels[i];
                if (v > 0.5f) snap->pos_count++;
                else if (v < 0.5f) snap->neg_count++;
                else snap->neutral_count++;
            }
        }
    }
}

static inline void SamplesSnapshot_Compute(SamplesSnapshot *snap,
                                             const BacktestResults *r,
                                             int label_type) {
    SamplesSnapshot_ComputeFromLabels(snap, r ? r->labels : NULL,
                                      r ? r->sample_count : 0, label_type);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SamplesSnapshot_Compute]
//======================================================================

// worker thread function
//======================================================================
// [STRUCT]_[BacktestWorkerArgs]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[a Run Backtest / Collect Features worker's own args — the panel it reports to and the request it starts from, built at the click; 64-aligned through BacktestRunConfig, so allocated by TrainingWorkers_AllocZeroed]
//======================================================================
// [CODE]
//======================================================================
struct BacktestWorkerArgs {
    RunControlState  *state;
    BacktestRunConfig request;   // the run's input — the worker makes it the run's record once it holds the lease (D-507)
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-02]
//----------------------------------------------------------------------
// [SIZE]_[577984B]
// [ALIGN]_[64]
// [CACHE_LINES]_[9031]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[BacktestWorkerArgs]
//======================================================================

//======================================================================
// [FUNCTION]_[RunControl_ForgetDisplay]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[a Run Control run's start, in its worker, holding the lease: forget the display that described the last results — the samples snapshot, the per-horizon collect table, the chart — just before Backtest_Run resets those results — and the run's snapshot]
//======================================================================
// Every Run Control run replaces the shared results, so every one retires what described them — Run Backtest used to
// leave the last collect's per-horizon table standing over results it had just reset, and a run that did not happen
// left the last one's snapshot for the GUI to adopt at rest: the engine header, the ML status and the P&L and volume
// charts drew a run that was gone (INGEST-0's review, F5(d)); the replay fills the snapshot only at a run's end. The GUI
// reads the snapshots only while the job shows no run, and the chart under its own mutex, so the worker can write them
// here.
//======================================================================
// [CODE]
//======================================================================
static inline void RunControl_ForgetDisplay(RunControlState *state) {
    memset(&state->stats_snapshot, 0, sizeof(state->stats_snapshot));
    state->mh_collect_snap_count = 0;
    if (state->candle_acc)
        CandleAccumulator_Reset(state->candle_acc);
    if (state->snapshot)
        memset(state->snapshot, 0, sizeof(*state->snapshot));
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RunControl_ForgetDisplay]
//======================================================================

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
static inline void *backtest_worker_fn(void *arg, uint64_t lease) {
    BacktestWorkerArgs *args = (BacktestWorkerArgs *)arg;
    RunControlState *state = args->state;
    // the run's start, holding the lease (D-507): its request becomes the record of what the results hold, and the
    // display that described the last results is forgotten — Backtest_Run resets those results next, so record, display
    // and data change together. A start the funnel refused never got here and changed none of them; a thread that did not
    // start never got here either — only its job ended, hiding the last results, as D-506 decided
    state->run_config = args->request;
    free(args);
    RunControl_ForgetDisplay(state);

    state->last_status = Backtest_Run(lease, &state->results, &state->run_config,
                                      &state->job.progress, &state->job.cancel,
                                      state->candle_acc, state->snapshot);
    if (state->last_status != BACKTEST_RUN_DONE) {   // no run happened: nothing to post-process
        SuiteJob_Publish(&state->job);
        return NULL;
    }

    // Compute display snapshot after labels are populated by Backtest_Run's
    // post-pass. Done BEFORE the job publishes, so the GUI never sees a stale
    // snapshot (the publish, then the trampoline's end, is the happens-before edge).
    SamplesSnapshot_Compute(&state->stats_snapshot, &state->results,
                              state->run_config.label_type);

    SuiteJob_Publish(&state->job);
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
// [OVERVIEW]_[worker-thread args for the multi-horizon label-collect job — run_control + the run's request (the one builder's, as Collect Features) + the snapped horizon list + parallel per-horizon TP/SL barrier arrays; 64-aligned through BacktestRunConfig, so allocated aligned (TrainingWorkers_AllocZeroed)]
//======================================================================
// [CODE]
//======================================================================
struct CollectMultiHorizonWorkerArgs {
    RunControlState *run_control;
    BacktestRunConfig request;   // the run's input — the worker makes it the run's record once it holds the lease (D-507)
    int snap_horizon_count;
    int snap_horizons[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    // v5.11.40 — per-horizon TP/SL. Snap-time arrays parallel to
    // snap_horizons[]. snap_tp_pct[h] is the TP barrier for horizon h.
    // For broadcast (single-value) mode, the click handler fills all
    // entries with the same value. Arrays are always horizon_count
    // wide; aligned 1:1 with snap_horizons.
    float snap_tp_pct[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    float snap_sl_pct[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    // E.1.2.G — per-horizon label KIND, snapped at click with the same
    // broadcast-or-positional resolution the TRAIN click uses. Before this the
    // collect labelled every horizon with the DROPDOWN kind while train obeyed
    // the Label Kind CSV — so the panel's collect summary could describe a
    // different label than training consumed (measured 2026-09-01: collect
    // said 1.5/49.6/48.8 under kind 12 while train shipped kind 7).
    int snap_label_kind[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-02]
// [SIZE]_[578176B]
// [ALIGN]_[64]
// [CACHE_LINES]_[9034]
// [STRADDLE]_[snap_tp_pct@578020 · snap_label_kind@578084]
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
static inline void *collect_multi_horizon_worker_fn(void *arg, uint64_t lease) {
    auto *args = (CollectMultiHorizonWorkerArgs *)arg;
    RunControlState *rc = args->run_control;
    int horizon_count = args->snap_horizon_count;
    int horizons[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    float tp_pcts[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    float sl_pcts[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    int label_kinds[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    memcpy(horizons, args->snap_horizons, sizeof(horizons));
    memcpy(tp_pcts,  args->snap_tp_pct,   sizeof(tp_pcts));
    memcpy(sl_pcts,  args->snap_sl_pct,   sizeof(sl_pcts));
    memcpy(label_kinds, args->snap_label_kind, sizeof(label_kinds));
    // the run's start, holding the lease (D-507) — see backtest_worker_fn: its request becomes the run's record (the
    // label pass below rewrites the record's label params to the labels it leaves in the results — the last horizon's),
    // and the display that described the last results is forgotten (E.1.2.G — the stale per-horizon table from a
    // previous collect must not outlive this one; the count returns non-zero only after [0..count) refills)
    rc->run_config = args->request;
    free(args);
    RunControl_ForgetDisplay(rc);

    // 1. Collect features ONCE. The request's label_forward_ticks is the
    //    CSV's first horizon at the click — we'll overwrite labels per
    //    horizon afterwards.
    rc->last_status = Backtest_Run(lease, &rc->results, &rc->run_config,
                                   &rc->job.progress, &rc->job.cancel,
                                   rc->candle_acc, rc->snapshot);
    if (rc->last_status != BACKTEST_RUN_DONE) {   // no run happened: no label pass, no per-horizon table
        fprintf(stderr, "[collect-mh] no run (%s) — no labels computed\n", BacktestRunStatus_Name(rc->last_status));
        SuiteJob_Publish(&rc->job);
        return NULL;
    }

    // 2. Per-horizon label diagnostic — ONE batched walk (E.1.2.D leaf 5),
    //    was one full-corpus walk PER horizon. Each target carries its own
    //    kind (E.1.2.G — the Label Kind CSV), horizon and barriers; the LAST
    //    target writes rc->results.labels directly, so the post-loop state
    //    (SamplesSnapshot below reads it) is the last horizon's — and the
    //    record then names exactly that target (BacktestRunConfig_RecordLabels,
    //    D-507 review F1).
    // v5.11.40 — label_tp_pct/label_sl_pct rotate per horizon (broadcast or
    //            per-horizon CSV from operator); double-typed, percent
    //            pass-through without /100.
    bool labelled = false;   // did the label pass below run — only then are results.labels the last horizon's
    // No samples — none collected, or the run's own label pass aborted and dropped them (Backtest_Run, step 10.7) —
    // means nothing to label: no pass, no table.
    if (!SuiteCancel_Requested(&rc->job.cancel) && horizon_count > 0 && rc->results.sample_count > 0) {
        LabelBatchTarget bt[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
        float *tmp_bufs[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX] = {0};
        int bt_ok = 1;
        // every target's params first, THEN the earlier horizons' buffers: an allocation that fails part-way must not
        // leave the last target — the one the fallback below labels and the record takes — uninitialised (it used to:
        // the loop broke before reaching it, and the fallback wrote through a garbage out_labels)
        for (int h = 0; h < horizon_count; ++h) {
            bt[h] = LabelBatchTarget{};
            // E.1.2.G — the CSV override reaches COLLECT exactly as it reaches
            // TRAIN. One kind source, two consumers, no divergence.
            bt[h].label_type    = label_kinds[h];
            bt[h].tp_pct        = (double)tp_pcts[h];
            bt[h].sl_pct        = (double)sl_pcts[h];
            bt[h].forward_ticks = horizons[h];
        }
        bt[horizon_count - 1].out_labels = rc->results.labels;   // post-state = last horizon
        for (int h = 0; h < horizon_count - 1; ++h) {
            tmp_bufs[h] = (float *)malloc((size_t)rc->results.sample_count * sizeof(float));
            if (!tmp_bufs[h]) { bt_ok = 0; break; }
            bt[h].out_labels = tmp_bufs[h];
        }
        int label_rc = -1;   // samples labelled, or -1 = the pass aborted (it logged why) — the dataset is then dropped
        if (!bt_ok) {
            // Pathological small-alloc failure: keep the post-state contract
            // (last horizon into results.labels) and drop the earlier
            // horizons' diagnostics rather than the whole collect.
            fprintf(stderr, "[collect-mh] batch buffer alloc failed; "
                            "labeling last horizon only\n");
            label_rc = Backtest_ComputeLabelsBatch(&rc->results, &rc->run_config,
                                                   &bt[horizon_count - 1], 1);
        } else {
            label_rc = Backtest_ComputeLabelsBatch(&rc->results, &rc->run_config,
                                                   bt, horizon_count);
        }
        if (label_rc < 0) {
            // The pass aborted (it said why) and left every label NaN — fail closed (step 10.7): the dataset is DROPPED,
            // so every training gate asks for a collect, and nothing describes labels that do not exist: no record
            // update, no table, no summary.
            BacktestResults_DropSamples(&rc->results, "collect-mh");
            rc->mh_collect_snap_count = 0;
        } else {
            // the record describes the labels the results now hold — the LAST horizon's, not the click's position 0:
            // Full Validation, Walk-Forward and the HP sweep take the labels' identity from it (D-507 review F1; until
            // MP-3a's labels carry their own params)
            BacktestRunConfig_RecordLabels(&rc->run_config, &bt[horizon_count - 1]);
            labelled = true;
            // Legacy accumulate-semantics: the old loop's every per-horizon walk
            // folded its NaN counters into results.stats. Same totals, one fold.
            for (int h = 0; h < horizon_count; ++h) {
                if (!bt_ok && h < horizon_count - 1) continue;  // never computed
                rc->results.stats.nan_labels_total   += bt[h].nan_total;
                rc->results.stats.nan_labels_dropped += bt[h].nan_dropped;
            }
            // The label pass above takes no cancel: once begun it runs to its end, and the dataset holds the last horizon's
            // labels — what Walk-Forward, the HP sweep and Full Validation use. So its summaries always finish (each one O(n),
            // milliseconds): a cancel here used to leave k < N rows, the last horizon's among the missing, so the table
            // described the dataset's labels nowhere (the step-9.2 review's A4; E.1.3 MP-6 step 10.4). (A failed buffer
            // labels only the last horizon — said above; it gets no table. An aborted pass never gets here: it dropped
            // the dataset above.)
            if (SuiteCancel_Requested(&rc->job.cancel) && bt_ok)
                fprintf(stderr, "[collect-mh] cancel came during the label pass, which cannot stop once begun — all %d "
                                "horizons were labelled; summarising them\n", horizon_count);
            int snaps_filled = 0;
            for (int h = 0; h < horizon_count; ++h) {
                if (!bt_ok && h < horizon_count - 1) continue;  // no buffer to read

                // E.1.2.G — the per-horizon DISPLAY snapshot, from THIS horizon's
                // own vector (results->labels only ever holds the last horizon's).
                // Kind-aware via the same SSoT the single-horizon panel line uses,
                // which also retires the hand-listed PVS special case that used to
                // live here (Class-19 shape: registry consumer enumerated by hand).
                SamplesSnapshot *hs = &rc->mh_collect_snap[h];
                SamplesSnapshot_ComputeFromLabels(hs, bt[h].out_labels,
                                                  rc->results.sample_count,
                                                  bt[h].label_type);
                hs->horizon_ticks = horizons[h];
                rc->mh_collect_tp[h] = tp_pcts[h];
                rc->mh_collect_sl[h] = sl_pcts[h];
                snaps_filled = h + 1;

                // stderr line, registry-driven for ANY kind (engine.log → LogViewer)
                char cls[192]; cls[0] = '\0'; size_t coff = 0;
                if (hs->label_kind == 2) {
                    int K = hs->num_classes > 16 ? 16 : hs->num_classes;
                    for (int k = 0; k < K && coff < sizeof(cls) - 24; ++k)
                        coff += snprintf(cls + coff, sizeof(cls) - coff, "%sc%d=%d",
                                         k ? ", " : "", k, hs->class_counts[k]);
                } else if (hs->label_kind == 0) {
                    snprintf(cls, sizeof(cls), "%d pos, %d neg, %d neutral",
                             hs->pos_count, hs->neg_count, hs->neutral_count);
                } else {
                    snprintf(cls, sizeof(cls), "mean=%.4f sigma=%.4f range=[%.4f, %.4f]",
                             hs->lmean, hs->lstddev, hs->lmin, hs->lmax);
                }
                fprintf(stderr, "[collect-mh] horizon=%d ticks kind=%s tp=%.3f sl=%.3f: "
                                "%d samples (%s)\n",
                        horizons[h],
                        (bt[h].label_type >= 0 && bt[h].label_type < LABEL_COUNT)
                            ? label_table[bt[h].label_type].name : "?",
                        tp_pcts[h], sl_pcts[h], hs->sample_count, cls);
            }
            // count LAST — the GUI reads once the job shows no run, but a torn mid-loop
            // count would still describe half-filled rows to the first frame. With a
            // failed buffer only the last horizon was labelled: no per-horizon table
            // then — rows before it would be a previous collect's (D-507 second
            // review, R3); the single line below describes the last horizon.
            rc->mh_collect_snap_count = bt_ok ? snaps_filled : 0;
        }
        for (int h = 0; h < horizon_count; ++h) free(tmp_bufs[h]);
    } else if (SuiteCancel_Requested(&rc->job.cancel)) {
        fprintf(stderr, "[collect-mh] cancelled at horizon 0/%d\n", horizon_count);
    }

    // 3. Final SamplesSnapshot from whatever the last horizon's labels are
    //    (operator's "current" view — Train Multi-Horizon will recompute
    //    per-horizon during training so this just reflects the last loop
    //    iteration's distribution). The record's kind is those labels' kind:
    //    the label pass recorded it (or, with no pass, the request's made them).
    SamplesSnapshot_Compute(&rc->stats_snapshot, &rc->results,
                              rc->run_config.label_type);
    // TECH_DEBT-302 (b) — stamp WHICH horizon that was, so the panel can stop implying the
    // distribution describes the whole run. Multi-horizon only; a single-horizon collect leaves
    // it 0 and the panel omits the qualifier.
    // Only when the label pass ran: a collect cancelled before it keeps the replay's labels, made with the
    // request's horizon (R4).
    rc->stats_snapshot.horizon_ticks =
        (labelled && horizon_count > 1 && horizon_count <= ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX)
            ? horizons[horizon_count - 1] : 0;

    SuiteJob_Publish(&rc->job);
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[collect_multi_horizon_worker_fn]
//======================================================================

//======================================================================
// [FUNCTION]_[RunControl_BuildRequest]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE request every Run Control run starts from — the Data panel's selection, the Run Control panel's cfg path and bandit prior, and a collect's label params — read into the worker's own args by the cell-tested BacktestRunConfig_FromSelection; returns the number of files]
//======================================================================
// [CODE]
//======================================================================
static inline int RunControl_BuildRequest(BacktestRunConfig *out, const RunControlState *rc, const DataPanelState *data,
                                          const BacktestLabelRequest *collect) {
    return BacktestRunConfig_FromSelection(out, data->files, data->selected, data->file_count, DATA_MAX_FILES,
                                           rc->config_path, rc->bandit_state_prior_path, collect);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RunControl_BuildRequest]
//======================================================================

// The two causes a start reports before the funnel (SuiteWorker_ReportNotStarted), worded once for every start.
static constexpr const char *START_CAUSE_NO_MEMORY = "out of memory";
static constexpr const char *START_CAUSE_NO_FILES  = "no data files selected";   // the request builder found none

//======================================================================
// [FUNCTION]_[RunControl_Start]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[start Run Backtest — its request built into the worker's own args (nothing shared changes unless the run starts), then through the suite's funnel]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void RunControl_Start(RunControlState *state, DataPanelState *data, LaunchFailureState *lf) {
    // D-507 — the click writes only its own request; the run's record, its display and the shared results change in
    // the worker, holding the lease — so a start the funnel refuses changes none of them, and a thread that does not
    // start changes none of them either: the funnel ends the job it began, which hides the last results (D-506).
    // (D-483 C's explicit bandit prior rides the request — "" = start uniform.)
    BacktestWorkerArgs *args = TrainingWorkers_AllocZeroed<BacktestWorkerArgs>();
    if (!args) {
        SuiteWorker_ReportNotStarted("Run Backtest", START_CAUSE_NO_MEMORY, lf->launch_msg, sizeof(lf->launch_msg));
        return;
    }
    args->state = state;
    if (RunControl_BuildRequest(&args->request, state, data, nullptr) == 0) {   // the gate already requires a file
        SuiteWorker_ReportNotStarted("Run Backtest", START_CAUSE_NO_FILES, lf->launch_msg, sizeof(lf->launch_msg));
        free(args);
        return;
    }
    // the funnel takes the lease or refuses naming the holder (a refused or failed start leaves the args with us)
    if (SuiteWorker_Launch("Run Backtest", &state->job, backtest_worker_fn, args, lf->launch_msg,
                           sizeof(lf->launch_msg)) != SUITE_LAUNCH_STARTED)
        free(args);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RunControl_Start]
//======================================================================

//======================================================================
// [FUNCTION]_[SuiteGate_ShowWhy]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[beside a greyed-out start button, the reason its gate gives — the first term that failed, in that term's tone (D-507)]
//======================================================================
// [CODE]
//======================================================================
static inline void SuiteGate_ShowWhy(const SuiteGate *g) {
    ImGui::SameLine();
    if (g->tone == SUITE_GATE_FIX) ImGui::TextColored(FoxmlColors::yellow, "%s", g->why);
    else                           ImGui::TextDisabled("%s", g->why);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteGate_ShowWhy]
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

    ImGui::Text("%d files, %d selected", state->file_count, DataPanel_SelectedCount(state));
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
static inline void GUI_Panel_RunControl(RunControlState *state, DataPanelState *data, LaunchFailureState *lf) {
    ImGui::Begin("Run Control");

    ImGui::InputText("Config", state->config_path, sizeof(state->config_path));
    // D-483 C — the backtest is FRESH-ONLY: it never loads learned bandit state from a model dir
    // (and never writes any). This file is the one way to start a run from learned weights.
    ImGui::InputText("Bandit prior (optional)", state->bandit_state_prior_path,
                     sizeof(state->bandit_state_prior_path));
    ImGui::SetItemTooltip("Optional path to a bandit_state.json whose buy-side Exp3 weights seed every\n"
                          "ML node at boot (bundle-id check skipped — transfer learning between sibling\n"
                          "bundles). Empty = uniform priors.\n\n"
                          "A backtest binds NO state dir: it loads nothing from the model tree and saves\n"
                          "nothing at completion (D-483 C, 2026-09-04) — a run is reproducible from its\n"
                          "cfg + data + this prior. Exit / Thompson bandits always start uniform.");

    if (SuiteJob_Running(&state->job)) {
        // progress bar
        ImGui::ProgressBar(state->job.progress / 100.0f, ImVec2(-1, 0));
        if (ImGui::Button("Cancel")) {
            SuiteJob_Cancel(&state->job);
        }
    } else {
        // run button — its gate (D-507; Backtest/SuiteStartGates.hpp)
        const SuiteGate gate = StartGate_RunBacktest(DataPanel_SelectedCount(data));
        const bool can_run = SuiteGate_Open(&gate);
        if (!can_run) ImGui::BeginDisabled();
        if (ImGui::Button("Run Backtest")) {
            RunControl_Start(state, data, lf);
        }
        ImGui::SetItemTooltip(
            "Replays selected files through the engine, computes stats only.\n"
            "Use this for quick performance evaluation (Sharpe, DD, win rate).\n\n"
            "If you want to TRAIN an ML model, use \"Collect Features\" in the\n"
            "Training panel instead — it runs the same backtest plus gathers\n"
            "the feature/label samples XGBoost needs.");
        if (!can_run) {
            ImGui::EndDisabled();
            SuiteGate_ShowWhy(&gate);
        }
    }

    if (SuiteJob_Done(&state->job) && !RunControl_HasRun(state)) {
        ImGui::Separator();
        ImGui::TextColored(FoxmlColors::red, "The last run did not happen — %s",
                           BacktestRunStatus_Name(state->last_status));
    }
    if (RunControl_HasRun(state)) {
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

    // NULL = no finished run to show (none yet, refused, or one in progress — the caller passes them only through
    // RunControl_HasRun, E.1.3 MP-6 step 10.3)
    if (!results || results->stats.total_trades == 0) {
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

        // The printf attribute puts every row() under -Werror=format (cmake/FormatGuard.cmake);
        // on a lambda it counts the implicit object, so fmt is argument 3.
        auto row = [](const char *label, const char *fmt, ...) __attribute__((format(printf, 3, 4))) {
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
    float train_correlation;     // regression: the in-sample r — the Train r column (CS-272)
    int   has_train_correlation; // 1 = the summary carried it; 0 = an older summary → the legacy proxy
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
    // 2026-09-03 — the gate metric (balanced accuracy, %) when the summary carries
    // it; has_balanced=0 for pre-2026-09-03 summaries → the row colors by plain
    // accuracy with the old bands (an old record is scored the way it was gated).
    float val_balanced_accuracy;
    int   has_balanced;
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
// [UPDATED]_[2026-09-30]
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
    // E.1.3 MP-6 step 10.3 (F6 of its review) — the GUI's copy of the last Run Control run's auto_stamp_secret, adopted
    // only while Run Control is at rest (PastRuns_AdoptVerifySecret). Step 10.6 — the secret Verify Stamp verifies with
    // is RESOLVED from it by the rule stamps are SIGNED by (CS-277, TrainingWorkers_ResolveStampSecret): the Training
    // panel's field, else this copy; both empty = devmode.
    char    verify_cfg_secret[sizeof(ControllerConfig<BACKTEST_FP>::auto_stamp_secret)];
    char    verify_secret[sizeof(ControllerConfig<BACKTEST_FP>::auto_stamp_secret)];
    // Step 10.6 — the selected run's actions block (its lines, the buttons, the verdict, the stamp-details header) as
    // drawn last frame: the run tables leave this much room below themselves. 0 = not yet drawn (a default stands in).
    float   actions_h;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-03]
// [SIZE]_[401968B]
// [ALIGN]_[16]
// [CACHE_LINES]_[6281]
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
// [FUNCTION]_[PastRuns_AdoptVerifySecret]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[copy the last Run Control run's auto_stamp_secret into Past Runs' own verify_secret while Run Control's outputs are at rest; held through a run — called each frame, before the panel]
//======================================================================
// F6 of the step-10.3 review: the panel used to hold the run's whole config for this one field, so the button had to
// wait out a run (minutes on a large corpus) or race its rewrite. A copy keeps it usable mid-run with the secret it had
// a frame before the run began.
//======================================================================
// [CODE]
//======================================================================
static inline void PastRuns_AdoptVerifySecret(PastRunsState *s, const RunControlState *rc,
                                              const char *panel_secret, size_t panel_cap) {
    if (!s) return;
    if (RunControl_AtRest(rc))
        snprintf(s->verify_cfg_secret, sizeof(s->verify_cfg_secret), "%s", rc->results.config_used.auto_stamp_secret);
    // Step 10.6 (the second review's finding 13) — verify by the rule stamps are SIGNED by: a model the Training panel
    // signed with its own secret failed Verify Stamp, which checked only the cfg's.
    TrainingWorkers_ResolveStampSecret(panel_secret, panel_cap, s->verify_cfg_secret, sizeof(s->verify_cfg_secret),
                                       s->verify_secret, sizeof(s->verify_secret));
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[PastRuns_AdoptVerifySecret]
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
        else if (strcmp(k, "train_correlation") == 0)  { r->train_correlation = (float)tt::parse_double_fast(v); r->has_train_correlation = 1; }   // locale-immune (H5)
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
        // 2026-09-03 — the gate metric (percent, like val_accuracy); absent on older summaries.
        // tt::parse_double_fast, NOT the atof its siblings use: the locale-determinism
        // baseline (tools/locale_determinism_known_pending.txt) is SHRINK-ONLY per file and
        // pre-commit Check F refused the 25th raw parse here — the same rule the E.1.2.C
        // expected_label_type key followed in NodeModelZoo.hpp.
        else if (strcmp(k, "val_balanced_accuracy") == 0) { r->val_balanced_accuracy = (float)tt::parse_double_fast(v); r->has_balanced = 1; }
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

    // expected record (optional; older runs may not have all fields). 2026-09-03:
    // side-addressed — the row shows the ENTRY record unless only an exit
    // summary exists (summary_side), so the expected record follows the same
    // side; the legacy shared expected.cfg is the fallback the resolver owns.
    f = NULL;
    if (ModelPath_ExpectedCfgResolve(run_dir, r->summary_side, path, sizeof(path)) != 0)
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
    // D-483 C + D-431 — a row is a HORIZON dir (`models/<class>/<family>/horizon_<N>/` — the scan's nested rows) whose
    // FAMILY, its parent, carries the `.foxml_state.lock` an engine's ML node bound to it, or a training run writing it,
    // holds; a retired flat run dir is its own family. Deleting under a holder pulls files from a run mid-write or a node
    // mid-serve — so probe the FAMILY's lock with LOCK_NB: held elsewhere ⇒ REFUSE (-2) — and HOLD the probe through the
    // walk of a nested row: released before it, a holder taking the lock in between would have its files removed under
    // it. (A family-level row — a retired flat run dir — is walked whole: the walk unlinks the lock file it holds, so a
    // taker racing that walk makes a fresh one; accepted — the operator is deleting that family.) (Until MP-6 (3c-1)'s
    // review — F1, 2026-10-04 — this probed the row's own dir: a nested family's lock was never seen, the probe made and
    // took a fresh lock file inside the horizon.) Probe only an EXISTING dir — the probe provisions its target's lock
    // file, so a delete never creates what it is about to remove; it may leave the lock file in a family that had none
    // (inert — a dotfile no scanner lists).
    char lock_dir[512];
    if (!ModelPath_FamilyOfHorizonDir(path, lock_dir, sizeof(lock_dir)) &&   // a nested row: its family
        snprintf(lock_dir, sizeof(lock_dir), "%s", path) >= (int)sizeof(lock_dir))   // a flat run dir: itself
        return -1;   // a path no lock dir can be named for: not deleted
    int probe_fd = -1;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        const int lrc = FoxDir_LockExclusive(lock_dir, MODEL_STATE_LOCK_FILE, &probe_fd);
        if (lrc == 0) {
            fprintf(stderr, "[past-runs] REFUSED delete of %s: its model family's lock (%s/%s) is held — an engine's "
                            "ML node bound to it, or a training run writing it (D-483)\n",
                    path, lock_dir, MODEL_STATE_LOCK_FILE);
            return -2;
        }
        // 1: held until the walk is over; -1: the lock cannot be taken (an unwritable family) — proceed, as before
    }
    // FTW_DEPTH = post-order traversal so files deleted before parent dir
    // FTW_PHYS = don't follow symlinks (avoid accidentally walking into other
    //            dirs if operator has bizarre symlink configuration)
    const int rc = nftw(path, past_runs_unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
    FoxDir_Unlock(&probe_fd);   // the walk is over (a flat row's walk removed the lock file too: its inode ends here)
    return rc;
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
// v5.11.57 — Verify Stamp uses the real cfg.auto_stamp_secret for HMAC verification (not just devmode). Since E.1.3 MP-6
// step 10.3 the panel reads its own copy (s->verify_secret, PastRuns_AdoptVerifySecret) instead of holding the last
// run's whole config, which a Run Control run rewrites (F8; F6 of its review) — so the button works mid-run too.
// Empty until a run has loaded a config: the verdict then says devmode.
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-4]
//======================================================================
// [CODE]
//======================================================================
static inline void GUI_Panel_PastRuns(PastRunsState *s) {
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
                            "Train a model in the Training panel — each run writes its family here.");
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
        const bool compare_clicked = ImGui::Button("Compare");   // its frame opens the modal below
        if (compare_clicked) s->compare_modal_open = 1;
        if (!can_compare) ImGui::EndDisabled();

        // Modal: render unconditionally (ImGui no-ops when not open). Inside:
        // pull both rows, render metric deltas with color-coded thresholds.
        // bool proxy so the int compare_modal_open can drive ImGui's bool*
        // signature; sync back after the modal returns. Opened by the click's
        // frame and begun in one call — centred each time it appears, never
        // saved to the ini (SuiteModal_Begin, D-507 9.3).
        bool modal_open_b = (s->compare_modal_open != 0);
        if (SuiteModal_Begin(compare_clicked, "Compare to Baseline", &modal_open_b)) {
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
        ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    // Step 10.6 — a scrolling table given no height takes the window's whole remaining height, so the selected run's
    // actions (Open Folder Path, Copy Path, Verify Stamp, the stamp details) always sat below the panel's edge. Each
    // table gets the height its rows need, capped by what the window has left after the actions block (measured last
    // frame) and never under PAST_RUNS_TABLE_MIN_ROWS rows; past the cap it scrolls its own rows (header frozen).
    enum { PAST_RUNS_TABLE_MIN_ROWS = 4 };
    const float row_h   = ImGui::GetFrameHeightWithSpacing();
    const bool  has_actions = s->selected >= 0 && s->selected < s->count;   // no run selected: no block to leave room for
    const float reserve     = !has_actions ? 0.0f : (s->actions_h > 0.0f ? s->actions_h : row_h * 6.0f);
    auto table_height = [&](int rows) -> float {
        const float chrome = row_h + ImGui::GetStyle().ScrollbarSize;   // the header row + the X scrollbar
        const float want   = chrome + row_h * (float)rows;
        const float floor  = chrome + row_h * (float)PAST_RUNS_TABLE_MIN_ROWS;
        float room = ImGui::GetContentRegionAvail().y - reserve;
        if (room < floor) room = floor;
        return want < room ? want : room;
    };

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
            } else if (ImGui::BeginTable("past_runs_class", 16, flags, ImVec2(0.0f, table_height(n_class)))) {  // v5.11.51: +Date +Delete cols; v5.15.5.E.bugfix: 15→16 (Samples col added but BeginTable count missed; ImGui asserted on 16th TableSetupColumn)
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
                ImGui::TableSetupScrollFreeze(0, 1);   // the header stays while the rows scroll (10.6)
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
                        // 2026-09-03 — a summary that carries the gate metric
                        // (val_balanced_accuracy) is colored by it against the
                        // balanced floor (1/K; no class counts here, so the
                        // uniform floor) + the diagnosis bands (+1pp marginal,
                        // +3pp fee) — the SAME pair the gate and the Training
                        // panel use. Older summaries keep the pre-fix plain
                        // accuracy bands (they were gated that way).
                        ImVec4 vcol;
                        if (r->has_balanced) {
                            const int   K     = r->expected_num_classes >= 2 ? r->expected_num_classes : 2;
                            const float floor = 100.0f * multiclass_balanced_baseline(K, NULL, 0);
                            vcol = (r->val_balanced_accuracy <= floor + 1.0f) ? FoxmlColors::red
                                 : (r->val_balanced_accuracy <  floor + 3.0f) ? FoxmlColors::yellow
                                                                              : FoxmlColors::green;
                        } else {
                            // pre-2026-09-03 record: chance-level bands on plain accuracy
                            // (3-class baseline ~33%, binary ~50%; majority dominance shifts these)
                            float thresh_low  = (r->expected_num_classes >= 2) ? 35.0f : 50.0f;
                            float thresh_good = (r->expected_num_classes >= 2) ? 50.0f : 60.0f;
                            vcol = (r->val_accuracy < thresh_low)  ? FoxmlColors::red
                                 : (r->val_accuracy < thresh_good) ? FoxmlColors::yellow
                                                                     : FoxmlColors::green;
                        }
                        ImGui::TextColored(vcol, "%.1f%%", r->val_accuracy);
                        if (r->has_balanced)
                            ImGui::SetItemTooltip("balanced accuracy (the gate metric): %.1f%%",
                                                  r->val_balanced_accuracy);
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
            } else if (ImGui::BeginTable("past_runs_regr", 15, flags, ImVec2(0.0f, table_height(n_regr)))) {  // v5.11.55: +Date +Delete cols; v5.15.5.E.bugfix: 14→15 (Samples col added but BeginTable count missed; same off-by-one as past_runs_class)
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
                ImGui::TableSetupScrollFreeze(0, 1);   // the header stays while the rows scroll (10.6)
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

                    // Train r — the summary's train_correlation (CS-272: the multi-horizon
                    // writer emits it for regression). An older summary has none, and keeps the
                    // proxy this column always showed: its "accuracy" value (the deleted single-
                    // horizon trainer stored r there; the multi-horizon writer wrote 0.00).
                    ImGui::TableNextColumn();
                    if (r->has_train_correlation) ImGui::Text("%.3f", r->train_correlation);
                    else                          ImGui::Text("%.3f", r->train_accuracy / 100.0f);

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
    // D-507 9.3 — opened and begun in ONE call (so the two can never sit in
    // different ID scopes again): opened while a delete is pending, never
    // over another popup (a plain OpenPopup would close the suite's
    // launch-failure modal, and it this one, every frame); centred each time
    // it appears, never saved to the ini (SuiteModal.hpp).
    if (SuiteModal_Begin(s->pending_delete_idx >= 0, "##DeleteConfirmModal")) {
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
                } else if (rc == -2) {
                    // D-483 C — a row whose FAMILY's lock is held is not deletable while it is in use
                    snprintf(s->status_msg, sizeof(s->status_msg),
                             "delete REFUSED: %s is in use — its model family's lock is held (an engine's ML "
                             "node bound to it, or a training run writing it)", dr->full_path);
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
        // Step 10.6 — measure this block (up to the stamp-details header, never its contents) for next frame's table
        // heights: each mark() moves the measured end past the last actions element drawn so far.
        const float actions_top = ImGui::GetCursorPosY();
        auto mark = [&] { s->actions_h = ImGui::GetCursorPosY() - actions_top; };
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
                // v5.11.57 — use cfg.auto_stamp_secret: the panel's own copy of
                // it (PastRuns_AdoptVerifySecret — E.1.3 MP-6 step 10.3). Empty =
                // devmode (accepts any signature). This is the backtest config's
                // secret; the engine verifies with engine.cfg's, and a stamp signed
                // with the Training panel's own secret field (CS-277) verifies only
                // where the two agree — E.1.3 MP-6 step 10.6 aligns this check
                // with the signing rule.
                const char *verify_secret = s->verify_secret;
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
                    // a FAIL is -1: the Stamp column draws it as a red ✗ and the "Stamp FAIL" filter selects it — 0 (the
                    // value here before E.1.3 MP-6 step 10.3) drew a failed stamp as "?, not yet verified" and hid it
                    // from that filter
                    r->stamp_verify_state = -1;
                    r->stamp_verify_has_full = 0;  // FAIL reason IS the message
                    snprintf(r->stamp_verify_msg, sizeof(r->stamp_verify_msg),
                        "%d/%d roles OK; %s FAIL — %s",
                        n_ok, n_checked, fail_role, fail_reason);
                }
            }
        }

        mark();   // the buttons row
        // Render verify result if button has been pressed for this run.
        if (r->stamp_verify_msg[0]) {
            ImVec4 vc = (r->stamp_verify_state == 1)
                ? ImVec4(0.55f, 0.76f, 0.51f, 1.0f)
                : ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
            ImGui::TextColored(vc, "%s", r->stamp_verify_msg);
            mark();   // the verdict line

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
                const bool details_open = ImGui::TreeNode(tree_id);
                mark();   // the "Stamp details" header — its expanded contents may run long, and the window scrolls them
                if (details_open) {
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
                        // Display-only doubles (H4): these three are FPN_Binary<64> since the
                        // .B.3 cfg-derived struct-gen, and a raw 16-byte struct fed to %g / %f
                        // printed garbage (UB). -Werror=format now refuses that at compile time.
                        ImGui::Text("  confidence_threshold_scale:       %.4g",
                                    FPN_ToDouble(v.confidence_threshold_scale));
                        ImGui::Text("  barrier_gate_enabled:             %d",
                                    v.barrier_gate_enabled);
                        ImGui::Text("  confidence_hard_block_threshold:  %.4g",
                                    FPN_ToDouble(v.confidence_hard_block_threshold));
                        ImGui::Text("  held_out_fraction:                %.3f",
                                    FPN_ToDouble(v.held_out_fraction));
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

    // save current run — NULL = no finished run (the caller passes one only through RunControl_HasRun)
    if (current && current->stats.total_trades > 0) {
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
    SuiteJob job;   // Grid Search (D-506 — the funnel owns its start and its end; progress counts cells of total)
    char config_path[256];
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-02]
// [SIZE]_[383872B]
// [ALIGN]_[64]
// [CACHE_LINES]_[5998]
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
    // divergence. Pre-fix: OptimizerPanel defaulted to "engine.cfg" while RunControl_Init
    // defaulted to "backtest.cfg" — two suite-internal panels loaded DIFFERENT cfg files.
    // foxml_suite agent CRIT-3 finding 2026-05-24. Fix: 1-line "engine.cfg" → "backtest.cfg"
    // restores parity between suite panels. (Note: backtest.cfg/engine.cfg structural drift
    // closes separately at v5.15.6.A/B/C per TECH_DEBT-123.)
    strncpy(state->config_path, CFG_PATH_BACKTEST_CFG, sizeof(state->config_path) - 1);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OptimizerPanel_Init]
//======================================================================

//======================================================================
// [STRUCT]_[OptWorkerArgs]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[a Grid Search worker's own args — the panel it reports to and every input of its sweep, snapped at the click: the request, the ranges (their keys included), the parameter count and the metric; 64-aligned through BacktestRunConfig]
//======================================================================
// The sweep used to read the panel's ranges, count and metric while it ran, and those inputs stay editable during a
// run: changing an axis's key mid-sweep made the remaining cells set a different cfg field with values computed for
// the old one (Class 13). It reads only these now (D-507).
//======================================================================
// [CODE]
//======================================================================
struct OptWorkerArgs {
    OptimizerPanelState *state;
    BacktestRunConfig    request;
    OptimizerRange       ranges[OPT_MAX_PARAMS];
    int                  num_params;
    int                  metric_idx;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-02]
//----------------------------------------------------------------------
// [SIZE]_[578112B]
// [ALIGN]_[64]
// [CACHE_LINES]_[9033]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[OptWorkerArgs]
//======================================================================

//======================================================================
// [FUNCTION]_[optimizer_worker_fn]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[background thread: run a parameter sweep from its click-time args — they live until the sweep ends]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void *optimizer_worker_fn(void *arg, uint64_t lease) {
    OptWorkerArgs *args = (OptWorkerArgs *)arg;
    OptimizerPanelState *state = args->state;

    Backtest_RunSweep(lease, &state->results, &args->request,
                       args->ranges, args->num_params, args->metric_idx,
                       &state->job.progress, &state->job.total, &state->job.cancel);
    free(args);   // the sweep read them to its last cell

    SuiteJob_Publish(&state->job);
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
static inline void GUI_Panel_Optimizer(OptimizerPanelState *state, DataPanelState *data, LaunchFailureState *lf) {
    ImGui::Begin("Optimizer");

    // parameter config
    static const char *metric_names[] = {"Sharpe", "Profit Factor", "Expectancy", "Return %", "P&L $"};
    ImGui::Combo("Metric", &state->metric_idx, metric_names, 5);

    // AlwaysClamp: a Ctrl+Click typed value is clamped too — a 3 used to reach the range editors below and write
    // ranges[2], past the array (D-507 second review, R2)
    ImGui::SliderInt("Parameters", &state->num_params, 1, OPT_MAX_PARAMS, "%d", ImGuiSliderFlags_AlwaysClamp);

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
        // v5.10.0a — XGBoost hyperparam sweeping (cfg-bound since v5.9.5h).
        // Common operator targets:
        //   xgb_subsample / xgb_colsample_bytree → tree regularization
        //   xgb_min_child_weight → leaf-purity gate
        //   xgb_seed → reproducibility check (sweep multiple seeds, look at
        //              variance to detect overfit-to-seed)
        // (The thread counts left this list at OMP-B, D-494: XGBoost has no
        // OpenMP, so sweeping one filled the grid with identical cells.)
        "xgb_subsample", "xgb_colsample_bytree", "xgb_min_child_weight",
        "xgb_seed"
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

    const long long total_combos = (long long)state->ranges[0].steps() *
                                   (state->num_params > 1 ? state->ranges[1].steps() : 1);   // 64 bits: never wraps
    ImGui::Text("Total combinations: %lld", total_combos);

    ImGui::Separator();

    if (SuiteJob_Running(&state->job)) {
        const int done = state->job.progress, total = state->job.total;
        float pct = total > 0 ? (float)done / total : 0.0f;
        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%d / %d", done, total);
        ImGui::ProgressBar(pct, ImVec2(-1, 0), overlay);
        if (ImGui::Button("Cancel"))
            SuiteJob_Cancel(&state->job);
    } else {
        // its gate (D-507; Backtest/SuiteStartGates.hpp) — each axis checked on its own, so two inverted ranges can no
        // longer multiply to a positive count
        const SuiteGate gate = StartGate_GridSearch(DataPanel_SelectedCount(data), state->num_params, OPT_MAX_PARAMS,
                                                    state->ranges[0].steps(),
                                                    state->num_params > 1 ? state->ranges[1].steps() : 1,
                                                    OPT_MAX_STEPS, OPT_MAX_GRID);
        const bool can_run = SuiteGate_Open(&gate);
        if (!can_run) ImGui::BeginDisabled();
        if (ImGui::Button("Run Grid Search")) {
            // D-507 — every input of the sweep is snapped into the worker's own args (the request from the shared
            // builder — no bandit prior: a sweep starts uniform — the ranges with their keys, the count, the metric);
            // the panel's fields stay editable and a running sweep never reads them
            OptWorkerArgs *args = TrainingWorkers_AllocZeroed<OptWorkerArgs>();
            if (!args) {
                SuiteWorker_ReportNotStarted("Grid Search", START_CAUSE_NO_MEMORY, lf->launch_msg,
                                             sizeof(lf->launch_msg));
            } else {
                args->state = state;
                const int files = BacktestRunConfig_FromSelection(&args->request, data->files, data->selected,
                                                                  data->file_count, DATA_MAX_FILES,
                                                                  state->config_path, "", nullptr);
                memcpy(args->ranges, state->ranges, sizeof(args->ranges));
                args->num_params = state->num_params;
                args->metric_idx = state->metric_idx;
                // no files: refused here, as Run Backtest refuses (the gate already requires one); else the funnel
                // resets the job under the lease, or refuses naming the holder
                if (files == 0) {
                    SuiteWorker_ReportNotStarted("Grid Search", START_CAUSE_NO_FILES, lf->launch_msg,
                                                 sizeof(lf->launch_msg));
                    free(args);
                } else if (SuiteWorker_Launch("Grid Search", &state->job, optimizer_worker_fn, args, lf->launch_msg,
                                              sizeof(lf->launch_msg)) != SUITE_LAUNCH_STARTED) {
                    free(args);
                }
            }
        }
        if (!can_run) {
            ImGui::EndDisabled();
            SuiteGate_ShowWhy(&gate);
        }
    }

    // results
    if (SuiteJob_Done(&state->job) && state->results.total_runs > 0) {
        ImGui::Separator();
        OptimizerResults *r = &state->results;
        // MP-6 (D-503) — only the cells whose backtest ran are recorded; say how many, so a refused cell's empty
        // bar or heatmap square is never read as a result
        int ran = 0;
        for (int i = 0; i < r->total_runs; ++i) ran += r->cell_ran[i];
        if (ran < r->total_runs)
            ImGui::TextColored(FoxmlColors::yellow, "%d of %d cells ran — the rest were refused, failed or not "
                               "reached (see the log) and are never the best", ran, r->total_runs);

        // best result header
        int bi = r->best_idx;
        if (bi < 0) {
            ImGui::TextColored(FoxmlColors::red, "No cell ran — there is no best");
        } else {
            ImGui::TextColored(ResultsPnlColor(r->stats[bi].total_pnl),
                               "Best: %s=%.2f", r->keys[0],
                               r->param_vals[0][bi / r->dims[1]]);
            if (r->num_params > 1)
                ImGui::SameLine(), ImGui::Text(" %s=%.2f", r->keys[1],
                                                r->param_vals[1][bi % r->dims[1]]);
            ImGui::Text("P&L $%.2f  |  Sharpe %.2f  |  WR %.1f%%  |  PF %.2f",
                         r->stats[bi].total_pnl, r->stats[bi].sharpe_ratio,
                         r->stats[bi].win_rate, r->stats[bi].profit_factor);
        }

        // 1D: bar chart
        if (r->num_params == 1) {
            if (ImPlot::BeginPlot("Sweep", ImVec2(-1, 200))) {
                ImPlot::SetupAxes(r->keys[0], metric_names[r->metric_idx]);
                ImPlot::PlotBars("##metric", r->param_vals[0], r->metric, r->dims[0], 0.6);
                ImPlot::EndPlot();
            }
        }

        // 2D: heatmap
        if (r->num_params == 2) {
            if (ImPlot::BeginPlot("Heatmap", ImVec2(-1, 250))) {
                ImPlot::SetupAxes(r->keys[0], r->keys[1]);
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
            ImGui::TableSetupColumn(r->keys[0], ImGuiTableColumnFlags_WidthFixed, 70);
            if (r->num_params > 1)
                ImGui::TableSetupColumn(r->keys[1], ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("P&L", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("WR%", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("PF", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Sharpe", ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Trades", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableHeadersRow();

            // the cells that ran first, by metric (descending); the rest after them
            int sorted[OPT_MAX_GRID];
            for (int i = 0; i < r->total_runs; i++) sorted[i] = i;
            for (int i = 0; i < r->total_runs - 1; i++)
                for (int j = i + 1; j < r->total_runs; j++) {
                    const int a = sorted[i], b = sorted[j];
                    if (r->cell_ran[b] > r->cell_ran[a] ||
                        (r->cell_ran[b] == r->cell_ran[a] && r->metric[b] > r->metric[a])) {
                        sorted[i] = b; sorted[j] = a;
                    }
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
                if (!r->cell_ran[idx]) {   // P&L, WR, PF, Sharpe, Trades — none recorded
                    ImGui::TableNextColumn(); ImGui::TextDisabled("not run");
                    for (int c = 0; c < 4; ++c) { ImGui::TableNextColumn(); ImGui::TextDisabled("-"); }
                    continue;
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
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[state for the Training panel — every training / validation / multi-horizon knob and each worker's display flags]
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
    // identical training output. UI exposes these as advanced tuning; the
    // Train buttons, Walk-Forward and Run Full Validation snapshot them at the
    // click with max_depth/lr/n_estimators above (Training_SnapshotHyperparams).
    // The HP sweep does not read them (HPROUTE, D-505).
    float ui_subsample;          // 0.5-1.0
    float ui_colsample_bytree;   // 0.5-1.0
    int   ui_min_child_weight;   // clamped to the cfg registry's INT(...) bound for xgb_min_child_weight (read at render)
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
    // results
    char model_path[256];
    // the training run's status line — the run (its sink) writes it, the GUI reads it through TrainingStatusLine_Read:
    // a published snapshot, never torn while the run rewrites it (E.1.3 MP-6 step 10, S1-F10's sibling)
    TrainingStatusLine run_status;
    // walk-forward validation (Phase 6A — A7 GUI rework)
    int wf_n_splits;          // number of temporal folds (default 5)
    // s5 leaf-16: 0 = AUTO (max of the Horizons CSV, resolved at every use via
    // Training_ResolvePurgeHorizon); nonzero = explicit operator override.
    // NOT ini-persisted — it re-defaults every launch, which is exactly why the
    // old literal-1000 default silently leaked on every fresh session.
    int wf_horizon_ticks;     // label horizon for purge gap calc (0 = auto-derive)
    int wf_buffer_ticks;      // extra purge buffer (default 512)
    int wf_min_train;         // min training samples per fold (default 500)
    SuiteJob wf_job;          // Walk-Forward (D-506): progress is a percent; complete = wf_results readable (release / acquire)
    WalkForwardResults wf_results;
    // save run (bundles config + model for deployment) — a typed line on its own cache line (H6; TECH_DEBT-269's last
    // straddlers, D-507 call 5: aligned, no edit above it walks it across a line)
    alignas(64) char run_name[64];
    // v5.8.7 — Full Validation (held-out + auto-stamp). Replaces the
    // hand-wired multi-button workflow with a single integrated path
    // that exercises Backtest_RunFullValidation, which is the function
    // carrying the v5.8.6 auto-stamp wiring (FEATURE_REGISTRY_HASH +
    // engine_version embedded in stamp body).
    SuiteJob fv_job;                  // Run Full Validation (D-506): progress is a percent; complete = fv_results holds a finished run — published by the job (release), read with SuiteJob_Done (acquire), as MP-1b's flag was
    FullValidationResults fv_results;
    char fv_auto_stamp_secret[128];   // HMAC secret; empty = the collected cfg's auto_stamp_secret, dev mode only when that is empty too (CS-277)
    float fv_held_out_fraction;       // 0.05 .. 0.30; clamped by HeldOutSplit_Make
    float fv_gap_threshold;           // gap threshold for stamp accept/refuse
    TrainingStatusLine fv_status;     // post-run summary + auto-stamp result (the job writes it; read through TrainingStatusLine_Read)
    // v5.10.0a.E — Hyperparam Sweep state. Mirrors the wf_* worker
    // pattern. Operator clicks Run Hyperparam Sweep → spawn worker that
    // calls Backtest_RunHyperparamTrainSweep using already-collected
    // feature_matrix.
    OptimizerRange  hp_ranges[OPT_MAX_PARAMS];
    int             hp_num_params;          // 1 or 2 active params
    OptimizerResults hp_results;
    SuiteJob        hp_job;                 // the HP sweep (D-506): progress counts cells of total; complete = hp_results readable
    // v5.10.0a.G.1 — Multi-Horizon training state. Operator clicks Train
    // Multi-Horizon button; worker trains N models, one per horizon.
    // v5.10.0a-bugfix2 — horizons editable IN PANEL via CSV input
    // (operator no longer needs to edit cfg.horizon_list + reload).
    // ui_horizon_csv is the operator-typed string (e.g. "100,500,1000");
    // ui_horizon_list/_count are parsed on each render — the panel's ONE
    // source of horizons (the cfg.horizon_list fallback, which read the
    // last run's config while a collect rewrote it, went at E.1.3 MP-6
    // step 10.3).
    SuiteJob        mh_job;                 // both Train buttons (D-506): progress = horizons done (1..N) of total, set by the run
    volatile int    mh_current_horizon;     // current horizon ticks
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
    // misaligned and refuses both multi-horizon starts.
    //
    // Every collect / train request reads the parsed arrays below (one
    // value or more behind an open gate: TrainingPanel_ParseInputs seeds a
    // field holding none). label_tp_pct / label_sl_pct keep position 0's
    // last value — what a field emptied of its values takes back.
    // each typed CSV line on its own cache line (H6; TECH_DEBT-269's last straddlers, D-507 call 5) — each aligned, so
    // a field inserted between them cannot walk the second across a line
    alignas(64) char ui_tp_pct_csv[64];   // e.g. "0.030" or "0.020,0.030,0.040"
    alignas(64) char ui_sl_pct_csv[64];
    // alignas: the tp/sl pair fills exactly one 64B line (8+8 floats) — every change to this struct walked it across a
    // line (D-477, the MP-6 deletions, the step-7 jobs); aligned, it stays put. RunControlState's tp/sl echo pair is
    // the precedent (MP-6 step 7).
    alignas(64) float ui_tp_per_horizon[PANEL_HORIZON_MAX];   // parsed values (broadcast or positional)
    float           ui_sl_per_horizon[PANEL_HORIZON_MAX];
    int             ui_tp_per_horizon_count;  // 1 = broadcast; N = positional; 0 = the field cannot be applied (its parse refuses)
    int             ui_sl_per_horizon_count;
    // v5.11.41 — the per-horizon display of a multi-horizon run (one row per horizon, max
    // PANEL_HORIZON_MAX). The run (TrainingWorkers_RunMultiHorizon) writes these as its sink:
    // mh_horizon_status[h] is the live row text (read as a snapshot), mh_horizon_progress[h] the horizon's WF + held-out %
    // (0..100), and mh_horizon_complete[h] = 1 (release-published, read with TrainingSink_Load) once
    // that horizon finished or failed, after every file it writes. The FullValidationResults
    // themselves are no longer kept here: no panel code read them, and the run returns them in its
    // TrainingRunResult (E.1.3 MP-1).
    // alignas: the completion flags are cross-thread (released by the run, acquired by the GUI) — H6 by
    // construction, so a field added or deleted above can never walk them across a line again.
    alignas(64) volatile int mh_horizon_complete[PANEL_HORIZON_MAX];
    alignas(64) volatile int mh_horizon_progress[PANEL_HORIZON_MAX];  // H6 (Stage-5.5): cross-thread, was straddling a line
    // 256B per row with its sequence, 248 of text (was 128B, which clipped the skill-floor refuse reason
    // mid-word — "classification: majo" — hiding the operative half of the
    // message; the REFUSED format + full refuse string need ~200B headroom). A TrainingStatusLine: the
    // jobs rewrite a row while the GUI shows it, so the GUI reads a snapshot (S1-F10 — E.1.3 MP-6 step 10)
    TrainingStatusLine     mh_horizon_status[PANEL_HORIZON_MAX];
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
    // Broadcast-or-match rule mirrors ui_tp_pct_csv: empty = the Label Type
    // combo; a single value broadcasts itself (D-476); N values map
    // positionally where N == ui_horizon_count; misalignment refuses both
    // multi-horizon starts (StartGate_NeedAlignedKinds).
    //
    // Format: integer label_type values per LABEL_* enum (LabelFunctions.hpp).
    // Operator types e.g. "0,2,1" → horizon_0=binary, horizon_1=multi,
    // horizon_2=regression.
    alignas(64) char ui_label_kind_csv[64];   // its own cache line (H6; TECH_DEBT-269's last straddler, D-507 call 5)
    alignas(64) int ui_label_kind_per_horizon[PANEL_HORIZON_MAX];   // parsed (broadcast or positional); H6-aligned (Stage-5.5 straddle)
    int             ui_label_kind_per_horizon_count; // 0=empty; 1=broadcast; N=positional
    // D-477 — the collect-time feature mask (hex text + its parsed value; 0 = all-on).
    // At the TAIL on purpose: inserting it mid-struct shifted `ui_tp_per_horizon` onto a
    // 64B line boundary on this [THREAD]-tagged struct (the strict layout gate caught it).
    char     ui_feature_mask_hex[24];
    uint64_t ui_feature_mask;
    // E.1.3 MP-6 step 10.5 — what each CSV field's parse gave (TrainingPanel_ParseInputs, once a frame): a field that
    // cannot be applied as typed shows its red line and refuses the starts that read it. GUI thread only; at the TAIL, as
    // the mask above; a cache line each (SuiteCsvParse is 64 B) — the strict layout gate holds this [THREAD] struct
    // straddle-free (9.4's choice: alignment, not exemptions)
    alignas(64) SuiteCsvParse ui_horizon_parse;
    alignas(64) SuiteCsvParse ui_label_kind_parse;
    alignas(64) SuiteCsvParse ui_tp_parse;
    alignas(64) SuiteCsvParse ui_sl_parse;
};
// Over-aligned through its alignas(64) members: it lives in foxml_suite.cpp's static storage, never malloc / calloc
// (Check K — an over-aligned type from bare malloc is misaligned, UB).
static_assert(alignof(TrainingPanelState) == 64, "TrainingPanelState's alignment changed: re-check how it is allocated");
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-03]
// [SIZE]_[412672B]
// [ALIGN]_[64]
// [CACHE_LINES]_[6448]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[TrainingPanelState]
//======================================================================

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
static inline int Training_ResolvePurgeHorizon(const TrainingPanelState *st) {   // st: the panel's own — never NULL
    if (st->wf_horizon_ticks > 0) return st->wf_horizon_ticks;  // explicit override
    int mx = 0;
    for (int i = 0; i < st->ui_horizon_count
                    && i < TrainingPanelState::PANEL_HORIZON_MAX; ++i) {
        if (st->ui_horizon_list[i] > mx) mx = st->ui_horizon_list[i];
    }
    if (mx > 0) return mx;
    return LABEL_DEFAULT_FORWARD_TICKS;  // no horizons known — the label pass's own default
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Training_ResolvePurgeHorizon]
//======================================================================

//======================================================================
// [FUNCTION]_[Training_ResolvePurgeForLabels]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the purge a run that trains on the COLLECTED labels uses — the panel's (auto or explicit) widened, in auto, to the labels' own horizon (Label_PurgeCovering); Walk-Forward, the HP sweep and Full Validation read it at their click]
//======================================================================
// The Horizons CSV is the next collect's input and can be edited after one: shrink it from 15000 to 1000 and an auto
// purge of 1000 ran against 15000-tick labels — the leak Training_ResolvePurgeHorizon's own comment names (D-507 third
// review, A2). With no horizon typed and no override, the labels' own horizon IS the purge — the panel's side would
// otherwise be LABEL_DEFAULT_FORWARD_TICKS, a constant nobody chose that over-purges a short-horizon run's folds
// (E.1.3 MP-6 step 10.3's second review, finding 8 — the deleted label_forward_ticks used to stand there). Reads the
// run's record: call it at a click, or only while Run Control's outputs are at rest (RunControl_AtRest).
//======================================================================
// [CODE]
//======================================================================
static inline int Training_ResolvePurgeForLabels(const TrainingPanelState *st, const BacktestRunConfig *rec,
                                                 const BacktestResults *data) {
    const bool explicit_override = st->wf_horizon_ticks > 0;   // st: the panel's own — never NULL (every caller passes its own)
    const int  labels_horizon    = BacktestRunConfig_LabelsHorizon(rec, data ? data->sample_count : 0);
    if (!explicit_override && st->ui_horizon_count == 0 && labels_horizon > 0) return labels_horizon;
    return Label_PurgeCovering(Training_ResolvePurgeHorizon(st), explicit_override, labels_horizon);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Training_ResolvePurgeForLabels]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingPanel_CollectLabels]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the label params both collect buttons ask for — ONE copy where each click used to hand-copy six fields]
//======================================================================
// D-476 — the kind obeys the Label Kind CSV like every other click (position 0), as the field's own warning promises
// (TECH_DEBT-323); s5 leaf-15 — the fee rides with its siblings; D-477 — a feature mask of 0 means all on.
//======================================================================
// [CODE]
//======================================================================
static inline BacktestLabelRequest TrainingPanel_CollectLabels(const TrainingPanelState *state) {
    BacktestLabelRequest l{};
    l.label_type        = Label_ResolveKindForHorizon(state->ui_label_kind_per_horizon,
                                                      state->ui_label_kind_per_horizon_count, state->label_type, 0);
    l.forward_ticks     = state->ui_horizon_list[0];   // the CSV's first (a start's gate requires one); none typed = 0 (the parse zero-fills) = the label pass's default
    l.tp_pct            = state->ui_tp_per_horizon[0];   // position 0 — the field's own value (seeded if it held none)
    l.sl_pct            = state->ui_sl_per_horizon[0];
    l.roundtrip_fee_pct = state->label_roundtrip_fee_pct;
    l.feature_mask      = state->ui_feature_mask;
    return l;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingPanel_CollectLabels]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingPanel_ForgetDatasetResults]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[a started collect replaces the dataset: forget the results that describe it — the walk-forward's and the HP sweep's — and keep what describes a model: the per-horizon training table, the training status line, Full Validation's results (D-507 call 4)]
//======================================================================
// Called only once a collect has STARTED: the lease is the collect's then, so no job writes these results.
//======================================================================
// [CODE]
//======================================================================
static inline void TrainingPanel_ForgetDatasetResults(TrainingPanelState *state) {
    SuiteJob_Forget(&state->wf_job);
    SuiteJob_Forget(&state->hp_job);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingPanel_ForgetDatasetResults]
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
    state->ui_feature_mask_hex[0] = '\0';   // D-477 — empty = all features
    state->ui_feature_mask        = 0;
    state->label_type = LABEL_WIN_LOSS;
    state->label_tp_pct = 1.5f;
    state->label_sl_pct = 1.0f;
    // s5 leaf-15: 0 = fee-blind (bytewise-identical to pre-s5 labels). The
    // operator sets the venue's round trip explicitly — no silent default that
    // would change every existing run's labels on upgrade.
    state->label_roundtrip_fee_pct = 0.0f;
    // v5.11.40 — CSV-aware TP/SL per-horizon. Empty at start: the first
    // parse seeds each field from label_tp_pct / label_sl_pct (one value,
    // broadcast); comma-separated values map one per horizon.
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
    strncpy(state->model_path, "models/buy_signal.json", sizeof(state->model_path) - 1);
    // walk-forward defaults (FoxML battle-tested values)
    state->wf_n_splits = 5;
    // s5 leaf-16: 0 = AUTO (derive max(Horizons CSV) at use — see
    // Training_ResolvePurgeHorizon). The old literal 1000 default is what let a
    // 67.5k-tick label grid run a ~1.5k-tick purge gap and leak into test.
    state->wf_horizon_ticks = 0;
    state->wf_buffer_ticks = PURGE_BUFFER_DEFAULT;
    state->wf_min_train = 500;
    memset(&state->wf_results, 0, sizeof(state->wf_results));
    // v5.8.7 — full validation defaults (mirrors the cfg defaults so the
    // suite UI is usable out-of-the-box without editing engine.cfg).
    memset(&state->fv_results, 0, sizeof(state->fv_results));
    state->fv_auto_stamp_secret[0] = '\0';   // empty = the collected cfg's secret, dev mode only if that is empty too (CS-277)
    state->fv_held_out_fraction = 0.20f;      // matches HELDOUT_FRACTION default
    state->fv_gap_threshold = 0.05f;          // matches gap_acceptable_threshold default
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
    memset(&state->hp_results, 0, sizeof(state->hp_results));
    // v5.10.0a.G.1 — Multi-Horizon training state init
    state->mh_current_horizon = 0;
    memset(state->mh_horizon_ticks, 0, sizeof(state->mh_horizon_ticks));
    // v5.10.0a-bugfix2 — the operator types the horizons as a CSV; it starts
    // pre-filled with a suggestion that matches the original Idea #4 spec
    // example so operators see what shape the field expects (emptied, the
    // starts ask for a horizon — E.1.3 MP-6 step 10.3).
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
static inline void *walkforward_worker_fn(void *arg, uint64_t lease) {
    (void)lease;   // held (the funnel) for the dataset it reads (D-503); Walk-Forward runs no backtest and calls no core
                   // entry, so it passes the token nowhere — a headless caller holds the lease at its verb (E.2)
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
                             &state->wf_job.progress, &state->wf_job.cancel,
                             snap_label_type,
                             /*cfg_override=*/nullptr, /*hp_override=*/&snap_hp);

    SuiteJob_Publish(&state->wf_job);   // wf_results is readable — LAST, after every write to it
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
static inline void *hp_sweep_worker_fn(void *arg, uint64_t lease) {
    (void)lease;   // held (the funnel) for the dataset it reads (D-503); the HP sweep runs no backtest (CS-194) and calls
                   // no core entry, so it passes the token nowhere — a headless caller holds the lease at its verb (E.2)
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

    memset(&state->hp_results, 0, sizeof(state->hp_results));   // under the lease; the funnel reset the job's progress

#ifdef USE_XGBOOST
    Backtest_RunHyperparamTrainSweep(
        &state->hp_results, data, ranges, num_params,
        label_type,
        wf_n_splits, wf_horizon, wf_buffer, wf_min_train,
        &state->hp_job.progress, &state->hp_job.total,
        &state->hp_job.cancel);
    // readable only when the sweep produced cells — published LAST, after every write to hp_results
    if (state->hp_results.total_runs > 0) SuiteJob_Publish(&state->hp_job);
#else
    fprintf(stderr, "[hpsweep] XGBoost not compiled in — sweep skipped\n");
#endif
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[hp_sweep_worker_fn]
//======================================================================

// v5.8.7 — Run Full Validation's worker thread. Since E.1.3 MP-1b it is an adapter: the job
// (Backtest_RunFullValidation and its v5.8.6 auto-stamp wiring — FEATURE_REGISTRY_HASH +
// engine_version in the stamp body) is TrainingWorkers_RunFullValidation in Backtest/TrainingWorkers.hpp.
//======================================================================
// [STRUCT]_[FullValidationWorkerArgs]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[worker-thread args for Run Full Validation — the panel (wired as the job's sink) + the click-time TrainingFvRequest (capture-at-click defeats the ImGui edit race, v5.10.0E)]
//======================================================================
// [CODE]
//======================================================================
struct FullValidationWorkerArgs {
    TrainingPanelState *state;      // the panel — wired as the job's sink by the worker
    TrainingFvRequest   req;        // the click-time request: every input of the job (MP-1b)
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [UPDATED]_[2026-09-30]
// [SIZE]_[536B]
// [ALIGN]_[8]
// [CACHE_LINES]_[9]
// [STRADDLE]_[none]
// [ORIGIN]_[AUTO]
//======================================================================
// [END_STRUCT]_[FullValidationWorkerArgs]
//======================================================================

//======================================================================
// [FUNCTION]_[fullvalidation_worker_fn]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[background thread (a SuiteWorkerFn, under the lease): the GUI adapter — clears the job's status line, wires the panel in as the job's sink and runs the click-time request through TrainingWorkers_RunFullValidation (WF + held-out gap + the re-stamp)]
//======================================================================
// [CODE]
//======================================================================
static inline void *fullvalidation_worker_fn(void *arg, uint64_t lease) {
    FullValidationWorkerArgs *args = (FullValidationWorkerArgs *)arg;
    TrainingPanelState *state = args->state;
    // the job's status line clears HERE, under the lease — a refused click never reaches it, so the last run's
    // summary stays beside its results (MP-6 step 8)
    TrainingStatusLine_Set(&state->fv_status, "");
    TrainingFvSink sink{};
    sink.status     = &state->fv_status;
    sink.progress   = &state->fv_job.progress;
    sink.cancel     = &state->fv_job.cancel;
    sink.complete   = &state->fv_job.complete;
    // the lease's refusal would be the funnel's invariant broken (LOUD); any other the core has already said, visibly
    TrainingWorkers_WorkerSaw("Run Full Validation's worker",
                              TrainingWorkers_RunFullValidation(lease, args->req, sink, &state->fv_results), lease);
    free(args);
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[fullvalidation_worker_fn]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingPanel_FullValidationArgs]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Run Full Validation's click-time request builder — the model path + its role, the one secret rule, the labels' identity from the run config, the WF + gate params and the hyperparameters from the panel; NULL on allocation failure]
//======================================================================
// [CODE]
//======================================================================
static inline FullValidationWorkerArgs *TrainingPanel_FullValidationArgs(TrainingPanelState *state,
                                                                         RunControlState *run_control,
                                                                         const BacktestResults *fv_data) {
    FullValidationWorkerArgs *a = TrainingWorkers_AllocZeroed<FullValidationWorkerArgs>();
    if (!a) return NULL;
    a->state = state;
    TrainingFvRequest &r = a->req;
    r = TrainingFvRequest{};
    // The model, its role, the secret and the labels' identity — through the core's GUI-free builder,
    // so the cells drive the very code that fills these (MP-1b review F2: CS-274 and CS-277 lived here).
    TrainingWorkers_FvRequestIdentity(&r, state->model_path, sizeof(state->model_path),
                                      state->fv_auto_stamp_secret, sizeof(state->fv_auto_stamp_secret),
                                      fv_data, &run_control->run_config);
    // The rest from the panel at click time.
    r.wf_n_splits       = state->wf_n_splits;
    r.wf_horizon_ticks  = Training_ResolvePurgeForLabels(state, &run_control->run_config, fv_data);   // s5 leaf-16 + A2
    r.wf_buffer_ticks   = state->wf_buffer_ticks;
    r.wf_min_train      = state->wf_min_train;
    r.gap_threshold     = state->fv_gap_threshold;
    r.held_out_fraction = state->fv_held_out_fraction;
    // E.1.2.C follow-up — the same click-time snapshot the Train path builds, so BOTH entry points
    // describe one architecture.
    r.hp = Training_SnapshotHyperparams(state);
    r.now_us = TrainingWorkers_WallClockUs();   // CS-273 — the run's clock, taken at the click
    return a;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingPanel_FullValidationArgs]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingPanel_LaunchFullValidation]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Run Full Validation's launch — build the click-time request and start the worker through the suite's funnel; a refusal or an allocation failure changes nothing, a thread-start failure ends the job the funnel began; each is said in the suite's one launch-failure modal (LaunchFailure_Modal) and the log, never a crash or a stuck "running"]
//======================================================================
// [CODE]
//======================================================================
static inline void TrainingPanel_LaunchFullValidation(TrainingPanelState *state,
                                                      RunControlState *run_control,
                                                      const BacktestResults *fv_data, LaunchFailureState *lf) {
    FullValidationWorkerArgs *fv_args = TrainingPanel_FullValidationArgs(state, run_control, fv_data);
    if (!fv_args) {
        SuiteWorker_ReportNotStarted("Run Full Validation", START_CAUSE_NO_MEMORY, lf->launch_msg,
                                     sizeof(lf->launch_msg));
        return;
    }
    // the funnel takes the lease and owns the job's start and end: a refusal (it names the holder) changes nothing;
    // a thread that does not start ends the job the funnel began (its last results hide, as the old click did);
    // either says why in the suite's one launch-failure window (D-507)
    if (SuiteWorker_Launch("Run Full Validation", &state->fv_job, fullvalidation_worker_fn, fv_args, lf->launch_msg,
                           sizeof(lf->launch_msg)) != SUITE_LAUNCH_STARTED)
        free(fv_args);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingPanel_LaunchFullValidation]
//======================================================================

// D-d (2026-08-22, operator-decided) — train_model_worker_fn + TrainModelWorkerArgs DELETED (~470 lines).
// Dead since v5.11.44 routed Train Model through the multi-horizon worker: zero
// pthread_create sites tree-wide (scan-1 NEW-3 / scan-2 NEW-4/W2, both re-derived
// 2026-08-22). It was compiled-in dead capital-adjacent code (H21 discipline) that
// DIVERGED from the live path (it neutral-filtered + class-weighted where the live
// trainer does not — scan-1 NEW-2's evidence), held the only
// FeatureStandardizer_Persist caller (the .scaler capability is hereby dormant-by-
// decision), and kept `model_trained` semantics alive (S1-F8). The expected.cfg
// producer — the one part worth keeping — was PORTED into mh_run_one_horizon_fv (now
// TrainingWorkers_RunHorizon, Backtest/TrainingWorkers.hpp — E.1.3 MP-1).


//======================================================================================================
// [v5.10.0a.G.1 — MULTI-HORIZON TRAINING WORKER]  (the GUI adapter since E.1.3 MP-1)
//======================================================================================================
// Trains N models, one per horizon in the panel's Horizons CSV, sharing the
// feature_matrix collected once. One batched pass labels every horizon (E.1.2.D leaf 5); each
// horizon then trains an XGBooster on (features, its labels), saves it to a per-horizon dir, and
// runs WF + held-out validation, which emits its stamp. The run itself lives in
// Backtest/TrainingWorkers.hpp (TrainingWorkers_RunMultiHorizon); this section is the panel's side
// of it — the click-time request builder both Train buttons use, the launcher, and the worker
// thread that wires the panel in as the run's sink.
//
// Per-horizon save path (D-431 nested): models/<class>/<run>/horizon_<H>/<role>.json
// Per-horizon summary:                    .../horizon_<H>/summary_{entry|exit}.txt (D-e)
//
// Operator workflow:
//   1. Type the horizons into the panel's Horizons CSV (e.g. 100,500,1000)
//   2. Click Collect Multi-Horizon (one collect labels every horizon)
//   3. Click Train Multi-Horizon — N models trained (serial, or in parallel per
//      multi_horizon_max_threads; each booster is single-threaded — XGBoost has no OpenMP, D-494)
//   4. The engine serves the trained horizons as an ensemble — it discovers
//      them from the deploy dir on disk (G.5); no cfg list is involved
//
// LITE caveats:
//   - No ensemble training-time discipline check — operator must
//     manually pick which to deploy OR rely on G.4 ensemble inference
//   - Save Run for multi-horizon: writes per-horizon dirs; Past Runs
//     panel rescan picks them up as N separate rows
//
// Click-time snapshot mirrors v5.10.0E pattern — since MP-1 the snapshot IS the run's request, the
// run config included, taken on the GUI thread.
//======================================================================================================
//======================================================================
// [STRUCT]_[MultiHorizonWorkerArgs]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[worker-thread args for the multi-horizon training run — the panel (wired as the run's sink) + the click-time TrainingRunRequest + the run-config snapshot the request points at]
//======================================================================
// [CODE]
//======================================================================
struct MultiHorizonWorkerArgs {
    TrainingPanelState *state;      // the panel — wired as the run's sink by the worker
    TrainingRunRequest  req;        // the click-time request: every input of the run (MP-1)
    BacktestRunConfig   run_cfg;    // the click-time run-config snapshot req.run_cfg points at (F3 — taken on the GUI thread)
};
// The embedded BacktestRunConfig makes the args 64-aligned, so they come from TrainingWorkers_AllocZeroed,
// never malloc / calloc (MP-1a review F1).
static_assert(alignof(MultiHorizonWorkerArgs) == 64,
              "MultiHorizonWorkerArgs' alignment changed: re-check how it is allocated");
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [UPDATED]_[2026-09-30]
// [SIZE]_[578624B]
// [ALIGN]_[64]
// [CACHE_LINES]_[9041]
// [STRADDLE]_[none]
// [ORIGIN]_[AUTO]
//======================================================================
// [END_STRUCT]_[MultiHorizonWorkerArgs]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingPanel_MultiHorizonArgs]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE click-time request builder for both Train buttons — every input of the run snapped on the GUI thread, the run config included; NULL on allocation failure]
//======================================================================
// [CODE]
//======================================================================
static inline MultiHorizonWorkerArgs *TrainingPanel_MultiHorizonArgs(TrainingPanelState *state,
                                                                     RunControlState *run_control,
                                                                     int horizon_count,
                                                                     const int *horizons) {
    MultiHorizonWorkerArgs *a = TrainingWorkers_AllocZeroed<MultiHorizonWorkerArgs>();
    if (!a) return NULL;
    a->state   = state;
    a->run_cfg = run_control->run_config;    // F3 — the worker never reads the shared one
    TrainingRunRequest &r = a->req;
    r = TrainingRunRequest{};
    r.data    = &run_control->results;
    r.run_cfg = &a->run_cfg;
    {
        size_t n = strnlen(state->run_name, sizeof(state->run_name));
        if (n >= sizeof(r.run_name)) n = sizeof(r.run_name) - 1;
        memcpy(r.run_name, state->run_name, n);
        r.run_name[n] = '\0';
    }
    snprintf(r.models_root, sizeof(r.models_root), "%s", "models");
    r.primary_label_type = Label_ResolveKindForHorizon(   // D-476: CSV position 0 wins
        state->ui_label_kind_per_horizon,
        state->ui_label_kind_per_horizon_count, state->label_type, 0);
    // v5.13.1.A — the side, snapped at click time (single-horizon too, v5.13.5.A).
    r.training_side = state->ui_training_side;
    r.horizon_count = horizon_count;
    // v5.11.40 — per-horizon TP/SL by the broadcast-or-match rule: one value broadcasts, N map
    // positionally (behind an open gate each list holds one value or more — TrainingPanel_ParseInputs
    // seeds a field holding none). D-476 — ONE label-kind rule for every click
    // (Label_ResolveKindForHorizon). Slots past horizon_count are never read.
    const float bcast_tp = state->ui_tp_per_horizon[0];
    const float bcast_sl = state->ui_sl_per_horizon[0];
    for (int i = 0; i < ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX; ++i) {
        const int live = (i < horizon_count);
        r.horizon_ticks[i] = live ? horizons[i] : 0;
        r.tp_pct[i] = (state->ui_tp_per_horizon_count > 1 && i < state->ui_tp_per_horizon_count)
            ? state->ui_tp_per_horizon[i] : bcast_tp;
        r.sl_pct[i] = (state->ui_sl_per_horizon_count > 1 && i < state->ui_sl_per_horizon_count)
            ? state->ui_sl_per_horizon[i] : bcast_sl;
        r.label_type[i] = live ? Label_ResolveKindForHorizon(
                                     state->ui_label_kind_per_horizon,
                                     state->ui_label_kind_per_horizon_count, state->label_type, i)
                               : 0;
    }
    // E.1.2.C — the hyperparameters through the panel's ONE mapping (the WF and FV clicks use it too).
    r.hp = Training_SnapshotHyperparams(state);
    // v5.11.41 — the WF + held-out params. v5.11.47 — the secret falls back to cfg.auto_stamp_secret
    // when the GUI field is empty (set it once in cfg instead of re-typing it every session).
    r.wf_n_splits       = state->wf_n_splits;
    r.wf_buffer_ticks   = state->wf_buffer_ticks;
    r.wf_min_train      = state->wf_min_train;
    r.gap_threshold     = state->fv_gap_threshold;
    r.held_out_fraction = state->fv_held_out_fraction;
    // CS-277 — the one secret rule, shared with Run Full Validation.
    TrainingWorkers_ResolveStampSecret(state->fv_auto_stamp_secret, sizeof(state->fv_auto_stamp_secret),
                                       run_control->results.config_used.auto_stamp_secret,
                                       sizeof(run_control->results.config_used.auto_stamp_secret),
                                       r.stamp_secret, sizeof(r.stamp_secret));
    // F16 — the serial / parallel choice is an explicit request field (its value is still the
    // collect-time cfg's, as it always was).
    r.max_threads = run_control->results.config_used.multi_horizon_max_threads;
    r.now_us      = TrainingWorkers_WallClockUs();   // CS-273 — the run's clock, taken once at the click
    return a;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingPanel_MultiHorizonArgs]
//======================================================================

//======================================================================
// [FUNCTION]_[train_multi_horizon_worker_fn]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[background thread: the GUI adapter — wires the panel in as the run's sink and runs the click-time request through TrainingWorkers_RunMultiHorizon]
//======================================================================
// [REFERENCE]_[PARITY]_[PARITY-21]
//======================================================================
// [CODE]
//======================================================================
static inline void *train_multi_horizon_worker_fn(void *arg, uint64_t lease) {
    MultiHorizonWorkerArgs *args = (MultiHorizonWorkerArgs *)arg;
    TrainingPanelState *state = args->state;
    // the job's display that SuiteJob does not hold resets HERE, under the lease (the core clears the per-horizon
    // rows and holds the run's total at 0 until it accepts the run) — a refused click never reaches it (MP-6 step 8)
    TrainingStatusLine_Set(&state->run_status, "");
    state->mh_current_horizon = 0;

    TrainingRunSink sink{};
    sink.status     = &state->run_status;
    sink.cancel     = &state->mh_job.cancel;
    sink.total      = &state->mh_job.total;
    sink.current    = &state->mh_current_horizon;
    sink.done       = &state->mh_job.progress;
    sink.complete   = nullptr;   // no reader wants a whole-run "done": the table renders on total, each row on its own
                                 // mh_horizon_complete[h] — the D-504 trace found mh_complete write-only (D-505: NULL)
    for (int h = 0; h < TrainingPanelState::PANEL_HORIZON_MAX; ++h) {
        sink.horizon[h].status     = &state->mh_horizon_status[h];
        sink.horizon[h].progress   = &state->mh_horizon_progress[h];
        sink.horizon[h].complete   = &state->mh_horizon_complete[h];
    }

    // The run's per-horizon validation results come back here. The panel shows its rows from the
    // sink's status lines, so the result is released once the run is done.
    TrainingRunResult *result = TrainingWorkers_AllocZeroed<TrainingRunResult>();
    if (!result) {
        TrainingSink_Status(sink.status, "Multi-horizon: out of memory (run result).");
        TrainingSink_FinishRun(sink);
    } else {
        // the lease's refusal would be the funnel's invariant broken (LOUD); a held model family the core has already
        // said on the run line, which the panel draws once the job ends (the run sink's `complete` is NULL here)
        TrainingWorkers_WorkerSaw("Train Multi-Horizon's worker",
                                  TrainingWorkers_RunMultiHorizon(lease, args->req, sink, result), lease);
        free(result);
    }
    free(args);
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[train_multi_horizon_worker_fn]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingPanel_LaunchMultiHorizon]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[both Train buttons' launch — build the click-time request and start the worker through the suite's funnel (true = started; the per-horizon table's horizons snap only then); a refusal or an allocation failure changes nothing, a thread-start failure ends the job the funnel began; each is said in the suite's one launch-failure modal (LaunchFailure_Modal) and the log]
//======================================================================
// [CODE]
//======================================================================
static inline bool TrainingPanel_LaunchMultiHorizon(TrainingPanelState *state,
                                                    RunControlState *run_control, const char *name,
                                                    int horizon_count, const int *horizons, LaunchFailureState *lf) {
    MultiHorizonWorkerArgs *mh_args =
        TrainingPanel_MultiHorizonArgs(state, run_control, horizon_count, horizons);
    if (!mh_args) {
        SuiteWorker_ReportNotStarted(name, START_CAUSE_NO_MEMORY, lf->launch_msg, sizeof(lf->launch_msg));
        return false;
    }
    // the funnel takes the lease and owns the job's start and end: a refusal (it names the holder) changes nothing;
    // a thread that does not start ends the job the funnel began (its last results hide, as the old click did);
    // either says why in the suite's one launch-failure window (D-507)
    if (SuiteWorker_Launch(name, &state->mh_job, train_multi_horizon_worker_fn, mh_args, lf->launch_msg,
                           sizeof(lf->launch_msg)) != SUITE_LAUNCH_STARTED) {
        free(mh_args);
        return false;
    }
    // E.1.2.C GUI polish (a) — the per-horizon table's horizons, snapped for the run that STARTED (GUI thread only: the
    // render never reads the live-reparsed ui_horizon_list, and a refused click must not relabel the last run's rows;
    // the arrays are sized PANEL_HORIZON_MAX = HORIZON_LIST_MAX since E.1.2.D leaf 13, so they track the grid)
    for (int i = 0; i < TrainingPanelState::PANEL_HORIZON_MAX; ++i)
        state->mh_horizon_ticks[i] = (i < horizon_count) ? horizons[i] : 0;
    return true;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingPanel_LaunchMultiHorizon]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingPanel_ParseHorizonCsv]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the panel's Horizons CSV → ui_horizon_list / ui_horizon_count / ui_horizon_parse (at most PANEL_HORIZON_MAX, each 1..MODEL_HORIZON_TICKS_MAX — the largest a model path names — and each distinct: a horizon is a model directory; it STOPS at the first value it cannot keep and says so — SuiteCsv_Parse, SuiteCsv_StopAtRepeat) — the ONE source of the panel's horizons (E.1.3 MP-6 step 10.3: no fallback to the last run's config)]
//======================================================================
// Called by TrainingPanel_ParseInputs — ONCE a frame, at the panel's top, before anything reads the horizons: the mode,
// every gate and every click read one parse, and an edit typed into the CSV reaches them the next frame. (The panel
// parsed twice — two byte-identical copies, the second after the input field — so for the frame of an edit the mode
// and the Train gate could disagree.)
//======================================================================
// [CODE]
//======================================================================
static inline void TrainingPanel_ParseHorizonCsv(TrainingPanelState *state) {
    state->ui_horizon_parse = SuiteCsv_Parse(state->ui_horizon_csv, state->ui_horizon_list,
                                             TrainingPanelState::PANEL_HORIZON_MAX, [](int, double *lo, double *hi) {
                                                 *lo = 1.0;
                                                 *hi = MODEL_HORIZON_TICKS_MAX;
                                             });
    SuiteCsv_StopAtRepeat(&state->ui_horizon_parse, state->ui_horizon_list, state->ui_horizon_csv);
    state->ui_horizon_count = state->ui_horizon_parse.count;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingPanel_ParseHorizonCsv]
//======================================================================

//======================================================================
// [FUNCTION]_[Training_TrainedKind]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the label kind horizon i of a collect or train start labels — D-476's one rule (Label_ResolveKindForHorizon) over the parsed Label Kind CSV and the combo — and, Training_TrainedHorizons, how many horizons a start labels (the parsed horizons; one in single mode)]
//======================================================================
// [CODE]
//======================================================================
static inline int Training_TrainedHorizons(const TrainingPanelState *st) {
    return st->ui_horizon_count > 1 ? st->ui_horizon_count : 1;
}
static inline int Training_TrainedKind(const TrainingPanelState *st, int i) {
    return Label_ResolveKindForHorizon(st->ui_label_kind_per_horizon, st->ui_label_kind_per_horizon_count,
                                       st->label_type, i);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Training_TrainedKind]
//======================================================================

//======================================================================
// [FUNCTION]_[Training_SlotUnit]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the unit the TP (tp) or SL slot's values carry across the horizons a start labels — the slot kind they all share, an unused slot counting as a percent (the stamp records its value raw, as one); TRAINING_SLOT_NONE when no labelled horizon's kind uses the slot, TRAINING_SLOT_MIXED when their units differ — and Training_SlotCaption, the field's caption for it]
//======================================================================
// E.1.3 MP-6 step 10.5's review (F1, F2, F4): which kind the TP / SL fields describe had three answers — the captions
// took the combo's label first (wrong whenever the Label Kind CSV overrides it: D-476), the fields' visibility took the
// kinds the horizons resolve to, and one value was range-checked against horizon 0's kind and applied to every horizon.
// One answer now, over the kinds the start labels: the captions, the fields' visibility, and the refusal of one value
// for horizons whose units differ all read it. An unused slot is not ignored downstream: until the locked recipe's S2,
// the stamp records its value raw as a percent bracket (Label_StampTpPct's pass-through; PARITY-065 item 4), so beside
// a percent kind it is the same unit, and beside a sigma kind a different one.
//======================================================================
// [CODE]
//======================================================================
enum : int { TRAINING_SLOT_NONE = -1, TRAINING_SLOT_MIXED = -2 };
static inline int Training_SlotUnit(const TrainingPanelState *st, bool tp) {
    const int unused = tp ? (int)TP_UNUSED : (int)SL_UNUSED;
    const int pct    = tp ? (int)TP_PCT : (int)SL_PCT;
    bool      used   = false;
    int       unit   = TRAINING_SLOT_NONE;
    for (int i = 0; i < Training_TrainedHorizons(st); ++i) {
        const int k    = Training_TrainedKind(st, i);
        const int kind = (k >= 0 && k < LABEL_COUNT) ? (tp ? (int)label_table[k].tp_kind : (int)label_table[k].sl_kind)
                                                     : unused;
        used         = used || kind != unused;
        const int u  = kind == unused ? pct : kind;
        if (unit == TRAINING_SLOT_NONE) unit = u;
        else if (u != unit) return TRAINING_SLOT_MIXED;
    }
    return used ? unit : TRAINING_SLOT_NONE;
}
static inline const char *Training_SlotCaption(int unit, bool tp) {
    if (unit == TRAINING_SLOT_MIXED) return tp ? "TP (units differ per horizon)" : "SL (units differ per horizon)";
    return tp ? Label_TpKindCaption(unit) : Label_SlKindCaption(unit);   // TRAINING_SLOT_NONE: "… (unused by this label)"
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Training_SlotUnit]
//======================================================================

//======================================================================
// [FUNCTION]_[Training_SlotFieldsShown]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[do the TP / SL fields render — when a labelled horizon's kind uses a slot, and, whatever the kinds, when a start refuses on one of them (Training_SlotInError: a field that cannot be applied as typed; in multi-horizon mode, a count that neither broadcasts nor matches the horizons) — a refusal never names a field that is not on screen; and Training_SlotTip, the tooltip of a field whose unit one word cannot name (units differ per horizon; unused), worded by why it shows]
//======================================================================
// Clean and unused, they stay hidden (D-473). Hidden, they are still not ignored — the stamp records an unused slot's
// value raw (Training_SlotUnit) — which is why a value in error refuses even then, and shows itself to be fixed.
// (E.1.3 MP-6 step 10.5's review, F2: with the Label Kind CSV giving every horizon a slot-less kind, the fields hid
// while the multi-horizon gates still refused on their counts — a reason naming a field not on screen. Its second
// review, N4: an unused slot shown beside the other one said a start refused on it when nothing did.)
//======================================================================
// [CODE]
//======================================================================
static inline bool Training_SlotInError(const TrainingPanelState *st, bool tp) {
    const SuiteCsvParse &r = tp ? st->ui_tp_parse : st->ui_sl_parse;
    const int            n = tp ? st->ui_tp_per_horizon_count : st->ui_sl_per_horizon_count;
    return r.stop != SUITE_CSV_OK || (st->ui_horizon_count > 1 && !StartGate_BroadcastsOrMatches(n, st->ui_horizon_count));
}
static inline bool Training_SlotFieldsShown(const TrainingPanelState *st) {
    return Training_SlotUnit(st, true) != TRAINING_SLOT_NONE || Training_SlotUnit(st, false) != TRAINING_SLOT_NONE ||
           Training_SlotInError(st, true) || Training_SlotInError(st, false);
}
static inline const char *Training_SlotTip(const TrainingPanelState *st, bool tp) {
    const int unit = Training_SlotUnit(st, tp);
    if (unit == TRAINING_SLOT_MIXED)
        return "UNITS DIFFER PER HORIZON — the labels the horizons train (Label Kind CSV) read this slot in\n"
               "different units: a percent of price for a barrier label, sigmas (k) or a sigma window in ticks\n"
               "for a Vol Barrier label; a horizon whose label ignores the slot reads it as a percent (the\n"
               "model stamp records it as a percent bracket). Give one value per horizon, each in its\n"
               "horizon's unit — a single value is refused, except 0, which means the default in every unit.";
    if (unit != TRAINING_SLOT_NONE) return nullptr;   // one unit: the kind's own words
    return Training_SlotInError(st, tp)
        ? "No horizon's label uses this slot, but the model stamp still records its value (as a percent\n"
          "bracket), so the starts refuse while it is in error — see the red line, or the start's reason.\n"
          "Type a value in range: one for every horizon, or one per horizon."
        : "No horizon's label uses this slot; it shows beside the other slot, which one does. The model\n"
          "stamp still records its value (as a percent bracket).";
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Training_SlotFieldsShown]
//======================================================================

//======================================================================
// [FUNCTION]_[Training_SideVerdict]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the training side's verdict on the labels a start labels — the worst tier (Training_SideLabelGate: 0 refuse, 1 warn, 2 ok) over the kinds its horizons resolve to (Training_TrainedKind) — and the kind that sets it (*kind, for the line that names it)]
//======================================================================
// E.1.2.C (F3): the verdict read the combo alone, so a Label Kind CSV carried a REFUSE-tier kind in behind it; then it
// read the combo and every CSV entry. It reads exactly the kinds the start labels now (E.1.3 MP-6 step 10.5's review,
// I4): the combo only where no CSV overrides it (D-476), a CSV entry past the horizons never — a kind nothing labels
// refuses nothing, and every kind something labels is read.
//======================================================================
// [CODE]
//======================================================================
static inline int Training_SideVerdict(const TrainingPanelState *st, int *kind) {
    int worst   = Training_TrainedKind(st, 0);
    int verdict = Training_SideLabelGate(worst, st->ui_training_side);
    for (int i = 1; i < Training_TrainedHorizons(st); ++i) {
        const int k = Training_TrainedKind(st, i);
        const int t = Training_SideLabelGate(k, st->ui_training_side);
        if (t < verdict) {
            verdict = t;
            worst   = k;
        }
    }
    if (kind) *kind = worst;
    return verdict;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Training_SideVerdict]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingPanel_ParseInputs]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[parse the panel's four CSV fields ONCE a frame, at its top, before anything reads them — the horizons, the label kinds, then TP and SL (a field holding no value seeded first with position 0's last value; each position's range its horizon's kind's; one value for horizons whose units differ refused, 0 excepted) — each stopping at the first value it cannot keep; label_tp_pct / label_sl_pct keep position 0's last value]
//======================================================================
// E.1.3 MP-6 step 10.5: the kinds and the slots were parsed inside the TP / SL render block — so a combo label with no
// slot left their counts from the last frame the block drew, still driving collect and train — and each field had its
// own loop, two of which dropped a bad value and shifted the rest onto the wrong horizon. One parser, one place.
//======================================================================
// [CODE]
//======================================================================
static inline void TrainingPanel_ParseInputs(TrainingPanelState *state) {
    TrainingPanel_ParseHorizonCsv(state);
    state->ui_label_kind_parse = SuiteCsv_Parse(state->ui_label_kind_csv, state->ui_label_kind_per_horizon,
                                                TrainingPanelState::PANEL_HORIZON_MAX, [](int, double *lo, double *hi) {
                                                    *lo = 0.0;
                                                    *hi = LABEL_COUNT - 1;
                                                });
    state->ui_label_kind_per_horizon_count = state->ui_label_kind_parse.count;
    // a TP / SL field holding no value — empty, or only separators (", " is what deleting the numbers from "0.5, 1.5"
    // leaves) — takes position 0's last value (label_tp_pct / label_sl_pct) BEFORE it parses, so what a start applies is
    // always a value on the field, through every check below: behind an open gate each list holds one value or more.
    // (It was seeded only when the fields drew, then only when the text was empty: a field of separators parsed to
    // nothing and the starts broadcast the stored value unchecked — the step's second review, N1.)
    if (!SuiteCsv_HasValue(state->ui_tp_pct_csv))
        SuiteCsv_WriteValue(state->ui_tp_pct_csv, sizeof(state->ui_tp_pct_csv), state->label_tp_pct);
    if (!SuiteCsv_HasValue(state->ui_sl_pct_csv))
        SuiteCsv_WriteValue(state->ui_sl_pct_csv, sizeof(state->ui_sl_pct_csv), state->label_sl_pct);
    // a position's legal range is its horizon's kind's: a barrier is a percent (no-margin cap 100), a sigma window a
    // tick count (up to LABEL_VOL_WINDOW_MAX) — the 2026-09-02 rule
    auto slot_range = [state](bool tp) {
        return [state, tp](int i, double *lo, double *hi) {
            const int  k    = Training_TrainedKind(state, i);
            const bool k_ok = k >= 0 && k < LABEL_COUNT;
            *lo = 0.0;
            *hi = tp ? Label_TpSlotMax(k_ok ? label_table[k].tp_kind : TP_PCT)
                     : Label_SlSlotMax(k_ok ? label_table[k].sl_kind : SL_PCT);
        };
    };
    state->ui_tp_parse = SuiteCsv_Parse(state->ui_tp_pct_csv, state->ui_tp_per_horizon,
                                        TrainingPanelState::PANEL_HORIZON_MAX, slot_range(true));
    state->ui_sl_parse = SuiteCsv_Parse(state->ui_sl_pct_csv, state->ui_sl_per_horizon,
                                        TrainingPanelState::PANEL_HORIZON_MAX, slot_range(false));
    // one value for horizons whose units differ is a percent at one and sigmas or ticks at another — refused, one per
    // horizon asked (the step's review, F4: it was range-checked against horizon 0's kind alone, then applied to all).
    // 0 alone may stand for all: every label path and the stamp read 0 as "this kind's default" (MIXED needs two
    // horizons, so one horizon never refuses)
    if (state->ui_tp_parse.stop == SUITE_CSV_OK && state->ui_tp_parse.count == 1 && state->ui_tp_per_horizon[0] != 0.0f &&
        Training_SlotUnit(state, true) == TRAINING_SLOT_MIXED)
        SuiteCsv_RefuseBroadcast(&state->ui_tp_parse, state->ui_tp_pct_csv);
    if (state->ui_sl_parse.stop == SUITE_CSV_OK && state->ui_sl_parse.count == 1 && state->ui_sl_per_horizon[0] != 0.0f &&
        Training_SlotUnit(state, false) == TRAINING_SLOT_MIXED)
        SuiteCsv_RefuseBroadcast(&state->ui_sl_parse, state->ui_sl_pct_csv);
    state->ui_tp_per_horizon_count = state->ui_tp_parse.count;
    state->ui_sl_per_horizon_count = state->ui_sl_parse.count;
    // label_tp_pct / _sl_pct track position 0's last value — what a field emptied of its values takes back (above)
    if (state->ui_tp_per_horizon_count > 0) state->label_tp_pct = state->ui_tp_per_horizon[0];
    if (state->ui_sl_per_horizon_count > 0) state->label_sl_pct = state->ui_sl_per_horizon[0];
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingPanel_ParseInputs]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingPanel_CsvError]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the first CSV field a collect or train start reads that cannot be applied as typed — the horizons, the label kinds, TP, SL (a TP / SL field in error shows itself: Training_SlotFieldsShown) — or nullptr; the four starts' gates take it (StartGate_NeedCsvs)]
//======================================================================
// [CODE]
//======================================================================
static inline const char *TrainingPanel_CsvError(const TrainingPanelState *st) {
    if (st->ui_horizon_parse.stop != SUITE_CSV_OK) return "Horizons (CSV)";
    if (st->ui_label_kind_parse.stop != SUITE_CSV_OK) return "Label Kind CSV";
    if (st->ui_tp_parse.stop != SUITE_CSV_OK) return "TP CSV";
    if (st->ui_sl_parse.stop != SUITE_CSV_OK) return "SL CSV";
    return nullptr;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingPanel_CsvError]
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
                                       DataPanelState *data, LaunchFailureState *lf) {
    ImGui::Begin("Training");

    // the frame's ONE parse of the four CSV fields, before anything reads them — the side's verdict over the kinds, the
    // TP / SL fields' gate, the mode, every start's gate and click (E.1.3 MP-6 step 10.5; an edit typed this frame, or
    // a combo changed this frame, reaches them the next)
    TrainingPanel_ParseInputs(state);
    const char *csv_bad = TrainingPanel_CsvError(state);   // the first CSV field a collect / train start reads that stopped

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
    ImGui::SeparatorText("Label & Collect");   // E.1.2.G — "this screen could be organized better"
    ImGui::Combo("Training Side", &state->ui_training_side, side_names, 2);
    // E.1.2.C 3-role (b) — flipping to the exit side defaults the label to
    // WILL_PEAK (P(peak) is exactly what the exit_threshold consumer wants);
    // the operator can still override to PVS etc. below. Mirrors the
    // label-combo retarget pattern; broadcast flows at click time.
    if (state->ui_training_side != prev_training_side) {
        if (state->ui_training_side == 1) state->label_type = LABEL_WILL_PEAK;
        // E.1.2.C — CLEAR the per-horizon Label-Kind CSV on a side flip, or the
        // retarget above is a lie. Every click resolves its kind through
        // Label_ResolveKindForHorizon (D-476: a NON-EMPTY CSV wins over the combo
        // at every position, single-horizon included), so with the CSV left in
        // place the combo would display "Will Peak" while every horizon trained on
        // the buy-side kind the CSV still held. Pre-D-476 the single-horizon click
        // read the combo directly and dodged this; it no longer does, so the clear
        // now covers all four paths.
        //
        // Clearing (rather than retargeting) is the honest choice: a per-horizon
        // label set typed for the ENTRY side carries no meaning on the exit side,
        // and an emptied CSV makes the visible combo authoritative again.
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
    // AGGREGATION over the label set the start labels (E.1.2.C; Training_SideVerdict): the gate used to read
    // state->label_type alone, so the per-horizon "Label Kind CSV" walked straight past it — an OK combo selection
    // could carry a REFUSE-tier horizon in behind it. Worst (numerically lowest) tier across the set wins; the line
    // names the label that sets it — a Label Kind CSV entry, or the combo's (the start gates' "label refused ...
    // (above)" points here; D-507 review F8). Both are valid label_table indices (the CSV parse keeps only kinds in
    // [0, LABEL_COUNT)).
    int       side_kind = 0;
    const int side_gate = Training_SideVerdict(state, &side_kind);
    if (side_gate == 0) {
        ImGui::TextColored(FoxmlColors::red,
            "exit side: label '%s' trains an ENTRY-goodness objective — inverted as an exit "
            "signal. Use Will Peak (default) or Peak/Valley/Stable.",
            label_table[side_kind].display_name);
    } else if (side_gate == 1) {
        ImGui::TextColored(FoxmlColors::yellow,
            "exit side: label '%s' is untriaged for exit semantics — proceed deliberately.",
            label_table[side_kind].display_name);
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

    // TP/SL barriers — D-473: gated on the REGISTRY COLUMNS, not a hand-listed id
    // set. The old list named four labels and silently gave none to any row added
    // after it was written — so VOL_BARRIER_3C_TIMED, the one row whose sl slot was
    // deliberately repurposed, had no UI at all: its sigma-window was un-settable
    // AND invisible, and the percent-shaped fallback then degenerated every label.
    // A hand-listed consumer of a registry is the Class-19 shape the registry's own
    // comment warns about; driving it off tp_kind/sl_kind means a new row auto-flows
    // its inputs instead of silently getting none.
    // E.1.3 MP-6 step 10.5 — the unit each field's values carry across the horizons the start labels (Training_SlotUnit:
    // one kind's, none, or "units differ"), and the fields render when a labelled horizon's kind uses a slot or a start
    // refuses on them (Training_SlotFieldsShown) — gated on the combo alone, a CSV kind with slots had its TP / SL hidden
    const int _tp_unit = Training_SlotUnit(state, true);
    const int _sl_unit = Training_SlotUnit(state, false);
    if (Training_SlotFieldsShown(state)) {
        // v5.11.40 — TP/SL fields now accept comma-separated values for
        // per-horizon mapping (broadcast-or-match rule). Single value
        // (e.g. "0.030") works as before — applies to every horizon.
        // CSV "0.020,0.030,0.040" maps positionally where N matches
        // the horizon count. Misalignment disables the Multi-Horizon
        // button with a hint (see render below).
        //
        // A field emptied of its values takes position 0's last value back
        // before it parses (TrainingPanel_ParseInputs).

        // 2026-09-02 (operator find) — captions + tooltips follow the slot's kind.
        // D-473 made the fields APPEAR for the sigma-window row but left them
        // percent-shaped, so the one row whose units differ was the one the panel
        // misdescribed. Since E.1.3 MP-6 step 10.5's review (F1) the kind is the one
        // the horizons are LABELLED with (Training_SlotUnit — the combo's only where no
        // Label Kind CSV overrides it). `###` pins the ImGui id while the caption changes.
        char tp_cap[64], sl_cap[64];
        snprintf(tp_cap, sizeof(tp_cap), "%s###label_tp_slot", Training_SlotCaption(_tp_unit, true));
        snprintf(sl_cap, sizeof(sl_cap), "%s###label_sl_slot", Training_SlotCaption(_sl_unit, false));
        ImGui::InputText(tp_cap, state->ui_tp_pct_csv, sizeof(state->ui_tp_pct_csv));
        if (const char *tip = Training_SlotTip(state, true)) {   // units differ per horizon, or unused
            ImGui::SetItemTooltip("%s", tip);
        } else if (_tp_unit == TP_SIGMA_K) {
            ImGui::SetItemTooltip("UNIT: SIGMAS — k, a multiplier of the sigma of the typical HORIZON move\n"
                                  "(barrier = k * sigma_tick * sqrt(H), D-475). NOT a percent of price.\n"
                                  "Larger k = wider barriers = more timeouts (class 0).\n\n"
                                  "Multi-horizon: comma-separated values map positionally to\n"
                                  "Horizons (CSV); a single value broadcasts. With a Label Kind CSV,\n"
                                  "each position's unit follows THAT horizon's kind.");
        } else {
            ImGui::SetItemTooltip("UNIT: percent of price — 0.5 = 0.5%% (= 50 bps).\n"
                                  "Take-profit barrier as %% of price\n"
                                  "label = 1 (or VALLEY for 3-class) if price moves up this much before SL is hit\n"
                                  "wider = fewer but higher-confidence labels\n"
                                  "tip: 0.050 = 5 bps. For short horizons (~1k ticks) at BTC scale,\n"
                                  "0.05-0.10%% gives balanced labels; 0.3+ usually = 99%% \"stable\".\n\n"
                                  "v5.11.40 multi-horizon: comma-separated values map positionally\n"
                                  "to Horizons (CSV), e.g. '0.020,0.030,0.040' for 3 horizons.\n"
                                  "A single value (e.g. '0.030') broadcasts to all horizons.");
        }
        ImGui::InputText(sl_cap, state->ui_sl_pct_csv, sizeof(state->ui_sl_pct_csv));
        if (const char *tip = Training_SlotTip(state, false)) {   // units differ per horizon, or unused
            ImGui::SetItemTooltip("%s", tip);
        } else if (_sl_unit == SL_VOL_WINDOW_TICKS) {
            ImGui::SetItemTooltip("UNIT: TICKS — the sigma-estimation window: how many PRIOR ticks the\n"
                                  "rolling sigma is measured over. NOT a stop-loss, NOT a percent.\n"
                                  "0 = the default (%d ticks). Below %d ticks no sigma can be produced,\n"
                                  "so such a window is floored to the default (D-473).\n"
                                  "Up to %d ticks is accepted, so a window that scales with the\n"
                                  "horizon can be set here.\n\n"
                                  "Multi-horizon: CSV maps positionally; a single value broadcasts.",
                                  LABEL_VOL_WINDOW_DEFAULT, LABEL_VOL_MIN_PERIODS, LABEL_VOL_WINDOW_MAX);
        } else {
            ImGui::SetItemTooltip("UNIT: percent of price — 0.5 = 0.5%% (= 50 bps).\n"
                                  "Stop-loss barrier as %% of price\n"
                                  "label = 0 (or PEAK for 3-class) if price drops this much before TP is hit\n"
                                  "wider = fewer but higher-confidence labels\n\n"
                                  "v5.11.40 multi-horizon: same CSV format as TP — single value\n"
                                  "broadcasts; N values map positionally to Horizons (CSV).");
        }
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
        // E.1.3 MP-6 step 10.5 — each field's parse (once a frame, TrainingPanel_ParseInputs) says where it stopped
        char csv_line[256];
        if (SuiteCsv_ErrorLine("TP CSV", &state->ui_tp_parse, csv_line, sizeof(csv_line)))
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "%s", csv_line);
        if (SuiteCsv_ErrorLine("SL CSV", &state->ui_sl_parse, csv_line, sizeof(csv_line)))
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "%s", csv_line);
    }

    // E.1.3 MP-6 step 10.5 — the Label Kind CSV renders ALWAYS: it overrides the label for every start that reads it, so
    // it is never hidden while it does (inside the TP / SL block, a combo label with no slot hid a populated CSV — its
    // field and its warning — that still drove collect and train)
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
            "  N must equal the Horizons count, or both multi-horizon starts refuse.\n\n"
            "Trains heterogeneous mixed-output ensembles in ONE click;\n"
            "v5.12.3.B+E mixed-output normalizer blends them at inference.");
        ImGui::Separator();
        ImGui::TextUnformatted("Lookup (auto-synced from FOREACH_TARGET):");
        for (int i = 0; i < LABEL_COUNT; i++)
            ImGui::Text("  %2d  %s", i, label_table[i].display_name);
        ImGui::EndTooltip();
    }
    // E.1.2.G — mismatch warning. A stale value here silently overrides the
    // Label Type combo for BOTH collect and train (2026-09-01: a leftover
    // "7" trained Peak/Valley/Stable for a full multi-horizon run while the
    // combo read "Vol Barrier 3-Class (timed)"; caught only by the no-skill
    // REFUSE). Say so IN the panel, at the field, in color.
    if (state->ui_label_kind_per_horizon_count > 0) {
        int mm = 0;
        for (int i = 0; i < state->ui_label_kind_per_horizon_count; ++i)
            if (state->ui_label_kind_per_horizon[i] != state->label_type) mm = 1;
        if (mm) {
            const int k0 = state->ui_label_kind_per_horizon[0];
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f),
                "⚠ CSV overrides Label Type: trains %s%s — not %s. Clear the field to use the combo.",
                (k0 >= 0 && k0 < LABEL_COUNT) ? label_table[k0].display_name : "?",
                state->ui_label_kind_per_horizon_count > 1 ? " (+ per-horizon kinds)" : "",
                (state->label_type >= 0 && state->label_type < LABEL_COUNT)
                    ? label_table[state->label_type].display_name : "?");
        }
    }
    {
        char csv_line[256];
        if (SuiteCsv_ErrorLine("Label Kind CSV", &state->ui_label_kind_parse, csv_line, sizeof(csv_line)))
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "%s", csv_line);
    }
    // v5.11.43 — Forward Ticks / Lookahead Ticks inputs DELETED. The Horizons CSV
    // (rendered below) is the single source of every label horizon: single-horizon
    // mode (1 entry) labels at horizons[0]; multi-horizon (N entries) labels each
    // horizon in the worker loop. (Its mirror, label_forward_ticks, went at E.1.3
    // MP-6 step 10.3: with nothing typed it held the last typed value, shown nowhere,
    // and Collect Features / Train Model labelled at it — F2 of the step's review.)

    // v5.11.43 — the horizon count is fresh before any button conditional fires: the CSV fields are parsed at the
    // panel's top (TrainingPanel_ParseInputs — once a frame, since E.1.3 MP-6 step 10.5).


    ImGui::Separator();

    // collect features button — disabled until its gate below opens (the
    // files, the side's verdict on the label, the CSV fields applied as typed,
    // a horizon, the suite free). prevents the
    // "click button N times because nothing visibly happens" UX trap that
    // fires N parallel backtests each writing to the same log file.
    //
    // v5.11.43 — auto-route by horizon count. Operator types horizons in
    // Horizons CSV; only the matching button is rendered. <=1 horizon =
    // "Collect Features" (single-mode worker). >1 = "Collect Multi-Horizon"
    // (multi-horizon worker). Both still write to results->feature_matrix.
    const int selected_files = DataPanel_SelectedCount(data);
    // the collect's gate (D-507; Backtest/SuiteStartGates.hpp): the files, the training side's verdict on the label
    // (E.1.2.C F3 — the line above), the CSV fields applied as typed (E.1.3 MP-6 step 10.5 — csv_bad), a horizon typed
    // (step 10.3 — the labels' horizon is the CSV's), then the suite free — a collect reallocates (MOVES) the shared feature_matrix / labels every training worker reads, so
    // it never overlaps another suite run: the lease is that rule
    const SuiteGate collect_gate = StartGate_CollectFeatures(selected_files, side_gate, state->ui_horizon_count, csv_bad);
    const bool can_collect = SuiteGate_Open(&collect_gate);
    // v5.11.43 — the horizon count routes the buttons: 0 or 1 = single mode; >1 = multi-horizon mode. The CSV is the
    // panel's ONE source: the old fallback to cfg.horizon_list read the LAST Run Control run's config (not the cfg
    // file), every frame, while a collect rewrote it (F8 — deleted at E.1.3 MP-6 step 10.3); with nothing typed,
    // single mode's starts refuse (their gates' horizons term) rather than label at a value nothing shows.
    const bool single_horizon_mode = (state->ui_horizon_count <= 1);

    if (single_horizon_mode) {
    if (!can_collect) ImGui::BeginDisabled();
    if (ImGui::Button("Collect Features")) {
        // D-507 — the request is built into the worker's own args: the shared run config, the display and the dataset
        // change only in the worker, holding the lease (the label params: TrainingPanel_CollectLabels — D-476's
        // position-0 kind, the barriers, the fee, the horizon, the feature mask)
        BacktestWorkerArgs *args = TrainingWorkers_AllocZeroed<BacktestWorkerArgs>();
        if (!args) {
            SuiteWorker_ReportNotStarted("Collect Features", START_CAUSE_NO_MEMORY, lf->launch_msg,
                                         sizeof(lf->launch_msg));
        } else {
            args->state = run_control;
            const BacktestLabelRequest labels = TrainingPanel_CollectLabels(state);
            // the gate already requires a file
            if (RunControl_BuildRequest(&args->request, run_control, data, &labels) == 0) {
                SuiteWorker_ReportNotStarted("Collect Features", START_CAUSE_NO_FILES, lf->launch_msg,
                                             sizeof(lf->launch_msg));
                free(args);
            } else if (SuiteWorker_Launch("Collect Features", &run_control->job, backtest_worker_fn, args,
                                          lf->launch_msg, sizeof(lf->launch_msg)) == SUITE_LAUNCH_STARTED) {
                TrainingPanel_ForgetDatasetResults(state);   // only once started: the lease is the collect's now
            } else {
                free(args);
            }
        }
    }
    ImGui::SetItemTooltip(
        "Runs a backtest AND gathers ML training samples (features + labels)\n"
        "for every slow-path cycle. Required before Train Model.\n\n"
        "Output goes to results->feature_matrix (in-memory). The dataset\n"
        "rebuilds every time you click — use Run Control's Run Backtest if\n"
        "you only need stats and want to skip the sample collection cost.\n\n"
        "The replay uses Run Control's Bandit prior, if one is set (the\n"
        "features and labels do not depend on it; the run's P&L can).");
    if (!can_collect) ImGui::EndDisabled();
    // the Run Control job's own run shows its progress (it holds the lease — or the click above just started it);
    // any other reason is the gate's
    if (SuiteJob_Running(&run_control->job)) {
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::yellow, "running... (%d%%)", run_control->job.progress);
    } else if (!can_collect) {
        SuiteGate_ShowWhy(&collect_gate);
    }
    } // end single_horizon_mode (Collect Features)

    // v5.11.24 — Collect Multi-Horizon button. Mirrors Train Multi-Horizon's
    // pattern (uses state->ui_horizon_csv populated by the input field below).
    // Disabled until its gate opens (below — the files, the CSV fields applied
    // as typed, the horizons, TP / SL and Label Kind counts that agree with
    // them, the side's verdict, the suite free).
    // Clicking spawns collect_multi_horizon_worker_fn which collects features
    // ONCE then loops over horizons recomputing labels + logging valid-sample
    // counts to engine.log. Final state: last horizon's labels in
    // results->labels[] (Train Multi-Horizon will recompute per horizon
    // during training, so no data loss).
    // v5.11.43 — only render Collect Multi-Horizon when N>1 horizons typed.
    // Single horizon → operator sees "Collect Features" only (rendered above).
    // N>1 → operator sees "Collect Multi-Horizon" only (rendered here).
    int mh_collect_horizon_count = state->ui_horizon_count;
    // v5.11.40 — broadcast-or-match alignment for per-horizon TP/SL and (E.1.3
    // MP-6 step 10.5) the label kinds, as Train Multi-Horizon's gate reads them:
    // a single value broadcasts, N values match the horizon count; anything
    // else disables the Multi-Horizon button with a hint. The rule is
    // StartGate_BroadcastsOrMatches, one of the gate's terms (D-507).
    const int tp_n = state->ui_tp_per_horizon_count;
    const int sl_n = state->ui_sl_per_horizon_count;
    const int lk_n = state->ui_label_kind_per_horizon_count;
    const SuiteGate mh_collect_gate =
        StartGate_CollectMultiHorizon(selected_files, mh_collect_horizon_count, tp_n, sl_n, lk_n, side_gate, csv_bad);
    const bool mh_can_collect = SuiteGate_Open(&mh_collect_gate);
    if (!single_horizon_mode) {
    if (!mh_can_collect) ImGui::BeginDisabled();
    if (ImGui::Button("Collect Multi-Horizon")) {
        // D-507 — everything the run starts from is snapped into the worker's own args (the request — built by the
        // same builder and label params as Collect Features — and the per-horizon arrays); the shared run config, the
        // display and the dataset change only in the worker, holding the lease. Built in place on the heap: with the
        // request inside, the args are ~578 KB — no stack copy.
        auto *args = TrainingWorkers_AllocZeroed<CollectMultiHorizonWorkerArgs>();
        if (!args) {
            SuiteWorker_ReportNotStarted("Collect Multi-Horizon", START_CAUSE_NO_MEMORY, lf->launch_msg,
                                         sizeof(lf->launch_msg));
        } else {
            CollectMultiHorizonWorkerArgs &snap = *args;
            snap.run_control = run_control;
            const BacktestLabelRequest labels = TrainingPanel_CollectLabels(state);
            const int files = RunControl_BuildRequest(&snap.request, run_control, data, &labels);
            snap.snap_horizon_count = mh_collect_horizon_count;
            // v5.11.40 — snap per-horizon TP/SL using broadcast-or-match.
            // single value (count==1) broadcasts to all horizons; N values
            // map positionally (the gate opened: each list holds one value
            // or more — TrainingPanel_ParseInputs seeds a field holding none).
            const float bcast_tp = state->ui_tp_per_horizon[0];
            const float bcast_sl = state->ui_sl_per_horizon[0];
            // E.1.2.G — kinds snap with the SAME broadcast-or-positional resolution
            // the TRAIN click uses. Collect previously labelled every horizon with
            // the dropdown while train obeyed the CSV — the panel could summarize
            // a label training never consumed (measured 2026-09-01).
            for (int i = 0; i < ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX; ++i) {
                snap.snap_horizons[i] = (i < mh_collect_horizon_count)
                    ? state->ui_horizon_list[i] : 0;
                snap.snap_tp_pct[i] = (state->ui_tp_per_horizon_count > 1
                                       && i < state->ui_tp_per_horizon_count)
                    ? state->ui_tp_per_horizon[i] : bcast_tp;
                snap.snap_sl_pct[i] = (state->ui_sl_per_horizon_count > 1
                                       && i < state->ui_sl_per_horizon_count)
                    ? state->ui_sl_per_horizon[i] : bcast_sl;
                // D-476 — ONE kind rule for every click (Label_ResolveKindForHorizon).
                snap.snap_label_kind[i] = Label_ResolveKindForHorizon(
                    state->ui_label_kind_per_horizon,
                    state->ui_label_kind_per_horizon_count, state->label_type, i);
            }
            if (files == 0) {   // the gate already requires a file
                SuiteWorker_ReportNotStarted("Collect Multi-Horizon", START_CAUSE_NO_FILES, lf->launch_msg,
                                             sizeof(lf->launch_msg));
                free(args);
            } else if (SuiteWorker_Launch("Collect Multi-Horizon", &run_control->job, collect_multi_horizon_worker_fn,
                                          args, lf->launch_msg, sizeof(lf->launch_msg)) == SUITE_LAUNCH_STARTED) {
                TrainingPanel_ForgetDatasetResults(state);   // only once started: the lease is the collect's now
            } else {
                free(args);
            }
        }
    }
    if (!mh_can_collect) ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        "v5.11.24 — Collects features ONCE, then loops over each horizon\n"
        "in 'Horizons (CSV)' (below) recomputing labels per horizon.\n\n"
        "Useful for inspecting per-horizon label class distribution\n"
        "BEFORE committing to a multi-horizon train run. Per-horizon\n"
        "valid-sample counts go to engine.log.\n\n"
        "Final results->labels[] holds the LAST horizon's labels (a collect\n"
        "cancelled before its label pass keeps the first horizon's), and the\n"
        "run's record names them: Walk-Forward and the HP sweep run on them,\n"
        "and Full Validation takes only that horizon's model (Model Path in\n"
        "its horizon_<H> directory). Train Multi-Horizon recomputes per\n"
        "horizon, so nothing is lost.\n\n"
        "The replay uses Run Control's Bandit prior, if one is set.");

    // the Run Control job's own run shows its progress; any other reason is the gate's (v5.11.40's TP/SL
    // misalignment hint is one of its terms)
    if (SuiteJob_Running(&run_control->job)) {
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::yellow, "running... (%d%%)", run_control->job.progress);
    } else if (!mh_can_collect) {
        SuiteGate_ShowWhy(&mh_collect_gate);
    }
    } // end !single_horizon_mode (Collect Multi-Horizon)
    // A Run Control run that STARTED but did not happen (its backtest refused the cfg or the data — INGEST-0 — or could
    // not allocate) — a run's outcome, published by its worker, not a start that failed: it stays here, under the collect
    // buttons that started it (Run Control shows its own under Run Backtest), not in the launch-failure modal, which only
    // a click opens.
    if (SuiteJob_Done(&run_control->job) && !RunControl_HasRun(run_control))
        ImGui::TextColored(FoxmlColors::red, "The last Run Control run did not happen — %s",
                           BacktestRunStatus_Name(run_control->last_status));

    // v5.11.43 — Horizons (CSV) input ALWAYS visible. Single source of
    // truth for both Collect/Train mode auto-routing AND every label
    // horizon. Type "1000" for single-horizon mode (one
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
        "Empty = Collect Features and Train Model refuse (type a horizon);\n"
        "Walk-Forward, the HP sweep and Full Validation still run on the\n"
        "collected labels.\n"
        "Max %d horizons, each 1..%d ticks and each named once; a value\n"
        "outside that, not a number, or a horizon typed twice stops the\n"
        "parse there and says so in red.",
        TrainingPanelState::PANEL_HORIZON_MAX, (int)MODEL_HORIZON_TICKS_MAX);
    ImGui::SameLine();
    ImGui::TextDisabled("(%d horizon%s parsed)",
                        state->ui_horizon_count,
                        state->ui_horizon_count == 1 ? "" : "s");
    {
        char csv_line[256];   // E.1.3 MP-6 step 10.5 — where the parse stopped, if it did
        if (SuiteCsv_ErrorLine("Horizons (CSV)", &state->ui_horizon_parse, csv_line, sizeof(csv_line)))
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "%s", csv_line);
    }

    // results pointer for the training sections below — every training run
    // (Train Model too: it runs on the funnel's worker, the multi-horizon run
    // with N=1, since v5.11.44) reads feature_matrix + labels while holding
    // the suite run lease, and a collect or a backtest — the runs that reset
    // and rebuild them — cannot start until the lease is free (D-503), so
    // the buffers never move under a reader.
    const BacktestResults *results = &run_control->results;   // read only at a click behind an open gate (the lease free); a gate drawn every frame reads RunControl_DatasetSamples

    // show feature collection status — display reads from a worker-written
    // snapshot, NOT from results->labels[] directly.
    //
    // Why: results->labels (+ sample_count, sample_capacity) is written by
    // the worker thread during collection and realloc'd as the buffer grows.
    // GUI rendering at 60fps that iterated those buffers raced with worker
    // reallocs → use-after-free → segfault on a 2.25M-sample run on
    // 2026-04-25. Snapshot pattern: worker computes the distribution stats
    // ONCE after Backtest_Run completes (in backtest_worker_fn) and publishes
    // its job's result last (SuiteJob_Publish — a release); the trampoline
    // then ends the job (a release compare-and-swap). GUI reads the snapshot
    // only once SuiteJob_Running shows no run — the acquire that pairs with
    // them is the happens-before edge.
    //
    // Bonus: the diagnostic compute happens once per run, not every render
    // frame. Iterating millions of labels every frame was wasteful even
    // when it didn't crash.
    const SamplesSnapshot *snap = &run_control->stats_snapshot;
    // E.1.2.G — per-horizon breakdown table (operator ask: "display the
    // breakdown per horizon instead of just the last one"). When a
    // multi-horizon collect has filled the per-horizon snapshots, render ALL
    // of them and retire the single-line-plus-footnote view; the legacy line
    // stays for single-horizon runs (count==0).
    // Both views read the snapshot only while Run Control's outputs are at rest (RunControl_AtRest) — the worker writes
    // it at the end of its run, so a read during the run could see half of it (the single-horizon line used to read it
    // every frame, relying on the click's zeroing; D-507 review F5). A training run leaves the table up.
    const bool rc_at_rest = RunControl_AtRest(run_control);
    int mh_n = rc_at_rest ? run_control->mh_collect_snap_count : 0;
    if (mh_n > 0) {
        const ImVec4 vg = ImVec4(0.55f, 0.76f, 0.51f, 1.0f);
        const ImVec4 vy = ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
        const ImVec4 vr = ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
        ImGui::TextColored(FoxmlColors::comment, "Per-horizon label distribution (collect)");
        if (ImGui::BeginTable("mh_collect_tbl", 7,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Horizon");
            ImGui::TableSetupColumn("Label");
            ImGui::TableSetupColumn("TP");
            ImGui::TableSetupColumn("SL");
            ImGui::TableSetupColumn("Samples");
            ImGui::TableSetupColumn("Breakdown", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Verdict", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (int h = 0; h < mh_n; ++h) {
                const SamplesSnapshot *hs = &run_control->mh_collect_snap[h];
                if (hs->sample_count <= 0) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", hs->horizon_ticks);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    (hs->label_type >= 0 && hs->label_type < LABEL_COUNT)
                        ? label_table[hs->label_type].display_name : "?");
                ImGui::TableNextColumn(); ImGui::Text("%.3g", run_control->mh_collect_tp[h]);
                ImGui::TableNextColumn(); ImGui::Text("%.3g", run_control->mh_collect_sl[h]);
                ImGui::TableNextColumn(); ImGui::Text("%d", hs->sample_count);

                // Breakdown cell, kind-aware
                char bd[256]; bd[0] = '\0'; size_t off = 0;
                if (hs->label_kind == 2) {
                    int K = hs->num_classes > 16 ? 16 : hs->num_classes;
                    for (int k = 0; k < K && off < sizeof(bd) - 32; ++k)
                        off += snprintf(bd + off, sizeof(bd) - off, "%sc%d %.1f%%",
                                        k ? "  ·  " : "", k,
                                        hs->baseline_total > 0
                                            ? 100.0 * hs->class_counts[k] / hs->baseline_total
                                            : 0.0);
                } else if (hs->label_kind == 0) {
                    snprintf(bd, sizeof(bd), "pos %d  ·  neg %d  ·  neutral %d",
                             hs->pos_count, hs->neg_count, hs->neutral_count);
                } else {
                    snprintf(bd, sizeof(bd), "mean %.4f  σ %.4f  [%.4f, %.4f]",
                             hs->lmean, hs->lstddev, hs->lmin, hs->lmax);
                }
                ImGui::TableNextColumn(); ImGui::TextUnformatted(bd);

                // Verdict cell — SAME thresholds as the single-snapshot
                // diagnosis (fair-share ratio 0.15 red / 0.5 yellow; 70%
                // dominance; TECH_DEBT-302a rarest-class rule).
                ImGui::TableNextColumn();
                if (hs->label_kind == 2 && hs->baseline_total > 0) {
                    int K = hs->num_classes > 16 ? 16 : hs->num_classes;
                    int used = 0, minc = 0, mincls = 0, maxc = 0, maxcls = 0;
                    for (int k = 0; k < K; ++k) {
                        int c = hs->class_counts[k];
                        if (c > maxc) { maxc = c; maxcls = k; }
                        if (c > 0) { if (!used || c < minc) { minc = c; mincls = k; } used++; }
                    }
                    float fair = 100.0f / (K > 0 ? K : 1);
                    float minp = 100.0f * minc / hs->baseline_total;
                    float maxp = 100.0f * maxc / hs->baseline_total;
                    if (used <= 1)
                        ImGui::TextColored(vr, "one class only — degenerate");
                    else if (minp < fair * 0.15f)
                        ImGui::TextColored(vr, "c%d starved (%.1f%% of %.0f%% fair)", mincls, minp, fair);
                    else if (maxp > 70.0f)
                        ImGui::TextColored(vy, "c%d dominates at %.1f%%", maxcls, maxp);
                    else if (minp < fair * 0.5f)
                        ImGui::TextColored(vy, "c%d under-represented (%.1f%%)", mincls, minp);
                    else
                        ImGui::TextColored(vg, "balanced");
                } else if (hs->label_kind == 0) {
                    if (hs->pos_count == 0 || hs->neg_count == 0)
                        ImGui::TextColored(vr, "one-sided — unlearnable");
                    else
                        ImGui::TextColored(vg, "two-sided");
                } else {
                    ImGui::TextColored(vg, "—");
                }
            }
            ImGui::EndTable();
        }
        ImGui::SetItemTooltip(
            "Every horizon's OWN class distribution from the last multi-horizon\n"
            "collect — the label kind shown is what training will actually use\n"
            "(the Label Kind CSV override reaches collect and train identically).\n"
            "Verdict thresholds match the single-run diagnosis: rarest class under\n"
            "15%% of fair share = starved; under 50%% = under-represented; one\n"
            "class >70%% = dominance.");
    } else if (rc_at_rest && snap->sample_count > 0) {
        // FoxML colors for diagnostics
        const ImVec4 diag_green  = ImVec4(0.55f, 0.76f, 0.51f, 1.0f);
        const ImVec4 diag_yellow = ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
        const ImVec4 diag_red    = ImVec4(0.95f, 0.35f, 0.35f, 1.0f);

        if (snap->label_kind == 1) {
            // regression: continuous labels — show distribution stats, not +/- counts.
            float lmin = snap->lmin, lmax = snap->lmax;
            float mean = snap->lmean, stddev = snap->lstddev;
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
            // TECH_DEBT-302a — the RAREST class was never computed, let alone tested. The ladder
            // below only ever asked "does one class dominate?", so a 61.5/34.4/4.1 split earned a
            // green "balanced — model can learn each" while the 4.1% class was the buy signal.
            // Track the minimum over NON-EMPTY classes (an absent class is the counts_used<=1 /
            // degenerate arm's business, not this one).
            int min_count = 0;
            int min_class = 0;
            for (int k = 0; k < K; k++) {
                if (snap->class_counts[k] > max_count) {
                    max_count = snap->class_counts[k]; max_class = k;
                }
                if (snap->class_counts[k] > 0) {
                    if (counts_used == 0 || snap->class_counts[k] < min_count) {
                        min_count = snap->class_counts[k]; min_class = k;
                    }
                    counts_used++;
                }
            }
            float max_pct = snap->sample_count > 0
                ? 100.0f * max_count / snap->sample_count : 0.0f;
            float min_pct = snap->sample_count > 0
                ? 100.0f * min_count / snap->sample_count : 0.0f;
            // K-AWARE, not a magic percentage: "balanced" means 100/K per class, so the honest
            // question is how far under its FAIR SHARE the rarest class sits. At K=3 fair share is
            // 33.3%, so 0.15 -> ~5% and 0.5 -> ~16.7%. The same thresholds stay meaningful at K=2
            // or K=5, which a hardcoded "<5%" would not.
            const float fair_share = (K > 0) ? (100.0f / (float)K) : 0.0f;
            const float min_ratio  = (fair_share > 0.0f) ? (min_pct / fair_share) : 1.0f;
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
            } else if (min_ratio < 0.15f) {
                // TECH_DEBT-302a — the arm that was missing. No class need DOMINATE for the split
                // to be unlearnable; it is the rarest class that decides what the model can emit,
                // and on this engine that class is usually the trade signal.
                mc_col = &diag_red;
                mc_text = "rarest class is starved — the model will rarely emit it";
                mc_tip  = "The smallest class holds under ~15%% of its fair share (100/K).\n"
                          "No class 'dominates', so this reads as balanced — but the class you\n"
                          "most likely care about (the trade signal) has almost no examples to\n"
                          "learn from, and accuracy will look fine while never predicting it.\n"
                          "Check per-class recall, not overall accuracy. Consider tighter\n"
                          "barriers or a longer lookahead to make that class more decisive.";
            } else if (min_ratio < 0.5f) {
                mc_col = &diag_yellow;
                mc_text = "rarest class is under-represented — check its per-class recall";
                mc_tip  = "The smallest class holds under half its fair share (100/K).\n"
                          "Learnable, but overall accuracy will be dominated by the commoner\n"
                          "classes — read per-class recall before trusting the headline number.";
            }
            // TECH_DEBT-302a — the "cN dominates at X%%" prefix used to be UNCONDITIONAL, so it
            // was glued onto every verdict including "balanced", producing the literal output
            // `c0 dominates at 61.5%% — balanced`. Report whichever class the verdict is ACTUALLY
            // about: the rarest one when scarcity is the finding, the largest when dominance is.
            if (min_ratio < 0.5f && max_pct <= 70.0f) {
                ImGui::TextColored(*mc_col, "Diagnosis: rarest c%d at %.1f%% (fair share %.1f%%) — %s",
                                   min_class, min_pct, fair_share, mc_text);
            } else if (max_pct > 70.0f || counts_used <= 1) {
                ImGui::TextColored(*mc_col, "Diagnosis: c%d dominates at %.1f%% — %s",
                                   max_class, max_pct, mc_text);
            } else {
                ImGui::TextColored(*mc_col, "Diagnosis: %s (largest c%d %.1f%%, rarest c%d %.1f%%)",
                                   mc_text, max_class, max_pct, min_class, min_pct);
            }
            ImGui::SetItemTooltip("%s", mc_tip);
        } else {
            // binary: +/-/neutral counts from snapshot
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
        // TECH_DEBT-302 (b) — say WHOSE distribution this is, for EVERY label kind. Under a multi-horizon collect the
        // snapshot is computed from the single results->labels[] array, which holds the LAST horizon's labels — so the
        // line above describes ONE horizon, not the run; the other horizons' splits are the per-horizon table's or the
        // [collect-mh] stderr lines. (Moved out of the multiclass branch — a regression or binary line went unqualified:
        // D-507 third review, A4.)
        if (snap->horizon_ticks > 0) {
            ImGui::TextColored(FoxmlColors::comment,
                               "  ^ horizon %d ticks ONLY (last of the multi-horizon collect) "
                               "— other horizons differ; see [collect-mh] lines",
                               snap->horizon_ticks);
        }
    } else if (rc_at_rest && run_control->results.samples_dropped) {
        // the collect ran but its label pass aborted, so its samples were dropped (step 10.7): say why there are none —
        // a gate's "collect first" alone sends the operator round the same refusal
        ImGui::TextColored(FoxmlColors::red, "No samples: the last collect's label pass aborted, so its samples were "
                                             "dropped. The log names the file — fix or deselect it (an unsorted one: "
                                             "csv_sort_check_mode=2, AUTO), then collect again.");
    }

    ImGui::Separator();

    // XGBoost hyperparameters
    ImGui::SeparatorText("XGBoost Parameters");
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
        // 2026-09-02 (operator find) — the bound is the cfg REGISTRY's clamp, read here
        // rather than re-typed: the old hardcoded 50 was an arbitrary GUI bound, not an
        // XGBoost limit, and it capped the E3 experiment (100-200 at the 1000-tick
        // horizon, where an independent event is ~10 rows). One number, one home.
        {
            const auto& _mcw = g_global_cfg_field_descriptors[FIELD_IDX_GLOBAL_xgb_min_child_weight].payload.as_int;
            const int _mcw_lo = (int)_mcw.clamp_min, _mcw_hi = (int)_mcw.clamp_max;
            ImGui::InputInt("Min Child Weight", &state->ui_min_child_weight, 1, 5);
            if (state->ui_min_child_weight < _mcw_lo) state->ui_min_child_weight = _mcw_lo;
            if (state->ui_min_child_weight > _mcw_hi) state->ui_min_child_weight = _mcw_hi;
            ImGui::SetItemTooltip("Min sum of hessians per leaf (%d-%d; the cfg registry's clamp).\n"
                                  "For classification the hessian is p(1-p) per row, so this is\n"
                                  "roughly ROWS x 0.25 at the margin — size it to the independent\n"
                                  "events in a leaf, not to rows (at ~10 rows per event, 100-200\n"
                                  "spans 10-20 events at the 1000-tick horizon).\n"
                                  "Higher = more regularization. No XGBoost upper bound.\n"
                                  "Default 5 (matches pre-v5.9.5h hardcoded)", _mcw_lo, _mcw_hi);
        }
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

        // D-477 (2026-09-02, operator find) — the trainer's half of the feature mask.
        // The serve side has packed through the mask-aware Features_PackAll under
        // node_<N>_feature_mask, the stamp has carried `feature_mask`, and load time
        // has compared the two — but nothing under Backtest/ applied a mask at COLLECT,
        // so "drop the day-reach features" after an E4 refusal could not be run from
        // the suite. This field IS that mask: the collect packs through the SAME
        // overload (masked columns are 0.0f, shape kept), the stamp records it, and a
        // deploying node's node_<N>_feature_mask must match at load. Parsed by the
        // same Cfg_ParseU64Mask the cfg key uses. Empty = all features.
        ImGui::InputText("Feature mask (hex; empty = all)", state->ui_feature_mask_hex,
                         sizeof(state->ui_feature_mask_hex));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(
                "Bit i SET = feature i is collected; CLEAR = column i is 0.0f at collect\n"
                "(the shape is kept, so the model sees a constant). Applies to Collect\n"
                "(single + multi-horizon); the stamp records it; a deploying node's\n"
                "node_<N>_feature_mask must equal it or the load is refused (strict).\n"
                "0xHEX (any case) or decimal. Empty = all features. 0x0 = every feature\n"
                "masked = refused here (treated as all-on).\n\n"
                "Feature bits (auto-synced from FOREACH_FEATURE):");
            ImGui::Separator();
            for (int i = 0; i < NUM_REGISTERED_FEATURES; i++)
                ImGui::Text("  bit %2d  %s", i, FEATURE_NAMES[i]);
            ImGui::EndTooltip();
        }
        {
            uint64_t parsed = 0; int ok = 1;
            if (state->ui_feature_mask_hex[0] != '\0')
                ok = Cfg_ParseU64Mask(state->ui_feature_mask_hex, &parsed);
            state->ui_feature_mask = ok ? parsed : 0;   // unparseable → all-on, and say so
            const uint64_t valid_bits = (NUM_REGISTERED_FEATURES >= 64)
                ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << NUM_REGISTERED_FEATURES) - 1ULL);
            if (!ok) {
                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f),
                    "⚠ Feature mask unparseable — expected 0xHEX or decimal; collecting with ALL "
                    "features until fixed.");
            } else if (state->ui_feature_mask_hex[0] != '\0' && parsed == 0) {
                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f),
                    "⚠ 0x0 would mask EVERY feature — treated as all-on; clear the field or set bits.");
            } else if (parsed != 0 && (parsed & valid_bits) != valid_bits) {
                const int masked = NUM_REGISTERED_FEATURES - __builtin_popcountll(parsed & valid_bits);
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f),
                    "masking %d of %d features at collect — the stamp records the mask; a deploying "
                    "node's node_<N>_feature_mask must match it.", masked, NUM_REGISTERED_FEATURES);
            }
        }
    }

    // The model Run Full Validation validates and re-stamps — shown in BOTH horizon modes, since Full Validation is
    // (v5.11.48 hid it in multi-horizon mode while Full Validation still read it: an input that drove a stamp with no
    // way to see it — D-507 third review, A1). Train Model / Train Multi-Horizon do not read it.
    ImGui::InputText("Model Path (Full Validation)", state->model_path, sizeof(state->model_path));
    ImGui::SetItemTooltip(
        "The model Run Full Validation validates and re-stamps.\n"
        "Point it at the model of the collected labels' horizon:\n"
        "  models/<class>/<run_name>/horizon_<H>/<role>.json\n"
        "Full Validation refuses a model of another horizon, and a path\n"
        "that names no horizon_<H> directory (its horizon cannot be checked).\n\n"
        "NOT used by Train Model / Train Multi-Horizon — those auto-generate\n"
        "save paths from Run Name + the nested family layout (D-431).");

    // v5.11.48 — Run Name prefix input rendered HERE (before Train buttons)
    // so operator sees + sets it BEFORE clicking train (its post-train twin
    // went with the Training panel's Save Run, D-d).
    ImGui::InputText("Run Name (prefix)", state->run_name, sizeof(state->run_name));
    ImGui::SetItemTooltip(
        "Family name. The worker creates ONE family dir and a horizon_<H> child\n"
        "per horizon inside it (D-431 nested layout).\n\n"
        "Example: Run Name \"btc_5min\" + horizons 1000,7500,15000 →\n"
        "  models/<class>/btc_5min/horizon_1000/barrier.json\n"
        "  models/<class>/btc_5min/horizon_7500/barrier.json\n"
        "  models/<class>/btc_5min/horizon_15000/barrier.json\n\n"
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
        //
        // E.1.3 MP-6 step 10.5's second review (N6): it still read the COMBO, while the writer takes the class tree from
        // the run's PRIMARY kind (position 0's resolved kind) and each horizon's role file from that horizon's kind — so
        // with a Label Kind CSV it named a file or a tree the run never writes. Both now come from the writer's own rules
        // over the kinds the start labels (Training_TrainedKind); horizons whose roles differ say so.
        const char* role_preview =
            Training_ResolveRole(Training_TrainedKind(state, 0), state->ui_training_side);
        for (int i = 1; i < Training_TrainedHorizons(state); ++i)
            if (strcmp(Training_ResolveRole(Training_TrainedKind(state, i), state->ui_training_side), role_preview) != 0)
                role_preview = "<role per horizon>";
        const char* class_preview = Training_ResolveClassTree(Training_TrainedKind(state, 0));
        // D-431 nested layout — the preview spells the SAME grammar the writer
        // builds via ModelPath_HorizonDir ("<family>/horizon_<N>"); the retired
        // flat "<run>_horizon_<H>" form lied to the operator for a month after
        // the writer moved (what-to-do-next 2026-09-02 gotcha).
        ImGui::TextDisabled("Will write to: models/%s/%s/%s<%s>/%s.json",
                            class_preview, state->run_name, MODEL_HORIZON_PREFIX,
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
    // worker-mutating state on click-back. The exclusion is the suite run
    // lease now (D-503): one suite run at a time, refused at the funnel, and
    // every start button's gate reads it and names the run that holds it
    // (D-507) — which also keeps a collect from reallocating the results a
    // trainer reads (E.1.2.D NEW-5) without a term of its own. The gate's
    // terms (Backtest/SuiteStartGates.hpp): a build that trains, the side's
    // verdict, the CSV fields applied as typed, a horizon, the suite free,
    // the samples.
    const SuiteGate train_gate =
        StartGate_TrainModel(START_GATE_BUILD_TRAINS, side_gate, state->ui_horizon_count,
                             RunControl_DatasetSamples(run_control), csv_bad);
    const bool can_train = SuiteGate_Open(&train_gate);

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
        // Train Model is the multi-horizon run with N=1 (v5.11.44).
        const int single_h = state->ui_horizon_list[0];   // the gate requires one horizon typed (E.1.3 MP-6 step 10.3)

        // MP-1 — the same request builder as Train Multi-Horizon: one horizon, and the operator's
        // training side applies here too (v5.13.5.A — one exit-side model without a CSV). The launch
        // snaps the per-horizon table's horizon once the run has started (MP-6 step 8).
        if (TrainingPanel_LaunchMultiHorizon(state, run_control, "Train Model", 1, &single_h, lf)) {
            // Train Model has always cleared the standalone walk-forward's results (its pipeline runs its own —
            // v5.11.44); now only once this run has started, so a refused click changes nothing (MP-6 step 8)
            SuiteJob_Forget(&state->wf_job);
            memset(&state->wf_results, 0, sizeof(state->wf_results));
        }
    }
    if (!can_train) {
        ImGui::EndDisabled();
        SuiteGate_ShowWhy(&train_gate);
    }
    } // end single_horizon_mode (Train Model)

    // v5.10.0a.G.1 — Train Multi-Horizon button. Adjacent to Train
    // Model so operators see both options. Gated on horizons being
    // configured in the panel's CSV (its one source since E.1.3 MP-6 step 10.3,
    // parsed once at the panel's top — the mode, this gate and the click read one list).
    // v5.10.0a-bugfix2: in-panel CSV editor — operator no longer
    // needs to edit cfg.horizon_list + reload to multi-horizon train.
    // v5.11.43 — only render in multi-horizon mode (single mode shows
    // Train Model, above).

    // v5.11.43 — second Horizons CSV InputText DELETED. Single source of
    // truth lives at the top of the panel (rendered always, near
    // Collect Features / Collect Multi-Horizon). Operator types horizons
    // there; auto-routing renders the matching Train button here.

    // v5.11.40 — broadcast-or-match for TP/SL on the train side too.
    // Same validation as Collect Multi-Horizon (above), against the CSV's
    // horizon count. v5.13.1.B — the label_kind CSV
    // follows the same rule. All three are terms of the gate (D-507;
    // Backtest/SuiteStartGates.hpp), after the CSV fields applied as typed
    // (E.1.3 MP-6 step 10.5) — the label-kind misalignment used to grey the
    // button with no reason shown.
    int train_tp_n = state->ui_tp_per_horizon_count;
    int train_sl_n = state->ui_sl_per_horizon_count;
    int train_lk_n = state->ui_label_kind_per_horizon_count;
    const SuiteGate mh_train_gate =
        StartGate_TrainMultiHorizon(START_GATE_BUILD_TRAINS, side_gate, state->ui_horizon_count, train_tp_n, train_sl_n,
                                    train_lk_n, RunControl_DatasetSamples(run_control), csv_bad);
    const bool mh_can_train = SuiteGate_Open(&mh_train_gate);
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
        "rule: a single value broadcasts — refused when the horizons' units\n"
        "differ; N values map positionally).\n"
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
        "results table renders below.\n"
        "\n"
        "Each horizon recomputes its labels at its own horizon\n"
        "and trains a separate model (D-431 nested family layout).\n"
        "Operator manually picks which horizon to deploy (or relies on\n"
        "v5.10.0a.G.4 ensemble inference once engine wiring lands). Past\n"
        "Runs panel treats each horizon as a separate row for\n"
        "Compare-to-Baseline.");
    if (mh_clicked) {
        // MP-1 — every input of the run is snapped HERE by the one request builder (v5.10.0E
        // pattern); the horizons are the CSV's, its one source (E.1.3 MP-6 step 10.3). The launch snaps
        // the per-horizon table's horizons once the run has started (MP-6 step 8).
        TrainingPanel_LaunchMultiHorizon(state, run_control, "Train Multi-Horizon", state->ui_horizon_count,
                                         state->ui_horizon_list, lf);
    }
    if (!mh_can_train) {
        ImGui::EndDisabled();
        SuiteGate_ShowWhy(&mh_train_gate);   // v5.11.40's misalignment hints are among its terms
    }
    // (ONE tooltip, on the button above: a second SetItemTooltip here bound to the reason text, or — with the button
    // enabled — replaced the first one, so its Label Kind text never showed; its content moved into the first)
    } // end !single_horizon_mode (Train Multi-Horizon block)

    // The training run's progress, its Cancel and its per-horizon table — in BOTH modes: Train Model is the same run with
    // one horizon (v5.11.44), and these rendered only in multi-horizon mode, so a Train Model run showed no progress and
    // could not be cancelled (D-505 item 1; E.1.3 MP-6 step 10.2)
    if (SuiteJob_Running(&state->mh_job)) {
        const int done = state->mh_job.progress, total = state->mh_job.total;
        float pct = total > 0 ? (float)done / total : 0.0f;
        char overlay[96];
        snprintf(overlay, sizeof(overlay), "horizon %d/%d (current: %d ticks)",
                 done, total, (int)state->mh_current_horizon);
        ImGui::ProgressBar(pct, ImVec2(-1, 0), overlay);
        // one label for both Train buttons: a label from the panel's mode would follow a CSV edited mid-run
        if (ImGui::Button("Cancel Training"))
            SuiteJob_Cancel(&state->mh_job);
    }

    // v5.11.41 — per-horizon results table. Renders during run AND
    // post-completion so operator can review metrics without scrolling
    // through stderr. Each row = one horizon's WF + held-out + stamp
    // status. Columns:
    //   Horizon  | Progress  | Status   | Metrics
    // (status string is built by the worker's per-horizon block).
    // Empty when no Multi-Horizon run has fired yet.
    if (state->mh_job.total > 0) {
        ImGui::Separator();
        ImGui::SeparatorText("Per-horizon results");
        if (ImGui::BeginTable("mh_horizons", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Horizon");
            ImGui::TableSetupColumn("WF %");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("Metrics");
            ImGui::TableHeadersRow();

            const int mh_total = state->mh_job.total;
            const int n_show = mh_total < TrainingPanelState::PANEL_HORIZON_MAX ? mh_total
                                                                                 : TrainingPanelState::PANEL_HORIZON_MAX;
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
                if (TrainingSink_Load(&state->mh_horizon_complete[h])) {   // F6 — acquire
                    ImGui::TextColored(FoxmlColors::green, "DONE");
                } else if (SuiteJob_Running(&state->mh_job) && h == (state->mh_job.progress - 1)) {
                    ImGui::TextColored(FoxmlColors::yellow, "running");
                } else {
                    ImGui::TextDisabled("waiting");
                }

                ImGui::TableNextColumn();
                char row_text[sizeof(TrainingStatusLine::text)];   // a snapshot — the row's job may be rewriting it
                if (TrainingStatusLine_Read(&state->mh_horizon_status[h], row_text, sizeof(row_text)) && row_text[0]) {
                    ImGui::TextWrapped("%s", row_text);
                } else {
                    ImGui::TextDisabled("--");
                }
            }
            ImGui::EndTable();
        }
    }

    // training results — kind-appropriate display.
    // D-d (2026-08-22, operator-decided) — the ~300-line results+Save-Run block
    // that rendered here was gated on `model_trained`, whose only true-writers
    // lived in the deleted train_model_worker_fn: permanently-invisible UI at
    // HEAD (S1-F8, the Class-44 shape). The MH results table above is the live
    // results view; the one living signal (the completion status line) now
    // renders whenever it has content:
    char run_text[sizeof(TrainingStatusLine::text)];   // a snapshot — the run may be rewriting it
    if (TrainingStatusLine_Read(&state->run_status, run_text, sizeof(run_text)) && run_text[0]) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.55f, 0.76f, 0.51f, 1.0f), "%s", run_text);
    }

    //==================================================================
    // WALK-FORWARD VALIDATION (Phase 6A — the REAL performance metric)
    //==================================================================
    ImGui::Separator();
    ImGui::SeparatorText("Walk-Forward Validation");
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
        // the value the clicks will use — it covers the collected labels too (A2); the record and the results are read
        // only while Run Control's outputs are at rest (RunControl_AtRest — a training run writes neither, so it shows
        // the same value mid-run)
        ImGui::SameLine();
        ImGui::TextDisabled("(auto = %d)",
                            RunControl_AtRest(run_control)
                                ? Training_ResolvePurgeForLabels(state, &run_control->run_config, &run_control->results)
                                : Training_ResolvePurgeHorizon(state));
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
        // its gate (D-507; Backtest/SuiteStartGates.hpp) — one run at a time (D-503); while WF itself runs, the
        // running branch below shows its progress and Cancel instead of this button
        const SuiteGate wf_gate = StartGate_WalkForward(START_GATE_BUILD_TRAINS, RunControl_DatasetSamples(run_control));
        const bool can_wf = SuiteGate_Open(&wf_gate);
        if (SuiteJob_Running(&state->wf_job)) {
            ImGui::ProgressBar(state->wf_job.progress / 100.0f, ImVec2(-1, 0), "Walk-forward...");
            if (ImGui::Button("Cancel Walk-Forward"))
                SuiteJob_Cancel(&state->wf_job);
        } else {
            if (!can_wf) ImGui::BeginDisabled();
            if (ImGui::Button("Run Walk-Forward")) {
                // the click-time snapshot on the stack; the heap copy the worker frees is made only to launch
                WalkForwardWorkerArgs snap{};
                WalkForwardWorkerArgs *wf_args = &snap;
                wf_args->state = state;
                wf_args->data = results;
                // E.1.2.C follow-up — snapshot at click, on the GUI thread, which is
                // the only moment the panel is coherent. label_type comes from
                // run_config (the field that produced results->labels[]), same
                // resolver-SSoT choice as the Run-Full-Validation path.
                wf_args->snap_wf_n_splits      = state->wf_n_splits;
                wf_args->snap_wf_horizon_ticks = Training_ResolvePurgeForLabels(state, &run_control->run_config, results);   // s5 leaf-16 + A2
                wf_args->snap_wf_buffer_ticks  = state->wf_buffer_ticks;
                wf_args->snap_wf_min_train     = state->wf_min_train;
                wf_args->snap_label_type       = run_control->run_config.label_type;
                wf_args->snap_hp = Training_SnapshotHyperparams(state);
                auto *heap = (WalkForwardWorkerArgs *)malloc(sizeof(WalkForwardWorkerArgs));
                if (!heap) {
                    SuiteWorker_ReportNotStarted("Walk-Forward", START_CAUSE_NO_MEMORY, lf->launch_msg,
                                                 sizeof(lf->launch_msg));
                } else {
                    *heap = snap;
                    if (SuiteWorker_Launch("Walk-Forward", &state->wf_job, walkforward_worker_fn, heap,
                                           lf->launch_msg, sizeof(lf->launch_msg)) != SUITE_LAUNCH_STARTED)
                        free(heap);
                }
            }
            if (!can_wf) {
                ImGui::EndDisabled();
                SuiteGate_ShowWhy(&wf_gate);
            }
        }
    }

    // walk-forward results display — kind-aware (label-type-aware metric invariant)
    if (SuiteJob_Done(&state->wf_job)) {
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
                // 2026-09-03 — the diagnosis reads BALANCED accuracy against the
                // balanced floor (1 / present classes), the SAME pair the stamp
                // gate refuses against (Backtest_BalancedSkillFloor): training is
                // class-weighted, so plain accuracy vs the majority share was the
                // wrong score for it (TECH_DEBT-313 lesser bullet). Plain accuracy
                // stays visible in the table above; the bands key on balanced.
                float val_acc = wf->mean_val_balanced_accuracy;
                float train_acc = wf->mean_train_balanced_accuracy;
                int K = (wf->num_classes >= 2) ? wf->num_classes : 2;
                // baseline_total, not sample_count: the counts are over the
                // baseline population (binary excludes neutrals) — same SSoT
                // route as the stamp gate's skill floor, so this diagnosis and
                // that gate can never disagree (F4 close).
                float baseline = multiclass_balanced_baseline(
                    K, snap->class_counts, snap->baseline_total);
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
    ImGui::SeparatorText("Hyperparam Sweep (XGBoost grid search over training)");
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
        "  / xgb_seed");
    {
        ImGui::PushItemWidth(-180);
        ImGui::SliderInt("Sweep params (1 or 2)", &state->hp_num_params, 1, OPT_MAX_PARAMS, "%d",
                         ImGuiSliderFlags_AlwaysClamp);   // a typed value clamped too (R2 — the editors index by it)
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
        const long long hp_total_cells = (long long)state->hp_ranges[0].steps()
                                       * (state->hp_num_params > 1 ? state->hp_ranges[1].steps() : 1);   // never wraps
        ImGui::Text("Total cells: %lld", hp_total_cells);
        ImGui::PopItemWidth();

        const BacktestResults *hp_data = &run_control->results;
        // its gate (D-507; Backtest/SuiteStartGates.hpp) — one run at a time (D-503); each axis checked on its own,
        // so two inverted ranges can no longer multiply to an in-range count
        const SuiteGate hp_gate =
            StartGate_HyperparamSweep(START_GATE_BUILD_TRAINS, state->hp_num_params, OPT_MAX_PARAMS,
                                      state->hp_ranges[0].steps(),
                                      state->hp_num_params > 1 ? state->hp_ranges[1].steps() : 1, OPT_MAX_STEPS,
                                      OPT_MAX_GRID, RunControl_DatasetSamples(run_control));
        const bool can_hp = SuiteGate_Open(&hp_gate);

        if (SuiteJob_Running(&state->hp_job)) {
            const int done = state->hp_job.progress, total = state->hp_job.total;
            float pct = total > 0 ? (float)done / total : 0.0f;
            char overlay[64];
            snprintf(overlay, sizeof(overlay), "%d / %d cells", done, total);
            ImGui::ProgressBar(pct, ImVec2(-1, 0), overlay);
            if (ImGui::Button("Cancel Hyperparam Sweep"))
                SuiteJob_Cancel(&state->hp_job);
        } else {
            if (!can_hp) ImGui::BeginDisabled();
            if (ImGui::Button("Run Hyperparam Sweep")) {
                // the click-time snapshot on the stack; the heap copy the worker frees is made only to launch (the
                // worker clears hp_results itself, under the lease — a refused start leaves the last sweep shown)
                HyperparamSweepWorkerArgs snap{};
                HyperparamSweepWorkerArgs *hp_args = &snap;
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
                hp_args->snap_wf_horizon_ticks = Training_ResolvePurgeForLabels(state, &run_control->run_config, hp_data);   // s5 leaf-16 + A2
                hp_args->snap_wf_buffer_ticks = state->wf_buffer_ticks;
                hp_args->snap_wf_min_train = state->wf_min_train;
                auto *heap = (HyperparamSweepWorkerArgs *)malloc(sizeof(HyperparamSweepWorkerArgs));
                if (!heap) {
                    SuiteWorker_ReportNotStarted("Hyperparam Sweep", START_CAUSE_NO_MEMORY, lf->launch_msg,
                                                 sizeof(lf->launch_msg));
                } else {
                    *heap = snap;
                    if (SuiteWorker_Launch("Hyperparam Sweep", &state->hp_job, hp_sweep_worker_fn, heap,
                                           lf->launch_msg, sizeof(lf->launch_msg)) != SUITE_LAUNCH_STARTED)
                        free(heap);
                }
            }
            if (!can_hp) {
                ImGui::EndDisabled();
                SuiteGate_ShowWhy(&hp_gate);
            }
        }

        // Results table — kind-aware (WF metric: accuracy or correlation)
        if (SuiteJob_Done(&state->hp_job) && state->hp_results.total_runs > 0) {
            // the keys the sweep recorded, never the live Key inputs above — editable after a sweep (D-507)
            const OptimizerResults *opt = &state->hp_results;
            ImGui::Separator();
            ImGui::Text("Best cell: %s=%.3f",
                        opt->keys[0],
                        opt->param_vals[0][opt->best_idx / opt->dims[1]]);
            if (opt->num_params > 1) {
                ImGui::SameLine();
                ImGui::Text(" %s=%.3f", opt->keys[1],
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
                ImGui::TableSetupColumn(opt->keys[0],
                                         ImGuiTableColumnFlags_WidthFixed, 120);
                if (opt->num_params > 1) {
                    ImGui::TableSetupColumn(opt->keys[1],
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
        ImGui::InputText("HMAC secret (empty = the cfg's)",
                         state->fv_auto_stamp_secret,
                         sizeof(state->fv_auto_stamp_secret));
        ImGui::PopItemWidth();
        ImGui::SetItemTooltip("Empty = the collected cfg's auto_stamp_secret (set it once\n"
                              "there), for both Train and Run Full Validation. Dev mode\n"
                              "only when that is empty too: the stamp is written but the\n"
                              "verifier accepts any signature on load (with a stderr\n"
                              "warn). Production: set a non-empty secret in BOTH the\n"
                              "suite (here or its cfg) and engine.cfg (held_out_stamp_secret),\n"
                              "and flip held_out_gate_strict=1 in engine.cfg to refuse\n"
                              "unsigned loads.");

        const BacktestResults *fv_data = &run_control->results;
        // its gate (D-507; Backtest/SuiteStartGates.hpp) — one run at a time (D-503); the model must be of the collected
        // labels' horizon (R1). The model's horizon is parsed from its path, a string only this thread writes; the
        // labels' is read from the run's record only while Run Control's outputs are at rest (RunControl_AtRest) —
        // mid-collect it passes as unknown and the gate's lease term says why, while a training run leaves the record at
        // rest, so a model of another horizon is named even while that run holds the lease (step 10.3's second review,
        // finding 6); a run that collected no samples has no labels' horizon
        const SuiteGate fv_gate =
            StartGate_FullValidation(START_GATE_BUILD_TRAINS, state->model_path[0] != '\0',
                                     ModelPath_HorizonOfModelFile(state->model_path),
                                     RunControl_AtRest(run_control)
                                         ? BacktestRunConfig_LabelsHorizon(&run_control->run_config, fv_data->sample_count)
                                         : 0,
                                     RunControl_DatasetSamples(run_control));
        const bool can_fv = SuiteGate_Open(&fv_gate);

        if (SuiteJob_Running(&state->fv_job)) {
            ImGui::ProgressBar(state->fv_job.progress / 100.0f, ImVec2(-1, 0),
                               "Full validation...");
            if (ImGui::Button("Cancel Full Validation"))
                SuiteJob_Cancel(&state->fv_job);
        } else {
            if (!can_fv) ImGui::BeginDisabled();
            if (ImGui::Button("Run Full Validation")) {
                // MP-1b — every input of the job is snapped HERE by its request builder (the v5.10.0E
                // click-time pattern; the label params from the run config, the rest from the panel).
                TrainingPanel_LaunchFullValidation(state, run_control, fv_data, lf);
            }
            if (!can_fv) {
                ImGui::EndDisabled();
                SuiteGate_ShowWhy(&fv_gate);
            }
        }
    }

    if (SuiteJob_Done(&state->fv_job)) {   // acquire: the job published the result before this flag
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

        // Auto-stamp result line. Sourced from the job's status line.
        // v5.11.36 — operator-flagged 2026-05-07: long status lines (e.g.
        // "Held-out OK; auto-stamp skipped — model_path='...'
        // non-empty but auto_stamp_path empty (internal copy failure;
        // report bug)") ran off the panel right edge on 1080p. Use
        // PushStyleColor + TextWrapped instead of TextColored so the
        // text wraps at panel width with color preserved.
        char fv_text[sizeof(TrainingStatusLine::text)];
        if (TrainingStatusLine_Read(&state->fv_status, fv_text, sizeof(fv_text)) && fv_text[0]) {
            ImVec4 stamp_col = fv->auto_stamp_attempted && fv->auto_stamp_ok
                ? ImVec4(0.55f, 0.76f, 0.51f, 1.0f)
                : ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, stamp_col);
            ImGui::TextWrapped("%s", fv_text);
            ImGui::PopStyleColor();
        }
    }

    // v5.10.0 Item A — per-phase timer breakdown. Read singleton state
    // populated by the latest backtest run. Header is collapsing — most
    // operators don't want it open by default. Only render when populated;
    // a never-run state has nothing useful to show.
    // TD-240 WIRED (2026-08-26): read the seqlock-PUBLISHED snapshot of the
    // last COMPLETED run — never the live PhaseTimer_Global() singleton (the
    // old direct read was the exact mid-run torn read the snapshot pair was
    // designed to prevent; parallel horizon workers += the global while this
    // renders). valid=0 until the first run publishes → header hidden.
    tt::PhaseTimerSnapshot pt;
    if (tt::PhaseTimer_ReadSnapshot(&pt) &&
        ImGui::CollapsingHeader("Phase Timing (last run)")) {
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
