// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [TUI WIDGETS]
//======================================================================================================
// FTXUI widget components for the trading engine TUI
// each widget is a pure function: TUISnapshot → ftxui::Element
// compose these into layouts in TUILayout.hpp
//======================================================================================================
#ifndef TUI_WIDGETS_HPP
#define TUI_WIDGETS_HPP

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/canvas.hpp>
#include <ftxui/screen/color.hpp>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <vector>

// TUISnapshot and TUIPositionSnap are defined in EngineTUI.hpp
// which includes this file via TUILayout.hpp — no circular include needed
// the structs are already visible by the time these functions are called

using namespace ftxui;

//======================================================================================================
// [FOXML COLOR PALETTE]
//======================================================================================================
// warm forest tones — matches the FoxML neovim colorscheme and zsh welcome
//======================================================================================================
namespace foxml {
    const Color clay       = Color::RGB(204, 142, 106);  // 173 — headers, branding
    const Color wheat      = Color::RGB(220, 198, 150);  // 180 — price values
    const Color mauve      = Color::RGB(175, 135, 135);  // 138 — accents
    const Color sage       = Color::RGB(180, 175, 140);  // 144 — labels
    const Color sand       = Color::RGB(190, 170, 140);  // sand — body text
    const Color peach      = Color::RGB(230, 165, 120);  // peach — section headers
    const Color fg         = Color::RGB(200, 190, 170);  // default foreground
    const Color dim        = Color::RGB(120, 115, 105);  // dimmed text
    const Color green      = Color::RGB(140, 195, 130);  // positive P&L
    const Color red        = Color::RGB(210, 120, 120);  // negative P&L
    const Color yellow     = Color::RGB(230, 200, 110);  // warnings, trailing
    const Color pink       = Color::RGB(210, 150, 170);  // hotkeys
    const Color surf       = Color::RGB(100, 100, 90);   // separators
}

//======================================================================================================
// [HELPERS]
//======================================================================================================
static inline Element pnl_text(double val, std::string s) {
    return text(s) | color(val >= 0 ? foxml::green : foxml::red);
}

static inline std::string fmt(const char *f, double v) {
    char buf[64]; snprintf(buf, sizeof(buf), f, v); return std::string(buf);
}

static inline std::string fmt_i(const char *f, int v) {
    char buf[64]; snprintf(buf, sizeof(buf), f, v); return std::string(buf);
}

static inline std::string fmt_lu(const char *f, unsigned long v) {
    char buf[64]; snprintf(buf, sizeof(buf), f, v); return std::string(buf);
}

//======================================================================================================
// [HEADER WIDGET]
//======================================================================================================
static inline Element Widget_Header(const TUISnapshot *s) {
    // fox ascii art
    auto fox = vbox({
        text("  ╱|、") | color(foxml::clay),
        hbox({text("(") | color(foxml::clay), text("˚") | color(foxml::mauve),
              text("ˎ ") | color(foxml::clay), text("。") | color(foxml::mauve),
              text("7") | color(foxml::clay)}),
        hbox({text(" |、") | color(foxml::clay), text("˜") | color(foxml::mauve),
              text("〵") | color(foxml::clay)}),
        hbox({text(" じし") | color(foxml::clay), text("ˍ") | color(foxml::mauve),
              text(",)ノ") | color(foxml::clay)}),
    });

    // FOXML block text
    auto foxml_logo = vbox({
        hbox({text("█▀▀") | color(foxml::clay), text(" "), text("█▀█") | color(foxml::wheat),
              text(" "), text("▀▄▀") | color(foxml::mauve), text(" "), text("█▀▄▀█") | color(foxml::sage),
              text(" "), text("█") | color(foxml::clay)}),
        hbox({text("█▀ ") | color(foxml::clay), text(" "), text("█ █") | color(foxml::wheat),
              text(" "), text(" █ ") | color(foxml::mauve), text(" "), text("█ ▀ █") | color(foxml::sage),
              text(" "), text("█") | color(foxml::clay)}),
        hbox({text("▀  ") | color(foxml::clay), text(" "), text("▀▀▀") | color(foxml::wheat),
              text(" "), text("▀ ▀") | color(foxml::mauve), text(" "), text("▀   ▀") | color(foxml::sage),
              text(" "), text("▀▀▀") | color(foxml::clay)}),
        hbox({
            text("● ") | color(foxml::mauve), text("● ") | color(foxml::wheat),
            text("● ") | color(foxml::clay), text("● ") | color(foxml::sage),
            text("● ") | color(foxml::mauve), text("● ") | color(foxml::wheat),
            text("● ") | color(foxml::clay), text("●") | color(foxml::sage),
        }),
    });

    // state + uptime
    std::string state_str = s->state_warmup ? "WARMUP" : (s->is_paused ? "PAUSED" : "ACTIVE");
    Color state_color = s->state_warmup ? foxml::yellow : (s->is_paused ? foxml::yellow : foxml::green);

    // uptime from first call (start_time in snapshot is 0 — TUI manages its own clock)
    static time_t tui_start = time(NULL);
    time_t now = time(NULL);
    int uptime_sec = (int)difftime(now, tui_start);
    int hrs = uptime_sec / 3600, mins = (uptime_sec % 3600) / 60, secs = uptime_sec % 60;
    char uptime_buf[16];
    snprintf(uptime_buf, sizeof(uptime_buf), "%02d:%02d:%02d", hrs, mins, secs);

    auto info = vbox({
        text(""),
        hbox({text("TICK TRADER") | bold | color(foxml::peach)}),
        hbox({text(state_str) | bold | color(state_color), text("  "),
              text(uptime_buf) | color(foxml::dim)}),
    });

    return hbox({
        fox,
        text("  "),
        info,
        filler(),
        foxml_logo,
    }) | borderLight;
}

