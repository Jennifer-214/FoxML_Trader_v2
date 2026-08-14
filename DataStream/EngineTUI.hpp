// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[DataStream/EngineTUI.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[TUI thread lifecycle + the TUISnapshot/PerNodeSnap engine->display data contract — seqlock double-buffer publish (v5.11.3.B); read-only display except explicit user commands; the PortfolioController render half is LEGACY single-core]
// [CONTAINS]
//   - [STRUCT]_[EngineTUI]   (session constants + theme + the raw-mode signal handler ride)
//   - [FUNCTION]_[TUI_Init]   (+ TUI_Cleanup)
//   - [FUNCTION]_[TUI_Render]   (LEGACY single-core direct render)
//   - [FUNCTION]_[TUI_HandleInput]   (LEGACY single-core input)
//   - [STRUCT]_[TUIPositionSnap]
//   - [STRUCT]_[MLSnapshot]   (+ MLSnapshot_Populate rides)
//   - [STRUCT]_[TUISnapshot]   (⊃ nested [STRUCT]_[PerNodeSnap] per-node record + the cluster-boundary assert ride)
//   - [STRUCT]_[TUISharedState]
//   - [FUNCTION]_[TUISnapshot_Publish_Begin]   (+ InitSeq / PublishHandle / End / ReadInto / Sequence seqlock family)
//   - [FUNCTION]_[TUI_CopySnapshot]   (3 overloads — the legacy/backtest populate path)
//   - [FUNCTION]_[TUI_PopulatePerCoreLatency]   (+ SlowPathLatency / AdvancedTopology / Topology populator family)
//   - [FUNCTION]_[TUI_Render_Snapshot]   (+ TUI_ReadKey)
//   - [FUNCTION]_[tui_thread_fn]
// [REFERENCE]_[DESIGN_SPEC]_[[cross-thread-snapshot-publish-cluster-isolation] [per-snapshot-cluster-layout-pattern]]
// [REFERENCE]_[INVARIANT]_[H6]
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
#include <stddef.h>   // v5.14.10.0 — offsetof for cluster-boundary static_asserts
#include "../Version.hpp"
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <time.h>
#include <atomic>     // v5.11.3.B — TUISharedState seq counter

#include "../CoreFrameworks/PortfolioController.hpp"
#include "../CoreFrameworks/SPSCRing.hpp"  // v5.0.3: SPSCRing_Depth for Q-depth display
#include "../CoreFrameworks/OrderGates.hpp"
#include "../CoreFrameworks/MetricCompute.hpp"  // v5.8.4c: shared metric helpers
#include "../CoreFrameworks/NodeLatencyStats.hpp"
#include <fcntl.h>
#include <sys/ioctl.h>

using namespace std;

//======================================================================
// [STRUCT]_[EngineTUI]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[render cadence + saved terminal state + optional LATENCY_PROFILING accumulators (session constants + the FoxML truecolor theme + the raw-mode signal handler ride)]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[SESSION CONSTANTS]
//------------------------------------------------------------------
// session IDs match the partitioning in PortfolioController slow-path session detection.
// adding a new session = update this and the bounds in the detection logic.
#define SESSION_ASIAN     0
#define SESSION_EU        1
#define SESSION_US        2
#define SESSION_OVERNIGHT 3
#define NUM_SESSIONS      4

// session names for display (indexed by session id; -1 = no session, render as "")
static const char *SESSION_NAMES[] = {"ASIA", "EU", "US", "OVERNIGHT"};

//------------------------------------------------------------------
// [SECTION]_[FOXML THEME - truecolor ANSI]
//------------------------------------------------------------------
// colors pulled from the FoxML neovim colorscheme palette
// uses 24-bit truecolor: \033[38;2;R;G;Bm (foreground)
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

//------------------------------------------------------------------
// [SECTION]_[TERMINAL RAW MODE]
//------------------------------------------------------------------
// global pointer for signal handler cleanup - only one TUI instance per process
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
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[96B]
// [ALIGN]_[8]
// [CACHE_LINES]_[2]
// [STRADDLE]_[original_term@20]
//======================================================================
// [END_STRUCT]_[EngineTUI]
//======================================================================

//======================================================================
// [FUNCTION]_[TUI_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[raw-mode setup + crash-restore signal handlers + hidden cursor (TUI_Cleanup rides — restores terminal + shows cursor)]
//======================================================================
// [CODE]
//======================================================================
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

//------------------------------------------------------------------
// [SECTION]_[CLEANUP]
//------------------------------------------------------------------
static inline void TUI_Cleanup(EngineTUI *tui) {
    if (!tui->raw_mode_active) return;

    tcsetattr(STDIN_FILENO, TCSANOW, &tui->original_term);
    tui->raw_mode_active = 0;

    // show cursor, clear screen
    printf("\033[?25h\033[2J\033[H");
    fflush(stdout);

    g_tui_instance = NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TUI_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[TUI_Render]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[LEGACY single-core direct render — reads PortfolioController live (no snapshot); every render_interval ticks; cursor-home overwrite to reduce flicker]
//======================================================================
// [CODE]
//======================================================================
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

    // convert FPN_Binary values to doubles for display
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
    printf(C_BOLD C_PEACH "    ( o.o )  " C_WHEAT "v" RELEASE_VERSION_STRING "  (engine " ENGINE_VERSION_STRING ")" C_RESET "\n"); row++;
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
    double risk_amt = FPN_ToDouble(ctrl->config.risk_pct) * 100.0;

    // ==== PORTFOLIO section ====
    double equity = balance + total_value;
    // single source of truth: derive total P&L from equity (same as snapshot path)
    double total_pnl = equity - starting;
    double return_pct = (starting != 0.0) ? (total_pnl / starting) * 100.0 : 0.0;
    double exposure_pct = (starting != 0.0) ? (total_value / starting) * 100.0 : 0.0;
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
    // v5.8.4c: canonical Compute_ProfitFactor (epsilon=0.0001, no sentinel).
    double profit_factor = Compute_ProfitFactor(g_wins, g_losses);
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
    printf(C_DIM "    log: logging/btcusdt_order_history.csv" C_RESET "\n"); row++;
    printf(C_SAND "  ================================================" C_RESET "\n"); row++;
#ifdef LATENCY_PROFILING
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;
    printf(C_BOLD C_PEACH "  LATENCY " C_DIM "(profiling enabled):" C_RESET "\n"); row++;
    if (tui->hot_count > 0) {
        double hot_avg = (double)tui->hot_sum / tui->hot_count / tui->tsc_per_ns;
        double hot_min_ns = (double)tui->hot_min / tui->tsc_per_ns;
        double hot_max_ns = (double)tui->hot_max / tui->tsc_per_ns;
        // percentiles from log2 histogram
        double hot_p50 = 0, hot_p95 = 0, hot_p99 = 0;
        { uint64_t p50t = tui->hot_count/2, p95t = tui->hot_count*95/100, p99t = tui->hot_count*99/100, cum = 0;
          for (int i = 0; i <= 20; i++) { cum += tui->hot_hist[i];
            if (!hot_p50 && cum >= p50t) hot_p50 = (1.5*(1ULL<<i))/tui->tsc_per_ns;
            if (!hot_p95 && cum >= p95t) hot_p95 = (1.5*(1ULL<<i))/tui->tsc_per_ns;
            if (!hot_p99 && cum >= p99t) hot_p99 = (1.5*(1ULL<<i))/tui->tsc_per_ns; } }
        printf(C_SAND "    hot path:  " C_FG "avg %.0fns" C_DIM "  p50 " C_FG "%.0fns"
               C_DIM "  p95 " C_FG "%.0fns" C_DIM "  p99 " C_FG "%.0fns"
               C_DIM "  (%lu)" C_RESET "\n",
               hot_avg, hot_p50, hot_p95, hot_p99, (unsigned long)tui->hot_count); row++;
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TUI_Render]
//======================================================================

//======================================================================
// [FUNCTION]_[TUI_HandleInput]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[LEGACY single-core input — q/p/r/s/k single-char commands acting directly on PortfolioController; pause = zero the buy gate, exit gate keeps running]
//======================================================================
// [CODE]
//======================================================================
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

    if (c == 'k' || c == 'K') {
        if (ctrl->kill_switch_active) {
            KillSwitch_Reset(ctrl);
            fprintf(stderr, "[TUI] kill switch reset — observing for %u cycles before trading\n",
                    ctrl->config.kill_recovery_warmup);
        }
        return 'k';
    }

    return 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TUI_HandleInput]
//======================================================================

//------------------------------------------------------------------
// [SECTION]_[MULTICORE TUI]
//------------------------------------------------------------------
// when MULTICORE_TUI is defined, the TUI runs on a separate thread with its own L1 cache.
// the engine thread copies a snapshot of display state every slow-path cycle.
// the TUI thread reads the snapshot and renders independently.
// zero L1 cache pollution on the engine core.

#ifdef MULTICORE_TUI
#include <pthread.h>

