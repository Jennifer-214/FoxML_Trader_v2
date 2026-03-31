#pragma once
// DashboardPanels — all engine dashboard panels for ImGui
// 1:1 port of ANSI_Section_* from TUIAnsi.hpp
// each panel is a dockable ImGui window reading from TUISnapshot
//
// adding a new panel:
//   1. write GUI_Panel_YourName(snap)
//   2. call it from GUI_RenderDashboard()
//   3. done — it's automatically dockable/rearrangeable

#include "imgui.h"
#include "implot.h"
#include "FoxmlTheme.hpp"
#include "../Version.hpp"
#include <ctime>

// ── helper: colored value text (green if positive, red if negative) ──
static inline ImVec4 PnlColor(double val) {
    return (val >= 0) ? FoxmlColors::green_b : FoxmlColors::red;
}

// ── helper: R² progress bar ──
// slope_dir: positive slope → green, negative → red, near zero → neutral
static inline void GUI_R2Bar(const char *label, double r2, float width = 80.0f,
                              double slope = 0.0) {
    ImGui::Text("%s", label);
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::text, "%.3f", r2);
    ImGui::SameLine();
    // color by trend direction: green=up, red=down, dim=flat
    // intensity scales with R² (high R² = more confident in direction)
    ImVec4 bar_color;
    if (r2 < 0.1)
        bar_color = FoxmlColors::comment;  // no signal
    else if (slope > 0.0001)
        bar_color = (r2 > 0.5) ? FoxmlColors::green_b : FoxmlColors::accent;  // uptrend
    else if (slope < -0.0001)
        bar_color = (r2 > 0.5) ? FoxmlColors::red : FoxmlColors::clay;  // downtrend
    else
        bar_color = FoxmlColors::sand;  // flat
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar_color);
    ImGui::ProgressBar((float)r2, ImVec2(width, ImGui::GetTextLineHeight()), "");
    ImGui::PopStyleColor();
}

// ── helper: section header with peach color ──
static inline void SectionHeader(const char *title) {
    ImGui::TextColored(FoxmlColors::primary, "%s", title);
    ImGui::Separator();
}

// ── helper: labeled value on same line ──
static inline void LabeledValue(const char *label, const char *fmt, ...) {
    ImGui::TextColored(FoxmlColors::sand, "%s", label);
    ImGui::SameLine();
    va_list args;
    va_start(args, fmt);
    char buf[128];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ImGui::TextUnformatted(buf);
}

