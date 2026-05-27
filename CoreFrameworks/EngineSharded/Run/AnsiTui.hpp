// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [EngineSharded/Run/AnsiTui.hpp — ANSI-build TUI render loop (non-GUI builds)]
//======================================================================================================
// Sub-sub-file of CoreFrameworks/EngineSharded/Run.hpp (split per file-size-split-discipline.md
// at v5.15.5.F.4d.1.B.6 Phase B Step B.4.1; second-tier subfolder pattern to bring Run.hpp
// under the 1,500-line source-header threshold).
//
// Contains:
//   - EngineSharded_AnsiTui_RenderLoop<F> — the ANSI-build TUI render loop body. ~200 LOC of
//     fprintf-with-ANSI-color-tokens UI hoisted out of EngineSharded_Run's `#else USE_IMGUI_GUI`
//     branch. Refreshes ~5x/sec on stdout with per-core latency table + positions + OMS
//     counters + ws/reconciler status. Hides cursor on entry / restores on exit.
//
// **Hot-path discipline:** This is a RENDER LOOP that runs on the MAIN thread (the
// EngineSharded_Run caller's thread). It does NOT touch hot path or slow path. Lives in
// the same wait-for-shutdown role as the GUI build's `while (!shutdown) sleep_for(100ms);`
// loop. Header location has no latency impact.
//
// **Block-scope-statics-as-args (B.2 lesson):** The body originally referenced
// `g_user_data` (BinanceUserDataState) + `g_reconciler` (ReconciliationLoopState<F>) which
// are block-scope `static` inside EngineSharded_Run. These CANNOT be referenced from a
// hoisted header function — block-scope statics have function-local linkage (not
// translation-unit-shared). Passed as explicit args. Sister to Async.hpp pattern from
// .B.6 Phase B Step B.2 (g_tick_rec / g_depth_shared / g_shared / g_candle_acc passed
// the same way).
//
// **Live-trading-only fields gated:** g_user_data + g_reconciler are only meaningful when
// live_trading=1 (the `if (live_trading) { ... }` block in the body). Pass-through values
// of these args are unused under live_trading=0 (no dereference inside the gate).
//
// **SH_* color macros + #undefs (Decision G — atomic extraction unit):** All 9 SH_* color
// tokens + the SH_PNL helper macro are defined inside the function body (lexical scope) +
// #undef'd at the function tail. Same pattern as the inline-block predecessor. Preserves
// the no-leak-into-other-code property.
//
// **Caller signature shape:** Function takes per-EngineSharded_Run scalars (live_trading,
// num_cores, tsc_ghz, use_synthetic) + state references (state, oms, last_price,
// last_volume, ticks_produced, ticks_consumed_total) + block-scope statics by reference
// (g_user_data, g_reconciler, g_sharded_order_lat). All captured via explicit args at
// call site; the lambda lifetime exactly equals the engine's wait-for-shutdown phase.
//======================================================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "../../../FixedPoint/FixedPointN.hpp"
#include "../../ExecutionCore.hpp"             // ExecutionCore<F> + EventLoopState
#include "../../OrderManager.hpp"              // OrderManagerState + OrderManager_TotalSubmitted / _TotalFilled / _TotalRejected / _InflightCount
#include "../../ReconciliationLoop.hpp"        // ReconciliationLoopState<F>
#include "../../CoreLatencyStats.hpp"          // CoreLatencyStats_Snapshot + CoreLatencySnapshot
#include "../../ShardedOrderLatency.hpp"       // ShardedOrderLatency

#include "../../../DataStream/BinanceUserData.hpp"  // BinanceUserDataState

#include "../../../Strategies/StrategyParameters.hpp"  // NUM_STRATEGIES + STRATEGY_SHORT_NAMES

#include "../Boot.hpp"  // g_engine_sharded_shutdown — polled inside render loop

// parent_index: CoreFrameworks/EngineSharded/Run.hpp

