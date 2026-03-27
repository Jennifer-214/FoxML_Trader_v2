// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ANSI TUI]
//======================================================================================================
// terminal dashboard using raw ANSI escape codes — zero library dependencies
// each section is a standalone render function: (AnsiBuf*, TUISnapshot*, row, width) → next row
// layout presets compose sections into full-screen layouts
//
// design for transparent terminals: foreground-only colors, no background fill
// uses synchronized output protocol (\033[?2026h/l) to prevent flicker
//
// separation of concerns:
//   TUIAnsi.hpp     — rendering only, reads TUISnapshot, writes ANSI to buffer
//   EngineTUI.hpp   — thread lifecycle, snapshot management, input handling
//   TUISnapshot     — data contract between engine and TUI (defined in EngineTUI.hpp)
//
// adding a new widget: write ANSI_Section_Foo(), call it from the layout functions
// adding a new layout: write ANSI_Layout_Foo(), add a case in ANSI_Layout_Render()
//======================================================================================================
#ifndef TUI_ANSI_HPP
#define TUI_ANSI_HPP

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <cmath>
#include <unistd.h>

// TUISnapshot and TUIPositionSnap defined in EngineTUI.hpp (included before this)

//======================================================================================================
// [FOXML COLOR PALETTE — ANSI truecolor]
//======================================================================================================
// warm forest tones — RGB values from the notcurses palette (TUINotcurses.hpp)
// uses 24-bit truecolor: \033[38;2;R;G;Bm (foreground only, transparent background)
//======================================================================================================
#define A_RESET   "\033[0m"
#define A_BOLD    "\033[1m"

#define A_CLAY    "\033[38;2;204;142;106m"   // warm terracotta — secondary accent
#define A_WHEAT   "\033[38;2;220;198;150m"   // warm gold — price, primary data
#define A_MAUVE   "\033[38;2;175;135;135m"   // dusty rose — unused, future
#define A_SAGE    "\033[38;2;180;175;140m"   // muted olive — unused, future
#define A_SAND    "\033[38;2;190;170;140m"   // warm tan — labels
#define A_PEACH   "\033[38;2;230;165;120m"   // warm orange — section titles
#define A_FG      "\033[38;2;200;190;170m"   // warm cream — normal text / values
#define A_DIM     "\033[38;2;120;115;105m"   // warm gray — hints, secondary info
#define A_GREEN   "\033[38;2;140;195;130m"   // forest green — positive P&L, OK
#define A_RED     "\033[38;2;210;120;120m"   // muted red — negative P&L, alerts
#define A_YELLOW  "\033[38;2;230;200;110m"   // warm amber — warnings, paused
#define A_PINK    "\033[38;2;210;150;170m"   // dusty pink — keybind highlights
#define A_SURF    "\033[38;2;100;100;90m"    // dark olive — dividers, separators

#define A_PNL(v) ((v) >= 0.0 ? A_GREEN : A_RED)

//======================================================================================================
// [RENDER BUFFER]
//======================================================================================================
// frame buffer — build entire screen content, flush in one write() with sync output
// prevents flicker and minimizes syscalls
//======================================================================================================
struct AnsiBuf {
    char data[65536];
    int len;
    int last_row;  // tracks current row for deferred \033[K (erase-to-end-of-line)
};

static inline void ab_clear(AnsiBuf *ab) { ab->len = 0; }

static inline void ab_append(AnsiBuf *ab, const char *s) {
    int n = (int)strlen(s);
    if (ab->len + n < (int)sizeof(ab->data)) {
        memcpy(ab->data + ab->len, s, n);
        ab->len += n;
    }
}

static inline void ab_appendn(AnsiBuf *ab, const char *s, int n) {
    if (ab->len + n < (int)sizeof(ab->data)) {
        memcpy(ab->data + ab->len, s, n);
        ab->len += n;
    }
}

__attribute__((format(printf, 2, 3)))
static inline void ab_printf(AnsiBuf *ab, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int remaining = (int)sizeof(ab->data) - ab->len;
    if (remaining <= 0) { va_end(ap); return; }
    int n = vsnprintf(ab->data + ab->len, remaining, fmt, ap);
    va_end(ap);
    if (n > 0) ab->len += (n < remaining) ? n : remaining - 1;
}

static inline void ab_flush(AnsiBuf *ab) {
    write(STDOUT_FILENO, ab->data, ab->len);
}

//======================================================================================================
// [HELPERS]
//======================================================================================================
// cursor positioning, divider lines, visual indicators
//======================================================================================================

// move cursor to (row, col) — overwrites in place, no pre-clear
// deferred \033[K erases tail of PREVIOUS row when starting a new row
// columns 1 to col-1 are filled with spaces (overwrites stale left margin)
// only call once per row — use \033[{col}G (CHA) for second positioning on same row
static inline void ab_goto(AnsiBuf *ab, int row, int col) {
    // erase tail of previous row (cursor is still on that row)
    if (ab->last_row >= 0) ab_append(ab, "\033[K");
    ab->last_row = row;
    // move to column 1 of target row, pad with spaces to target column
    ab_printf(ab, "\033[%d;1H", row);
    for (int i = 1; i < col; i++) ab_appendn(ab, " ", 1);
}

// clear an empty/gap row — uses \033[2K since there's no content to preserve
static inline void ab_clear_row(AnsiBuf *ab, int row) {
    if (ab->last_row >= 0) ab_append(ab, "\033[K");
    ab->last_row = -1;
    ab_printf(ab, "\033[%d;1H\033[2K", row);
}

// position cursor for right-column overlay — does NOT clear or pad
// does NOT interact with ab_goto/last_row tracking
static inline void ab_goto_right(AnsiBuf *ab, int row, int col) {
    ab_printf(ab, "\033[%d;%dH", row, col);
}

// horizontal divider line (heavy = ═, normal = ─)
static inline void ab_divider(AnsiBuf *ab, int y, int w, bool heavy) {
    int dw = (w > 200) ? 200 : w;
    ab_goto(ab, y, 1);
    ab_append(ab, A_SURF);
    const char *ch = heavy ? "═" : "─";
    int ch_len = (int)strlen(ch);
    for (int i = 0; i < dw; i++) ab_appendn(ab, ch, ch_len);
    ab_append(ab, A_RESET);
}

