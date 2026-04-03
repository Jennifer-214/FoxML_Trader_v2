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
#include "Fingerprint.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>

//======================================================================================================
// [DATA PANEL STATE]
//======================================================================================================
#define DATA_MAX_FILES 256

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
    strncpy(state->config_path, "backtest.cfg", sizeof(state->config_path) - 1);
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

//======================================================================================================
// [COMPARISON STATE]
//======================================================================================================
#define COMPARISON_MAX_RUNS 8

struct ComparisonState {
    BacktestStats stats[COMPARISON_MAX_RUNS];
    double equity_curves[COMPARISON_MAX_RUNS][BACKTEST_MAX_EQUITY];
    int equity_counts[COMPARISON_MAX_RUNS];
    char labels[COMPARISON_MAX_RUNS][64];
    int run_count;
};

static inline void Comparison_Init(ComparisonState *state) {
    memset(state, 0, sizeof(*state));
}

static inline void Comparison_SaveRun(ComparisonState *state, const BacktestResults *results,
                                       const char *label) {
    if (state->run_count >= COMPARISON_MAX_RUNS) {
        // shift everything down, drop oldest
        memmove(&state->stats[0], &state->stats[1],
                (COMPARISON_MAX_RUNS - 1) * sizeof(BacktestStats));
        memmove(&state->equity_curves[0], &state->equity_curves[1],
                (COMPARISON_MAX_RUNS - 1) * sizeof(state->equity_curves[0]));
        memmove(&state->equity_counts[0], &state->equity_counts[1],
                (COMPARISON_MAX_RUNS - 1) * sizeof(int));
        memmove(&state->labels[0], &state->labels[1],
                (COMPARISON_MAX_RUNS - 1) * sizeof(state->labels[0]));
        state->run_count = COMPARISON_MAX_RUNS - 1;
    }
    int idx = state->run_count;
    state->stats[idx] = results->stats;
    int ec = results->equity_count;
    if (ec > BACKTEST_MAX_EQUITY) ec = BACKTEST_MAX_EQUITY;
    memcpy(state->equity_curves[idx], results->equity_curve, ec * sizeof(double));
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
        for (int r = 0; r < state->run_count; r++) {
            int n = state->equity_counts[r];
            if (n < 2) continue;
            // build x-axis (trade index)
            double xs[BACKTEST_MAX_EQUITY];
            for (int i = 0; i < n; i++) xs[i] = (double)i;

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
            for (int i = 0; i < data->file_count && state->run_config.num_data_files < 16; i++) {
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
    float train_accuracy;
    int positive_count, negative_count;
    char status_msg[128];
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
                             &state->wf_progress, &state->wf_cancel);

    state->wf_has_results = true;
    state->wf_complete = 1;
    state->wf_running = 0;
    return NULL;
}

//======================================================================================================
// [PANEL: TRAINING]
//======================================================================================================
static inline void GUI_Panel_Training(TrainingPanelState *state,
                                       RunControlState *run_control,
                                       DataPanelState *data) {
    ImGui::Begin("Training");

    // label config
    static const char *label_names[] = {"Win/Loss", "Barrier", "Forward P&L", "Regime"};
    ImGui::Combo("Label Type", &state->label_type, label_names, LABEL_COUNT);

    if (state->label_type == LABEL_WIN_LOSS || state->label_type == LABEL_BARRIER) {
        ImGui::InputFloat("TP Barrier %", &state->label_tp_pct, 0.1f, 0.5f, "%.1f");
        ImGui::InputFloat("SL Barrier %", &state->label_sl_pct, 0.1f, 0.5f, "%.1f");
    }
    if (state->label_type == LABEL_FORWARD_PNL) {
        ImGui::InputInt("Forward Ticks", &state->label_forward_ticks, 100, 1000);
    }

    ImGui::Separator();

    // collect features button
    bool has_data = data->selected_count > 0;
    if (!has_data) ImGui::BeginDisabled();
    if (ImGui::Button("Collect Features")) {
        // set up run config with feature collection enabled
        run_control->run_config.num_data_files = 0;
        for (int i = 0; i < data->file_count && run_control->run_config.num_data_files < 16; i++) {
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
        run_control->running = 1;

        if (run_control->candle_acc)
            CandleAccumulator_Init(run_control->candle_acc, 60);

        BacktestWorkerArgs *args = (BacktestWorkerArgs *)malloc(sizeof(BacktestWorkerArgs));
        args->state = run_control;
        pthread_create(&run_control->worker_tid, NULL, backtest_worker_fn, args);
        pthread_detach(run_control->worker_tid);
    }
    if (!has_data) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Select data files first");
    }

    // show feature collection status
    BacktestResults *results = &run_control->results;
    if (results->sample_count > 0) {
        // count label distribution
        state->positive_count = 0;
        state->negative_count = 0;
        int neutral_count = 0;
        for (int i = 0; i < results->sample_count; i++) {
            if (results->labels[i] > 0.5f) state->positive_count++;
            else if (results->labels[i] < 0.5f) state->negative_count++;
            else neutral_count++;
        }

        int labeled = state->positive_count + state->negative_count;
        ImGui::Text("Samples: %d  |  +: %d  |  -: %d  |  neutral: %d  |  Ratio: %.1f%%",
                     results->sample_count, state->positive_count, state->negative_count,
                     neutral_count,
                     labeled > 0
                         ? (float)state->positive_count / labeled * 100.0f : 0.0f);
    }

    ImGui::Separator();

    // XGBoost hyperparameters
    ImGui::Text("XGBoost Parameters");
    ImGui::InputInt("Max Depth", &state->max_depth, 1, 2);
    ImGui::InputFloat("Learning Rate", &state->learning_rate, 0.01f, 0.1f, "%.3f");
    ImGui::InputInt("Estimators", &state->n_estimators, 10, 50);
    ImGui::InputText("Model Path", state->model_path, sizeof(state->model_path));

    // train button
    bool can_train = results->sample_count >= 10;
#ifndef USE_XGBOOST
    can_train = false;
#endif
    if (!can_train) ImGui::BeginDisabled();
    if (ImGui::Button("Train Model")) {
#ifdef USE_XGBOOST
        // create output directory
        mkdir("models", 0755);

        // filter out neutral labels (0.5) — XGBoost binary needs 0 or 1
        // compact features + labels into contiguous arrays without neutrals
        int n_valid = 0;
        for (int i = 0; i < results->sample_count; i++) {
            if (results->labels[i] == 0.5f) continue;
            if (n_valid != i) {
                memcpy(&results->feature_matrix[n_valid * MODEL_NUM_FEATURES],
                       &results->feature_matrix[i * MODEL_NUM_FEATURES],
                       MODEL_NUM_FEATURES * sizeof(float));
                results->labels[n_valid] = results->labels[i];
            }
            n_valid++;
        }

        DMatrixHandle dtrain;
        XGDMatrixCreateFromMat(results->feature_matrix, n_valid,
                               MODEL_NUM_FEATURES, NAN, &dtrain);
        XGDMatrixSetFloatInfo(dtrain, "label", results->labels, n_valid);

        BoosterHandle booster;
        XGBoosterCreate(&dtrain, 1, &booster);

        char depth_s[8]; snprintf(depth_s, 8, "%d", state->max_depth);
        char lr_s[16]; snprintf(lr_s, 16, "%f", state->learning_rate);
        XGBoosterSetParam(booster, "max_depth", depth_s);
        XGBoosterSetParam(booster, "eta", lr_s);
        XGBoosterSetParam(booster, "objective", "binary:logistic");
        XGBoosterSetParam(booster, "nthread", "4");
        XGBoosterSetParam(booster, "verbosity", "0");

        for (int i = 0; i < state->n_estimators; i++)
            XGBoosterUpdateOneIter(booster, i, dtrain);

        // embed model format version for compatibility check
        char ver_s[8]; snprintf(ver_s, 8, "%d", MODEL_FORMAT_VERSION);
        XGBoosterSetAttr(booster, "foxml_version", ver_s);

        // embed config+data fingerprint for train-serve parity verification
        {
            const char *fp_paths[16];
            for (int i = 0; i < run_control->run_config.num_data_files && i < 16; i++)
                fp_paths[i] = run_control->run_config.data_paths[i];
            char fp_hex[65];
            Fingerprint_Compute<BACKTEST_FP>(fp_hex, &results->config_used,
                sizeof(results->config_used), fp_paths, run_control->run_config.num_data_files);
            XGBoosterSetAttr(booster, "foxml_fingerprint", fp_hex);
            fprintf(stderr, "[TRAIN] model fingerprint: %.12s...\n", fp_hex);
        }

        // save model
        XGBoosterSaveModel(booster, state->model_path);

        // compute training accuracy (in-sample — just for sanity check)
        bst_ulong out_len;
        const float *out_result;
        DMatrixHandle dpred;
        XGDMatrixCreateFromMat(results->feature_matrix, n_valid,
                               MODEL_NUM_FEATURES, NAN, &dpred);
        XGBoosterPredict(booster, dpred, 0, 0, 0, &out_len, &out_result);
        int correct = 0;
        for (int i = 0; i < n_valid; i++) {
            int pred = out_result[i] >= 0.5f ? 1 : 0;
            int truth = results->labels[i] > 0.5f ? 1 : 0;
            if (pred == truth) correct++;
        }
        state->train_accuracy = (n_valid > 0) ? (float)correct / n_valid * 100.0f : 0.0f;
        XGDMatrixFree(dpred);

        // feature importance (gain-based)
        memset(state->feature_importance, 0, sizeof(state->feature_importance));
        // XGBoost doesn't have a simple GetScore in C API for all features
        // use prediction contribution as a proxy: not available in all versions
        // for now, zero-importance displayed (Phase 6: implement via dump/parse)

        XGDMatrixFree(dtrain);
        XGBoosterFree(booster);

        state->model_trained = true;
        snprintf(state->status_msg, sizeof(state->status_msg),
                 "Model saved to %s (accuracy: %.1f%%)", state->model_path, state->train_accuracy);
#endif
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

    // training results
    if (state->model_trained) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.55f, 0.76f, 0.51f, 1.0f), "%s", state->status_msg);
        ImGui::Text("Train Accuracy: %.1f%% (in-sample)", state->train_accuracy);

        // save run: bundle config + model into models/{run_name}/
        ImGui::Separator();
        ImGui::InputText("Run Name", state->run_name, sizeof(state->run_name));
        if (ImGui::Button("Save Run")) {
            char run_dir[320];
            snprintf(run_dir, sizeof(run_dir), "models/%s", state->run_name);
            mkdir("models", 0755);
            mkdir(run_dir, 0755);

            // copy model
            char dst_model[384];
            snprintf(dst_model, sizeof(dst_model), "%s/model.xgb", run_dir);
            FILE *msrc = fopen(state->model_path, "rb");
            FILE *mdst = fopen(dst_model, "wb");
            if (msrc && mdst) {
                char buf[4096]; size_t n;
                while ((n = fread(buf, 1, sizeof(buf), msrc)) > 0) fwrite(buf, 1, n, mdst);
            }
            if (msrc) fclose(msrc);
            if (mdst) fclose(mdst);

            // copy config
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

            // write results summary
            char dst_summary[384];
            snprintf(dst_summary, sizeof(dst_summary), "%s/summary.txt", run_dir);
            FILE *sf = fopen(dst_summary, "w");
            if (sf) {
                fprintf(sf, "run: %s\n", state->run_name);
                fprintf(sf, "accuracy: %.1f%%\n", state->train_accuracy);
                fprintf(sf, "model: %s\n", dst_model);
                fprintf(sf, "config: %s\n", dst_cfg);
                fprintf(sf, "label_type: %d\n", state->label_type);
                fprintf(sf, "max_depth: %d\n", state->max_depth);
                fprintf(sf, "learning_rate: %.3f\n", state->learning_rate);
                fprintf(sf, "n_estimators: %d\n", state->n_estimators);
                fclose(sf);
            }

            snprintf(state->save_msg, sizeof(state->save_msg),
                     "Saved to %s/ (model + config + summary)", run_dir);
        }
        if (state->save_msg[0])
            ImGui::TextColored(FoxmlColors::green, "%s", state->save_msg);
        ImGui::SetItemTooltip("Bundles model + backtest.cfg + summary into models/{name}/\n"
                              "Deploy: copy engine.cfg to live, set ml_model_path to model.xgb");
    }

    //==================================================================
    // WALK-FORWARD VALIDATION (Phase 6A — the REAL performance metric)
    //==================================================================
    ImGui::Separator();
    ImGui::Text("Walk-Forward Validation");

    // parameters
    ImGui::InputInt("Folds", &state->wf_n_splits, 1, 2);
    if (state->wf_n_splits < 2) state->wf_n_splits = 2;
    if (state->wf_n_splits > 20) state->wf_n_splits = 20;
    ImGui::InputInt("Horizon Ticks", &state->wf_horizon_ticks, 100, 500);
    ImGui::SetItemTooltip("Label forward window for purge gap calculation");
    ImGui::InputInt("Purge Buffer", &state->wf_buffer_ticks, 64, 256);
    ImGui::SetItemTooltip("Extra purge gap beyond max(horizon, feature_lookback)\ndefault 512 ticks");
    ImGui::InputInt("Min Train", &state->wf_min_train, 100, 500);
    ImGui::SetItemTooltip("Minimum samples per training fold\nfolds with fewer are skipped");

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

    // walk-forward results display
    if (state->wf_has_results) {
        WalkForwardResults *wf = &state->wf_results;

        // aggregate metrics — the metrics that actually matter
        ImGui::Separator();
        {
            // mean ± std validation accuracy (green if reasonable, red if overfit)
            ImVec4 val_color = (wf->overfit_count > 0)
                ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f)   // red: overfit detected
                : ImVec4(0.55f, 0.76f, 0.51f, 1.0f);   // green: clean
            ImGui::TextColored(val_color, "Val Accuracy: %.1f%% +/- %.1f%%",
                               wf->mean_val_accuracy * 100.0f, wf->std_val_accuracy * 100.0f);
            ImGui::SameLine();
            ImGui::TextDisabled("(train: %.1f%%)", wf->mean_train_accuracy * 100.0f);
        }

        // overfit warning
        if (wf->overfit_count > 0) {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f),
                "WARNING: %d/%d folds flagged as overfit", wf->overfit_count, wf->valid_folds);
        }

        // fingerprint
        if (wf->fingerprint[0] != '\0') {
            char short_fp[13];
            Fingerprint_Short(wf->fingerprint, short_fp, 12);
            ImGui::TextDisabled("Fingerprint: %s  (%.0f ms)", short_fp, wf->elapsed_ms);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Full: %s\nReproducible: same config + data = same hash", wf->fingerprint);
        }

        // per-fold table
        if (wf->valid_folds > 0 && ImGui::TreeNode("Per-Fold Results")) {
            ImGui::BeginTable("folds", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg);
            ImGui::TableSetupColumn("Fold", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Train", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Val", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Gap", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (int i = 0; i < wf->num_folds; i++) {
                if (!wf->folds[i].valid) continue;
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", i + 1);

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

    ImGui::End();
}

#endif // BACKTEST_PANELS_HPP