//======================================================================
// [STRUCT]_[TUIPositionSnap]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-position display row — all doubles (no FPN on the TUI thread; the engine converts during snapshot copy); idx = -1 marks an empty slot]
//======================================================================
// [CODE]
//======================================================================
struct TUIPositionSnap {
    int idx;
    double entry, qty, tp, sl, orig_tp;
    double value, gross_pnl, net_pnl;
    int is_trailing, above_orig_tp;
    uint64_t ticks_held;
    double hold_minutes;  // wall clock hold duration
    time_t entry_time;    // wall clock entry timestamp (for chart markers)
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[104B]
// [ALIGN]_[8]
// [CACHE_LINES]_[2]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[TUIPositionSnap]
//======================================================================

//======================================================================
// [STRUCT]_[MLSnapshot]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[FoxML display fields + prediction-history ring (MLSnapshot_Populate rides — ONE populate fn shared by TUI_CopySnapshot and BacktestSnapshot_Copy)]
//======================================================================
// [CODE]
//======================================================================
struct MLSnapshot {
    // summary (Phase 6C)
    double cost_bps;           // estimated trade cost in bps (CostModel)
    double foxml_vol_scale;    // inverse-vol position scale factor (VolScaler)
    double confidence;         // prediction confidence [0, 1] (ConfidenceScorer)
    double bandit_blend;       // bandit effective blend ratio [0, blend_ratio]
    double bandit_weights[5];  // per-strategy bandit weights (normalized)
    int bandit_active;         // 1 if bandit has enough samples to influence
    int cost_gate_enabled;     // config mirror for display
    int foxml_vol_scaling_enabled;
    int confidence_enabled;
    int bandit_enabled;
    // bandit detail (Phase 7C)
    int bandit_pulls[5];       // per-arm pull counts
    double bandit_avg_reward[5]; // per-arm average reward (bps)
    double bandit_probs[5];    // per-arm selection probabilities
    int bandit_total_steps;    // total bandit updates
    char bandit_arm_names[5][32]; // human-readable arm labels
    // confidence detail (Phase 7C)
    double confidence_ic;      // information coefficient (rank correlation)
    double confidence_rmse;    // rolling RMSE of predictions
    double confidence_freshness; // data recency factor [0, 1]
    // cost detail (Phase 7C)
    double cost_spread;        // spread component (bps)
    double cost_timing;        // volatility timing component (bps)
    double cost_impact;        // market impact component (bps)
    double cost_breakeven;     // breakeven return threshold (bps)
    // model info (Phase 7C)
    char ml_model_path[256];
    int ml_model_loaded;       // 1 if buy-signal model active
    double ml_last_prediction; // most recent model output
    char regime_model_path[256];
    int regime_model_loaded;   // 1 if regime model active
    // prediction history ring buffer (Phase 7D)
    static constexpr int PRED_HISTORY_LEN = 240;
    double pred_history[PRED_HISTORY_LEN];
    double conf_history[PRED_HISTORY_LEN];
    int pred_head;
    int pred_count;
};

// populates MLSnapshot from controller state.
// called by TUI_CopySnapshot and BacktestSnapshot_Copy — one copy function, not two.
template <unsigned F>
static inline void MLSnapshot_Populate(MLSnapshot *snap, const PortfolioController<F> *ctrl) {
    // summary fields
    snap->cost_bps = ctrl->last_cost_bps;
    snap->foxml_vol_scale = ctrl->foxml_vol_scale;
    snap->confidence = ctrl->last_confidence;
    snap->cost_gate_enabled = BITMAP_IS_SET(ctrl->config.gate_cfg_flags, MASK_GATE_CFG_COST_GATE_ENABLED);
    snap->foxml_vol_scaling_enabled = BITMAP_IS_SET(ctrl->config.ml_cfg_flags, MASK_ML_CFG_FOXML_VOL_SCALING_ENABLED);
    snap->confidence_enabled = BITMAP_IS_SET(ctrl->config.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_ENABLED);
    snap->bandit_enabled = BITMAP_IS_SET(ctrl->config.ml_cfg_flags, MASK_ML_CFG_BANDIT_ENABLED);

    // bandit detail
    snap->bandit_total_steps = ctrl->bandit.total_steps;
    if (BITMAP_IS_SET(ctrl->config.ml_cfg_flags, MASK_ML_CFG_BANDIT_ENABLED)) {
        snap->bandit_blend = Bandit_EffectiveBlend(&ctrl->bandit);
        snap->bandit_active = (ctrl->bandit.total_steps >= ctrl->bandit.min_samples) ? 1 : 0;
        double bw[BANDIT_MAX_ARMS], bp[BANDIT_MAX_ARMS];
        Bandit_GetWeights(&ctrl->bandit, bw);
        Bandit_GetProbabilities(&ctrl->bandit, bp);
        for (int i = 0; i < 5; i++) {
            snap->bandit_weights[i] = bw[i];
            snap->bandit_probs[i] = bp[i];
            snap->bandit_pulls[i] = ctrl->bandit.pulls[i];
            snap->bandit_avg_reward[i] = (ctrl->bandit.pulls[i] > 0)
                ? ctrl->bandit.cum_reward[i] / ctrl->bandit.pulls[i] : 0.0;
            // v5.15.5.A.3 — arm_names extracted into ctrl->bandit_display_meta.
            strncpy(snap->bandit_arm_names[i], ctrl->bandit_display_meta.arm_names[i], 31);
            snap->bandit_arm_names[i][31] = '\0';
        }
    } else {
        snap->bandit_blend = 0.0;
        snap->bandit_active = 0;
        for (int i = 0; i < 5; i++) {
            snap->bandit_weights[i] = 0.0;
            snap->bandit_probs[i] = 0.0;
            snap->bandit_pulls[i] = 0;
            snap->bandit_avg_reward[i] = 0.0;
            snap->bandit_arm_names[i][0] = '\0';
        }
    }

    // confidence detail — IC and RMSE are the stable components,
    // freshness is time-dependent and computed live by ConfidenceScorer_Compute
    snap->confidence_ic = RollingIC_Compute(&ctrl->confidence.ic);
    snap->confidence_rmse = RollingRMSE_Compute(&ctrl->confidence.rmse);
    snap->confidence_freshness = Confidence_Stability(snap->confidence_rmse); // stability proxy

    // cost detail
    snap->cost_spread = ctrl->last_costs.spread_cost;
    snap->cost_timing = ctrl->last_costs.timing_cost;
    snap->cost_impact = ctrl->last_costs.impact_cost;
    snap->cost_breakeven = CostModel_Breakeven(ctrl->last_cost_bps);

    // model info
    snap->ml_model_loaded = Model_IsLoaded(&ctrl->ml_strategy.buy_model);
    snap->ml_last_prediction = FPN_ToDouble(ctrl->ml_strategy.last_prediction);
    strncpy(snap->ml_model_path, ctrl->ml_strategy.buy_model.model_path, 255);
    snap->ml_model_path[255] = '\0';
    snap->regime_model_loaded = Model_IsLoaded(&ctrl->regime_model);
    strncpy(snap->regime_model_path, ctrl->regime_model.model_path, 255);
    snap->regime_model_path[255] = '\0';

    // push prediction into ring buffer (accumulates across calls)
    if (snap->ml_model_loaded) {
        snap->pred_history[snap->pred_head] = snap->ml_last_prediction;
        snap->conf_history[snap->pred_head] = snap->confidence;
        snap->pred_head = (snap->pred_head + 1) % MLSnapshot::PRED_HISTORY_LEN;
        if (snap->pred_count < MLSnapshot::PRED_HISTORY_LEN)
            snap->pred_count++;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// FoxML display fields — populated once by MLSnapshot_Populate(), shared by
// TUI_CopySnapshot and BacktestSnapshot_Copy.  add new ML fields here.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[4800B]
// [ALIGN]_[8]
// [CACHE_LINES]_[75]
// [STRADDLE]_[bandit_weights@32 · bandit_avg_reward@112]
//======================================================================
// [END_STRUCT]_[MLSnapshot]
//======================================================================

//======================================================================
// [STRUCT]_[TUISnapshot]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [CONCURRENCY]]
// [THREAD]_[[SLOW_PATH_WRITER] [TUI_READER]]
// [STRADDLE_EXEMPT]_[strat_stats]_[seqlock-published bulk-copy record — writer publishes whole, readers copy whole (ReadInto); no per-field concurrent access, straddle affects copy bandwidth only — D-414 leaf-3 2026-08-10]
// [STRADDLE_EXEMPT]_[per_node]_[seqlock-published bulk-copy record — same rationale as strat_stats; element-uniform PerNodeSnap array — D-414 leaf-3 2026-08-10]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[THE engine->display data contract — full dashboard state incl. the PerNodeSnap inner struct (per-core panel; alignas(64) bandit telemetry cluster) + the cluster-boundary assert; copied whole under the seqlock]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-11]
// [REFERENCE]_[DESIGN_SPEC]_[per-snapshot-cluster-layout-pattern.md]
//======================================================================
// [CODE]
//======================================================================
struct TUISnapshot {
    // market
    double price, volume;
    // state
    int state_warmup; // 1 = warmup, 0 = active
    int is_paused;
    uint64_t start_time;
    // v5.9.0c — captured cfg file path. Engine header panel renders this so
    // operators see at-a-glance which cfg drove the configuration. Distinct
    // binaries read different files (engine_gui → engine.cfg, foxml_suite →
    // backtest.cfg) — operator confusion was the root cause of v5.8 paper-
    // test "all DIP" bug. Populated in TUI_CopySnapshot* from
    // ControllerConfig::source_cfg_path.
    char source_cfg_path[256];
    // v5.12.1.C — WS heartbeat. ws_last_tick_us = local wall-clock us at
    // last WS tick received (published by producer fan_out, v5.12.1.A.2).
    // ws_ticks_per_5s = rolling tick count over the last 5 seconds (5 ×
    // 1-second buckets ring-rotated by producer; sums to current 5s
    // throughput). Header bar shows "WS: <ticks/sec>/s, last <ms>ms ago"
    // with green/yellow/red color-coding per gap thresholds.
    uint64_t ws_last_tick_us;
    uint64_t ws_ticks_per_5s;
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
    TUIPositionSnap positions[MAX_PORTFOLIO_POSITIONS];
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
    int buying_halted;
    int halt_reason;
    int gate_reason;            // GATE_REASON_* — specific reason gate is off
    // regime
    int current_regime;   // REGIME_RANGING, REGIME_TRENDING, REGIME_VOLATILE
    int strategy_id;      // STRATEGY_MEAN_REVERSION or STRATEGY_MOMENTUM
    int regime_auto;      // 1 = regime auto mode (default_strategy=-1)
    double regime_duration_min; // minutes in current regime
    double short_r2;      // price regression R² (short window)
    double long_r2;       // price regression R² (long window)
    double vol_ratio;     // short/long variance ratio (volatility spike)
    double ror_slope;     // slope-of-slopes (trend acceleration)
    double ema_sma_spread; // EMA/SMA crossover spread (primary trending signal)
    double volume_spike_ratio; // current volume / rolling max (spike detection)
    int spike_active;     // 1 if spike_ratio >= threshold
    double vwap, vwap_dev; // VWAP and deviation from it
    double ema_price;      // EMA price for proactive gate (0 = disabled)
    double book_imbalance; // bid/ask imbalance [-1, +1] (0 = no depth data)
    double book_spread;    // bid-ask spread
    double danger_score;   // danger gradient [0, 1] — 0=safe, 1=crash
    int current_session;   // 0=asian, 1=european, 2=us, 3=overnight (-1=disabled)
    double session_mult;   // current session gate multiplier
    int sl_cooldown;      // remaining slow-path cycles in post-SL cooldown
    int min_warmup_samples; // configured minimum for warmup display
    int warmup_samples_now; // current rolling sample count (v4.0.4 — for "X / Y" progress)
    int engine_state;     // 0=warmup, 1=active, 2=closing
    // config display
    double cfg_tp, cfg_sl, cfg_fee, cfg_slippage;
    int trailing_enabled;
    int live_trading;      // 1 = live capital (trading_mode==LIVE); NEW-1
    double cfg_hold_score, cfg_trail_mult, cfg_sl_trail_mult;
    double cfg_offset_val; // offset pct or stddev mult depending on mode
    // stats. Two flavors:
    //   total_buys / total_exits_fills — per-fill heartbeat counters
    //     (each leg-fill bumps; a paired trade entry/exit = 2 fills).
    //   wins / losses — per-trade counters (leg-A-only stamped, so 1
    //     paired close = 1 win or 1 loss).
    // Stats panel shows both: e.g. "buys: 3 (6 fills)  exits: 1 (2 fills)".
    uint32_t total_buys, total_exits_fills, wins, losses;
    // v4.7.18: paper_reset_seq mirror so retained-history GUI panels
    // (Per-Core P&L ring, equity curve, etc.) can detect reset events
    // and clear their buffers. Bumped by the engine in the paper-reset
    // handler. Panel saves last_seen value, compares each frame.
    uint32_t paper_reset_seq;
    double win_rate, profit_factor, avg_win, avg_loss, avg_loss_market, avg_hold;
    double expectancy;       // (win_rate * avg_win) - (loss_rate * avg_loss)
    int all_wins_run;        // v5.8.4c: 1 if wins>0 && losses==0; display layer renders "—" / "∞"
    double max_drawdown;     // peak-to-trough equity drop ($)
    double max_drawdown_pct; // peak-to-trough as % of peak
    double fee_ratio;        // total_fees / gross_wins (% of gains eaten by fees)
    // Phase 8 — maker/taker breakdown. Sum of maker_fees + taker_fees should
    // equal total_fees (sanity invariant). maker_fills_count is 0 in legacy /
    // backtest paths (all-taker accounting). Phase 9 hybrid execution will
    // produce non-zero maker counts when POST_ONLY limit fills land.
    uint32_t maker_fills_count;
    uint32_t taker_fills_count;
    double total_maker_fees;
    double total_taker_fees;
    // latency
#ifdef LATENCY_PROFILING
    double hot_avg_ns, hot_min_ns, hot_max_ns, hot_p50_ns, hot_p95_ns, hot_p99_ns;
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
    // kill switch
    int kill_switch_active;
    int kill_reason;        // 0=none, 1=daily_loss, 2=drawdown
    int kill_recovery;      // remaining warmup cycles after kill reset
    // vol-scaled sizing
    double vol_scale;       // last applied vol scale factor
    // no-trade band
    int no_trade_band_blocked; // 1 if last signal was suppressed by no-trade band
    double signal_strength;    // |price - avg| / avg as percentage
    // FoxML integration (Phase 6C) — populated by MLSnapshot_Populate()
    MLSnapshot ml;
    // per-strategy stats
    struct StrategyStatsSnap {
      double pnl;
      uint32_t wins, losses, total;
    };
    // v5.10.3.A — Sized to NUM_STRATEGIES (= NUM_STRATEGIES_REAL + 1 for AUTO
    // sentinel) per parity-check Finding #8. Pre-fix this was [5] but TUIAnsi
    // iterations used NUM_STRATEGIES (=6) → UB at index 5. AUTO bin (idx=5)
    // populated as zero-init by Populate; per-strategy aggregation deferred
    // to v5.11.X polish.
    StrategyStatsSnap strat_stats[NUM_STRATEGIES];
    // right panel: session stats + fill diagnostics
    double session_high, session_low;
    double tick_rate;
    uint32_t fills_rejected;
    int last_reject_reason;  // 0=none, 1=spacing, 2=balance, 3=exposure, 4=breaker, 5=full, 6=dup
    // v5.11.4.B — async log writer health (parity-check 2026-05-07 Section J).
    // The drainer pushes order events to a SPSC ring; a writer pthread drains
    // them (does realloc + fwrite + fflush off the drainer's tail-latency
    // path). These two atomic counters live on oms->event_log; without
    // surfacing them, sustained ring-fullness or writer realloc OOM is a
    // SILENT failure — the drainer keeps spinning + spends time it shouldn't,
    // or events get dropped, and the operator has no GUI signal.
    //
    // Both are monotonic counters (never reset). GUI panels render the raw
    // value; non-zero = something to investigate. Health log emits WARN on
    // first non-zero observation (rate-limited).
    uint64_t oms_log_ring_full_spins;        // total spin/usleep iters in OrderEventLog_Append
    uint64_t oms_log_writer_realloc_failed;  // realloc failures inside async writer thread (legacy — should stay 0 post-v5.11.5.C)
    uint64_t oms_log_full_drops;             // v5.11.5.D — events dropped because mmap'd capacity exhausted (parity-check J.1)
    // Phase 14: per-core latency stats. Populated only when engine_mode ==
    // sharded AND NodeLatencyStats are enabled. Display panel renders only
    // when sharded_mode_active is set.
    int sharded_mode_active;       // 1 = sharded engine running, 0 = legacy
    int partial_exit_enabled;      // 1 = paired-leg geometry (slot 2c+leg)
    int per_node_count;            // number of cores actively reporting
    // v5.0.2 (Engine Topology): system + thread layout for the GUI
    // Engine Topology panel. Populated once at boot in EngineSharded_Run
    // (values are static after thread spawn).
    int16_t producer_cpu;          // pinned CPU for the producer thread
    int16_t drainer_cpu;           // pinned CPU for the drainer thread
    int16_t nproc;                 // sysconf(_SC_NPROCESSORS_ONLN)
    int16_t slow_path_pin_offset;  // raw cfg value (-1 disabled, 0 auto, >0 explicit)
    // ─────────────────────────────────────────────────────────────────
    // PerNodeSnap field-init discipline (v5.9.2c)
    // ─────────────────────────────────────────────────────────────────
    // Adding a new field here requires updating BOTH places:
    //
    //   1. This struct definition (the read side, GUI/TUI consumers)
    //   2. CoreFrameworks/ShardedSnapshot.hpp::TUI_CopySnapshotSharded
    //      populator (the write side, runs every snapshot cycle)
    //
    // OR the field must be deliberately legitimately-zero (e.g. hot-path
    // latency stats `samples`, `*_ns` are zero in sharded mode because
    // the populator doesn't surface them — slow-path latency `sp_*_ns`
    // is the sharded equivalent).
    //
    // The comprehensive parity audit at v5.9.2c (DOCS/changelogs/
    // 2026-05-02-v5.9-ml-hardening.md) verified all 49 actively-
    // populated fields have populator lines and all 14 legitimately-
    // zero fields have a documented reason. Discipline rule applies
    // for any future field addition.
    //
    // Sentinel test below in tests/controller_test.cpp v5.9.2c
    // EXTENSIBILITY block exercises the populator end-to-end with a
    // synthesized NodeContext + asserts representative fields land
    // correctly. This catches "added field but forgot populator" at
    // PR time rather than runtime.
    // ─────────────────────────────────────────────────────────────────
    //==================================================================
    // [STRUCT]_[PerNodeSnap]
    //------------------------------------------------------------------
    // [TAG]_[[ENGINE] [MONITORING_PLANE]]
    // [THREAD]_[[SLOW_PATH_WRITER] [TUI_READER]]
    // [STRADDLE_EXEMPT]_[sp_breakdown_p50_ns]_[field of a seqlock-published bulk-copy record — no per-field concurrent access (whole-record publish/copy) — D-414 leaf-3 2026-08-10]
    // [STRADDLE_EXEMPT]_[ensemble_n_updates_per_regime]_[field of a seqlock-published bulk-copy record — same rationale — D-414 leaf-3 2026-08-10]
    // [STRADDLE_EXEMPT]_[thompson_precision_post]_[field of a seqlock-published bulk-copy record — same rationale — D-414 leaf-3 2026-08-10]
    // [STRADDLE_EXEMPT]_[sp_breakdown_p99_ns]_[same seqlock-published bulk-copy rationale as its p50 sibling — shifted onto the 64B boundary by the v5.15.5 lifetime_p99 field append 2026-08-14]
    // [SCHEMA]_[v1.0]
    // [OVERVIEW]_[the per-node TUI display record — hot/slow latency quartiles + strategy/halt reason + gate flags + gate diagnostics + ML observability; one per execution node, published tear-free via the seqlock]
    // [REFERENCE]_[TECH_DEBT]_[[TECH_DEBT-13] [TECH_DEBT-28]]
    // [REFERENCE]_[DESIGN_SPEC]_[per-snapshot-cluster-layout-pattern.md]
    //==================================================================
    // [CODE]
    //==================================================================
    struct PerNodeSnap {
        // Hot-path latency (per-tick gate eval cycles).
        uint64_t samples;
        double   min_ns;
        double   p50_ns;
        double   p95_ns;
        double   p99_ns;
        double   max_ns;
        double   avg_ns;
        // v5.15.5: lifetime p99 from the NodeLatencyStats log2-bucket
        // histogram (ALL samples since boot, not the 256-window) — the
        // long-horizon tail for overnight monitoring. Within-bucket
        // interpolated estimate; see NodeLatencyStats_Snapshot.
        double   lifetime_p99_ns;
        // v5.0.1 (Phase H): slow-path latency (per-cycle work in
        // per-core slow-path thread).
        uint64_t sp_samples;
        double   sp_min_ns;
        double   sp_p50_ns;
        double   sp_p95_ns;
        double   sp_p99_ns;
        double   sp_max_ns;
        double   sp_avg_ns;
        double   sp_lifetime_p99_ns;   // v5.15.5: slow-path lifetime p99 (histogram; see above)
        uint8_t  strategy_id_display;  // STRATEGY_* constant for this core
        uint8_t  resolved_strategy_id; // v4.0.4: when strategy=AUTO, the regime-resolved
                                        // concrete strategy. Equals strategy_id_display for
                                        // non-AUTO. STRATEGY_NONE if AUTO hasn't resolved yet.
        uint8_t  halt_reason;          // v4.0.4: per-core halt reason (0=ok, 1=spacing,
                                        // 2=vwap, 3=long-slope, 4=vol-delta, 5=min-stddev,
                                        // 6=sl-cooldown, 7=warmup, 8=core-budget,
                                        // 9=core-kill, 10=imbalance — v5.6.0 added 10).
                                        // Source: NodeContext::halt_reason. Names array
                                        // in DashboardPanels.hpp must stay in sync; bound
                                        // check is sizeof(halt_names)/sizeof(halt_names[0]).
        uint8_t  strategy_halt_reason; // v5.6.2: strategy-internal halt reason. Codes
                                        // defined in StrategyInterface.hpp (SHALT_*).
                                        // Set by strategy _BuildParameters before
                                        // Gate_Zero / BUY_BLOCKED. Display priority:
                                        // halt_reason > 0 wins; else this; else
                                        // gate_flags & BUY_BLOCKED; else "no signal".
        uint8_t  gate_flags;           // v5.6.0: snapshot of cached_params.flags so the
                                        // GUI can render BUY_BLOCKED / VOLUME_REQUIRED /
                                        // TP_ENABLED / SL_ENABLED / BUY_ABOVE / PAIR_ACTIVE.
                                        // See EXECUTION_DISPLAY_INVARIANTS.md for the
                                        // predicate↔display matrix.
        // v5.14.9.B.2 — permission + bitmap_consistency MIGRATED to state_flags
        // bitmap (TECH_DEBT-013 candidate (3) close). Read via:
        //   STATE_FLAG_IS_SET(pc, PERMISSION_ALLOWED)   — entries allowed
        //   STATE_FLAG_IS_SET(pc, BITMAP_CONSISTENT)    — display↔execution invariant
        // See MemHeaders/PerNodeStateFlagsRegistry.hpp for full inventory.
        uint32_t sl_cooldown_remaining;// v4.0.4: per-core SL cooldown counter
        double   buy_gate_price;       // current buy gate threshold (for chart overlay)
        double   bg_volume_threshold;  // v5.6.1: cached_params.bg_volume_threshold —
                                        // shown in collapsing header alongside the price
                                        // gate. Active only when GATE_FLAG_VOLUME_REQUIRED
                                        // is set in gate_flags. Pre-v5.6 unsurfaced
                                        // entirely; if any future strategy enables the
                                        // VOLUME_REQUIRED flag, the operator could not
                                        // see why entries were blocked on volume.

        // v5.6.3 — gate diagnostics. Each {actual, threshold} pair shows
        // why a controller-level check passes or fails. Sourced from the
        // SAME variable the controller reads (single-source rule —
        // EXECUTION_DISPLAY_INVARIANTS.md). All values 0.0 when the
        // corresponding check isn't running (cfg disabled or rolling
        // not warmed). Display: green when actual passes threshold,
        // red/yellow when fails.
        double   diag_spacing_actual;     // current dist to last_entry_price
        double   diag_spacing_floor;      // min_dist (stddev * spacing_multiplier)
        double   diag_vwap_actual;        // current bg_price_threshold
        double   diag_vwap_threshold;     // vwap - vwap*vwap_offset
        double   diag_long_slope;         // long_rel_slope (price_slope / price_avg)
        double   diag_long_slope_min;     // cfg.min_long_slope
        double   diag_volume_delta;       // rolling.volume_delta
        double   diag_volume_delta_min;   // cfg.min_buy_delta
        double   diag_stddev_pct;         // rolling.price_stddev / rolling.price_avg
        double   diag_stddev_pct_min;     // cfg.min_stddev_pct
        double   diag_tp_pct_actual;      // out.tp_pct (set by strategy)
        double   diag_tp_pct_floor;       // 3 * fee_rate_taker (fee-floor)
        // Phase 6prep sharded c16 — per-core ML observability. Populated only
        // for STRATEGY_ML cores by TUI_CopySnapshotSharded; non-ML cores leave
        // is_ml=0 and renderer skips them.
        // v5.14.9.B.2 — gate_direction + is_ml + ml_model_loaded MIGRATED to
        // state_flags bitmap (TECH_DEBT-013 candidate (3) close). Read via:
        //   STATE_FLAG_IS_SET(pc, GATE_BUY_ABOVE)    — buy direction (MOM)
        //   STATE_FLAG_IS_SET(pc, IS_ML)             — STRATEGY_ML core
        //   STATE_FLAG_IS_SET(pc, ML_MODEL_LOADED)   — zoo has any role loaded
        double   ml_last_prediction;   // most recent ML inference output [0, 1]
        double   ml_last_confidence;   // ConfidenceScorer_Compute result [0, 1]
        double   ml_confidence_ic;     // RollingIC value for tooltip / debug
        double   ml_confidence_rmse;   // RollingRMSE value
        // v5.14.1.G — portfolio turnover diagnostic. Average symmetric-
        // difference ratio across the rolling window of top-K arm picks.
        // 0.0 = stable convictions (same top-3 every cycle); 1.0 = thrashing
        // (top picks fully shift each cycle). Populated by per-core
        // RollingTurnover ring on slow-path; published here for TUI display.
        double   ml_portfolio_turnover; // avg turnover ∈ [0, 1] across window
        double   ml_active_prediction; // prediction at fill time of open position (0 if no open pos)
        // v5.14.9.B — soft risk degradation ladder factor (composite confidence
        // × FOREACH_DEGRADATION_CURVE compute fn). 1.0 = full size; (0, 1) =
        // soft scale; 0.0 = ladder bottom (entry blocked + SHALT_LOW_CONFIDENCE).
        // Populated by ML_BuildParameters via mctx.out_confidence_factor.
        // Read by ML Status panel for "Conf factor: 0.42" display + entry log
        // attribution. nullptr-safe: pre-v5.14.9.B builds leave at 0.0;
        // operator who hasn't activated the ladder sees 1.0 (default cfg).
        double   ml_confidence_factor;
        // v5.13.6 — sell-side ML prediction (parity-check Section J observability gap).
        // Per-cycle blended exit_predictor probability (0 when use_exit_model
        // disabled or no exit models loaded). Operator sees real-time exit
        // probs in the GUI dashboard; without this, paper-test debugging of
        // "did exit_predictor fire just now?" requires log grep.
        // ml_last_exit_dominant_horizon: -1 = no prediction this cycle;
        // otherwise [0..exit_predictor_count) the arm with highest prob.
        double   ml_last_exit_prediction;
        int      ml_last_exit_dominant_horizon;
        // v5.15.5.A.6 — buy-side per-horizon barrier dispatch observability.
        // Mirrors the exit-side pattern at MLStatusPanel.hpp. Surfaces
        // which horizon's barriers drove the most recent ML buy trade
        // (DOMINANT mode) or the blended barrier value (BLEND mode).
        // -1 = no buy-side ensemble dispatch this cycle (LEGACY mode or
        // ensemble inactive). >=0 = dominant arm index [0..primary_count).
        int      ml_last_buy_dominant_horizon;
        // Active blend mode that fired this cycle's TP/SL resolution.
        // Maps to FOREACH_BARRIER_BLEND_MODE enum:
        //   0=LEGACY, 1=BLEND, 2=DOMINANT, 3=BOTH_BLEND_DRIVES,
        //   4=BOTH_DOMINANT_DRIVES (shadow modes log compare).
        uint8_t  ml_last_barrier_mode_used;
        // Shadow-mode telemetry: count of shadow ring writes since boot.
        // Only increments when barrier_blend_mode is 3 or 4. Operator can
        // gauge how much shadow data has accumulated for offline A/B analysis.
        uint32_t ml_barrier_shadow_event_count;
        // v5.14.8.C — ML observability failure modes via FOREACH_FAILURE_MODE
        // registry. Bit-packed BIT_FLAG entries share `failure_flags` uint16_t
        // bitmap; COUNTER_U32 + PERCENT_U8 entries declare standalone fields.
        // BIT_FLAG entries today: 11 of 16 used (2 load-failure + the 8-bit
        // GROUP_DRIFT surface + ml_model_corrupt; 5 bits headroom).
        // See MemHeaders/FailureModeRegistry.hpp for the row inventory.
        // Read at panel: if (FAILURE_IS_SET(pc, ml_model_load_failed)) { ... }
        // Set at slow path: FAILURE_SET(snap, ml_model_load_failed); / FAILURE_CLR.
        uint16_t failure_flags;              // BIT_FLAG entries (up to 16 today)
        // v5.14.9.B.2 — non-failure boolean state bitmap (TECH_DEBT-013
        // candidate (3) close). 11 bits today of 16 (the original 7 +
        // ML_SCALER_PRESENT / DRIFT_BREACHED / DRIFT_KILL_TRIPPED /
        // NODE_KILL_TRIPPED added at v5.15.1 TD-028; 5 bits headroom).
        // Adding a new bit: 1 row to FOREACH_PER_NODE_STATE_FLAG in
        // MemHeaders/PerNodeStateFlagsRegistry.hpp (the row inventory SSoT).
        // Read via STATE_FLAG_IS_SET(pc, NAME); set via STATE_FLAG_SET / CLR.
        uint16_t state_flags;                // BIT_FLAG entries (11 of 16 used)
        double   ml_last_threshold;          // ml_buy_threshold at last decision
        double   ml_last_effective_threshold;// post-confidence-damping threshold actually used
        uint32_t ml_nan_feature_events;      // FOREACH_FAILURE_MODE COUNTER_U32; total feature-pack NaN/Inf events
        uint32_t ml_nan_prediction_events;   // FOREACH_FAILURE_MODE COUNTER_U32; total prediction NaN/Inf events
        // v5.14.9.B.2 — strategy_was_explicit_set MIGRATED to state_flags
        // bitmap (TECH_DEBT-013 candidate (3) close). Read via:
        //   STATE_FLAG_IS_SET(pc, STRATEGY_EXPLICITLY_SET)
        // v5.14.8.C — FOREACH_FAILURE_MODE PERCENT_U8 entry. Per-core warmup
        // progress (rolling.count vs min_warmup_samples; 0..100). The global
        // TUISnapshot.warmup_samples_now collapses cores in sharded mode —
        // per-core visibility spots a single core stuck due to misconfigured
        // slow-path cadence.
        uint8_t  warmup_progress_pct;
        // v5.9.3a + v5.14.8.C + v5.15.1 — scaler observability.
        // ml_scaler_present MIGRATED to state_flags bitmap (TECH_DEBT-028);
        // read via STATE_FLAG_IS_SET(pc, ML_SCALER_PRESENT).
        // ml_scaler_load_failed lives in failure_flags BIT_FLAG; read via
        // FAILURE_IS_SET(pc, ml_scaler_load_failed).
        // Mutually-exclusive states (operator-facing matrix):
        //   present=1 + failed=0 → green "scaler: applied"
        //   present=0 + failed=1 → red "scaler: WARN — load failed"
        //   present=0 + failed=0 → sand "scaler: NONE (legacy v5 model)"
        // v5.9.5i — cfg drift detection summary. Counts mismatches
        // between stamp's recorded cfg + runtime cfg at boot. ML Status
        // panel renders summary; details live in stderr boot log.
        uint8_t  cfg_drift_tier1_count;  // freshness_tau, threshold_scale, barrier_gate
        uint8_t  cfg_drift_tier2_count;  // hard_block, bandit, fees, hyperparams, build_flags
        uint8_t  cfg_drift_strict_refused; // 1 = Tier 1 + strict mode
        // v4.0.4: per-core P&L breakdown for Account panel. Sourced from
        // NodeContext::node_realized / node_wins / node_losses / node_fees.
        // The aggregate equals oms->realized_pnl modulo timing (snapshot
        // taken between updates can show transient skew).
        double   node_realized;        // net P&L from this core's exits
        double   node_fees;            // fees paid by this core's fills
        double   node_allocated;       // capital share (nodes[i].allocated_balance)
        uint32_t node_wins;            // exit count with net > 0
        uint32_t node_losses;          // exit count with net <= 0
        uint32_t node_open_positions;  // entries_processed - exits_processed
        // Phase 2.1: per-core open notional (sum of entry_price × qty for
        // open positions). Tracks how much of allocated_balance is currently
        // deployed. Phase 2.2 uses (allocated - open_notional) as the
        // sizing budget for new entries.
        double   node_open_notional;   // raw notional of open positions
        double   node_budget_used_pct; // open_notional / allocated × 100
        // Phase 3: per-core kill switch state for the Risk panel
        double   node_peak_balance;    // peak watermark (allocated + realized + MTM)
        double   node_dd_pct;          // current drawdown fraction (0..1)
        uint32_t node_ks_trips_total;  // historical trip count
        // v5.15.1 — node_kill_tripped + drift_breached + drift_kill_tripped
        // MIGRATED to state_flags bitmap (TECH_DEBT-028; matches cohort
        // homogeneity rule). Read via STATE_FLAG_IS_SET(pc, NODE_KILL_TRIPPED)
        // / STATE_FLAG_IS_SET(pc, DRIFT_BREACHED) / STATE_FLAG_IS_SET(pc,
        // DRIFT_KILL_TRIPPED). drift-kill vs MTM-kill vs manual-kill all
        // set NODE_KILL_TRIPPED at snapshot; DRIFT_KILL_TRIPPED distinguishes
        // the drift sub-case (auto_kill_on_drift triggered).
        uint16_t drift_n_samples;      // current ic_samples count (0..256)
        double   drift_avg_ic;         // live-computed avg IC over the ring
        // v5.0.2 (Engine Topology): per-core thread layout
        int16_t  hot_path_cpu;         // pinned CPU (-1 if unpinned)
        int16_t  slow_path_cpu;        // pinned CPU (-1 if unpinned/centralized)
        uint32_t poll_interval_ticks;  // resolved per-core (override or global)
        // v5.0.3 (Engine Topology advanced): live thread observability.
        // sp_state: 0=running, 1=parked (reset_in_progress), 2=cadence-yield, 3=paused (user)
        // sp_last_tick_us: monotonic us of last completed slow-path cycle
        // sp_cycles_total: monotonic count of completed slow-path cycles
        // sp_yield_count: monotonic count of cadence-or-park yields
        // sp_submit_q_depth: live SPSC submit_queues[c] depth (informational)
        uint64_t sp_last_tick_us;
        uint64_t sp_cycles_total;
        uint64_t sp_yield_count;
        uint16_t sp_submit_q_depth;
        uint8_t  sp_state;
        // v5.1.1 (slow-path work breakdown): per-section p50 in ns.
        // Sections: 0=rebuild, 1=push_params, 2=time_exit, 3=trail_sl, 4=other
        double   sp_breakdown_p50_ns[5];
        double   sp_breakdown_p99_ns[5];
        // v5.10.0a.G.10 — ensemble (multi-horizon) visualization.
        // Populated by TUI snapshot when ensemble_handle is set; default
        // ensemble_active=0 keeps the GUI heatmap hidden for single-zoo
        // deployments. NUM_REGIMES rows × ENSEMBLE_HORIZON_MAX cols.
        //
        // v5.14.10.0 — BANDIT TELEMETRY CLUSTER BOUNDARY (alignas(64)).
        // First application of per-snapshot-cluster-layout-pattern.md (NEW
        // DESIGN_SPECS). Cluster starts here on a fresh cache-line boundary
        // to (a) prevent false-sharing with the preceding ML observability
        // fields whose write cadence differs, and (b) co-locate all bandit
        // telemetry (Exp3 ensemble_* fields below + Thompson fields added
        // in v5.14.10.D) so GUI render fetches the entire bandit cluster in
        // one cache-warm sweep. Cluster span: ~408B today (Exp3 only); grows
        // to ~512B (8 cache lines exact) when Thompson lands. Static_assert
        // below enforces the cache-line boundary at compile time.
        alignas(64)
        uint8_t  ensemble_active;
        uint8_t  ensemble_n_horizons;                  // 0 = inactive
        int      ensemble_horizon_ticks[8];            // ENSEMBLE_HORIZON_MAX
        int      ensemble_last_predicted_regime;
        int      ensemble_last_predicted_horizon_idx;
        double   ensemble_weights[5][8];               // [regime][horizon]; 5 = NUM_REGIMES
        int      ensemble_n_updates_per_regime[5];     // total_steps per bandit
        char     ensemble_blend_mode[16];              // "weighted" or "selection"
        uint32_t ensemble_disabled_horizon_mask;       // bit i set = arm i disabled
        // v5.14.10.D — FULL Bayesian dashboard for Thompson sampling. 5 fields
        // packed in the same alignas(64) bandit telemetry cluster (started at
        // ensemble_active above per v5.14.10.0 per-snapshot-cluster-layout-pattern).
        // Populated only when cfg.bandit_algorithm != 0 + initialized_thompson_bandits=1
        // (cfg=0 default leaves at zero — ML Status panel skips render).
        // Bit-packed state byte (per the BITMAP_* packing discipline):
        //   bit 0      : THOMPSON_BANDIT_ACTIVE (1 = Thompson dispatch fired this cycle)
        //   bits 1-3   : THOMPSON_CHOSEN_ARM (0-7; argmax-of-posterior at last predict)
        //   bits 4-7   : reserved for future Thompson telemetry (mode, sub-strategy, ...)
        // Encode: state = (active ? MASK_THOMPSON_BANDIT_ACTIVE : 0) |
        //                 ((arm & 0x07) << SHIFT_THOMPSON_CHOSEN_ARM)
        // Decode: bool a    = (state & MASK_THOMPSON_BANDIT_ACTIVE) != 0;
        //         uint8_t c = (state & MASK_THOMPSON_CHOSEN_ARM) >> SHIFT_THOMPSON_CHOSEN_ARM;
        static constexpr uint8_t MASK_THOMPSON_BANDIT_ACTIVE  = 0x01;  // bit 0
        static constexpr uint8_t MASK_THOMPSON_CHOSEN_ARM     = 0x0E;  // bits 1-3
        static constexpr uint8_t SHIFT_THOMPSON_CHOSEN_ARM    = 1;
        uint8_t  thompson_state;                                  // 1B  packed (active + chosen_arm)
        // (implicit padding to the next aligned field)
        // Display arrays (FLOAT — display precision sufficient; saves 32B/array vs double):
        float    thompson_mu_post[8];                             // 32B  posterior mean per arm (BANDIT_MAX_ARMS=8)
        float    thompson_precision_post[8];                      // 32B  posterior precision per arm (= 1/variance)
        uint32_t thompson_total_pulls[8];                         // 32B  pull count per arm (matches BanditState.pulls width)
        // v5.15.1 — Model Health drift surface. Aggregated drift state for
        // operator visibility. Drift BITS live in failure_flags (set by
        // ShardedSnapshot OR-aggregating each zoo role's
        // handle->drift_flags_at_load); panel reads via FAILURE_IS_SET.
        // training_timestamp_us captured here (8B) for "model age" rendering.
        // At-load hash diagnostic values (feature_hash, label_hash,
        // build_flags, scaler_registry_hash) deferred to v5.15.1.post or
        // v5.15.2 — would need feature_registry_hash + label_registry_hash
        // added to ModelHandle as runtime-only fields (not in stamp body
        // registry). Drift bits + tooltips give enough operator signal today.
        uint64_t handle_training_timestamp_us;                    // 8 B (representative role; 0 if no timestamp)
    };
    //==================================================================
    // [END_CODE]
    //==================================================================
    // [DERIVED]
    // [ORIGIN]_[AUTO]
    // [UPDATED]_[2026-08-14]
    // [SIZE]_[1216B]
    // [ALIGN]_[64]
    // [CACHE_LINES]_[19]
    // [STRADDLE]_[sp_breakdown_p50_ns@504 · sp_breakdown_p99_ns@544 · ensemble_n_updates_per_regime@1008 · thompson_precision_post@1084]
    //==================================================================
    // [END_STRUCT]_[PerNodeSnap]
    //==================================================================
    PerNodeSnap per_node[16];      // up to MAX_EXECUTION_NODES
};

// ───────────────────────────────────────────────────────────────────────────
// v5.14.10.0 — PerNodeSnap layout discipline (per-snapshot-cluster-layout-pattern.md)
// ───────────────────────────────────────────────────────────────────────────
//
// Compile-time enforcement that the bandit telemetry cluster starts on a
// cache-line boundary. Catches inadvertent layout drift if a future field
// lands BEFORE ensemble_active without preserving the alignas(64) marker.
//
// To extend (e.g., when adding a new clustered telemetry surface):
//   1. Add `alignas(64)` to the FIRST field of the new cluster
//   2. Add a static_assert here mirroring this pattern
//   3. Update per-snapshot-cluster-layout-pattern.md "Reference applications"
//
// Substantial close of TECH_DEBT-011 (PerNodeSnap layout discipline).
// Future ships will apply the same pattern to other clusters (ML
// observability, gate diagnostics, slow-path observability) per the
// DESIGN_SPECS doc; remaining clusters tracked as deferred items.
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(PerNodeSnap, ensemble_active) %64 == 0 — the bandit-cluster cache-line boundary]
static_assert(offsetof(TUISnapshot::PerNodeSnap, ensemble_active) % 64 == 0,
    "v5.14.10.0: bandit telemetry cluster (PerNodeSnap::ensemble_active) "
    "must start on 64-byte cache-line boundary. Did a new field land before "
    "ensemble_active without preserving the alignas(64) marker?");
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — layout emitter cannot probe this block yet; quartet lands when the emitter covers it, D-327)
//======================================================================
// [END_STRUCT]_[TUISnapshot]
//======================================================================

//======================================================================
// [STRUCT]_[TUISharedState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the cross-thread seam — double-buffered snapshots under the seqlock seq (engine slow-path publishes, TUI/GUI reads) + the REVERSE control channel (GUI-written request flags / drag / hot-swap, engine-consumed)]
// [THREAD]_[[SLOW_PATH_WRITER] [TUI_READER]]
//======================================================================
// [CODE]
//======================================================================
struct TUISharedState {
    TUISnapshot snapshots[2];
    // v5.11.3.B — Seqlock encoding of publish state. Replaces the former
    // `volatile int active_idx` flat-flag pattern. The flat flag was
    // tear-prone: when the writer published faster than the reader read
    // (which happens under load with the publish-frequent slow path) the
    // writer could overwrite the same buffer the reader was holding a
    // pointer into mid-frame. The seqlock + double-buffer combo (matches
    // ParameterSlot in CoreFrameworks/) closes that hazard.
    //
    // Encoding:
    //   bit 0 (parity)    — 0 = stable, 1 = mid-write
    //   bit 1 (active idx) — flips after each successful write
    //
    // Writer protocol: load seq → store seq+1 (mid-write, idx unchanged)
    // → fill snapshots[idx ^ 1] → store seq+2 (parity even, idx flipped).
    // Two release stores fence around the non-atomic populate.
    //
    // Reader protocol: full memcpy of snapshots[idx] into stack-local copy
    // bracketed by acquire-loads of seq; retry if mid-write or seq advanced
    // during the copy. Wait-free for the writer; bounded retry for the
    // reader (only retries when the writer just lapped, rare in steady state
    // where writer cadence is slow-path and reader cadence is 60 Hz).
    //
    // Use TUISnapshot_Publish_Begin/End on the writer side and
    // TUISnapshot_ReadInto on the reader side — direct seq mutation is
    // forbidden outside those helpers.
    std::atomic<uint64_t> seq;
    volatile sig_atomic_t quit_requested;
    volatile sig_atomic_t pause_requested;
    volatile sig_atomic_t reload_requested;
    volatile sig_atomic_t regime_cycle_requested;
    volatile sig_atomic_t kill_reset_requested;
    volatile sig_atomic_t paper_reset_requested;  // reset balance + positions to starting state
    // v4.7.18: monotonic counter bumped by the engine each time a paper
    // reset completes. GUI panels with retained history (Per-Core P&L
    // ring buffer, equity curve, etc.) save the last seen value and
    // clear their buffers when it changes. Stays simple — atomic-style
    // semantics on uint32 are fine for sig_atomic_t reads on x86.
    volatile sig_atomic_t paper_reset_seq;
    // GUI drag-TP/SL: slot index + new price (engine clears after pickup)
    volatile int drag_slot;       // -1 = no drag, 0-15 = position slot
    volatile int drag_is_tp;      // 1 = TP, 0 = SL
    volatile double drag_price;   // new price value
    // Hot-swap strategy per core (sharded mode). GUI writes; controller core
    // reads + acts on the next slow-path rebuild. Value 0xFF (STRATEGY_NONE)
    // means "no pending swap"; any other value triggers the swap. Controller
    // resets to 0xFF after applying. Per-core array so multiple cores can
    // be queued independently.
    volatile uint8_t swap_strategy_requested[16];
    // Phase 3: per-core kill switch reset. GUI writes 1 to reset the trip
    // for that specific core. Controller resets the flag back to 0 after
    // clearing node_kill_tripped + refreshing node_peak_balance to current.
    // Independent per core — resetting core 0 doesn't touch core 3.
    volatile sig_atomic_t kill_reset_per_node[16];
    // v4.7.8: manual close per portfolio slot. GUI writes 1 to force-close
    // the position at that slot (bypasses hot-path SG; emits a synthetic
    // exit event from the drainer). Drainer reads + acts + clears the
    // flag back to 0. Indexed by portfolio slot (under partials, slot 2c
    // is leg A and 2c+1 is leg B — closing one closes only that leg).
    volatile sig_atomic_t manual_close_requested[16];
    // v5.0.3 (Engine Topology advanced): per-engine pause toggle. GUI sets
    // bit c to pause slow-path c; clears bit c to resume. Slow-path thread
    // checks its bit at top of loop; if set, yields without doing work
    // (sp_state=3, sp_yield_count++). Single-writer (GUI) per bit; per-
    // core thread c is single-reader of bit c. Doesn't affect hot-path.
    volatile uint16_t paused_engines_mask;
    // v5.10.0c — Hot model swap. GUI's "Apply (live)" button next to the
    // Model Dir Combo writes the new path + sets the request flag for
    // that core. Engine slow-path consumes: verifies the new model via
    // NodeModelZoo_TryLoadRole, swaps the active handle on success, frees
    // the old handle after one slow-path grace period. Single-writer
    // (GUI) / single-reader (engine slow-path); per-core array.
    // Sentinel: pending_model_path[c][0]=='\0' AND swap_model_path_requested[c]==0
    // means "no pending swap". Both fields write-then-flag pattern: GUI
    // writes pending_model_path FIRST, then atomic-stores
    // swap_model_path_requested with __ATOMIC_RELEASE; engine reads with
    // __ATOMIC_ACQUIRE.
    volatile uint8_t swap_model_path_requested[16];
    char pending_model_path[16][256];
    EngineTUI tui;
    const char *config_path;
    void *candle_acc;  // CandleAccumulator* (GUI build only, NULL for ANSI)
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-14]
// [SIZE]_[64640B]
// [ALIGN]_[64]
// [CACHE_LINES]_[1010]
// [STRADDLE]_[swap_strategy_requested@60216 · kill_reset_per_node@60232 · manual_close_requested@60296]
//======================================================================
// [END_STRUCT]_[TUISharedState]
//======================================================================

//======================================================================
// [FUNCTION]_[TUISnapshot_Publish_Begin]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the seqlock family (InitSeq / PublishHandle / End / ReadInto / Sequence ride) — writer wait-free two-release-store publish; reader bounded-retry memcpy; direct seq mutation FORBIDDEN outside these]
//======================================================================
// [CODE]
//======================================================================

// Init the seqlock to a stable starting state (idx=0, parity=0). Both buffers
// must already be zeroed by the caller (or set to a sentinel "not yet
// populated" state) — this only initializes the sequence counter.
static inline void TUISnapshot_InitSeq(TUISharedState *shared) {
    shared->seq.store(0, std::memory_order_release);
}

// Result of TUISnapshot_Publish_Begin — gives the writer access to both
// buffers (back for write, front for graph-history carry-over).
struct TUISnapshot_PublishHandle {
    TUISnapshot *back;          // writer fills this; readers will see it after End
    const TUISnapshot *front;   // last-published; engine carries graph history forward
};

// Begin a publish cycle. Stamps the parity bit to "mid-write" so any reader
// in flight retries; returns pointers to both buffers. The caller fills
// `back` and then calls TUISnapshot_Publish_End — the back buffer becomes
// the new active.
static inline TUISnapshot_PublishHandle TUISnapshot_Publish_Begin(TUISharedState *shared) {
    uint64_t s = shared->seq.load(std::memory_order_relaxed);
    uint64_t cur_idx  = (s >> 1) & 1ULL;
    uint64_t next_idx = cur_idx ^ 1ULL;
    // Mid-write: parity flips odd. Active idx (bit 1) is unchanged so any
    // reader concurrently sampling seq sees the OLD active idx still — they
    // continue reading the previous publication, never the in-progress back.
    shared->seq.store(s + 1, std::memory_order_release);
    return TUISnapshot_PublishHandle{
        &shared->snapshots[next_idx],
        &shared->snapshots[cur_idx]
    };
}

// End a publish cycle. Bumps seq one more (becomes even, idx bit toggled).
// Any subsequent reader sees the just-filled buffer as active.
static inline void TUISnapshot_Publish_End(TUISharedState *shared) {
    uint64_t s = shared->seq.load(std::memory_order_relaxed);
    // s is currently odd (mid-write set in Begin). +1 → even, idx toggled.
    shared->seq.store(s + 1, std::memory_order_release);
}

// Tear-free reader. Copies the active snapshot into caller-owned storage.
// Retries when the writer is mid-write or has just lapped during the copy.
// Bounded under steady-state load; in pathological cases (writer publishing
// faster than memcpy can complete) this could spin, but the writer's cadence
// is slow-path (~10ms-100ms) and the memcpy is microseconds — retries are
// effectively never observed.
static inline void TUISnapshot_ReadInto(const TUISharedState *shared, TUISnapshot *out) {
    uint64_t s1, s2;
    for (;;) {
        s1 = shared->seq.load(std::memory_order_acquire);
        if ((s1 & 1ULL) != 0) {
            __builtin_ia32_pause();
            continue;
        }
        uint64_t idx = (s1 >> 1) & 1ULL;
        *out = shared->snapshots[idx];  // memcpy of the snapshot struct
        s2 = shared->seq.load(std::memory_order_acquire);
        if (s1 == s2) return;
        // Writer lapped during copy — retry.
    }
}

// Introspection: current sequence value, for tests and the parity-check
// torn-read regression test.
static inline uint64_t TUISnapshot_Sequence(const TUISharedState *shared) {
    return shared->seq.load(std::memory_order_relaxed);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TUISnapshot_Publish_Begin]
//======================================================================

//======================================================================
// [FUNCTION]_[TUI_CopySnapshot]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the legacy/backtest populate path (3 overloads) — engine-thread per-slow-path-cycle FPN/Money -> double conversion into the snapshot; sharded populators fill the per-core fields separately]
//======================================================================
// [CODE]
//======================================================================
// overload: explicit price/volume (used by backtest — no DataStream available)
template <unsigned F>
static inline void TUI_CopySnapshot(TUISnapshot *snap,
                                      const PortfolioController<F> *ctrl,
                                      double price_d, double volume_d);

// overload: DataStream (used by live engine)
template <unsigned F>
static inline void TUI_CopySnapshot(TUISnapshot *snap,
                                      const PortfolioController<F> *ctrl,
                                      const DataStream<F> *stream) {
    TUI_CopySnapshot(snap, ctrl, stream->price_d, stream->volume_d);
}

template <unsigned F>
static inline void TUI_CopySnapshot(TUISnapshot *snap,
                                      const PortfolioController<F> *ctrl,
                                      double price_d, double volume_d) {
    // Phase 14: per-core stats default to "not active". A separate
    // populator (TUI_CopySnapshotPerCore) writes the actual values when
    // sharded mode is running. Legacy mode leaves these zeroed and the
    // TUI panel just doesn't render the per-core section.
    snap->sharded_mode_active = 0;
    snap->partial_exit_enabled = 0;
    snap->per_node_count = 0;
    // v5.0.2: topology — zeroed in legacy mode (panel won't render).
    snap->producer_cpu = -1;
    snap->drainer_cpu = -1;
    snap->nproc = 0;
    snap->slow_path_pin_offset = 0;

    snap->price  = price_d;
    snap->volume = volume_d;
    snap->state_warmup = (ctrl->state == CONTROLLER_WARMUP);
    snap->is_paused = FPN_IsZero(ctrl->buy_conds.price) && !snap->state_warmup;
    snap->start_time = 0; // TUI thread computes uptime from its own start_time
    // v5.9.0c — capture cfg path for engine header panel display
    {
        size_t n = strlen(ctrl->config.source_cfg_path);
        if (n >= sizeof(snap->source_cfg_path)) n = sizeof(snap->source_cfg_path) - 1;
        memcpy(snap->source_cfg_path, ctrl->config.source_cfg_path, n);
        snap->source_cfg_path[n] = '\0';
    }

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
    double fee_r = Money_ToDouble(ctrl->config.fee_rate);
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
        ps->entry    = Money_ToDouble(pos->entry_price);
        ps->qty      = Money_ToDouble(pos->quantity);
        ps->tp       = Money_ToDouble(pos->take_profit_price);
        ps->sl       = Money_ToDouble(pos->stop_loss_price);
        ps->orig_tp  = Money_ToDouble(pos->original_tp);
        ps->value    = price_d * ps->qty;
        ps->gross_pnl = (ps->entry != 0.0) ? ((price_d - ps->entry) / ps->entry) * 100.0 : 0.0;
        ps->net_pnl   = ps->gross_pnl - (fee_r * 200.0);
        ps->is_trailing  = !Money_Eq(pos->take_profit_price, pos->original_tp);
        ps->above_orig_tp = (price_d > ps->orig_tp) && (ps->entry != 0.0);
        ps->ticks_held   = ctrl->total_ticks - ctrl->entry_ticks[idx];
        ps->hold_minutes = (ctrl->entry_time[idx] > 0)
            ? difftime(time(NULL), ctrl->entry_time[idx]) / 60.0 : 0.0;
        ps->entry_time = ctrl->entry_time[idx];
        snap->total_value += ps->value;
        snap->total_qty   += ps->qty;
        active &= active - 1;
    }

