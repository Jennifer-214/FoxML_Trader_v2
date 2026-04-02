#pragma once
// GuiThread — SDL2 + Dear ImGui + implot render loop
// replaces the ANSI TUI thread when built with USE_IMGUI_GUI
//
// architecture:
//   - reads TUISnapshot via atomic double-buffer (same as ANSI TUI)
//   - sends commands (quit/pause/reload) via atomic flags on TUISharedState
//   - renders at ~60fps, engine hot path is unaffected
//
// each panel is a standalone ImGui window (dockable, rearrangeable)
// chart and dashboard are independent — either can be hidden/moved

#include <SDL.h>
#include <SDL_opengl.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include "FoxmlTheme.hpp"
#include "DashboardPanels.hpp"
#include "CandleAccumulator.hpp"
#include "ChartPanel.hpp"
#include "SettingsPanel.hpp"
#include "TradeHistoryPanel.hpp"
#include "LogViewerPanel.hpp"

//==========================================================================
// GUI CONTEXT
//==========================================================================
struct GuiContext {
    SDL_Window   *window;
    SDL_GLContext gl_ctx;
    bool          running;
};

//==========================================================================
// GUI INIT
//==========================================================================
static inline bool Gui_Init(GuiContext *gui, const char *title, int w, int h) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "[gui] SDL_Init error: %s\n", SDL_GetError());
        return false;
    }

    // OpenGL 3.3 core
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    // alpha channel for compositor transparency (Hyprland)
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    gui->window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!gui->window) {
        fprintf(stderr, "[gui] SDL_CreateWindow error: %s\n", SDL_GetError());
        return false;
    }

    gui->gl_ctx = SDL_GL_CreateContext(gui->window);
    SDL_GL_MakeCurrent(gui->window, gui->gl_ctx);
    SDL_GL_SetSwapInterval(1);  // vsync

    // imgui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "foxml_gui.ini";  // layout persistence

    // load Hack Nerd Font Mono (matches kitty terminal) + Noto CJK for kaomoji
    {
        ImFontConfig hack_cfg;
        hack_cfg.OversampleH = 2;
        hack_cfg.OversampleV = 1;
        static const ImWchar latin_ranges[] = {
            0x0020, 0x00FF, // Latin
            0x2190, 0x21FF, // Arrows
            0x2500, 0x257F, // Box drawing
            0x25A0, 0x25FF, // Geometric shapes
            0x2600, 0x26FF, // Miscellaneous symbols
            0,
        };
        ImFont *font = io.Fonts->AddFontFromFileTTF(
            "/usr/share/fonts/TTF/HackNerdFontMono-Regular.ttf",
            18.0f, &hack_cfg, latin_ranges);

        // merge Japanese glyphs from Noto Sans CJK (for kaomoji fox art)
        if (font) {
            ImFontConfig cjk_cfg;
            cjk_cfg.MergeMode = true;  // merge into existing font
            cjk_cfg.OversampleH = 1;
            cjk_cfg.OversampleV = 1;
            static const ImWchar jp_ranges[] = {
                0x3000, 0x303F, // CJK punctuation (、)
                0x3040, 0x309F, // Hiragana (じし)
                0x30A0, 0x30FF, // Katakana (ドヘノ)
                0xFF00, 0xFFEF, // Fullwidth
                0,
            };
            io.Fonts->AddFontFromFileTTF(
                "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                18.0f, &cjk_cfg, jp_ranges);
        }

        if (!font) {
            fprintf(stderr, "[gui] Hack Nerd Font not found, using default\n");
            io.FontGlobalScale = 1.3f;
        }
    }

    // theme + transparency
    Foxml_ApplyTheme();

    // panel transparency — match terminal at KITTY_BG_OPACITY=0.6
    ImGuiStyle &style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg].w         = 0.60f;  // match terminal opacity
    style.Colors[ImGuiCol_ChildBg].w          = 0.55f;
    style.Colors[ImGuiCol_PopupBg].w          = 0.80f;  // popups slightly more opaque
    style.Colors[ImGuiCol_TitleBg].w          = 0.60f;
    style.Colors[ImGuiCol_TitleBgActive].w    = 0.65f;
    style.Colors[ImGuiCol_DockingEmptyBg].w   = 0.0f;   // fully transparent empty dockspace
    style.Colors[ImGuiCol_TableRowBgAlt].w    = 0.10f;   // subtle table striping
    style.Colors[ImGuiCol_FrameBg].w          = 0.45f;
    style.Colors[ImGuiCol_Tab].w              = 0.55f;
    style.Colors[ImGuiCol_TabDimmed].w        = 0.40f;
    style.Colors[ImGuiCol_ScrollbarBg].w      = 0.20f;

    // backends
    ImGui_ImplSDL2_InitForOpenGL(gui->window, gui->gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 330");

    gui->running = true;
    return true;
}