// R² visual bar: ████░░ (value 0.0-1.0 mapped to width chars)
static inline void ab_r2_bar(AnsiBuf *ab, double r2, int width) {
    int filled = (int)(r2 * width + 0.5);
    if (filled > width) filled = width;
    if (filled < 0) filled = 0;
    ab_append(ab, A_WHEAT);
    for (int i = 0; i < filled; i++) ab_append(ab, "█");
    ab_append(ab, A_SURF);
    for (int i = filled; i < width; i++) ab_append(ab, "░");
    ab_append(ab, A_RESET);
}

// half-block area chart — 2× vertical resolution vs sparkline, solid and readable
// each terminal cell has upper half (▀) and lower half (▄) = 2 sub-rows per row
// height_chars rows = height_chars*2 vertical levels (e.g. 3 rows = 6 levels)
// area fill from bottom to data point, per-column coloring
// color_by_sign: 0 = color by trend (rising=up, falling=dn), 1 = color by value sign (>=0=up, <0=dn)
// start_y/start_x: absolute terminal position (ANSI TUI uses ab_goto, not newlines)
static inline void ab_halfblock_chart(AnsiBuf *ab, const double *data, int head,
                                       int count, int max_len, int width,
                                       int height_chars, int start_y, int start_x,
                                       const char *up_color, const char *dn_color,
                                       const char *flat_color,
                                       int color_by_sign = 0) {
    if (count < 2 || width < 4 || height_chars < 1) return;

    int vis = (count < width) ? count : width;
    int start_offset = count - vis;
    int start_idx = (head - count + max_len) % max_len;

    // find min/max
    double vmin = 1e18, vmax = -1e18;
    for (int i = 0; i < vis; i++) {
        int idx = (start_idx + start_offset + i) % max_len;
        if (data[idx] < vmin) vmin = data[idx];
        if (data[idx] > vmax) vmax = data[idx];
    }
    double range = vmax - vmin;
    int sub_rows = height_chars * 2; // 2 sub-rows per terminal row

    // compute fill height and color signal for each column
    int col_level[120];
    double col_signal[120]; // trend delta or raw value (depending on color_by_sign)
    if (vis > 120) vis = 120;

    for (int c = 0; c < vis; c++) {
        int idx = (start_idx + start_offset + c) % max_len;
        double v = data[idx];
        if (range < 1e-10)
            col_level[c] = sub_rows / 2;
        else {
            col_level[c] = (int)((v - vmin) / range * (sub_rows - 1));
            if (col_level[c] < 0) col_level[c] = 0;
            if (col_level[c] >= sub_rows) col_level[c] = sub_rows - 1;
        }
        if (color_by_sign) {
            col_signal[c] = v; // color by value sign: >= 0 = up_color, < 0 = dn_color
        } else {
            // trend: compare to 5 columns back (smooths out tick-to-tick noise)
            col_signal[c] = 0.0;
            int lookback = (c >= 5) ? 5 : c;
            if (lookback > 0) {
                int prev_idx = (start_idx + start_offset + c - lookback) % max_len;
                col_signal[c] = v - data[prev_idx];
            }
        }
    }

    // render row by row, top to bottom
    // terminal row r corresponds to sub-rows: upper = (height_chars-1-r)*2+1, lower = (height_chars-1-r)*2
    for (int r = 0; r < height_chars; r++) {
        ab_goto(ab, start_y + r, start_x);
        int upper_sub = (height_chars - 1 - r) * 2 + 1; // which sub-row is the upper half
        int lower_sub = (height_chars - 1 - r) * 2;     // which sub-row is the lower half

        for (int c = 0; c < vis; c++) {
            const char *color = flat_color;
            if (col_signal[c] > 1e-10) color = up_color;
            else if (col_signal[c] < -1e-10) color = dn_color;
            ab_append(ab, color);

            int fill = col_level[c]; // filled up to this sub-row (inclusive)
            int has_upper = (fill >= upper_sub);
            int has_lower = (fill >= lower_sub);

            if (has_upper && has_lower)
                ab_append(ab, "\xe2\x96\x88"); // █ full block
            else if (has_lower)
                ab_append(ab, "\xe2\x96\x84"); // ▄ lower half
            else
                ab_append(ab, " ");
        }
        ab_append(ab, A_RESET);
    }
}

// sparkline chart from ring buffer data (▁▂▃▄▅▆▇█)
static inline void ab_sparkline(AnsiBuf *ab, const double *data, int head,
                                 int count, int max_len, int width,
                                 const char *color) {
    if (count < 2) return;

    int vis = (count < width) ? count : width;
    int start_offset = count - vis;
    int start = (head - count + max_len) % max_len;

    // find min/max for normalization
    double vmin = 1e18, vmax = -1e18;
    for (int i = 0; i < vis; i++) {
        int idx = (start + start_offset + i) % max_len;
        if (data[idx] < vmin) vmin = data[idx];
        if (data[idx] > vmax) vmax = data[idx];
    }
    double range = vmax - vmin;

    // flat data → mid-height line
    if (range < 1e-10) {
        ab_append(ab, color);
        for (int i = 0; i < vis; i++) ab_append(ab, "▄");
        ab_append(ab, A_RESET);
        return;
    }

    static const char *blocks[] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };

    ab_append(ab, color);
    for (int i = 0; i < vis; i++) {
        int idx = (start + start_offset + i) % max_len;
        int level = (int)((data[idx] - vmin) / range * 7.0);
        if (level < 0) level = 0;
        if (level > 7) level = 7;
        ab_append(ab, blocks[level]);
    }
    ab_append(ab, A_RESET);
}

// P&L sparkline — each bar colored green/red based on that value's sign
static inline void ab_sparkline_pnl(AnsiBuf *ab, const double *data, int head,
                                     int count, int max_len, int width) {
    if (count < 2) return;

    int vis = (count < width) ? count : width;
    int start_offset = count - vis;
    int start = (head - count + max_len) % max_len;

    double vmin = 1e18, vmax = -1e18;
    for (int i = 0; i < vis; i++) {
        int idx = (start + start_offset + i) % max_len;
        if (data[idx] < vmin) vmin = data[idx];
        if (data[idx] > vmax) vmax = data[idx];
    }
    double range = vmax - vmin;

    if (range < 1e-10) {
        const char *c = (vmin >= 0.0) ? A_GREEN : A_RED;
        ab_append(ab, c);
        for (int i = 0; i < vis; i++) ab_append(ab, "▄");
        ab_append(ab, A_RESET);
        return;
    }

    static const char *blocks[] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
    for (int i = 0; i < vis; i++) {
        int idx = (start + start_offset + i) % max_len;
        ab_append(ab, data[idx] >= 0.0 ? A_GREEN : A_RED);
        int level = (int)((data[idx] - vmin) / range * 7.0);
        if (level < 0) level = 0;
        if (level > 7) level = 7;
        ab_append(ab, blocks[level]);
    }
    ab_append(ab, A_RESET);
}

