// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ENGINE TUI]
//======================================================================================================
// simple terminal UI using ANSI escape codes for monitoring the engine state
// no framework, no dependencies beyond standard POSIX terminal control
//
// the engine runs identically with or without the TUI - tui_enabled=0 skips all display calls
// the TUI only READS engine state, never writes it (except explicit user commands: pause/reload/quit)
//
// terminal is set to raw mode for single-char input (no enter needed for commands)
// signal handler restores terminal on crash so the user doesnt have to type `reset`
//======================================================================================================
#ifndef ENGINE_TUI_HPP
#define ENGINE_TUI_HPP

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <time.h>

#include "../CoreFrameworks/PortfolioController.hpp"
#include "../CoreFrameworks/OrderGates.hpp"
#if defined(USE_FTXUI) || defined(USE_ANSI_TUI)
#include <fcntl.h>
#include <sys/ioctl.h>
#endif

using namespace std;

//======================================================================================================
// [FOXML THEME - truecolor ANSI]
//======================================================================================================
// colors pulled from the FoxML neovim colorscheme palette
// uses 24-bit truecolor: \033[38;2;R;G;Bm (foreground)
//======================================================================================================
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"

// palette - earthy tones from the FoxML neovim colorscheme
#define C_PEACH   "\033[38;2;212;152;90m"    // #d4985a - titles, headers, accent
#define C_WHEAT   "\033[38;2;212;180;131m"   // #d4b483 - warm accent, price
#define C_FG      "\033[38;2;213;196;176m"   // #d5c4b0 - normal text
#define C_DIM     "\033[38;2;90;98;112m"     // #5a6270 - comments, hints
#define C_GREEN   "\033[38;2;122;171;136m"   // #7aab88 - positive P&L
#define C_RED     "\033[38;2;192;104;104m"   // #c06868 - negative P&L
#define C_YELLOW  "\033[38;2;196;180;138m"   // #c4b48a - warnings
#define C_SAND    "\033[38;2;168;154;122m"   // #a89a7a - labels, separators
#define C_WARM    "\033[38;2;176;164;152m"   // #b0a498 - secondary labels
#define C_PINK    "\033[38;2;184;150;122m"   // #b8967a - secondary accent
#define C_SURF    "\033[38;2;58;65;75m"      // #3a414b - dim separators
#define C_LAV     "\033[38;2;138;154;122m"   // #8a9a7a - lavender/muted green

// conditional P&L color: green if >= 0, red if < 0
#define C_PNL(v) ((v) >= 0.0 ? C_GREEN : C_RED)

//======================================================================================================
// [STRUCT]
//======================================================================================================
struct EngineTUI {
    int enabled;
    uint64_t last_render_tick;
    uint32_t render_interval;      // render every N ticks (not every tick - would thrash the terminal)
    struct termios original_term;  // saved terminal state for cleanup
    int raw_mode_active;
    uint64_t start_time;           // for uptime display
#ifdef LATENCY_PROFILING
    // hot path = BuyGate + PositionExitGate + PortfolioController_Tick (fast portion)
    // slow path = full tick including slow-path operations (every poll_interval)
    uint64_t hot_min, hot_max, hot_sum, hot_count;
    uint64_t slow_min, slow_max, slow_sum, slow_count;
    // per-component hot path breakdown (cycle counts)
    uint64_t bg_sum, bg_max;   // BuyGate
    uint64_t eg_sum, eg_max;   // ExitGate (includes skip-when-empty)
    uint64_t pc_sum, pc_max;   // PortfolioController_Tick (fast path only)
    // fill vs no-fill breakdown within PCTick
    uint64_t pc_fill_sum, pc_fill_max, pc_fill_count;
    uint64_t pc_nofill_sum, pc_nofill_max, pc_nofill_count;
    // position count accumulator for per-position ExitGate cost
    uint64_t eg_pos_sum;       // sum of active position counts across hot ticks
    // log2 histogram for percentile computation (bucket k = [2^k, 2^(k+1)) cycles)
    // 21 buckets covers 1 cycle to 1M cycles (~0.3ns to 286μs at 3.5GHz)
    uint32_t hot_hist[21];
    double tsc_per_ns;  // TSC cycles per nanosecond, calibrated at startup
#endif
};

//======================================================================================================
// [TERMINAL RAW MODE]
//======================================================================================================
// global pointer for signal handler cleanup - only one TUI instance per process
//======================================================================================================
static EngineTUI *g_tui_instance = NULL;

static void tui_signal_handler(int sig) {
    if (g_tui_instance && g_tui_instance->raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_tui_instance->original_term);
        // show cursor
        write(STDOUT_FILENO, "\033[?25h", 6);
    }
    // re-raise to get default behavior (core dump, exit, etc)
    signal(sig, SIG_DFL);
    raise(sig);
}

//======================================================================================================
// [INIT]
//======================================================================================================
static inline void TUI_Init(EngineTUI *tui, int enabled, uint32_t render_interval) {
    tui->enabled          = enabled;
    tui->last_render_tick = 0;
    tui->render_interval  = render_interval;
    tui->raw_mode_active  = 0;
    tui->start_time       = (uint64_t)time(NULL);

    if (!enabled) return;

    // save terminal state and switch to raw mode for single-char input
    tcgetattr(STDIN_FILENO, &tui->original_term);

    struct termios raw = tui->original_term;
    raw.c_lflag &= ~(ICANON | ECHO);  // no line buffering, no echo
    raw.c_cc[VMIN]  = 0;               // non-blocking read
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    tui->raw_mode_active = 1;

    // install signal handlers for clean terminal restore on crash
    g_tui_instance = tui;
    signal(SIGINT,  tui_signal_handler);
    signal(SIGTERM, tui_signal_handler);
    signal(SIGSEGV, tui_signal_handler);

    // hide cursor during rendering
    printf("\033[?25l");
    fflush(stdout);
}

//======================================================================================================
// [CLEANUP]
//======================================================================================================
static inline void TUI_Cleanup(EngineTUI *tui) {
    if (!tui->raw_mode_active) return;

    tcsetattr(STDIN_FILENO, TCSANOW, &tui->original_term);
    tui->raw_mode_active = 0;

    // show cursor, clear screen
    printf("\033[?25h\033[2J\033[H");
    fflush(stdout);

    g_tui_instance = NULL;
}