    // financials
    double starting = Money_ToDouble(ctrl->config.starting_balance);
    double balance  = Money_ToDouble(ctrl->balance);
    double realized = Money_ToDouble(ctrl->realized_pnl);
    double unrealized = Money_ToDouble(ctrl->portfolio_delta);
    snap->balance    = balance;
    snap->starting   = starting;
    snap->realized   = realized;
    snap->unrealized = unrealized;
    snap->equity     = balance + snap->total_value;
    snap->total_pnl  = snap->equity - starting; // derive from equity (always correct)
    snap->return_pct = (starting != 0.0) ? (snap->total_pnl / starting) * 100.0 : 0.0;
    snap->exposure_pct = (starting != 0.0) ? (snap->total_value / starting) * 100.0 : 0.0;
    snap->max_exp    = Money_ToDouble(ctrl->config.max_exposure_pct) * 100.0;
    snap->fees       = Money_ToDouble(ctrl->total_fees);
    snap->fee_rate_pct = fee_r * 100.0;
    snap->risk_amt   = Money_ToDouble(ctrl->config.risk_pct) * 100.0;
    snap->max_dd     = Money_ToDouble(ctrl->config.max_drawdown_pct) * 100.0;
    snap->breaker_tripped = (snap->total_pnl < -(starting * Money_ToDouble(ctrl->config.max_drawdown_pct)));
    snap->buying_halted = ctrl->buying_halted;
    snap->halt_reason = ctrl->halt_reason;
    snap->gate_reason = ctrl->gate_reason;