//======================================================================================================
// [SECTION: HEADER]
//======================================================================================================
static inline int ANSI_Section_Header(AnsiBuf *ab, const TUISnapshot *s,
                                       int y, int w, uint64_t start_time) {
    ab_divider(ab, y++, w, true);

    // kaomoji fox + title (use CHA \033[G for second position on same row)
    ab_goto(ab, y, 4);
    ab_printf(ab, A_BOLD A_PEACH "/l、" A_RESET);
    ab_printf(ab, "\033[20G" A_BOLD A_PEACH "FOXML TRADER" A_RESET);
    y++;
    ab_goto(ab, y, 3);
    ab_printf(ab, A_BOLD A_PEACH "( °_ ° 7" A_RESET);
    ab_printf(ab, "\033[20G" A_WHEAT "engine v3.0.27" A_RESET);
    y++;
    ab_goto(ab, y, 4);
    ab_printf(ab, A_BOLD A_PEACH "ド  ヘ" A_RESET);
    y++;
    ab_goto(ab, y, 4);
    ab_printf(ab, A_BOLD A_PEACH "じし_,)ノ" A_RESET);
    y++;

    ab_divider(ab, y++, w, true);

    // state + uptime
    time_t now = time(NULL);
    unsigned elapsed = (unsigned)difftime(now, (time_t)start_time);
    unsigned hours = elapsed / 3600, mins = (elapsed % 3600) / 60, secs = elapsed % 60;
    const char *state_str = (s->engine_state == 0) ? "WARMUP" :
                            (s->engine_state == 2) ? "CLOSING" : "ACTIVE";

    ab_goto(ab, y, 2);
    if (s->live_trading)
        ab_printf(ab, A_BOLD "\033[31m LIVE " A_RESET);
    else
        ab_printf(ab, A_DIM " PAPER" A_RESET);
    ab_printf(ab, A_SAND " STATE: " A_FG "%-8s" A_DIM "  │  "
              A_SAND "UPTIME: " A_FG "%02u:%02u:%02u",
              state_str, hours, mins, secs);
    if (s->is_paused)
        ab_printf(ab, A_DIM "  │  " A_BOLD A_YELLOW "PAUSED" A_RESET);
    ab_append(ab, A_RESET);
    y++;

    // trading blocked indicator — show reason when buy gate is disabled
    if (s->engine_state == 0) {
        ab_goto(ab, y, 2);
        ab_printf(ab, A_BOLD A_YELLOW " ▌ BUYING PAUSED" A_DIM "  warmup — waiting for market data (%d/%d samples)" A_RESET,
                  s->roll_count, s->min_warmup_samples);
        y++;
    } else if (s->sl_cooldown > 0) {
        ab_goto(ab, y, 2);
        ab_printf(ab, A_BOLD A_YELLOW " ▌ BUYING PAUSED" A_DIM "  post-SL cooldown (%d cycles remaining)" A_RESET,
                  s->sl_cooldown);
        y++;
    } else if (s->current_regime == 2 || s->current_regime == 3) {  // VOLATILE or TRENDING_DOWN
        ab_goto(ab, y, 2);
        const char *pause_reason = (s->current_regime == 3) ? "downtrend — buying paused" : "volatile regime — buying paused";
        ab_printf(ab, A_BOLD A_YELLOW " ▌ BUYING PAUSED" A_DIM "  %s" A_RESET, pause_reason);
        y++;
    } else if (s->breaker_tripped) {
        ab_goto(ab, y, 2);
        ab_printf(ab, A_BOLD A_RED " ▌ BUYING PAUSED" A_DIM "  circuit breaker — max drawdown hit" A_RESET);
        y++;
    }

    return y;
}

//======================================================================================================
// [SECTION: TOP BAR — key metrics at a glance]
//======================================================================================================
static inline int ANSI_Section_TopBar(AnsiBuf *ab, const TUISnapshot *s, int y, int w) {
    ab_divider(ab, y++, w, false);

    ab_goto(ab, y, 2);
    ab_printf(ab, A_SAND " PRICE " A_BOLD A_WHEAT "$%.2f" A_RESET, s->price);
    ab_printf(ab, A_DIM "  │ " A_SAND "P&L " A_BOLD "%s$%+.2f" A_RESET,
              A_PNL(s->total_pnl), s->total_pnl);

    const char *regime_color = (s->current_regime == 1) ? A_GREEN :
                               (s->current_regime == 2 || s->current_regime == 3) ? A_RED : A_DIM;
    const char *regime_str = (s->current_regime == 1) ? "TREND" :
                             (s->current_regime == 2) ? "VOLAT" :
                             (s->current_regime == 3) ? "TR_DN" : "RANGE";
    ab_printf(ab, A_DIM "  │ " A_BOLD "%s%s" A_RESET, regime_color, regime_str);
    ab_printf(ab, A_DIM "  │ " A_SAND "POS " A_FG "%d/%d" A_RESET, s->active_count, s->max_positions);
    uint32_t total_exits = s->wins + s->losses;
    if (total_exits > 0) {
        ab_printf(ab, A_DIM "  │ " A_GREEN "W:%u " A_RED "L:%u "
                  "%s%.0f%%" A_RESET,
                  s->wins, s->losses,
                  (s->win_rate >= 50.0) ? A_GREEN : A_RED, s->win_rate);
    }
    y++;

    ab_divider(ab, y++, w, false);
    return y;
}