//======================================================================================================
// [RENDER]
//======================================================================================================
// clears the screen and draws the full dashboard
// only renders every render_interval ticks to avoid thrashing the terminal
//
// uses cursor home (\033[H) instead of clear (\033[2J) to reduce flicker -
// overwrites in place rather than clearing and redrawing
//======================================================================================================
template <unsigned F>
static inline void TUI_Render(EngineTUI *tui, const PortfolioController<F> *ctrl,
                               const DataStream<F> *stream, uint64_t tick) {
    if (!tui->enabled) return;
    if (tick - tui->last_render_tick < tui->render_interval) return;
    tui->last_render_tick = tick;

    // compute uptime
    uint64_t now     = (uint64_t)time(NULL);
    uint64_t elapsed = now - tui->start_time;
    uint32_t hours   = (uint32_t)(elapsed / 3600);
    uint32_t mins    = (uint32_t)((elapsed % 3600) / 60);
    uint32_t secs    = (uint32_t)(elapsed % 60);

    // state string
    const char *state_str = (ctrl->state == CONTROLLER_WARMUP) ? "WARMUP" : "ACTIVE";

    // convert FPN values to doubles for display
    double price  = FPN_ToDouble(stream->price);
    double volume = FPN_ToDouble(stream->volume);
    double buy_p  = FPN_ToDouble(ctrl->buy_conds.price);
    double buy_v  = FPN_ToDouble(ctrl->buy_conds.volume);
    double pnl    = FPN_ToDouble(ctrl->portfolio_delta);

    // rolling stats
    double roll_price_avg = FPN_ToDouble(ctrl->rolling.price_avg);
    double roll_vol_avg   = FPN_ToDouble(ctrl->rolling.volume_avg);
    double roll_stddev    = FPN_ToDouble(ctrl->rolling.price_stddev);
    double roll_vol_slope = FPN_ToDouble(ctrl->rolling.volume_slope);
    double roll_p_min     = FPN_ToDouble(ctrl->rolling.price_min);
    double roll_p_max     = FPN_ToDouble(ctrl->rolling.price_max);

    // distance from buy gate (how far price needs to drop to trigger)
    double gate_dist = price - buy_p;
    double gate_dist_pct = (roll_price_avg != 0.0) ? (gate_dist / roll_price_avg) * 100.0 : 0.0;

    // entry spacing
    double spacing = FPN_ToDouble(RollingStats_EntrySpacing(&ctrl->rolling, ctrl->config.spacing_multiplier));

    int active_count = Portfolio_CountActive(&ctrl->portfolio);

    //==================================================================================================
    // [PHASE 1] pre-render positions into buffer (compute totals needed by left column)
    //==================================================================================================
    #define POS_MAX_LINES 70
    #define POS_LINE_W 200
    char pos_buf[POS_MAX_LINES][POS_LINE_W];
    int pln = 0;

    snprintf(pos_buf[pln++], POS_LINE_W,
             C_BOLD C_PEACH "POSITIONS " C_DIM "(%d/16):" C_RESET, active_count);

    uint16_t active = ctrl->portfolio.active_bitmap;
    int displayed = 0;
    double total_value = 0.0;
    double total_qty   = 0.0;
    double fee_r = FPN_ToDouble(ctrl->config.fee_rate);
    while (active) {
        int idx = __builtin_ctz(active);
        const Position<F> *pos = &ctrl->portfolio.positions[idx];

        double entry   = FPN_ToDouble(pos->entry_price);
        double qty     = FPN_ToDouble(pos->quantity);
        double tp      = FPN_ToDouble(pos->take_profit_price);
        double sl      = FPN_ToDouble(pos->stop_loss_price);
        double pos_pnl = 0.0;
        if (entry != 0.0) pos_pnl = ((price - entry) / entry) * 100.0;

        double to_tp = tp - price;
        double to_sl = price - sl;
        double value = price * qty;
        double net_pnl = pos_pnl - (fee_r * 200.0);

        total_value += value;
        total_qty   += qty;

        double price_diff = price - entry;
        if (displayed > 0)
            snprintf(pos_buf[pln++], POS_LINE_W, C_SURF "·" C_RESET);
        snprintf(pos_buf[pln++], POS_LINE_W,
                 C_WHEAT "#%-2d " C_FG "$%.2f" C_DIM "->" C_WHEAT "$%.2f"
                 " %s%+.2f" C_RESET,
                 displayed, entry, price, C_PNL(price_diff), price_diff);
        snprintf(pos_buf[pln++], POS_LINE_W,
                 C_SAND "    qty:" C_FG "%.6f" C_SAND " val:" C_FG "$%.2f" C_RESET,
                 qty, value);
        int is_trailing = !FPN_Equal(pos->take_profit_price, pos->original_tp);
        double orig_tp_d = FPN_ToDouble(pos->original_tp);
        int above_orig_tp = (price > orig_tp_d) && (entry != 0.0);
        // status: "HOLDING" when above original TP and trailing keeps it open
        const char *trail_status = "";
        if (above_orig_tp && is_trailing)
            trail_status = C_BOLD C_YELLOW " HOLDING" C_RESET;
        else if (is_trailing)
            trail_status = C_YELLOW " trail" C_RESET;
        snprintf(pos_buf[pln++], POS_LINE_W,
                 C_SAND "    TP:" C_GREEN "$%.0f" C_RESET "%s"
                 C_SAND " SL:" C_RED "$%.0f" C_RESET,
                 tp, trail_status, sl);
        double hold_min = (ctrl->entry_time[idx] > 0)
            ? difftime(time(NULL), ctrl->entry_time[idx]) / 60.0 : 0.0;
        snprintf(pos_buf[pln++], POS_LINE_W,
                 C_SAND "    g:" "%s%+.2f%%" C_SAND " n:" "%s%+.2f%%" C_DIM " hold:%.0fm" C_RESET,
                 C_PNL(pos_pnl), pos_pnl, C_PNL(net_pnl), net_pnl, hold_min);
        displayed++;
        active &= active - 1;
        if (pln >= POS_MAX_LINES - 4) break;  // safety
    }

    //==================================================================================================
    // [PHASE 2] print left column (no positions - they go right)
    //==================================================================================================
    // cursor home + clear
    printf("\033[H\033[2J");

    int row = 1;  // track current row for right-column overlay

    printf(C_SAND "  ================================================================" C_RESET "\n"); row++;
    printf(C_BOLD C_PEACH "     /\\_/\\   FOXML TRADER" C_RESET
           C_RESET "\n"); row++;
    printf(C_BOLD C_PEACH "    ( o.o )  " C_WHEAT "engine v3.0.27" C_RESET "\n"); row++;
    printf(C_BOLD C_PEACH "     > ^ <" C_RESET "\n"); row++;
    printf(C_SAND "  ================================================================" C_RESET "\n"); row++;
    int is_paused = FPN_IsZero(ctrl->buy_conds.price) && (ctrl->state == CONTROLLER_ACTIVE);
    printf(C_SAND "  STATE: " C_FG "%-8s" C_RESET
           C_DIM "  |  " C_SAND "UPTIME: " C_FG "%02u:%02u:%02u" C_RESET
           "%s" "\n",
           state_str, hours, mins, secs,
           is_paused ? C_DIM "  |  " C_BOLD C_YELLOW "PAUSED" C_RESET : ""); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;
    printf(C_SAND "  PRICE: " C_BOLD C_WHEAT "%-12.2f" C_RESET
           C_DIM "  |  " C_SAND "VOLUME: " C_FG "%-12.8f" C_RESET "\n", price, volume); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    int pos_start_row = row;  // positions start alongside market structure

    printf(C_BOLD C_PEACH "  MARKET STRUCTURE " C_DIM "(rolling %d-tick window):" C_RESET "\n", ctrl->rolling.count); row++;
    printf(C_SAND "    avg price:  " C_FG "%-12.2f" C_DIM "  |  " C_SAND "stddev: " C_FG "%-10.2f" C_RESET "\n", roll_price_avg, roll_stddev); row++;
    printf(C_SAND "    range:      " C_FG "%-12.2f" C_DIM "  -  " C_FG "%-12.2f" C_RESET "\n", roll_p_min, roll_p_max); row++;
    double roll_price_slope = FPN_ToDouble(ctrl->rolling.price_slope);
    // normalize slopes by price for display (percentage per tick, price-independent)
    double slope_pct = (roll_price_avg != 0.0) ? (roll_price_slope / roll_price_avg) * 100.0 : 0.0;
    printf(C_SAND "    avg volume: " C_FG "%-12.8f" C_DIM "  |  " C_SAND "vol slope: " C_FG "%+.8f" C_RESET "\n", roll_vol_avg, roll_vol_slope); row++;
    const char *trend_color = (slope_pct > 0.001) ? C_GREEN : (slope_pct < -0.001) ? C_RED : C_DIM;
    const char *trend_str   = (slope_pct > 0.001) ? "UP" : (slope_pct < -0.001) ? "DOWN" : "FLAT";
    printf(C_SAND "    price slope: " C_FG "%+.6f%%/tick" C_DIM "  |  " C_SAND "trend: " "%s%s" C_RESET "\n",
           slope_pct, trend_color, trend_str); row++;
    // long-window trend (512-tick), also normalized by price
    double long_slope = FPN_ToDouble(ctrl->rolling_long->price_slope);
    double long_avg   = FPN_ToDouble(ctrl->rolling_long->price_avg);
    double long_slope_pct = (long_avg != 0.0) ? (long_slope / long_avg) * 100.0 : 0.0;
    int long_count = ctrl->rolling_long->count;
    const char *long_trend_color = (long_slope_pct > 0.001) ? C_GREEN : (long_slope_pct < -0.001) ? C_RED : C_DIM;
    const char *long_trend_str   = (long_slope_pct > 0.001) ? "UP" : (long_slope_pct < -0.001) ? "DOWN" : "FLAT";
    printf(C_SAND "    long window " C_DIM "(%d-tick):" C_FG " %+.6f%%/tick"
           C_DIM "  |  " "%s%s" C_RESET "\n",
           long_count, long_slope_pct, long_trend_color, long_trend_str); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;
    // adaptive filter state
    double live_offset = FPN_ToDouble(ctrl->mean_rev.live_offset_pct) * 100.0;  // display as %
    double live_vmult  = FPN_ToDouble(ctrl->mean_rev.live_vol_mult);
    int stddev_mode = !FPN_IsZero(ctrl->config.offset_stddev_mult);

    const char *gate_op = ctrl->buy_conds.gate_direction ? ">=" : "<=";
    printf(C_BOLD C_PEACH "  BUY GATE " C_DIM "(adaptive):" C_RESET "\n"); row++;
    if (stddev_mode) {
        double live_sm = FPN_ToDouble(ctrl->mean_rev.live_stddev_mult);
        printf(C_SAND "    price %s " C_FG "%-12.2f" C_DIM "  (stddev: %.2fx)" C_RESET "\n", gate_op, buy_p, live_sm); row++;
    } else {
        printf(C_SAND "    price %s " C_FG "%-12.2f" C_DIM "  (offset: %.3f%%)" C_RESET "\n", gate_op, buy_p, live_offset); row++;
    }
    printf(C_SAND "    vol   >= " C_FG "%-12.8f" C_DIM "  (mult: %.2fx)" C_RESET "\n", buy_v, live_vmult); row++;
    double spacing_pct = (roll_price_avg != 0.0) ? (spacing / roll_price_avg) * 100.0 : 0.0;
    if (buy_p > 0.01) {
        printf(C_SAND "    distance:   " C_FG "$%-10.2f" C_DIM "  (%.3f%% away)" C_RESET "\n", gate_dist, gate_dist_pct); row++;
    } else {
        printf(C_SAND "    distance:   " C_DIM "—  (gate disabled)" C_RESET "\n"); row++;
    }
    printf(C_SAND "    spacing:    " C_FG "$%-10.2f" C_DIM "  (%.3f%% of avg)" C_RESET "\n", spacing, spacing_pct); row++;
    // multi-timeframe gate status
    int long_gate_enabled = !FPN_IsZero(ctrl->config.min_long_slope);
    if (long_gate_enabled) {
        double min_ls = FPN_ToDouble(ctrl->config.min_long_slope);
        // gate uses relative slope (slope/price_avg), match the comparison
        double rel_slope = (long_avg != 0.0) ? long_slope / long_avg : 0.0;
        int long_gate_ok = (rel_slope >= min_ls);
        if (long_gate_ok) {
            printf(C_SAND "    long trend: " C_GREEN "OK" C_RESET "\n"); row++;
        } else {
            printf(C_SAND "    long trend: " C_BOLD C_RED "BLOCKED" C_RESET
                   C_DIM " (%+.6f < %+.6f)" C_RESET "\n", rel_slope, min_ls); row++;
        }
    }
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    double realized = FPN_ToDouble(ctrl->realized_pnl);
    double balance  = FPN_ToDouble(ctrl->balance);
    double starting = FPN_ToDouble(ctrl->config.starting_balance);
    double fees     = FPN_ToDouble(ctrl->total_fees);
    double total_pnl = realized + pnl;
    double return_pct = (starting != 0.0) ? (total_pnl / starting) * 100.0 : 0.0;
    double risk_amt = FPN_ToDouble(ctrl->config.risk_pct) * 100.0;

    // ==== PORTFOLIO section ====
    double equity = balance + total_value;
    double deployed = starting - balance;
    double exposure_pct = (starting != 0.0) ? (deployed / starting) * 100.0 : 0.0;
    double max_exp = FPN_ToDouble(ctrl->config.max_exposure_pct) * 100.0;

    printf(C_BOLD C_PEACH "  PORTFOLIO:" C_RESET "\n"); row++;
    printf(C_SAND "    equity:     " C_BOLD C_FG "$%-12.4f" C_RESET C_DIM "  (cash + positions)" C_RESET "\n", equity); row++;
    printf(C_SAND "    balance:    " C_FG "$%-12.4f" C_RESET C_DIM "  (started: $%.0f)" C_RESET "\n", balance, starting); row++;
    printf(C_SAND "    held:       " C_FG "$%-12.4f" C_RESET C_DIM "  (qty: %.6f)" C_RESET "\n", total_value, total_qty); row++;
    printf(C_SAND "    exposure:   " C_FG "%.1f%%/%.0f%%" C_RESET
           C_DIM "  |  " C_SAND "fees: " C_FG "$%.4f" C_DIM " (%.1f%%)" C_RESET "\n",
           exposure_pct, max_exp, fees, FPN_ToDouble(ctrl->config.fee_rate) * 100.0); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    // ==== P&L section ====
    printf(C_BOLD C_PEACH "  P&L:" C_RESET "\n"); row++;
    printf(C_SAND "    realized:   " "%s$%-+12.4f" C_RESET C_DIM "  (after fees)" C_RESET "\n", C_PNL(realized), realized); row++;
    printf(C_SAND "    unrealized: " "%s$%-+12.4f" C_RESET C_DIM "  (open positions)" C_RESET "\n", C_PNL(pnl), pnl); row++;
    printf(C_SAND "    total:      " C_BOLD "%s$%-+12.4f" C_RESET C_DIM "  (" "%s%+.2f%%" C_DIM ")" C_RESET "\n",
           C_PNL(total_pnl), total_pnl, C_PNL(return_pct), return_pct); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    // ==== RISK section ====
    double max_dd  = FPN_ToDouble(ctrl->config.max_drawdown_pct) * 100.0;
    int breaker_tripped = (total_pnl < -(starting * FPN_ToDouble(ctrl->config.max_drawdown_pct)));

    printf(C_BOLD C_PEACH "  RISK:" C_RESET "\n"); row++;
    printf(C_SAND "    risk/pos:   " C_FG "%.1f%%" C_RESET
           C_DIM "  |  " C_SAND "breaker: " "%s%s" C_RESET C_DIM " (max dd: %.0f%%)" C_RESET "\n",
           risk_amt, breaker_tripped ? C_BOLD C_RED : C_GREEN,
           breaker_tripped ? "TRIPPED" : "OK", max_dd); row++;
    const char *offset_mode_str = stddev_mode ? "stddev" : "pct";
    printf(C_SAND "    strategy:   " C_FG "MEAN REVERSION" C_RESET
           C_DIM " (" C_FG "%s" C_DIM ")  |  " C_YELLOW "PAPER" C_RESET "\n", offset_mode_str); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    // ==== CONFIG section ====
    double cfg_tp = FPN_ToDouble(ctrl->config.take_profit_pct) * 100.0;
    double cfg_sl = FPN_ToDouble(ctrl->config.stop_loss_pct) * 100.0;
    double cfg_fee = FPN_ToDouble(ctrl->config.fee_rate) * 100.0;
    double cfg_hold = FPN_ToDouble(ctrl->config.tp_hold_score);
    int trailing_enabled = !FPN_IsZero(ctrl->config.tp_hold_score);

    printf(C_BOLD C_PEACH "  CONFIG:" C_RESET "\n"); row++;
    printf(C_SAND "    TP: " C_FG "%.1f%%" C_RESET
           C_SAND "  SL: " C_FG "%.1f%%" C_RESET
           C_SAND "  risk: " C_FG "%.1f%%" C_RESET
           C_SAND "  fee: " C_FG "%.1f%%" C_RESET "\n",
           cfg_tp, cfg_sl, risk_amt, cfg_fee); row++;
    if (stddev_mode) {
        double cfg_sm = FPN_ToDouble(ctrl->config.offset_stddev_mult);
        printf(C_SAND "    offset: " C_FG "stddev %.1fx" C_RESET, cfg_sm);
    } else {
        double cfg_op = FPN_ToDouble(ctrl->config.entry_offset_pct) * 100.0;
        printf(C_SAND "    offset: " C_FG "%.3f%%" C_RESET, cfg_op);
    }
    if (trailing_enabled) {
        double cfg_tm = FPN_ToDouble(ctrl->config.tp_trail_mult);
        double cfg_sm = FPN_ToDouble(ctrl->config.sl_trail_mult);
        printf(C_SAND "  trail: " C_FG "%.1f" C_DIM "σ" C_RESET
               C_SAND " sl: " C_FG "%.1f" C_DIM "σ" C_RESET
               C_SAND " score: " C_FG "%.2f" C_RESET, cfg_tm, cfg_sm, cfg_hold);
    } else {
        printf(C_DIM "  trailing: off" C_RESET);
    }
    printf("\n"); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    // ==== STATS section ====
    uint32_t total_exits = ctrl->wins + ctrl->losses;
    double win_rate = (total_exits > 0) ? ((double)ctrl->wins / total_exits) * 100.0 : 0.0;
    double g_wins  = FPN_ToDouble(ctrl->gross_wins);
    double g_losses = FPN_ToDouble(ctrl->gross_losses);
    double profit_factor = (g_losses > 0.001) ? g_wins / g_losses : 0.0;
    double avg_win  = (ctrl->wins > 0) ? g_wins / ctrl->wins : 0.0;
    double avg_loss = (ctrl->losses > 0) ? g_losses / ctrl->losses : 0.0;
    double avg_hold = (total_exits > 0) ? (double)ctrl->total_hold_ticks / total_exits : 0.0;

    printf(C_BOLD C_PEACH "  STATS:" C_RESET "\n"); row++;
    printf(C_SAND "    buys: " C_FG "%-4u" C_RESET
           C_DIM "  |  " C_SAND "exits: " C_FG "%-4u" C_RESET
           C_DIM "  |  " C_SAND "hold: " C_FG "%.0f ticks" C_RESET "\n",
           ctrl->total_buys, total_exits, avg_hold); row++;
    printf(C_SAND "    wins: " C_GREEN "%-4u" C_RESET
           C_SAND "  losses: " C_RED "%-4u" C_RESET
           C_SAND "  rate: " "%s%.1f%%" C_RESET
           C_DIM "  |  " C_SAND "pf: " "%s%.2f" C_RESET "\n",
           ctrl->wins, ctrl->losses,
           (win_rate >= 50.0) ? C_GREEN : (total_exits > 0 ? C_RED : C_DIM), win_rate,
           (profit_factor >= 1.0) ? C_GREEN : (total_exits > 0 ? C_RED : C_DIM), profit_factor); row++;
    printf(C_SAND "    avg win: " C_GREEN "$%.4f" C_RESET
           C_SAND "  avg loss: " C_RED "$%.4f" C_RESET "\n",
           avg_win, avg_loss); row++;
    printf(C_DIM "    log: btcusdt_order_history.csv" C_RESET "\n"); row++;
    printf(C_SAND "  ================================================" C_RESET "\n"); row++;
#ifdef LATENCY_PROFILING
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;
    printf(C_BOLD C_PEACH "  LATENCY " C_DIM "(profiling enabled):" C_RESET "\n"); row++;
    if (tui->hot_count > 0) {
        double hot_avg = (double)tui->hot_sum / tui->hot_count / tui->tsc_per_ns;
        double hot_min_ns = (double)tui->hot_min / tui->tsc_per_ns;
        double hot_max_ns = (double)tui->hot_max / tui->tsc_per_ns;
        // percentiles from log2 histogram
        double hot_p50 = 0, hot_p95 = 0;
        { uint64_t p50t = tui->hot_count/2, p95t = tui->hot_count*95/100, cum = 0;
          for (int i = 0; i <= 20; i++) { cum += tui->hot_hist[i];
            if (!hot_p50 && cum >= p50t) hot_p50 = (1.5*(1ULL<<i))/tui->tsc_per_ns;
            if (!hot_p95 && cum >= p95t) hot_p95 = (1.5*(1ULL<<i))/tui->tsc_per_ns; } }
        printf(C_SAND "    hot path:  " C_FG "avg %.0fns" C_DIM "  min " C_FG "%.0fns"
               C_DIM "  max " C_FG "%.0fns" C_DIM "  p50 " C_FG "%.0fns" C_DIM "  p95 " C_FG "%.0fns"
               C_DIM "  (%lu ticks)" C_RESET "\n",
               hot_avg, hot_min_ns, hot_max_ns, hot_p50, hot_p95, (unsigned long)tui->hot_count); row++;
        double bg_avg = (double)tui->bg_sum / tui->hot_count / tui->tsc_per_ns;
        double bg_max_ns = (double)tui->bg_max / tui->tsc_per_ns;
        double eg_avg = (double)tui->eg_sum / tui->hot_count / tui->tsc_per_ns;
        double eg_max_ns = (double)tui->eg_max / tui->tsc_per_ns;
        double pc_avg = (double)tui->pc_sum / tui->hot_count / tui->tsc_per_ns;
        double pc_max_ns = (double)tui->pc_max / tui->tsc_per_ns;
        double eg_per_pos = (tui->eg_pos_sum > 0)
            ? (double)tui->eg_sum / tui->eg_pos_sum / tui->tsc_per_ns : 0;
        printf(C_DIM "      buygate:  " C_FG "avg %.0fns" C_DIM "  max " C_FG "%.0fns" C_RESET "\n", bg_avg, bg_max_ns); row++;
        printf(C_DIM "      exitgate: " C_FG "avg %.0fns" C_DIM "  max " C_FG "%.0fns"
               C_DIM "  (%.0fns/pos)" C_RESET "\n", eg_avg, eg_max_ns, eg_per_pos); row++;
        printf(C_DIM "      pctick:   " C_FG "avg %.0fns" C_DIM "  max " C_FG "%.0fns" C_RESET "\n", pc_avg, pc_max_ns); row++;
        if (tui->pc_nofill_count > 0) {
            double nf_avg = (double)tui->pc_nofill_sum / tui->pc_nofill_count / tui->tsc_per_ns;
            double nf_max = (double)tui->pc_nofill_max / tui->tsc_per_ns;
            printf(C_DIM "        no-fill: " C_FG "avg %.0fns" C_DIM "  max " C_FG "%.0fns"
                   C_DIM "  (%lu)" C_RESET "\n", nf_avg, nf_max, (unsigned long)tui->pc_nofill_count); row++;
        }
        if (tui->pc_fill_count > 0) {
            double f_avg = (double)tui->pc_fill_sum / tui->pc_fill_count / tui->tsc_per_ns;
            double f_max = (double)tui->pc_fill_max / tui->tsc_per_ns;
            printf(C_DIM "        fill:    " C_FG "avg %.0fns" C_DIM "  max " C_FG "%.0fns"
                   C_DIM "  (%lu)" C_RESET "\n", f_avg, f_max, (unsigned long)tui->pc_fill_count); row++;
        }
    }
    if (tui->slow_count > 0) {
        double slow_avg = (double)tui->slow_sum / tui->slow_count / tui->tsc_per_ns;
        double slow_min_ns = (double)tui->slow_min / tui->tsc_per_ns;
        double slow_max_ns = (double)tui->slow_max / tui->tsc_per_ns;
        const char *slow_unit = (slow_avg >= 1000.0) ? "us" : "ns";
        double slow_avg_d = (slow_avg >= 1000.0) ? slow_avg / 1000.0 : slow_avg;
        double slow_min_d = (slow_min_ns >= 1000.0) ? slow_min_ns / 1000.0 : slow_min_ns;
        double slow_max_d = (slow_max_ns >= 1000.0) ? slow_max_ns / 1000.0 : slow_max_ns;
        printf(C_SAND "    slow path: " C_FG "avg %.1f%s" C_DIM "  min " C_FG "%.1f%s"
               C_DIM "  max " C_FG "%.1f%s" C_DIM "  (%lu cycles)" C_RESET "\n",
               slow_avg_d, slow_unit, slow_min_d, slow_unit, slow_max_d, slow_unit,
               (unsigned long)tui->slow_count); row++;
    }
#endif
    // pad left column if right column (positions) extends further down
    { int pos_end_row = pos_start_row + pln;
      while (row < pos_end_row) { printf("\n"); row++; } }

    printf(C_PINK "  [q]" C_DIM "uit  " C_PINK "[p]" C_DIM "ause  " C_PINK "[r]" C_DIM "eload config" C_RESET "                \n"); row++;

    //==================================================================================================
    // [PHASE 3] overlay positions on right column using cursor positioning
    //==================================================================================================
    // separator at column 60, right content starts at column 64
    int sep_col = 66;

    // draw || separator and position lines (only for position height, not full left column)
    for (int i = 0; i < pln; i++) {
        int r = pos_start_row + i;
        printf("\033[%d;%dH" C_SURF "||" C_RESET " %s", r, sep_col, pos_buf[i]);
    }

    fflush(stdout);
}