//======================================================================================================
// [PRICE WIDGET]
//======================================================================================================
static inline Element Widget_Price(const TUISnapshot *s) {
    return hbox({
        text("PRICE ") | color(foxml::sand),
        text(fmt("%.2f", s->price)) | bold | color(foxml::wheat),
        text("  ") | color(foxml::dim),
        text("VOL ") | color(foxml::sand),
        text(fmt("%.8f", s->volume)) | color(foxml::fg),
    });
}

//======================================================================================================
// [MARKET STRUCTURE WIDGET]
//======================================================================================================
static inline Element Widget_Market(const TUISnapshot *s) {
    const char *trend_str = (s->slope_pct > 0.001) ? "UP" : (s->slope_pct < -0.001) ? "DOWN" : "FLAT";
    Color trend_color = (s->slope_pct > 0.001) ? foxml::green : (s->slope_pct < -0.001) ? foxml::red : foxml::dim;

    return vbox({
        text("MARKET STRUCTURE") | bold | color(foxml::peach),
        hbox({text("  avg: ") | color(foxml::sand), text(fmt("%.2f", s->roll_price_avg)) | color(foxml::fg),
              text("  stddev: ") | color(foxml::sand), text(fmt("%.2f", s->roll_stddev)) | color(foxml::fg)}),
        hbox({text("  slope: ") | color(foxml::sand), text(fmt("%+.6f%%/tick", s->slope_pct)) | color(foxml::fg),
              text("  trend: ") | color(foxml::sand), text(trend_str) | bold | color(trend_color)}),
        hbox({text("  range: ") | color(foxml::sand), text(fmt("%.2f", s->roll_p_min)) | color(foxml::fg),
              text(" - ") | color(foxml::dim), text(fmt("%.2f", s->roll_p_max)) | color(foxml::fg)}),
    });
}

//======================================================================================================
// [BUY GATE WIDGET]
//======================================================================================================
static inline Element Widget_BuyGate(const TUISnapshot *s) {
    std::string mode = s->stddev_mode ? fmt("stddev: %.2fx", s->live_sm) : fmt("offset: %.3f%%", s->live_offset);

    return vbox({
        text("BUY GATE") | bold | color(foxml::peach),
        hbox({text(s->gate_direction ? "  price >= " : "  price <= ") | color(foxml::sand), text(fmt("%.2f", s->buy_p)) | color(foxml::fg),
              text("  (") | color(foxml::dim), text(mode) | color(foxml::fg), text(")") | color(foxml::dim)}),
        hbox({text("  distance: ") | color(foxml::sand), text(fmt("$%.2f", s->gate_dist)) | color(foxml::fg),
              text(fmt("  (%.3f%%)", s->gate_dist_pct)) | color(foxml::dim)}),
        hbox({text("  spacing: ") | color(foxml::sand), text(fmt("$%.2f", s->spacing)) | color(foxml::fg),
              text(fmt("  (%.3f%%)", s->spacing_pct)) | color(foxml::dim)}),
    });
}