//==========================================================================
// PANEL: HEADER — fox kaomoji, version, state, uptime, session
//==========================================================================
static inline void GUI_Panel_Header(const TUISnapshot *s, uint64_t start_time) {
    ImGui::Begin("Header", nullptr, ImGuiWindowFlags_NoTitleBar);

    // fox kaomoji (Hack Nerd Font loaded with Japanese glyph ranges)
    ImGui::TextColored(FoxmlColors::primary, "/l\u3001          FOXML TRADER");
    ImGui::TextColored(FoxmlColors::primary, "( \u00b0_ \u00b0 7");
    ImGui::SameLine(180);
    ImGui::TextColored(FoxmlColors::wheat, "engine v" ENGINE_VERSION_STRING);
    ImGui::TextColored(FoxmlColors::primary, "\u30c9  \u30d8");
    ImGui::TextColored(FoxmlColors::primary, "\u3058\u3057_,)\u30ce");

    ImGui::Separator();

    // state + uptime
    time_t now = time(NULL);
    unsigned elapsed = (unsigned)difftime(now, (time_t)start_time);
    unsigned hours = elapsed / 3600, mins = (elapsed % 3600) / 60, secs = elapsed % 60;
    const char *state_str = (s->engine_state == 0) ? "WARMUP" :
                            (s->engine_state == 2) ? "CLOSING" : "ACTIVE";

    if (s->live_trading)
        ImGui::TextColored(FoxmlColors::red_b, "LIVE");
    else
        ImGui::TextColored(FoxmlColors::comment, "PAPER");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::sand, "STATE:");
    ImGui::SameLine();
    ImGui::Text("%-8s", state_str);
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::comment, "|");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::sand, "UPTIME:");
    ImGui::SameLine();
    ImGui::Text("%02u:%02u:%02u", hours, mins, secs);

    if (s->is_paused) {
        const char *reason = "gate";
        if (s->sl_cooldown > 0) reason = "cooldown";
        else if (s->breaker_tripped) reason = "breaker";
        else if (s->current_regime == 2) reason = "volatile";
        else if (s->current_regime == 3) reason = "downtrend";
        else if (s->engine_state == 0) reason = "warmup";
        else if (s->long_gate_enabled && !s->long_gate_ok) reason = "long trend";
        else if (s->buy_p > 0.01) {
            int price_ok = s->gate_direction
                ? (s->price >= s->buy_p) : (s->price <= s->buy_p);
            int vol_ok = (s->volume >= s->buy_v);
            if (!price_ok && !vol_ok) reason = "price+vol";
            else if (!price_ok) reason = "price";
            else if (!vol_ok) reason = "volume";
        }
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "|");
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::yellow, "PAUSED (%s)", reason);
    }

    if (s->current_session >= 0) {
        static const char *sess_names[] = {"ASIA", "EU", "US", "OVERNIGHT"};
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "|");
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::sand, "%s (%.1fx)", sess_names[s->current_session], s->session_mult);
    }

    // trading blocked indicator
    if (s->engine_state == 0) {
        ImGui::TextColored(FoxmlColors::yellow, "BUYING PAUSED");
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "warmup — waiting for market data (%d/%d samples)",
                          s->roll_count, s->min_warmup_samples);
    } else if (s->sl_cooldown > 0) {
        ImGui::TextColored(FoxmlColors::yellow, "BUYING PAUSED");
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "post-SL cooldown (%d cycles remaining)", s->sl_cooldown);
    } else if (s->current_regime == 2 || s->current_regime == 3) {
        ImGui::TextColored(FoxmlColors::yellow, "BUYING PAUSED");
        ImGui::SameLine();
        const char *r = (s->current_regime == 3) ? "downtrend" : "volatile regime";
        ImGui::TextColored(FoxmlColors::comment, "%s — buying paused", r);
    } else if (s->breaker_tripped) {
        ImGui::TextColored(FoxmlColors::red, "BUYING PAUSED");
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "circuit breaker — max drawdown hit");
    }

    ImGui::End();
}

//==========================================================================
// PANEL: TOP BAR — key metrics at a glance
//==========================================================================
static inline void GUI_Panel_TopBar(const TUISnapshot *s) {
    ImGui::Begin("Top Bar", nullptr, ImGuiWindowFlags_NoTitleBar);

    ImGui::TextColored(FoxmlColors::wheat, "$%.2f", s->price);
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::comment, "(%.6f)", s->volume);
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::comment, "|");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::sand, "P&L");
    ImGui::SameLine();
    ImGui::TextColored(PnlColor(s->total_pnl), "$%+.2f", s->total_pnl);

    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::comment, "|");
    ImGui::SameLine();

    ImVec4 regime_color = (s->current_regime == 1) ? FoxmlColors::green :
                          (s->current_regime == 2 || s->current_regime == 3) ? FoxmlColors::red : FoxmlColors::comment;
    const char *regime_str = (s->current_regime == 1) ? "TREND" :
                             (s->current_regime == 2) ? "VOLAT" :
                             (s->current_regime == 3) ? "TR_DN" : "RANGE";
    ImGui::TextColored(regime_color, "%s", regime_str);

    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::comment, "|");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::sand, "POS");
    ImGui::SameLine();
    ImGui::Text("%d/%d", s->active_count, s->max_positions);

    uint32_t total_exits = s->wins + s->losses;
    if (total_exits > 0) {
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "|");
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::green, "W:%u", s->wins);
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::red, "L:%u", s->losses);
        ImGui::SameLine();
        ImGui::TextColored((s->win_rate >= 50.0) ? FoxmlColors::green : FoxmlColors::red,
                          "%.0f%%", s->win_rate);
    }

    ImGui::End();
}