    // regime
    snap->current_regime = ctrl->regime.current_regime;
    snap->strategy_id    = ctrl->strategy_id;
    snap->regime_auto    = (ctrl->config.default_strategy < 0);
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
    // EMA/SMA crossover spread
    {
        double ema = FPN_ToDouble(ctrl->ema_price);
        double sma = FPN_ToDouble(ctrl->rolling.price_avg);
        snap->ema_sma_spread = (sma > 1e-15) ? (ema - sma) / sma : 0.0;
    }
    // volume spike
    snap->volume_spike_ratio = FPN_ToDouble(ctrl->volume_spike_ratio);
    snap->spike_active = FPN_GreaterThanOrEqual(ctrl->volume_spike_ratio,
                                                 ctrl->config.spike_threshold);
    snap->vwap = FPN_ToDouble(ctrl->rolling.vwap);
    snap->vwap_dev = FPN_ToDouble(ctrl->rolling.vwap_deviation);
    snap->ema_price = FPN_ToDouble(ctrl->ema_price);
    snap->book_imbalance = FPN_ToDouble(ctrl->book_imbalance);
    snap->book_spread = 0.0; // populated from depth thread if available
    snap->danger_score = FPN_ToDouble(ctrl->danger_score);
    snap->current_session = ctrl->current_session;
    snap->session_mult = FPN_ToDouble(ctrl->session_mult);
    snap->sl_cooldown = (int)ctrl->sl_cooldown_counter;
    snap->min_warmup_samples = (int)ctrl->config.min_warmup_samples;
    // kill switch
    snap->kill_switch_active = ctrl->kill_switch_active;
    snap->kill_reason = ctrl->kill_reason;
    snap->kill_recovery = (int)ctrl->kill_recovery_counter;
    // vol scale
    snap->vol_scale = ctrl->last_vol_scale;
    // no-trade band (signal strength computed relative to rolling avg)
    {
      double bavg = FPN_ToDouble(ctrl->rolling.price_avg);
      double bprice = FPN_ToDouble(ctrl->buy_conds.price);
      snap->signal_strength = (bavg > 1e-15) ? fabs(bprice - bavg) / bavg * 100.0 : 0.0;
      double min_signal = Money_ToDouble(ctrl->config.fee_rate) * FPN_ToDouble(ctrl->config.no_trade_band_mult) * 100.0;
      snap->no_trade_band_blocked = BITMAP_IS_SET(ctrl->config.gate_cfg_flags, MASK_GATE_CFG_NO_TRADE_BAND_ENABLED) &&
          (snap->signal_strength < min_signal) && !snap->state_warmup;
    }
    // FoxML integration (Phase 6C) — single populate function
    MLSnapshot_Populate(&snap->ml, ctrl);
    // per-strategy reward attribution
    // v5.10.3.A — Source array `ctrl->strategy_stats` is sized NUM_STRATEGIES_REAL=5
    // (no AUTO entry on the source side; AUTO is a dispatcher sentinel). Snapshot
    // array is sized NUM_STRATEGIES=6 (including AUTO). Iterate REAL for the
    // populated indices; explicitly zero the AUTO bin so TUIAnsi iterations
    // through NUM_STRATEGIES read valid data at every index.
    for (int i = 0; i < NUM_STRATEGIES_REAL; i++) {
      snap->strat_stats[i].pnl   = Money_ToDouble(ctrl->strategy_stats[i].realized_pnl);
      snap->strat_stats[i].wins  = ctrl->strategy_stats[i].wins;
      snap->strat_stats[i].losses = ctrl->strategy_stats[i].losses;
      snap->strat_stats[i].total = ctrl->strategy_stats[i].total_trades;
    }
    snap->strat_stats[NUM_STRATEGIES_REAL] = {};  // AUTO bin: no per-strategy stats
    // session stats + fill diagnostics
    snap->session_high = ctrl->session_high;
    snap->session_low = ctrl->session_low;
    snap->tick_rate = (double)ctrl->total_ticks;  // raw count, TUI computes rate from uptime
    snap->fills_rejected = ctrl->fills_rejected;
    snap->last_reject_reason = ctrl->last_reject_reason;