//======================================================================================================
// [SECTION: MARKET STRUCTURE]
//======================================================================================================
static inline int ANSI_Section_Market(AnsiBuf *ab, const TUISnapshot *s, int y, int w) {
    ab_goto(ab, y, 2);
    ab_printf(ab, A_BOLD A_PEACH " MARKET STRUCTURE" A_RESET);
    y++;

    // avg + stddev + R² with visual bar
    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "avg: " A_FG "%.2f" A_DIM "     "
              A_SAND "stddev: " A_FG "%.2f" A_DIM "     "
              A_SAND "R²: " A_FG "%.3f ",
              s->roll_price_avg, s->roll_stddev, s->short_r2);
    ab_r2_bar(ab, s->short_r2, 6);
    y++;

    // short window slope + direction arrow
    const char *trend_arrow = (s->slope_pct > 0.001) ? "▲" :
                              (s->slope_pct < -0.001) ? "▼" : "▸";
    const char *trend_color = (s->slope_pct > 0.001) ? A_GREEN :
                              (s->slope_pct < -0.001) ? A_RED : A_DIM;
    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "slope: " "%s%+.6f%%/tick " A_RESET, trend_color, s->slope_pct);
    ab_printf(ab, "%s%s" A_RESET, trend_color, trend_arrow);
    y++;

    // long window slope
    const char *lt_arrow = (s->long_slope_pct > 0.001) ? "▲" :
                           (s->long_slope_pct < -0.001) ? "▼" : "▸";
    const char *lt_color = (s->long_slope_pct > 0.001) ? A_GREEN :
                           (s->long_slope_pct < -0.001) ? A_RED : A_DIM;
    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "long:  " "%s%+.6f%%/tick" A_RESET
              A_DIM " (%d-tick)  " "%s%s" A_RESET,
              lt_color, s->long_slope_pct, s->long_count, lt_color, lt_arrow);
    y++;

    return y;
}

//======================================================================================================
// [SECTION: REGIME SIGNALS]
//======================================================================================================
// displays all regime detection inputs — the signals that drive Regime_Classify
// new section: shows R², vol_ratio, ror_slope that weren't in previous TUI backends
//======================================================================================================
static inline int ANSI_Section_Regime(AnsiBuf *ab, const TUISnapshot *s, int y, int w) {
    ab_goto(ab, y, 2);
    ab_printf(ab, A_BOLD A_PEACH " REGIME SIGNALS" A_RESET);
    y++;

    // regime name + strategy
    const char *regime_name = (s->current_regime == 1) ? "TRENDING" :
                              (s->current_regime == 2) ? "VOLATILE" :
                              (s->current_regime == 3) ? "TRENDING_DOWN" : "RANGING";
    const char *regime_color = (s->current_regime == 1) ? A_GREEN :
                               (s->current_regime == 2 || s->current_regime == 3) ? A_RED : A_DIM;
    const char *strat_name = (s->strategy_id == 1) ? "MOMENTUM" : "MEAN REVERSION";

    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "regime: " A_BOLD "%s%s" A_RESET A_DIM " (%.0fm)"
              A_RESET A_DIM "     " A_SAND "strategy: " A_BOLD A_FG "%s" A_RESET,
              regime_color, regime_name, s->regime_duration_min, strat_name);
    y++;

    // R² short + long with visual bars
    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "R²  short: " A_FG "%.3f ", s->short_r2);
    ab_r2_bar(ab, s->short_r2, 6);
    ab_printf(ab, A_DIM "    " A_SAND "long: " A_FG "%.3f ", s->long_r2);
    ab_r2_bar(ab, s->long_r2, 6);
    y++;

    // vol_ratio (short/long variance) + ror_slope (trend acceleration)
    const char *ror_arrow = (s->ror_slope > 0.0001) ? "↗" :
                            (s->ror_slope < -0.0001) ? "↘" : "→";
    const char *ror_color = (s->ror_slope > 0.0001) ? A_GREEN :
                            (s->ror_slope < -0.0001) ? A_RED : A_DIM;
    const char *vr_color = (s->vol_ratio > 2.0) ? A_RED :
                           (s->vol_ratio > 1.5) ? A_YELLOW : A_FG;

    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "vol ratio: " "%s%.2f" A_RESET
              A_DIM "    " A_SAND "ror: " "%s%+.6f %s" A_RESET,
              vr_color, s->vol_ratio, ror_color, s->ror_slope, ror_arrow);
    if (s->spike_active)
        ab_printf(ab, A_DIM "  " A_BOLD A_YELLOW "SPIKE %.1fx" A_RESET, s->volume_spike_ratio);
    else if (s->volume_spike_ratio > 1.0)
        ab_printf(ab, A_DIM "  vol:%.1fx" A_RESET, s->volume_spike_ratio);
    y++;

    return y;
}