//======================================================================================================
// [HANDLE INPUT]
//======================================================================================================
// reads a single char from stdin and handles TUI commands
// returns: 0 = no action, 'q' = quit requested, 'p' = pause toggled, 'r' = reload requested
//
// pause: sets buy gate price to 0 (disables buys), exit gate keeps running
// unpause: restores buy gate to current conditions (regression will readjust on next slow path)
//======================================================================================================
template <unsigned F>
static inline char TUI_HandleInput(EngineTUI *tui, PortfolioController<F> *ctrl,
                                    const char *config_path, int *running) {
    if (!tui->enabled) return 0;

    char c = 0;
    int n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return 0;

    if (c == 'q' || c == 'Q') {
        *running = 0;
        return 'q';
    }

    if (c == 'p' || c == 'P') {
        int is_paused = FPN_IsZero(ctrl->buy_conds.price);
        if (is_paused)
            PortfolioController_Unpause(ctrl);
        else {
            ctrl->buy_conds.price  = FPN_Zero<F>();
            ctrl->buy_conds.volume = FPN_Zero<F>();
        }
        return 'p';
    }

    if (c == 'r' || c == 'R') {
        ControllerConfig<F> new_cfg = ControllerConfig_Load<F>(config_path);
        PortfolioController_HotReload(ctrl, new_cfg);
        fprintf(stderr, "[TUI] config reloaded from %s\n", config_path);
        return 'r';
    }

    if (c == 's' || c == 'S') {
        PortfolioController_CycleRegime(ctrl);
        return 's';
    }

    return 0;
}