namespace tt {

//======================================================================================================
// EngineSharded_AnsiTui_RenderLoop — ANSI-build TUI render loop body
//======================================================================================================
// Hoisted from EngineSharded_Run `#else USE_IMGUI_GUI` branch at v5.15.5.F.4d.1.B.6 Phase B
// Step B.4.1. Originally lived inline at Run.hpp:1778-2007. Body preserved verbatim modulo
// block-scope-static-to-arg translation.
//
// Returns after g_engine_sharded_shutdown is raised (engine join begins on caller side).
//
// Args (explicit captures-as-args per B.2 discipline; block-scope statics passed by
// reference; per-EngineSharded_Run scalars passed by value):
//   - live_trading: was the engine spawned in live mode? Gates WS + reconciler render block.
//   - num_cores: per-core count for latency table iteration.
//   - tsc_ghz: TSC frequency for ns conversion in latency table.
//   - use_synthetic: header banner string toggle ("synthetic ticks" vs "real Binance feed").
//   - state: EventLoopState<F> ref for state.oms->* reads + state.total_entries / _exits.
//   - oms: OrderManagerState<F> ref for OrderManager_Total* + InflightCount reads.
//   - last_price / last_volume: atomic doubles producer writes; render reads.
//   - ticks_produced / ticks_consumed_total: atomic counters producer + executors write.
//   - cores: ExecutionCore<F> array for CoreLatencyStats_Snapshot reads.
//   - g_user_data / g_reconciler: block-scope statics — passed by reference so live block
//     reads ws_connected / fills_received / events_received / total_polls / drift_corrections
//     / last_drift_usdt. Both unused under live_trading=0 (gate inside body).
//   - g_sharded_order_lat: file-shared inline singleton (Latency.hpp); pass by reference for
//     count / failures / total_us / min_us / max_us reads.
//======================================================================================================
template <unsigned F>
inline void EngineSharded_AnsiTui_RenderLoop(
    bool live_trading,
    int num_cores,
    double tsc_ghz,
    bool use_synthetic,
    EventLoopState<F>& state,
    OrderManagerState<F>& oms,
    std::atomic<double>& last_price,
    std::atomic<double>& last_volume,
    std::atomic<uint64_t>& ticks_produced,
    std::atomic<uint64_t>& ticks_consumed_total,
    const ExecutionCore<F>* cores,
    BinanceUserDataState& g_user_data,
    ReconciliationLoopState<F>& g_reconciler,
    ShardedOrderLatency& g_sharded_order_lat
) {
    // Live TUI render loop. Refreshes ~5x/sec on stdout with the per-core
    // latency table front and center. Uses the same warm color palette as
    // the legacy TUI for visual consistency. Hides the cursor while running
    // and clears the screen on each refresh; restores both on exit.
    //
    // ANSI escape codes used:
    //   \033[?25l   hide cursor
    //   \033[?25h   show cursor
    //   \033[2J     clear screen
    //   \033[H      cursor home (1;1)
    //   \033[K      clear to end of line
    //
    // Color tokens borrowed from DataStream/TUIAnsi.hpp.
    #define SH_RESET   "\033[0m"
    #define SH_BOLD    "\033[1m"
    #define SH_WHEAT   "\033[38;2;220;198;150m"
    #define SH_SAND    "\033[38;2;190;170;140m"
    #define SH_PEACH   "\033[38;2;230;165;120m"
    #define SH_FG      "\033[38;2;200;190;170m"
    #define SH_DIM     "\033[38;2;120;115;105m"
    #define SH_GREEN   "\033[38;2;140;195;130m"
    #define SH_RED     "\033[38;2;210;120;120m"
    #define SH_PNL(v)  ((v) >= 0.0 ? SH_GREEN : SH_RED)

    fprintf(stdout, "\033[?25l");  // hide cursor
    fflush(stdout);

    auto t_start = std::chrono::steady_clock::now();
    while (!g_engine_sharded_shutdown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Header
        fprintf(stdout, "\033[H");  // cursor home, no clear (avoids flicker)
        fprintf(stdout, SH_BOLD SH_PEACH "  /l、" SH_RESET "  " SH_BOLD SH_PEACH "FOXML TRADER" SH_RESET "  " SH_DIM "(per-core sharded)" SH_RESET "\033[K\n");
        fprintf(stdout, SH_DIM " ( °_ ° 7" SH_RESET "  " SH_DIM "engine v3.7.2" SH_RESET "  " SH_FG "%s" SH_RESET "\033[K\n",
                use_synthetic ? "synthetic ticks" : "real Binance feed");
        fprintf(stdout, SH_DIM "  ド  ヘ" SH_RESET "\033[K\n");
        fprintf(stdout, SH_DIM " じし_,)ノ" SH_RESET "\033[K\n");
        fprintf(stdout, "\033[K\n");

        // Top bar
        auto now = std::chrono::steady_clock::now();
        long uptime = std::chrono::duration_cast<std::chrono::seconds>(now - t_start).count();
        double bal = FPN_ToDouble(state.oms->balance);
        double pnl = FPN_ToDouble(state.oms->realized_pnl);
        int active = __builtin_popcount(state.oms->portfolio.active_bitmap);
        fprintf(stdout, " " SH_DIM "STATE: " SH_RESET SH_FG "ACTIVE" SH_RESET
                "  " SH_DIM "│" SH_RESET "  " SH_DIM "UPTIME: " SH_RESET SH_FG "%02ld:%02ld:%02ld" SH_RESET
                "  " SH_DIM "│" SH_RESET "  " SH_DIM "MODE: " SH_RESET SH_PEACH "SHARDED" SH_RESET "\033[K\n",
                uptime / 3600, (uptime / 60) % 60, uptime % 60);
        fprintf(stdout, "\033[K\n");

        // Market + account
        double price_d = last_price.load(std::memory_order_relaxed);
        double vol_d = last_volume.load(std::memory_order_relaxed);
        // compute equity = balance + unrealized P&L across all open positions
        double unrealized = 0.0;
        {
            uint16_t bm = state.oms->portfolio.active_bitmap;
            while (bm) {
                int s = __builtin_ctz(bm);
                bm &= (uint16_t)(bm - 1);
                double entry = FPN_ToDouble(state.oms->portfolio.positions[s].entry_price);
                double qty   = FPN_ToDouble(state.oms->portfolio.positions[s].quantity);
                unrealized += (price_d - entry) * qty;
            }
        }
        double equity = bal + unrealized;
        fprintf(stdout, " " SH_DIM " PRICE " SH_RESET SH_BOLD SH_WHEAT "$%.2f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "VOL " SH_RESET SH_FG "%.4f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "BAL " SH_RESET SH_FG "$%.2f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "EQUITY " SH_RESET SH_BOLD "%s$%.2f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "POS " SH_RESET SH_FG "%d/%u" SH_RESET "\033[K\n",
                price_d, vol_d, bal, SH_PNL(equity - 10000.0), equity, active, (unsigned)num_cores);
        fprintf(stdout, " " SH_DIM " P&L " SH_RESET SH_BOLD "%s$%+.4f" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "UNREAL " SH_RESET SH_BOLD "%s$%+.4f" SH_RESET "\033[K\n",
                SH_PNL(pnl), pnl, SH_PNL(unrealized), unrealized);
        fprintf(stdout, "\033[K\n");

        // Counters
        fprintf(stdout, " " SH_DIM " produced  " SH_RESET SH_FG "%lu" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "consumed  " SH_RESET SH_FG "%lu" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "entries  " SH_RESET SH_FG "%lu" SH_RESET
                "  " SH_DIM "│" SH_RESET " " SH_DIM "exits  " SH_RESET SH_FG "%lu" SH_RESET "\033[K\n",
                (unsigned long)ticks_produced.load(),
                (unsigned long)ticks_consumed_total.load(),
                (unsigned long)state.total_entries,
                (unsigned long)state.total_exits);
        fprintf(stdout, "\033[K\n");

        // Per-core latency table — the headline
        fprintf(stdout, SH_BOLD SH_PEACH " PER-CORE LATENCY" SH_RESET SH_DIM "  (last 256 samples per core)" SH_RESET "\033[K\n");
        fprintf(stdout, "  " SH_DIM "core   samples       min        p50        p95        p99        max        avg" SH_RESET "\033[K\n");
        for (int i = 0; i < num_cores; ++i) {
            CoreLatencySnapshot ls = CoreLatencyStats_Snapshot(&cores[i].latency_stats, tsc_ghz);
            if (ls.total_count == 0) {
                fprintf(stdout, "  " SH_FG " %2d   " SH_DIM "%8s   %6s ns   %6s ns   %6s ns   %6s ns   %6s ns   %6s ns" SH_RESET "\033[K\n",
                        i, "0", "-", "-", "-", "-", "-", "-");
            } else {
                fprintf(stdout, "  " SH_FG " %2d   " SH_DIM "%8lu   " SH_FG "%6.0f ns   %6.0f ns   %6.0f ns   %6.0f ns   %6.0f ns   %6.0f ns" SH_RESET "\033[K\n",
                        i, (unsigned long)ls.total_count,
                        ls.min_ns, ls.p50_ns, ls.p95_ns, ls.p99_ns, ls.max_ns, ls.avg_ns);
            }
        }

        // Per-core position details — shows which core has a position,
        // entry price, quantity, unrealized P&L, and TP/SL levels.
        // only renders rows for active positions to keep the display compact.
        if (active > 0) {
            fprintf(stdout, "\033[K\n");
            fprintf(stdout, SH_BOLD SH_PEACH " POSITIONS" SH_RESET "\033[K\n");
            fprintf(stdout, "  " SH_DIM "core   strategy     entry          qty       unreal         TP             SL" SH_RESET "\033[K\n");
            uint16_t bm = state.oms->portfolio.active_bitmap;
            while (bm) {
                int s = __builtin_ctz(bm);
                bm &= (uint16_t)(bm - 1);
                double entry_d = FPN_ToDouble(state.oms->portfolio.positions[s].entry_price);
                double qty_d   = FPN_ToDouble(state.oms->portfolio.positions[s].quantity);
                double tp_d    = FPN_ToDouble(state.oms->portfolio.positions[s].take_profit_price);
                double sl_d    = FPN_ToDouble(state.oms->portfolio.positions[s].stop_loss_price);
                double unreal_d = (price_d - entry_d) * qty_d;
                const char* strat = (s < state.registered_count && state.cores[s].strategy_id < NUM_STRATEGIES)
                    ? STRATEGY_SHORT_NAMES[state.cores[s].strategy_id] : "?";
                fprintf(stdout, "  " SH_FG " %2d    " SH_PEACH "%-5s" SH_RESET
                        "  " SH_FG "$%10.2f   %10.6f   " SH_BOLD "%s$%+8.4f" SH_RESET
                        "   " SH_GREEN "$%.2f" SH_RESET "   " SH_RED "$%.2f" SH_RESET "\033[K\n",
                        s, strat, entry_d, qty_d,
                        SH_PNL(unreal_d), unreal_d,
                        tp_d, sl_d);
            }
        }
        fprintf(stdout, "\033[K\n");

        // Order latency line — only meaningful in live mode, but always shown
        // so paper users see the placeholder. all values in microseconds.
        uint64_t ord_count    = g_sharded_order_lat.count.load(std::memory_order_relaxed);
        uint64_t ord_failures = g_sharded_order_lat.failures.load(std::memory_order_relaxed);
        uint64_t ord_total_us = g_sharded_order_lat.total_us.load(std::memory_order_relaxed);
        uint64_t ord_min_us   = g_sharded_order_lat.min_us.load(std::memory_order_relaxed);
        uint64_t ord_max_us   = g_sharded_order_lat.max_us.load(std::memory_order_relaxed);
        fprintf(stdout, "\033[K\n");
        if (live_trading) {
            if (ord_count > 0) {
                double ord_avg_us = (double)ord_total_us / (double)ord_count;
                fprintf(stdout, " " SH_BOLD SH_PEACH "ORDER LATENCY" SH_RESET SH_DIM " (live)" SH_RESET
                        "  " SH_DIM "orders" SH_RESET " " SH_FG "%lu" SH_RESET
                        "  " SH_DIM "fail" SH_RESET " " SH_FG "%lu" SH_RESET
                        "  " SH_DIM "min" SH_RESET " " SH_FG "%lu µs" SH_RESET
                        "  " SH_DIM "avg" SH_RESET " " SH_FG "%.0f µs" SH_RESET
                        "  " SH_DIM "max" SH_RESET " " SH_FG "%lu µs" SH_RESET "\033[K\n",
                        (unsigned long)ord_count, (unsigned long)ord_failures,
                        (unsigned long)ord_min_us, ord_avg_us, (unsigned long)ord_max_us);
            } else {
                fprintf(stdout, " " SH_BOLD SH_PEACH "ORDER LATENCY" SH_RESET SH_DIM " (live, no orders yet)" SH_RESET "\033[K\n");
            }
        } else {
            fprintf(stdout, " " SH_DIM "ORDER LATENCY: paper mode (no orders submitted)" SH_RESET "\033[K\n");
        }

        // OMS counter line. shows the OrderManager-side view: submissions
        // it received from the drainer, fills it observed (paper mode bumps
        // both immediately, live mode bumps filled when the adapter callback
        // comes back), rejections, and how many orders are still in flight
        // in the OMS table. orthogonal to the per-core latency above.
        uint64_t oms_sub      = OrderManager_TotalSubmitted(&oms);
        uint64_t oms_fill     = OrderManager_TotalFilled(&oms);
        uint64_t oms_rej      = OrderManager_TotalRejected(&oms);
        int      oms_inflight = OrderManager_InflightCount(&oms);
        fprintf(stdout, " " SH_BOLD SH_PEACH "OMS" SH_RESET
                "          " SH_DIM "submitted" SH_RESET " " SH_FG "%lu" SH_RESET
                "  " SH_DIM "filled" SH_RESET " " SH_FG "%lu" SH_RESET
                "  " SH_DIM "rejected" SH_RESET " " SH_FG "%lu" SH_RESET
                "  " SH_DIM "inflight" SH_RESET " " SH_FG "%d" SH_RESET "\033[K\n",
                (unsigned long)oms_sub, (unsigned long)oms_fill,
                (unsigned long)oms_rej, oms_inflight);

        // User data WS status line (phase 04)
        if (live_trading) {
            int ws_conn = g_user_data.ws_connected.load(std::memory_order_relaxed);
            uint64_t ws_fills = g_user_data.fills_received.load(std::memory_order_relaxed);
            uint64_t ws_events = g_user_data.events_received.load(std::memory_order_relaxed);
            fprintf(stdout, " " SH_BOLD SH_PEACH "WS FILLS" SH_RESET
                    "     " SH_DIM "status" SH_RESET " %s"
                    "  " SH_DIM "fills" SH_RESET " " SH_FG "%lu" SH_RESET
                    "  " SH_DIM "events" SH_RESET " " SH_FG "%lu" SH_RESET "\033[K\n",
                    ws_conn ? (SH_GREEN "CONNECTED" SH_RESET)
                            : (SH_RED "DISCONNECTED" SH_RESET),
                    (unsigned long)ws_fills, (unsigned long)ws_events);
            // Reconciler status
            uint64_t rc_polls = g_reconciler.total_polls.load(std::memory_order_relaxed);
            uint64_t rc_corr  = g_reconciler.drift_corrections.load(std::memory_order_relaxed);
            double   rc_drift = g_reconciler.last_drift_usdt.load(std::memory_order_relaxed);
            fprintf(stdout, " " SH_BOLD SH_PEACH "RECONCILE" SH_RESET
                    "    " SH_DIM "polls" SH_RESET " " SH_FG "%lu" SH_RESET
                    "  " SH_DIM "corrections" SH_RESET " " SH_FG "%lu" SH_RESET
                    "  " SH_DIM "drift" SH_RESET " %s$%.4f" SH_RESET "\033[K\n",
                    (unsigned long)rc_polls, (unsigned long)rc_corr,
                    SH_PNL(rc_drift), rc_drift);
        }

        // Footer
        fprintf(stdout, "\033[K\n");
        fprintf(stdout, " " SH_DIM "Press Ctrl+C to stop and dump final stats. Subtract ~25-30ns rdtsc floor for actual work cost." SH_RESET "\033[K\n");
        // Clear any trailing rows from a previous render with more cores
        for (int i = 0; i < 4; ++i) fprintf(stdout, "\033[K\n");
        fflush(stdout);
    }

    fprintf(stdout, "\033[?25h");  // show cursor
    fprintf(stdout, "\n");
    fflush(stdout);

    #undef SH_RESET
    #undef SH_BOLD
    #undef SH_WHEAT
    #undef SH_SAND
    #undef SH_PEACH
    #undef SH_FG
    #undef SH_DIM
    #undef SH_GREEN
    #undef SH_RED
    #undef SH_PNL
}

} // namespace tt