    // config
    snap->cfg_tp  = Money_ToDouble(ctrl->config.take_profit_pct) * 100.0;
    snap->cfg_sl  = Money_ToDouble(ctrl->config.stop_loss_pct) * 100.0;
    snap->cfg_fee = fee_r * 100.0;
    snap->cfg_slippage = Money_ToDouble(ctrl->config.slippage_pct) * 100.0;
    snap->live_trading = ControllerConfig_IsLiveCapital(ctrl->config); // NEW-1 — display mirror routes the single predicate
    snap->trailing_enabled = !FPN_IsZero(ctrl->config.tp_hold_score);
    snap->cfg_hold_score   = FPN_ToDouble(ctrl->config.tp_hold_score);
    snap->cfg_trail_mult   = FPN_ToDouble(ctrl->config.tp_trail_mult);
    snap->cfg_sl_trail_mult = FPN_ToDouble(ctrl->config.sl_trail_mult);
    snap->cfg_offset_val = snap->stddev_mode
        ? FPN_ToDouble(ctrl->config.offset_stddev_mult)
        : Money_ToDouble(ctrl->config.entry_offset_pct) * 100.0;

    // stats
    snap->total_buys = ctrl->total_buys;
    snap->wins       = ctrl->wins;
    snap->losses     = ctrl->losses;
    // Phase 8 — maker/taker counters + fees. BacktestSnapshot_Copy is a thin
    // wrapper around TUI_CopySnapshot per CLAUDE.md "Snapshot sync rule
    // (simplified 2026-04)" — backtest gets these for free, no second update site.
    snap->maker_fills_count = ctrl->maker_fills_count;
    snap->taker_fills_count = ctrl->taker_fills_count;
    snap->total_maker_fees  = Money_ToDouble(ctrl->total_maker_fees);
    snap->total_taker_fees  = Money_ToDouble(ctrl->total_taker_fees);
    uint32_t total_exits = ctrl->wins + ctrl->losses;
    snap->win_rate      = (total_exits > 0) ? ((double)ctrl->wins / total_exits) * 100.0 : 0.0;
    double g_wins  = Money_ToDouble(ctrl->gross_wins);
    double g_losses = Money_ToDouble(ctrl->gross_losses);
    // v5.8.4c: canonical helpers (single source of truth across backtest +
    // live + sharded snapshot paths).
    snap->profit_factor = Compute_ProfitFactor(g_wins, g_losses);
    snap->all_wins_run  = Compute_AllWinsRun(g_wins, g_losses);
    snap->avg_win  = (ctrl->wins > 0)  ? g_wins / ctrl->wins : 0.0;
    snap->avg_loss = (ctrl->losses > 0) ? g_losses / ctrl->losses : 0.0;
    // market-only avg loss: subtract estimated round-trip fees per losing exit
    // fee_per_exit ≈ avg_position_cost * fee_rate * 2 (entry + exit)
    // use total_fees / total_exits as a simpler proxy
    {
      uint32_t tex = ctrl->wins + ctrl->losses;
      double fee_per_exit = (tex > 0) ? Money_ToDouble(ctrl->total_fees) / tex : 0.0;
      snap->avg_loss_market = (ctrl->losses > 0) ? snap->avg_loss - fee_per_exit : 0.0;
      if (snap->avg_loss_market < 0.0) snap->avg_loss_market = 0.0; // clamp (loss was entirely fees)
    }
    snap->avg_hold = (total_exits > 0)  ? (double)ctrl->total_hold_ticks / total_exits : 0.0;