//======================================================================================================
// [SECTION: BUY GATE]
//======================================================================================================
static inline int ANSI_Section_BuyGate(AnsiBuf *ab, const TUISnapshot *s, int y, int w) {
    ab_goto(ab, y, 2);
    ab_printf(ab, A_BOLD A_PEACH " BUY GATE" A_RESET);
    y++;

    ab_goto(ab, y, 3);
    const char *gate_op = s->gate_direction ? ">=" : "<=";
    if (s->stddev_mode)
        ab_printf(ab, A_SAND "price %s " A_FG "%.2f" A_DIM "  (stddev: %.2fx)",
                  gate_op, s->buy_p, s->live_sm);
    else
        ab_printf(ab, A_SAND "price %s " A_FG "%.2f" A_DIM "  (offset: %.3f%%)",
                  gate_op, s->buy_p, s->live_offset);
    if (s->buy_p > 0.01)
        ab_printf(ab, A_DIM "   " A_SAND "dist: " A_FG "$%.2f" A_DIM " (%.3f%%)",
                  s->gate_dist, s->gate_dist_pct);
    else
        ab_printf(ab, A_DIM "   (gate disabled)");
    ab_append(ab, A_RESET);
    y++;

    // volume gate + live status
    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "vol   %s " A_FG "%.8f" A_DIM "  (mult: %.2fx)",
              gate_op, s->buy_v, s->live_vmult);
    // show what's blocking the buy right now
    if (s->buy_p < 0.01) {
        ab_printf(ab, A_DIM "   " A_BOLD A_YELLOW "GATE OFF" A_RESET);
    } else {
        int price_ok = s->gate_direction
            ? (s->price >= s->buy_p)   // momentum: price >= gate
            : (s->price <= s->buy_p);  // MR: price <= gate
        int vol_ok = (s->volume >= s->buy_v);
        if (price_ok && vol_ok)
            ab_printf(ab, A_DIM "   " A_GREEN "READY" A_RESET);
        else if (!price_ok && !vol_ok)
            ab_printf(ab, A_DIM "   " A_YELLOW "wait: price+vol" A_RESET);
        else if (!price_ok)
            ab_printf(ab, A_DIM "   " A_YELLOW "wait: price" A_RESET);
        else
            ab_printf(ab, A_DIM "   " A_YELLOW "wait: vol" A_RESET);
    }
    ab_append(ab, A_RESET);
    y++;

    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "spacing: " A_FG "$%.2f" A_DIM " (%.3f%%)" A_RESET,
              s->spacing, s->spacing_pct);
    if (s->long_gate_enabled) {
        if (s->long_gate_ok)
            ab_printf(ab, A_DIM "     " A_SAND "long trend: " A_GREEN "OK" A_RESET);
        else
            ab_printf(ab, A_DIM "     " A_SAND "long trend: " A_BOLD A_RED "BLOCKED" A_RESET);
    }
    if (s->sl_cooldown > 0)
        ab_printf(ab, A_DIM "  " A_BOLD A_YELLOW "COOLDOWN (%d)" A_RESET, s->sl_cooldown);
    y++;

    // fill rejection diagnostics
    if (s->fills_rejected > 0 && s->last_reject_reason > 0 && s->last_reject_reason <= 6) {
        static const char *reasons[] = {"", "spacing", "balance", "exposure",
                                         "breaker", "max_pos", "duplicate"};
        ab_goto(ab, y, 3);
        ab_printf(ab, A_DIM "fills " A_FG "%u" A_DIM "/" A_FG "%u"
                  A_DIM "  last reject: " A_YELLOW "%s" A_RESET,
                  s->total_buys, s->total_buys + s->fills_rejected,
                  reasons[s->last_reject_reason]);
        y++;
    }

    return y;
}

//======================================================================================================
// [SECTION: PORTFOLIO]
//======================================================================================================
static inline int ANSI_Section_Portfolio(AnsiBuf *ab, const TUISnapshot *s, int y, int w) {
    ab_goto(ab, y, 2);
    ab_printf(ab, A_BOLD A_PEACH " PORTFOLIO" A_RESET);
    y++;

    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "equity: " A_FG "$%.2f" A_DIM "       "
              A_SAND "balance: " A_FG "$%.2f" A_RESET,
              s->equity, s->balance);
    y++;

    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "exposure: " A_FG "%.1f%%/%.0f%%" A_DIM "       "
              A_SAND "fees: " A_FG "$%.4f" A_RESET,
              s->exposure_pct, s->max_exp, s->fees);
    y++;

    return y;
}

//======================================================================================================
// [SECTION: P&L]
//======================================================================================================
static inline int ANSI_Section_PnL(AnsiBuf *ab, const TUISnapshot *s, int y, int w) {
    ab_goto(ab, y, 2);
    ab_printf(ab, A_BOLD A_PEACH " P&L" A_RESET);
    y++;

    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "realized: " "%s$%+.4f" A_RESET A_DIM "     "
              A_SAND "unrealized: " "%s$%+.4f" A_RESET,
              A_PNL(s->realized), s->realized,
              A_PNL(s->unrealized), s->unrealized);
    y++;

    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "total: " A_BOLD "%s$%+.4f" A_RESET
              A_DIM "          (" "%s%+.2f%%" A_DIM ")" A_RESET,
              A_PNL(s->total_pnl), s->total_pnl,
              A_PNL(s->return_pct), s->return_pct);
    y++;

    return y;
}

//======================================================================================================
// [SECTION: RISK]
//======================================================================================================
static inline int ANSI_Section_Risk(AnsiBuf *ab, const TUISnapshot *s, int y, int w) {
    ab_goto(ab, y, 2);
    ab_printf(ab, A_BOLD A_PEACH " RISK" A_RESET);
    y++;

    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "risk/pos: " A_FG "%.1f%%" A_RESET A_DIM "  │  "
              A_SAND "breaker: ", s->risk_amt);
    if (s->breaker_tripped)
        ab_printf(ab, A_BOLD A_RED "TRIPPED" A_RESET);
    else
        ab_printf(ab, A_GREEN "OK" A_RESET);
    y++;

    return y;
}

//======================================================================================================
// [SECTION: CONFIG]
//======================================================================================================
static inline int ANSI_Section_Config(AnsiBuf *ab, const TUISnapshot *s, int y, int w) {
    ab_goto(ab, y, 2);
    ab_printf(ab, A_BOLD A_PEACH " CONFIG" A_RESET);
    y++;

    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "TP: " A_FG "%.1f%%" A_SAND "  SL: " A_FG "%.1f%%"
              A_SAND "  fee: " A_FG "%.1f%%" A_RESET,
              s->cfg_tp, s->cfg_sl, s->cfg_fee);
    if (s->cfg_slippage > 0.0)
        ab_printf(ab, A_SAND "  slip: " A_FG "%.2f%%" A_RESET, s->cfg_slippage);
    if (s->trailing_enabled)
        ab_printf(ab, A_DIM "  " A_SAND "trail: " A_FG "%.1f" A_DIM "σ"
                  A_SAND " score: " A_FG "%.2f" A_RESET,
                  s->cfg_trail_mult, s->cfg_hold_score);
    else
        ab_printf(ab, A_DIM "  trailing: off" A_RESET);
    if (s->live_trading)
        ab_printf(ab, A_BOLD "  \033[31m LIVE\033[0m" A_RESET);
    y++;

    return y;
}

//======================================================================================================
// [SECTION: STATS]
//======================================================================================================
static inline int ANSI_Section_Stats(AnsiBuf *ab, const TUISnapshot *s, int y, int w) {
    ab_goto(ab, y, 2);
    ab_printf(ab, A_BOLD A_PEACH " STATS" A_RESET);
    y++;

    uint32_t total_exits = s->wins + s->losses;

    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "buys: " A_FG "%-4u" A_SAND "  exits: " A_FG "%-4u"
              A_SAND "  W: " A_GREEN "%u" A_SAND "  L: " A_RED "%u" A_RESET,
              s->total_buys, total_exits, s->wins, s->losses);
    y++;

    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "rate: " "%s%.1f%%" A_RESET
              A_SAND "  pf: " "%s%.2f" A_RESET
              A_SAND "  avg W: " A_GREEN "$%.2f" A_SAND "  L: " A_RED "$%.2f" A_RESET,
              A_PNL(s->win_rate - 50.0), s->win_rate,
              A_PNL(s->profit_factor - 1.0), s->profit_factor,
              s->avg_win, s->avg_loss);
    y++;

    return y;
}

