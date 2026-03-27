// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [NOTCURSES TUI]
//======================================================================================================
// terminal dashboard using notcurses — replaces FTXUI widgets + layouts
// each section is a standalone render function: (ncplane*, TUISnapshot*, row) → rows_used
// layout presets position sections on the standard plane
//
// separation of concerns:
//   TUINotcurses.hpp — rendering only, reads TUISnapshot, writes to ncplane
//   EngineTUI.hpp    — thread lifecycle, snapshot management, input handling
//   TUISnapshot      — data contract between engine and TUI (defined in EngineTUI.hpp)
//
// adding a new widget: write NC_Section_Foo(), call it from the layout functions
// adding a new layout: write NC_Layout_Foo(), add a case in NC_Layout_Render()
//======================================================================================================
#ifndef TUI_NOTCURSES_HPP
#define TUI_NOTCURSES_HPP

#include <notcurses/notcurses.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <clocale>

// TUISnapshot and TUIPositionSnap defined in EngineTUI.hpp (included before this)

//======================================================================================================
// [FOXML COLOR PALETTE]
//======================================================================================================
// warm forest tones — same RGB values as the FTXUI palette in TUIWidgets.hpp
//======================================================================================================
namespace nc_foxml {
    static inline void clay(struct ncplane *n)   { ncplane_set_fg_rgb8(n, 204, 142, 106); }
    static inline void wheat(struct ncplane *n)  { ncplane_set_fg_rgb8(n, 220, 198, 150); }
    static inline void mauve(struct ncplane *n)  { ncplane_set_fg_rgb8(n, 175, 135, 135); }
    static inline void sage(struct ncplane *n)   { ncplane_set_fg_rgb8(n, 180, 175, 140); }
    static inline void sand(struct ncplane *n)   { ncplane_set_fg_rgb8(n, 190, 170, 140); }
    static inline void peach(struct ncplane *n)  { ncplane_set_fg_rgb8(n, 230, 165, 120); }
    static inline void fg(struct ncplane *n)     { ncplane_set_fg_rgb8(n, 200, 190, 170); }
    static inline void dim(struct ncplane *n)    { ncplane_set_fg_rgb8(n, 120, 115, 105); }
    static inline void green(struct ncplane *n)  { ncplane_set_fg_rgb8(n, 140, 195, 130); }
    static inline void red(struct ncplane *n)    { ncplane_set_fg_rgb8(n, 210, 120, 120); }
    static inline void yellow(struct ncplane *n) { ncplane_set_fg_rgb8(n, 230, 200, 110); }
    static inline void pink(struct ncplane *n)   { ncplane_set_fg_rgb8(n, 210, 150, 170); }
    static inline void surf(struct ncplane *n)   { ncplane_set_fg_rgb8(n, 100, 100,  90); }

    static inline void pnl(struct ncplane *n, double v) {
        if (v >= 0) green(n); else red(n);
    }
    static inline void bold_on(struct ncplane *n)  { ncplane_set_styles(n, NCSTYLE_BOLD); }
    static inline void bold_off(struct ncplane *n) { ncplane_set_styles(n, 0); }
}

