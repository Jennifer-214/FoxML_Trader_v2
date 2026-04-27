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
#include "../Strategies/StrategyInterface.hpp"
#include "SettingsPanel.hpp"  // cfg_write_field for hot-swap persistence
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
    // v4.0.4: prefer state_warmup (populated by both sharded + legacy
    // snapshots) over engine_state (legacy field, not populated in sharded
    // mode — caused stuck "WARMUP" display after warmup actually completed).
    const char *state_str = s->state_warmup ? "WARMUP" :
                            (s->engine_state == 2) ? "CLOSING" : "ACTIVE";

    if (s->live_trading)
        ImGui::TextColored(FoxmlColors::red_b, "LIVE");
    else
        ImGui::TextColored(FoxmlColors::comment, "PAPER");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::sand, "STATE:");
    ImGui::SameLine();
    ImGui::Text("%-8s", state_str);
    // v4.0.4: warmup progress display when in warmup. Shows samples
    // collected vs target so user knows how long until trading starts.
    if (s->state_warmup && s->min_warmup_samples > 0) {
        int remaining = s->min_warmup_samples - s->warmup_samples_now;
        if (remaining < 0) remaining = 0;
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::yellow,
            "(%d / %d, %d to go)",
            s->warmup_samples_now, s->min_warmup_samples, remaining);
    }
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::comment, "|");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::sand, "UPTIME:");
    ImGui::SameLine();
    ImGui::Text("%02u:%02u:%02u", hours, mins, secs);

    if (s->is_paused && s->gate_reason > 0) {
        int ri = (s->gate_reason >= 0 && s->gate_reason < NUM_GATE_REASONS) ? s->gate_reason : 0;
        ImVec4 color = GATE_REASON_TABLE[ri].is_danger ? FoxmlColors::red : FoxmlColors::yellow;
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "|");
        ImGui::SameLine();
        ImGui::TextColored(color, "PAUSED (%s)", GATE_REASON_TABLE[ri].name);
    }

    if (s->current_session >= 0 && s->current_session < NUM_SESSIONS) {
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "|");
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::sand, "%s (%.1fx)", SESSION_NAMES[s->current_session], s->session_mult);
    }

    // trading blocked indicator — detailed reason from gate_reason table
    if (s->gate_reason > 0 && s->is_paused) {
        int ri = (s->gate_reason >= 0 && s->gate_reason < NUM_GATE_REASONS) ? s->gate_reason : 0;
        ImVec4 hdr = GATE_REASON_TABLE[ri].is_danger ? FoxmlColors::red : FoxmlColors::yellow;
        ImGui::TextColored(hdr, "BUYING PAUSED");
        ImGui::SameLine();
        // 3 reasons have dynamic data — format them, rest use table description directly
        char detail[128];
        if (ri == GATE_REASON_WARMUP)
            snprintf(detail, sizeof(detail), GATE_REASON_TABLE[ri].description, s->roll_count, s->min_warmup_samples);
        else if (ri == GATE_REASON_DANGER)
            snprintf(detail, sizeof(detail), GATE_REASON_TABLE[ri].description, s->danger_score * 100.0);
        else if (ri == GATE_REASON_COOLDOWN)
            snprintf(detail, sizeof(detail), GATE_REASON_TABLE[ri].description, s->sl_cooldown);
        else
            snprintf(detail, sizeof(detail), "%s", GATE_REASON_TABLE[ri].description);
        ImGui::TextColored(FoxmlColors::comment, "%s", detail);
    }

    // per-core strategy overview (sharded mode)
    if (s->sharded_mode_active && s->per_core_count > 0) {
        static const ImVec4 sc[] = {
            {0.40f, 0.60f, 0.85f, 1.0f},  // MR blue
            {0.85f, 0.55f, 0.25f, 1.0f},  // MOM orange
            {0.45f, 0.75f, 0.45f, 1.0f},  // DIP green
            {0.65f, 0.45f, 0.80f, 1.0f},  // ML purple
            {0.35f, 0.75f, 0.80f, 1.0f},  // EMA cyan
        };
        ImGui::TextColored(FoxmlColors::sand, "CORES:");
        for (int i = 0; i < s->per_core_count && i < 16; ++i) {
            ImGui::SameLine();
            uint8_t sid = s->per_core[i].strategy_id_display;
            ImVec4 col = (sid < NUM_STRATEGIES) ? sc[sid] : FoxmlColors::comment;
            const char *name = (sid < NUM_STRATEGIES) ? STRATEGY_SHORT_NAMES[sid]
                               : (sid == 0xFF ? "OFF" : "?");
            ImGui::TextColored(col, "C%d:%s", i, name);
        }
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

    int ri = s->current_regime;
    if (ri < 0 || ri >= NUM_REGIMES) ri = 0;
    ImVec4 regime_color = (ri == REGIME_TRENDING) ? FoxmlColors::green :
                          (ri == REGIME_MILD_TREND) ? FoxmlColors::sand :
                          (ri == REGIME_VOLATILE || ri == REGIME_TRENDING_DOWN) ? FoxmlColors::red : FoxmlColors::comment;
    ImGui::TextColored(regime_color, "%s", REGIME_INFO[ri].short_name);

    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::comment, "|");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::sand, "POS");
    ImGui::SameLine();
    ImGui::Text("%d/%d", s->active_count, s->max_positions);

    ImGui::End();
}