//==========================================================================
// PANEL: MARKET STRUCTURE
//==========================================================================
static inline void GUI_Panel_Market(const TUISnapshot *s) {
    ImGui::Begin("Market Structure");
    SectionHeader("MARKET STRUCTURE");

    // avg + stddev + R²
    ImGui::TextColored(FoxmlColors::sand, "avg:");
    ImGui::SameLine();
    ImGui::Text("%.2f", s->roll_price_avg);
    ImGui::SameLine(0, 20);
    ImGui::TextColored(FoxmlColors::sand, "stddev:");
    ImGui::SameLine();
    ImGui::Text("%.2f", s->roll_stddev);
    ImGui::SameLine(0, 20);
    GUI_R2Bar("R²:", s->short_r2, 80.0f, s->slope_pct);

    // short slope + arrow
    const char *trend_arrow = (s->slope_pct > 0.001) ? "^" :
                              (s->slope_pct < -0.001) ? "v" : ">";
    ImVec4 trend_color = (s->slope_pct > 0.001) ? FoxmlColors::green :
                         (s->slope_pct < -0.001) ? FoxmlColors::red : FoxmlColors::comment;
    ImGui::TextColored(FoxmlColors::sand, "slope:");
    ImGui::SameLine();
    ImGui::TextColored(trend_color, "%+.6f%%/tick %s", s->slope_pct, trend_arrow);

    // long slope
    const char *lt_arrow = (s->long_slope_pct > 0.001) ? "^" :
                           (s->long_slope_pct < -0.001) ? "v" : ">";
    ImVec4 lt_color = (s->long_slope_pct > 0.001) ? FoxmlColors::green :
                      (s->long_slope_pct < -0.001) ? FoxmlColors::red : FoxmlColors::comment;
    ImGui::TextColored(FoxmlColors::sand, "long:");
    ImGui::SameLine();
    ImGui::TextColored(lt_color, "%+.6f%%/tick", s->long_slope_pct);
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::comment, "(%d-tick)", s->long_count);
    ImGui::SameLine();
    ImGui::TextColored(lt_color, "%s", lt_arrow);

    ImGui::End();
}

//==========================================================================
// PANEL: REGIME SIGNALS
//==========================================================================
static inline void GUI_Panel_Regime(const TUISnapshot *s) {
    ImGui::Begin("Regime Signals");
    SectionHeader("REGIME SIGNALS");

    const char *regime_name = (s->current_regime == 1) ? "TRENDING" :
                              (s->current_regime == 2) ? "VOLATILE" :
                              (s->current_regime == 3) ? "TRENDING_DOWN" : "RANGING";
    ImVec4 regime_color = (s->current_regime == 1) ? FoxmlColors::green :
                          (s->current_regime == 2 || s->current_regime == 3) ? FoxmlColors::red : FoxmlColors::comment;
    const char *strat_name = (s->strategy_id == 4) ? "EMA CROSS" :
                              (s->strategy_id == 2) ? "SIMPLE DIP" :
                              (s->strategy_id == 1) ? "MOMENTUM" : "MEAN REVERSION";

    ImGui::TextColored(FoxmlColors::sand, "regime:");
    ImGui::SameLine();
    ImGui::TextColored(regime_color, "%s", regime_name);
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::comment, "(%.0fm)", s->regime_duration_min);
    ImGui::SameLine(0, 20);
    ImGui::TextColored(FoxmlColors::sand, "strategy:");
    ImGui::SameLine();
    if (s->regime_auto) {
        ImGui::TextColored(FoxmlColors::primary, "AUTO");
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, ">");
        ImGui::SameLine();
    }
    ImGui::Text("%s", strat_name);

    // R² short + long
    GUI_R2Bar("short:", s->short_r2, 80.0f, s->slope_pct);
    ImGui::SameLine(0, 20);
    GUI_R2Bar("long:", s->long_r2, 80.0f, s->long_slope_pct);

    // vol ratio + ROR
    ImVec4 vr_color = (s->vol_ratio > 2.0) ? FoxmlColors::red :
                      (s->vol_ratio > 1.5) ? FoxmlColors::yellow : FoxmlColors::text;
    ImVec4 ror_color = (s->ror_slope > 0.0001) ? FoxmlColors::green :
                       (s->ror_slope < -0.0001) ? FoxmlColors::red : FoxmlColors::comment;
    const char *ror_arrow = (s->ror_slope > 0.0001) ? "^" :
                            (s->ror_slope < -0.0001) ? "v" : ">";

    ImGui::TextColored(FoxmlColors::sand, "vol ratio:");
    ImGui::SameLine();
    ImGui::TextColored(vr_color, "%.2f", s->vol_ratio);
    ImGui::SameLine(0, 20);
    ImGui::TextColored(FoxmlColors::sand, "ror:");
    ImGui::SameLine();
    ImGui::TextColored(ror_color, "%+.6f %s", s->ror_slope, ror_arrow);

    if (s->spike_active) {
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::yellow, "SPIKE %.1fx", s->volume_spike_ratio);
    } else if (s->volume_spike_ratio > 1.0) {
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::comment, "vol:%.1fx", s->volume_spike_ratio);
    }

    // VWAP
    if (s->vwap > 0.0) {
        ImVec4 vwap_color = (s->vwap_dev < -0.001) ? FoxmlColors::green :
                            (s->vwap_dev > 0.001) ? FoxmlColors::red : FoxmlColors::text;
        ImGui::TextColored(FoxmlColors::sand, "vwap:");
        ImGui::SameLine();
        ImGui::Text("$%.2f", s->vwap);
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::sand, "dev:");
        ImGui::SameLine();
        ImGui::TextColored(vwap_color, "%+.3f%%", s->vwap_dev * 100.0);

        if (s->book_imbalance != 0.0) {
            ImVec4 book_color = (s->book_imbalance > 0.1) ? FoxmlColors::green :
                                (s->book_imbalance < -0.1) ? FoxmlColors::red : FoxmlColors::text;
            ImGui::SameLine(0, 10);
            ImGui::TextColored(FoxmlColors::sand, "book:");
            ImGui::SameLine();
            ImGui::TextColored(book_color, "%+.2f", s->book_imbalance);
        }
    }

    ImGui::End();
}