//======================================================================================================
// [PORTFOLIO WIDGET]
//======================================================================================================
static inline Element Widget_Portfolio(const TUISnapshot *s) {
    float exp_ratio = (s->max_exp > 0) ? (float)(s->exposure_pct / s->max_exp) : 0;

    return vbox({
        text("PORTFOLIO") | bold | color(foxml::peach),
        hbox({text("  equity:   ") | color(foxml::sand), text(fmt("$%.4f", s->equity)) | color(foxml::wheat)}),
        hbox({text("  balance:  ") | color(foxml::sand), text(fmt("$%.4f", s->balance)) | color(foxml::fg)}),
        hbox({text("  held:     ") | color(foxml::sand), text(fmt("$%.4f", s->total_value)) | color(foxml::fg)}),
        hbox({text("  exposure: ") | color(foxml::sand),
              text(fmt("%.1f%%", s->exposure_pct)) | color(foxml::fg),
              text(fmt("/%.0f%%", s->max_exp)) | color(foxml::dim),
              text("  "), gauge(exp_ratio) | size(WIDTH, EQUAL, 15) | color(foxml::clay)}),
    });
}

//======================================================================================================
// [P&L WIDGET]
//======================================================================================================
static inline Element Widget_PnL(const TUISnapshot *s) {
    return vbox({
        text("P&L") | bold | color(foxml::peach),
        hbox({text("  realized:   ") | color(foxml::sand),
              text(fmt("$%+.4f", s->realized)) | color(s->realized >= 0 ? foxml::green : foxml::red)}),
        hbox({text("  unrealized: ") | color(foxml::sand),
              text(fmt("$%+.4f", s->unrealized)) | color(s->unrealized >= 0 ? foxml::green : foxml::red)}),
        hbox({text("  total:      ") | color(foxml::sand),
              text(fmt("$%+.4f", s->total_pnl)) | bold | color(s->total_pnl >= 0 ? foxml::green : foxml::red),
              text(fmt("  (%.2f%%)", s->return_pct)) | color(foxml::dim)}),
    });
}

//======================================================================================================
// [RISK WIDGET]
//======================================================================================================
static inline Element Widget_Risk(const TUISnapshot *s) {
    const char *strat = (s->strategy_id == 1) ? "MOMENTUM" : "MEAN REVERSION";
    const char *regime = (s->current_regime == 1) ? "TRENDING" :
                         (s->current_regime == 2) ? "VOLATILE" :
                         (s->current_regime == 3) ? "TRENDING_DOWN" : "RANGING";
    Color regime_color = (s->current_regime == 1) ? foxml::green :
                         (s->current_regime == 2 || s->current_regime == 3) ? foxml::red : foxml::dim;

    return vbox({
        text("RISK") | bold | color(foxml::peach),
        hbox({text("  risk/pos: ") | color(foxml::sand), text(fmt("%.1f%%", s->risk_amt)) | color(foxml::fg),
              text("  breaker: ") | color(foxml::sand),
              text(s->breaker_tripped ? "TRIPPED" : "OK") | bold | color(s->breaker_tripped ? foxml::red : foxml::green)}),
        hbox({text("  strategy: ") | color(foxml::sand), text(strat) | color(foxml::fg),
              text("  |  ") | color(foxml::dim), text("PAPER") | color(foxml::yellow)}),
        hbox({text("  regime:   ") | color(foxml::sand), text(regime) | bold | color(regime_color),
              text(fmt("  (%.0fm)", s->regime_duration_min)) | color(foxml::dim)}),
    });
}