    // v5.8.4c: canonical Compute_Expectancy (keeps fabs(avg_loss) — defensive).
    snap->expectancy = Compute_Expectancy((uint32_t)total_exits,
                                           (uint32_t)ctrl->wins,
                                           snap->avg_win, snap->avg_loss);

    // max drawdown
    snap->max_drawdown = Money_ToDouble(ctrl->max_drawdown);
    double pe = Money_ToDouble(ctrl->peak_equity);
    snap->max_drawdown_pct = (pe > 0.0) ?
        (snap->max_drawdown / pe) * 100.0 : 0.0;

    // fee ratio: what % of gross wins go to fees
    snap->fee_ratio = (g_wins > 0.001) ?
        (Money_ToDouble(ctrl->total_fees) / g_wins) * 100.0 : 0.0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TUI_CopySnapshot]
//======================================================================

//======================================================================
// [FUNCTION]_[TUI_PopulatePerCoreLatency]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the sharded populator family (PerCoreSlowPathLatency / AdvancedTopology / Topology ride) — templated on CoresT/StateT/OmsT so this header stays free of engine-type includes]
// [REFERENCE]_[DESIGN_SPEC]_[cross-thread-snapshot-publish-cluster-isolation.md]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[PER-CORE LATENCY POPULATOR — Phase 14, sharded mode only]
//------------------------------------------------------------------
template <typename CoresT>
static inline void TUI_PopulatePerCoreLatency(TUISnapshot *snap,
                                                CoresT *nodes,
                                                int num_nodes,
                                                double tsc_ghz) {
    snap->sharded_mode_active = 1;
    if (num_nodes < 0) num_nodes = 0;
    if (num_nodes > 16) num_nodes = 16;
    snap->per_node_count = num_nodes;
    for (int i = 0; i < num_nodes; ++i) {
        tt::NodeLatencySnapshot ls = tt::NodeLatencyStats_Snapshot(
            &nodes[i].latency_stats, tsc_ghz);
        snap->per_node[i].samples = ls.total_count;
        snap->per_node[i].min_ns  = ls.min_ns;
        snap->per_node[i].p50_ns  = ls.p50_ns;
        snap->per_node[i].p95_ns  = ls.p95_ns;
        snap->per_node[i].p99_ns  = ls.p99_ns;
        snap->per_node[i].max_ns  = ls.max_ns;
        snap->per_node[i].avg_ns  = ls.avg_ns;
        snap->per_node[i].lifetime_p99_ns = ls.lifetime_p99_ns;
    }
    // Zero unused slots so renderer doesn't show stale data from a previous
    // tick when num_nodes changes
    for (int i = num_nodes; i < 16; ++i) {
        snap->per_node[i].samples = 0;
        snap->per_node[i].min_ns  = 0;
        snap->per_node[i].p50_ns  = 0;
        snap->per_node[i].p95_ns  = 0;
        snap->per_node[i].p99_ns  = 0;
        snap->per_node[i].max_ns  = 0;
        snap->per_node[i].avg_ns  = 0;
        snap->per_node[i].lifetime_p99_ns = 0;
        // v5.0.1 (Phase H): slow-path latency too
        snap->per_node[i].sp_samples = 0;
        snap->per_node[i].sp_min_ns  = 0;
        snap->per_node[i].sp_p50_ns  = 0;
        snap->per_node[i].sp_p95_ns  = 0;
        snap->per_node[i].sp_p99_ns  = 0;
        snap->per_node[i].sp_max_ns  = 0;
        snap->per_node[i].sp_avg_ns  = 0;
        snap->per_node[i].sp_lifetime_p99_ns = 0;
    }
}