//======================================================================================================
// [MULTICORE TUI]
//======================================================================================================
// when MULTICORE_TUI is defined, the TUI runs on a separate thread with its own L1 cache.
// the engine thread copies a snapshot of display state every slow-path cycle.
// the TUI thread reads the snapshot and renders independently.
// zero L1 cache pollution on the engine core.
//======================================================================================================
#ifdef MULTICORE_TUI
#include <pthread.h>

//======================================================================================================
// [SNAPSHOT STRUCT]
//======================================================================================================
// all doubles — no FPN on the TUI thread. engine converts during snapshot copy.
//======================================================================================================
struct TUIPositionSnap {
    int idx;
    double entry, qty, tp, sl, orig_tp;
    double value, gross_pnl, net_pnl;
    int is_trailing, above_orig_tp;
    uint64_t ticks_held;
    double hold_minutes;  // wall clock hold duration
};

struct TUISnapshot {
    // market
    double price, volume;
    // state
    int state_warmup; // 1 = warmup, 0 = active
    int is_paused;
    uint64_t start_time;
    // rolling stats
    double roll_price_avg, roll_stddev, roll_p_min, roll_p_max;
    double roll_vol_avg, roll_vol_slope;
    double slope_pct;
    int roll_count;
    // long window
    double long_slope_pct;
    int long_count;
    // buy gate
    double buy_p, buy_v;
    double gate_dist, gate_dist_pct;
    double spacing, spacing_pct;
    int stddev_mode;
    int gate_direction; // 0 = buy below (MR), 1 = buy above (momentum)
    double live_offset, live_vmult, live_sm;
    int long_gate_enabled, long_gate_ok;
    double long_rel_slope, long_min_ls;
    // portfolio
    int active_count;
    int max_positions;
    double equity, balance, starting;
    double total_value, total_qty;
    double exposure_pct, max_exp;
    double fees, fee_rate_pct;
    // positions
    TUIPositionSnap positions[16];
    // P&L
    double realized, unrealized, total_pnl, return_pct;
    // graph history (ring buffers, updated every snapshot copy)
    static constexpr int GRAPH_LEN = 120;  // ~2 min at 1 update/sec
    double price_history[GRAPH_LEN];
    double volume_history[GRAPH_LEN];
    double pnl_history[GRAPH_LEN];
    int graph_head;
    int graph_count;
    // risk
    double risk_amt, max_dd;
    int breaker_tripped;
    // regime
    int current_regime;   // REGIME_RANGING, REGIME_TRENDING, REGIME_VOLATILE
    int strategy_id;      // STRATEGY_MEAN_REVERSION or STRATEGY_MOMENTUM
    double regime_duration_min; // minutes in current regime
    double short_r2;      // price regression R² (short window)
    double long_r2;       // price regression R² (long window)
    double vol_ratio;     // short/long variance ratio (volatility spike)
    double ror_slope;     // slope-of-slopes (trend acceleration)
    double volume_spike_ratio; // current volume / rolling max (spike detection)
    int spike_active;     // 1 if spike_ratio >= threshold
    int sl_cooldown;      // remaining slow-path cycles in post-SL cooldown
    int min_warmup_samples; // configured minimum for warmup display
    int engine_state;     // 0=warmup, 1=active, 2=closing
    // config display
    double cfg_tp, cfg_sl, cfg_fee, cfg_slippage;
    int trailing_enabled;
    int live_trading;      // 1 = use_real_money enabled
    double cfg_hold_score, cfg_trail_mult, cfg_sl_trail_mult;
    double cfg_offset_val; // offset pct or stddev mult depending on mode
    // stats
    uint32_t total_buys, wins, losses;
    double win_rate, profit_factor, avg_win, avg_loss, avg_hold;
    // latency
#ifdef LATENCY_PROFILING
    double hot_avg_ns, hot_min_ns, hot_max_ns, hot_p50_ns, hot_p95_ns;
    uint64_t hot_count;
    double slow_avg_ns, slow_min_ns, slow_max_ns;
    uint64_t slow_count;
    // per-component hot path breakdown
    double bg_avg_ns, bg_max_ns;   // BuyGate
    double eg_avg_ns, eg_max_ns;   // ExitGate
    double eg_per_pos_ns;          // ExitGate per active position
    double pc_avg_ns, pc_max_ns;   // PortfolioController_Tick
    // fill vs no-fill PCTick breakdown
    double pc_fill_avg_ns, pc_fill_max_ns;
    uint64_t pc_fill_count;
    double pc_nofill_avg_ns, pc_nofill_max_ns;
    uint64_t pc_nofill_count;
#endif
    // right panel: session stats + fill diagnostics
    double session_high, session_low;
    double tick_rate;
    uint32_t fills_rejected;
    int last_reject_reason;  // 0=none, 1=spacing, 2=balance, 3=exposure, 4=breaker, 5=full, 6=dup
};