//======================================================================================================
// [STATS WIDGET]
//======================================================================================================
static inline Element Widget_Stats(const TUISnapshot *s) {
    uint32_t total_exits = s->wins + s->losses;
    Color wr_color = (s->win_rate >= 50.0) ? foxml::green : (total_exits > 0 ? foxml::red : foxml::dim);
    Color pf_color = (s->profit_factor >= 1.0) ? foxml::green : (total_exits > 0 ? foxml::red : foxml::dim);

    return vbox({
        text("STATS") | bold | color(foxml::peach),
        hbox({text("  buys: ") | color(foxml::sand), text(fmt_i("%d", s->total_buys)) | color(foxml::fg),
              text("  exits: ") | color(foxml::sand), text(fmt_i("%d", total_exits)) | color(foxml::fg),
              text("  hold: ") | color(foxml::sand), text(fmt("%.0f ticks", s->avg_hold)) | color(foxml::fg)}),
        hbox({text("  wins: ") | color(foxml::sand), text(fmt_i("%d", s->wins)) | color(foxml::green),
              text("  losses: ") | color(foxml::sand), text(fmt_i("%d", s->losses)) | color(foxml::red),
              text("  rate: ") | color(foxml::sand), text(fmt("%.1f%%", s->win_rate)) | color(wr_color),
              text("  pf: ") | color(foxml::sand), text(fmt("%.2f", s->profit_factor)) | color(pf_color)}),
    });
}

//======================================================================================================
// [CONFIG WIDGET]
//======================================================================================================
static inline Element Widget_Config(const TUISnapshot *s) {
    std::string offset_str = s->stddev_mode
        ? fmt("stddev %.2fx", s->cfg_offset_val)
        : fmt("%.3f%%", s->cfg_offset_val);

    Elements rows;
    rows.push_back(text("CONFIG") | bold | color(foxml::peach));
    rows.push_back(hbox({
        text("  TP: ") | color(foxml::sand), text(fmt("%.1f%%", s->cfg_tp)) | color(foxml::fg),
        text("  SL: ") | color(foxml::sand), text(fmt("%.1f%%", s->cfg_sl)) | color(foxml::fg),
        text("  risk: ") | color(foxml::sand), text(fmt("%.1f%%", s->risk_amt)) | color(foxml::fg),
        text("  fee: ") | color(foxml::sand), text(fmt("%.1f%%", s->cfg_fee)) | color(foxml::fg),
    }));
    rows.push_back(hbox({
        text("  offset: ") | color(foxml::sand), text(offset_str) | color(foxml::fg),
    }));
    if (s->trailing_enabled) {
        rows.push_back(hbox({
            text("  trail: ") | color(foxml::sand), text(fmt("%.1f", s->cfg_trail_mult)) | color(foxml::fg),
            text("σ") | color(foxml::dim),
            text("  sl: ") | color(foxml::sand), text(fmt("%.1f", s->cfg_sl_trail_mult)) | color(foxml::fg),
            text("σ") | color(foxml::dim),
            text("  score: ") | color(foxml::sand), text(fmt("%.2f", s->cfg_hold_score)) | color(foxml::fg),
        }));
    }

    return vbox(rows);
}

//======================================================================================================
// [POSITIONS WIDGET]
//======================================================================================================
static inline Element Widget_Positions(const TUISnapshot *s) {
    Elements rows;
    rows.push_back(hbox({
        text("POSITIONS") | bold | color(foxml::peach),
        text(fmt_i(" (%d/16)", s->active_count)) | color(foxml::dim),
    }));

    int displayed = 0;
    for (int i = 0; i < 16; i++) {
        const TUIPositionSnap *ps = &s->positions[i];
        if (ps->idx < 0) continue;

        double diff = s->price - ps->entry;
        Color diff_color = diff >= 0 ? foxml::green : foxml::red;

        if (displayed > 0)
            rows.push_back(separator() | color(foxml::surf));

        rows.push_back(hbox({
            text(fmt_i("#%-2d ", displayed)) | color(foxml::wheat),
            text(fmt("$%.2f", ps->entry)) | color(foxml::fg),
            text("→") | color(foxml::dim),
            text(fmt("$%.2f", s->price)) | color(foxml::wheat),
            text(fmt(" %+.2f", diff)) | color(diff_color),
        }));
        rows.push_back(hbox({
            text("    qty:") | color(foxml::sand),
            text(fmt("%.6f", ps->qty)) | color(foxml::fg),
            text(" val:") | color(foxml::sand),
            text(fmt("$%.2f", ps->value)) | color(foxml::fg),
        }));

        const char *trail = ps->is_trailing ? (ps->above_orig_tp ? " HOLD" : " trail") : "";
        Color trail_color = ps->above_orig_tp ? foxml::yellow : foxml::dim;

        rows.push_back(hbox({
            text("    TP:") | color(foxml::sand),
            text(fmt("$%.0f", ps->tp)) | color(foxml::green),
            text(trail) | color(trail_color),
            text(" SL:") | color(foxml::sand),
            text(fmt("$%.0f", ps->sl)) | color(foxml::red),
        }));
        rows.push_back(hbox({
            text("    g:") | color(foxml::sand),
            text(fmt("%+.2f%%", ps->gross_pnl)) | color(ps->gross_pnl >= 0 ? foxml::green : foxml::red),
            text(" n:") | color(foxml::sand),
            text(fmt("%+.2f%%", ps->net_pnl)) | color(ps->net_pnl >= 0 ? foxml::green : foxml::red),
            text(fmt(" hold:%.0fm", ps->hold_minutes)) | color(foxml::dim),
        }));

        displayed++;
    }

    if (displayed == 0)
        rows.push_back(text("  (none)") | color(foxml::dim));

    return vbox(rows) | vscroll_indicator | frame | flex;
}

