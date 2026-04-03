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
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>

//======================================================================================================
// [DATA PANEL STATE]
//======================================================================================================
#define DATA_MAX_FILES 64

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
    char config_path[256];
};

static inline void RunControl_Init(RunControlState *state) {
    memset(state, 0, sizeof(*state));
    strncpy(state->config_path, "engine.cfg", sizeof(state->config_path) - 1);
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

    state->complete = 1;
    state->running = 0;
    return NULL;
}

static inline void RunControl_Start(RunControlState *state, DataPanelState *data) {
    if (state->running) return;

    // build run config from data panel selection
    state->run_config.num_data_files = 0;
    for (int i = 0; i < data->file_count && state->run_config.num_data_files < 16; i++) {
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

    // select all / none
    if (ImGui::Button("Select All")) {
        for (int i = 0; i < state->file_count; i++) state->selected[i] = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Select None")) {
        for (int i = 0; i < state->file_count; i++) state->selected[i] = false;
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

        ImGui::EndTable();
    }

    ImGui::End();
}

#endif // BACKTEST_PANELS_HPP
