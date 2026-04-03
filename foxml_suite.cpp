// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FOXML SUITE]
//======================================================================================================
// standalone backtesting + ML training workstation.
// separate binary from engine_gui — shares all engine headers, same FPN hot path.
// does NOT connect to Binance — replays historical CSV data through identical engine code.
//
// build: cmake -B build_suite -DUSE_IMGUI_GUI=ON && cmake --build build_suite --target foxml_suite
//======================================================================================================

#include <SDL.h>
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

#include "Backtest/BacktestPanels.hpp"

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
    fprintf(stderr, "foxml suite — backtesting + ML training workstation\n");
    fprintf(stderr, "Copyright (c) 2026 Jennifer Lewis. All rights reserved.\n\n");

    //==================================================================================================
    // SDL2 + ImGui init (same pattern as GUI/GuiThread.hpp)
    //==================================================================================================
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "[suite] SDL_Init error: %s\n", SDL_GetError());
        return 1;
    }

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

    // comparison state (overlay multiple runs)
    static ComparisonState comparison;
    Comparison_Init(&comparison);

    // optimizer state
    static OptimizerPanelState optimizer;
    OptimizerPanel_Init(&optimizer);

    // training state
    static TrainingPanelState training;
    TrainingPanel_Init(&training);

    // trade CSV reader for chart markers
    TradeData trades;
    TradeData_Init(&trades, BACKTEST_TRADE_CSV);

    // chart settings
    ChartSettings chart_settings;

    // settings panel — suite uses its own config so experiments don't affect live trading
    static SettingsState settings = {};
    {
        const char *suite_cfg = "backtest.cfg";
        FILE *check = fopen(suite_cfg, "r");
        if (!check) {
            // first run: copy from engine.cfg as starting point
            FILE *src = fopen("engine.cfg", "r");
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
        GUI_Panel_RunControl(&run_control, &data_panel);
        GUI_Panel_Results(&run_control.results);
        GUI_Panel_Comparison(&comparison, &run_control.results);
        GUI_Panel_Optimizer(&optimizer, &data_panel);
        GUI_Panel_Training(&training, &run_control, &data_panel);

        // dashboard panels — show backtest engine state (reuse from live GUI)
        if (run_control.complete) {
            uint64_t suite_start = (uint64_t)time(NULL); // just for uptime display
            GUI_RenderDashboard(&suite_snap, suite_start);
        }

        // settings (config editing — reuse existing panel)
        // suite doesn't hot-reload mid-backtest, but needs a valid pointer (NULL = crash)
        static volatile sig_atomic_t suite_reload_flag = 0;
        GUI_Panel_Settings(&settings, &suite_reload_flag);
        suite_reload_flag = 0; // consume — config takes effect on next run

        // trade history (reuse existing panel — reads backtest CSV)
        if (run_control.complete) {
            TradeHistory_Refresh(&trade_history);
        }
        GUI_Panel_TradeHistory(&trade_history);

        // charts (reuse existing panels — fed from backtest candle accumulator)
        CandleSnapshot csnap = {};
        CandleAccumulator_Snapshot(&candle_acc, &csnap);
        if (run_control.complete)
            TradeData_Refresh(&trades);
        trades.max_visible_markers = chart_settings.visible_candles * 2;

        ChartState cs = {};
        ChartState_Prepare(&cs, &csnap, &chart_settings);
        // price chart without live drag (pass NULL for shared state pointer)
        GUI_PriceChart(&cs, &suite_snap, &trades, &chart_settings, &candle_acc, NULL);
        GUI_VolumeChart(&cs, &suite_snap, &chart_settings);
        GUI_EquityChart(&trades);
        GUI_LivePnLChart(&suite_snap);

        // update window title with backtest status
        if (run_control.running) {
            char title[128];
            snprintf(title, sizeof(title), "foxml suite  |  running... %d%%", run_control.progress_pct);
            SDL_SetWindowTitle(window, title);
        } else if (run_control.complete) {
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
