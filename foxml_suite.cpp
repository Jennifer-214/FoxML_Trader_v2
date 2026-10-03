// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FOXML SUITE]
//======================================================================================================
// standalone backtesting + ML training workstation.
// separate binary from engine_gui — shares all engine headers, same FPN_Binary hot path.
// does NOT connect to Binance — replays historical CSV data through identical engine code.
//
// build: cmake -B build_suite -DUSE_IMGUI_GUI=ON && cmake --build build_suite --target foxml_suite
//======================================================================================================

#include <SDL.h>
#include <locale.h>   // .E.0.1: LC_NUMERIC=C boot pin (re-pinned after SDL_Init)
#include <SDL_opengl.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include "GUI/FoxmlTheme.hpp"
#include "DataStream/EngineTUI.hpp"
#include "GUI/CandleAccumulator.hpp"
#include "GUI/ChartPanel.hpp"
#include "GUI/TradeReader.hpp"
#include "GUI/TradeHistoryPanel.hpp"
#include "GUI/SettingsPanel.hpp"
#include "GUI/DashboardPanels.hpp"
#include "GUI/LogViewerPanel.hpp"
#include "GUI/EngineHeaderPanel.hpp"  // v5.8.6b: engine version + registry hash header
#include "GUI/MLStatusPanel.hpp"      // v5.9.0b: per-core ML observability

#include "Backtest/BacktestPanels.hpp"
#include "CoreFrameworks/SystemInit.hpp"  // v5.11.0.A — engine_set_mxcsr_ftz_daz
#include "CoreFrameworks/CfgPaths.hpp"    // the cfg-filename SSoT (the suite cfg + the default engine cfg)

#include <sys/stat.h>  // mkdir
#include <stdlib.h>    // getenv (FOXML_ROOT, the repo-root cwd anchor)
#include <unistd.h>    // 2026-08-20 — readlink()/chdir() for the repo-root cwd anchor
#include <string.h>    // strrchr (cwd anchor)
#include <errno.h>     // strerror on chdir failure (cwd anchor)

//======================================================================================================
// [SUITE DOCK LAYOUT]
//======================================================================================================
static void Suite_SetupDefaultLayout(ImGuiID dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);

    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::DockBuilderSetNodeSize(dockspace_id, vp->Size);
    ImGui::DockBuilderSetNodePos(dockspace_id, vp->Pos);

    // left 60% (charts), right 40% (controls + results)
    ImGuiID dock_left, dock_right;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.60f, &dock_left, &dock_right);

    // left: charts stacked
    ImGuiID dock_left_top, dock_left_bottom;
    ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Up, 0.70f, &dock_left_top, &dock_left_bottom);
    ImGui::DockBuilderDockWindow("Price Chart", dock_left_top);
    ImGui::DockBuilderDockWindow("Volume", dock_left_bottom);
    ImGui::DockBuilderDockWindow("Equity Curve", dock_left_bottom);

    // right: controls on top, results on bottom
    ImGuiID dock_right_top, dock_right_bottom;
    ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Up, 0.55f, &dock_right_top, &dock_right_bottom);

    ImGui::DockBuilderDockWindow("Data", dock_right_top);
    ImGui::DockBuilderDockWindow("Run Control", dock_right_top);
    ImGui::DockBuilderDockWindow("Settings", dock_right_top);
    ImGui::DockBuilderDockWindow("Optimizer", dock_right_top);
    ImGui::DockBuilderDockWindow("Training", dock_right_top);

    ImGui::DockBuilderDockWindow("Results", dock_right_bottom);
    ImGui::DockBuilderDockWindow("Comparison", dock_right_bottom);
    ImGui::DockBuilderDockWindow("Trade History", dock_right_bottom);
    ImGui::DockBuilderDockWindow("Market", dock_right_bottom);
    ImGui::DockBuilderDockWindow("Account", dock_right_bottom);
    ImGui::DockBuilderDockWindow("Stats", dock_right_bottom);
    ImGui::DockBuilderDockWindow("ML Intelligence", dock_right_bottom);

    ImGui::DockBuilderFinish(dockspace_id);
}