//==========================================================================
// PANEL: BUY GATE
//==========================================================================
static inline void GUI_Panel_BuyGate(const TUISnapshot *s) {
    ImGui::Begin("Buy Gate");
    SectionHeader("BUY GATE");

    const char *gate_op = s->gate_direction ? ">=" : "<=";

    // price gate
    ImGui::TextColored(FoxmlColors::sand, "price %s", gate_op);
    ImGui::SameLine();
    ImGui::Text("%.2f", s->buy_p);
    ImGui::SameLine(0, 10);
    if (s->stddev_mode)
        ImGui::TextColored(FoxmlColors::comment, "(stddev: %.2fx)", s->live_sm);
    else
        ImGui::TextColored(FoxmlColors::comment, "(offset: %.3f%%)", s->live_offset);

    if (s->buy_p > 0.01) {
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::sand, "dist:");
        ImGui::SameLine();
        ImGui::Text("$%.2f (%.3f%%)", s->gate_dist, s->gate_dist_pct);
    } else {
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::comment, "(gate disabled)");
    }

    // volume gate + status
    ImGui::TextColored(FoxmlColors::sand, "vol   %s", gate_op);
    ImGui::SameLine();
    ImGui::Text("%.8f", s->buy_v);
    ImGui::SameLine(0, 10);
    ImGui::TextColored(FoxmlColors::comment, "(mult: %.2fx)", s->live_vmult);

    ImGui::SameLine(0, 10);
    if (s->buy_p < 0.01) {
        ImGui::TextColored(FoxmlColors::yellow, "GATE OFF");
    } else {
        int price_ok = s->gate_direction
            ? (s->price >= s->buy_p)
            : (s->price <= s->buy_p);
        int vol_ok = (s->volume >= s->buy_v);
        if (price_ok && vol_ok)
            ImGui::TextColored(FoxmlColors::green_b, "READY");
        else if (!price_ok && !vol_ok)
            ImGui::TextColored(FoxmlColors::yellow, "wait: price+vol");
        else if (!price_ok)
            ImGui::TextColored(FoxmlColors::yellow, "wait: price");
        else
            ImGui::TextColored(FoxmlColors::yellow, "wait: vol");
    }

    // spacing + long trend
    ImGui::TextColored(FoxmlColors::sand, "spacing:");
    ImGui::SameLine();
    ImGui::Text("$%.2f (%.3f%%)", s->spacing, s->spacing_pct);
    if (s->long_gate_enabled) {
        ImGui::SameLine(0, 20);
        ImGui::TextColored(FoxmlColors::sand, "long trend:");
        ImGui::SameLine();
        if (s->long_gate_ok)
            ImGui::TextColored(FoxmlColors::green, "OK");
        else
            ImGui::TextColored(FoxmlColors::red, "BLOCKED");
    }
    if (s->sl_cooldown > 0) {
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::yellow, "COOLDOWN (%d)", s->sl_cooldown);
    }

    // fill rejection diagnostics
    if (s->fills_rejected > 0 && s->last_reject_reason > 0 && s->last_reject_reason <= 7) {
        static const char *reasons[] = {"", "spacing", "balance", "exposure",
                                         "breaker", "max_pos", "duplicate", "min_vol"};
        ImGui::TextColored(FoxmlColors::comment, "fills");
        ImGui::SameLine();
        ImGui::Text("%u/%u", s->total_buys, s->total_buys + s->fills_rejected);
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::comment, "last reject:");
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::yellow, "%s", reasons[s->last_reject_reason]);
    }

    ImGui::End();
}