//======================================================================================================
// [SECTION: POSITIONS]
//======================================================================================================
static inline int ANSI_Section_Positions(AnsiBuf *ab, const TUISnapshot *s,
                                          int y, int w, int max_rows) {
    ab_goto(ab, y, 2);
    ab_printf(ab, A_BOLD A_PEACH " POSITIONS " A_DIM "(%d/%d)" A_RESET, s->active_count, s->max_positions);
    y++;

    // compact mode: ≥5 positions → single line with detail on the right
    // expanded mode: <5 positions → two lines with full detail below
    int compact = (s->active_count >= 5);
    // right column start for compact mode (detail info)
    int rcol = 48;
    if (rcol > w - 40) rcol = w - 40;
    if (rcol < 40) rcol = 40;

    int displayed = 0;
    for (int i = 0; i < 16 && y < max_rows; i++) {
        const TUIPositionSnap *ps = &s->positions[i];
        if (ps->idx < 0) continue;

        double diff = s->price - ps->entry;

        if (compact) {
            // single line: linear layout with fixed-width fields, no CHA gaps
            ab_goto(ab, y, 2);
            ab_printf(ab, A_WHEAT "#%-2d " A_FG "%5.0f" A_DIM "→" A_WHEAT "%-5.0f "
                      "%s%+-5.0f" A_RESET "  ",
                      displayed, ps->entry, s->price, A_PNL(diff), diff);
            ab_printf(ab, A_GREEN "TP:%-6.0f " A_RED "SL:%-5.0f" A_RESET "  ",
                      ps->tp, ps->sl);
            ab_printf(ab, A_FG "$%-4.0f" A_RESET, ps->value);
            ab_printf(ab, A_SAND " g:" "%s%+.2f%%" A_RESET, A_PNL(ps->gross_pnl), ps->gross_pnl);
            ab_printf(ab, A_SAND " n:" "%s%+.2f%%" A_RESET, A_PNL(ps->net_pnl), ps->net_pnl);
            ab_printf(ab, A_DIM " %2.0fm" A_RESET, ps->hold_minutes);
            if (ps->above_orig_tp && ps->is_trailing)
                ab_printf(ab, A_BOLD A_YELLOW " H" A_RESET);
            else if (ps->is_trailing)
                ab_printf(ab, A_YELLOW " T" A_RESET);
            y++;
        } else {
            // expanded: line 1 = entry/price/diff/TP/SL/hold, line 2 = val/qty/g/n
            if (y + 1 >= max_rows) break;
            ab_goto(ab, y, 3);
            ab_printf(ab, A_WHEAT "#%-2d " A_FG "$%.0f" A_DIM " → " A_WHEAT "$%.0f  "
                      "%s%+.0f" A_RESET,
                      displayed, ps->entry, s->price, A_PNL(diff), diff);
            ab_printf(ab, A_DIM "   " A_GREEN "TP:$%.0f" A_DIM "  " A_RED "SL:$%.0f" A_RESET,
                      ps->tp, ps->sl);
            ab_printf(ab, A_DIM "  %.0fm" A_RESET, ps->hold_minutes);
            if (ps->above_orig_tp && ps->is_trailing)
                ab_printf(ab, A_BOLD A_YELLOW " HOLD" A_RESET);
            else if (ps->is_trailing)
                ab_printf(ab, A_YELLOW " trail" A_RESET);
            y++;

            ab_goto(ab, y, 7);
            ab_printf(ab, A_SAND "val:" A_FG "$%.2f" A_SAND "  qty:" A_FG "%.6f"
                      A_SAND "  g:" "%s%+.2f%%" A_SAND "  n:" "%s%+.2f%%" A_RESET,
                      ps->value, ps->qty,
                      A_PNL(ps->gross_pnl), ps->gross_pnl,
                      A_PNL(ps->net_pnl), ps->net_pnl);
            y++;
        }
        displayed++;
    }

    if (displayed == 0) {
        ab_goto(ab, y, 4);
        ab_printf(ab, A_DIM "(no positions)" A_RESET);
        y++;
    }

    return y;
}

//======================================================================================================
// [SECTION: SPARKLINE CHARTS]
//======================================================================================================
static inline int ANSI_Section_Charts(AnsiBuf *ab, const TUISnapshot *s, int y, int w) {
    if (s->graph_count < 2) return y;

    int chart_w = w - 10;
    if (chart_w > 120) chart_w = 120;
    if (chart_w < 20) chart_w = 20;

    // price sparkline (wheat)
    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "price" A_RESET);
    y++;
    ab_goto(ab, y, 3);
    ab_sparkline(ab, s->price_history, s->graph_head, s->graph_count,
                 TUISnapshot::GRAPH_LEN, chart_w, A_WHEAT);
    y++;

    // P&L sparkline (per-bar green/red by value sign)
    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "p&l" A_RESET);
    y++;
    ab_goto(ab, y, 3);
    ab_sparkline_pnl(ab, s->pnl_history, s->graph_head, s->graph_count,
                     TUISnapshot::GRAPH_LEN, chart_w);
    y++;

    // volume sparkline (clay)
    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "vol" A_RESET);
    y++;
    ab_goto(ab, y, 3);
    ab_sparkline(ab, s->volume_history, s->graph_head, s->graph_count,
                 TUISnapshot::GRAPH_LEN, chart_w, A_CLAY);
    y++;

    return y;
}

//======================================================================================================
// [SECTION: CONTROLS]
//======================================================================================================
static inline int ANSI_Section_Controls(AnsiBuf *ab, int y, int w) {
    ab_goto(ab, y, 2);
    ab_printf(ab, A_PINK "[q]" A_DIM "uit  "
              A_PINK "[p]" A_DIM "ause  "
              A_PINK "[r]" A_DIM "eload  "
              A_PINK "[s]" A_DIM "witch  "
              A_PINK "[l]" A_DIM "ayout" A_RESET);
    y++;
    return y;
}