//======================================================================================================
// [HELPER — formatted putstr]
//======================================================================================================
static inline int nc_printf(struct ncplane *n, int y, int x, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static inline int nc_printf(struct ncplane *n, int y, int x, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return ncplane_putstr_yx(n, y, x, buf);
}

//======================================================================================================
// [SECTION: HEADER]
//======================================================================================================
static inline int NC_Section_Header(struct ncplane *n, const TUISnapshot *s, int y) {
    nc_foxml::surf(n);
    nc_printf(n, y++, 0, "════════════════════════════════════════════════════════════════");
    nc_foxml::bold_on(n); nc_foxml::peach(n);
    nc_printf(n, y++, 2, "/\\_/\\   FOXML TRADER");
    nc_printf(n, y, 2, "( o.o )  ");
    nc_foxml::wheat(n);
    nc_printf(n, y++, 11, "engine v3.0.27");
    nc_foxml::peach(n);
    nc_printf(n, y++, 2, " > ^ <");
    nc_foxml::bold_off(n);
    nc_foxml::surf(n);
    nc_printf(n, y++, 0, "════════════════════════════════════════════════════════════════");

    // state + uptime
    time_t now = time(NULL);
    unsigned elapsed = (unsigned)difftime(now, s->start_time);
    unsigned hours = elapsed / 3600, mins = (elapsed % 3600) / 60, secs = elapsed % 60;
    const char *state_str = (s->engine_state == 0) ? "WARMUP" :
                            (s->engine_state == 2) ? "CLOSING" : "ACTIVE";
    nc_foxml::sand(n);
    nc_printf(n, y, 0, "  STATE: ");
    nc_foxml::fg(n);
    nc_printf(n, y, 9, "%-8s", state_str);
    nc_foxml::dim(n);
    nc_printf(n, y, 17, "  |  ");
    nc_foxml::sand(n);
    nc_printf(n, y, 22, "UPTIME: ");
    nc_foxml::fg(n);
    nc_printf(n, y, 30, "%02u:%02u:%02u", hours, mins, secs);
    if (s->is_paused) {
        nc_foxml::dim(n);
        nc_printf(n, y, 40, "  |  ");
        nc_foxml::bold_on(n); nc_foxml::yellow(n);
        nc_printf(n, y, 45, "PAUSED");
        nc_foxml::bold_off(n);
    }
    y++;
    return y;
}

//======================================================================================================
// [SECTION: TOP BAR — key metrics at a glance]
//======================================================================================================
static inline int NC_Section_TopBar(struct ncplane *n, const TUISnapshot *s, int y) {
    nc_foxml::surf(n);
    nc_printf(n, y++, 0, "────────────────────────────────────────────────────────────────");

    // price
    nc_foxml::sand(n);
    nc_printf(n, y, 1, "PRICE ");
    nc_foxml::bold_on(n); nc_foxml::wheat(n);
    nc_printf(n, y, 7, "$%.2f", s->price);
    nc_foxml::bold_off(n);

    // P&L
    nc_foxml::dim(n);
    nc_printf(n, y, 22, "|");
    nc_foxml::sand(n);
    nc_printf(n, y, 24, "P&L ");
    nc_foxml::bold_on(n); nc_foxml::pnl(n, s->total_pnl);
    nc_printf(n, y, 28, "$%+.2f", s->total_pnl);
    nc_foxml::bold_off(n);

    // regime
    nc_foxml::dim(n);
    nc_printf(n, y, 42, "|");
    const char *regime_str = (s->current_regime == 1) ? "TREND" :
                             (s->current_regime == 2) ? "VOLAT" :
                             (s->current_regime == 3) ? "TR_DN" : "RANGE";
    if (s->current_regime == 1) nc_foxml::green(n);
    else if (s->current_regime == 2 || s->current_regime == 3) nc_foxml::red(n);
    else nc_foxml::dim(n);
    nc_foxml::bold_on(n);
    nc_printf(n, y, 44, "%s", regime_str);
    nc_foxml::bold_off(n);

    // positions
    nc_foxml::dim(n);
    nc_printf(n, y, 50, "|");
    nc_foxml::sand(n);
    nc_printf(n, y, 52, "POS ");
    nc_foxml::fg(n);
    nc_printf(n, y, 56, "%d/16", s->active_count);
    y++;

    nc_foxml::surf(n);
    nc_printf(n, y++, 0, "────────────────────────────────────────────────────────────────");
    return y;
}

//======================================================================================================
// [SECTION: MARKET STRUCTURE]
//======================================================================================================
static inline int NC_Section_Market(struct ncplane *n, const TUISnapshot *s, int y) {
    nc_foxml::bold_on(n); nc_foxml::peach(n);
    nc_printf(n, y++, 1, "MARKET STRUCTURE");
    nc_foxml::bold_off(n);

    nc_foxml::sand(n);
    nc_printf(n, y, 2, "avg: ");
    nc_foxml::fg(n);
    nc_printf(n, y, 7, "%.2f", s->roll_price_avg);
    nc_foxml::sand(n);
    nc_printf(n, y, 20, "stddev: ");
    nc_foxml::fg(n);
    nc_printf(n, y++, 28, "%.2f", s->roll_stddev);

    nc_foxml::sand(n);
    nc_printf(n, y, 2, "slope: ");
    const char *trend_c = (s->slope_pct > 0.001) ? "" : (s->slope_pct < -0.001) ? "" : "";
    if (s->slope_pct > 0.001) nc_foxml::green(n);
    else if (s->slope_pct < -0.001) nc_foxml::red(n);
    else nc_foxml::dim(n);
    nc_printf(n, y, 9, "%+.6f%%/tick", s->slope_pct);
    nc_foxml::sand(n);
    nc_printf(n, y, 28, "R²: ");
    nc_foxml::fg(n);
    nc_printf(n, y++, 32, "%.3f", s->short_r2);

    return y;
}

//======================================================================================================
// [SECTION: BUY GATE]
//======================================================================================================
static inline int NC_Section_BuyGate(struct ncplane *n, const TUISnapshot *s, int y) {
    nc_foxml::bold_on(n); nc_foxml::peach(n);
    nc_printf(n, y++, 1, "BUY GATE");
    nc_foxml::bold_off(n);

    nc_foxml::sand(n);
    nc_printf(n, y, 2, s->gate_direction ? "price >= " : "price <= ");
    nc_foxml::fg(n);
    nc_printf(n, y, 11, "%.2f", s->buy_p);
    nc_foxml::sand(n);
    nc_printf(n, y, 26, "dist: ");
    nc_foxml::fg(n);
    nc_printf(n, y++, 32, "%.2f (%.3f%%)", s->gate_dist, s->gate_dist_pct);

    nc_foxml::sand(n);
    nc_printf(n, y, 2, "spacing: ");
    nc_foxml::fg(n);
    nc_printf(n, y++, 11, "%.2f", s->spacing);

    return y;
}

//======================================================================================================
// [SECTION: PORTFOLIO]
//======================================================================================================
static inline int NC_Section_Portfolio(struct ncplane *n, const TUISnapshot *s, int y) {
    nc_foxml::bold_on(n); nc_foxml::peach(n);
    nc_printf(n, y++, 1, "PORTFOLIO");
    nc_foxml::bold_off(n);

    nc_foxml::sand(n);
    nc_printf(n, y, 2, "equity: ");
    nc_foxml::fg(n);
    nc_printf(n, y, 10, "$%.2f", s->equity);
    nc_foxml::sand(n);
    nc_printf(n, y, 26, "balance: ");
    nc_foxml::fg(n);
    nc_printf(n, y++, 35, "$%.2f", s->balance);

    nc_foxml::sand(n);
    nc_printf(n, y, 2, "exposure: ");
    nc_foxml::fg(n);
    nc_printf(n, y, 12, "%.1f%%/%.0f%%", s->exposure_pct, s->max_exp);
    nc_foxml::sand(n);
    nc_printf(n, y, 26, "fees: ");
    nc_foxml::fg(n);
    nc_printf(n, y++, 32, "$%.4f", s->fees);

    return y;
}

//======================================================================================================
// [SECTION: P&L]
//======================================================================================================
static inline int NC_Section_PnL(struct ncplane *n, const TUISnapshot *s, int y) {
    nc_foxml::bold_on(n); nc_foxml::peach(n);
    nc_printf(n, y++, 1, "P&L");
    nc_foxml::bold_off(n);

    nc_foxml::sand(n);
    nc_printf(n, y, 2, "realized: ");
    nc_foxml::pnl(n, s->realized);
    nc_printf(n, y, 12, "$%+.4f", s->realized);
    nc_foxml::sand(n);
    nc_printf(n, y, 26, "unrealized: ");
    nc_foxml::pnl(n, s->unrealized);
    nc_printf(n, y++, 38, "$%+.4f", s->unrealized);

    nc_foxml::sand(n);
    nc_printf(n, y, 2, "total: ");
    nc_foxml::bold_on(n); nc_foxml::pnl(n, s->total_pnl);
    nc_printf(n, y, 9, "$%+.4f", s->total_pnl);
    nc_foxml::bold_off(n);
    nc_foxml::dim(n);
    nc_printf(n, y++, 26, "(%+.2f%%)", s->return_pct);

    return y;
}

//======================================================================================================
// [SECTION: RISK]
//======================================================================================================
static inline int NC_Section_Risk(struct ncplane *n, const TUISnapshot *s, int y) {
    nc_foxml::bold_on(n); nc_foxml::peach(n);
    nc_printf(n, y++, 1, "RISK");
    nc_foxml::bold_off(n);

    nc_foxml::sand(n);
    nc_printf(n, y, 2, "risk/pos: ");
    nc_foxml::fg(n);
    nc_printf(n, y, 12, "%.1f%%", (double)s->risk_amt);
    nc_foxml::dim(n);
    nc_printf(n, y, 20, "|");
    nc_foxml::sand(n);
    nc_printf(n, y, 22, "breaker: ");
    if (s->breaker_tripped) { nc_foxml::red(n); nc_printf(n, y, 31, "TRIPPED"); }
    else { nc_foxml::green(n); nc_printf(n, y, 31, "OK"); }
    y++;

    const char *strat = (s->strategy_id == 1) ? "MOMENTUM" : "MEAN REVERSION";
    const char *regime = (s->current_regime == 1) ? "TRENDING" :
                         (s->current_regime == 2) ? "VOLATILE" :
                         (s->current_regime == 3) ? "TRENDING_DOWN" : "RANGING";
    nc_foxml::sand(n);
    nc_printf(n, y, 2, "strategy: ");
    nc_foxml::bold_on(n); nc_foxml::fg(n);
    nc_printf(n, y, 12, "%s", strat);
    nc_foxml::bold_off(n);
    nc_foxml::dim(n);
    nc_printf(n, y, 28, "|");
    if (s->current_regime == 1) nc_foxml::green(n);
    else if (s->current_regime == 2 || s->current_regime == 3) nc_foxml::red(n);
    else nc_foxml::dim(n);
    nc_printf(n, y, 30, "%s", regime);
    nc_foxml::dim(n);
    nc_printf(n, y++, 40, "(%.0fm)", s->regime_duration_min);

    return y;
}

//======================================================================================================
// [SECTION: STATS]
//======================================================================================================
static inline int NC_Section_Stats(struct ncplane *n, const TUISnapshot *s, int y) {
    nc_foxml::bold_on(n); nc_foxml::peach(n);
    nc_printf(n, y++, 1, "STATS");
    nc_foxml::bold_off(n);

    uint32_t total_exits = s->wins + s->losses;
    nc_foxml::sand(n);
    nc_printf(n, y, 2, "buys: ");
    nc_foxml::fg(n);
    nc_printf(n, y, 8, "%-4u", s->total_buys);
    nc_foxml::sand(n);
    nc_printf(n, y, 14, "exits: ");
    nc_foxml::fg(n);
    nc_printf(n, y, 21, "%-4u", total_exits);
    nc_foxml::sand(n);
    nc_printf(n, y, 27, "W: ");
    nc_foxml::green(n);
    nc_printf(n, y, 30, "%u", s->wins);
    nc_foxml::sand(n);
    nc_printf(n, y, 34, "L: ");
    nc_foxml::red(n);
    nc_printf(n, y++, 37, "%u", s->losses);

    nc_foxml::sand(n);
    nc_printf(n, y, 2, "rate: ");
    nc_foxml::pnl(n, s->win_rate - 50.0);
    nc_printf(n, y, 8, "%.1f%%", s->win_rate);
    nc_foxml::sand(n);
    nc_printf(n, y, 16, "pf: ");
    nc_foxml::pnl(n, s->profit_factor - 1.0);
    nc_printf(n, y, 20, "%.2f", s->profit_factor);
    nc_foxml::sand(n);
    nc_printf(n, y, 28, "avg W: ");
    nc_foxml::green(n);
    nc_printf(n, y, 35, "$%.2f", s->avg_win);
    nc_foxml::sand(n);
    nc_printf(n, y, 44, "L: ");
    nc_foxml::red(n);
    nc_printf(n, y++, 47, "$%.2f", s->avg_loss);

    return y;
}

//======================================================================================================
// [SECTION: POSITIONS]
//======================================================================================================
static inline int NC_Section_Positions(struct ncplane *n, const TUISnapshot *s, int y, int max_rows) {
    nc_foxml::bold_on(n); nc_foxml::peach(n);
    nc_printf(n, y++, 1, "POSITIONS (%d/16)", s->active_count);
    nc_foxml::bold_off(n);

    int displayed = 0;
    for (int i = 0; i < 16 && (y + 2) < max_rows; i++) {
        const TUIPositionSnap *ps = &s->positions[i];
        if (ps->idx < 0) continue;

        double diff = s->price - ps->entry;

        nc_foxml::wheat(n);
        nc_printf(n, y, 1, "#%-2d", displayed);
        nc_foxml::fg(n);
        nc_printf(n, y, 5, "$%.0f", ps->entry);
        nc_foxml::dim(n);
        nc_printf(n, y, 13, "→");
        nc_foxml::wheat(n);
        nc_printf(n, y, 15, "$%.0f", s->price);
        nc_foxml::pnl(n, diff);
        nc_printf(n, y, 23, "%+.0f", diff);

        nc_foxml::green(n);
        nc_printf(n, y, 30, "TP:$%.0f", ps->tp);
        nc_foxml::red(n);
        nc_printf(n, y, 42, "SL:$%.0f", ps->sl);

        nc_foxml::dim(n);
        nc_printf(n, y, 53, "%.0fm", ps->hold_minutes);
        y++;
        displayed++;
    }

    if (displayed == 0) {
        nc_foxml::dim(n);
        nc_printf(n, y++, 3, "(no positions)");
    }

    return y;
}

//======================================================================================================
// [SECTION: CONTROLS]
//======================================================================================================
static inline int NC_Section_Controls(struct ncplane *n, int y) {
    nc_foxml::pink(n);
    nc_printf(n, y, 1, "[q]");
    nc_foxml::dim(n);
    nc_printf(n, y, 4, "uit  ");
    nc_foxml::pink(n);
    nc_printf(n, y, 9, "[p]");
    nc_foxml::dim(n);
    nc_printf(n, y, 12, "ause  ");
    nc_foxml::pink(n);
    nc_printf(n, y, 18, "[r]");
    nc_foxml::dim(n);
    nc_printf(n, y, 21, "eload  ");
    nc_foxml::pink(n);
    nc_printf(n, y, 28, "[s]");
    nc_foxml::dim(n);
    nc_printf(n, y, 31, "witch  ");
    nc_foxml::pink(n);
    nc_printf(n, y, 38, "[l]");
    nc_foxml::dim(n);
    nc_printf(n, y++, 41, "ayout");
    return y;
}

//======================================================================================================
// [LAYOUT DEFINITIONS]
//======================================================================================================
#define NC_LAYOUT_STANDARD  0
#define NC_LAYOUT_CHARTS    1
#define NC_LAYOUT_COMPACT   2
#define NC_LAYOUT_COUNT     3

//======================================================================================================
// [LAYOUT: STANDARD — full dashboard]
//======================================================================================================
static inline void NC_Layout_Standard(struct ncplane *n, const TUISnapshot *s,
                                       struct ncdplot *price_plot, struct ncdplot *pnl_plot,
                                       unsigned term_h, unsigned term_w) {
    int y = 0;
    y = NC_Section_Header(n, s, y);
    y = NC_Section_TopBar(n, s, y);
    y = NC_Section_Market(n, s, y);

    nc_foxml::surf(n);
    nc_printf(n, y++, 0, "────────────────────────────────────────────────────────────────");

    y = NC_Section_BuyGate(n, s, y);

    nc_foxml::surf(n);
    nc_printf(n, y++, 0, "────────────────────────────────────────────────────────────────");

    y = NC_Section_Portfolio(n, s, y);

    nc_foxml::surf(n);
    nc_printf(n, y++, 0, "────────────────────────────────────────────────────────────────");

    y = NC_Section_PnL(n, s, y);

    nc_foxml::surf(n);
    nc_printf(n, y++, 0, "────────────────────────────────────────────────────────────────");

    y = NC_Section_Risk(n, s, y);

    nc_foxml::surf(n);
    nc_printf(n, y++, 0, "────────────────────────────────────────────────────────────────");

    y = NC_Section_Positions(n, s, y, (int)term_h - 4);

    nc_foxml::surf(n);
    nc_printf(n, y++, 0, "────────────────────────────────────────────────────────────────");

    y = NC_Section_Stats(n, s, y);

    nc_foxml::surf(n);
    nc_printf(n, (int)term_h - 2, 0, "────────────────────────────────────────────────────────────────");
    NC_Section_Controls(n, (int)term_h - 1);

    (void)price_plot; (void)pnl_plot; (void)term_w;
}

//======================================================================================================
// [LAYOUT: CHARTS — focus on price + P&L charts]
//======================================================================================================
static inline void NC_Layout_Charts(struct ncplane *n, const TUISnapshot *s,
                                     struct ncdplot *price_plot, struct ncdplot *pnl_plot,
                                     unsigned term_h, unsigned term_w) {
    // top bar with key metrics
    int y = 0;
    nc_foxml::bold_on(n); nc_foxml::wheat(n);
    nc_printf(n, y, 1, "$%.2f", s->price);
    nc_foxml::bold_off(n);
    nc_foxml::dim(n);
    nc_printf(n, y, 14, "|");
    nc_foxml::bold_on(n); nc_foxml::pnl(n, s->total_pnl);
    nc_printf(n, y, 16, "$%+.2f", s->total_pnl);
    nc_foxml::bold_off(n);
    nc_foxml::dim(n);
    nc_printf(n, y, 28, "|");
    if (s->current_regime == 1) nc_foxml::green(n);
    else if (s->current_regime == 2 || s->current_regime == 3) nc_foxml::red(n);
    else nc_foxml::dim(n);
    const char *regime = (s->current_regime == 1) ? "TREND" :
                         (s->current_regime == 2) ? "VOLAT" :
                         (s->current_regime == 3) ? "TR_DN" : "RANGE";
    nc_printf(n, y, 30, "%s", regime);
    nc_foxml::dim(n);
    nc_printf(n, y, 36, "|");
    nc_foxml::sand(n);
    nc_printf(n, y++, 38, "POS %d/16", s->active_count);

    nc_foxml::surf(n);
    nc_printf(n, y++, 0, "────────────────────────────────────────────────────────────────");

    // charts rendered by notcurses ncdplot on their own planes (positioned by caller)
    // positions below charts
    int chart_bottom = (int)term_h - 8;
    y = chart_bottom;

    nc_foxml::surf(n);
    nc_printf(n, y++, 0, "────────────────────────────────────────────────────────────────");

    y = NC_Section_Positions(n, s, y, (int)term_h - 2);

    nc_foxml::surf(n);
    nc_printf(n, (int)term_h - 2, 0, "────────────────────────────────────────────────────────────────");
    NC_Section_Controls(n, (int)term_h - 1);

    (void)price_plot; (void)pnl_plot; (void)term_w;
}

//======================================================================================================
// [LAYOUT: COMPACT — minimal metrics only]
//======================================================================================================
static inline void NC_Layout_Compact(struct ncplane *n, const TUISnapshot *s,
                                      struct ncdplot *price_plot, struct ncdplot *pnl_plot,
                                      unsigned term_h, unsigned term_w) {
    int y = 0;
    y = NC_Section_TopBar(n, s, y);
    y = NC_Section_Positions(n, s, y, (int)term_h - 6);

    nc_foxml::surf(n);
    nc_printf(n, y++, 0, "────────────────────────────────────────────────────────────────");
    y = NC_Section_Stats(n, s, y);

    nc_foxml::surf(n);
    nc_printf(n, (int)term_h - 2, 0, "────────────────────────────────────────────────────────────────");
    NC_Section_Controls(n, (int)term_h - 1);

    (void)price_plot; (void)pnl_plot; (void)term_w;
}

//======================================================================================================
// [LAYOUT DISPATCH]
//======================================================================================================
static inline void NC_Layout_Render(struct ncplane *n, const TUISnapshot *s,
                                     struct ncdplot *price_plot, struct ncdplot *pnl_plot,
                                     int layout_id, unsigned term_h, unsigned term_w) {
    switch (layout_id) {
        case NC_LAYOUT_CHARTS:
            NC_Layout_Charts(n, s, price_plot, pnl_plot, term_h, term_w);
            break;
        case NC_LAYOUT_COMPACT:
            NC_Layout_Compact(n, s, price_plot, pnl_plot, term_h, term_w);
            break;
        default:
            NC_Layout_Standard(n, s, price_plot, pnl_plot, term_h, term_w);
            break;
    }
}

//======================================================================================================
// [CHART MANAGEMENT]
//======================================================================================================
// creates ncdplot instances on child planes for price and P&L charts
// call once at init and on terminal resize
//======================================================================================================
struct NCCharts {
    struct ncplane *price_plane;
    struct ncplane *pnl_plane;
    struct ncdplot *price_plot;
    struct ncdplot *pnl_plot;
};

static inline NCCharts NC_Charts_Create(struct ncplane *parent, unsigned term_h, unsigned term_w) {
    NCCharts charts = {};

    // price chart: upper half of chart area (below header, rows 2-ish to midpoint)
    unsigned chart_top = 2;  // below top bar in charts layout
    unsigned chart_mid = chart_top + (term_h - chart_top - 8) / 2;
    unsigned chart_bot = term_h - 8;
    unsigned price_h = chart_mid - chart_top;
    unsigned pnl_h = chart_bot - chart_mid;

    if (price_h < 4 || pnl_h < 4 || term_w < 20) return charts;

    // price chart plane
    ncplane_options price_opts = {};
    price_opts.y = chart_top;
    price_opts.x = 0;
    price_opts.rows = price_h;
    price_opts.cols = term_w;
    charts.price_plane = ncplane_create(parent, &price_opts);

    if (charts.price_plane) {
        ncplot_options plot_opts = {};
        plot_opts.gridtype = NCBLIT_BRAILLE;
        plot_opts.rangex = 120; // match TUISnapshot::GRAPH_LEN
        // wheat color for price
        ncchannels_set_fg_rgb8(&plot_opts.maxchannels, 220, 198, 150);
        ncchannels_set_fg_rgb8(&plot_opts.minchannels, 180, 160, 120);
        charts.price_plot = ncdplot_create(charts.price_plane, &plot_opts, 0, 0);
    }

    // P&L chart plane
    ncplane_options pnl_opts = {};
    pnl_opts.y = chart_mid;
    pnl_opts.x = 0;
    pnl_opts.rows = pnl_h;
    pnl_opts.cols = term_w;
    charts.pnl_plane = ncplane_create(parent, &pnl_opts);

    if (charts.pnl_plane) {
        ncplot_options plot_opts = {};
        plot_opts.gridtype = NCBLIT_BRAILLE;
        plot_opts.rangex = 120;
        // green/red based on total P&L (set at render time)
        ncchannels_set_fg_rgb8(&plot_opts.maxchannels, 140, 195, 130);
        ncchannels_set_fg_rgb8(&plot_opts.minchannels, 140, 195, 130);
        charts.pnl_plot = ncdplot_create(charts.pnl_plane, &plot_opts, 0, 0);
    }

    return charts;
}

static inline void NC_Charts_Destroy(NCCharts *charts) {
    if (charts->price_plot) ncdplot_destroy(charts->price_plot);
    if (charts->pnl_plot) ncdplot_destroy(charts->pnl_plot);
    if (charts->price_plane) ncplane_destroy(charts->price_plane);
    if (charts->pnl_plane) ncplane_destroy(charts->pnl_plane);
    *charts = {};
}

// feed snapshot data into the charts
static inline void NC_Charts_Update(NCCharts *charts, const TUISnapshot *s) {
    if (!charts->price_plot || !charts->pnl_plot) return;

    int len = s->graph_count;
    if (len < 2) return;

    int start = (s->graph_head - len + TUISnapshot::GRAPH_LEN) % TUISnapshot::GRAPH_LEN;
    for (int i = 0; i < len; i++) {
        int idx = (start + i) % TUISnapshot::GRAPH_LEN;
        ncdplot_add_sample(charts->price_plot, (uint64_t)i, s->price_history[idx]);
        ncdplot_add_sample(charts->pnl_plot, (uint64_t)i, s->pnl_history[idx]);
    }
}

#endif // TUI_NOTCURSES_HPP