//======================================================================================================
// [SHARED STATE]
//======================================================================================================
struct TUISharedState {
    TUISnapshot snapshots[2];
    volatile int active_idx;
    volatile sig_atomic_t quit_requested;
    volatile sig_atomic_t pause_requested;
    volatile sig_atomic_t reload_requested;
    volatile sig_atomic_t regime_cycle_requested;
    EngineTUI tui;
    const char *config_path;
};

//======================================================================================================
// [SNAPSHOT COPY]
//======================================================================================================
// runs on engine thread, every slow-path cycle. converts FPN→double.
//======================================================================================================
template <unsigned F>
static inline void TUI_CopySnapshot(TUISnapshot *snap,
                                      const PortfolioController<F> *ctrl,
                                      const DataStream<F> *stream) {
    snap->price  = stream->price_d;   // use stashed double (no FPN_ToDouble)
    snap->volume = stream->volume_d;
    snap->state_warmup = (ctrl->state == CONTROLLER_WARMUP);
    snap->is_paused = FPN_IsZero(ctrl->buy_conds.price) && !snap->state_warmup;
    snap->start_time = 0; // TUI thread computes uptime from its own start_time

    // rolling stats
    double avg = FPN_ToDouble(ctrl->rolling.price_avg);
    double slope = FPN_ToDouble(ctrl->rolling.price_slope);
    snap->roll_price_avg = avg;
    snap->roll_stddev    = FPN_ToDouble(ctrl->rolling.price_stddev);
    snap->roll_p_min     = FPN_ToDouble(ctrl->rolling.price_min);
    snap->roll_p_max     = FPN_ToDouble(ctrl->rolling.price_max);
    snap->roll_vol_avg   = FPN_ToDouble(ctrl->rolling.volume_avg);
    snap->roll_vol_slope = FPN_ToDouble(ctrl->rolling.volume_slope);
    snap->slope_pct      = (avg != 0.0) ? (slope / avg) * 100.0 : 0.0;
    snap->roll_count     = ctrl->rolling.count;

    // long window
    double long_slope = FPN_ToDouble(ctrl->rolling_long->price_slope);
    double long_avg   = FPN_ToDouble(ctrl->rolling_long->price_avg);
    snap->long_slope_pct = (long_avg != 0.0) ? (long_slope / long_avg) * 100.0 : 0.0;
    snap->long_count     = ctrl->rolling_long->count;

    // buy gate
    double buy_p = FPN_ToDouble(ctrl->buy_conds.price);
    snap->buy_p = buy_p;
    snap->buy_v = FPN_ToDouble(ctrl->buy_conds.volume);
    snap->gate_dist     = snap->price - buy_p;
    snap->gate_dist_pct = (avg != 0.0) ? (snap->gate_dist / avg) * 100.0 : 0.0;
    double spacing = FPN_ToDouble(RollingStats_EntrySpacing(&ctrl->rolling, ctrl->config.spacing_multiplier));
    snap->spacing     = spacing;
    snap->spacing_pct = (avg != 0.0) ? (spacing / avg) * 100.0 : 0.0;
    snap->stddev_mode = !FPN_IsZero(ctrl->config.offset_stddev_mult);
    snap->gate_direction = ctrl->buy_conds.gate_direction;
    snap->live_offset = FPN_ToDouble(ctrl->mean_rev.live_offset_pct) * 100.0;
    snap->live_vmult  = FPN_ToDouble(ctrl->mean_rev.live_vol_mult);
    snap->live_sm     = FPN_ToDouble(ctrl->mean_rev.live_stddev_mult);
    snap->long_gate_enabled = !FPN_IsZero(ctrl->config.min_long_slope);
    double min_ls = FPN_ToDouble(ctrl->config.min_long_slope);
    snap->long_min_ls = min_ls;
    snap->long_rel_slope = (long_avg != 0.0) ? long_slope / long_avg : 0.0;
    snap->long_gate_ok = !snap->long_gate_enabled || (snap->long_rel_slope >= min_ls);

    // portfolio + positions
    double price_d = snap->price;
    double fee_r = FPN_ToDouble(ctrl->config.fee_rate);
    snap->active_count = Portfolio_CountActive(&ctrl->portfolio);
    snap->max_positions = (int)ctrl->config.max_positions;
    snap->total_value = 0.0;
    snap->total_qty   = 0.0;
    uint16_t active = ctrl->portfolio.active_bitmap;
    for (int i = 0; i < 16; i++) snap->positions[i].idx = -1;
    while (active) {
        int idx = __builtin_ctz(active);
        const Position<F> *pos = &ctrl->portfolio.positions[idx];
        TUIPositionSnap *ps = &snap->positions[idx];
        ps->idx      = idx;
        ps->entry    = FPN_ToDouble(pos->entry_price);
        ps->qty      = FPN_ToDouble(pos->quantity);
        ps->tp       = FPN_ToDouble(pos->take_profit_price);
        ps->sl       = FPN_ToDouble(pos->stop_loss_price);
        ps->orig_tp  = FPN_ToDouble(pos->original_tp);
        ps->value    = price_d * ps->qty;
        ps->gross_pnl = (ps->entry != 0.0) ? ((price_d - ps->entry) / ps->entry) * 100.0 : 0.0;
        ps->net_pnl   = ps->gross_pnl - (fee_r * 200.0);
        ps->is_trailing  = !FPN_Equal(pos->take_profit_price, pos->original_tp);
        ps->above_orig_tp = (price_d > ps->orig_tp) && (ps->entry != 0.0);
        ps->ticks_held   = ctrl->total_ticks - ctrl->entry_ticks[idx];
        ps->hold_minutes = (ctrl->entry_time[idx] > 0)
            ? difftime(time(NULL), ctrl->entry_time[idx]) / 60.0 : 0.0;
        snap->total_value += ps->value;
        snap->total_qty   += ps->qty;
        active &= active - 1;
    }

    // financials
    double starting = FPN_ToDouble(ctrl->config.starting_balance);
    double balance  = FPN_ToDouble(ctrl->balance);
    double realized = FPN_ToDouble(ctrl->realized_pnl);
    double unrealized = FPN_ToDouble(ctrl->portfolio_delta);
    snap->balance    = balance;
    snap->starting   = starting;
    snap->realized   = realized;
    snap->unrealized = unrealized;
    snap->equity     = balance + snap->total_value;
    snap->total_pnl  = snap->equity - starting; // derive from equity (always correct)
    snap->return_pct = (starting != 0.0) ? (snap->total_pnl / starting) * 100.0 : 0.0;
    snap->exposure_pct = (starting != 0.0) ? ((starting - balance) / starting) * 100.0 : 0.0;
    snap->max_exp    = FPN_ToDouble(ctrl->config.max_exposure_pct) * 100.0;
    snap->fees       = FPN_ToDouble(ctrl->total_fees);
    snap->fee_rate_pct = fee_r * 100.0;
    snap->risk_amt   = FPN_ToDouble(ctrl->config.risk_pct) * 100.0;
    snap->max_dd     = FPN_ToDouble(ctrl->config.max_drawdown_pct) * 100.0;
    snap->breaker_tripped = (snap->total_pnl < -(starting * FPN_ToDouble(ctrl->config.max_drawdown_pct)));

    // regime
    snap->current_regime = ctrl->regime.current_regime;
    snap->strategy_id    = ctrl->strategy_id;
    snap->regime_duration_min = difftime(time(NULL), ctrl->regime.regime_start_time) / 60.0;
    snap->short_r2   = FPN_ToDouble(ctrl->rolling.price_r_squared);
    snap->long_r2    = FPN_ToDouble(ctrl->rolling_long->price_r_squared);
    snap->engine_state = ctrl->state;
    // variance ratio: short/long (volatility spike detection)
    double sv = FPN_ToDouble(ctrl->rolling.price_variance);
    double lv = FPN_ToDouble(ctrl->rolling_long->price_variance);
    snap->vol_ratio  = (lv > 1e-15) ? sv / lv : 1.0;
    // ROR: compute if ready
    snap->ror_slope  = 0.0;
    if (ctrl->regime_ror.count >= MAX_WINDOW) {
        LinearRegression3XResult<F> ror_r = RORRegressor_Compute(
            const_cast<RORRegressor<F>*>(&ctrl->regime_ror));
        snap->ror_slope = FPN_ToDouble(ror_r.model.slope);
    }
    // volume spike
    snap->volume_spike_ratio = FPN_ToDouble(ctrl->volume_spike_ratio);
    snap->spike_active = FPN_GreaterThanOrEqual(ctrl->volume_spike_ratio,
                                                 ctrl->config.spike_threshold);
    snap->sl_cooldown = (int)ctrl->sl_cooldown_counter;
    snap->min_warmup_samples = (int)ctrl->config.min_warmup_samples;
    // session stats + fill diagnostics
    snap->session_high = ctrl->session_high;
    snap->session_low = ctrl->session_low;
    snap->tick_rate = (double)ctrl->total_ticks;  // raw count, TUI computes rate from uptime
    snap->fills_rejected = ctrl->fills_rejected;
    snap->last_reject_reason = ctrl->last_reject_reason;

    // config
    snap->cfg_tp  = FPN_ToDouble(ctrl->config.take_profit_pct) * 100.0;
    snap->cfg_sl  = FPN_ToDouble(ctrl->config.stop_loss_pct) * 100.0;
    snap->cfg_fee = fee_r * 100.0;
    snap->cfg_slippage = FPN_ToDouble(ctrl->config.slippage_pct) * 100.0;
    snap->live_trading = ctrl->config.use_real_money;
    snap->trailing_enabled = !FPN_IsZero(ctrl->config.tp_hold_score);
    snap->cfg_hold_score   = FPN_ToDouble(ctrl->config.tp_hold_score);
    snap->cfg_trail_mult   = FPN_ToDouble(ctrl->config.tp_trail_mult);
    snap->cfg_sl_trail_mult = FPN_ToDouble(ctrl->config.sl_trail_mult);
    snap->cfg_offset_val = snap->stddev_mode
        ? FPN_ToDouble(ctrl->config.offset_stddev_mult)
        : FPN_ToDouble(ctrl->config.entry_offset_pct) * 100.0;

    // stats
    snap->total_buys = ctrl->total_buys;
    snap->wins       = ctrl->wins;
    snap->losses     = ctrl->losses;
    uint32_t total_exits = ctrl->wins + ctrl->losses;
    snap->win_rate      = (total_exits > 0) ? ((double)ctrl->wins / total_exits) * 100.0 : 0.0;
    double g_wins  = FPN_ToDouble(ctrl->gross_wins);
    double g_losses = FPN_ToDouble(ctrl->gross_losses);
    snap->profit_factor = (g_losses > 0.001) ? g_wins / g_losses : 0.0;
    snap->avg_win  = (ctrl->wins > 0)  ? g_wins / ctrl->wins : 0.0;
    snap->avg_loss = (ctrl->losses > 0) ? g_losses / ctrl->losses : 0.0;
    snap->avg_hold = (total_exits > 0)  ? (double)ctrl->total_hold_ticks / total_exits : 0.0;
}