//======================================================================================================
// [LATENCY WIDGET]
//======================================================================================================
#ifdef LATENCY_PROFILING
static inline Element Widget_Latency(const TUISnapshot *s) {
    if (s->hot_count == 0) return text("");

    Elements rows;
    rows.push_back(text("LATENCY") | bold | color(foxml::peach));
    rows.push_back(hbox({
        text("  hot: ") | color(foxml::sand),
        text(fmt("avg %.0fns", s->hot_avg_ns)) | color(foxml::fg),
        text(fmt("  min %.0fns", s->hot_min_ns)) | color(foxml::dim),
        text(fmt("  max %.0fns", s->hot_max_ns)) | color(foxml::dim),
    }));
    rows.push_back(hbox({
        text("       ") | color(foxml::sand),
        text(fmt("p50 %.0fns", s->hot_p50_ns)) | color(foxml::fg),
        text(fmt("  p95 %.0fns", s->hot_p95_ns)) | color(foxml::fg),
        text(fmt_lu("  (%lu ticks)", (unsigned long)s->hot_count)) | color(foxml::dim),
    }));

    if (s->slow_count > 0) {
        double sa = (s->slow_avg_ns >= 1000.0) ? s->slow_avg_ns / 1000.0 : s->slow_avg_ns;
        const char *su = (s->slow_avg_ns >= 1000.0) ? "us" : "ns";
        rows.push_back(hbox({
            text("  slow: ") | color(foxml::sand),
            text(fmt("avg %.1f", sa)) | color(foxml::fg),
            text(su) | color(foxml::fg),
            text(fmt_lu("  (%lu cycles)", (unsigned long)s->slow_count)) | color(foxml::dim),
        }));
    }

    return vbox(rows);
}
#endif