//======================================================================================================
// [MAIN]
//======================================================================================================
int main(int argc, char *argv[]) {
    // v5.11.0.A — Suite does FP math during model training (XGBoost feature
    // standardizer, label binning, walk-forward scoring). Match engine's
    // MXCSR state so trained models score identically at serve time.
    tt::engine_set_mxcsr_ftz_daz();

    // 2026-08-20 — anchor cwd to the repo root BEFORE any relative file I/O.
    // Every suite path is repo-root-relative (logging/, data/, models/,
    // backtest.cfg, foxml_suite.ini, data/foxml_gui_state.txt). Launched from a
    // desktop entry or any other directory, the suite found no data, disabled
    // Collect Features ("Select data files first"), and wrote its log into the
    // launch cwd — indistinguishable from a broken trainer at the operator seat.
    // Same anchoring the data scripts use (scripts/download_data.sh: cd to
    // `readlink -f $0`/..). Root = parent of the dir holding this binary — all
    // build dirs (build_gui/, build_suite/, build_debug/, ...) sit one level
    // below the repo root, and /proc/self/exe resolves the bin/foxml_suite
    // symlink to the real build dir. FOXML_ROOT env overrides for a relocated
    // install; on readlink AND override both unavailable, behaves as before
    // (launch-cwd-relative).
    {
        const char *root = getenv("FOXML_ROOT");
        char exe_dir[512];
        if (!root) {
            ssize_t n = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
            if (n > 0) {
                exe_dir[n] = '\0';
                char *slash = strrchr(exe_dir, '/');
                if (slash) {
                    *slash = '\0';  // strip binary name → the build dir
                    size_t len = strlen(exe_dir);
                    snprintf(exe_dir + len, sizeof(exe_dir) - len, "/..");
                    root = exe_dir;
                }
            }
        }
        if (root && chdir(root) != 0)
            fprintf(stderr, "[suite] WARN: chdir(%s) failed (%s) — relative paths "
                            "resolve against the launch cwd\n", root, strerror(errno));
    }

    fprintf(stderr, "foxml suite — backtesting + ML training workstation\n");
    fprintf(stderr, "Copyright (c) 2026 Jennifer Lewis. All rights reserved.\n\n");

    // ensure logging dir exists, then redirect stderr to logging/foxml_suite.log
    // so the in-app Log panel can tail it. mirrors main()'s logging/ + stderr-redirect block.
    mkdir("logging", 0755);
    {
        const char *log_path = "logging/foxml_suite.log";
        const char *prev_path = "logging/foxml_suite.log.1";
        rename(log_path, prev_path);  // rotate previous run, silently fails if absent
        FILE *lf = freopen(log_path, "w", stderr);
        if (lf) {
            setvbuf(stderr, NULL, _IOLBF, 0);  // line-buffered so the Log panel tails cleanly
            fprintf(stderr, "[suite] stderr → %s\n", log_path);
        }
    }

    //==================================================================================================
    // SDL2 + ImGui init (same pattern as GUI/GuiThread.hpp)
    //==================================================================================================
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "[suite] SDL_Init error: %s\n", SDL_GetError());
        return 1;
    }

    // .E.0.1 locale-determinism: SDL/X11 init can reset LC_* process-wide → pin
    // LC_NUMERIC=C after SDL_Init so all suite threads parse floats under C.
    setlocale(LC_NUMERIC, "C");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    SDL_Window *window = SDL_CreateWindow("foxml suite",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1600, 900, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        fprintf(stderr, "[suite] SDL_CreateWindow error: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1); // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "foxml_suite.ini"; // separate layout from engine GUI

    // load fonts (same as GuiThread.hpp)
    {
        ImFontConfig hack_cfg;
        hack_cfg.OversampleH = 2;
        hack_cfg.OversampleV = 1;
        static const ImWchar latin_ranges[] = {
            0x0020, 0x00FF, 0x2190, 0x21FF, 0x2500, 0x257F,
            0x25A0, 0x25FF, 0x2600, 0x26FF, 0,
        };
        ImFont *font = io.Fonts->AddFontFromFileTTF(
            "/usr/share/fonts/TTF/HackNerdFontMono-Regular.ttf",
            18.0f, &hack_cfg, latin_ranges);
        if (font) {
            ImFontConfig cjk_cfg;
            cjk_cfg.MergeMode = true;
            cjk_cfg.OversampleH = 1;
            cjk_cfg.OversampleV = 1;
            static const ImWchar jp_ranges[] = {
                0x3000, 0x303F, 0x3040, 0x309F, 0x30A0, 0x30FF, 0xFF00, 0xFFEF, 0,
            };
            io.Fonts->AddFontFromFileTTF(
                "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                18.0f, &cjk_cfg, jp_ranges);
        }
        if (!font) io.FontGlobalScale = 1.3f;
    }

    // v5.11.39 — font scale persistence (operator-flagged 2026-05-07,
    // re-flagged after v5.11.37 because the original implementation
    // only added the read to GuiThread.hpp; foxml_suite.cpp has its
    // own font init block that needs the same logic). Identical
    // shape as GuiThread.hpp:171-186.
    //
    // Save side (SettingsPanel.hpp) writes data/foxml_gui_state.txt
    // on slider drag; read side here applies it at boot. Default
    // 0.6 if file missing or value out of [0.5, 1.5] range.
    {
        FILE* f = fopen("data/foxml_gui_state.txt", "r");
        float saved_scale = 0.6f;  // 0.6 default — operator-set baseline
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                float v;
                if (sscanf(line, "font_scale=%f", &v) == 1) {
                    if (v >= 0.5f && v <= 1.5f) saved_scale = v;
                    break;
                }
            }
            fclose(f);
        }
        io.FontGlobalScale = saved_scale;
    }

    // theme + transparency
    Foxml_ApplyTheme();
    ImGuiStyle &style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg].w         = 0.60f;
    style.Colors[ImGuiCol_ChildBg].w          = 0.55f;
    style.Colors[ImGuiCol_PopupBg].w          = 0.80f;
    style.Colors[ImGuiCol_TitleBg].w          = 0.60f;
    style.Colors[ImGuiCol_TitleBgActive].w    = 0.65f;
    style.Colors[ImGuiCol_DockingEmptyBg].w   = 0.0f;
    style.Colors[ImGuiCol_TableRowBgAlt].w    = 0.10f;
    style.Colors[ImGuiCol_FrameBg].w          = 0.45f;
    style.Colors[ImGuiCol_Tab].w              = 0.55f;
    style.Colors[ImGuiCol_TabDimmed].w        = 0.40f;
    style.Colors[ImGuiCol_ScrollbarBg].w      = 0.20f;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 330");

    //==================================================================================================
    // panel state init
    //==================================================================================================
    static DataPanelState data_panel;
    DataPanel_Init(&data_panel);

    static RunControlState run_control;
    RunControl_Init(&run_control);

    // candle accumulator for chart visualization
    CandleAccumulator candle_acc;
    CandleAccumulator_Init(&candle_acc, 60);
    run_control.candle_acc = &candle_acc;

    // snapshot populated by backtest worker — dashboard panels read this
    static TUISnapshot suite_snap = {};
    run_control.snapshot = &suite_snap;
    // what the panels draw: a copy of the run's snapshot, taken while Run Control's outputs are at rest (E.1.3 MP-6
    // step 10.3 — the worker fills suite_snap at its run's end with no seqlock; RunControl_AdoptSnapshot)
    static TUISnapshot suite_snap_view = {};

    // comparison state (overlay multiple runs)
    static ComparisonState comparison;
    Comparison_Init(&comparison);
    // v4.3 — Past Runs viewer state. Scans models/{name}/ subdirs at startup;
    // the Rescan button repopulates if user saves/deletes runs externally.
    static PastRunsState past_runs;
    PastRuns_Init(&past_runs);
    PastRuns_Scan(&past_runs);

    // optimizer state
    static OptimizerPanelState optimizer;
    OptimizerPanel_Init(&optimizer);

    // training state
    static TrainingPanelState training;
    TrainingPanel_Init(&training);
    // D-507 — the ONE line saying why the last start did not happen, shared by the three panels that start runs and
    // shown in one modal (LaunchFailure_Modal, drawn after the panels that start runs)
    static LaunchFailureState launch_failure;

    // log viewer — tails logging/foxml_suite.log so the user can see backtest
    // progress in real time without flipping to a terminal. backtest worker
    // thread fprintf's go through stderr → the log file → this panel.
    static LogViewer log_viewer;
    LogViewer_Init(&log_viewer, "logging/foxml_suite.log");

    // trade CSV reader for chart markers
    TradeData trades;
    TradeData_Init(&trades, BACKTEST_TRADE_CSV);

    // chart settings
    ChartSettings chart_settings;

    // settings panel — suite uses its own config so experiments don't affect live trading
    static SettingsState settings = {};
    {
        const char *suite_cfg = CFG_PATH_BACKTEST_CFG;
        FILE *check = fopen(suite_cfg, "r");
        if (!check) {
            // first run: copy from engine.cfg as starting point
            FILE *src = fopen(CFG_PATH_DEFAULT_ENGINE_CFG, "r");
            if (src) {
                FILE *dst = fopen(suite_cfg, "w");
                if (dst) {
                    char buf[4096]; size_t n;
                    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
                    fclose(dst);
                    fprintf(stderr, "[suite] created backtest.cfg from engine.cfg\n");
                }
                fclose(src);
            }
        } else {
            fclose(check);
        }
        strncpy(settings.cfg_path, suite_cfg, 255);
    }

    // v5.15.5.F.4d.1.B.3 Step 5.5 (2026-05-24) — backtest.cfg/engine.cfg drift guardrail.
    // Per foxml_suite ML↔LIVE agent CRIT-1: backtest.cfg is first-boot copy of engine.cfg;
    // idempotent-skip on subsequent boots; the two files DRIFT silently after first boot.
    // Phase F structurally encoded the drift into stamp HMAC body — Stamp_AssembleAndEmit
    // reads results->config_used (from backtest.cfg); live engine reads engine.cfg; stamp
    // values reflect backtest.cfg state. Drift-check at load fires correctly but operator
    // may dismiss as "models always drift" without recognizing root cause is 2 cfg files.
    //
    // This boot-time guardrail surfaces drift IMMEDIATELY so operator knows to reconcile
    // (or knowingly accept divergence) before running backtests. Stop-gap until structural
    // fix lands at v5.15.6.A/B/C per TECH_DEBT-123 (lives_in_struct-aware parser dispatch).
    //
    // Discipline: simple byte-level file diff (fast; catches all drift). More-precise
    // STAMP_BOUND_CFG_DERIVED-field-specific diff queued with the structural fix.
    // Compares the DEFAULT engine cfg (CFG_PATH_DEFAULT_ENGINE_CFG) only — an engine launched
    // with another cfg file on its command line is outside this check.
    {
        FILE* fe = fopen(CFG_PATH_DEFAULT_ENGINE_CFG, "rb");
        FILE* fb = fopen(CFG_PATH_BACKTEST_CFG, "rb");
        if (fe && fb) {
            fseek(fe, 0, SEEK_END); long se = ftell(fe);
            fseek(fb, 0, SEEK_END); long sb = ftell(fb);
            int differs = (se != sb);
            if (!differs) {
                fseek(fe, 0, SEEK_SET); fseek(fb, 0, SEEK_SET);
                char be[4096], bb[4096];
                size_t ne, nb;
                while ((ne = fread(be, 1, sizeof(be), fe)) > 0 &&
                       (nb = fread(bb, 1, sizeof(bb), fb)) > 0) {
                    if (ne != nb || memcmp(be, bb, ne) != 0) { differs = 1; break; }
                }
            }
            if (differs) {
                fprintf(stderr, "\n[suite] !!! engine.cfg / backtest.cfg DIVERGENT !!!\n");
                fprintf(stderr, "[suite]     engine.cfg = %ld bytes, backtest.cfg = %ld bytes\n", se, sb);
                fprintf(stderr, "[suite]     Backtest stamp bodies encode backtest.cfg state.\n");
                fprintf(stderr, "[suite]     Live engine reads engine.cfg — train/serve cfg-state\n");
                fprintf(stderr, "[suite]     parity is NOT GUARANTEED. Inspect via:\n");
                fprintf(stderr, "[suite]         diff engine.cfg backtest.cfg\n");
                fprintf(stderr, "[suite]     Structural fix queued at v5.15.6 (TECH_DEBT-123).\n\n");
            }
        }
        if (fe) fclose(fe);
        if (fb) fclose(fb);
    }

    // trade history
    TradeHistory trade_history;
    TradeHistory_Init(&trade_history, BACKTEST_TRADE_CSV);

    bool first_frame = true;
    bool running = true;

    //==================================================================================================
    // render loop
    //==================================================================================================
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // dockspace
        ImGuiID dockspace_id = ImGui::GetID("SuiteDock");
        ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport(),
                                     ImGuiDockNodeFlags_PassthruCentralNode);

        // default layout on first frame
        if (first_frame) {
            if (ImGui::DockBuilderGetNode(dockspace_id) == NULL ||
                ImGui::DockBuilderGetNode(dockspace_id)->ChildNodes[0] == NULL) {
                Suite_SetupDefaultLayout(dockspace_id);
            }
            first_frame = false;
        }

        // keyboard — Q to quit
        if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_Q))
            running = false;

        //==============================================================================================
        // panels
        //==============================================================================================

        // backtest panels (right side)
        GUI_Panel_DataBrowser(&data_panel);
        GUI_Panel_RunControl(&run_control, &data_panel, &launch_failure);
        // E.1.3 MP-6 step 10.3 (F8) — the Run Control results are a run's to write while it runs: the panels that show
        // them get them only through the one predicate (a finished run that ran), never every frame mid-run; the GUI
        // thread is the lease's only acquirer, so no run can start between this check and the panel's reads
        const BacktestResults *rc_results = RunControl_HasRun(&run_control) ? &run_control.results : nullptr;
        GUI_Panel_Results(rc_results);
        GUI_Panel_Comparison(&comparison, rc_results);
        // v5.11.57 — Verify Stamp verifies with cfg.auto_stamp_secret (devmode while there is none): the panel's own
        // copy, refreshed while Run Control's outputs are at rest and held through a run (E.1.3 MP-6 step 10.3)
        PastRuns_AdoptVerifySecret(&past_runs, &run_control);
        GUI_Panel_PastRuns(&past_runs);
        GUI_Panel_Optimizer(&optimizer, &data_panel, &launch_failure);
        GUI_Panel_Training(&training, &run_control, &data_panel, &launch_failure);
        GUI_Panel_LogViewer(&log_viewer);
        // after every panel that starts a run, outside each one's window: the popup opens and begins at the same ID
        // scope, whichever panel's button failed (Backtest/SuiteModal.hpp)
        LaunchFailure_Modal(&launch_failure);

        // every panel below draws the run's snapshot through the GUI's copy (Class 63 — F4 of the step-10.3 review):
        // refreshed while Run Control's outputs are at rest, held through a run
        RunControl_AdoptSnapshot(&suite_snap_view, &run_control);

        // dashboard panels — show backtest engine state (reuse from live GUI)
        if (RunControl_HasRun(&run_control)) {
            uint64_t suite_start = (uint64_t)time(NULL); // just for uptime display
            GUI_RenderDashboard(&suite_snap_view, suite_start);
        }

        // settings (config editing — reuse existing panel)
        // suite doesn't hot-reload mid-backtest, but needs a valid pointer (NULL = crash)
        static volatile sig_atomic_t suite_reload_flag = 0;
        GUI_Panel_Settings(&settings, &suite_reload_flag);
        suite_reload_flag = 0; // consume — config takes effect on next run

        // v5.8.6b: engine header — version + feature registry hash + format
        // v5.9.0c: pass snap so cfg path renders too
        tt::EngineHeader_Render(&suite_snap_view);
        // v5.9.0b: ML status — per-core load state, prediction context, NaN counters
        tt::MLStatus_Render(&suite_snap_view);

        // trade history (reuse existing panel — reads backtest CSV)
        if (RunControl_HasRun(&run_control)) {
            // partial_exit_enabled=0: the suite's own panel call below passes the same
            // (no partials concept in the training/backtest surface), so v3-era rows are
            // left un-normalized here exactly as before.
            TradeHistory_Refresh(&trade_history, /*partial_exit_enabled=*/0);
        }
        GUI_Panel_TradeHistory(&trade_history);

        // charts (reuse existing panels — fed from backtest candle accumulator)
        CandleSnapshot csnap = {};
        CandleAccumulator_Snapshot(&candle_acc, &csnap);
        if (RunControl_HasRun(&run_control))
            TradeData_Refresh(&trades);
        trades.max_visible_markers = chart_settings.visible_candles * 2;

        ChartState cs = {};
        ChartState_Prepare(&cs, &csnap, &chart_settings);
        // price chart without live drag (pass NULL for shared state pointer)
        GUI_PriceChart(&cs, &suite_snap_view, &trades, &chart_settings, &candle_acc, NULL);
        GUI_VolumeChart(&cs, &suite_snap_view, &chart_settings);
        GUI_EquityChart(&trades);
        GUI_LivePnLChart(&suite_snap_view);

        // update window title with backtest status
        if (SuiteJob_Running(&run_control.job)) {
            char title[128];
            snprintf(title, sizeof(title), "foxml suite  |  running... %d%%", run_control.job.progress);
            SDL_SetWindowTitle(window, title);
        } else if (RunControl_HasRun(&run_control)) {
            char title[128];
            snprintf(title, sizeof(title), "foxml suite  |  P&L $%+.2f  |  %u trades",
                     run_control.results.stats.total_pnl, run_control.results.stats.total_trades);
            SDL_SetWindowTitle(window, title);
        }

        //==============================================================================================
        // render
        //==============================================================================================
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    //==================================================================================================
    // shutdown
    //==================================================================================================
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
