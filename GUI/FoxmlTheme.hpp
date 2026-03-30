#pragma once
// FoxML Classic palette — matches Kitty terminal + Waybar + TUI exactly
// sourced from ~/THEME/FoxML/themes/FoxML_Classic/palette.sh
// and ~/.config/kitty/kitty.conf + ~/.config/waybar/style.css

#include "imgui.h"
#include "implot.h"

namespace FoxmlColors {
    // backgrounds — shifted towards slate (#3a414b) to remove pink undertone
    // kitty BG is #1a1214 (pink-warm), but GUI looks better with slate-neutral
    constexpr ImVec4 bg         = {0.098f, 0.094f, 0.098f, 1.0f};  // #191819 (neutral dark)
    constexpr ImVec4 bg_dark    = {0.071f, 0.071f, 0.075f, 1.0f};  // #121213 (neutral darker)
    constexpr ImVec4 bg_hl      = {0.133f, 0.137f, 0.149f, 1.0f};  // #222326 (slate-tinted highlight)
    constexpr ImVec4 grid       = {0.176f, 0.153f, 0.137f, 1.0f};  // #2d2723 (warm neutral, no purple)

    // text (from kitty.conf)
    constexpr ImVec4 text       = {0.835f, 0.769f, 0.690f, 1.0f};  // #d5c4b0
    constexpr ImVec4 fg_dim     = {0.478f, 0.478f, 0.478f, 1.0f};  // #7a7a7a
    constexpr ImVec4 comment    = {0.353f, 0.384f, 0.439f, 1.0f};  // #5a6270

    // warm tones
    constexpr ImVec4 wheat      = {0.831f, 0.706f, 0.514f, 1.0f};  // #d4b483
    constexpr ImVec4 clay       = {0.690f, 0.376f, 0.227f, 1.0f};  // #b0603a
    constexpr ImVec4 sand       = {0.659f, 0.604f, 0.478f, 1.0f};  // #a89a7a
    constexpr ImVec4 warm       = {0.690f, 0.643f, 0.596f, 1.0f};  // #b0a498
    constexpr ImVec4 primary    = {0.831f, 0.596f, 0.353f, 1.0f};  // #d4985a (waybar peach)
    constexpr ImVec4 secondary  = {0.722f, 0.590f, 0.478f, 1.0f};  // #b8967a
    constexpr ImVec4 accent     = {0.541f, 0.604f, 0.478f, 1.0f};  // #8a9a7a

    // semantic
    constexpr ImVec4 green      = {0.420f, 0.604f, 0.478f, 1.0f};  // #6b9a7a
    constexpr ImVec4 green_b    = {0.478f, 0.671f, 0.533f, 1.0f};  // #7aab88
    constexpr ImVec4 red        = {0.690f, 0.333f, 0.333f, 1.0f};  // #b05555
    constexpr ImVec4 red_b      = {0.753f, 0.408f, 0.408f, 1.0f};  // #c06868
    constexpr ImVec4 yellow     = {0.769f, 0.706f, 0.541f, 1.0f};  // #c4b48a
    constexpr ImVec4 blue       = {0.478f, 0.604f, 0.706f, 1.0f};  // #7a9ab4
    constexpr ImVec4 cyan       = {0.478f, 0.604f, 0.671f, 1.0f};  // #7a9aab

    // ui chrome (from waybar + kitty selection)
    constexpr ImVec4 surface    = {0.227f, 0.255f, 0.294f, 1.0f};  // #3a414b (kitty selection_bg)
    constexpr ImVec4 selection  = {0.227f, 0.243f, 0.275f, 1.0f};  // #3a3e46 (slate selection)
}