//======================================================================================================
// [PRICE CHART — line + buy gate + TP/SL bands]
//======================================================================================================
static inline Element Widget_PriceGraph(const TUISnapshot *s, int term_w, int term_h) {
    if (s->graph_count < 2) return text("  (collecting data...)") | color(foxml::dim);

    int len = s->graph_count;
    int start = (s->graph_head - len + TUISnapshot::GRAPH_LEN) % TUISnapshot::GRAPH_LEN;

    // find min/max for price, include buy gate and TP/SL in range
    double pmn = 1e18, pmx = -1e18, vmx = 0;
    for (int i = 0; i < len; i++) {
        double p = s->price_history[(start + i) % TUISnapshot::GRAPH_LEN];
        double v = s->volume_history[(start + i) % TUISnapshot::GRAPH_LEN];
        if (p < pmn) pmn = p;
        if (p > pmx) pmx = p;
        if (v > vmx) vmx = v;
    }
    // extend range to include buy gate if visible
    if (s->buy_p > 0 && s->buy_p < pmn) pmn = s->buy_p;
    if (s->buy_p > pmx) pmx = s->buy_p;
    double prange = pmx - pmn;
    if (prange < 0.01) prange = 0.01;
    // pad 5% on each side so lines don't clip at edges
    pmn -= prange * 0.05;
    pmx += prange * 0.05;
    prange = pmx - pmn;
    if (vmx < 1e-10) vmx = 1e-10;

    // copy data
    std::vector<double> prices(len), volumes(len);
    for (int i = 0; i < len; i++) {
        prices[i] = s->price_history[(start + i) % TUISnapshot::GRAPH_LEN];
        volumes[i] = s->volume_history[(start + i) % TUISnapshot::GRAPH_LEN];
    }

    // capture snapshot values for lambda
    double buy_gate = s->buy_p;
    double nearest_tp = 0, nearest_sl = 0;
    for (int i = 0; i < 16; i++) {
        if (s->positions[i].idx >= 0) {
            nearest_tp = s->positions[i].tp;
            nearest_sl = s->positions[i].sl;
            break;
        }
    }

    // responsive canvas sizing — scale to terminal dimensions
    // canvas coords: 2 braille dots per char col, 4 per char row
    int cw = std::max(term_w - 2, 20) * 2;
    int ch = std::max(std::min(term_h / 4, 14), 6) * 4;
    int vch = std::max(std::min(term_h / 12, 4), 2) * 4;

    // price chart canvas
    auto price_canvas = [prices, pmn, prange, cw, ch, buy_gate, nearest_tp, nearest_sl]() {
        ftxui::Canvas c(cw, ch);
        int dlen = (int)prices.size();
        if (dlen < 2) return canvas(std::move(c));

        auto y_of = [&](double price) -> int {
            return ch - 1 - (int)(((price - pmn) / prange) * (ch - 2));
        };

        // TP band (green, dotted)
        if (nearest_tp > 0) {
            int tp_y = y_of(nearest_tp);
            if (tp_y >= 0 && tp_y < ch)
                for (int x = 0; x < cw; x += 4)
                    c.DrawPoint(x, tp_y, true, ftxui::Color::RGB(140, 195, 130));
        }

        // SL band (red, dotted)
        if (nearest_sl > 0) {
            int sl_y = y_of(nearest_sl);
            if (sl_y >= 0 && sl_y < ch)
                for (int x = 0; x < cw; x += 4)
                    c.DrawPoint(x, sl_y, true, ftxui::Color::RGB(210, 120, 120));
        }

        // buy gate line (clay, dashed)
        if (buy_gate > 0) {
            int bg_y = y_of(buy_gate);
            if (bg_y >= 0 && bg_y < ch)
                for (int x = 0; x < cw; x += 3)
                    c.DrawPoint(x, bg_y, true, ftxui::Color::RGB(204, 142, 106));
        }

        // price line (wheat, solid)
        for (int x = 1; x < cw; x++) {
            int i0 = (int)((double)(x-1) / cw * dlen); if (i0 >= dlen) i0 = dlen - 1;
            int i1 = (int)((double)x / cw * dlen);     if (i1 >= dlen) i1 = dlen - 1;
            c.DrawPointLine(x-1, y_of(prices[i0]), x, y_of(prices[i1]),
                            ftxui::Color::RGB(220, 198, 150));
        }

        // current price dot (bright)
        if (dlen > 0) {
            int last_y = y_of(prices[dlen - 1]);
            c.DrawPoint(cw - 1, last_y, true, ftxui::Color::RGB(255, 230, 180));
            c.DrawPoint(cw - 2, last_y, true, ftxui::Color::RGB(255, 230, 180));
        }

        return canvas(std::move(c));
    };

    // volume bars canvas (separate, below price)
    auto vol_canvas = [volumes, vmx, cw, vch]() {
        ftxui::Canvas c(cw, vch);
        int dlen = (int)volumes.size();

        for (int x = 0; x < cw; x++) {
            int idx = (int)((double)x / cw * dlen);
            if (idx >= dlen) idx = dlen - 1;
            int vh = (int)((volumes[idx] / vmx) * (vch - 1));
            for (int y = 0; y < vh; y++)
                c.DrawPoint(x, vch - 1 - y, true, ftxui::Color::RGB(180, 175, 140));
        }

        return canvas(std::move(c));
    };

    // use actual data min/max for labels (before padding), current price for header
    double data_min = pmn + prange * 0.05; // undo the padding we added for chart scaling
    double data_max = pmx - prange * 0.05;
    double cur_price = prices.back();

    // legend: current price + overlays
    Elements legend;
    legend.push_back(text(fmt("$%.2f", cur_price)) | bold | color(foxml::wheat));
    if (buy_gate > 0)
        legend.push_back(text(fmt("  gate $%.2f", buy_gate)) | color(foxml::clay));
    if (nearest_tp > 0)
        legend.push_back(text(fmt("  TP $%.0f", nearest_tp)) | color(foxml::green));
    if (nearest_sl > 0)
        legend.push_back(text(fmt("  SL $%.0f", nearest_sl)) | color(foxml::red));

    return vbox({
        hbox({text("PRICE") | bold | color(foxml::peach), text("  ") | color(foxml::dim),
              hbox(legend)}),
        price_canvas() | flex,
        hbox({text(fmt("$%.2f", data_min)) | color(foxml::dim),
              filler(),
              text(fmt("$%.2f", data_max)) | color(foxml::dim),
              filler(),
              text("now") | color(foxml::dim)}),
        vol_canvas(),
    }) | flex;
}