// v5.0.1 (Phase H): populate per-core SLOW-path latency stats from
// EventLoopState's NodeContext::slow_path_latency. Sibling to
// TUI_PopulatePerCoreLatency (which handles hot-path). Caller invokes
// AFTER TUI_PopulatePerCoreLatency so the per-core slot count is set.
//
// StateT templated to keep this header free of EventLoopState dependency.
template <typename StateT>
static inline void TUI_PopulatePerCoreSlowPathLatency(TUISnapshot *snap,
                                                       const StateT *state,
                                                       double tsc_ghz) {
    int n = snap->per_node_count;
    if (n < 0) n = 0;
    if (n > 16) n = 16;
    for (int i = 0; i < n; ++i) {
        tt::NodeLatencySnapshot ls = tt::NodeLatencyStats_Snapshot(
            &state->display_meta[i].slow_path_latency, tsc_ghz);
        snap->per_node[i].sp_samples = ls.total_count;
        snap->per_node[i].sp_min_ns  = ls.min_ns;
        snap->per_node[i].sp_p50_ns  = ls.p50_ns;
        snap->per_node[i].sp_p95_ns  = ls.p95_ns;
        snap->per_node[i].sp_p99_ns  = ls.p99_ns;
        snap->per_node[i].sp_max_ns  = ls.max_ns;
        snap->per_node[i].sp_avg_ns  = ls.avg_ns;
        snap->per_node[i].sp_lifetime_p99_ns = ls.lifetime_p99_ns;
        // v5.1.1: per-section breakdown.
        for (int s = 0; s < 5; ++s) {
            tt::NodeLatencySnapshot ss = tt::NodeLatencyStats_Snapshot(
                &state->display_meta[i].slow_path_breakdown[s], tsc_ghz);
            snap->per_node[i].sp_breakdown_p50_ns[s] = ss.p50_ns;
            snap->per_node[i].sp_breakdown_p99_ns[s] = ss.p99_ns;
        }
    }
}