//==========================================================================
// GUI SHUTDOWN
//==========================================================================
static inline void Gui_Shutdown(GuiContext *gui) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gui->gl_ctx);
    SDL_DestroyWindow(gui->window);
    SDL_Quit();
}

//==========================================================================
// GUI FRAME
//==========================================================================
static inline bool Gui_BeginFrame(GuiContext *gui) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT)
            gui->running = false;
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_CLOSE &&
            event.window.windowID == SDL_GetWindowID(gui->window))
            gui->running = false;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // full-window dockspace — transparent background so compositor shows through
    ImGuiID dockspace_id = ImGui::GetID("DockSpace");
    ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport(),
                                 ImGuiDockNodeFlags_PassthruCentralNode);

    return gui->running;
}

static inline void Gui_EndFrame(GuiContext *gui) {
    ImGui::Render();
    ImGuiIO &io = ImGui::GetIO();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    // transparent clear — lets Hyprland compositor show wallpaper through empty areas
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(gui->window);
}

//==========================================================================
// DEFAULT DOCK LAYOUT — runs once on first frame (no ini file yet)
// chart 60% left, dashboard panels stacked 40% right
//==========================================================================
static inline void Gui_SetupDefaultLayout(ImGuiID dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);

    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::DockBuilderSetNodeSize(dockspace_id, vp->Size);
    ImGui::DockBuilderSetNodePos(dockspace_id, vp->Pos);

    // split: left 60% (chart), right 40% (dashboard)
    ImGuiID dock_left, dock_right;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.60f, &dock_left, &dock_right);

    // charts go left — price on top, volume + equity below
    ImGuiID dock_left_top, dock_left_bottom;
    ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Up, 0.70f, &dock_left_top, &dock_left_bottom);
    ImGui::DockBuilderDockWindow("Price Chart", dock_left_top);
    ImGui::DockBuilderDockWindow("Volume", dock_left_bottom);
    ImGui::DockBuilderDockWindow("Live P&L", dock_left_bottom);
    ImGui::DockBuilderDockWindow("Equity Curve", dock_left_bottom);

    // stack all dashboard panels into right side
    // split right into top section and bottom section
    ImGuiID dock_right_top, dock_right_bottom;
    ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Up, 0.70f, &dock_right_top, &dock_right_bottom);

    // top-right: main dashboard panels (tabbed/stacked)
    ImGui::DockBuilderDockWindow("Header", dock_right_top);
    ImGui::DockBuilderDockWindow("Top Bar", dock_right_top);
    ImGui::DockBuilderDockWindow("Market", dock_right_top);
    ImGui::DockBuilderDockWindow("Buy Gate", dock_right_top);
    ImGui::DockBuilderDockWindow("Account", dock_right_top);
    ImGui::DockBuilderDockWindow("Positions", dock_right_top);

    // bottom-right: stats + settings + trade history + log (tabbed)
    ImGui::DockBuilderDockWindow("Stats", dock_right_bottom);
    ImGui::DockBuilderDockWindow("Latency", dock_right_bottom);
    ImGui::DockBuilderDockWindow("Settings", dock_right_bottom);
    ImGui::DockBuilderDockWindow("Trade History", dock_right_bottom);
    ImGui::DockBuilderDockWindow("Engine Log", dock_right_bottom);

    // volume + equity tabbed together in bottom-left
    // (user can drag them apart if they want)

    ImGui::DockBuilderFinish(dockspace_id);
}