//==========================================================================
// PANEL: PORTFOLIO
//==========================================================================
static inline void GUI_Panel_Portfolio(const TUISnapshot *s) {
    ImGui::Begin("Portfolio");
    SectionHeader("PORTFOLIO");

    ImGui::TextColored(FoxmlColors::sand, "equity:");
    ImGui::SameLine();
    ImGui::Text("$%.2f", s->equity);
    ImGui::SameLine(0, 30);
    ImGui::TextColored(FoxmlColors::sand, "balance:");
    ImGui::SameLine();
    ImGui::Text("$%.2f", s->balance);

    ImGui::TextColored(FoxmlColors::sand, "exposure:");
    ImGui::SameLine();
    ImGui::Text("%.1f%%/%.0f%%", s->exposure_pct, s->max_exp);
    ImGui::SameLine(0, 30);
    ImGui::TextColored(FoxmlColors::sand, "fees:");
    ImGui::SameLine();
    ImGui::Text("$%.4f", s->fees);

    ImGui::End();
}

//==========================================================================
// PANEL: P&L
//==========================================================================
static inline void GUI_Panel_PnL(const TUISnapshot *s) {
    ImGui::Begin("P&L");
    SectionHeader("P&L");

    ImGui::TextColored(FoxmlColors::sand, "realized:");
    ImGui::SameLine();
    ImGui::TextColored(PnlColor(s->realized), "$%+.4f", s->realized);
    ImGui::SameLine(0, 20);
    ImGui::TextColored(FoxmlColors::sand, "unrealized:");
    ImGui::SameLine();
    ImGui::TextColored(PnlColor(s->unrealized), "$%+.4f", s->unrealized);

    ImGui::TextColored(FoxmlColors::sand, "total:");
    ImGui::SameLine();
    ImGui::TextColored(PnlColor(s->total_pnl), "$%+.4f", s->total_pnl);
    ImGui::SameLine(0, 30);
    ImGui::TextColored(FoxmlColors::comment, "(");
    ImGui::SameLine(0, 0);
    ImGui::TextColored(PnlColor(s->return_pct), "%+.2f%%", s->return_pct);
    ImGui::SameLine(0, 0);
    ImGui::TextColored(FoxmlColors::comment, ")");

    ImGui::End();
}

//==========================================================================
// PANEL: RISK
//==========================================================================
static inline void GUI_Panel_Risk(const TUISnapshot *s) {
    ImGui::Begin("Risk");
    SectionHeader("RISK");

    ImGui::TextColored(FoxmlColors::sand, "risk/pos:");
    ImGui::SameLine();
    ImGui::Text("%.1f%%", s->risk_amt);
    ImGui::SameLine(0, 10);
    ImGui::TextColored(FoxmlColors::comment, "|");
    ImGui::SameLine(0, 10);
    ImGui::TextColored(FoxmlColors::sand, "breaker:");
    ImGui::SameLine();
    if (s->breaker_tripped)
        ImGui::TextColored(FoxmlColors::red, "TRIPPED");
    else
        ImGui::TextColored(FoxmlColors::green, "OK");

    ImGui::End();
}

//==========================================================================
// PANEL: CONFIG
//==========================================================================
static inline void GUI_Panel_Config(const TUISnapshot *s) {
    ImGui::Begin("Config");
    SectionHeader("CONFIG");

    ImGui::TextColored(FoxmlColors::sand, "TP:");
    ImGui::SameLine();
    ImGui::Text("%.1f%%", s->cfg_tp);
    ImGui::SameLine(0, 10);
    ImGui::TextColored(FoxmlColors::sand, "SL:");
    ImGui::SameLine();
    ImGui::Text("%.1f%%", s->cfg_sl);
    ImGui::SameLine(0, 10);
    ImGui::TextColored(FoxmlColors::sand, "fee:");
    ImGui::SameLine();
    ImGui::Text("%.1f%%", s->cfg_fee);

    if (s->cfg_slippage > 0.0) {
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::sand, "slip:");
        ImGui::SameLine();
        ImGui::Text("%.2f%%", s->cfg_slippage);
    }

    if (s->trailing_enabled) {
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::sand, "trail:");
        ImGui::SameLine();
        ImGui::Text("%.1f", s->cfg_trail_mult);
        ImGui::SameLine(0, 5);
        ImGui::TextColored(FoxmlColors::sand, "score:");
        ImGui::SameLine();
        ImGui::Text("%.2f", s->cfg_hold_score);
    } else {
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::comment, "trailing: off");
    }

    if (s->live_trading) {
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::red_b, "LIVE");
    }

    ImGui::End();
}