//------------------------------------------------------------------
// [SECTION]_[ADVANCED TOPOLOGY POPULATION (v5.0.3)]
//------------------------------------------------------------------
// Sibling to TUI_PopulateTopology — copies the live observability fields
// from NodeContext into TUISnapshot::PerNodeSnap for the Engine Topology
// panel's State/Cycles/LastCycle/Q-depth columns. Called from the GUI
// snapshot publish each cycle (cheap — handful of relaxed loads).
//
// StateT/OmsT templated to keep this header free of EventLoopState and
// OrderManagerState dependencies.
//======================================================================================================
template <typename StateT, typename OmsT>
static inline void TUI_PopulateAdvancedTopology(TUISnapshot *snap,
                                                  const StateT *state,
                                                  const OmsT *oms) {
    // v5.12.1.C — heartbeat: snapshot WS freshness for header render
    // v5.15.5.B.2 — source-side reads now hit the WsHeartbeatTelemetry cluster
    // (alignas(64) isolated). PerNodeSnap field names unchanged.
    snap->ws_last_tick_us = state->ws_telemetry.last_tick_us.load(std::memory_order_acquire);
    snap->ws_ticks_per_5s = state->ws_telemetry.ticks_per_5s.load(std::memory_order_relaxed);
    int n = snap->per_node_count;
    if (n < 0) n = 0;
    if (n > 16) n = 16;
    for (int i = 0; i < n; ++i) {
        // v5.15.5.B.2 — source-side reads now hit the SlowPathTelemetry cluster
        // (alignas(64) isolated; cross-thread-snapshot-publish-cluster-isolation.md).
        // PerNodeSnap field names unchanged — only the source-of-truth changed.
        snap->per_node[i].sp_last_tick_us =
            state->nodes[i].sp_telemetry.last_tick_us.load(std::memory_order_relaxed);
        snap->per_node[i].sp_cycles_total =
            state->nodes[i].sp_telemetry.cycles_total.load(std::memory_order_relaxed);
        snap->per_node[i].sp_yield_count =
            state->nodes[i].sp_telemetry.yield_count.load(std::memory_order_relaxed);
        snap->per_node[i].sp_state =
            state->nodes[i].sp_telemetry.state.load(std::memory_order_relaxed);
        snap->per_node[i].sp_submit_q_depth =
            (uint16_t)SPSCRing_Depth(&oms->submit_queues[i]);
    }
}

//------------------------------------------------------------------
// [SECTION]_[TOPOLOGY POPULATION (Engine Topology panel)]
//------------------------------------------------------------------
// v5.0.2: snapshot the system + thread layout for the GUI Engine Topology
// panel. Called ONCE at boot from EngineSharded_Run after thread spawn,
// since the values are static for the lifetime of the engine.
//
// Args:
//   producer_cpu        — pin assignment for producer thread (always 0 today)
//   drainer_cpu         — pin assignment for drainer thread (num_nodes + 1)
//   nproc               — sysconf(_SC_NPROCESSORS_ONLN)
//   slow_path_pin_off   — raw cfg.slow_path_pin_offset (-1 disabled, 0 auto, >0 explicit)
//   hot_cpu[i]          — per-core hot-path pin (i + 1 in current layout)
//   slow_cpu[i]         — per-core slow-path pin (-1 if unpinned)
//   poll_interval[i]    — per-core resolved poll cadence
//======================================================================================================
static inline void TUI_PopulateTopology(TUISnapshot *snap,
                                         int producer_cpu,
                                         int drainer_cpu,
                                         int nproc,
                                         int slow_path_pin_off,
                                         const int *hot_cpu,
                                         const int *slow_cpu,
                                         const uint32_t *poll_interval) {
    snap->producer_cpu         = (int16_t)producer_cpu;
    snap->drainer_cpu          = (int16_t)drainer_cpu;
    snap->nproc                = (int16_t)nproc;
    snap->slow_path_pin_offset = (int16_t)slow_path_pin_off;
    int n = snap->per_node_count;
    if (n < 0) n = 0;
    if (n > 16) n = 16;
    for (int i = 0; i < n; ++i) {
        snap->per_node[i].hot_path_cpu  = (int16_t)hot_cpu[i];
        snap->per_node[i].slow_path_cpu = (int16_t)slow_cpu[i];
        snap->per_node[i].poll_interval_ticks = poll_interval[i];
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// TUI_PopulatePerCoreLatency: called from the sharded engine's controller
// core. Walks the registered execution cores, snapshots each one's
// NodeLatencyStats, and writes the per-core fields into the TUISnapshot.
// The render path checks snap->sharded_mode_active to decide whether to
// render the panel.
//
// CoresT is templated so this header doesn't need to know about the
// ExecutionCore type. The caller passes a pointer to the core array and
// num_nodes; the lambda accesses each core's latency_stats by index.
//======================================================================
// [END_FUNCTION]_[TUI_PopulatePerCoreLatency]
//======================================================================

//------------------------------------------------------------------
// [SECTION]_[SHARDED SNAPSHOT COPY — lives in CoreFrameworks/ShardedSnapshot.hpp]
//------------------------------------------------------------------
// TUI_CopySnapshotSharded is in a separate header because it depends on
// EventLoopState and EventLoopAggregates (OMS headers) which EngineTUI.hpp
// doesn't include. EngineSharded.hpp includes ShardedSnapshot.hpp after
// both dependencies are available.
//------------------------------------------------------------------

//======================================================================
// [FUNCTION]_[TUI_Render_Snapshot]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[TUI-thread dashboard render from the local snapshot copy (TUI_ReadKey rides) — reads only doubles, never engine state]
//======================================================================
// [CODE]
//======================================================================
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
    printf(C_BOLD C_PEACH "    ( o.o )  " C_WHEAT "v" RELEASE_VERSION_STRING "  (engine " ENGINE_VERSION_STRING ")" C_RESET "\n"); row++;
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
        int rk = s->current_regime;
        if (rk < 0 || rk >= NUM_REGIMES) rk = 0;
        const char *regime_name = REGIME_INFO[rk].full_name;
        const char *regime_color = (rk == REGIME_TRENDING || rk == REGIME_MILD_TREND) ? C_GREEN :
                                   (rk == REGIME_VOLATILE || rk == REGIME_TRENDING_DOWN) ? C_RED : C_DIM;
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
    printf(C_DIM "    log: logging/btcusdt_order_history.csv" C_RESET "\n"); row++;
    printf(C_SAND "  ================================================" C_RESET "\n"); row++;

#ifdef LATENCY_PROFILING
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;
    printf(C_BOLD C_PEACH "  LATENCY " C_DIM "(profiling, multicore):" C_RESET "\n"); row++;
    if (s->hot_count > 0) {
        printf(C_SAND "    hot path:  " C_FG "avg %.0fns" C_DIM "  min " C_FG "%.0fns" C_DIM "  max " C_FG "%.0fns"
               C_DIM "  (%lu ticks)" C_RESET "\n", s->hot_avg_ns, s->hot_min_ns, s->hot_max_ns,
               (unsigned long)s->hot_count); row++;
        printf(C_DIM "               p50 " C_FG "%.0fns" C_DIM "  p95 " C_FG "%.0fns" C_DIM "  p99 " C_FG "%.0fns" C_RESET "\n",
               s->hot_p50_ns, s->hot_p95_ns, s->hot_p99_ns); row++;
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

//------------------------------------------------------------------
// [SECTION]_[TUI READ KEY]
//------------------------------------------------------------------
static inline char TUI_ReadKey(EngineTUI *tui) {
    if (!tui->enabled) return 0;
    char c = 0;
    read(STDIN_FILENO, &c, 1);
    return c;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TUI_Render_Snapshot]
//======================================================================

//======================================================================
// [FUNCTION]_[tui_thread_fn]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the TUI thread — raw-mode + non-blocking stdin, 10 FPS tear-free ReadInto + ANSI_Render, q/p/r/s/k/l keys -> atomic request flags; terminal restored on exit]
//======================================================================
// [CODE]
//======================================================================
#include "TUIAnsi.hpp"

static inline void *tui_thread_fn(void *arg) {
    TUISharedState *shared = (TUISharedState *)arg;

    // ANSI TUI — zero library dependencies, diff-based rendering
    // raw mode + non-blocking stdin for input handling
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

        // v5.11.3.B — tear-free snapshot read (replaces direct active_idx load).
        TUISnapshot snap_local;
        TUISnapshot_ReadInto(shared, &snap_local);
        const TUISnapshot *s = &snap_local;
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
            else if (c == 'k' || c == 'K')
                __atomic_store_n(&shared->kill_reset_requested, 1, __ATOMIC_RELEASE);
            else if (c == 'l' || c == 'L')
                current_layout = (current_layout + 1) % ANSI_LAYOUT_COUNT;
        }

        usleep(100000); // 10 FPS
    }

    fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    printf("\033[?25h\033[H\033[J");  // show cursor, home, clear below (not full screen wipe)
    fflush(stdout);

    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[tui_thread_fn]
//======================================================================

#endif // MULTICORE_TUI
#endif // ENGINE_TUI_HPP