//==========================================================================
// PANEL: MARKET (merged Market Structure + Regime Signals)
//==========================================================================
static inline void GUI_Panel_Market(const TUISnapshot *s) {
    ImGui::Begin("Market");
    SectionHeader("MARKET");

    // regime + strategy
    int rj = s->current_regime;
    if (rj < 0 || rj >= NUM_REGIMES) rj = 0;
    ImVec4 regime_color = (rj == REGIME_TRENDING) ? FoxmlColors::green :
                          (rj == REGIME_MILD_TREND) ? FoxmlColors::sand :
                          (rj == REGIME_VOLATILE || rj == REGIME_TRENDING_DOWN) ? FoxmlColors::red : FoxmlColors::comment;

    if (s->sharded_mode_active && s->per_core_count > 0) {
        // v4.0.4: per-core strategy breakdown. The pre-sharded "headline
        // strategy" is meaningless when each core runs its own. Show:
        //   - regime headline (sourced from first AUTO core's hysteresis)
        //   - count of cores per resolved strategy as a mini bar
        // Each AUTO core has its own regime_state — this displays the
        // first one. Hover the regime to see the source.
        ImGui::TextColored(FoxmlColors::sand, "regime (core 0):");
        ImGui::SameLine();
        ImGui::TextColored(regime_color, "%s", REGIME_INFO[rj].full_name);
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "(%.0fm)", s->regime_duration_min);

        // count cores by resolved strategy (falls back to display id when
        // resolution hasn't run yet — ML/MR/MOM/DIP/EMA cores all hit the
        // resolved branch since their resolved_strategy_id == strategy_id).
        int strat_count[NUM_STRATEGIES] = {0};
        int unresolved = 0;
        for (int i = 0; i < s->per_core_count && i < 16; ++i) {
            uint8_t sid = s->per_core[i].resolved_strategy_id;
            if (sid >= NUM_STRATEGIES) sid = s->per_core[i].strategy_id_display;
            if (sid >= NUM_STRATEGIES) { unresolved++; continue; }
            strat_count[sid]++;
        }
        // strategy palette — same as Buy Gate / Chart so colors agree
        static const ImVec4 strat_colors[NUM_STRATEGIES] = {
            {0.85f, 0.65f, 0.35f, 0.9f},  // MR
            {0.85f, 0.45f, 0.45f, 0.9f},  // MOM
            {0.45f, 0.75f, 0.45f, 0.9f},  // DIP
            {0.65f, 0.45f, 0.80f, 0.9f},  // ML
            {0.35f, 0.75f, 0.80f, 0.9f},  // EMA
            {0.70f, 0.70f, 0.70f, 0.9f},  // AUTO (shouldn't appear post-resolve)
        };
        ImGui::TextColored(FoxmlColors::sand, "cores (%d):", s->per_core_count);
        for (int sid = 0; sid < NUM_STRATEGIES; ++sid) {
            if (strat_count[sid] == 0) continue;
            ImGui::SameLine(0, 8);
            ImGui::TextColored(strat_colors[sid], "%s:%d",
                STRATEGY_SHORT_NAMES[sid], strat_count[sid]);
        }
        if (unresolved > 0) {
            ImGui::SameLine(0, 8);
            ImGui::TextColored(FoxmlColors::comment, "?:%d", unresolved);
        }
    } else {
        // legacy single-engine view — kept for engine_mode=single_core.
        const char *strat_name = (s->strategy_id == 4) ? "EMA CROSS" :
                                  (s->strategy_id == 2) ? "SIMPLE DIP" :
                                  (s->strategy_id == 1) ? "MOMENTUM" : "MEAN REVERSION";
        ImGui::TextColored(FoxmlColors::sand, "regime:");
        ImGui::SameLine();
        ImGui::TextColored(regime_color, "%s", REGIME_INFO[rj].full_name);
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
    }

    // avg + stddev
    ImGui::TextColored(FoxmlColors::sand, "avg:");
    ImGui::SameLine();
    ImGui::Text("%.2f", s->roll_price_avg);
    ImGui::SameLine(0, 20);
    ImGui::TextColored(FoxmlColors::sand, "stddev:");
    ImGui::SameLine();
    ImGui::Text("%.2f", s->roll_stddev);

    // slopes
    const char *trend_arrow = (s->slope_pct > 0.001) ? "^" :
                              (s->slope_pct < -0.001) ? "v" : ">";
    ImVec4 trend_color = (s->slope_pct > 0.001) ? FoxmlColors::green :
                         (s->slope_pct < -0.001) ? FoxmlColors::red : FoxmlColors::comment;
    ImGui::TextColored(FoxmlColors::sand, "slope:");
    ImGui::SameLine();
    ImGui::TextColored(trend_color, "%+.6f%%/tick %s", s->slope_pct, trend_arrow);
    const char *lt_arrow = (s->long_slope_pct > 0.001) ? "^" :
                           (s->long_slope_pct < -0.001) ? "v" : ">";
    ImVec4 lt_color = (s->long_slope_pct > 0.001) ? FoxmlColors::green :
                      (s->long_slope_pct < -0.001) ? FoxmlColors::red : FoxmlColors::comment;
    ImGui::TextColored(FoxmlColors::sand, "long:");
    ImGui::SameLine();
    ImGui::TextColored(lt_color, "%+.6f%%/tick (%d-tick) %s", s->long_slope_pct, s->long_count, lt_arrow);

    // R² bars + signals
    GUI_R2Bar("short:", s->short_r2, 80.0f, s->slope_pct);
    ImGui::SameLine(0, 20);
    GUI_R2Bar("long:", s->long_r2, 80.0f, s->long_slope_pct);

    // vol ratio + ROR + spike
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

    // VWAP + book
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

    // FoxML integration status (Phase 6C)
    {
        int any_active = s->ml.cost_gate_enabled | s->ml.foxml_vol_scaling_enabled |
                         s->ml.confidence_enabled | s->ml.bandit_enabled;
        if (any_active) {
            ImGui::Separator();
            ImGui::TextColored(FoxmlColors::sand, "FoxML:");
            if (s->ml.cost_gate_enabled) {
                ImGui::SameLine(0, 10);
                ImGui::TextColored(FoxmlColors::sand, "cost:");
                ImGui::SameLine();
                ImGui::Text("%.1f bps", s->ml.cost_bps);
            }
            if (s->ml.foxml_vol_scaling_enabled) {
                ImGui::SameLine(0, 10);
                ImGui::TextColored(FoxmlColors::sand, "vsz:");
                ImGui::SameLine();
                ImGui::Text("%.0f%%", s->ml.foxml_vol_scale * 100.0);
            }
            if (s->ml.confidence_enabled) {
                ImGui::SameLine(0, 10);
                ImGui::TextColored(FoxmlColors::sand, "conf:");
                ImGui::SameLine();
                ImGui::Text("%.2f", s->ml.confidence);
            }
            if (s->ml.bandit_enabled) {
                ImGui::SameLine(0, 10);
                ImGui::TextColored(FoxmlColors::sand, "bandit:");
                ImGui::SameLine();
                ImVec4 bc = s->ml.bandit_active ? FoxmlColors::green : FoxmlColors::comment;
                ImGui::TextColored(bc, "%.0f%% %s",
                    s->ml.bandit_blend * 100.0, s->ml.bandit_active ? "ON" : "ramp");
            }
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

    // v4.0 sharded: per-core gate table. Each core has its own gate price,
    // colored by the core's strategy (matches the chart's gate-line colors).
    // The detailed view below shows core 0; this table shows all cores.
    if (s->sharded_mode_active && s->per_core_count > 0) {
        // strategy color palette — same indices as ChartPanel's strat_colors
        // so a row's color matches its line on the chart.
        static const ImVec4 strat_colors[NUM_STRATEGIES] = {
            {0.85f, 0.65f, 0.35f, 0.9f},  // MR — orange/sand
            {0.85f, 0.45f, 0.45f, 0.9f},  // MOM — red
            {0.45f, 0.75f, 0.45f, 0.9f},  // DIP — green
            {0.65f, 0.45f, 0.80f, 0.9f},  // ML — purple
            {0.35f, 0.75f, 0.80f, 0.9f},  // EMA — cyan
        };
        ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##bg_percore", 5, tf)) {
            ImGui::TableSetupColumn("Core",   ImGuiTableColumnFlags_WidthFixed, 35);
            ImGui::TableSetupColumn("Strat",  ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Gate",   ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Dist",   ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (int i = 0; i < s->per_core_count && i < 16; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", i);
                ImGui::TableNextColumn();
                uint8_t sid = s->per_core[i].strategy_id_display;
                ImVec4 col = (sid < NUM_STRATEGIES) ? strat_colors[sid]
                                                   : ImVec4(0.6f,0.6f,0.6f,0.9f);
                // v4.0.4: AUTO cores show "AUTO(DIP)" with the regime-resolved
                // concrete strategy in parens. Color follows the resolved strategy
                // so the chart's per-core gate line matches the row color.
                if (sid == STRATEGY_AUTO) {
                    uint8_t rs = s->per_core[i].resolved_strategy_id;
                    if (rs < NUM_STRATEGIES && rs != STRATEGY_AUTO) {
                        ImVec4 rcol = strat_colors[rs];
                        ImGui::TextColored(rcol, "AUTO(%s)", STRATEGY_SHORT_NAMES[rs]);
                    } else {
                        ImGui::TextColored(col, "AUTO");
                    }
                } else {
                    ImGui::TextColored(col, "%s",
                        sid < NUM_STRATEGIES ? STRATEGY_SHORT_NAMES[sid] : "?");
                }
                ImGui::TableNextColumn();
                double gate_p = s->per_core[i].buy_gate_price;
                if (gate_p > 0.01) {
                    ImGui::Text("$%.2f", gate_p);
                } else {
                    ImGui::TextColored(FoxmlColors::comment, "—");
                }
                ImGui::TableNextColumn();
                if (gate_p > 0.01 && s->price > 0.01) {
                    double dist_pct = ((s->price - gate_p) / s->price) * 100.0;
                    ImGui::Text("%+.3f%%", dist_pct);
                } else {
                    ImGui::TextColored(FoxmlColors::comment, "—");
                }
                ImGui::TableNextColumn();
                // Per-core direction (MOM = >=, everything else = <=).
                if (gate_p < 0.01) {
                    ImGui::TextColored(FoxmlColors::yellow, "off");
                } else if (s->positions[i].idx >= 0) {
                    ImGui::TextColored(FoxmlColors::comment, "in pos");
                } else {
                    int price_ok = s->per_core[i].gate_direction
                        ? (s->price >= gate_p)
                        : (s->price <= gate_p);
                    if (price_ok)
                        ImGui::TextColored(FoxmlColors::green_b, "READY");
                    else
                        ImGui::TextColored(FoxmlColors::yellow, "wait");
                }
            }
            ImGui::EndTable();
        }
        ImGui::Separator();

        // v4.0.4: per-core expandable details replacing the legacy single-core
        // lower block. Each core gets a collapsing header with its full state.
        // Halt reason names match the codes in EventLoop_RebuildAllParameters.
        static const char* halt_names[] = {
            "ok", "spacing", "vwap", "long-slope", "vol-delta",
            "min-stddev", "sl-cooldown", "warmup", "core-budget", "core-kill"
        };
        for (int i = 0; i < s->per_core_count && i < 16; ++i) {
            const TUISnapshot::PerCoreSnap *pc = &s->per_core[i];
            uint8_t sid = pc->strategy_id_display;
            const char *sname = (sid < NUM_STRATEGIES) ? STRATEGY_SHORT_NAMES[sid] : "?";
            char hdr[64];
            if (sid == STRATEGY_AUTO && pc->resolved_strategy_id < NUM_STRATEGIES) {
                snprintf(hdr, sizeof(hdr), "Core %d  AUTO(%s)##bgdetail", i,
                         STRATEGY_SHORT_NAMES[pc->resolved_strategy_id]);
            } else {
                snprintf(hdr, sizeof(hdr), "Core %d  %s##bgdetail", i, sname);
            }
            ImGui::PushID(i + 200);
            if (ImGui::CollapsingHeader(hdr)) {
                // Gate price
                ImGui::TextColored(FoxmlColors::sand, "  gate %s",
                    pc->gate_direction ? ">=" : "<=");
                ImGui::SameLine();
                if (pc->buy_gate_price > 0.01) {
                    ImGui::Text("$%.2f", pc->buy_gate_price);
                    if (s->price > 0.01) {
                        double dist = s->price - pc->buy_gate_price;
                        double dist_pct = (dist / s->price) * 100.0;
                        ImGui::SameLine(0, 10);
                        ImGui::TextColored(FoxmlColors::comment,
                            "(dist %+.3f%%)", dist_pct);
                    }
                } else {
                    ImGui::TextColored(FoxmlColors::comment, "off");
                }
                // Halt reason
                // halt_names array now goes up through index 9 (core-kill,
                // Phase 3). Bound: < (sizeof(halt_names)/sizeof(*halt_names)).
                if (pc->halt_reason > 0 && pc->halt_reason < 10) {
                    ImGui::SameLine(0, 15);
                    ImGui::TextColored(FoxmlColors::yellow,
                        "halted: %s", halt_names[pc->halt_reason]);
                }
                // Position state
                if (s->positions[i].idx >= 0) {
                    ImGui::SameLine(0, 15);
                    ImGui::TextColored(FoxmlColors::comment, "(in pos)");
                }
                // Cooldown
                if (pc->sl_cooldown_remaining > 0) {
                    ImGui::SameLine(0, 15);
                    ImGui::TextColored(FoxmlColors::yellow,
                        "cooldown %u", pc->sl_cooldown_remaining);
                }
                // ML extras
                if (pc->is_ml) {
                    ImGui::TextColored(FoxmlColors::sand, "  ML:");
                    ImGui::SameLine();
                    ImGui::TextColored(FoxmlColors::comment, "model:");
                    ImGui::SameLine();
                    ImGui::TextColored(pc->ml_model_loaded
                                       ? FoxmlColors::green : FoxmlColors::yellow,
                        "%s", pc->ml_model_loaded ? "loaded" : "none");
                    ImGui::SameLine(0, 10);
                    ImGui::TextColored(FoxmlColors::comment, "pred:");
                    ImGui::SameLine();
                    ImGui::Text("%.4f", pc->ml_last_prediction);
                    ImGui::SameLine(0, 10);
                    ImGui::TextColored(FoxmlColors::comment, "conf:");
                    ImGui::SameLine();
                    ImGui::Text("%.2f", pc->ml_last_confidence);
                }
            }
            ImGui::PopID();
        }
        ImGui::End();
        return;  // skip the legacy single-core block below in sharded mode
    }

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
        int gi = (s->gate_reason >= 0 && s->gate_reason < NUM_GATE_REASONS) ? s->gate_reason : 0;
        ImGui::TextColored(FoxmlColors::yellow, "GATE OFF (%s)", GATE_REASON_TABLE[gi].name);
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

    // fill rejection diagnostics — names from REJECT_REASON_NAMES (PortfolioController.hpp)
    if (s->fills_rejected > 0 && s->last_reject_reason > 0 &&
        s->last_reject_reason < NUM_REJECT_REASONS) {
        ImGui::TextColored(FoxmlColors::comment, "fills");
        ImGui::SameLine();
        ImGui::Text("%u/%u", s->total_buys, s->total_buys + s->fills_rejected);
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::comment, "last reject:");
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::yellow, "%s",
                           REJECT_REASON_NAMES[s->last_reject_reason]);
    }


    ImGui::End();
}