//==========================================================================
// PANEL: POSITIONS — proper table with aligned columns
//==========================================================================
static inline void GUI_Panel_Positions(const TUISnapshot *s) {
    ImGui::Begin("Positions");

    ImGui::TextColored(FoxmlColors::primary, "POSITIONS");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::comment, "(%d/%d)", s->active_count, s->max_positions);
    ImGui::Separator();

    int has_positions = 0;
    for (int i = 0; i < 16; i++)
        if (s->positions[i].idx >= 0) { has_positions = 1; break; }

    if (!has_positions) {
        ImGui::TextColored(FoxmlColors::comment, "(no positions)");
        ImGui::End();
        return;
    }

    ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("##positions", 10, flags)) {
        ImGui::TableSetupColumn("#",      ImGuiTableColumnFlags_WidthFixed, 25);
        ImGui::TableSetupColumn("Entry",  ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Now",    ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Diff",   ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("TP",     ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("SL",     ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Value",  ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("Gross",  ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("Net",    ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("Hold",   ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableHeadersRow();

        int displayed = 0;
        for (int i = 0; i < 16; i++) {
            const TUIPositionSnap *ps = &s->positions[i];
            if (ps->idx < 0) continue;

            double diff = s->price - ps->entry;
            ImGui::TableNextRow();

            // # column
            ImGui::TableNextColumn();
            ImGui::TextColored(FoxmlColors::wheat, "#%d", displayed);

            // entry
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", ps->entry);

            // current price
            ImGui::TableNextColumn();
            ImGui::TextColored(FoxmlColors::wheat, "%.0f", s->price);

            // diff
            ImGui::TableNextColumn();
            ImGui::TextColored(PnlColor(diff), "%+.0f", diff);

            // TP
            ImGui::TableNextColumn();
            ImGui::TextColored(FoxmlColors::green, "%.0f", ps->tp);

            // SL
            ImGui::TableNextColumn();
            ImGui::TextColored(FoxmlColors::red, "%.0f", ps->sl);

            // value
            ImGui::TableNextColumn();
            ImGui::Text("$%.0f", ps->value);

            // gross P&L %
            ImGui::TableNextColumn();
            ImGui::TextColored(PnlColor(ps->gross_pnl), "%+.2f%%", ps->gross_pnl);

            // net P&L %
            ImGui::TableNextColumn();
            ImGui::TextColored(PnlColor(ps->net_pnl), "%+.2f%%", ps->net_pnl);

            // hold time + trailing indicator
            ImGui::TableNextColumn();
            if (ps->above_orig_tp && ps->is_trailing)
                ImGui::TextColored(FoxmlColors::yellow, "%.0fm H", ps->hold_minutes);
            else if (ps->is_trailing)
                ImGui::TextColored(FoxmlColors::yellow, "%.0fm T", ps->hold_minutes);
            else
                ImGui::Text("%.0fm", ps->hold_minutes);

            displayed++;
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

//==========================================================================
// PANEL: STATS
//==========================================================================
static inline void GUI_Panel_Stats(const TUISnapshot *s) {
    ImGui::Begin("Stats");
    SectionHeader("STATS");

    uint32_t total_exits = s->wins + s->losses;

    ImGui::TextColored(FoxmlColors::sand, "buys:");
    ImGui::SameLine();
    ImGui::Text("%-4u", s->total_buys);
    ImGui::SameLine(0, 10);
    ImGui::TextColored(FoxmlColors::sand, "exits:");
    ImGui::SameLine();
    ImGui::Text("%-4u", total_exits);
    ImGui::SameLine(0, 10);
    ImGui::TextColored(FoxmlColors::sand, "W:");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::green, "%u", s->wins);
    ImGui::SameLine(0, 10);
    ImGui::TextColored(FoxmlColors::sand, "L:");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::red, "%u", s->losses);

    ImGui::TextColored(FoxmlColors::sand, "rate:");
    ImGui::SameLine();
    ImGui::TextColored(PnlColor(s->win_rate - 50.0), "%.1f%%", s->win_rate);
    ImGui::SameLine(0, 10);
    ImGui::TextColored(FoxmlColors::sand, "pf:");
    ImGui::SameLine();
    ImGui::TextColored(PnlColor(s->profit_factor - 1.0), "%.2f", s->profit_factor);
    ImGui::SameLine(0, 10);
    ImGui::TextColored(FoxmlColors::sand, "avg W:");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::green, "$%.2f", s->avg_win);
    ImGui::SameLine(0, 10);
    ImGui::TextColored(FoxmlColors::sand, "L:");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::red, "$%.2f", s->avg_loss);

    if (s->losses > 0 && s->avg_loss_market < s->avg_loss) {
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "(mkt: $%.2f)", s->avg_loss_market);
    }

    if (total_exits > 0) {
        ImGui::TextColored(FoxmlColors::sand, "E[trade]:");
        ImGui::SameLine();
        ImGui::TextColored(PnlColor(s->expectancy), "$%+.2f", s->expectancy);
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::sand, "maxDD:");
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::red, "$%.2f", s->max_drawdown);
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "(%.2f%%)", s->max_drawdown_pct);
        if (s->fee_ratio > 0.0) {
            ImGui::SameLine(0, 10);
            ImGui::TextColored(FoxmlColors::sand, "fees/wins:");
            ImGui::SameLine();
            ImGui::TextColored(FoxmlColors::comment, "%.0f%%", s->fee_ratio);
        }
    }

    ImGui::End();
}

