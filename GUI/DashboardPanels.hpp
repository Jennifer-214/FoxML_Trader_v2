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
#include <chrono>  // v5.0.3: Engine Topology drift display

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
            {0.40f, 0.60f, 0.85f, 1.0f},  // 0 MR  blue
            {0.85f, 0.55f, 0.25f, 1.0f},  // 1 MOM orange
            {0.45f, 0.75f, 0.45f, 1.0f},  // 2 DIP green
            {0.65f, 0.45f, 0.80f, 1.0f},  // 3 ML  purple
            {0.35f, 0.75f, 0.80f, 1.0f},  // 4 EMA cyan
            {0.90f, 0.80f, 0.50f, 1.0f},  // 5 AUTO gold (was missing — rendered as
                                           //   out-of-bounds garbage color, making
                                           //   the C{n}:AUTO label invisible in the
                                           //   CORES header line)
        };
        // sc[] must stay aligned with STRATEGY_* enum (size = NUM_STRATEGIES).
        static_assert(sizeof(sc)/sizeof(sc[0]) == NUM_STRATEGIES,
                      "sc[] color array out of sync with NUM_STRATEGIES");
        ImGui::TextColored(FoxmlColors::sand, "CORES:");
        for (int i = 0; i < s->per_core_count && i < 16; ++i) {
            ImGui::SameLine();
            uint8_t sid = s->per_core[i].strategy_id_display;
            // For AUTO, append the resolved strategy in parens so the header
            // matches the Buy Gate panel's "AUTO(MR)" / "AUTO(DIP)" labels.
            ImVec4 col = (sid < NUM_STRATEGIES) ? sc[sid] : FoxmlColors::comment;
            const char *name = (sid < NUM_STRATEGIES) ? STRATEGY_SHORT_NAMES[sid]
                               : (sid == 0xFF ? "OFF" : "?");
            if (sid == STRATEGY_AUTO) {
                uint8_t rsid = s->per_core[i].resolved_strategy_id;
                const char *rname = (rsid < NUM_STRATEGIES && rsid != STRATEGY_AUTO)
                                    ? STRATEGY_SHORT_NAMES[rsid] : "?";
                ImGui::TextColored(col, "C%d:%s(%s)", i, name, rname);
            } else {
                ImGui::TextColored(col, "C%d:%s", i, name);
            }
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
        // first one. v4.7.35: dropped misleading "core 0" suffix; the
        // value is actually the first-AUTO-core's regime classification
        // (or core 0 fallback if no AUTO core configured).
        ImGui::TextColored(FoxmlColors::sand, "regime:");
        ImGui::SameLine();
        ImGui::TextColored(regime_color, "%s", REGIME_INFO[rj].full_name);
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "(%.0fm)", s->regime_duration_min);

        // count cores by resolved strategy (falls back to display id when
        // resolution hasn't run yet — ML/MR/MOM/DIP/EMA cores all hit the
        // resolved branch since their resolved_strategy_id == strategy_id).
        // v4.7.35: also count AUTO-configured cores separately so the
        // breakdown distinguishes "1 native MR + 1 AUTO routing to MR"
        // from "2 explicitly-MR cores" (both show MR:2 in resolved view).
        int strat_count[NUM_STRATEGIES] = {0};
        int auto_count = 0;
        int unresolved = 0;
        for (int i = 0; i < s->per_core_count && i < 16; ++i) {
            if (s->per_core[i].strategy_id_display == STRATEGY_AUTO) auto_count++;
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
        // v4.7.35: AUTO count appended last so user can see how many
        // of the resolved counts above came from AUTO routing vs
        // explicit strategy assignment.
        if (auto_count > 0) {
            ImGui::SameLine(0, 8);
            ImGui::TextColored(FoxmlColors::sand, "(AUTO:%d)", auto_count);
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
            {0.85f, 0.65f, 0.35f, 0.9f},  // 0 MR — orange/sand
            {0.85f, 0.45f, 0.45f, 0.9f},  // 1 MOM — red
            {0.45f, 0.75f, 0.45f, 0.9f},  // 2 DIP — green
            {0.65f, 0.45f, 0.80f, 0.9f},  // 3 ML — purple
            {0.35f, 0.75f, 0.80f, 0.9f},  // 4 EMA — cyan
            {0.90f, 0.80f, 0.50f, 0.9f},  // 5 AUTO — gold (was missing — array
                                           //   defaulted strat_colors[5] to zero
                                           //   = transparent black, making any
                                           //   AUTO core's row invisible)
        };
        // v5.6.0: halt_names + the BUY_BLOCKED flag bit are needed by both
        // the top-table Status column AND the collapsing-header detail block,
        // so declare here at panel scope rather than inside the second loop.
        // Codes 0..10 must stay in sync with EngineTUI.hpp's halt_reason
        // comment + ControllerEventLoop.hpp:1812-1814.
        static const char* halt_names[] = {
            "ok", "spacing", "vwap", "long-slope", "vol-delta",
            "min-stddev", "sl-cooldown", "warmup", "core-budget", "core-kill",
            "imbalance"
        };
        constexpr int halt_names_count =
            (int)(sizeof(halt_names) / sizeof(halt_names[0]));
        // v5.6.2: SHALT_* names for strategy-internal halt reasons. Mirror
        // of SHALT_SHORT_NAMES in StrategyInterface.hpp — keep in sync.
        static const char* shalt_names[] = {
            "ok",            // SHALT_OK = 0
            "no-uptrend",    // SHALT_NO_UPTREND = 1
            "no-mean-rev",   // SHALT_NO_MEAN_REV = 2
            "fee-floor",     // SHALT_FEE_FLOOR = 3
            "cost-gate",     // SHALT_COST_GATE = 4
            "stddev-zero",   // SHALT_STDDEV_ZERO = 5
            "no-breakout",   // SHALT_NO_BREAKOUT = 6
            "ml-no-pred",    // SHALT_ML_NO_PRED = 7
            "ml-below-thr",  // SHALT_ML_BELOW_THR = 8
            "low-confidence",// SHALT_LOW_CONFIDENCE = 9
            "no-signal",     // SHALT_NO_SIGNAL = 10
        };
        constexpr int shalt_names_count =
            (int)(sizeof(shalt_names) / sizeof(shalt_names[0]));
        constexpr uint8_t GUI_GATE_FLAG_BUY_BLOCKED = 0x20;  // mirrors
            // GateParameters.hpp:61. Display-side mirror keeps the GUI
            // module from needing CoreFrameworks/ headers; checked by
            // EXECUTION_DISPLAY_INVARIANTS.md test for value parity.
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
                // v5.6.0: Status column priority chain — every hot-path
                // BG_Evaluate term gets a surface here. Order matters
                // (most-informative wins). See EXECUTION_DISPLAY_INVARIANTS.md.
                //
                // Hot path: bg_fires = price_ok & volume_check & ~blocked
                //           can_enter = ~any_active & permission & bg_fires
                //
                // Display priority:
                //   1. in pos          (any_active = 1; positions snapshot)
                //   2. blocked         (BUY_BLOCKED flag set, hot path refuses)
                //   3. off: <halt>     (gate zeroed by controller, halt code set)
                //   4. off: no signal  (gate zeroed by strategy, no halt code —
                //                       v5.6.2 will replace with strategy_halt_reason)
                //   5. wait            (gate live, price hasn't crossed)
                //   6. READY           (price crossed, no other blocker)
                const auto *pc = &s->per_core[i];
                // v5.6.1: priority chain extended for permission + bitmap
                // drift. Permission=0 means the hot path will refuse
                // entries regardless of price/flags (kill switch trip,
                // startup gate). Bitmap drift means GUI and hot path
                // disagree on any_active — flagged for diagnosis.
                if (!pc->bitmap_consistency) {
                    ImGui::TextColored(FoxmlColors::red,
                        "DRIFT (bitmap)");
                } else if (s->positions[i].idx >= 0) {
                    ImGui::TextColored(FoxmlColors::comment, "in pos");
                } else if (pc->permission == 0) {
                    ImGui::TextColored(FoxmlColors::yellow, "PERM_OFF");
                } else if (pc->gate_flags & GUI_GATE_FLAG_BUY_BLOCKED) {
                    // v5.6.2: prefer the specific SHALT code (fee-floor /
                    // cost-gate) over generic "blocked" when set.
                    if (pc->strategy_halt_reason > 0 &&
                        pc->strategy_halt_reason < shalt_names_count) {
                        ImGui::TextColored(FoxmlColors::yellow,
                            "blocked: %s",
                            shalt_names[pc->strategy_halt_reason]);
                    } else {
                        ImGui::TextColored(FoxmlColors::yellow, "blocked");
                    }
                } else if (pc->halt_reason > 0 &&
                           pc->halt_reason < halt_names_count) {
                    ImGui::TextColored(FoxmlColors::yellow,
                        "off: %s", halt_names[pc->halt_reason]);
                } else if (pc->strategy_halt_reason > 0 &&
                           pc->strategy_halt_reason < shalt_names_count) {
                    // v5.6.2: strategy zero-gated for a strategy-internal
                    // reason that didn't go through BUY_BLOCKED. Today
                    // only SHALT_NO_SIGNAL lands here; future per-strategy
                    // codes (NO_UPTREND, NO_MEAN_REV, etc) will too.
                    ImGui::TextColored(FoxmlColors::yellow,
                        "off: %s", shalt_names[pc->strategy_halt_reason]);
                } else if (gate_p < 0.01) {
                    // Catch-all when neither halt_reason nor strategy_halt_reason
                    // is set but threshold is zero — should not happen post-v5.6.2
                    // (SHALT_NO_SIGNAL is the post-pass fallback). If you see this,
                    // a code path is producing a zero gate without setting any
                    // reason — treat as drift.
                    ImGui::TextColored(FoxmlColors::yellow, "off: ???");
                } else {
                    int price_ok = pc->gate_direction
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
        // halt_names + halt_names_count declared at panel scope above so the
        // top-table Status column also has access. v5.6.0 — added "imbalance"
        // at index 10.
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
                // v5.6.0: bound matches halt_names_count so adding new codes
                // doesn't silently drop them (was hardcoded `< 10` before,
                // hiding halt_reason=10=imbalance entirely).
                if (pc->halt_reason > 0 && pc->halt_reason < halt_names_count) {
                    ImGui::SameLine(0, 15);
                    ImGui::TextColored(FoxmlColors::yellow,
                        "halted: %s", halt_names[pc->halt_reason]);
                }
                // v5.6.1/2: BUY_BLOCKED flag readout. Independent of halt_reason —
                // strategy-level fee-floor + cost-gate set BUY_BLOCKED. When a
                // SHALT code is also set, show the specific reason
                // (fee-floor / cost-gate / etc); otherwise show the bare flag.
                if (pc->gate_flags & GUI_GATE_FLAG_BUY_BLOCKED) {
                    ImGui::SameLine(0, 15);
                    if (pc->strategy_halt_reason > 0 &&
                        pc->strategy_halt_reason < shalt_names_count) {
                        ImGui::TextColored(FoxmlColors::yellow,
                            "BLOCKED: %s",
                            shalt_names[pc->strategy_halt_reason]);
                    } else {
                        ImGui::TextColored(FoxmlColors::yellow, "BUY_BLOCKED");
                    }
                } else if (pc->strategy_halt_reason > 0 &&
                           pc->strategy_halt_reason < shalt_names_count) {
                    // SHALT set but no BUY_BLOCKED → strategy-zero-gated.
                    ImGui::SameLine(0, 15);
                    ImGui::TextColored(FoxmlColors::yellow,
                        "shalt: %s",
                        shalt_names[pc->strategy_halt_reason]);
                }
                // v5.6.1: permission atomic. 0 = entries forbidden by
                // controller (kill switch / startup gate). The Risk panel
                // already shows kill-switch state, but this surface ties
                // the visibility to the same row as the gate state.
                if (pc->permission == 0) {
                    ImGui::SameLine(0, 15);
                    ImGui::TextColored(FoxmlColors::red, "PERM_OFF");
                }
                // v5.6.1: bitmap drift — Class 2c regression detector. The
                // hot path's any_active mask and the GUI's positions
                // bitmap should always agree. If they don't, the engine
                // and the operator are looking at different states.
                if (!pc->bitmap_consistency) {
                    ImGui::SameLine(0, 15);
                    ImGui::TextColored(FoxmlColors::red, "DRIFT(bitmap)");
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
                // v5.6.1: volume gate — only meaningful when
                // GATE_FLAG_VOLUME_REQUIRED is set in gate_flags. Today no
                // strategy sets this flag, but the surface needs to exist
                // before any future strategy enables it (otherwise that
                // strategy's volume gate would be silent).
                constexpr uint8_t GUI_GATE_FLAG_VOLUME_REQUIRED = 0x08;
                if (pc->gate_flags & GUI_GATE_FLAG_VOLUME_REQUIRED) {
                    ImGui::TextColored(FoxmlColors::sand,
                        "  vol thr: %.4f", pc->bg_volume_threshold);
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
                // v4.7.34: AUTO cores show "AUTO(routed)" so user can tell
                // at a glance whether a core is regime-routing or fixed.
                // Color follows the resolved strategy so per-core panels +
                // chart per-core gate lines stay in sync visually.
                uint8_t cfg_sid = pc->strategy_id_display;
                uint8_t live_sid = pc->resolved_strategy_id;
                if (live_sid >= NUM_STRATEGIES) live_sid = cfg_sid;
                if (cfg_sid == STRATEGY_AUTO &&
                    live_sid < NUM_STRATEGIES && live_sid != STRATEGY_AUTO) {
                    ImVec4 c = strat_colors[live_sid];
                    if (pc->core_kill_tripped) c.w = 0.45f;
                    ImGui::TextColored(c, "AUTO(%s)", STRATEGY_SHORT_NAMES[live_sid]);
                } else if (live_sid < NUM_STRATEGIES) {
                    ImVec4 c = strat_colors[live_sid];
                    if (pc->core_kill_tripped) c.w = 0.45f;
                    ImGui::TextColored(c, "%s", STRATEGY_SHORT_NAMES[live_sid]);
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
static inline void GUI_Panel_Positions(const TUISnapshot *s, TUISharedState *shared = NULL) {
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

    // v4.7.8: 12th column for Action (Close button) when shared state is
    // wired. Falls back to 11 columns when shared==NULL (legacy callers).
    int n_cols = (shared != NULL) ? 12 : 11;
    if (ImGui::BeginTable("##positions", n_cols, flags)) {
        ImGui::TableSetupColumn("#",      ImGuiTableColumnFlags_WidthFixed, 40);
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
        if (shared) {
            ImGui::TableSetupColumn("Act", ImGuiTableColumnFlags_WidthFixed, 50);
        }
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

            // v4.7.8: manual close button. Sets the per-slot close request
            // flag — drainer reads, emits a synthetic SELL via OMS, clears
            // the flag. Race-tolerant: HandleFill dedups via slot bitmap
            // if SL fires the same window.
            if (shared) {
                ImGui::TableNextColumn();
                ImGui::PushID(ps->idx);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.25f, 0.25f, 0.7f));
                if (ImGui::SmallButton("Close")) {
                    shared->manual_close_requested[ps->idx] = 1;
                }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Force-close slot %d at next market tick.\n"
                                      "Bypasses TP/SL gates. Bound for fill at current price.",
                                      ps->idx);
                }
                ImGui::PopID();
            }

            displayed++;
        }

        ImGui::EndTable();
    }

    // v4.7.9: text-entry TP/SL realignment. Single-row form below the
    // table — pick slot, type new price, click Apply TP or Apply SL.
    // Reuses the existing drag pickup mechanism (drag_slot/drag_is_tp/
    // drag_price) so no new pipeline. Same atomic write pattern as the
    // chart drag handler. Only rendered when shared state is wired
    // (otherwise we have no way to apply the value).
    if (shared) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(FoxmlColors::comment, "Realign TP/SL:");
        ImGui::SameLine();

        // Build a list of currently-open slot indices for the combo
        static int picked_slot = -1;
        char combo_label[32];
        if (picked_slot >= 0 && picked_slot < 16 && s->positions[picked_slot].idx >= 0) {
            int leg = s->partial_exit_enabled ? (picked_slot & 1) : 0;
            int core = s->partial_exit_enabled ? (picked_slot >> 1) : picked_slot;
            snprintf(combo_label, sizeof(combo_label),
                     s->partial_exit_enabled ? "#%d.%c" : "#%d",
                     core, leg == 0 ? 'A' : 'B');
        } else {
            picked_slot = -1;
            snprintf(combo_label, sizeof(combo_label), "Slot");
        }
        ImGui::SetNextItemWidth(70);
        if (ImGui::BeginCombo("##realign_slot", combo_label)) {
            for (int i = 0; i < 16; i++) {
                if (s->positions[i].idx < 0) continue;
                int leg  = s->partial_exit_enabled ? (i & 1) : 0;
                int core = s->partial_exit_enabled ? (i >> 1) : i;
                char item[32];
                snprintf(item, sizeof(item),
                         s->partial_exit_enabled ? "#%d.%c" : "#%d",
                         core, leg == 0 ? 'A' : 'B');
                if (ImGui::Selectable(item, picked_slot == i)) picked_slot = i;
            }
            ImGui::EndCombo();
        }

        // Pre-populate inputs with current TP/SL when a slot is selected
        static double new_tp = 0.0, new_sl = 0.0;
        static int last_picked = -2;
        if (picked_slot != last_picked && picked_slot >= 0) {
            new_tp = s->positions[picked_slot].tp;
            new_sl = s->positions[picked_slot].sl;
            last_picked = picked_slot;
        }

        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::green, "TP");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputDouble("##realign_tp", &new_tp, 0.0, 0.0, "%.2f");
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::red, "SL");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputDouble("##realign_sl", &new_sl, 0.0, 0.0, "%.2f");

        ImGui::SameLine();
        bool can_apply = (picked_slot >= 0 && picked_slot < 16 &&
                           s->positions[picked_slot].idx >= 0);
        if (!can_apply) ImGui::BeginDisabled();
        if (ImGui::SmallButton("Apply TP")) {
            __atomic_store_n(&shared->drag_is_tp, 1, __ATOMIC_RELEASE);
            shared->drag_price = new_tp;
            __atomic_store_n(&shared->drag_slot, picked_slot, __ATOMIC_RELEASE);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply SL")) {
            __atomic_store_n(&shared->drag_is_tp, 0, __ATOMIC_RELEASE);
            shared->drag_price = new_sl;
            __atomic_store_n(&shared->drag_slot, picked_slot, __ATOMIC_RELEASE);
        }
        if (!can_apply) ImGui::EndDisabled();

        if (ImGui::IsItemHovered() && !can_apply) {
            ImGui::SetTooltip("Pick a slot first");
        }
    }

    ImGui::End();
}