//======================================================================================================
// [RENDER FROM SNAPSHOT]
//======================================================================================================
// runs on TUI thread. reads only from snapshot (all doubles, no FPN).
//======================================================================================================
static inline void TUI_Render_Snapshot(EngineTUI *tui, const TUISnapshot *s) {
    if (!tui->enabled) return;

    uint64_t now = (uint64_t)time(NULL);
    uint64_t elapsed = now - tui->start_time;
    uint32_t hours = (uint32_t)(elapsed / 3600);
    uint32_t mins  = (uint32_t)((elapsed % 3600) / 60);
    uint32_t secs  = (uint32_t)(elapsed % 60);
    const char *state_str = s->state_warmup ? "WARMUP" : "ACTIVE";

    // pre-render positions
    #define SNAP_POS_MAX 70
    #define SNAP_POS_W 200
    char pos_buf[SNAP_POS_MAX][SNAP_POS_W];
    int pln = 0;
    snprintf(pos_buf[pln++], SNAP_POS_W, C_BOLD C_PEACH "POSITIONS " C_DIM "(%d/16):" C_RESET, s->active_count);
    int displayed = 0;
    for (int i = 0; i < 16; i++) {
        const TUIPositionSnap *ps = &s->positions[i];
        if (ps->idx < 0) continue;
        double diff = s->price - ps->entry;
        if (displayed > 0)
            snprintf(pos_buf[pln++], SNAP_POS_W, C_SURF "·" C_RESET);
        snprintf(pos_buf[pln++], SNAP_POS_W,
                 C_WHEAT "#%-2d " C_FG "$%.2f" C_DIM "->" C_WHEAT "$%.2f %s%+.2f" C_RESET,
                 displayed, ps->entry, s->price, C_PNL(diff), diff);
        snprintf(pos_buf[pln++], SNAP_POS_W,
                 C_SAND "    qty:" C_FG "%.6f" C_SAND " val:" C_FG "$%.2f" C_RESET, ps->qty, ps->value);
        const char *trail_status = "";
        if (ps->above_orig_tp && ps->is_trailing)
            trail_status = C_BOLD C_YELLOW " HOLDING" C_RESET;
        else if (ps->is_trailing)
            trail_status = C_YELLOW " trail" C_RESET;
        snprintf(pos_buf[pln++], SNAP_POS_W,
                 C_SAND "    TP:" C_GREEN "$%.0f" C_RESET "%s" C_SAND " SL:" C_RED "$%.0f" C_RESET,
                 ps->tp, trail_status, ps->sl);
        snprintf(pos_buf[pln++], SNAP_POS_W,
                 C_SAND "    g:" "%s%+.2f%%" C_SAND " n:" "%s%+.2f%%" C_DIM " hold:%.0fm" C_RESET,
                 C_PNL(ps->gross_pnl), ps->gross_pnl, C_PNL(ps->net_pnl), ps->net_pnl, ps->hold_minutes);
        displayed++;
        if (pln >= SNAP_POS_MAX - 4) break;
    }

    // left column
    printf("\033[H\033[2J");
    int row = 1;
    printf(C_SAND "  ================================================================" C_RESET "\n"); row++;
    printf(C_BOLD C_PEACH "     /\\_/\\   FOXML TRADER" C_RESET "\n"); row++;
    printf(C_BOLD C_PEACH "    ( o.o )  " C_WHEAT "engine v3.0.27" C_RESET "\n"); row++;
    printf(C_BOLD C_PEACH "     > ^ <" C_RESET "\n"); row++;
    printf(C_SAND "  ================================================================" C_RESET "\n"); row++;
    printf(C_SAND "  STATE: " C_FG "%-8s" C_RESET C_DIM "  |  " C_SAND "UPTIME: " C_FG "%02u:%02u:%02u" C_RESET "%s\n",
           state_str, hours, mins, secs,
           s->is_paused ? C_DIM "  |  " C_BOLD C_YELLOW "PAUSED" C_RESET : ""); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;
    printf(C_SAND "  PRICE: " C_BOLD C_WHEAT "%-12.2f" C_RESET C_DIM "  |  " C_SAND "VOLUME: " C_FG "%-12.8f" C_RESET "\n", s->price, s->volume); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;
    int pos_start_row = row;

    // market structure
    const char *trend_color = (s->slope_pct > 0.001) ? C_GREEN : (s->slope_pct < -0.001) ? C_RED : C_DIM;
    const char *trend_str   = (s->slope_pct > 0.001) ? "UP" : (s->slope_pct < -0.001) ? "DOWN" : "FLAT";
    const char *lt_color = (s->long_slope_pct > 0.001) ? C_GREEN : (s->long_slope_pct < -0.001) ? C_RED : C_DIM;
    const char *lt_str   = (s->long_slope_pct > 0.001) ? "UP" : (s->long_slope_pct < -0.001) ? "DOWN" : "FLAT";

    printf(C_BOLD C_PEACH "  MARKET STRUCTURE " C_DIM "(rolling %d-tick window):" C_RESET "\n", s->roll_count); row++;
    printf(C_SAND "    avg price:  " C_FG "%-12.2f" C_DIM "  |  " C_SAND "stddev: " C_FG "%-10.2f" C_RESET "\n", s->roll_price_avg, s->roll_stddev); row++;
    printf(C_SAND "    range:      " C_FG "%-12.2f" C_DIM "  -  " C_FG "%-12.2f" C_RESET "\n", s->roll_p_min, s->roll_p_max); row++;
    printf(C_SAND "    avg volume: " C_FG "%-12.8f" C_DIM "  |  " C_SAND "vol slope: " C_FG "%+.8f" C_RESET "\n", s->roll_vol_avg, s->roll_vol_slope); row++;
    printf(C_SAND "    price slope: " C_FG "%+.6f%%/tick" C_DIM "  |  " C_SAND "trend: %s%s" C_RESET "\n", s->slope_pct, trend_color, trend_str); row++;
    printf(C_SAND "    long window " C_DIM "(%d-tick):" C_FG " %+.6f%%/tick" C_DIM "  |  %s%s" C_RESET "\n", s->long_count, s->long_slope_pct, lt_color, lt_str); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    // buy gate
    const char *snap_gate_op = s->gate_direction ? ">=" : "<=";
    printf(C_BOLD C_PEACH "  BUY GATE " C_DIM "(adaptive):" C_RESET "\n"); row++;
    if (s->stddev_mode)
        printf(C_SAND "    price %s " C_FG "%-12.2f" C_DIM "  (stddev: %.2fx)" C_RESET "\n", snap_gate_op, s->buy_p, s->live_sm);
    else
        printf(C_SAND "    price %s " C_FG "%-12.2f" C_DIM "  (offset: %.3f%%)" C_RESET "\n", snap_gate_op, s->buy_p, s->live_offset);
    row++;
    printf(C_SAND "    vol   >= " C_FG "%-12.8f" C_DIM "  (mult: %.2fx)" C_RESET "\n", s->buy_v, s->live_vmult); row++;
    if (s->buy_p > 0.01)
        printf(C_SAND "    distance:   " C_FG "$%-10.2f" C_DIM "  (%.3f%% away)" C_RESET "\n", s->gate_dist, s->gate_dist_pct);
    else
        printf(C_SAND "    distance:   " C_DIM "—  (gate disabled)" C_RESET "\n");
    row++;
    printf(C_SAND "    spacing:    " C_FG "$%-10.2f" C_DIM "  (%.3f%% of avg)" C_RESET "\n", s->spacing, s->spacing_pct); row++;
    if (s->long_gate_enabled) {
        if (s->long_gate_ok)
            printf(C_SAND "    long trend: " C_GREEN "OK" C_RESET "\n");
        else
            printf(C_SAND "    long trend: " C_BOLD C_RED "BLOCKED" C_RESET C_DIM " (%+.6f < %+.6f)" C_RESET "\n", s->long_rel_slope, s->long_min_ls);
        row++;
    }
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    // portfolio
    printf(C_BOLD C_PEACH "  PORTFOLIO:" C_RESET "\n"); row++;
    printf(C_SAND "    equity:     " C_BOLD C_FG "$%-12.4f" C_RESET C_DIM "  (cash + positions)" C_RESET "\n", s->equity); row++;
    printf(C_SAND "    balance:    " C_FG "$%-12.4f" C_RESET C_DIM "  (started: $%.0f)" C_RESET "\n", s->balance, s->starting); row++;
    printf(C_SAND "    held:       " C_FG "$%-12.4f" C_RESET C_DIM "  (qty: %.6f)" C_RESET "\n", s->total_value, s->total_qty); row++;
    printf(C_SAND "    exposure:   " C_FG "%.1f%%/%.0f%%" C_RESET C_DIM "  |  " C_SAND "fees: " C_FG "$%.4f" C_DIM " (%.1f%%)" C_RESET "\n",
           s->exposure_pct, s->max_exp, s->fees, s->fee_rate_pct); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    // P&L
    printf(C_BOLD C_PEACH "  P&L:" C_RESET "\n"); row++;
    printf(C_SAND "    realized:   %s$%-+12.4f" C_RESET C_DIM "  (after fees)" C_RESET "\n", C_PNL(s->realized), s->realized); row++;
    printf(C_SAND "    unrealized: %s$%-+12.4f" C_RESET C_DIM "  (open positions)" C_RESET "\n", C_PNL(s->unrealized), s->unrealized); row++;
    printf(C_SAND "    total:      " C_BOLD "%s$%-+12.4f" C_RESET C_DIM "  (%s%+.2f%%" C_DIM ")" C_RESET "\n",
           C_PNL(s->total_pnl), s->total_pnl, C_PNL(s->return_pct), s->return_pct); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    // risk
    printf(C_BOLD C_PEACH "  RISK:" C_RESET "\n"); row++;
    printf(C_SAND "    risk/pos:   " C_FG "%.1f%%" C_RESET C_DIM "  |  " C_SAND "breaker: %s%s" C_RESET C_DIM " (max dd: %.0f%%)" C_RESET "\n",
           s->risk_amt, s->breaker_tripped ? C_BOLD C_RED : C_GREEN, s->breaker_tripped ? "TRIPPED" : "OK", s->max_dd); row++;
    {
        const char *strat_name = (s->strategy_id == STRATEGY_MOMENTUM) ? "MOMENTUM" : "MEAN REVERSION";
        const char *regime_name = (s->current_regime == REGIME_TRENDING) ? "TRENDING" :
                                  (s->current_regime == REGIME_VOLATILE) ? "VOLATILE" : "RANGING";
        const char *regime_color = (s->current_regime == REGIME_TRENDING) ? C_GREEN :
                                   (s->current_regime == REGIME_VOLATILE) ? C_RED : C_DIM;
        printf(C_SAND "    strategy:   " C_FG "%s" C_RESET C_DIM " (%s)  |  " C_YELLOW "PAPER" C_RESET "\n",
               strat_name, s->stddev_mode ? "stddev" : "pct"); row++;
        printf(C_SAND "    regime:     %s%s" C_RESET C_DIM " (%.0fm)" C_RESET "\n",
               regime_color, regime_name, s->regime_duration_min); row++;
    }
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    // config
    printf(C_BOLD C_PEACH "  CONFIG:" C_RESET "\n"); row++;
    printf(C_SAND "    TP: " C_FG "%.1f%%" C_RESET C_SAND "  SL: " C_FG "%.1f%%" C_RESET
           C_SAND "  risk: " C_FG "%.1f%%" C_RESET C_SAND "  fee: " C_FG "%.1f%%" C_RESET "\n",
           s->cfg_tp, s->cfg_sl, s->risk_amt, s->cfg_fee); row++;
    if (s->stddev_mode)
        printf(C_SAND "    offset: " C_FG "stddev %.1fx" C_RESET, s->cfg_offset_val);
    else
        printf(C_SAND "    offset: " C_FG "%.3f%%" C_RESET, s->cfg_offset_val);
    if (s->trailing_enabled)
        printf(C_SAND "  trail: " C_FG "%.1f" C_DIM "σ" C_RESET C_SAND " sl: " C_FG "%.1f" C_DIM "σ" C_RESET
               C_SAND " score: " C_FG "%.2f" C_RESET, s->cfg_trail_mult, s->cfg_sl_trail_mult, s->cfg_hold_score);
    else
        printf(C_DIM "  trailing: off" C_RESET);
    printf("\n"); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    // stats
    uint32_t total_exits = s->wins + s->losses;
    printf(C_BOLD C_PEACH "  STATS:" C_RESET "\n"); row++;
    printf(C_SAND "    buys: " C_FG "%-4u" C_RESET C_DIM "  |  " C_SAND "exits: " C_FG "%-4u" C_RESET
           C_DIM "  |  " C_SAND "hold: " C_FG "%.0f ticks" C_RESET "\n", s->total_buys, total_exits, s->avg_hold); row++;
    printf(C_SAND "    wins: " C_GREEN "%-4u" C_RESET C_SAND "  losses: " C_RED "%-4u" C_RESET
           C_SAND "  rate: %s%.1f%%" C_RESET C_DIM "  |  " C_SAND "pf: %s%.2f" C_RESET "\n",
           s->wins, s->losses,
           (s->win_rate >= 50.0) ? C_GREEN : (total_exits > 0 ? C_RED : C_DIM), s->win_rate,
           (s->profit_factor >= 1.0) ? C_GREEN : (total_exits > 0 ? C_RED : C_DIM), s->profit_factor); row++;
    printf(C_SAND "    avg win: " C_GREEN "$%.4f" C_RESET C_SAND "  avg loss: " C_RED "$%.4f" C_RESET "\n",
           s->avg_win, s->avg_loss); row++;
    printf(C_DIM "    log: btcusdt_order_history.csv" C_RESET "\n"); row++;
    printf(C_SAND "  ================================================" C_RESET "\n"); row++;

#ifdef LATENCY_PROFILING
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;
    printf(C_BOLD C_PEACH "  LATENCY " C_DIM "(profiling, multicore):" C_RESET "\n"); row++;
    if (s->hot_count > 0) {
        printf(C_SAND "    hot path:  " C_FG "avg %.0fns" C_DIM "  min " C_FG "%.0fns" C_DIM "  max " C_FG "%.0fns"
               C_DIM "  (%lu ticks)" C_RESET "\n", s->hot_avg_ns, s->hot_min_ns, s->hot_max_ns,
               (unsigned long)s->hot_count); row++;
        printf(C_DIM "               p50 " C_FG "%.0fns" C_DIM "  p95 " C_FG "%.0fns" C_RESET "\n",
               s->hot_p50_ns, s->hot_p95_ns); row++;
        printf(C_DIM "      buygate:  " C_FG "avg %.0fns" C_DIM "  max " C_FG "%.0fns" C_RESET "\n", s->bg_avg_ns, s->bg_max_ns); row++;
        printf(C_DIM "      exitgate: " C_FG "avg %.0fns" C_DIM "  max " C_FG "%.0fns"
               C_DIM "  (%.0fns/pos)" C_RESET "\n", s->eg_avg_ns, s->eg_max_ns, s->eg_per_pos_ns); row++;
        printf(C_DIM "      pctick:   " C_FG "avg %.0fns" C_DIM "  max " C_FG "%.0fns" C_RESET "\n", s->pc_avg_ns, s->pc_max_ns); row++;
        if (s->pc_nofill_count > 0)
            { printf(C_DIM "        no-fill: " C_FG "avg %.0fns" C_DIM "  max " C_FG "%.0fns"
                   C_DIM "  (%lu)" C_RESET "\n", s->pc_nofill_avg_ns, s->pc_nofill_max_ns, (unsigned long)s->pc_nofill_count); row++; }
        if (s->pc_fill_count > 0)
            { printf(C_DIM "        fill:    " C_FG "avg %.0fns" C_DIM "  max " C_FG "%.0fns"
                   C_DIM "  (%lu)" C_RESET "\n", s->pc_fill_avg_ns, s->pc_fill_max_ns, (unsigned long)s->pc_fill_count); row++; }
    }
    if (s->slow_count > 0) {
        const char *su = (s->slow_avg_ns >= 1000.0) ? "us" : "ns";
        double sa = (s->slow_avg_ns >= 1000.0) ? s->slow_avg_ns / 1000.0 : s->slow_avg_ns;
        double sn = (s->slow_min_ns >= 1000.0) ? s->slow_min_ns / 1000.0 : s->slow_min_ns;
        double sx = (s->slow_max_ns >= 1000.0) ? s->slow_max_ns / 1000.0 : s->slow_max_ns;
        printf(C_SAND "    slow path: " C_FG "avg %.1f%s" C_DIM "  min " C_FG "%.1f%s" C_DIM "  max " C_FG "%.1f%s"
               C_DIM "  (%lu cycles)" C_RESET "\n", sa, su, sn, su, sx, su, (unsigned long)s->slow_count); row++;
    }
#endif

    // pad left column if right column (positions) extends further down
    int pos_end_row = pos_start_row + pln;
    while (row < pos_end_row) { printf("\n"); row++; }

    printf(C_PINK "  [q]" C_DIM "uit  " C_PINK "[p]" C_DIM "ause  " C_PINK "[r]" C_DIM "eload  " C_PINK "[s]" C_DIM "witch regime" C_RESET "    \n"); row++;

    // right column positions
    int sep_col = 66;
    for (int i = 0; i < pln; i++) {
        printf("\033[%d;%dH" C_SURF "||" C_RESET " %s", pos_start_row + i, sep_col, pos_buf[i]);
    }
    fflush(stdout);
}