//==========================================================================
// PANEL: LATENCY (conditional on LATENCY_PROFILING)
//==========================================================================
#ifdef LATENCY_PROFILING
static inline void GUI_Panel_Latency(const TUISnapshot *s) {
    ImGui::Begin("Latency");
    SectionHeader("LATENCY");

    if (s->hot_count > 0) {
        ImGui::TextColored(FoxmlColors::sand, "hot:");
        ImGui::SameLine();
        ImGui::Text("avg %.0fns", s->hot_avg_ns);
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::comment, "p50");
        ImGui::SameLine();
        ImGui::Text("%.0fns", s->hot_p50_ns);
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::comment, "p95");
        ImGui::SameLine();
        ImGui::Text("%.0fns", s->hot_p95_ns);
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "(%lu)", (unsigned long)s->hot_count);

        ImGui::TextColored(FoxmlColors::comment, "  bg:");
        ImGui::SameLine();
        ImGui::Text("%.0fns", s->bg_avg_ns);
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::comment, "eg:");
        ImGui::SameLine();
        ImGui::Text("%.0fns (%.0f/pos)", s->eg_avg_ns, s->eg_per_pos_ns);
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::comment, "pc:");
        ImGui::SameLine();
        ImGui::Text("%.0fns", s->pc_avg_ns);
    }

    if (s->slow_count > 0) {
        const char *unit = (s->slow_avg_ns >= 1000.0) ? "us" : "ns";
        double avg = (s->slow_avg_ns >= 1000.0) ? s->slow_avg_ns / 1000.0 : s->slow_avg_ns;
        ImGui::TextColored(FoxmlColors::sand, "slow:");
        ImGui::SameLine();
        ImGui::Text("avg %.1f%s", avg, unit);
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "(%lu)", (unsigned long)s->slow_count);
    }

    ImGui::End();
}
#endif

//==========================================================================
// RENDER ALL DASHBOARD PANELS
//==========================================================================
static inline void GUI_RenderDashboard(const TUISnapshot *s, uint64_t start_time) {
    GUI_Panel_Header(s, start_time);
    GUI_Panel_TopBar(s);
    GUI_Panel_Market(s);
    GUI_Panel_Regime(s);
    GUI_Panel_BuyGate(s);
    GUI_Panel_Portfolio(s);
    GUI_Panel_PnL(s);
    GUI_Panel_Risk(s);
    GUI_Panel_Config(s);
    GUI_Panel_Positions(s);
    GUI_Panel_Stats(s);
#ifdef LATENCY_PROFILING
    GUI_Panel_Latency(s);
#endif
}