//======================================================================================================
// [SECTION: LATENCY PROFILING]
//======================================================================================================
#ifdef LATENCY_PROFILING
static inline int ANSI_Section_Latency(AnsiBuf *ab, const TUISnapshot *s, int y, int w) {
    ab_goto(ab, y, 2);
    ab_printf(ab, A_BOLD A_PEACH " LATENCY" A_RESET);
    y++;

    if (s->hot_count > 0) {
        ab_goto(ab, y, 3);
        ab_printf(ab, A_SAND "hot:  " A_FG "avg %.0fns" A_DIM "  p50 " A_FG "%.0fns"
                  A_DIM "  p95 " A_FG "%.0fns" A_DIM "  (%lu)" A_RESET,
                  s->hot_avg_ns, s->hot_p50_ns, s->hot_p95_ns,
                  (unsigned long)s->hot_count);
        y++;
        ab_goto(ab, y, 3);
        ab_printf(ab, A_DIM "  bg: " A_FG "%.0fns" A_DIM "  eg: " A_FG "%.0fns"
                  A_DIM " (%.0f/pos)  pc: " A_FG "%.0fns" A_RESET,
                  s->bg_avg_ns, s->eg_avg_ns, s->eg_per_pos_ns, s->pc_avg_ns);
        y++;
    }
    if (s->slow_count > 0) {
        const char *unit = (s->slow_avg_ns >= 1000.0) ? "μs" : "ns";
        double avg = (s->slow_avg_ns >= 1000.0) ? s->slow_avg_ns / 1000.0 : s->slow_avg_ns;
        ab_goto(ab, y, 3);
        ab_printf(ab, A_SAND "slow: " A_FG "avg %.1f%s" A_DIM "  (%lu)" A_RESET,
                  avg, unit, (unsigned long)s->slow_count);
        y++;
    }

    return y;
}
#endif

//======================================================================================================
// [LAYOUT DEFINITIONS]
//======================================================================================================
#define ANSI_LAYOUT_STANDARD  0
#define ANSI_LAYOUT_CHARTS    1
#define ANSI_LAYOUT_COMPACT   2
#define ANSI_LAYOUT_COUNT     3

//======================================================================================================
// [SECTION: RIGHT PANEL — session stats, fill diagnostics, trade summary]
//======================================================================================================
// overlays on existing rows using ab_goto_right (no interaction with ab_goto/last_row)
// called AFTER left panel + gap clearing + bottom bar, so it writes on top of rendered rows
// hidden on narrow terminals (< 100 columns)
//======================================================================================================
static inline void ANSI_Section_RightPanel(AnsiBuf *ab, const TUISnapshot *s,
                                            int h, int w, uint64_t start_time) {
    if (w < 100) return;  // not enough width
    int rc = w - 36;
    if (rc < 62) rc = 62;

    // separator
    for (int r = 7; r <= 22 && r < h - 2; r++) {
        ab_goto_right(ab, r, rc - 2);
        ab_printf(ab, A_SURF "│" A_RESET);
    }

    // SESSION
    ab_goto_right(ab, 7, rc);
    ab_printf(ab, A_BOLD A_PEACH "SESSION" A_RESET);
    ab_goto_right(ab, 8, rc);
    ab_printf(ab, A_SAND "high: " A_FG "$%.2f" A_RESET, s->session_high);
    ab_goto_right(ab, 9, rc);
    ab_printf(ab, A_SAND "low:  " A_FG "$%.2f" A_RESET, s->session_low);
    ab_goto_right(ab, 10, rc);
    // tick_rate field holds raw total_ticks, compute rate from uptime
    time_t now = time(NULL);
    double uptime = difftime(now, (time_t)start_time);
    double rate = (uptime > 0) ? s->tick_rate / uptime : 0.0;
    ab_printf(ab, A_SAND "rate: " A_FG "%.1f" A_DIM " ticks/s" A_RESET, rate);
    ab_goto_right(ab, 11, rc);
    ab_printf(ab, A_SAND "ticks: " A_FG "%.0f" A_RESET, s->tick_rate);

    // FILLS
    ab_goto_right(ab, 13, rc);
    ab_printf(ab, A_BOLD A_PEACH "FILLS" A_RESET);
    ab_goto_right(ab, 14, rc);
    ab_printf(ab, A_SAND "accepted: " A_FG "%u" A_RESET, s->total_buys);
    ab_goto_right(ab, 15, rc);
    ab_printf(ab, A_SAND "rejected: " A_FG "%u" A_RESET, s->fills_rejected);
    if (s->last_reject_reason > 0 && s->last_reject_reason <= 6) {
        static const char *reasons[] = {"", "spacing", "balance", "exposure",
                                         "breaker", "max_pos", "duplicate"};
        ab_goto_right(ab, 16, rc);
        ab_printf(ab, A_SAND "last: " A_YELLOW "%s" A_RESET,
                  reasons[s->last_reject_reason]);
    }

    // TRADES
    ab_goto_right(ab, 18, rc);
    ab_printf(ab, A_BOLD A_PEACH "TRADES" A_RESET);
    ab_goto_right(ab, 19, rc);
    ab_printf(ab, A_GREEN "W:" A_FG "%u " A_RED "L:" A_FG "%u " A_SAND "rate: "
              "%s%.1f%%" A_RESET,
              s->wins, s->losses,
              (s->win_rate >= 50.0) ? A_GREEN : A_RED, s->win_rate);
    ab_goto_right(ab, 20, rc);
    ab_printf(ab, A_SAND "pf: " "%s%.2f" A_RESET,
              (s->profit_factor >= 1.0) ? A_GREEN : A_RED, s->profit_factor);
    ab_goto_right(ab, 21, rc);
    ab_printf(ab, A_SAND "avg W: " A_GREEN "$%.2f" A_RESET, s->avg_win);
    ab_goto_right(ab, 22, rc);
    ab_printf(ab, A_SAND "avg L: " A_RED "$%.2f" A_RESET, s->avg_loss);
}