//======================================================================================================
// [TUI READ KEY]
//======================================================================================================
static inline char TUI_ReadKey(EngineTUI *tui) {
    if (!tui->enabled) return 0;
    char c = 0;
    read(STDIN_FILENO, &c, 1);
    return c;
}

//======================================================================================================
// [TUI THREAD FUNCTION]
//======================================================================================================
#ifdef USE_NOTCURSES
#include "TUINotcurses.hpp"
#elif defined(USE_ANSI_TUI)
#include "TUIAnsi.hpp"
#elif defined(USE_FTXUI)
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/loop.hpp>
#include "TUILayout.hpp"
#endif

static inline void *tui_thread_fn(void *arg) {
    TUISharedState *shared = (TUISharedState *)arg;

#ifdef USE_NOTCURSES
    // notcurses manages terminal state internally (raw mode, cursor, resize)
    setlocale(LC_ALL, "");

    notcurses_options nc_opts = {};
    nc_opts.flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_NO_ALTERNATE_SCREEN
                  | NCOPTION_NO_FONT_CHANGES | NCOPTION_DRAIN_INPUT
                  | NCOPTION_NO_QUIT_SIGHANDLERS | NCOPTION_INHIBIT_SETLOCALE;
    struct notcurses *nc = notcurses_core_init(&nc_opts, stdout);
    if (!nc) {
        fprintf(stderr, "[TUI] notcurses_init failed\n");
        return NULL;
    }

    struct ncplane *stdp = notcurses_stdplane(nc);
    unsigned term_h, term_w;
    ncplane_dim_yx(stdp, &term_h, &term_w);
    fprintf(stderr, "[TUI] notcurses init OK: %ux%u, TERM=%s\n",
            term_w, term_h, getenv("TERM") ? getenv("TERM") : "(null)");

    int current_layout = NC_LAYOUT_STANDARD;

    // create chart planes (for CHARTS layout)
    NCCharts charts = NC_Charts_Create(stdp, term_h, term_w);

    while (!__atomic_load_n(&shared->quit_requested, __ATOMIC_ACQUIRE)) {
        // read snapshot
        int idx = __atomic_load_n(&shared->active_idx, __ATOMIC_ACQUIRE);
        const TUISnapshot *s = &shared->snapshots[idx];

        // check for terminal resize
        unsigned new_h, new_w;
        ncplane_dim_yx(stdp, &new_h, &new_w);
        if (new_h != term_h || new_w != term_w) {
            term_h = new_h;
            term_w = new_w;
            NC_Charts_Destroy(&charts);
            charts = NC_Charts_Create(stdp, term_h, term_w);
        }

        // clear and render
        ncplane_erase(stdp);
        NC_Layout_Render(stdp, s, charts.price_plot, charts.pnl_plot,
                         current_layout, term_h, term_w);

        // update chart data (only visible in CHARTS layout but always fed)
        NC_Charts_Update(&charts, s);

        notcurses_render(nc);

        // non-blocking input
        struct ncinput ni;
        uint32_t key = notcurses_get_nblock(nc, &ni);
        if (key == (uint32_t)'q' || key == (uint32_t)'Q')
            __atomic_store_n(&shared->quit_requested, 1, __ATOMIC_RELEASE);
        else if (key == (uint32_t)'p' || key == (uint32_t)'P')
            __atomic_store_n(&shared->pause_requested, 1, __ATOMIC_RELEASE);
        else if (key == (uint32_t)'r' || key == (uint32_t)'R')
            __atomic_store_n(&shared->reload_requested, 1, __ATOMIC_RELEASE);
        else if (key == (uint32_t)'s' || key == (uint32_t)'S')
            __atomic_store_n(&shared->regime_cycle_requested, 1, __ATOMIC_RELEASE);
        else if (key == (uint32_t)'l' || key == (uint32_t)'L') {
            current_layout = (current_layout + 1) % NC_LAYOUT_COUNT;
            NC_Charts_Destroy(&charts);
            charts = NC_Charts_Create(stdp, term_h, term_w);
        }

        usleep(100000); // 10 FPS
    }

    NC_Charts_Destroy(&charts);
    notcurses_stop(nc);

#elif defined(USE_ANSI_TUI)
    // raw ANSI TUI — zero library dependencies
    // same terminal management as FTXUI path: raw mode + non-blocking stdin
    struct termios old_term, raw_term;
    tcgetattr(STDIN_FILENO, &old_term);
    raw_term = old_term;
    raw_term.c_lflag &= ~(ICANON | ECHO);
    raw_term.c_cc[VMIN] = 0;
    raw_term.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw_term);

    int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);

    printf("\033[?25l\033[2J");
    fflush(stdout);

    uint64_t tui_start = (uint64_t)time(NULL);
    int current_layout = ANSI_LAYOUT_STANDARD;

    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    int term_w = ws.ws_col, term_h = ws.ws_row;
    int frame_count = 0;

    while (!__atomic_load_n(&shared->quit_requested, __ATOMIC_ACQUIRE)) {
        if (++frame_count % 20 == 0) {
            struct winsize ws2;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws2) == 0) {
                if (ws2.ws_col != term_w || ws2.ws_row != term_h) {
                    term_w = ws2.ws_col;
                    term_h = ws2.ws_row;
                }
            }
        }

        int idx = __atomic_load_n(&shared->active_idx, __ATOMIC_ACQUIRE);
        const TUISnapshot *s = &shared->snapshots[idx];
        ANSI_Render(s, current_layout, term_h, term_w, tui_start);

        char c = 0;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == 'q' || c == 'Q')
                __atomic_store_n(&shared->quit_requested, 1, __ATOMIC_RELEASE);
            else if (c == 'p' || c == 'P')
                __atomic_store_n(&shared->pause_requested, 1, __ATOMIC_RELEASE);
            else if (c == 'r' || c == 'R')
                __atomic_store_n(&shared->reload_requested, 1, __ATOMIC_RELEASE);
            else if (c == 's' || c == 'S')
                __atomic_store_n(&shared->regime_cycle_requested, 1, __ATOMIC_RELEASE);
            else if (c == 'l' || c == 'L')
                current_layout = (current_layout + 1) % ANSI_LAYOUT_COUNT;
        }

        usleep(100000); // 10 FPS
    }

    fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    printf("\033[?25h\033[H\033[J");  // show cursor, home, clear below (not full screen wipe)
    fflush(stdout);

