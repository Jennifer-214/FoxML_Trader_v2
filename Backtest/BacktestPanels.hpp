// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BACKTEST PANELS]
//======================================================================================================
// Phase 1 panels: Data Browser, Run Control, Results
// follows existing panel pattern from DashboardPanels.hpp:
//   - each panel is a standalone ImGui window (dockable, rearrangeable)
//   - state structs are separate from render functions
//   - GUI never calls engine functions directly (reads display structs only)
//======================================================================================================
#ifndef BACKTEST_PANELS_HPP
#define BACKTEST_PANELS_HPP

#include "imgui.h"
#include "BacktestEngine.hpp"
#include "BacktestSharded.hpp"  // phase 13: per-core sharded backtest path
#include "Fingerprint.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>

//======================================================================================================
// [DATA PANEL STATE]
//======================================================================================================
// scan cap for the Data panel — must be ≥ MAX_DATA_FILES so the GUI doesn't
// silently truncate before the run_config buffer fills. paired with Limits.hpp.
#define DATA_MAX_FILES 2048

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

static inline void DataPanel_Init(DataPanelState *state) {
    memset(state, 0, sizeof(*state));
    strncpy(state->data_dir, "data/", sizeof(state->data_dir) - 1);
}

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

//======================================================================================================
// [SAMPLES SNAPSHOT — thread-safe display struct]
//======================================================================================================
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

//======================================================================================================
// [RUN CONTROL STATE]
//======================================================================================================
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

static inline void RunControl_Init(RunControlState *state) {
    memset(state, 0, sizeof(*state));
    strncpy(state->config_path, "backtest.cfg", sizeof(state->config_path) - 1);
    BacktestResults_Init(&state->results);
}

// Compute distribution stats from results->labels[] into a SamplesSnapshot.
// MUST only be called when no other thread is writing to results->labels —
// i.e. by the worker thread AFTER Backtest_Run has populated labels in the
// post-pass, BEFORE running=0 is set. The GUI thread reads the snapshot
// only when running==0, giving a safe happens-before relationship.
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

// worker thread function
struct BacktestWorkerArgs {
    RunControlState *state;
};

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