//==========================================================================
// PANEL: ACCOUNT (merged Portfolio + P&L + Risk)
//==========================================================================
static inline void GUI_Panel_Account(const TUISnapshot *s, TUISharedState *shared = NULL) {
    ImGui::Begin("Account");
    SectionHeader("ACCOUNT");

    ImGui::TextColored(FoxmlColors::sand, "equity:");
    ImGui::SameLine();
    ImGui::Text("$%.2f", s->equity);
    ImGui::SameLine(0, 20);
    ImGui::TextColored(FoxmlColors::sand, "balance:");
    ImGui::SameLine();
    ImGui::Text("$%.2f", s->balance);

    ImGui::TextColored(FoxmlColors::sand, "realized:");
    ImGui::SameLine();
    ImGui::TextColored(PnlColor(s->realized), "$%+.2f", s->realized);
    ImGui::SameLine(0, 20);
    ImGui::TextColored(FoxmlColors::sand, "unrealized:");
    ImGui::SameLine();
    ImGui::TextColored(PnlColor(s->unrealized), "$%+.2f", s->unrealized);
    ImGui::SameLine(0, 20);
    ImGui::TextColored(FoxmlColors::sand, "return:");
    ImGui::SameLine();
    ImGui::TextColored(PnlColor(s->return_pct), "%+.2f%%", s->return_pct);

    double gross = s->total_pnl + s->fees;
    ImGui::TextColored(FoxmlColors::sand, "gross:");
    ImGui::SameLine();
    ImGui::TextColored(PnlColor(gross), "$%+.2f", gross);
    ImGui::SameLine(0, 20);
    ImGui::TextColored(FoxmlColors::sand, "net:");
    ImGui::SameLine();
    ImGui::TextColored(PnlColor(s->total_pnl), "$%+.2f", s->total_pnl);
    ImGui::SameLine(0, 20);
    ImGui::TextColored(FoxmlColors::sand, "fees:");
    ImGui::SameLine();
    ImGui::Text("$%.2f", s->fees);

    // Phase 8 (post-coding) — maker/taker breakdown. Hover tooltip on the
    // fees line shows the split when there's at least one fill.
    if (ImGui::IsItemHovered()) {
        uint32_t total_fills = s->maker_fills_count + s->taker_fills_count;
        if (total_fills > 0) {
            double maker_pct = (double)s->maker_fills_count / total_fills * 100.0;
            ImGui::SetTooltip("Maker: %u fills (%.1f%%) — $%.4f\n"
                              "Taker: %u fills (%.1f%%) — $%.4f\n"
                              "Total: $%.4f",
                              s->maker_fills_count, maker_pct,         s->total_maker_fees,
                              s->taker_fills_count, 100.0 - maker_pct, s->total_taker_fees,
                              s->total_maker_fees + s->total_taker_fees);
        } else {
            ImGui::SetTooltip("No fills yet — maker/taker breakdown N/A");
        }
    }

    // Inline maker/taker line below fees, only shown when there are fills.
    {
        uint32_t total_fills = s->maker_fills_count + s->taker_fills_count;
        if (total_fills > 0) {
            double maker_pct = (double)s->maker_fills_count / total_fills * 100.0;
            ImGui::TextColored(FoxmlColors::sand, "M/T:");
            ImGui::SameLine();
            ImGui::Text("%u/%u", s->maker_fills_count, s->taker_fills_count);
            ImGui::SameLine();
            ImGui::TextDisabled("(%.0f%% maker)", maker_pct);
            ImGui::SameLine(0, 20);
            ImGui::TextColored(FoxmlColors::sand, "split:");
            ImGui::SameLine();
            ImGui::Text("$%.4f / $%.4f", s->total_maker_fees, s->total_taker_fees);
        }
    }

    ImGui::TextColored(FoxmlColors::sand, "exposure:");
    ImGui::SameLine();
    ImGui::Text("%.1f%%/%.0f%%", s->exposure_pct, s->max_exp);
    ImGui::SameLine(0, 20);
    ImGui::TextColored(FoxmlColors::sand, "risk/pos:");
    ImGui::SameLine();
    ImGui::Text("%.1f%%", s->risk_amt);
    ImGui::SameLine(0, 20);
    ImGui::TextColored(FoxmlColors::sand, "breaker:");
    ImGui::SameLine();
    if (s->breaker_tripped)
        ImGui::TextColored(FoxmlColors::red, "TRIPPED");
    else
        ImGui::TextColored(FoxmlColors::green, "OK");

    // v4.0.4: per-core P&L breakdown. The aggregate above is the OMS-wide
    // total; this table splits it back out by which core booked each exit.
    // Useful for spotting a single core eating its allocation while the
    // others are flat.
    if (s->sharded_mode_active && s->per_core_count > 0) {
        ImGui::Spacing();
        SectionHeader("PER-CORE P&L");
        // strategy palette — same as Market / Buy Gate / Chart
        static const ImVec4 strat_colors[NUM_STRATEGIES] = {
            {0.85f, 0.65f, 0.35f, 0.9f},  // MR
            {0.85f, 0.45f, 0.45f, 0.9f},  // MOM
            {0.45f, 0.75f, 0.45f, 0.9f},  // DIP
            {0.65f, 0.45f, 0.80f, 0.9f},  // ML
            {0.35f, 0.75f, 0.80f, 0.9f},  // EMA
            {0.70f, 0.70f, 0.70f, 0.9f},  // AUTO (shouldn't appear post-resolve)
        };
        ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("per_core_pnl", 7, tflags)) {
            ImGui::TableSetupColumn("Core", ImGuiTableColumnFlags_WidthFixed, 36.0f);
            ImGui::TableSetupColumn("Strat", ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableSetupColumn("Alloc",   ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Realized",ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Fees",    ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("W/L",     ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Budget",  ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < s->per_core_count && i < 16; ++i) {
                const TUISnapshot::PerCoreSnap *pc = &s->per_core[i];
                ImGui::TableNextRow();
                if (pc->core_kill_tripped) {
                    // 2A: highlight killed cores. Subtle red row tint so the
                    // panel doesn't look broken when a core trips, just
                    // visibly distinct. Hover any cell for the dd% reason.
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                        ImGui::GetColorU32(ImVec4(0.4f, 0.1f, 0.1f, 0.45f)));
                }
                ImGui::TableNextColumn();
                if (pc->core_kill_tripped) {
                    ImGui::TextColored(FoxmlColors::red, "%d!", i);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Core %d killed (dd %.2f%%) — entries halted "
                                          "until manual reset", i, pc->core_dd_pct * 100.0);
                    }
                } else {
                    ImGui::Text("%d", i);
                }

                ImGui::TableNextColumn();
                uint8_t sid = pc->resolved_strategy_id;
                if (sid >= NUM_STRATEGIES) sid = pc->strategy_id_display;
                if (sid < NUM_STRATEGIES) {
                    if (pc->core_kill_tripped) {
                        // Strikethrough effect via dimmed colored text
                        ImVec4 c = strat_colors[sid];
                        c.w = 0.45f;
                        ImGui::TextColored(c, "%s", STRATEGY_SHORT_NAMES[sid]);
                    } else {
                        ImGui::TextColored(strat_colors[sid], "%s", STRATEGY_SHORT_NAMES[sid]);
                    }
                } else {
                    ImGui::TextDisabled("?");
                }

                ImGui::TableNextColumn();
                ImGui::Text("$%.2f", pc->core_allocated);

                ImGui::TableNextColumn();
                ImGui::TextColored(PnlColor(pc->core_realized), "$%+.2f", pc->core_realized);

                ImGui::TableNextColumn();
                ImGui::Text("$%.4f", pc->core_fees);

                ImGui::TableNextColumn();
                uint32_t total = pc->core_wins + pc->core_losses;
                if (total > 0) {
                    double win_pct = (double)pc->core_wins / total * 100.0;
                    ImVec4 wlc = (win_pct >= 50.0) ? FoxmlColors::green : FoxmlColors::red;
                    ImGui::TextColored(wlc, "%u/%u", pc->core_wins, pc->core_losses);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%.1f%% win rate (%u trades)", win_pct, total);
                    }
                } else {
                    ImGui::TextDisabled("0/0");
                }

                ImGui::TableNextColumn();
                // Phase 2.1: "Budget" column shows how much of the core's
                // allocation is currently deployed. Color: green <50%,
                // yellow 50-90%, red >90% (>90% means new entries will
                // be clamped or rejected once Phase 2.2 enforcement lands).
                if (pc->core_open_positions == 0) {
                    ImGui::TextDisabled("0%%");
                } else {
                    double pct = pc->core_budget_used_pct;
                    ImVec4 col = (pct < 50.0)  ? FoxmlColors::green
                               : (pct < 90.0)  ? FoxmlColors::yellow
                                                : FoxmlColors::red;
                    ImGui::TextColored(col, "%.0f%%", pct);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Budget used: $%.2f / $%.2f (%u open position%s)",
                            pc->core_open_notional, pc->core_allocated,
                            pc->core_open_positions, pc->core_open_positions == 1 ? "" : "s");
                    }
                }
            }
            ImGui::EndTable();
        }
    }

    // Paper reset button — only shown when not live trading
    if (shared && !s->live_trading) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.3f, 0.3f, 0.7f));
        if (ImGui::Button("Reset Paper")) {
            shared->paper_reset_requested = 1;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Reset balance, positions, and counters\nto starting state (paper mode only)");
        }
    }

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

    if (ImGui::BeginTable("##positions", 11, flags)) {
        ImGui::TableSetupColumn("#",      ImGuiTableColumnFlags_WidthFixed, 25);
        ImGui::TableSetupColumn("Strat",  ImGuiTableColumnFlags_WidthFixed, 35);
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
            // Subtle row-bg tint per core so paired legs visually group.
            // Alternating cores stay default vs lightly tinted; both
            // legs of the same core share the tint. No-op under
            // partials-off (no leg-B rows exist).
            if (s->partial_exit_enabled && ((ps->idx >> 1) & 1)) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                    ImGui::GetColorU32(ImVec4(0.20f, 0.20f, 0.25f, 0.30f)));
            }

            // # column — under partials, slot 2c+leg belongs to core c.
            // Show "0.A" and "0.B" so the pair is obvious; under
            // partials-off, show plain core id.
            int row_core_id = s->partial_exit_enabled ? (ps->idx >> 1) : ps->idx;
            int row_leg     = s->partial_exit_enabled ? (ps->idx & 1)  : 0;
            ImGui::TableNextColumn();
            if (s->partial_exit_enabled) {
                ImGui::TextColored(FoxmlColors::wheat, "#%d.%c",
                                    row_core_id, row_leg == 0 ? 'A' : 'B');
            } else {
                ImGui::TextColored(FoxmlColors::wheat, "#%d", ps->idx);
            }

            // strategy (color-coded). Under partials, look up core_id =
            // slot/2 — both legs share the same per_core entry, so leg A
            // and leg B render with the same strategy color (correct —
            // they're one trade).
            ImGui::TableNextColumn();
            {
                static const ImVec4 sc[] = {
                    {0.40f, 0.60f, 0.85f, 1.0f},
                    {0.85f, 0.55f, 0.25f, 1.0f},
                    {0.45f, 0.75f, 0.45f, 1.0f},
                    {0.65f, 0.45f, 0.80f, 1.0f},
                    {0.35f, 0.75f, 0.80f, 1.0f},
                };
                uint8_t sid = (row_core_id < 16 && s->sharded_mode_active)
                    ? s->per_core[row_core_id].strategy_id_display : 0xFF;
                ImVec4 col = (sid < NUM_STRATEGIES) ? sc[sid] : FoxmlColors::comment;
                const char *name = (sid < NUM_STRATEGIES) ? STRATEGY_SHORT_NAMES[sid] : "?";
                // Dim leg B slightly so it visually nests under leg A
                if (row_leg == 1) col.w = 0.6f;
                ImGui::TextColored(col, "%s", name);
            }

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
        ImGui::TextColored(FoxmlColors::comment, "(%.2f%% / %.1f%%)", s->max_drawdown_pct, s->max_dd);
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
        ImGui::SameLine(0, 10);
        ImGui::TextColored(FoxmlColors::comment, "p99");
        ImGui::SameLine();
        ImGui::Text("%.0fns", s->hot_p99_ns);
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
//==========================================================================
// PANEL: ML INTELLIGENCE — bandit arms, confidence, cost, model info
//==========================================================================
static inline void GUI_Panel_MLIntelligence(const TUISnapshot *s) {
    int any_active = s->ml.cost_gate_enabled | s->ml.foxml_vol_scaling_enabled |
                     s->ml.confidence_enabled | s->ml.bandit_enabled |
                     s->ml.ml_model_loaded | s->ml.regime_model_loaded;
    if (!any_active) return; // no ML features enabled — skip entirely

    ImGui::Begin("ML Intelligence");

    // Bandit Arms
    if (s->ml.bandit_enabled && ImGui::CollapsingHeader("Bandit Arms", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextColored(FoxmlColors::sand, "Exp3-IX  |  %d steps  |  blend: %.0f%% %s",
            s->ml.bandit_total_steps, s->ml.bandit_blend * 100.0,
            s->ml.bandit_active ? "ON" : "ramp");
        if (ImGui::BeginTable("bandit_arms", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Arm");
            ImGui::TableSetupColumn("Pulls");
            ImGui::TableSetupColumn("Avg Reward");
            ImGui::TableSetupColumn("Weight");
            ImGui::TableSetupColumn("Prob");
            ImGui::TableHeadersRow();
            for (int i = 0; i < 5; i++) {
                if (s->ml.bandit_arm_names[i][0] == '\0') continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s", s->ml.bandit_arm_names[i]);
                ImGui::TableNextColumn(); ImGui::Text("%d", s->ml.bandit_pulls[i]);
                ImGui::TableNextColumn();
                ImVec4 rc = s->ml.bandit_avg_reward[i] >= 0 ? FoxmlColors::green : FoxmlColors::red;
                ImGui::TextColored(rc, "%+.1f bps", s->ml.bandit_avg_reward[i]);
                ImGui::TableNextColumn(); ImGui::Text("%.0f%%", s->ml.bandit_weights[i] * 100.0);
                ImGui::TableNextColumn(); ImGui::Text("%.0f%%", s->ml.bandit_probs[i] * 100.0);
            }
            ImGui::EndTable();
        }
    }

    // Confidence Breakdown
    if (s->ml.confidence_enabled && ImGui::CollapsingHeader("Confidence", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextColored(FoxmlColors::sand, "IC:");
        ImGui::SameLine();
        ImVec4 ic_c = (s->ml.confidence_ic > 0.1) ? FoxmlColors::green :
                      (s->ml.confidence_ic > 0.0) ? FoxmlColors::yellow : FoxmlColors::red;
        ImGui::TextColored(ic_c, "%.3f", s->ml.confidence_ic);
        ImGui::SameLine(0, 15);
        ImGui::TextColored(FoxmlColors::sand, "RMSE:");
        ImGui::SameLine();
        ImGui::Text("%.3f", s->ml.confidence_rmse);
        ImGui::SameLine(0, 15);
        ImGui::TextColored(FoxmlColors::sand, "Stability:");
        ImGui::SameLine();
        ImGui::Text("%.2f", s->ml.confidence_freshness);

        // confidence bar
        float conf_f = (float)s->ml.confidence;
        ImVec4 bar_c = conf_f > 0.5f ? FoxmlColors::green : conf_f > 0.3f ? FoxmlColors::yellow : FoxmlColors::red;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar_c);
        ImGui::ProgressBar(conf_f, ImVec2(-1, 0), "");
        ImGui::PopStyleColor();
        ImGui::SameLine(0);
        ImGui::Text("Combined: %.2f", s->ml.confidence);
    }

    // Cost Decomposition
    if (s->ml.cost_gate_enabled && ImGui::CollapsingHeader("Cost Model", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextColored(FoxmlColors::sand, "Spread:"); ImGui::SameLine();
        ImGui::Text("%.1f bps", s->ml.cost_spread);
        ImGui::SameLine(0, 15);
        ImGui::TextColored(FoxmlColors::sand, "Timing:"); ImGui::SameLine();
        ImGui::Text("%.1f bps", s->ml.cost_timing);
        ImGui::SameLine(0, 15);
        ImGui::TextColored(FoxmlColors::sand, "Impact:"); ImGui::SameLine();
        ImGui::Text("%.1f bps", s->ml.cost_impact);

        ImGui::TextColored(FoxmlColors::sand, "Total:"); ImGui::SameLine();
        ImVec4 tc = s->ml.cost_bps > 10.0 ? FoxmlColors::red : FoxmlColors::text;
        ImGui::TextColored(tc, "%.1f bps", s->ml.cost_bps);
        ImGui::SameLine(0, 15);
        ImGui::TextColored(FoxmlColors::sand, "Breakeven:"); ImGui::SameLine();
        ImGui::Text("%.1f bps", s->ml.cost_breakeven * 10000.0);
    }

    // Model Info
    if ((s->ml.ml_model_loaded || s->ml.regime_model_loaded) &&
        ImGui::CollapsingHeader("Models")) {
        if (s->ml.ml_model_loaded) {
            ImGui::TextColored(FoxmlColors::green, "Buy Signal:"); ImGui::SameLine();
            ImGui::TextWrapped("%s", s->ml.ml_model_path);
            ImGui::TextColored(FoxmlColors::sand, "Last prediction:"); ImGui::SameLine();
            ImGui::Text("%.4f", s->ml.ml_last_prediction);
        }
        if (s->ml.regime_model_loaded) {
            ImGui::TextColored(FoxmlColors::green, "Regime:"); ImGui::SameLine();
            ImGui::TextWrapped("%s", s->ml.regime_model_path);
        }
    }

    // Per-Core ML — sharded mode only. Phase 6prep sharded c16. Shows the
    // per-core breakdown that the headline summary collapses. Each ML core
    // gets a row with prediction / confidence / IC / RMSE.
    if (s->sharded_mode_active) {
        int any_ml_core = 0;
        for (int i = 0; i < s->per_core_count && i < 16; ++i) {
            if (s->per_core[i].is_ml) { any_ml_core = 1; break; }
        }
        if (any_ml_core && ImGui::CollapsingHeader("Per-Core ML",
                            ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("##percore_ml", 6, tf)) {
                ImGui::TableSetupColumn("Core",     ImGuiTableColumnFlags_WidthFixed, 35);
                ImGui::TableSetupColumn("Model",    ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("Pred",     ImGuiTableColumnFlags_WidthFixed, 65);
                ImGui::TableSetupColumn("Conf",     ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableSetupColumn("IC",       ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("RMSE",     ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableHeadersRow();
                for (int i = 0; i < s->per_core_count && i < 16; ++i) {
                    if (!s->per_core[i].is_ml) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", i);
                    ImGui::TableNextColumn();
                    if (s->per_core[i].ml_model_loaded) {
                        ImGui::TextColored(FoxmlColors::green, "loaded");
                    } else {
                        ImGui::TextColored(FoxmlColors::comment, "none");
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%.4f", s->per_core[i].ml_last_prediction);
                    ImGui::TableNextColumn();
                    {
                        float cf = (float)s->per_core[i].ml_last_confidence;
                        if (cf < 0.0f) cf = 0.0f; if (cf > 1.0f) cf = 1.0f;
                        ImVec4 bc = cf > 0.5f ? FoxmlColors::green
                                  : cf > 0.3f ? FoxmlColors::yellow
                                  : FoxmlColors::red;
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bc);
                        char overlay[32];
                        snprintf(overlay, sizeof(overlay), "%.2f", cf);
                        ImGui::ProgressBar(cf, ImVec2(-1, 0), overlay);
                        ImGui::PopStyleColor();
                    }
                    ImGui::TableNextColumn();
                    {
                        double ic = s->per_core[i].ml_confidence_ic;
                        ImVec4 cc = ic > 0.1 ? FoxmlColors::green
                                  : ic > 0.0 ? FoxmlColors::yellow
                                  : FoxmlColors::red;
                        ImGui::TextColored(cc, "%+.3f", ic);
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", s->per_core[i].ml_confidence_rmse);
                }
                ImGui::EndTable();
            }
            // Footnote: explain what the headline `Confidence` block shows
            // when the per-core view is open (avoids confusion with the
            // top-of-panel single-core summary).
            ImGui::TextColored(FoxmlColors::comment,
                "(headline above shows highest-conf core; "
                "noise-floor IC clamps to 0.01)");
        }
    }

    ImGui::End();
}

static inline void GUI_RenderDashboard(const TUISnapshot *s, uint64_t start_time,
                                        TUISharedState *shared = NULL) {
    GUI_Panel_Header(s, start_time);
    GUI_Panel_TopBar(s);
    GUI_Panel_Market(s);
    GUI_Panel_BuyGate(s);
    GUI_Panel_Account(s, shared);
    GUI_Panel_Positions(s);
    GUI_Panel_Stats(s);
    GUI_Panel_MLIntelligence(s);
#ifdef LATENCY_PROFILING
    GUI_Panel_Latency(s);
#endif
    // v4.0.4: standalone "Per-Core Strategy" panel removed — its hot-swap
    // dropdown + Apply button moved into Settings → Core N → "Core
    // Configuration" section, so all per-core knobs (strategy, risk %,
    // model path, model dir, plus all overrides) live under one tab.

    // Phase 3.5 — RISK PANEL. Per-core kill switch dashboard with reset
    // controls. Account panel stays read-only / monitoring; this panel
    // is for taking action when a core gets in trouble. Future home for
    // manual halt, force-close, drawdown override slider, etc.
    if (s->sharded_mode_active && s->per_core_count > 0 && shared) {
        ImGui::Begin("Risk");
        SectionHeader("PER-CORE RISK");
        ImGui::TextColored(FoxmlColors::comment,
            "(kill switch tracks peak-to-trough drawdown including unrealized; "
            "trip blocks future entries — open positions ride to TP/SL)");

        ImGuiTableFlags rt = ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("risk_per_core", 8, rt)) {
            ImGui::TableSetupColumn("Core",   ImGuiTableColumnFlags_WidthFixed, 36);
            ImGui::TableSetupColumn("Strat",  ImGuiTableColumnFlags_WidthFixed, 56);
            ImGui::TableSetupColumn("Peak",   ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Curr",   ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("DD%%",   ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Trips",  ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableHeadersRow();

            static const ImVec4 strat_colors[NUM_STRATEGIES] = {
                {0.85f, 0.65f, 0.35f, 0.9f},  // MR
                {0.85f, 0.45f, 0.45f, 0.9f},  // MOM
                {0.45f, 0.75f, 0.45f, 0.9f},  // DIP
                {0.65f, 0.45f, 0.80f, 0.9f},  // ML
                {0.35f, 0.75f, 0.80f, 0.9f},  // EMA
                {0.70f, 0.70f, 0.70f, 0.9f},  // AUTO
            };

            for (int i = 0; i < s->per_core_count && i < 16; ++i) {
                const TUISnapshot::PerCoreSnap *pc = &s->per_core[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::Text("%d", i);

                ImGui::TableNextColumn();
                uint8_t sid = pc->resolved_strategy_id;
                if (sid >= NUM_STRATEGIES) sid = pc->strategy_id_display;
                if (sid < NUM_STRATEGIES) {
                    ImGui::TextColored(strat_colors[sid], "%s", STRATEGY_SHORT_NAMES[sid]);
                } else {
                    ImGui::TextDisabled("?");
                }

                ImGui::TableNextColumn();
                ImGui::Text("$%.2f", pc->core_peak_balance);

                // Current value = allocated + realized (+ unrealized when MTM
                // is on, but we don't surface unrealized separately yet —
                // the dd% derives from the current that the engine saw).
                // Reconstruct approximately: peak * (1 - dd) gives current
                // at the time peak was last evaluated.
                double approx_current = pc->core_peak_balance * (1.0 - pc->core_dd_pct);
                ImGui::TableNextColumn();
                // Color Curr by direction vs allocation: green when above
                // (core in profit overall), red when below (in loss),
                // default neutral when flat. Same threshold semantics as
                // the Realized column in PER-CORE P&L.
                ImVec4 curr_col = FoxmlColors::text;
                if (approx_current > pc->core_allocated + 0.005) {
                    curr_col = FoxmlColors::green;
                } else if (approx_current < pc->core_allocated - 0.005) {
                    curr_col = FoxmlColors::red;
                }
                ImGui::TextColored(curr_col, "$%.2f", approx_current);

                // DD%, color-coded
                ImGui::TableNextColumn();
                double dd = pc->core_dd_pct * 100.0;
                ImVec4 dd_col = (dd < 5.0)  ? FoxmlColors::green
                              : (dd < 10.0) ? FoxmlColors::yellow
                                            : FoxmlColors::red;
                ImGui::TextColored(dd_col, "%.1f%%", dd);

                ImGui::TableNextColumn();
                if (pc->core_ks_trips_total > 0) {
                    ImGui::TextColored(FoxmlColors::yellow, "%u", pc->core_ks_trips_total);
                } else {
                    ImGui::TextDisabled("0");
                }

                ImGui::TableNextColumn();
                if (pc->core_kill_tripped) {
                    ImGui::TextColored(FoxmlColors::red_b, "KILLED");
                } else {
                    ImGui::TextColored(FoxmlColors::green, "armed");
                }

                ImGui::TableNextColumn();
                if (pc->core_kill_tripped) {
                    if (ImGui::Button("Reset")) {
                        // Per-core reset signal — engine slow path picks this
                        // up, clears trip flag, refreshes peak to current.
                        shared->kill_reset_per_core[i] = 1;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Clear kill trip + refresh peak watermark.\n"
                                          "Core %d will resume trading on next slow-path cycle.", i);
                    }
                } else {
                    ImGui::BeginDisabled();
                    ImGui::Button("Reset");
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    // Per-core latency panel (sharded mode only)
    if (s->sharded_mode_active && s->per_core_count > 0) {
        ImGui::Begin("Per-Core Latency");
        SectionHeader("PER-CORE LATENCY");
        ImGui::TextColored(FoxmlColors::comment, "(last 256 samples, subtract ~25-30ns rdtsc floor)");

        ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##percore", 8, tf)) {
            ImGui::TableSetupColumn("Core",    ImGuiTableColumnFlags_WidthFixed, 35);
            ImGui::TableSetupColumn("Strat",   ImGuiTableColumnFlags_WidthFixed, 35);
            ImGui::TableSetupColumn("Samples", ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Min",     ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("p50",     ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("p95",     ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("p99",     ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Max",     ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableHeadersRow();

            for (int i = 0; i < s->per_core_count && i < 16; ++i) {
                const TUISnapshot::PerCoreSnap *pc = &s->per_core[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", i);
                ImGui::TableNextColumn();
                uint8_t sid = pc->strategy_id_display;
                ImGui::TextColored(FoxmlColors::primary, "%s",
                                   sid < NUM_STRATEGIES ? STRATEGY_SHORT_NAMES[sid] : "?");
                ImGui::TableNextColumn();
                if (pc->samples == 0) {
                    ImGui::TextColored(FoxmlColors::comment, "-");
                    for (int j = 0; j < 5; ++j) { ImGui::TableNextColumn(); ImGui::TextColored(FoxmlColors::comment, "-"); }
                } else {
                    ImGui::Text("%lu", (unsigned long)pc->samples);
                    ImGui::TableNextColumn(); ImGui::Text("%.0f", pc->min_ns);
                    ImGui::TableNextColumn(); ImGui::Text("%.0f", pc->p50_ns);
                    ImGui::TableNextColumn(); ImGui::Text("%.0f", pc->p95_ns);
                    ImGui::TableNextColumn(); ImGui::Text("%.0f", pc->p99_ns);
                    ImGui::TableNextColumn(); ImGui::Text("%.0f", pc->max_ns);
                }
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }
}