#elif defined(USE_FTXUI)
    // DOM-only FTXUI rendering with manual terminal management
    // set terminal to raw mode + non-blocking stdin
    struct termios old_term, raw_term;
    tcgetattr(STDIN_FILENO, &old_term);
    raw_term = old_term;
    raw_term.c_lflag &= ~(ICANON | ECHO);
    raw_term.c_cc[VMIN] = 0;
    raw_term.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw_term);

    // make stdin non-blocking with fcntl (belt + suspenders)
    int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);

    // hide cursor, clear screen
    printf("\033[?25l\033[2J");
    fflush(stdout);

    int current_layout = LAYOUT_STANDARD;

    // cache terminal size — only update on change to prevent jitter
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    int term_w = ws.ws_col, term_h = ws.ws_row;
    int frame_count = 0;

    while (!__atomic_load_n(&shared->quit_requested, __ATOMIC_ACQUIRE)) {
        // re-check terminal size every 20 frames (~2 sec) to handle resize
        if (++frame_count % 20 == 0) {
            struct winsize ws2;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws2) == 0) {
                if (ws2.ws_col != term_w || ws2.ws_row != term_h) {
                    term_w = ws2.ws_col;
                    term_h = ws2.ws_row;
                    printf("\033[2J"); // clear on resize
                }
            }
        }

        // read snapshot
        int idx = __atomic_load_n(&shared->active_idx, __ATOMIC_ACQUIRE);
        const TUISnapshot *s = &shared->snapshots[idx];

        // render FTXUI element tree to a fixed-size screen buffer
        auto element = Layout_Render(s, current_layout, term_w, term_h);
        auto screen = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(term_w),
            ftxui::Dimension::Fixed(term_h));
        ftxui::Render(screen, element);

        // synchronized output: terminal buffers everything and paints in one pass
        // clear entire screen inside sync block so no stale content interferes
        std::string content = screen.ToString();
        std::string output;
        output.reserve(content.size() + 64);
        output += "\033[?2026h";  // begin synchronized output
        output += "\033[2J";      // clear entire screen
        output += "\033[H";       // cursor home
        output += content;
        output += "\033[?2026l";  // end synchronized output
        write(STDOUT_FILENO, output.data(), output.size());

        // non-blocking keyboard read
        char c = 0;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == 'q' || c == 'Q')
                __atomic_store_n(&shared->quit_requested, 1, __ATOMIC_RELEASE);
            else if (c == 'p' || c == 'P')
                __atomic_store_n(&shared->pause_requested, 1, __ATOMIC_RELEASE);
            else if (c == 'r' || c == 'R')
                __atomic_store_n(&shared->reload_requested, 1, __ATOMIC_RELEASE);
            else if (c == 's' || c == 'S')
                __atomic_store_n(&shared->regime_cycle_requested, 1, __ATOMIC_RELEASE);
            else if (c == 'l' || c == 'L')
                current_layout = (current_layout + 1) % LAYOUT_COUNT;
        }

        usleep(100000); // 10 FPS
    }

    // restore terminal
    fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    printf("\033[?25h\033[2J"); // show cursor, clear screen
    fflush(stdout);

#else
    // legacy printf TUI
    TUI_Init(&shared->tui, 1, 10);

    while (!__atomic_load_n(&shared->quit_requested, __ATOMIC_ACQUIRE)) {
        int idx = __atomic_load_n(&shared->active_idx, __ATOMIC_ACQUIRE);
        TUI_Render_Snapshot(&shared->tui, &shared->snapshots[idx]);

        char c = TUI_ReadKey(&shared->tui);
        if (c == 'q' || c == 'Q')
            __atomic_store_n(&shared->quit_requested, 1, __ATOMIC_RELEASE);
        else if (c == 'p' || c == 'P')
            __atomic_store_n(&shared->pause_requested, 1, __ATOMIC_RELEASE);
        else if (c == 'r' || c == 'R')
            __atomic_store_n(&shared->reload_requested, 1, __ATOMIC_RELEASE);
        else if (c == 's' || c == 'S')
            __atomic_store_n(&shared->regime_cycle_requested, 1, __ATOMIC_RELEASE);

        usleep(100000); // 10 FPS
    }

    TUI_Cleanup(&shared->tui);
#endif // USE_FTXUI

    return NULL;
}

#endif // MULTICORE_TUI

//======================================================================================================
//======================================================================================================
#endif // ENGINE_TUI_HPP