static inline void Foxml_ApplyTheme() {
    using namespace FoxmlColors;

    ImGuiStyle &s = ImGui::GetStyle();

    // rounding + spacing
    s.WindowRounding    = 0.0f;   // sharp corners like waybar
    s.FrameRounding     = 0.0f;
    s.GrabRounding      = 2.0f;
    s.TabRounding       = 0.0f;
    s.ScrollbarRounding = 0.0f;
    s.WindowPadding     = {8, 6};
    s.FramePadding      = {6, 3};
    s.ItemSpacing       = {8, 4};
    s.WindowBorderSize  = 1.0f;

    ImVec4 *c = s.Colors;

    // backgrounds — use bg (same as kitty), no plum
    c[ImGuiCol_WindowBg]        = bg;
    c[ImGuiCol_ChildBg]         = bg_dark;
    c[ImGuiCol_PopupBg]         = bg;
    c[ImGuiCol_MenuBarBg]       = bg;
    c[ImGuiCol_DockingEmptyBg]  = bg_dark;

    // text
    c[ImGuiCol_Text]            = text;
    c[ImGuiCol_TextDisabled]    = comment;

    // borders — warm gold like waybar border-image, not plum
    c[ImGuiCol_Border]          = {primary.x, primary.y, primary.z, 0.25f};
    c[ImGuiCol_BorderShadow]    = {0, 0, 0, 0};

    // frames — bg_hl (warm dark, no plum)
    c[ImGuiCol_FrameBg]         = bg_hl;
    c[ImGuiCol_FrameBgHovered]  = {surface.x, surface.y, surface.z, 0.6f};
    c[ImGuiCol_FrameBgActive]   = selection;

    // title bars
    c[ImGuiCol_TitleBg]         = bg_dark;
    c[ImGuiCol_TitleBgActive]   = bg_hl;
    c[ImGuiCol_TitleBgCollapsed]= bg_dark;

    // tabs — muted, matching waybar bubble style
    c[ImGuiCol_Tab]                 = bg_hl;
    c[ImGuiCol_TabHovered]          = {surface.x, surface.y, surface.z, 0.6f};
    c[ImGuiCol_TabSelected]         = {bg.x + 0.04f, bg.y + 0.03f, bg.z + 0.03f, 1.0f};
    c[ImGuiCol_TabSelectedOverline] = {primary.x, primary.y, primary.z, 0.4f};
    c[ImGuiCol_TabDimmed]           = bg_dark;
    c[ImGuiCol_TabDimmedSelected]   = bg_hl;

    // sliders + checkmarks — warm tones, NO blue defaults
    c[ImGuiCol_SliderGrab]      = {secondary.x, secondary.y, secondary.z, 0.8f};
    c[ImGuiCol_SliderGrabActive]= primary;
    c[ImGuiCol_CheckMark]       = primary;

    // scrollbar
    c[ImGuiCol_ScrollbarBg]     = bg_dark;
    c[ImGuiCol_ScrollbarGrab]   = {surface.x, surface.y, surface.z, 0.5f};
    c[ImGuiCol_ScrollbarGrabHovered]  = surface;
    c[ImGuiCol_ScrollbarGrabActive]   = sand;

    // buttons
    c[ImGuiCol_Button]          = bg_hl;
    c[ImGuiCol_ButtonHovered]   = {surface.x, surface.y, surface.z, 0.6f};
    c[ImGuiCol_ButtonActive]    = {primary.x, primary.y, primary.z, 0.25f};

    // headers
    c[ImGuiCol_Header]          = bg_hl;
    c[ImGuiCol_HeaderHovered]   = {surface.x, surface.y, surface.z, 0.6f};
    c[ImGuiCol_HeaderActive]    = {primary.x, primary.y, primary.z, 0.25f};

    // separator, resize grip
    c[ImGuiCol_Separator]       = {surface.x, surface.y, surface.z, 0.3f};
    c[ImGuiCol_SeparatorHovered]= {primary.x, primary.y, primary.z, 0.4f};
    c[ImGuiCol_SeparatorActive] = primary;
    c[ImGuiCol_ResizeGrip]      = {surface.x, surface.y, surface.z, 0.3f};
    c[ImGuiCol_ResizeGripHovered]= {primary.x, primary.y, primary.z, 0.4f};
    c[ImGuiCol_ResizeGripActive]= primary;

    // docking
    c[ImGuiCol_DockingPreview]  = {primary.x, primary.y, primary.z, 0.3f};

    // plots
    c[ImGuiCol_PlotLines]       = primary;
    c[ImGuiCol_PlotLinesHovered]= wheat;
    c[ImGuiCol_PlotHistogram]   = secondary;
    c[ImGuiCol_PlotHistogramHovered] = wheat;

    // table
    c[ImGuiCol_TableHeaderBg]   = bg_hl;
    c[ImGuiCol_TableBorderStrong]= {surface.x, surface.y, surface.z, 0.3f};
    c[ImGuiCol_TableBorderLight]= {surface.x, surface.y, surface.z, 0.15f};
    c[ImGuiCol_TableRowBg]      = {0, 0, 0, 0};
    c[ImGuiCol_TableRowBgAlt]   = {bg_hl.x, bg_hl.y, bg_hl.z, 0.3f};

    // ── implot theme ──
    ImPlotStyle &ps = ImPlot::GetStyle();
    ps.Colors[ImPlotCol_PlotBg]     = bg_dark;
    ps.Colors[ImPlotCol_PlotBorder] = {surface.x, surface.y, surface.z, 0.2f};
    ps.Colors[ImPlotCol_LegendBg]   = {bg.x, bg.y, bg.z, 0.8f};
    ps.Colors[ImPlotCol_LegendBorder] = {surface.x, surface.y, surface.z, 0.3f};
    ps.Colors[ImPlotCol_LegendText] = text;
    ps.Colors[ImPlotCol_TitleText]  = wheat;
    ps.Colors[ImPlotCol_InlayText]  = sand;
    ps.Colors[ImPlotCol_AxisText]   = comment;
    ps.Colors[ImPlotCol_AxisGrid]   = {surface.x, surface.y, surface.z, 0.15f};
    ps.Colors[ImPlotCol_AxisBgHovered]  = {surface.x, surface.y, surface.z, 0.3f};
    ps.Colors[ImPlotCol_AxisBgActive]   = {primary.x, primary.y, primary.z, 0.25f};
    ps.Colors[ImPlotCol_Crosshairs] = {wheat.x, wheat.y, wheat.z, 0.5f};
}