//======================================================================================================
// [PANEL: DATA BROWSER]
//======================================================================================================
static inline void GUI_Panel_DataBrowser(DataPanelState *state) {
    ImGui::Begin("Data");

    ImGui::InputText("Directory", state->data_dir, sizeof(state->data_dir));
    ImGui::SameLine();
    if (ImGui::Button("Scan") || !state->scanned)
        DataPanel_Scan(state);

    if (state->file_count == 0) {
        ImGui::TextDisabled("No CSV files found in %s", state->data_dir);
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

//======================================================================================================
// [PANEL: RUN CONTROL]
//======================================================================================================
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

//======================================================================================================
// [PANEL: RESULTS]
//======================================================================================================
static inline ImVec4 ResultsPnlColor(double v) {
    return v >= 0.0 ? ImVec4(0.55f, 0.76f, 0.51f, 1.0f)    // foxml green
                    : ImVec4(0.82f, 0.47f, 0.47f, 1.0f);    // foxml red
}

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

//======================================================================================================
// [COMPARISON STATE]
//======================================================================================================
#define COMPARISON_MAX_RUNS 8

struct ComparisonState {
    BacktestStats stats[COMPARISON_MAX_RUNS];
    double *equity_curves[COMPARISON_MAX_RUNS];   // dynamic per-run snapshots
    int     equity_counts[COMPARISON_MAX_RUNS];
    char    labels[COMPARISON_MAX_RUNS][64];
    int run_count;
};

//==========================================================================
// PAST RUNS VIEWER (v4.3) — scan models/{run_name}/ subdirs, parse the
// summary.txt + expected.cfg in each, render a sortable table for easy
// comparison across saved runs. Differs from ComparisonState (in-memory
// equity curves only) — Past Runs persists across restarts, captures ML
// metrics specifically (accuracy, val acc, label kind, hyperparams).
//==========================================================================
#define PAST_RUNS_MAX 64

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
};

struct PastRunsState {
    PastRun runs[PAST_RUNS_MAX];
    int     count;
    int     selected;            // index of last clicked row (for inspector / actions)
    char    status_msg[256];     // last action status (e.g., "loaded", "deleted")
    int     sort_column;         // 0..N-1, which column to sort by
    int     sort_descending;     // 0 = asc, 1 = desc
};

static inline void PastRuns_Init(PastRunsState *s) {
    memset(s, 0, sizeof(*s));
    s->selected = -1;
    s->sort_column = 6;          // default sort by val_accuracy descending
    s->sort_descending = 1;
}

// helper: parse a key=value line into a (key, value) pair via simple split.
// returns 1 on success, 0 if line doesn't contain '='.
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

// scan one run directory's metadata files
static inline int PastRuns_LoadOne(PastRun *r, const char *run_dir) {
    memset(r, 0, sizeof(*r));
    const char *base = strrchr(run_dir, '/');
    base = base ? base + 1 : run_dir;
    strncpy(r->dir_name, base, sizeof(r->dir_name) - 1);

    char path[400];
    char line[512];

    // summary.txt
    snprintf(path, sizeof(path), "%s/summary.txt", run_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;  // no summary = not a Save Run bundle
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
        const char *role_files[] = {"barrier.json", "buy_signal.json", "regime.json",
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

// v4.3 — scan one directory for run subdirs containing summary.txt. Used
// recursively for the two-level models/{kind}/{run_name}/ layout AND for
// backward compat with flat models/{run_name}/ runs from before v4.3.
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
        if (PastRuns_LoadOne(&s->runs[s->count], sub)) s->count++;
    }
    closedir(d);
}

static inline void PastRuns_Scan(PastRunsState *s) {
    s->count = 0;
    s->status_msg[0] = '\0';
    // v4.3 — walk the kind-organized subdirs first
    PastRuns_ScanOneDir(s, "models/classification");
    PastRuns_ScanOneDir(s, "models/regression");
    // Backward compat: also scan models/ directly for runs saved before v4.3
    // (those still have summary.txt at models/{run_name}/).
    PastRuns_ScanOneDir(s, "models");
    snprintf(s->status_msg, sizeof(s->status_msg),
             "scanned %d run(s) in models/{classification,regression,...}",
             s->count);
}

// label-type-aware metric label
static inline const char* PastRun_MetricLabel(int expected_num_classes) {
    if (expected_num_classes == 1) return "Corr (r)";   // regression
    if (expected_num_classes >= 2) return "Acc (multi)";// multiclass
    return "Acc (bin)";                                  // binary (0)
}

static inline void GUI_Panel_PastRuns(PastRunsState *s) {
    ImGui::Begin("Past Runs");
    SectionHeader("PAST RUNS");

    if (ImGui::Button("Rescan")) PastRuns_Scan(s);
    ImGui::SameLine();
    if (s->status_msg[0])
        ImGui::TextColored(FoxmlColors::comment, "(%s)", s->status_msg);

    if (s->count == 0) {
        ImGui::TextDisabled("No saved runs found in models/. "
                            "Train a model and click 'Save Run' in the Training panel.");
        ImGui::End();
        return;
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
    auto render_run_cell = [&](int i) {
        PastRun *r = &s->runs[i];
        ImGui::TableSetColumnIndex(0);
        char rowid[200];
        // v5.8.9 — surface stamp presence inline in the run name. "[stamped]"
        // prefix means a .stamp file exists alongside the saved model;
        // operators can filter visually for deploy-ready runs without a
        // separate column. Verify Stamp button (inspector below) actually
        // validates signature + drift hash.
        const char *stamp_tag = r->has_stamp ? "[stamped] " : "";
        snprintf(rowid, sizeof(rowid), "%s%s##run%d", stamp_tag, r->dir_name, i);
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
            } else if (ImGui::BeginTable("past_runs_class", 12, flags)) {
                ImGui::TableSetupColumn("Run",        ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 220);
                ImGui::TableSetupColumn("Role",       ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Label",      ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("Classes",    ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("TP bps",     ImGuiTableColumnFlags_WidthFixed, 75);
                ImGui::TableSetupColumn("SL bps",     ImGuiTableColumnFlags_WidthFixed, 75);
                ImGui::TableSetupColumn("Lookahead",  ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Train Acc",  ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Val Acc",    ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Gap",        ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Overfit",    ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Depth/LR/N", ImGuiTableColumnFlags_WidthFixed, 120);
                ImGui::TableHeadersRow();

                for (int i = 0; i < s->count; ++i) {
                    PastRun *r = &s->runs[i];
                    if (r->label_kind == 1) continue;  // skip regression runs
                    ImGui::TableNextRow();

                    render_run_cell(i);
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

                    ImGui::TableNextColumn();
                    ImGui::Text("%d/%.2f/%d", r->max_depth, r->learning_rate, r->n_estimators);
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
            } else if (ImGui::BeginTable("past_runs_regr", 11, flags)) {
                ImGui::TableSetupColumn("Run",        ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 220);
                ImGui::TableSetupColumn("Role",       ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Label",      ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("TP bps",     ImGuiTableColumnFlags_WidthFixed, 75);
                ImGui::TableSetupColumn("SL bps",     ImGuiTableColumnFlags_WidthFixed, 75);
                ImGui::TableSetupColumn("Lookahead",  ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Train r",    ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Val r",      ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Val MSE",    ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableSetupColumn("Gap (r)",    ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Depth/LR/N", ImGuiTableColumnFlags_WidthFixed, 120);
                ImGui::TableHeadersRow();

                for (int i = 0; i < s->count; ++i) {
                    PastRun *r = &s->runs[i];
                    if (r->label_kind != 1) continue;  // only regression
                    ImGui::TableNextRow();

                    render_run_cell(i);
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
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
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

        // Path hint for engine.cfg
        ImGui::TextColored(FoxmlColors::sand,
            "To use in engine: set core_N_model_dir=models/%s/ in engine.cfg",
            r->dir_name);

        if (ImGui::Button("Open Folder Path")) {
            // copy the path to status_msg as a hint (no shell exec from here)
            snprintf(s->status_msg, sizeof(s->status_msg),
                     "models/%s/", r->dir_name);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete (manual)")) {
            // safety: don't actually rm here. show user the command to run.
            snprintf(s->status_msg, sizeof(s->status_msg),
                     "to delete: rm -r models/%s/", r->dir_name);
        }

        // v5.8.9 — Verify Stamp: runs verify_model_stamp on the saved
        // model's .stamp file using the current build's
        // FEATURE_REGISTRY_HASH() so the operator can confirm match
        // (signature valid + format version + drift hash) before
        // deploying. Same code path the live engine fires at boot.
        if (r->has_stamp) {
            ImGui::SameLine();
            if (ImGui::Button("Verify Stamp")) {
                // Try common role files in priority order — same shape as
                // PastRuns_LoadOne's stat() check above.
                const char *role_files[] = {
                    "barrier.json", "buy_signal.json", "regime.json",
                    "barrier.xgb",  "buy_signal.xgb",  "regime.xgb",
                    NULL
                };
                char model_path[512];
                const char *found = NULL;
                for (int i = 0; role_files[i]; ++i) {
                    snprintf(model_path, sizeof(model_path), "models/%s/%s",
                             r->dir_name, role_files[i]);
                    struct stat mst;
                    if (stat(model_path, &mst) == 0) {
                        found = model_path;
                        break;
                    }
                }
                if (!found) {
                    snprintf(r->stamp_verify_msg, sizeof(r->stamp_verify_msg),
                        "no model file found in models/%s/", r->dir_name);
                    r->stamp_verify_state = -1;
                } else {
                    // Empty secret = devmode (accepts any signature, just
                    // checks format version + sha + registry hash).
                    // Operators verifying for deploy should set the secret
                    // here once we expose a UI input — for now devmode keeps
                    // the button low-friction.
                    ModelStampResult vr = verify_model_stamp(
                        found, /*secret=*/"",
                        /*gap_threshold=*/(double)r->gap_acceptable_threshold > 0.0
                            ? (double)r->gap_acceptable_threshold : 0.05,
                        /*expected_format_version=*/MODEL_FORMAT_VERSION,
                        /*expected_feature_registry_hash=*/FEATURE_REGISTRY_HASH());
                    r->stamp_verify_state = vr.valid;
                    // v5.9.5d — capture full result for "Stamp details"
                    // expansion below. Operator audits recorded vs runtime
                    // cfg without leaving the suite.
                    r->stamp_verify_full = vr;
                    r->stamp_verify_has_full = (vr.valid == 1) ? 1 : 0;
                    if (vr.valid == 1) {
                        snprintf(r->stamp_verify_msg, sizeof(r->stamp_verify_msg),
                            "OK — engine=%s registry=%016lx",
                            vr.engine_version[0] ? vr.engine_version : "unknown",
                            (unsigned long)vr.feature_registry_hash);
                    } else {
                        snprintf(r->stamp_verify_msg, sizeof(r->stamp_verify_msg),
                            "FAIL — %s", vr.reason);
                    }
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
                    if (v.has_model_num_outputs) {
                        ImGui::Text("model_num_outputs: %d", v.model_num_outputs);
                    }
                    // v5.9.4a training_poll_interval (cadence)
                    if (v.has_training_poll_interval) {
                        ImGui::Text("training_poll:    %u",
                                    (unsigned)v.training_poll_interval);
                    }
                    // v5.9.3a scaler binding
                    if (v.has_scaler_fields) {
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
                    if (v.has_inference_cfg) {
                        ImGui::Separator();
                        ImGui::TextColored(FoxmlColors::comment,
                                           "Recorded cfg at training time:");
                        ImGui::Text("  confidence_threshold_scale:       %.4g",
                                    v.inference_cfg_confidence_threshold_scale);
                        ImGui::Text("  barrier_gate_enabled:             %d",
                                    v.inference_cfg_barrier_gate_enabled);
                        ImGui::Text("  confidence_hard_block_threshold:  %.4g",
                                    v.inference_cfg_confidence_hard_block_threshold);
                        ImGui::Text("  held_out_fraction:                %.3f",
                                    v.inference_cfg_held_out_fraction);
                        ImGui::Text("  freshness_tau:                    %.1f",
                                    v.inference_cfg_freshness_tau);
                        if (v.has_inference_cfg_bandit) {
                            ImGui::Text("  bandit_blend_ratio:               %.4g",
                                        v.inference_cfg_bandit_blend_ratio);
                        }
                        if (v.has_inference_cfg_fees) {
                            ImGui::Text("  fee_rate_maker / taker:           %.5f / %.5f",
                                        v.inference_cfg_fee_rate_maker,
                                        v.inference_cfg_fee_rate_taker);
                        }
                    }
                    ImGui::TreePop();
                }
            }
        }
    }

    ImGui::End();
}

static inline void Comparison_Init(ComparisonState *state) {
    memset(state, 0, sizeof(*state));
}

static inline void Comparison_Free(ComparisonState *state) {
    for (int i = 0; i < COMPARISON_MAX_RUNS; i++) {
        free(state->equity_curves[i]);
        state->equity_curves[i] = NULL;
    }
}

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

//======================================================================================================
// [PANEL: COMPARISON]
//======================================================================================================
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

//======================================================================================================
// [OPTIMIZER PANEL STATE]
//======================================================================================================
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

static inline void OptimizerPanel_Init(OptimizerPanelState *state) {
    memset(state, 0, sizeof(*state));
    state->num_params = 1;
    state->metric_idx = OPT_METRIC_PNL;
    strncpy(state->ranges[0].key, "take_profit_pct", 31);
    state->ranges[0].lo = 1.0; state->ranges[0].hi = 5.0; state->ranges[0].step = 0.5;
    strncpy(state->ranges[1].key, "stop_loss_pct", 31);
    state->ranges[1].lo = 0.5; state->ranges[1].hi = 3.0; state->ranges[1].step = 0.5;
    strncpy(state->config_path, "engine.cfg", sizeof(state->config_path) - 1);
}

struct OptWorkerArgs {
    OptimizerPanelState *state;
};

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

//======================================================================================================
// [PANEL: OPTIMIZER]
//======================================================================================================
static inline void GUI_Panel_Optimizer(OptimizerPanelState *state, DataPanelState *data) {
    ImGui::Begin("Optimizer");

    // parameter config
    static const char *metric_names[] = {"Sharpe", "Profit Factor", "Expectancy", "Return %", "P&L $"};
    ImGui::Combo("Metric", &state->metric_idx, metric_names, 5);

    ImGui::SliderInt("Parameters", &state->num_params, 1, 2);

    for (int p = 0; p < state->num_params; p++) {
        ImGui::PushID(p);
        char hdr[32]; snprintf(hdr, sizeof(hdr), "Param %d", p + 1);
        if (ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen)) {
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

//======================================================================================================
// [TRAINING PANEL STATE]
//======================================================================================================
struct TrainingPanelState {
    // XGBoost hyperparameters
    int max_depth;
    float learning_rate;
    int n_estimators;
    int label_type;
    float label_tp_pct;
    float label_sl_pct;
    int label_forward_ticks;
    // results
    float feature_importance[MODEL_MAX_FEATURES];
    char feature_names[MODEL_MAX_FEATURES][32];
    char model_path[256];
    bool model_trained;
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
    // Populated by train_model_worker_fn post-FeatureStandardizer_Persist;
    // empty when scaler not persisted. Single-writer (worker) → reader (UI
    // render after tm_complete=1 flips). Pre-v5.9.5d the SHA was only
    // logged via stderr — operator couldn't see it in foxml_suite.
    char scaler_sha256_hex[80];
    // walk-forward validation (Phase 6A — A7 GUI rework)
    int wf_n_splits;          // number of temporal folds (default 5)
    int wf_horizon_ticks;     // label horizon for purge gap calc (default 1000)
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
};

static inline void TrainingPanel_Init(TrainingPanelState *state) {
    memset(state, 0, sizeof(*state));
    state->max_depth = 4;
    state->learning_rate = 0.1f;
    state->n_estimators = 100;
    state->label_type = LABEL_WIN_LOSS;
    state->label_tp_pct = 1.5f;
    state->label_sl_pct = 1.0f;
    strncpy(state->run_name, "run_01", sizeof(state->run_name) - 1);
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
    state->wf_horizon_ticks = 1000;
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
}

// walk-forward worker thread
struct WalkForwardWorkerArgs {
    TrainingPanelState *state;
    const BacktestResults *data;
};

static inline void *walkforward_worker_fn(void *arg) {
    WalkForwardWorkerArgs *args = (WalkForwardWorkerArgs *)arg;
    TrainingPanelState *state = args->state;
    const BacktestResults *data = args->data;
    free(args);

    Backtest_RunWalkForward(&state->wf_results, data,
                             state->wf_n_splits, state->wf_horizon_ticks,
                             state->wf_buffer_ticks, state->wf_min_train,
                             &state->wf_progress, &state->wf_cancel,
                             state->label_type);

    state->wf_has_results = true;
    state->wf_complete = 1;
    state->wf_running = 0;
    return NULL;
}

// v5.8.7 — full-validation worker thread. Mirrors walkforward_worker_fn but
// calls Backtest_RunFullValidation, which carries the v5.8.6 auto-stamp
// wiring (FEATURE_REGISTRY_HASH + engine_version embedded in stamp body).
struct FullValidationWorkerArgs {
    TrainingPanelState *state;
    const BacktestResults *data;
};

static inline void *fullvalidation_worker_fn(void *arg) {
    FullValidationWorkerArgs *args = (FullValidationWorkerArgs *)arg;
    TrainingPanelState *state = args->state;
    const BacktestResults *data = args->data;
    free(args);

    // Build held-out split and unlock immediately. The friction-grade lock
    // exists to make held-out access a deliberate operator action; the
    // suite UI's Run Full Validation button is exactly that deliberate
    // action, so unlocking here is correct.
    HeldOutSplit split = HeldOutSplit_Make(data->sample_count,
                                            (double)state->fv_held_out_fraction);
    char unlock_token[33];
    memcpy(unlock_token, split.lock_token, sizeof(unlock_token));
    HeldOutSplit_Unlock(&split, unlock_token);

    // Pre-populate auto-stamp request fields. Backtest_RunFullValidation
    // gates the stamp_write_for_model call on auto_stamp_path being non-empty
    // AND ran_held_out=1; both are met here when training succeeds.
    //
    // v5.8.10 — gate path-setting on the cfg's auto_stamp_on_held_out flag.
    // When the operator runs the suite with auto_stamp_on_held_out=0 (intent:
    // manual stamping via tools/stamp_model.sh), the FV button still runs
    // held-out validation but skips the stamp write. Honors operator intent.
    memset(&state->fv_results, 0, sizeof(state->fv_results));
    int auto_stamp_enabled = data->config_used.auto_stamp_on_held_out;
    if (auto_stamp_enabled) {
        size_t n = strlen(state->model_path);
        if (n >= sizeof(state->fv_results.auto_stamp_path))
            n = sizeof(state->fv_results.auto_stamp_path) - 1;
        memcpy(state->fv_results.auto_stamp_path, state->model_path, n);
        state->fv_results.auto_stamp_path[n] = '\0';
    }
    {
        size_t n = strlen(state->fv_auto_stamp_secret);
        if (n >= sizeof(state->fv_results.auto_stamp_secret))
            n = sizeof(state->fv_results.auto_stamp_secret) - 1;
        memcpy(state->fv_results.auto_stamp_secret, state->fv_auto_stamp_secret, n);
        state->fv_results.auto_stamp_secret[n] = '\0';
    }
    state->fv_results.auto_stamp_format_version = 0;  // 0 = use MODEL_FORMAT_VERSION

    Backtest_RunFullValidation(&state->fv_results, data, &split,
                                state->wf_n_splits, state->wf_horizon_ticks,
                                state->wf_buffer_ticks, state->wf_min_train,
                                &state->fv_progress, &state->fv_cancel,
                                state->label_type, state->fv_gap_threshold);

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
            // Now show what state was actually observed.
            if (state->model_path[0] == '\0') {
                snprintf(state->fv_status_msg, sizeof(state->fv_status_msg),
                    "Held-out OK; auto-stamp skipped — model_path empty "
                    "(set Model Path field before Run Full Validation)");
            } else if (state->fv_results.auto_stamp_path[0] == '\0') {
                snprintf(state->fv_status_msg, sizeof(state->fv_status_msg),
                    "Held-out OK; auto-stamp skipped — model_path='%s' did not "
                    "propagate to auto_stamp_path (worker race or copy failure)",
                    state->model_path);
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

// v5.9.0d — Train Model worker thread (V5_9_AUDIT-#7).
// Pre-v5.9.0d: Train Model ran SYNCHRONOUSLY on the UI thread, freezing
// the GUI 5-30s during XGBoost training. The synchronous-train warning
// at GUI_Panel_Training documented this as a known UX gap.
//
// Worker pattern mirrors fullvalidation_worker_fn (v5.8.7). Audit walked
// the body's race surface (DOCS/V5_9_ML_HARDENING_AUDIT.md follow-up):
//   - Snapshot user-modifiable state at entry (max_depth, lr, n_estimators,
//     label_type, model_path). UI can change these mid-train without
//     affecting the running worker.
//   - tm_cancel polled between XGBoosterUpdateOneIter calls. XGBoost
//     has no mid-iteration cancel API; bounded latency = one iter time.
//   - All exit paths set tm_complete=1 + tm_running=0 (including malloc
//     failure + cancel paths).
//   - state->{train_accuracy, status_msg, model_trained, train_mse,
//     train_correlation, feature_importance} are written by worker;
//     UI render reads after tm_complete flag flips. x86 aligned-atomic
//     for these primitives matches v5.8.7 fv_* pattern.
struct TrainModelWorkerArgs {
    TrainingPanelState *state;
    const RunControlState *run_control;
};

static inline void *train_model_worker_fn(void *arg) {
    TrainModelWorkerArgs *args = (TrainModelWorkerArgs *)arg;
    TrainingPanelState *state = args->state;
    const RunControlState *run_control = args->run_control;
    free(args);

    const BacktestResults *results = &run_control->results;

    // v5.9.0d — snapshot user-modifiable state at worker entry. UI can
    // change these mid-train via ImGui inputs; the worker uses the values
    // captured at click time, not mid-train.
    int snap_max_depth      = state->max_depth;
    float snap_learning_rate = state->learning_rate;
    int snap_n_estimators   = state->n_estimators;
    int snap_label_type     = state->label_type;
    char snap_model_path[256];
    {
        size_t n = strlen(state->model_path);
        if (n >= sizeof(snap_model_path)) n = sizeof(snap_model_path) - 1;
        memcpy(snap_model_path, state->model_path, n);
        snap_model_path[n] = '\0';
    }

#ifdef USE_XGBOOST
    // log to stderr so the Log panel shows the user "training started"
    fprintf(stderr, "[TRAIN] starting on %d samples (label_type=%d, "
                    "max_depth=%d, lr=%.3f, n_est=%d)...\n",
            results->sample_count, snap_label_type,
            snap_max_depth, snap_learning_rate, snap_n_estimators);
    fflush(stderr);

    // create output directory
    mkdir("models", 0755);

    // filter out neutral labels (0.5) — XGBoost binary needs 0 or 1
    int n_valid = 0;
    float *train_features = (float *)malloc(results->sample_count * MODEL_NUM_FEATURES * sizeof(float));
    float *train_labels   = (float *)malloc(results->sample_count * sizeof(float));
    if (!train_features || !train_labels) {
        free(train_features); free(train_labels);
        snprintf(state->status_msg, sizeof(state->status_msg), "Failed to allocate training buffers");
        state->model_trained = true;  // show the error message
        state->tm_complete = 1;
        state->tm_running = 0;
        return NULL;
    }

    for (int i = 0; i < results->sample_count; i++) {
        if (results->labels[i] == 0.5f) continue;
        memcpy(&train_features[n_valid * MODEL_NUM_FEATURES],
               &results->feature_matrix[i * MODEL_NUM_FEATURES],
               MODEL_NUM_FEATURES * sizeof(float));
        train_labels[n_valid] = results->labels[i];
        n_valid++;
    }

    DMatrixHandle dtrain;
    XGDMatrixCreateFromMat(train_features, n_valid, MODEL_NUM_FEATURES, NAN, &dtrain);
    XGDMatrixSetFloatInfo(dtrain, "label", train_labels, n_valid);

    BoosterHandle booster;
    XGBoosterCreate(&dtrain, 1, &booster);

    char depth_s[8]; snprintf(depth_s, 8, "%d", snap_max_depth);
    char lr_s[16]; snprintf(lr_s, 16, "%f", snap_learning_rate);
    XGBoosterSetParam(booster, "max_depth", depth_s);
    XGBoosterSetParam(booster, "eta", lr_s);

    int num_classes = (snap_label_type >= 0 && snap_label_type < LABEL_COUNT)
                      ? label_table[snap_label_type].num_classes : 0;
    int is_multiclass  = (num_classes >= 2);
    int is_regression  = (num_classes == 1);

    if (is_multiclass) {
        XGBoosterSetParam(booster, "objective", "multi:softprob");
        char nc_s[8]; snprintf(nc_s, 8, "%d", num_classes);
        XGBoosterSetParam(booster, "num_class", nc_s);
        float *mc_weights = (float *)malloc(n_valid * sizeof(float));
        int   mc_counts[16] = {0};
        if (mc_weights) {
            XGBoost_ComputeMulticlassWeights(train_labels, n_valid, num_classes,
                                              mc_weights, mc_counts);
            XGDMatrixSetFloatInfo(dtrain, "weight", mc_weights, n_valid);
            fprintf(stderr, "[TRAIN] multiclass class counts:");
            for (int k = 0; k < num_classes && k < 16; k++) {
                fprintf(stderr, " c%d=%d (%.1f%%)", k, mc_counts[k],
                        n_valid > 0 ? 100.0f * mc_counts[k] / n_valid : 0.0f);
            }
            fprintf(stderr, " — per-sample weights applied\n");
            fflush(stderr);
            free(mc_weights);
        }
    } else if (is_regression) {
        XGBoosterSetParam(booster, "objective", "reg:squarederror");
    } else {
        XGBoosterSetParam(booster, "objective", "binary:logistic");
        int n_pos = 0, n_neg = 0;
        double spw = XGBoost_ComputeScalePosWeight(train_labels, n_valid, &n_pos, &n_neg);
        char spw_s[24]; snprintf(spw_s, sizeof(spw_s), "%.4f", spw);
        XGBoosterSetParam(booster, "scale_pos_weight", spw_s);
        fprintf(stderr, "[TRAIN] class balance: +%d / -%d → scale_pos_weight=%s%s\n",
                n_pos, n_neg, spw_s,
                n_pos == 0 ? "  WARNING: zero positives, model cannot learn" : "");
        fflush(stderr);
    }
    XGBoosterSetParam(booster, "nthread", "4");
    XGBoosterSetParam(booster, "verbosity", "0");

    // v5.9.0d — iteration loop with tm_cancel poll. XGBoost has no
    // mid-iteration cancel; cancel response bounded by one iter time
    // (typically 100ms-1s for typical hyperparameters).
    int cancelled = 0;
    for (int i = 0; i < snap_n_estimators; i++) {
        if (state->tm_cancel) {
            cancelled = 1;
            fprintf(stderr, "[TRAIN] cancelled at iteration %d/%d\n", i, snap_n_estimators);
            fflush(stderr);
            break;
        }
        XGBoosterUpdateOneIter(booster, i, dtrain);
    }

    if (cancelled) {
        XGDMatrixFree(dtrain);
        XGBoosterFree(booster);
        free(train_features);
        free(train_labels);
        snprintf(state->status_msg, sizeof(state->status_msg),
                 "Training cancelled by user");
        state->model_trained = false;
        state->tm_complete = 1;
        state->tm_running = 0;
        return NULL;
    }

    // embed model format version + fingerprint
    char ver_s[8]; snprintf(ver_s, 8, "%d", MODEL_FORMAT_VERSION);
    XGBoosterSetAttr(booster, "foxml_version", ver_s);
    {
        const char *fp_paths[MAX_DATA_FILES];
        for (int i = 0; i < run_control->run_config.num_data_files && i < MAX_DATA_FILES; i++)
            fp_paths[i] = run_control->run_config.data_paths[i];
        char fp_hex[65];
        Fingerprint_Compute<BACKTEST_FP>(fp_hex, &results->config_used,
            sizeof(results->config_used), fp_paths, run_control->run_config.num_data_files);
        XGBoosterSetAttr(booster, "foxml_fingerprint", fp_hex);
        fprintf(stderr, "[TRAIN] model fingerprint: %.12s...\n", fp_hex);
    }

    XGBoosterSaveModel(booster, snap_model_path);

    // compute in-sample training metric
    bst_ulong out_len;
    const float *out_result;
    DMatrixHandle dpred;
    XGDMatrixCreateFromMat(train_features, n_valid, MODEL_NUM_FEATURES, NAN, &dpred);
    XGBoosterPredict(booster, dpred, 0, 0, 0, &out_len, &out_result);
    if (is_multiclass) {
        state->train_accuracy = WalkForward_ComputeMulticlassAccuracy(
            out_result, train_labels, n_valid, num_classes) * 100.0f;
        state->train_mse = 0.0f;
        state->train_correlation = 0.0f;
    } else if (is_regression) {
        state->train_mse         = WalkForward_ComputeMSE(out_result, train_labels, n_valid);
        state->train_correlation = WalkForward_ComputeCorrelation(out_result, train_labels, n_valid);
        state->train_accuracy    = 0.0f;
    } else {
        state->train_accuracy = WalkForward_ComputeAccuracy(
            out_result, train_labels, n_valid, 0.5f) * 100.0f;
        state->train_mse = 0.0f;
        state->train_correlation = 0.0f;
    }
    XGDMatrixFree(dpred);

    memset(state->feature_importance, 0, sizeof(state->feature_importance));

    // v5.9.3b — train-time scaler computation + sidecar persist (Gap G).
    // Reads train_features (still alive at this point, freed below).
    // Atomic ordering: Compute → Persist → SHA-256 hex → log to operator.
    // Operator runs tools/stamp_model.sh --feature-scaler-present=1
    // --scaler-sha256=<hex> (or sets auto_stamp_path in cfg for next run)
    // to bind the sidecar to the model's stamp.
    int scaler_persisted = 0;
    char scaler_sha256_hex[80] = {0};
    char scaler_path[600] = {0};
    {
        tt::FeatureStandardizer scaler;
        tt::FeatureStandardizer_Compute(&scaler, train_features, n_valid,
                                          tt::SCALER_STDDEV_FLOOR);
        snprintf(scaler_path, sizeof(scaler_path), "%s.scaler", snap_model_path);
        if (tt::FeatureStandardizer_Persist(&scaler, scaler_path)) {
            scaler_persisted = 1;
            // Compute SHA-256 of the on-disk file (verifies it landed;
            // don't trust in-memory compute). Hex output for stamp.
            if (!tt::sha256_file_hex_inproc(scaler_path, scaler_sha256_hex,
                                              sizeof(scaler_sha256_hex))) {
                fprintf(stderr, "[train] scaler persisted but SHA-256 read failed\n");
                scaler_sha256_hex[0] = '\0';
            } else {
                fprintf(stderr, "[train] scaler persisted: %s\n"
                                "[train] scaler_sha256=%s — pass to stamp tool to bind\n",
                        scaler_path, scaler_sha256_hex);
            }
            // v5.9.5d — copy SHA into shared state for GUI display.
            // MUST happen before tm_complete=1 flag flip below; worker→UI
            // ordering follows the v5.9.0c pattern (single-writer worker
            // publishes via flag, UI reads after flag observed). x86
            // aligned-byte writes are atomic; the volatile completion flag
            // forces no-reordering at the compiler level.
            size_t hex_n = strnlen(scaler_sha256_hex, sizeof(scaler_sha256_hex));
            if (hex_n >= sizeof(state->scaler_sha256_hex))
                hex_n = sizeof(state->scaler_sha256_hex) - 1;
            memcpy(state->scaler_sha256_hex, scaler_sha256_hex, hex_n);
            state->scaler_sha256_hex[hex_n] = '\0';
        } else {
            // Gap G atomic contract: persist failure must not propagate to
            // stamp claiming scaler. Worker doesn't emit stamps directly,
            // but log loudly so operator doesn't manually bind a missing file.
            fprintf(stderr,
                "[train] [WARN] scaler persist FAILED; train_features have "
                "non-degenerate stats but no .scaler on disk. Do not stamp "
                "this model with feature_scaler_present=1.\n");
        }
    }

    XGDMatrixFree(dtrain);
    XGBoosterFree(booster);
    free(train_features);
    free(train_labels);

    // v5.9.4a — Gap I full closure: auto-unlink orphan scaler on cancel.
    // Pre-v5.9.4a (v5.9.3b) just logged "operator must rm orphan"; now we
    // unlink automatically since the model file itself is also discarded
    // on cancel (no stamp written by this worker → no consumer for the
    // sidecar). Atomic + idempotent: unlink failure (file already gone)
    // is non-fatal.
    if (state->tm_cancel && scaler_persisted) {
        if (unlink(scaler_path) == 0) {
            fprintf(stderr,
                "[train] cancelled post-scaler-persist; auto-removed orphan %s\n",
                scaler_path);
        } else {
            // unlink failed — file may already be gone (concurrent cleanup),
            // disk error, or permissions. Best-effort; don't crash worker.
            fprintf(stderr,
                "[train] cancelled post-scaler-persist; could not auto-remove %s "
                "(may already be cleaned)\n", scaler_path);
        }
    }

    state->model_trained = true;
    // v5.9.5d — status line ends with "next: Run Full Validation to
    // auto-stamp" so operator knows the workflow continuation. Train Model
    // alone produces an unstamped model (no held-out metric → no
    // generalization gap → engine refuses load in strict mode); Run Full
    // Validation produces the stamp.
    const char *next_hint = " — next: Run Full Validation to auto-stamp";
    if (is_regression) {
        snprintf(state->status_msg, sizeof(state->status_msg),
                 "Model saved to %s (MSE: %.6f, corr: %.4f)%s%s",
                 snap_model_path, state->train_mse, state->train_correlation,
                 scaler_persisted ? " [+scaler]" : "", next_hint);
    } else {
        snprintf(state->status_msg, sizeof(state->status_msg),
                 "Model saved to %s (accuracy: %.1f%%)%s%s",
                 snap_model_path, state->train_accuracy,
                 scaler_persisted ? " [+scaler]" : "", next_hint);
    }
#endif  // USE_XGBOOST

    state->tm_complete = 1;
    state->tm_running = 0;
    return NULL;
}

//======================================================================================================
// [PANEL: TRAINING]
//======================================================================================================
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
    int prev_label_type = state->label_type;
    ImGui::Combo("Label Type", &state->label_type, label_names, LABEL_COUNT);
    // v4.2.2: when label type changes, retarget the default Model Path so it
    // matches the role this label trains. Pre-patch, the path stayed at the
    // legacy "models/buy_signal.json" regardless of label type — confusing
    // since 3-class barrier models would then save under a binary-role name
    // (Save Run still rewrote it to barrier.json on bundle, but the in-progress
    // training output had the wrong filename). Now the field tracks the role.
    if (state->label_type != prev_label_type) {
        const char* role = "buy_signal";  // legacy / binary default
        if (state->label_type == LABEL_PEAK_VALLEY_STABLE) role = "barrier";
        else if (state->label_type == LABEL_REGIME)       role = "regime";
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
        // 3 decimals + 0.01 step lets users dial in tight barriers (5 bps = 0.050)
        // for short lookahead horizons where 0.1+ rarely triggers. Pre-fix, the
        // 0.1 step + "%.1f" format clamped at 0.1 — anything tighter rounded to 0.
        ImGui::InputFloat("TP Barrier %", &state->label_tp_pct, 0.01f, 0.1f, "%.3f");
        ImGui::SetItemTooltip("Take-profit barrier as %% of price\n"
                              "label = 1 (or VALLEY for 3-class) if price moves up this much before SL is hit\n"
                              "wider = fewer but higher-confidence labels\n"
                              "tip: 0.050 = 5 bps. For short horizons (~1k ticks) at BTC scale,\n"
                              "0.05-0.10%% gives balanced labels; 0.3+ usually = 99%% \"stable\".");
        ImGui::InputFloat("SL Barrier %", &state->label_sl_pct, 0.01f, 0.1f, "%.3f");
        ImGui::SetItemTooltip("Stop-loss barrier as %% of price\n"
                              "label = 0 (or PEAK for 3-class) if price drops this much before TP is hit\n"
                              "wider = fewer but higher-confidence labels");
    }
    if (state->label_type == LABEL_FORWARD_PNL) {
        ImGui::InputInt("Forward Ticks", &state->label_forward_ticks, 100, 1000);
        ImGui::SetItemTooltip("How many ticks to look ahead\n"
                              "label = 1 if price is higher, 0 if lower");
    }
    // Lookahead horizon for multiclass and peak/valley labels
    if (state->label_type == LABEL_PEAK_VALLEY_STABLE ||
        state->label_type == LABEL_WILL_PEAK || state->label_type == LABEL_WILL_VALLEY) {
        ImGui::InputInt("Lookahead Ticks", &state->label_forward_ticks, 100, 1000);
        ImGui::SetItemTooltip("How many ticks forward to scan for the barrier hit\n"
                              "0 = scan to end of data. Default 500.\n"
                              "Larger = more confident labels, fewer stable cases\n"
                              "Smaller = more stable cases, sharper peak/valley signal");
    }

    ImGui::Separator();

    // collect features button — disabled if no data selected OR a backtest is
    // already running (mirrors the Walk-Forward pattern). prevents the
    // "click button N times because nothing visibly happens" UX trap that
    // fires N parallel backtests each writing to the same log file.
    bool has_data = data->selected_count > 0;
    bool can_collect = has_data && !run_control->running;
    if (!can_collect) ImGui::BeginDisabled();
    if (ImGui::Button("Collect Features")) {
        // clear previous training/walk-forward results on re-collect
        state->model_trained = false;
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
    ImGui::InputText("Model Path", state->model_path, sizeof(state->model_path));
    ImGui::SetItemTooltip("Where to save the trained model\n"
                          "used by the engine at runtime for ML buy signals");

    // v5.9.0d — Train Model now runs in a worker thread. The audit at
    // train_model_worker_fn (above) walked the race surface + cancellation
    // path. UI shows a progress indicator + Cancel button while running;
    // results display (below) reads the same state fields the worker writes.
    bool can_train = results->sample_count >= 10;
#ifndef USE_XGBOOST
    can_train = false;
#endif

    if (state->tm_running) {
        // Worker is mid-train. Show indeterminate progress + cancel button.
        // XGBoost has no native iteration-progress callback; we use a
        // pulsing bar as a "still alive" signal. Cancel polls between
        // XGBoosterUpdateOneIter calls (bounded latency = one iter time).
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(),
                           ImVec2(-1, 0), "Training XGBoost...");
        if (ImGui::Button("Cancel Training")) {
            state->tm_cancel = 1;
        }
    } else {
        if (!can_train) ImGui::BeginDisabled();
        if (ImGui::Button("Train Model")) {
            // clear previous results + spawn worker
            state->model_trained = false;
            state->status_msg[0] = '\0';
            state->wf_has_results = false;
            state->tm_cancel = 0;
            state->tm_complete = 0;
            state->tm_running = 1;
            TrainModelWorkerArgs *args = (TrainModelWorkerArgs *)malloc(sizeof(TrainModelWorkerArgs));
            args->state = state;
            args->run_control = run_control;
            pthread_create(&state->tm_tid, NULL, train_model_worker_fn, args);
            pthread_detach(state->tm_tid);
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
    }

    // training results — kind-appropriate display.
    if (state->model_trained) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.55f, 0.76f, 0.51f, 1.0f), "%s", state->status_msg);
        // v5.9.5d — surface scaler SHA-256 to operator (was stderr-only).
        // Wraps to next line; selectable so operator can copy. Hidden when
        // empty (no scaler persisted; degenerate-features case).
        if (state->scaler_sha256_hex[0]) {
            ImGui::TextColored(FoxmlColors::comment, "scaler_sha256:");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.80f, 0.60f, 1.0f));
            // InputText with ReadOnly = selectable + copyable single-line.
            ImGui::PushItemWidth(-1);
            ImGui::InputText("##scaler_sha", state->scaler_sha256_hex,
                              sizeof(state->scaler_sha256_hex),
                              ImGuiInputTextFlags_ReadOnly);
            ImGui::PopItemWidth();
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("SHA-256 of the persisted .scaler sidecar file.\n"
                                  "Run Full Validation auto-binds via in-process emit\n"
                                  "(v5.9.5b). For manual stamping via tools/stamp_model.sh,\n"
                                  "pass --scaler-sha256=<this value> + --feature-scaler-present=1.");
        }
        if (LabelType_IsRegression(state->label_type)) {
            ImGui::Text("Train MSE: %.6f  |  Pearson r: %.4f  (in-sample)",
                         state->train_mse, state->train_correlation);
            ImGui::SetItemTooltip("Pearson r is the load-bearing metric for regression.\n"
                                  "  |r| < 0.05  → no signal (model didn't learn)\n"
                                  "  |r| 0.05-0.2 → weak but real signal\n"
                                  "  |r| > 0.2   → strong signal for tick-scale prediction\n\n"
                                  "MSE alone can be misleading: a model predicting always-zero\n"
                                  "gets low MSE on small-magnitude targets while having zero\n"
                                  "predictive power. Always read r alongside MSE.");
        } else {
            ImGui::Text("Train Accuracy: %.1f%% (in-sample)", state->train_accuracy);
        }

        // save run: bundle config + model into models/{run_name}/
        ImGui::Separator();
        ImGui::InputText("Run Name", state->run_name, sizeof(state->run_name));
        if (ImGui::Button("Save Run")) {
            // pick role-specific filename so CoreModelZoo auto-discovers it.
            // role is derived from label_type: 3-class softmax → "barrier",
            // regime classifier → "regime", everything else → "buy_signal".
            const char *role_name = "buy_signal";
            int expected_num_classes = 0;  // 0 = binary
            if (state->label_type == LABEL_PEAK_VALLEY_STABLE) {
                role_name = "barrier";
                expected_num_classes = 3;
            } else if (state->label_type == LABEL_REGIME) {
                role_name = "regime";
                expected_num_classes = 4;
            } else if (state->label_type == LABEL_FORWARD_PNL) {
                role_name = "buy_signal";  // regression treated as binary slot
                expected_num_classes = 1;  // 1 = regression
            }

            // v4.3 — kind-organized layout: models/{kind}/{run_name}/.
            // Classification models go under classification/; regression
            // under regression/. Engine cfg path is the full subdir-prefix
            // path: core_N_model_dir=models/classification/your_run/.
            const char *kind_dir = (expected_num_classes == 1) ? "regression"
                                                                : "classification";
            char run_dir[400];
            snprintf(run_dir, sizeof(run_dir), "models/%s/%s",
                     kind_dir, state->run_name);
            mkdir("models", 0755);
            char kind_parent[300];
            snprintf(kind_parent, sizeof(kind_parent), "models/%s", kind_dir);
            mkdir(kind_parent, 0755);
            mkdir(run_dir, 0755);

            // detect source extension (.json or .xgb)
            const char *src_ext = strrchr(state->model_path, '.');
            if (!src_ext) src_ext = ".xgb";

            // copy model with role-specific name
            char dst_model[384];
            snprintf(dst_model, sizeof(dst_model), "%s/%s%s", run_dir, role_name, src_ext);
            FILE *msrc = fopen(state->model_path, "rb");
            FILE *mdst = fopen(dst_model, "wb");
            if (msrc && mdst) {
                char buf[4096]; size_t n;
                while ((n = fread(buf, 1, sizeof(buf), msrc)) > 0) fwrite(buf, 1, n, mdst);
            }
            if (msrc) fclose(msrc);
            if (mdst) fclose(mdst);

            // v5.8.9 — copy the .stamp file alongside the model if Run Full
            // Validation produced one. Without this, the saved bundle would
            // be missing the stamp and the deployed engine would fall back
            // to "no stamp" (warn-load or refuse depending on
            // held_out_gate_strict). Preserves the auto-stamp work end-to-end.
            char src_stamp[480];
            snprintf(src_stamp, sizeof(src_stamp), "%s.stamp", state->model_path);
            char dst_stamp[480];
            snprintf(dst_stamp, sizeof(dst_stamp), "%s.stamp", dst_model);
            FILE *ssrc = fopen(src_stamp, "rb");
            if (ssrc) {
                FILE *sdst = fopen(dst_stamp, "wb");
                if (sdst) {
                    char buf[4096]; size_t n;
                    while ((n = fread(buf, 1, sizeof(buf), ssrc)) > 0) fwrite(buf, 1, n, sdst);
                    fclose(sdst);
                }
                fclose(ssrc);
            }

            // copy full backtest.cfg as historical record (for reproducibility)
            char dst_cfg[384];
            snprintf(dst_cfg, sizeof(dst_cfg), "%s/engine.cfg", run_dir);
            FILE *csrc = fopen("backtest.cfg", "r");
            FILE *cdst = fopen(dst_cfg, "w");
            if (csrc && cdst) {
                char buf[4096]; size_t n;
                while ((n = fread(buf, 1, sizeof(buf), csrc)) > 0) fwrite(buf, 1, n, cdst);
            }
            if (csrc) fclose(csrc);
            if (cdst) fclose(cdst);

            // write expected.cfg — ML-relevant fields only. when the engine
            // loads this run via core_N_model_dir, it reads expected.cfg and
            // compares against the live engine.cfg. mismatches → loud warnings
            // (or strict failure if model_verify_strict=1). this is the
            // stupid-proof check: prevents accidentally deploying a 3-class
            // model with barrier_gate_enabled=0, etc.
            char dst_expected[384];
            snprintf(dst_expected, sizeof(dst_expected), "%s/expected.cfg", run_dir);
            FILE *ef = fopen(dst_expected, "w");
            if (ef) {
                fprintf(ef, "# auto-generated by foxml_suite Save Run — DO NOT EDIT\n");
                fprintf(ef, "# the engine compares these against engine.cfg at load time.\n");
                fprintf(ef, "# mismatch → warning (default) or failure (model_verify_strict=1).\n");
                fprintf(ef, "\n");
                fprintf(ef, "# role this model fills in CoreModelZoo (barrier|regime|exit|buy_signal)\n");
                fprintf(ef, "expected_role = %s\n", role_name);
                fprintf(ef, "expected_label_type = %d\n", state->label_type);
                fprintf(ef, "expected_num_classes = %d\n", expected_num_classes);
                fprintf(ef, "\n");
                fprintf(ef, "# ML config the model was trained against. live engine should match.\n");
                if (expected_num_classes >= 2) {
                    fprintf(ef, "barrier_gate_enabled = 1   # 3-class model REQUIRES this to be useful\n");
                } else {
                    fprintf(ef, "# barrier_gate_enabled = 0 or 1 both fine for binary models\n");
                }
                fprintf(ef, "ml_buy_threshold = %.3f\n", FPN_ToDouble(results->config_used.ml_buy_threshold));
                fprintf(ef, "ml_tp_pct = %.6f\n", FPN_ToDouble(results->config_used.ml_tp_pct));
                fprintf(ef, "ml_sl_pct = %.6f\n", FPN_ToDouble(results->config_used.ml_sl_pct));
                fprintf(ef, "ml_backend = %d\n", results->config_used.ml_backend);
                // v4.3.1 — record the slow-path cadence the model was trained
                // against. Sharded engine reads cfg.poll_interval at boot;
                // model expects to see RollingStats computed at the same
                // cadence. Mismatch → silent train-serve drift (12.5× time-
                // window difference at the old hardcoded value of 8 vs
                // backtest default 100). Engine compares + warns on load.
                fprintf(ef, "expected_poll_interval = %u\n",
                        results->config_used.poll_interval);
                // v4.3 — feature pack version the model was trained on.
                // Engine refuses to load a v1 model into a v2 feature
                // pipeline (and vice versa) — feature indices change.
                fprintf(ef, "expected_feature_format_version = %u\n",
                        (unsigned)MODEL_FORMAT_VERSION);
                fprintf(ef, "expected_num_features = %u\n",
                        (unsigned)MODEL_NUM_FEATURES);
                // Phase 7 prep — held-out validation cfg saved for reproducibility.
                // Live engine compares these; mismatch = warning (or fail under
                // model_verify_strict=1). Documents the validation discipline
                // the model was evaluated under.
                fprintf(ef, "held_out_fraction = %.4f\n",
                        FPN_ToDouble(results->config_used.held_out_fraction));
                fprintf(ef, "gap_acceptable_threshold = %.4f\n",
                        FPN_ToDouble(results->config_used.gap_acceptable_threshold));
                fprintf(ef, "\n");
                fprintf(ef, "# training hyperparameters (informational, not verified at runtime)\n");
                fprintf(ef, "# max_depth = %d\n", state->max_depth);
                fprintf(ef, "# learning_rate = %.3f\n", state->learning_rate);
                fprintf(ef, "# n_estimators = %d\n", state->n_estimators);
                fprintf(ef, "# train_accuracy = %.1f%% (in-sample)\n", state->train_accuracy);
                fclose(ef);
            }

            // write results summary
            char dst_summary[384];
            snprintf(dst_summary, sizeof(dst_summary), "%s/summary.txt", run_dir);
            FILE *sf = fopen(dst_summary, "w");
            if (sf) {
                fprintf(sf, "run: %s\n", state->run_name);
                fprintf(sf, "role: %s\n", role_name);
                fprintf(sf, "accuracy: %.1f%%\n", state->train_accuracy);
                fprintf(sf, "model: %s\n", dst_model);
                fprintf(sf, "config: %s\n", dst_cfg);
                fprintf(sf, "expected: %s\n", dst_expected);
                fprintf(sf, "label_type: %d\n", state->label_type);
                fprintf(sf, "expected_num_classes: %d\n", expected_num_classes);
                fprintf(sf, "max_depth: %d\n", state->max_depth);
                fprintf(sf, "learning_rate: %.3f\n", state->learning_rate);
                fprintf(sf, "n_estimators: %d\n", state->n_estimators);
                // v4.3 — label barriers from the Training panel. These are
                // the values shown in the Past Runs table and they describe
                // what the model was trained to predict (different from
                // engine ml_tp_pct/ml_sl_pct which are deployment thresholds).
                fprintf(sf, "label_tp_pct: %.4f\n", state->label_tp_pct);
                fprintf(sf, "label_sl_pct: %.4f\n", state->label_sl_pct);
                fprintf(sf, "label_lookahead_ticks: %d\n", state->label_forward_ticks);
                // v4.3 — also persist Walk-Forward metrics if a WF run has
                // been completed for this training. Past Runs viewer reads
                // these to show val accuracy + overfit gap. valid_folds == 0
                // means no WF was run; skip the block (older format).
                if (state->wf_results.valid_folds > 0) {
                    // label-kind-aware metric writeout. For binary/multiclass
                    // (label_kind != 1) WF populates mean_val_accuracy; for
                    // regression (label_kind == 1) WF populates correlation +
                    // mse instead. Save whichever is meaningful.
                    fprintf(sf, "label_kind: %d\n", state->wf_results.label_kind);
                    if (state->wf_results.label_kind == 1) {
                        fprintf(sf, "val_correlation: %.4f\n",
                                state->wf_results.mean_val_correlation);
                        fprintf(sf, "val_mse: %.6f\n",
                                state->wf_results.mean_val_mse);
                        fprintf(sf, "train_val_gap: %.4f\n",
                                state->wf_results.mean_train_correlation -
                                state->wf_results.mean_val_correlation);
                    } else {
                        fprintf(sf, "val_accuracy: %.2f\n",
                                state->wf_results.mean_val_accuracy * 100.0f);
                        fprintf(sf, "val_stddev: %.2f\n",
                                state->wf_results.std_val_accuracy * 100.0f);
                        fprintf(sf, "train_val_gap: %.4f\n",
                                state->wf_results.mean_train_accuracy -
                                state->wf_results.mean_val_accuracy);
                    }
                    fprintf(sf, "overfit_folds: %d\n",
                            state->wf_results.overfit_count);
                    fprintf(sf, "valid_folds: %d\n",
                            state->wf_results.valid_folds);
                }
                // v5.8.9 — held-out + auto-stamp results from Run Full
                // Validation. Captured only when fv_has_results is set
                // (i.e. operator pressed the FV button after Train+WF).
                // Older saves (or saves without FV) skip this block.
                if (state->fv_has_results) {
                    const FullValidationResults *fv = &state->fv_results;
                    if (fv->ran_held_out) {
                        fprintf(sf, "held_out_metric: %.4f\n",
                                (double)fv->held_out_metric);
                        fprintf(sf, "held_out_count: %d\n",
                                (int)fv->held_out_count);
                        fprintf(sf, "held_out_gap_threshold: %.4f\n",
                                (double)fv->gap_threshold);
                    }
                    fprintf(sf, "auto_stamp_attempted: %d\n", fv->auto_stamp_attempted);
                    fprintf(sf, "auto_stamp_ok: %d\n", fv->auto_stamp_ok);
                    if (fv->auto_stamp_path_written[0]) {
                        fprintf(sf, "auto_stamp_path_written: %s\n",
                                fv->auto_stamp_path_written);
                    }
                    if (fv->auto_stamp_attempted && !fv->auto_stamp_ok &&
                        fv->auto_stamp_error[0]) {
                        fprintf(sf, "auto_stamp_error: %s\n", fv->auto_stamp_error);
                    }
                }
                fclose(sf);
            }

            snprintf(state->save_msg, sizeof(state->save_msg),
                     "Saved to %s/ (role=%s, model + expected.cfg)", run_dir, role_name);
        }
        if (state->save_msg[0])
            ImGui::TextColored(FoxmlColors::green, "%s", state->save_msg);
        ImGui::SetItemTooltip("Bundles model + expected.cfg + summary into models/{name}/.\n"
                              "Filename is role-derived (barrier/regime/buy_signal) so the\n"
                              "engine's CoreModelZoo auto-discovers it.\n"
                              "Deploy: set core_N_model_dir = models/{name}/ in engine.cfg.");
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
    ImGui::SetItemTooltip("Label forward window in ticks\n"
                          "controls the purge gap between train and test sets\n"
                          "prevents data leakage from labels that look into the future\n"
                          "should match the label horizon used during feature collection");
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
        bool can_wf = results->sample_count >= 50;
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
    // so the live engine's CoreModelZoo_TryLoadRole can refuse to load a
    // model trained against a different feature set or engine version.
    //
    // This button is the ONLY UI path that exercises Backtest_RunFullValidation
    // (and therefore the v5.8.6 auto-stamp wiring). Pre-v5.8.7 the function
    // existed but was unreachable from the suite.
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
        bool can_fv =
#ifdef USE_XGBOOST
            fv_data->sample_count >= 50 && state->model_path[0] != '\0';
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
        if (state->fv_status_msg[0]) {
            ImVec4 stamp_col = fv->auto_stamp_attempted && fv->auto_stamp_ok
                ? ImVec4(0.55f, 0.76f, 0.51f, 1.0f)
                : ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
            ImGui::TextColored(stamp_col, "%s", state->fv_status_msg);
        }
    }

    ImGui::End();
}

#endif // BACKTEST_PANELS_HPP