//==========================================================================
// GUI KEYBOARD — same controls as ANSI TUI
//==========================================================================
static inline void Gui_HandleKeys(TUISharedState *shared) {
    if (ImGui::GetIO().WantCaptureKeyboard) return;

    if (ImGui::IsKeyPressed(ImGuiKey_Q))
        __atomic_store_n(&shared->quit_requested, 1, __ATOMIC_RELEASE);
    if (ImGui::IsKeyPressed(ImGuiKey_P))
        __atomic_store_n(&shared->pause_requested, 1, __ATOMIC_RELEASE);
    if (ImGui::IsKeyPressed(ImGuiKey_R))
        __atomic_store_n(&shared->reload_requested, 1, __ATOMIC_RELEASE);
    if (ImGui::IsKeyPressed(ImGuiKey_S))
        __atomic_store_n(&shared->regime_cycle_requested, 1, __ATOMIC_RELEASE);
    if (ImGui::IsKeyPressed(ImGuiKey_K))
        __atomic_store_n(&shared->kill_reset_requested, 1, __ATOMIC_RELEASE);
}

//==========================================================================
// GUI THREAD — drop-in replacement for tui_thread_fn
//==========================================================================
static inline void *gui_thread_fn(void *arg) {
    TUISharedState *shared = (TUISharedState *)arg;

    GuiContext gui;
    if (!Gui_Init(&gui, "foxml trader", 1600, 900)) {
        fprintf(stderr, "[gui] init failed, falling back to headless\n");
        return NULL;
    }

    uint64_t gui_start = (uint64_t)time(NULL);
    bool first_frame = true;

    // trade CSV reader for chart markers + equity curve
    TradeData trades;
    TradeData_Init(&trades, "logging/btcusdt_order_history.csv");

    // chart display settings
    ChartSettings chart_settings;

    // settings panel state
    SettingsState settings = {};
    strncpy(settings.cfg_path, "engine.cfg", 255);

    // trade history + log viewer
    TradeHistory trade_history;
    TradeHistory_Init(&trade_history, "logging/btcusdt_order_history.csv");
    LogViewer log_viewer;
    LogViewer_Init(&log_viewer, "logging/engine.log");

    while (gui.running && !__atomic_load_n(&shared->quit_requested, __ATOMIC_ACQUIRE)) {
        if (!Gui_BeginFrame(&gui)) break;

        // set up default dock layout on first frame (before ini file exists)
        if (first_frame) {
            ImGuiID dockspace_id = ImGui::GetID("DockSpace");
            // only build layout if no saved layout exists
            if (ImGui::DockBuilderGetNode(dockspace_id) == NULL ||
                ImGui::DockBuilderGetNode(dockspace_id)->ChildNodes[0] == NULL) {
                Gui_SetupDefaultLayout(dockspace_id);
            }
            first_frame = false;
        }

        // read current snapshot
        int idx = __atomic_load_n(&shared->active_idx, __ATOMIC_ACQUIRE);
        const TUISnapshot *snap = &shared->snapshots[idx];

        // keyboard controls
        Gui_HandleKeys(shared);

        // update window title with live price + P&L
        if (snap->price > 0) {
            char title[128];
            snprintf(title, sizeof(title), "foxml trader  |  $%.2f  |  P&L $%+.2f",
                     snap->price, snap->total_pnl);
            SDL_SetWindowTitle(gui.window, title);
        }

        // dashboard panels (right side)
        GUI_RenderDashboard(snap, snap->start_time ? snap->start_time : gui_start);

        // charts — 3 separate dockable windows
        CandleSnapshot csnap = {};
        if (shared->candle_acc) {
            CandleAccumulator_Snapshot((CandleAccumulator *)shared->candle_acc, &csnap);
        }
        trades.max_visible_markers = chart_settings.visible_candles * 2;
        TradeData_Refresh(&trades);

        ChartState cs = {};
        ChartState_Prepare(&cs, &csnap, &chart_settings);
        CandleAccumulator *ca = shared->candle_acc ? (CandleAccumulator *)shared->candle_acc : NULL;
        GUI_PriceChart(&cs, snap, &trades, &chart_settings, ca, (void *)shared);
        GUI_VolumeChart(&cs, snap, &chart_settings);
        GUI_LivePnLChart(snap);
        GUI_EquityChart(&trades);

        // settings, trade history, log viewer
        GUI_Panel_Settings(&settings, &shared->reload_requested);
        GUI_Panel_TradeHistory(&trade_history);
        GUI_Panel_LogViewer(&log_viewer);

        Gui_EndFrame(&gui);
    }

    __atomic_store_n(&shared->quit_requested, 1, __ATOMIC_RELEASE);
    Gui_Shutdown(&gui);
    return NULL;
}