//======================================================================================================
// [P&L GRAPH WIDGET]
//======================================================================================================
static inline Element Widget_PnLGraph(const TUISnapshot *s, int term_w, int term_h) {
    if (s->graph_count < 2) return text("  (collecting data...)") | color(foxml::dim);

    int len = s->graph_count;
    int start = (s->graph_head - len + TUISnapshot::GRAPH_LEN) % TUISnapshot::GRAPH_LEN;

    double mn = 1e18, mx = -1e18;
    for (int i = 0; i < len; i++) {
        double v = s->pnl_history[(start + i) % TUISnapshot::GRAPH_LEN];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    // include zero line in range
    if (mn > 0) mn = 0;
    if (mx < 0) mx = 0;
    double range = mx - mn;
    if (range < 0.01) range = 0.01;

    std::vector<double> data(len);
    for (int i = 0; i < len; i++)
        data[i] = s->pnl_history[(start + i) % TUISnapshot::GRAPH_LEN];

    int cw = std::max(term_w - 2, 20) * 2;
    int ch = std::max(std::min(term_h / 5, 10), 4) * 4;
    auto pnl_color = s->total_pnl >= 0
        ? ftxui::Color::RGB(140, 195, 130)   // green
        : ftxui::Color::RGB(210, 120, 120);   // red
    // zero line position
    double zero_y_frac = (0.0 - mn) / range;

    auto canvas_fn = [data, mn, range, cw, ch, pnl_color, zero_y_frac]() {
        ftxui::Canvas c(cw, ch);
        int dlen = (int)data.size();
        if (dlen < 2) return canvas(std::move(c));

        // draw zero line (dim)
        int zero_y = ch - 1 - (int)(zero_y_frac * (ch - 2));
        for (int x = 0; x < cw; x += 3)
            c.DrawPoint(x, zero_y, true, ftxui::Color::RGB(100, 100, 90));

        // P&L line
        for (int x = 1; x < cw; x++) {
            int i0 = (int)((double)(x-1) / cw * dlen); if (i0 >= dlen) i0 = dlen - 1;
            int i1 = (int)((double)x / cw * dlen);     if (i1 >= dlen) i1 = dlen - 1;
            int y0 = ch - 1 - (int)(((data[i0] - mn) / range) * (ch - 2));
            int y1 = ch - 1 - (int)(((data[i1] - mn) / range) * (ch - 2));
            c.DrawPointLine(x-1, y0, x, y1, pnl_color);
        }

        return canvas(std::move(c));
    };

    return vbox({
        hbox({text("P&L") | bold | color(foxml::peach),
              text(fmt("  $%+.2f", mx)) | color(foxml::dim)}),
        canvas_fn() | flex,
        hbox({text(fmt("  $%+.2f", mn)) | color(foxml::dim)}),
    }) | flex;
}

//======================================================================================================
// [CONTROLS WIDGET]
//======================================================================================================
static inline Element Widget_Controls() {
    return hbox({
        text("[q]") | color(foxml::pink), text("uit  ") | color(foxml::dim),
        text("[p]") | color(foxml::pink), text("ause  ") | color(foxml::dim),
        text("[r]") | color(foxml::pink), text("eload  ") | color(foxml::dim),
        text("[s]") | color(foxml::pink), text("witch regime  ") | color(foxml::dim),
        text("[l]") | color(foxml::pink), text("ayout") | color(foxml::dim),
    });
}

#endif // TUI_WIDGETS_HPP