//======================================================================================================
// [LAYOUT: STANDARD — full dashboard]
//======================================================================================================
static inline void ANSI_Layout_Standard(AnsiBuf *ab, const TUISnapshot *s,
                                         int h, int w, uint64_t start_time) {
    int y = 1;
    y = ANSI_Section_Header(ab, s, y, w, start_time);
    y = ANSI_Section_TopBar(ab, s, y, w);
    y = ANSI_Section_Market(ab, s, y, w);
    ab_divider(ab, y++, w, false);
    y = ANSI_Section_Regime(ab, s, y, w);
    ab_divider(ab, y++, w, false);
    y = ANSI_Section_BuyGate(ab, s, y, w);
    ab_divider(ab, y++, w, false);
    y = ANSI_Section_Portfolio(ab, s, y, w);
    ab_divider(ab, y++, w, false);
    y = ANSI_Section_PnL(ab, s, y, w);
    ab_divider(ab, y++, w, false);
    y = ANSI_Section_Risk(ab, s, y, w);
    ab_divider(ab, y++, w, false);
    y = ANSI_Section_Config(ab, s, y, w);
    ab_divider(ab, y++, w, false);
    y = ANSI_Section_Positions(ab, s, y, w, h - 8);
    ab_divider(ab, y++, w, false);
    y = ANSI_Section_Stats(ab, s, y, w);
    ab_divider(ab, y++, w, false);
    y = ANSI_Section_Charts(ab, s, y, w);
#ifdef LATENCY_PROFILING
    ab_divider(ab, y++, w, false);
    y = ANSI_Section_Latency(ab, s, y, w);
#endif

    // clear gap rows between content and bottom bar (per-row, no bulk erase)
    for (int r = y; r < h - 1; r++) ab_clear_row(ab, r);
    ab_divider(ab, h - 1, w, true);
    ANSI_Section_Controls(ab, h, w);

    // right panel disabled — overlay approach causes flicker because ab_goto's
    // deferred \033[K] erases right-column content. needs proper two-column layout.
    // ANSI_Section_RightPanel(ab, s, h, w, start_time);
}

//======================================================================================================
// [LAYOUT: CHARTS — sparklines + regime signals + positions]
//======================================================================================================
static inline void ANSI_Layout_Charts(AnsiBuf *ab, const TUISnapshot *s, int h, int w) {
    int y = 1;

    // compact top bar — one line with key metrics
    ab_goto(ab, y, 2);
    ab_printf(ab, A_BOLD A_WHEAT "$%.2f" A_RESET, s->price);
    ab_printf(ab, A_DIM " │ " A_BOLD "%s$%+.2f" A_RESET, A_PNL(s->total_pnl), s->total_pnl);
    const char *regime_color = (s->current_regime == 1) ? A_GREEN :
                               (s->current_regime == 2 || s->current_regime == 3) ? A_RED : A_DIM;
    const char *regime = (s->current_regime == 1) ? "TREND" :
                         (s->current_regime == 2) ? "VOLAT" :
                         (s->current_regime == 3) ? "TR_DN" : "RANGE";
    ab_printf(ab, A_DIM " │ " "%s%s" A_RESET, regime_color, regime);
    ab_printf(ab, A_DIM " │ " A_SAND "POS %d/%d" A_RESET, s->active_count, s->max_positions);
    y++;
    ab_divider(ab, y++, w, false);

    // sparkline charts
    y = ANSI_Section_Charts(ab, s, y, w);

    // regime signals (compact)
    ab_divider(ab, y++, w, false);
    ab_goto(ab, y, 3);
    ab_printf(ab, A_SAND "R² " A_FG "%.3f ", s->short_r2);
    ab_r2_bar(ab, s->short_r2, 6);
    ab_printf(ab, A_DIM "  " A_SAND "vol: " A_FG "%.2f"
              A_DIM "  " A_SAND "ror: " A_FG "%+.6f" A_RESET,
              s->vol_ratio, s->ror_slope);
    y++;

    // positions
    ab_divider(ab, y++, w, false);
    y = ANSI_Section_Positions(ab, s, y, w, h - 2);

    ab_printf(ab, "\033[%d;1H\033[J", y);
    ab_divider(ab, h - 1, w, false);
    ANSI_Section_Controls(ab, h, w);
}

//======================================================================================================
// [LAYOUT: COMPACT — minimal metrics only]
//======================================================================================================
static inline void ANSI_Layout_Compact(AnsiBuf *ab, const TUISnapshot *s, int h, int w) {
    int y = 1;
    y = ANSI_Section_TopBar(ab, s, y, w);
    y = ANSI_Section_Positions(ab, s, y, w, h - 8);
    ab_divider(ab, y++, w, false);
    y = ANSI_Section_Stats(ab, s, y, w);

    ab_printf(ab, "\033[%d;1H\033[J", y);
    ab_divider(ab, h - 1, w, false);
    ANSI_Section_Controls(ab, h, w);
}

//======================================================================================================
// [LAYOUT DISPATCH]
//======================================================================================================
static inline void ANSI_Layout_Render(AnsiBuf *ab, const TUISnapshot *s,
                                       int layout_id, int h, int w,
                                       uint64_t start_time) {
    switch (layout_id) {
        case ANSI_LAYOUT_CHARTS:
            ANSI_Layout_Charts(ab, s, h, w);
            break;
        case ANSI_LAYOUT_COMPACT:
            ANSI_Layout_Compact(ab, s, h, w);
            break;
        default:
            ANSI_Layout_Standard(ab, s, h, w, start_time);
            break;
    }
}

//======================================================================================================
// [TOP-LEVEL RENDER]
//======================================================================================================
// builds entire frame in buffer, writes in one shot with synchronized output
// call from TUI thread at desired FPS
//======================================================================================================
static inline void ANSI_Render(const TUISnapshot *s, int layout_id,
                                int term_h, int term_w, uint64_t start_time) {
    AnsiBuf ab;
    ab.len = 0;
    ab.last_row = -1;

    // synchronized output: terminal buffers everything, paints in one pass
    ab_append(&ab, "\033[?2026h");   // begin sync
    ab_append(&ab, "\033[H");        // cursor home (no clear — content overwrites in place)

    ANSI_Layout_Render(&ab, s, layout_id, term_h, term_w, start_time);

    // finalize: erase tail of last content row
    if (ab.last_row >= 0) ab_append(&ab, "\033[K");

    ab_append(&ab, "\033[?2026l");   // end sync
    ab_flush(&ab);
}

#endif // TUI_ANSI_HPP