//==========================================================================
// PANEL: PER-CORE P&L HISTORY (v4.7.11)
//==========================================================================
// Overlays each core's realized P&L over the session as 4 lines on the
// same chart. Useful for spotting which core is adding alpha vs which
// is bleeding fees. Samples once per second (or on every snapshot tick
// if slower). Ring buffer of last 1800 samples = 30 min @ 1Hz; older
// samples scroll off the left.
//
// Static state — single-instance panel, OK to keep in function-static.
// Pure GUI thread, doesn't touch engine state.
//==========================================================================
static inline void GUI_Panel_PerCorePnL(const TUISnapshot *s) {
    static constexpr int PNL_HISTORY = 1800;
    static double pnl_history[PNL_HISTORY][16];  // [time_idx][core]
    static double time_history[PNL_HISTORY];
    static int      history_count    = 0;
    static int      history_head     = 0;
    static double   last_sample_t    = 0.0;
    static double   session_t0       = 0.0;
    static uint32_t last_seen_reset_seq = 0;

    double now_s = (double)time(NULL);
    if (session_t0 == 0.0) session_t0 = now_s;

    // v4.7.18: detect paper reset and clear the ring. Pre-v4.7.18 the
    // panel kept 30min of pre-reset samples, mixing stale -P&L with new
    // zeros and making the Y axis autofit to a misleading range.
    if ((uint32_t)s->paper_reset_seq != last_seen_reset_seq) {
        last_seen_reset_seq = (uint32_t)s->paper_reset_seq;
        history_count = 0;
        history_head  = 0;
        session_t0    = now_s;  // restart the "session seconds" axis
        last_sample_t = 0.0;
    }

    // Sample at most once per second
    if (now_s - last_sample_t >= 1.0) {
        last_sample_t = now_s;
        for (int c = 0; c < 16; c++) {
            double v = (c < s->per_core_count) ? s->per_core[c].core_realized : 0.0;
            pnl_history[history_head][c] = v;
        }
        time_history[history_head] = now_s - session_t0;
        history_head = (history_head + 1) % PNL_HISTORY;
        if (history_count < PNL_HISTORY) history_count++;
    }

    ImGui::Begin("Per-Core P&L");
    SectionHeader("PER-CORE P&L (session)");

    if (history_count < 2) {
        ImGui::TextColored(FoxmlColors::comment, "(collecting samples — wait 2s)");
        ImGui::End();
        return;
    }

    // Linearize the ring into a contiguous buffer for ImPlot
    static double xs[PNL_HISTORY];
    static double ys[16][PNL_HISTORY];
    int n = history_count;
    int start = (history_count == PNL_HISTORY) ? history_head : 0;
    for (int i = 0; i < n; i++) {
        int src = (start + i) % PNL_HISTORY;
        xs[i] = time_history[src];
        for (int c = 0; c < 16; c++) ys[c][i] = pnl_history[src][c];
    }

    if (ImPlot::BeginPlot("##percore_pnl", ImVec2(-1, -1),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes("session seconds", "realized P&L ($)",
                          ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        // One line per active core. Same color palette as the per-core
        // panel strategies (blue/orange/green/purple/teal cycle).
        static const ImVec4 line_colors[] = {
            {0.40f, 0.60f, 0.85f, 1.0f},  // core 0
            {0.85f, 0.55f, 0.25f, 1.0f},  // core 1
            {0.45f, 0.75f, 0.45f, 1.0f},  // core 2
            {0.65f, 0.45f, 0.80f, 1.0f},  // core 3
            {0.35f, 0.75f, 0.80f, 1.0f},  // core 4+
        };
        for (int c = 0; c < s->per_core_count && c < 16; c++) {
            char label[16];
            uint8_t sid = s->per_core[c].resolved_strategy_id;
            if (sid >= NUM_STRATEGIES) sid = s->per_core[c].strategy_id_display;
            const char *strat = (sid < NUM_STRATEGIES)
                ? STRATEGY_SHORT_NAMES[sid] : "?";
            snprintf(label, sizeof(label), "C%d %s", c, strat);
            ImPlotSpec spec;
            spec.LineColor  = line_colors[c % 5];
            spec.LineWeight = 1.5f;
            ImPlot::PlotLine(label, xs, ys[c], n, spec);
        }
        // Zero-line for breakeven reference
        double zx[2] = {xs[0], xs[n - 1]};
        double zy[2] = {0.0, 0.0};
        ImPlotSpec zspec;
        zspec.LineColor  = ImVec4(0.6f, 0.6f, 0.6f, 0.4f);
        zspec.LineWeight = 1.0f;
        ImPlot::PlotLine("##zero", zx, zy, 2, zspec);
        ImPlot::EndPlot();
    }
    ImGui::End();
}

//==========================================================================
// PANEL: STATS
//==========================================================================
static inline void GUI_Panel_Stats(const TUISnapshot *s) {
    ImGui::Begin("Stats");
    SectionHeader("STATS");

    uint32_t closed_trades = s->wins + s->losses;
    // v4.7.18: under partials, 1 logical trade = 2 leg-fills. Show both
    // numbers so the user can tell the difference at a glance.
    //   buys/exits headline = LOGICAL trades
    //   "(N fills)" tail    = PER-FILL heartbeat
    uint32_t logical_buys = (s->partial_exit_enabled && s->total_buys > 0)
                          ? (s->total_buys / 2u) : s->total_buys;

    ImGui::TextColored(FoxmlColors::sand, "buys:");
    ImGui::SameLine();
    ImGui::Text("%u", logical_buys);
    if (s->partial_exit_enabled) {
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "(%u fills)", s->total_buys);
    }
    ImGui::SameLine(0, 10);
    ImGui::TextColored(FoxmlColors::sand, "exits:");
    ImGui::SameLine();
    ImGui::Text("%u", closed_trades);
    if (s->partial_exit_enabled) {
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment, "(%u fills)", s->total_exits_fills);
    }
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
    // v5.3.1 (Phase D): profit_factor < 0 is the "all wins, no losses" sentinel
    // (mathematically ∞). Pre-fix code rendered 0.00 which was misleading.
    if (s->profit_factor < 0.0) {
        ImGui::TextColored(FoxmlColors::green, "%s", "\xE2\x88\x9E");  // ∞
    } else {
        ImGui::TextColored(PnlColor(s->profit_factor - 1.0), "%.2f", s->profit_factor);
    }
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

    if (closed_trades > 0) {
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
    GUI_Panel_Positions(s, shared);
    GUI_Panel_PerCorePnL(s);
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
                // v4.7.34: AUTO cores show "AUTO(routed)" — same pattern as
                // Per-Core P&L panel + Buy Gate detail header. Color follows
                // the resolved strategy.
                uint8_t cfg_sid_r  = pc->strategy_id_display;
                uint8_t live_sid_r = pc->resolved_strategy_id;
                if (live_sid_r >= NUM_STRATEGIES) live_sid_r = cfg_sid_r;
                if (cfg_sid_r == STRATEGY_AUTO &&
                    live_sid_r < NUM_STRATEGIES && live_sid_r != STRATEGY_AUTO) {
                    ImGui::TextColored(strat_colors[live_sid_r], "AUTO(%s)",
                                        STRATEGY_SHORT_NAMES[live_sid_r]);
                } else if (live_sid_r < NUM_STRATEGIES) {
                    ImGui::TextColored(strat_colors[live_sid_r], "%s",
                                        STRATEGY_SHORT_NAMES[live_sid_r]);
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

    // Per-core latency panel (sharded mode only). v5.0.1 (Phase H): split
    // into HOT and SLOW sub-tables. Slow-path table only populated when
    // engine_arch=per_core_slow; otherwise sp_samples stays 0.
    if (s->sharded_mode_active && s->per_core_count > 0) {
        ImGui::Begin("Per-Core Latency");

        SectionHeader("PER-ENGINE HOT-PATH LATENCY");
        ImGui::TextColored(FoxmlColors::comment, "(last 256 samples, subtract ~25-30ns rdtsc floor)");

        ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##percore_hot", 9, tf)) {
            ImGui::TableSetupColumn("Engine",  ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Strat",   ImGuiTableColumnFlags_WidthFixed, 35);
            ImGui::TableSetupColumn("Samples", ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Min",     ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Avg",     ImGuiTableColumnFlags_WidthFixed, 50);
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
                    for (int j = 0; j < 6; ++j) { ImGui::TableNextColumn(); ImGui::TextColored(FoxmlColors::comment, "-"); }
                } else {
                    ImGui::Text("%lu", (unsigned long)pc->samples);
                    ImGui::TableNextColumn(); ImGui::Text("%.0f", pc->min_ns);
                    ImGui::TableNextColumn(); ImGui::Text("%.0f", pc->avg_ns);
                    ImGui::TableNextColumn(); ImGui::Text("%.0f", pc->p50_ns);
                    ImGui::TableNextColumn(); ImGui::Text("%.0f", pc->p95_ns);
                    ImGui::TableNextColumn(); ImGui::Text("%.0f", pc->p99_ns);
                    ImGui::TableNextColumn(); ImGui::Text("%.0f", pc->max_ns);
                }
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        SectionHeader("PER-ENGINE SLOW-PATH LATENCY");
        ImGui::TextColored(FoxmlColors::comment,
            "(per-cycle work in engine_arch=per_core_slow; centralized = 0 samples)");

        if (ImGui::BeginTable("##percore_slow", 9, tf)) {
            ImGui::TableSetupColumn("Engine",  ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Strat",   ImGuiTableColumnFlags_WidthFixed, 35);
            ImGui::TableSetupColumn("Samples", ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Min",     ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Avg",     ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("p50",     ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("p95",     ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("p99",     ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Max",     ImGuiTableColumnFlags_WidthFixed, 60);
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
                if (pc->sp_samples == 0) {
                    ImGui::TextColored(FoxmlColors::comment, "-");
                    for (int j = 0; j < 6; ++j) { ImGui::TableNextColumn(); ImGui::TextColored(FoxmlColors::comment, "-"); }
                } else {
                    // Slow-path values are µs-scale; show with µ suffix when large.
                    auto fmt_ns = [](double ns) -> const char* {
                        static char buf[32];
                        if (ns >= 1000.0) snprintf(buf, sizeof(buf), "%.1fµs", ns / 1000.0);
                        else              snprintf(buf, sizeof(buf), "%.0fns", ns);
                        return buf;
                    };
                    ImGui::Text("%lu", (unsigned long)pc->sp_samples);
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(pc->sp_min_ns));
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(pc->sp_avg_ns));
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(pc->sp_p50_ns));
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(pc->sp_p95_ns));
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(pc->sp_p99_ns));
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(pc->sp_max_ns));
                }
            }
            ImGui::EndTable();
        }

        // v5.1.1: per-section work breakdown — only meaningful in
        // per_core_slow (centralized = section samples are zero).
        ImGui::Spacing();
        SectionHeader("PER-ENGINE SLOW-PATH WORK BREAKDOWN");
        ImGui::TextColored(FoxmlColors::comment,
            "(per-section p50/p99 inside the slow-path cycle; per_core_slow only)");

        if (ImGui::BeginTable("##percore_breakdown", 11, tf)) {
            ImGui::TableSetupColumn("Engine", ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Strat",  ImGuiTableColumnFlags_WidthFixed, 35);
            ImGui::TableSetupColumn("Rolling p50", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Rolling p99", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Rebuild p50", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Rebuild p99", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Push p50",  ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Push p99",  ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("TimeExit",  ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("TrailSL",   ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Σ p50",     ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableHeadersRow();
            auto fmt_ns = [](double ns) -> const char* {
                static char buf[32];
                if (ns >= 1000.0) snprintf(buf, sizeof(buf), "%.1fµs", ns / 1000.0);
                else              snprintf(buf, sizeof(buf), "%.0fns", ns);
                return buf;
            };
            for (int i = 0; i < s->per_core_count && i < 16; ++i) {
                const TUISnapshot::PerCoreSnap *pc = &s->per_core[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", i);
                ImGui::TableNextColumn();
                uint8_t sid = pc->strategy_id_display;
                ImGui::TextColored(FoxmlColors::primary, "%s",
                                   sid < NUM_STRATEGIES ? STRATEGY_SHORT_NAMES[sid] : "?");
                if (s->engine_arch != 1) {
                    for (int k = 0; k < 9; ++k) {
                        ImGui::TableNextColumn();
                        ImGui::TextColored(FoxmlColors::comment, "-");
                    }
                } else {
                    // Section order in struct: 0=rebuild, 1=push, 2=time, 3=trail, 4=other
                    // Column display reorders for readability.
                    // v5.1.3: indices match SP_SECTION_* (0=ROLLING, 1=REBUILD,
                    // 2=PUSH, 3=TIME_EXIT, 4=TRAIL_SL).
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(pc->sp_breakdown_p50_ns[0]));
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(pc->sp_breakdown_p99_ns[0]));
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(pc->sp_breakdown_p50_ns[1]));
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(pc->sp_breakdown_p99_ns[1]));
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(pc->sp_breakdown_p50_ns[2]));
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(pc->sp_breakdown_p99_ns[2]));
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(pc->sp_breakdown_p50_ns[3]));
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(pc->sp_breakdown_p50_ns[4]));
                    double sum_p50 = pc->sp_breakdown_p50_ns[0] +
                                     pc->sp_breakdown_p50_ns[1] +
                                     pc->sp_breakdown_p50_ns[2] +
                                     pc->sp_breakdown_p50_ns[3] +
                                     pc->sp_breakdown_p50_ns[4];
                    ImGui::TableNextColumn(); ImGui::Text("%s", fmt_ns(sum_p50));
                }
            }
            ImGui::EndTable();
        }
        ImGui::TextColored(FoxmlColors::comment,
            "Σ p50 = sum of section p50s — should approximate the total sp p50 above.");

        ImGui::End();
    }

    // ─────────────────────────────────────────────────────────────────────
    // v5.0.2 (Phase H): Engine Topology panel — shows the static thread
    // layout: which CPU each thread is pinned to, what each engine's
    // strategy + cadence is, and the system architecture (engine_arch,
    // nproc, slow_path_pin_offset). Helps diagnose pin conflicts /
    // unexpected OS scheduling and explains "why is engine 3 a bit
    // jittery on this box".
    // ─────────────────────────────────────────────────────────────────────
    if (s->sharded_mode_active && s->per_core_count > 0) {
        ImGui::Begin("Engine Topology");

        SectionHeader("SYSTEM");
        const char *arch_label = (s->engine_arch == 1)
            ? "per_core_slow"
            : "centralized";
        ImGui::TextColored(FoxmlColors::comment, "engine_arch:");
        ImGui::SameLine();
        ImGui::TextColored(s->engine_arch == 1
                            ? FoxmlColors::green_b
                            : FoxmlColors::accent,
                           "%s", arch_label);

        ImGui::TextColored(FoxmlColors::comment, "system CPUs (nproc):");
        ImGui::SameLine();
        ImGui::Text("%d", (int)s->nproc);

        ImGui::TextColored(FoxmlColors::comment, "slow_path_pin_offset:");
        ImGui::SameLine();
        if (s->slow_path_pin_offset < 0) {
            ImGui::TextColored(FoxmlColors::yellow, "%d (UNPINNED)",
                               (int)s->slow_path_pin_offset);
        } else if (s->slow_path_pin_offset == 0) {
            ImGui::TextColored(FoxmlColors::text, "0 (auto: base CPU %d)",
                               s->per_core_count + 2);
        } else {
            ImGui::TextColored(FoxmlColors::text, "%d (explicit base)",
                               (int)s->slow_path_pin_offset);
        }

        ImGui::Spacing();
        SectionHeader("SHARED THREADS");
        if (ImGui::BeginTable("##topo_shared", 2,
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("CPU",    ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("Producer");
            ImGui::TableNextColumn(); ImGui::Text("%d", (int)s->producer_cpu);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("Drainer");
            ImGui::TableNextColumn(); ImGui::Text("%d", (int)s->drainer_cpu);

            ImGui::EndTable();
        }

        ImGui::Spacing();
        SectionHeader("PER-ENGINE THREADS");
        // v5.0.3: now includes live thread state, drift, and lifecycle.
        const int topo_col_count = (s->engine_arch == 1 && shared) ? 10 : 9;
        if (ImGui::BeginTable("##topo_engines", topo_col_count,
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Engine",   ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Strategy", ImGuiTableColumnFlags_WidthFixed, 95);
            ImGui::TableSetupColumn("Hot CPU",  ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Slow CPU", ImGuiTableColumnFlags_WidthFixed, 65);
            ImGui::TableSetupColumn("Poll",     ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("State",    ImGuiTableColumnFlags_WidthFixed, 65);
            ImGui::TableSetupColumn("Last cycle", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Cycles",   ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Q",        ImGuiTableColumnFlags_WidthFixed, 40);
            if (s->engine_arch == 1 && shared) {
                ImGui::TableSetupColumn("",     ImGuiTableColumnFlags_WidthFixed, 70);
            }
            ImGui::TableHeadersRow();

            // wall-now in us — used for "last cycle" Δ display
            uint64_t now_us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            for (int i = 0; i < s->per_core_count && i < 16; ++i) {
                const TUISnapshot::PerCoreSnap *pc = &s->per_core[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();

                ImGui::TableNextColumn(); ImGui::Text("%d", i);

                ImGui::TableNextColumn();
                uint8_t sid = pc->strategy_id_display;
                uint8_t rsid = pc->resolved_strategy_id;
                if (sid == STRATEGY_AUTO && rsid != STRATEGY_NONE) {
                    ImGui::TextColored(FoxmlColors::primary, "AUTO(%s)",
                                       rsid < NUM_STRATEGIES
                                            ? STRATEGY_SHORT_NAMES[rsid] : "?");
                } else {
                    ImGui::TextColored(FoxmlColors::primary, "%s",
                                       sid < NUM_STRATEGIES
                                            ? STRATEGY_SHORT_NAMES[sid] : "?");
                }

                ImGui::TableNextColumn();
                if (pc->hot_path_cpu < 0) {
                    ImGui::TextColored(FoxmlColors::yellow, "unpin");
                } else {
                    ImGui::Text("%d", (int)pc->hot_path_cpu);
                }

                ImGui::TableNextColumn();
                if (s->engine_arch != 1) {
                    ImGui::TextColored(FoxmlColors::comment, "(prod)");
                } else if (pc->slow_path_cpu < 0) {
                    ImGui::TextColored(FoxmlColors::yellow, "unpin");
                } else {
                    ImGui::Text("%d", (int)pc->slow_path_cpu);
                }

                ImGui::TableNextColumn();
                ImGui::Text("%u t", (unsigned)pc->poll_interval_ticks);

                // State — coarse thread state with color
                ImGui::TableNextColumn();
                if (s->engine_arch != 1) {
                    ImGui::TextColored(FoxmlColors::comment, "(prod)");
                } else {
                    switch (pc->sp_state) {
                        case 0: ImGui::TextColored(FoxmlColors::green_b, "running"); break;
                        case 1: ImGui::TextColored(FoxmlColors::yellow,  "parked");  break;
                        case 2: ImGui::TextColored(FoxmlColors::comment, "yield");   break;
                        case 3: ImGui::TextColored(FoxmlColors::red,     "PAUSED");  break;
                        default: ImGui::TextColored(FoxmlColors::comment, "?"); break;
                    }
                }

                // Last cycle — Δus since last sp_last_tick_us. Drift indicator.
                ImGui::TableNextColumn();
                if (s->engine_arch != 1) {
                    ImGui::TextColored(FoxmlColors::comment, "-");
                } else if (pc->sp_last_tick_us == 0) {
                    ImGui::TextColored(FoxmlColors::comment, "warmup");
                } else {
                    uint64_t delta = (now_us > pc->sp_last_tick_us)
                        ? (now_us - pc->sp_last_tick_us) : 0;
                    if (delta < 1000) {
                        ImGui::Text("%lluµs", (unsigned long long)delta);
                    } else if (delta < 1000000) {
                        ImGui::Text("%.1fms", (double)delta / 1000.0);
                    } else {
                        ImGui::TextColored(FoxmlColors::yellow, "%.1fs",
                                           (double)delta / 1000000.0);
                    }
                }

                // Cycles — total completed slow-path cycles
                ImGui::TableNextColumn();
                if (s->engine_arch != 1) {
                    ImGui::TextColored(FoxmlColors::comment, "-");
                } else {
                    ImGui::Text("%llu", (unsigned long long)pc->sp_cycles_total);
                }

                // Submit queue depth (capacity 32; warn if > 16)
                ImGui::TableNextColumn();
                if (pc->sp_submit_q_depth > 16) {
                    ImGui::TextColored(FoxmlColors::yellow, "%u",
                                       (unsigned)pc->sp_submit_q_depth);
                } else {
                    ImGui::Text("%u", (unsigned)pc->sp_submit_q_depth);
                }

                // Pause/Resume — only in per_core_slow with shared control
                if (s->engine_arch == 1 && shared) {
                    ImGui::TableNextColumn();
                    bool paused = (shared->paused_engines_mask &
                                   (uint16_t)(1u << i)) != 0;
                    const char *btn_label = paused ? "Resume" : "Pause";
                    if (ImGui::Button(btn_label)) {
                        // Toggle bit i. Single GUI thread is the writer.
                        if (paused) {
                            shared->paused_engines_mask &= (uint16_t)~(1u << i);
                        } else {
                            shared->paused_engines_mask |= (uint16_t)(1u << i);
                        }
                    }
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextColored(FoxmlColors::comment,
            "Hot CPU = pinned core for the SPSC consumer (per-tick gate eval).");
        ImGui::TextColored(FoxmlColors::comment,
            "Slow CPU = pinned core for the per-engine slow-path thread.");
        ImGui::TextColored(FoxmlColors::comment,
            "(prod) = centralized arch — slow-path runs on the producer thread.");
        ImGui::TextColored(FoxmlColors::comment,
            "State: running=actively rebuilding | yield=between cadences | parked=reset in progress | PAUSED=user toggle.");
        ImGui::TextColored(FoxmlColors::comment,
            "Last cycle = wall-time since the last completed cycle. > 1s = thread starved or stopped.");
        ImGui::TextColored(FoxmlColors::comment,
            "Q = current OMS submit-queue depth (cap 32). > 16 indicates drainer backpressure.");

        ImGui::End();
    }
}
