// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [TICK TRADER ENGINE]
//======================================================================================================
// main event loop - wires the Binance data stream to the execution pipeline
// single-threaded, poll-based: BinanceStream -> BuyGate -> PositionExitGate -> PortfolioController
//
// the engine runs identically whether data is arriving or not - poll() timeout ensures
// exit gates are always checked against last known price
//
// burst drain: when multiple frames buffer between poll cycles (volatile markets),
// we drain ALL of them, running exit gates on each intermediate price so TP/SL triggers
// arent missed in a burst. BuyGate only runs on the final/freshest price
//
// session lifecycle: 24-hour cycle with clean wind-down, position close, and reconnect
// reconnect procedure is airtight - verifies bitmap is zero before proceeding
//======================================================================================================
#include "DataStream/BinanceCrypto.hpp"
#include "DataStream/BinanceOrderAPI.hpp"
#include "DataStream/EngineTUI.hpp"
#include "CoreFrameworks/PortfolioController.hpp"
#include "MemHeaders/PoolAllocator.hpp"
#include "DataStream/TradeLog.hpp"
#include "DataStream/MetricsLog.hpp"

#include <stdio.h>
#include <stdlib.h>

#ifdef LATENCY_PROFILING
#include <x86intrin.h>
#include <unistd.h>
#endif

constexpr unsigned FP = 64;

//======================================================================================================
// [AIRTIGHT RECONNECT]
//======================================================================================================
// the key guarantee: we NEVER reconnect with orphaned positions
// if the bitmap isnt zero after force-close, thats a bug and we halt rather than lose track of money
//======================================================================================================
static inline void engine_force_close_all(PortfolioController<FP> *ctrl, TradeLog *log, FPN<FP> last_price) {
    uint16_t active = ctrl->portfolio.active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);
        Position<FP> *pos = &ctrl->portfolio.positions[idx];

        double entry_d   = FPN_ToDouble(pos->entry_price);
        double exit_d    = FPN_ToDouble(last_price);
        double qty_d     = FPN_ToDouble(pos->quantity);
        double delta_pct = 0.0;
        if (entry_d != 0.0) delta_pct = ((exit_d - entry_d) / entry_d) * 100.0;

        // book P&L, balance, and fees (same accounting as DrainExits)
        FPN<FP> gross_proceeds = FPN_Mul(last_price, pos->quantity);
        FPN<FP> exit_fee = FPN_Mul(gross_proceeds, ctrl->config.fee_rate);
        FPN<FP> net_proceeds = FPN_SubSat(gross_proceeds, exit_fee);
        FPN<FP> entry_cost = FPN_Mul(pos->entry_price, pos->quantity);
        FPN<FP> entry_fee = FPN_Mul(entry_cost, ctrl->config.fee_rate);
        FPN<FP> pos_pnl = FPN_Sub(net_proceeds, FPN_AddSat(entry_cost, entry_fee));

        ctrl->balance = FPN_AddSat(ctrl->balance, net_proceeds);
        ctrl->realized_pnl = FPN_AddSat(ctrl->realized_pnl, pos_pnl);
        ctrl->total_fees = FPN_AddSat(ctrl->total_fees, exit_fee);
        ctrl->losses++;

        { TradeLogRecord r = {};
          r.tick = ctrl->total_ticks; r.price = exit_d; r.quantity = qty_d;
          r.entry_price = entry_d; r.delta_pct = delta_pct;
          r.balance = FPN_ToDouble(ctrl->balance);
          r.strategy_id = ctrl->entry_strategy[idx];
          r.regime = ctrl->regime.current_regime;
          snprintf(r.reason, sizeof(r.reason), "SESSION_CLOSE");
          TradeLog_Sell(log, &r); }

        Portfolio_RemovePosition(&ctrl->portfolio, idx);
        active &= active - 1;
    }

    // hard gate: verify bitmap is zero
    if (ctrl->portfolio.active_bitmap != 0) {
        fprintf(stderr, "[ENGINE] FATAL: bitmap not zero after force-close: 0x%04X\n",
                ctrl->portfolio.active_bitmap);
        fprintf(stderr, "[ENGINE] halting - refusing to reconnect with orphaned positions\n");
        exit(1);
    }
}

//======================================================================================================
// [MAIN]
//======================================================================================================
int main(int argc, char *argv[]) {
    fprintf(stderr, "Tick Trader — Copyright (c) 2026 Jennifer Lewis. All rights reserved.\n");
    fprintf(stderr, "Licensed under AGPL-3.0-or-later. Commercial license: jenn.lewis5789@gmail.com\n\n");

    const char *cfg_path = (argc > 1) ? argv[1] : "engine.cfg";

    //==================================================================================================
    // load configs
    //==================================================================================================
    BinanceConfig bcfg       = BinanceConfig_Load(cfg_path);
    ControllerConfig<FP> ccfg = ControllerConfig_Load<FP>(cfg_path);

    // auto-redirect stderr to log file — always when log_file is set
    // rotates on startup: engine.log → engine.log.1 (keeps one previous session)
    // in TUI mode: diagnostics go to file instead of being eaten by screen redraws
    // in headless mode: no manual 2>engine.log needed
    if (bcfg.log_file[0]) {
        char prev[272];
        snprintf(prev, sizeof(prev), "%s.1", bcfg.log_file);
        rename(bcfg.log_file, prev); // silently fails if no existing log
        FILE *lf = freopen(bcfg.log_file, "w", stderr);
        if (!lf) {
            perror("freopen log_file");
        } else {
            setvbuf(stderr, NULL, _IOLBF, 0); // line-buffered so tail -f works
        }
    }

    //==================================================================================================
    // init stream
    //==================================================================================================
    BinanceStream bs;
    if (!BinanceStream_Init(&bs, &bcfg)) {
        fprintf(stderr, "[ENGINE] failed to connect - exiting\n");
        return 1;
    }

    //==================================================================================================
    // init controller
    //==================================================================================================
    PortfolioController<FP> ctrl;
    ctrl.rolling_long = NULL;  // Init checks this before free on reinit
    PortfolioController_Init(&ctrl, ccfg);

    // try to load snapshot from previous session
    const char *snapshot_path = ccfg.use_real_money ? "portfolio.live.snapshot" : "portfolio.paper.snapshot";
    if (PortfolioController_LoadSnapshot(&ctrl, snapshot_path)) {
        int pos_count = Portfolio_CountActive(&ctrl.portfolio);
        fprintf(stderr, "[ENGINE] resumed %d positions from snapshot\n", pos_count);
        fprintf(stderr, "[ENGINE] realized P&L: $%.4f\n", FPN_ToDouble(ctrl.realized_pnl));
    }

    //==================================================================================================
    // init order pool
    //==================================================================================================
    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);

    //==================================================================================================
    // init trade log
    //==================================================================================================
    TradeLog log;
    TradeLog_Init(&log, bcfg.symbol);

    // metrics log — diagnostics for verifying regime switching, strategy behavior
    MetricsLog metrics;
    char metrics_path[128];
    snprintf(metrics_path, sizeof(metrics_path), "%s_metrics.csv", bcfg.symbol);
    MetricsLog_Init(&metrics, metrics_path);

    //==================================================================================================
    // init live trading (REST API)
    //==================================================================================================
    BinanceOrderAPI order_api = {};
    uint16_t live_position_bitmap = 0; // which paper slots have real Binance orders

    if (ccfg.use_real_money) {
        char api_key[128] = {}, api_secret[128] = {};
        if (!LoadSecrets("secrets.cfg", api_key, api_secret)) {
            fprintf(stderr, "[ENGINE] ERROR: use_real_money=1 but secrets.cfg missing or incomplete\n");
            return 1;
        }
        // REST host for orders — always use Binance US for US-based users
        // websocket data source is separate (controlled by use_binance_us in BinanceConfig)
        // for US users: set use_binance_us=0 for fast Global websocket data,
        // the REST API always goes to api.binance.us when not testnet
        const char *rest_host = bcfg.use_testnet ? "testnet.binance.vision" : "api.binance.us";
        if (!BinanceOrderAPI_Init(&order_api, rest_host, api_key, api_secret, bcfg.symbol)) {
            fprintf(stderr, "[ENGINE] ERROR: failed to connect to REST API at %s\n", rest_host);
            return 1;
        }
        fprintf(stderr, "╔════════════════════════════════════════╗\n");
        fprintf(stderr, "║   LIVE TRADING MODE ENABLED            ║\n");
        fprintf(stderr, "║   Real orders will be placed on %s  ║\n",
                bcfg.use_testnet ? "TESTNET" : "PRODUCTION");
        fprintf(stderr, "╚════════════════════════════════════════╝\n");
        if (!bcfg.use_testnet) {
            fprintf(stderr, "[SAFETY] WARNING: Trading with REAL MONEY\n");
            fprintf(stderr, "[SAFETY] Starting in 10 seconds... (Ctrl+C to abort)\n");
            sleep(10);
        }

        // startup reconciliation: query actual balances, reconcile with snapshot
        double usdt_start = 0, btc_start = 0;
        if (!BinanceOrderAPI_GetBalances(&order_api, &usdt_start, &btc_start)) {
            fprintf(stderr, "[LIVE] FATAL: could not query exchange balances at startup\n");
            BinanceOrderAPI_Cleanup(&order_api);
            return 1;
        }
        fprintf(stderr, "[LIVE] exchange USDT: $%.2f  BTC: %.8f ($%.2f)\n",
                usdt_start, btc_start, btc_start * 70000.0); // approx, no price yet

        int pos_count = Portfolio_CountActive(&ctrl.portfolio);
        if (btc_start > 0.000001 && pos_count > 0) {
            // have BTC and snapshot positions — mark as live
            live_position_bitmap = ctrl.portfolio.active_bitmap;
            fprintf(stderr, "[LIVE] %d snapshot positions matched with BTC holdings\n", pos_count);
        } else if (btc_start < 0.000001 && pos_count > 0) {
            // no BTC but snapshot has positions — they were closed externally
            fprintf(stderr, "[LIVE] no BTC on exchange — clearing %d stale snapshot positions\n", pos_count);
            ctrl.portfolio.active_bitmap = 0;
            live_position_bitmap = 0;
        } else {
            // clean start or no positions
            live_position_bitmap = 0;
            // if BTC on exchange but no snapshot positions, sell it all —
            // recovers orphaned value from prior sessions into USDT
            if (btc_start > 0.000001) {
                double qty_d = binance_round_qty(btc_start, order_api.filters.lot_step_size);
                if (qty_d >= order_api.filters.lot_min_qty) {
                    fprintf(stderr, "[LIVE] orphaned BTC %.8f — selling to recover USDT\n", qty_d);
                    char oid[32]; double fp = 0, fq = 0;
                    BinanceOrderAPI_MarketSell(&order_api, qty_d, oid, &fp, &fq);
                    // re-query USDT balance after sell
                    BinanceOrderAPI_GetBalances(&order_api, &usdt_start, &btc_start);
                    fprintf(stderr, "[LIVE] post-sweep USDT: $%.2f  BTC: %.8f\n", usdt_start, btc_start);
                }
            }
        }
        ctrl.balance = FPN_FromDouble<FP>(usdt_start);
        // in live mode, starting_balance = actual exchange equity, not engine.cfg
        ctrl.config.starting_balance = FPN_FromDouble<FP>(usdt_start);
        fprintf(stderr, "[LIVE] starting_balance set to $%.2f from exchange\n", usdt_start);
    }

    //==================================================================================================
    // init TUI
    //==================================================================================================
#ifdef MULTICORE_TUI
    // multicore: TUI runs on separate thread, engine thread never renders
    TUISharedState shared = {};
    shared.config_path = cfg_path;
    shared.active_idx = 0;
    shared.quit_requested = 0;
    shared.pause_requested = 0;
    shared.reload_requested = 0;

    pthread_t tui_tid;
    pthread_create(&tui_tid, NULL, tui_thread_fn, &shared);

#ifdef __linux__
    // pin engine to Core 3 (usually quieter), TUI to Core 2
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset); CPU_SET(3, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0)
        fprintf(stderr, "[ENGINE] warning: failed to pin engine to core 3\n");
    CPU_ZERO(&cpuset); CPU_SET(2, &cpuset);
    if (pthread_setaffinity_np(tui_tid, sizeof(cpuset), &cpuset) != 0)
        fprintf(stderr, "[ENGINE] warning: failed to pin TUI to core 2\n");
#endif

    // latency stats live on engine thread (not shared TUI struct)
    EngineTUI tui = {};  // dummy for latency stats in profiling mode
    tui.start_time = (uint64_t)time(NULL);
#else
    EngineTUI tui;
#ifdef LATENCY_BENCH
    TUI_Init(&tui, 0, 10);  // bench mode: TUI disabled for clean latency measurement
#else
    TUI_Init(&tui, bcfg.tui_enabled, 10);
#endif
#endif // MULTICORE_TUI

#ifdef LATENCY_PROFILING
    // calibrate TSC: measure cycles over a known 10ms sleep to get cycles-per-nanosecond
    {
        unsigned int tsc_aux;
        uint64_t cal_start = __rdtscp(&tsc_aux);
        usleep(10000); // 10ms
        uint64_t cal_end = __rdtscp(&tsc_aux);
        tui.tsc_per_ns = (double)(cal_end - cal_start) / 10000000.0; // 10ms = 10M ns
        tui.hot_min = UINT64_MAX; tui.hot_max = 0; tui.hot_sum = 0; tui.hot_count = 0;
        tui.slow_min = UINT64_MAX; tui.slow_max = 0; tui.slow_sum = 0; tui.slow_count = 0;
        tui.bg_sum = 0; tui.bg_max = 0;
        tui.eg_sum = 0; tui.eg_max = 0;
        tui.pc_sum = 0; tui.pc_max = 0;
        tui.pc_fill_sum = 0; tui.pc_fill_max = 0; tui.pc_fill_count = 0;
        tui.pc_nofill_sum = 0; tui.pc_nofill_max = 0; tui.pc_nofill_count = 0;
        tui.eg_pos_sum = 0;
        memset(tui.hot_hist, 0, sizeof(tui.hot_hist));
        fprintf(stderr, "[LATENCY] TSC calibrated: %.2f cycles/ns (%.1f GHz)\n",
                tui.tsc_per_ns, tui.tsc_per_ns);
    }
#endif

    //==================================================================================================
    // main loop
    //==================================================================================================
    DataStream<FP> last_stream = {};
    int running = 1;
    int has_data = 0;  // dont run engine until we have at least one real tick

    while (running) {
#ifdef BUSY_POLL
        // spin-poll: never sleep, keeps L1/L2/icache permanently warm
        // trades CPU power for minimum latency (~100-200ns vs ~1000ns)
        int ready;
        while (!(ready = BinanceStream_Poll(&bs, 0))) {
            __builtin_ia32_pause(); // hardware hint to CPU for better spin-wait
        }
#else
        int ready = BinanceStream_Poll(&bs, bcfg.poll_timeout_ms);
#endif

        //==============================================================================================
        // DATA INGESTION - drain all buffered frames
        //==============================================================================================
        // during volatile markets binance can burst 100+ trades/sec
        // we drain ALL frames, running exit gates on each intermediate price
        // so TP/SL triggers arent missed in a burst
        // BuyGate only runs on the final/freshest price after the drain
        //==============================================================================================
        if (ready & POLL_SOCKET) {
            while (1) {
                DataStream<FP> tick;
                int ok = BinanceStream_ReadTick(&bs, &tick);
                if (!ok) {
                    // save snapshot before reconnect - positions survive
                    PortfolioController_SaveSnapshot(&ctrl, snapshot_path);
                    fprintf(stderr, "[ENGINE] connection lost - reconnecting (positions preserved)\n");
                    BinanceStream_Reconnect(&bs, &bcfg);
                    break;
                }
                last_stream = tick;
                has_data = 1;

            // exit gate on EVERY tick in the burst
            if (ctrl.portfolio.active_bitmap) {
#if defined(LATENCY_PROFILING) && !defined(LATENCY_LITE)
                unsigned int tsc_aux;
                uint64_t t1 = __rdtscp(&tsc_aux);
                PositionExitGate(&ctrl.portfolio, last_stream.price, &ctrl.exit_buf, ctrl.total_ticks);
                uint64_t t2 = __rdtscp(&tsc_aux);
                tui.eg_sum += (t2 - t1);
                if ((t2-t1) > tui.eg_max) tui.eg_max = (t2-t1);
                tui.eg_pos_sum += __builtin_popcount(ctrl.portfolio.active_bitmap);
#else
                PositionExitGate(&ctrl.portfolio, last_stream.price, &ctrl.exit_buf, ctrl.total_ticks);
#endif
            }

                // drain until SSL has no more buffered data
                if (!BinanceStream_HasPending(&bs)) break;
            }
        }

        //==============================================================================================
        // TUI INPUT
        //==============================================================================================
#ifdef MULTICORE_TUI
        // multicore: check atomic flags from TUI thread
        if (__atomic_load_n(&shared.quit_requested, __ATOMIC_ACQUIRE))
            running = 0;
        if (__atomic_exchange_n(&shared.pause_requested, 0, __ATOMIC_ACQ_REL)) {
            int is_paused = FPN_IsZero(ctrl.buy_conds.price);
            if (is_paused)
                PortfolioController_Unpause(&ctrl);
            else {
                ctrl.buy_conds.price  = FPN_Zero<FP>();
                ctrl.buy_conds.volume = FPN_Zero<FP>();
            }
        }
        if (__atomic_exchange_n(&shared.regime_cycle_requested, 0, __ATOMIC_ACQ_REL))
            PortfolioController_CycleRegime(&ctrl);
        if (__atomic_exchange_n(&shared.reload_requested, 0, __ATOMIC_ACQ_REL)) {
            ControllerConfig<FP> new_cfg = ControllerConfig_Load<FP>(cfg_path);
            PortfolioController_HotReload(&ctrl, new_cfg);
            fprintf(stderr, "[ENGINE] config reloaded from %s\n", cfg_path);
        }
#else
        if (ready & POLL_STDIN) {
            TUI_HandleInput(&tui, &ctrl, cfg_path, &running);
        }
#endif

        //==============================================================================================
        // SESSION LIFECYCLE
        //==============================================================================================
        if (BinanceStream_InWindDown(&bs, bcfg.wind_down_minutes)) {
            // disable buy gate during wind-down
            ctrl.buy_conds.price  = FPN_Zero<FP>();
            ctrl.buy_conds.volume = FPN_Zero<FP>();
        }

        if (BinanceStream_ShouldReconnect(&bs)) {
            // 24-hour session boundary - planned reconnect
            // save snapshot before force-close in case something goes wrong
            PortfolioController_SaveSnapshot(&ctrl, snapshot_path);
            fprintf(stderr, "[ENGINE] 24h session boundary - closing positions and reconnecting\n");

            // airtight reconnect procedure:
            // 1. buy gate already disabled (wind-down)
            // 2. force-close all remaining positions
            // 3. verify bitmap is zero (halts if not)
            // 4. clear pool, reconnect, restart warmup
            // LIVE: sell entire BTC balance before 24h force-close
            // sells everything in one order — catches tracked positions + any dust
            if (ccfg.use_real_money) {
                double usdt_tmp = 0, btc_bal = 0;
                BinanceOrderAPI_GetBalances(&order_api, &usdt_tmp, &btc_bal);
                double qty_d = binance_round_qty(btc_bal, order_api.filters.lot_step_size);
                if (qty_d >= order_api.filters.lot_min_qty) {
                    double notional = qty_d * last_stream.price_d;
                    if (notional >= order_api.filters.min_notional) {
                        char oid[32]; double fp = 0, fq = 0;
                        fprintf(stderr, "[LIVE] 24h reconnect — selling entire BTC balance: %.8f ($%.2f)\n", qty_d, notional);
                        BinanceOrderAPI_MarketSell(&order_api, qty_d, oid, &fp, &fq);
                    }
                }
                live_position_bitmap = 0;
            }
            engine_force_close_all(&ctrl, &log, last_stream.price);

            // clear pool
            pool.bitmap = 0;

            // reconnect
            BinanceStream_Reconnect(&bs, &bcfg);

            // reset controller to warmup
            PortfolioController_Init(&ctrl, ccfg);

            continue;
        }

        //==============================================================================================
        // ENGINE TICK - runs on timeout with last known price, but only after first real tick
        //==============================================================================================
        if (has_data) {
#ifdef MULTICORE_TUI
            // live update to active snapshot — 3 stores, 1 cache line, no FPN_ToDouble
            // price/volume from stashed doubles (atof during websocket parse)
            // active_count from hot-path bitmap (already in L1 from ExitGate)
            {
                int tui_idx = __atomic_load_n(&shared.active_idx, __ATOMIC_ACQUIRE);
                shared.snapshots[tui_idx].price = last_stream.price_d;
                shared.snapshots[tui_idx].volume = last_stream.volume_d;
                shared.snapshots[tui_idx].active_count = __builtin_popcount(ctrl.portfolio.active_bitmap);
                shared.snapshots[tui_idx].roll_count = ctrl.rolling.count;
                shared.snapshots[tui_idx].state_warmup = (ctrl.state == CONTROLLER_WARMUP);
            }
#endif
#ifdef LATENCY_PROFILING
            unsigned int tsc_aux;
            uint64_t t0 = __rdtscp(&tsc_aux);
#endif
            BuyGate(&ctrl.buy_conds, &last_stream, &pool);
#if defined(LATENCY_PROFILING) && !defined(LATENCY_LITE)
            uint64_t t1 = __rdtscp(&tsc_aux);
#endif
            if (ctrl.portfolio.active_bitmap)
                PositionExitGate(&ctrl.portfolio, last_stream.price, &ctrl.exit_buf, ctrl.total_ticks);
            int regime_before = ctrl.regime.current_regime;
            int strategy_before = ctrl.strategy_id;
            uint32_t buys_before_metrics = ctrl.total_buys;
#if defined(LATENCY_PROFILING) && !defined(LATENCY_LITE)
            uint64_t t2 = __rdtscp(&tsc_aux);
#endif
            // LIVE: save exit buffer before PCTick drains it (DrainExits clears inside)
            // config-constant branch: predicted 100% in paper mode, ~0ns cost
            int saved_exit_count = 0;
            uint32_t saved_exit_slots[16];
            double saved_exit_qtys[16];
            if (ccfg.use_real_money && ctrl.exit_buf.count > 0) {
                for (uint32_t i = 0; i < ctrl.exit_buf.count; i++) {
                    uint32_t pidx = ctrl.exit_buf.records[i].position_index;
                    saved_exit_slots[saved_exit_count] = pidx;
                    saved_exit_qtys[saved_exit_count] = FPN_ToDouble(
                        ctrl.portfolio.positions[pidx].quantity);
                    saved_exit_count++;
                }
            }
            PortfolioController_Tick(&ctrl, &pool, last_stream.price, last_stream.volume, &log);
#ifdef LATENCY_PROFILING
            uint64_t t3 = __rdtscp(&tsc_aux);
#endif

            // log regime/strategy changes and fills (OUTSIDE high-precision window)
            if (ctrl.regime.current_regime != regime_before) {
                char detail[128];
                snprintf(detail, sizeof(detail), "%s->%s",
                         _regime_str(regime_before), _regime_str(ctrl.regime.current_regime));
                MetricsLog_Event(&metrics, &ctrl, last_stream.price_d, "REGIME_CHANGE", detail);
            }
            if (ctrl.strategy_id != strategy_before) {
                char detail[128];
                snprintf(detail, sizeof(detail), "%s->%s",
                         _strategy_str(strategy_before), _strategy_str(ctrl.strategy_id));
                MetricsLog_Event(&metrics, &ctrl, last_stream.price_d, "STRATEGY_SWITCH", detail);
            }
            if (ctrl.total_buys > buys_before_metrics) {
                char detail[64];
                snprintf(detail, sizeof(detail), "positions:%d strategy:%s",
                         __builtin_popcount(ctrl.portfolio.active_bitmap), _strategy_str(ctrl.strategy_id));
                MetricsLog_Event(&metrics, &ctrl, last_stream.price_d, "FILL", detail);

                // LIVE: fire-and-forget buy order + set bitmap (Phase 3 wires this)
                // skip if a sell happened this same tick — avoid back-to-back REST calls
                // the paper position will be undone below, BuyGate fires again next cycle
                if (ccfg.use_real_money && saved_exit_count == 0) {
                    uint16_t active = ctrl.portfolio.active_bitmap;
                    while (active) {
                        int slot = __builtin_ctz(active);
                        if (ctrl.entry_ticks[slot] == ctrl.total_ticks) {
                            double qty_d = FPN_ToDouble(ctrl.portfolio.positions[slot].quantity);
                            double notional = qty_d * last_stream.price_d;
                            // 2x minNotional ensures sells also pass (Binance uses 5-min avg price)
                            if (qty_d >= order_api.filters.lot_min_qty
                                && notional >= order_api.filters.min_notional * 2.0) {
                                char oid[32];
                                double fp = 0, fq = 0;
                                if (BinanceOrderAPI_MarketBuy(&order_api, qty_d, oid, &fp, &fq)) {
                                    live_position_bitmap |= (1 << slot);
                                    // sync paper position to actual fill
                                    if (fq > 0 && fabs(fq - qty_d) > 1e-10)
                                        ctrl.portfolio.positions[slot].quantity = FPN_FromDouble<FP>(fq);
                                    if (fp > 0)
                                        ctrl.portfolio.positions[slot].entry_price = FPN_FromDouble<FP>(fp);
                                } else {
                                    fprintf(stderr, "[LIVE] BUY failed — paper position kept, no real backing\n");
                                }
                            } else {
                                fprintf(stderr, "[LIVE] qty too small (%.8f) or below minNotional — skipped\n", qty_d);
                            }
                        }
                        active &= active - 1;
                    }
                }

                // undo paper positions that weren't backed by real orders
                // with single-slot mode, if the buy was skipped or failed, the paper
                // engine shouldn't track a phantom position
                if (ccfg.use_real_money) {
                    uint16_t check = ctrl.portfolio.active_bitmap;
                    while (check) {
                        int slot = __builtin_ctz(check);
                        if (ctrl.entry_ticks[slot] == ctrl.total_ticks
                            && !(live_position_bitmap & (1 << slot))) {
                            double qty_d = FPN_ToDouble(ctrl.portfolio.positions[slot].quantity);
                            double cost = qty_d * last_stream.price_d;
                            fprintf(stderr, "[LIVE] undoing paper position slot %d — no real backing (notional $%.2f)\n", slot, cost);
                            ctrl.portfolio.active_bitmap &= ~(1 << slot);
                            // restore balance: reverse the cost + fee deduction from position sizing
                            FPN<FP> position_cost = FPN_Mul(ctrl.portfolio.positions[slot].quantity,
                                                            ctrl.portfolio.positions[slot].entry_price);
                            FPN<FP> fee = FPN_Mul(position_cost, ctrl.config.fee_rate);
                            ctrl.balance = FPN_AddSat(ctrl.balance, FPN_AddSat(position_cost, fee));
                            // reverse the entry fee that was counted at buy time — the buy didn't happen
                            ctrl.total_fees = FPN_SubSat(ctrl.total_fees, fee);
                            ctrl.total_buys--;
                        }
                        check &= check - 1;
                    }
                }
            }

            // LIVE: sell exited positions (bitmap-gated)
            if (saved_exit_count > 0 && ccfg.use_real_money) {
                // check if any exited positions were live
                int has_live_exit = 0;
                for (int i = 0; i < saved_exit_count; i++) {
                    if (live_position_bitmap & (1 << saved_exit_slots[i])) {
                        has_live_exit = 1;
                        break;
                    }
                }
                if (has_live_exit && ctrl.config.max_positions == 1) {
                    // single-slot mode: sell entire BTC balance — the exchange balance
                    // IS the position, selling it all eliminates dust from qty rounding
                    double usdt_tmp = 0, btc_bal = 0;
                    BinanceOrderAPI_GetBalances(&order_api, &usdt_tmp, &btc_bal);
                    double qty_d = binance_round_qty(btc_bal, order_api.filters.lot_step_size);
                    double notional = qty_d * last_stream.price_d;
                    if (qty_d >= order_api.filters.lot_min_qty && notional >= order_api.filters.min_notional) {
                        char oid[32];
                        double fp = 0, fq = 0;
                        fprintf(stderr, "[LIVE] SELL entire BTC balance: %.8f BTC ($%.2f)\n", qty_d, notional);
                        if (BinanceOrderAPI_MarketSell(&order_api, qty_d, oid, &fp, &fq))
                            live_position_bitmap = 0;
                        else {
                            fprintf(stderr, "[LIVE] SELL failed — clearing bitmap, check Binance dashboard\n");
                            live_position_bitmap = 0;
                        }
                    } else {
                        fprintf(stderr, "[LIVE] SELL skipped — BTC balance %.8f ($%.2f) below minimum\n", qty_d, notional);
                        live_position_bitmap = 0;
                    }
                } else if (has_live_exit) {
                    // multi-slot mode: sell per-position quantities
                    for (int i = 0; i < saved_exit_count; i++) {
                        int slot = saved_exit_slots[i];
                        if (!(live_position_bitmap & (1 << slot))) continue;
                        double qty_d = saved_exit_qtys[i];
                        double notional = qty_d * last_stream.price_d;
                        if (qty_d >= order_api.filters.lot_min_qty && notional >= order_api.filters.min_notional) {
                            char oid[32];
                            double fp = 0, fq = 0;
                            if (BinanceOrderAPI_MarketSell(&order_api, qty_d, oid, &fp, &fq))
                                live_position_bitmap &= ~(1 << slot);
                            else {
                                fprintf(stderr, "[LIVE] SELL failed slot %d — clearing bitmap, check Binance dashboard\n", slot);
                                live_position_bitmap &= ~(1 << slot);
                            }
                        } else {
                            fprintf(stderr, "[LIVE] SELL skipped slot %d — notional $%.2f below minimum, clearing bitmap\n", slot, notional);
                            live_position_bitmap &= ~(1 << slot);
                        }
                    }
                } else {
                    // paper-only exits — just clear bits
                    for (int i = 0; i < saved_exit_count; i++)
                        live_position_bitmap &= ~(1 << saved_exit_slots[i]);
                }
            }

#ifdef LATENCY_PROFILING
            uint64_t cycles = t3 - t0;
            // tick_count == 0 means slow path just ran, > 0 means hot path only
            if (ctrl.tick_count == 0) {
                if (cycles < tui.slow_min) tui.slow_min = cycles;
                if (cycles > tui.slow_max) tui.slow_max = cycles;
                tui.slow_sum += cycles;
                tui.slow_count++;
            } else {
                if (cycles < tui.hot_min) tui.hot_min = cycles;
                if (cycles > tui.hot_max) tui.hot_max = cycles;
                tui.hot_sum += cycles;
                tui.hot_count++;
                // log2 histogram bucket for percentile tracking
                int bucket = (cycles > 0) ? (63 - __builtin_clzll(cycles)) : 0;
                if (bucket > 20) bucket = 20;
                tui.hot_hist[bucket]++;
#ifndef LATENCY_LITE
                // per-component breakdown
                uint64_t bg = t1 - t0, eg = t2 - t1, pc = t3 - t2;
                tui.bg_sum += bg; if (bg > tui.bg_max) tui.bg_max = bg;
                tui.eg_sum += eg; if (eg > tui.eg_max) tui.eg_max = eg;
                tui.pc_sum += pc; if (pc > tui.pc_max) tui.pc_max = pc;
                // fill vs no-fill PCTick + per-position ExitGate cost
                tui.eg_pos_sum += __builtin_popcount(ctrl.portfolio.active_bitmap);
                if (ctrl.total_buys > buys_before_metrics) {
                    tui.pc_fill_sum += pc; if (pc > tui.pc_fill_max) tui.pc_fill_max = pc;
                    tui.pc_fill_count++;
                } else {
                    tui.pc_nofill_sum += pc; if (pc > tui.pc_nofill_max) tui.pc_nofill_max = pc;
                    tui.pc_nofill_count++;
                }
#endif
            }
#endif

            // save portfolio snapshot every slow-path cycle (crash recovery)
            if (ctrl.tick_count == 0) {
                // LIVE: balance sync + external trade detection + orphan detection
                if (ccfg.use_real_money) {
                    // query USDT + BTC balances in one API call (~150ms, not two)
                    double usdt_bal = 0, btc_bal = 0;
                    int bal_ok = BinanceOrderAPI_GetBalances(&order_api, &usdt_bal, &btc_bal);
                    double btc_value = btc_bal * last_stream.price_d;

                    // only sync if balance query succeeded — failed query returns 0/0
                    // which would incorrectly zero out balance and trigger false orphan detection
                    if (bal_ok) {
                        // external trade detection: if BTC balance is 0 but we think
                        // positions are live, they were closed on the Binance dashboard
                        if (btc_bal < 0.000001 && live_position_bitmap != 0) {
                            fprintf(stderr, "[LIVE] BTC balance is 0 — positions closed externally, clearing bitmap\n");
                            live_position_bitmap = 0;
                        }

                        // sync paper balance when no live positions
                        // (USDT balance is complete picture when not holding BTC)
                        if (live_position_bitmap == 0) {
                            ctrl.balance = FPN_FromDouble<FP>(usdt_bal);
                        }

                        // orphan detection: real positions without paper backing
                        uint16_t orphans = live_position_bitmap & ~ctrl.portfolio.active_bitmap;
                        if (orphans) {
                            fprintf(stderr, "[LIVE] WARNING: %d orphaned real positions — selling\n",
                                    __builtin_popcount(orphans));
                            while (orphans) {
                                int idx = __builtin_ctz(orphans);
                                double qty_d = FPN_ToDouble(ctrl.portfolio.positions[idx].quantity);
                                double notional = qty_d * last_stream.price_d;
                                if (qty_d >= order_api.filters.lot_min_qty
                                    && notional >= order_api.filters.min_notional * 2.0) {
                                    char oid[32]; double fp = 0, fq = 0;
                                    if (BinanceOrderAPI_MarketSell(&order_api, qty_d, oid, &fp, &fq) && fq > 0) {
                                        // book the exit in paper ledger
                                        FPN<FP> proceeds = FPN_FromDouble<FP>(fp * fq);
                                        FPN<FP> exit_fee = FPN_Mul(proceeds, ctrl.config.fee_rate);
                                        ctrl.balance = FPN_AddSat(ctrl.balance, FPN_SubSat(proceeds, exit_fee));
                                        ctrl.total_fees = FPN_AddSat(ctrl.total_fees, exit_fee);
                                        ctrl.losses++;
                                        fprintf(stderr, "[LIVE] orphan sold: %.8f @ $%.2f, fee $%.4f\n",
                                                fq, fp, FPN_ToDouble(exit_fee));
                                    }
                                } else {
                                    fprintf(stderr, "[LIVE] orphan slot %d too small to sell (notional $%.2f)\n", idx, notional);
                                }
                                live_position_bitmap &= ~(1 << idx);
                                orphans &= orphans - 1;
                            }
                        }

                        // reverse orphan: paper position exists but BTC was sold externally
                        // (e.g. user sold on Binance app) — undo the paper position
                        if (ctrl.portfolio.active_bitmap && btc_bal < 0.000001) {
                            uint16_t ghost = ctrl.portfolio.active_bitmap;
                            fprintf(stderr, "[LIVE] BTC balance is zero but %d paper position(s) — undoing (external sell?)\n",
                                    __builtin_popcount(ghost));
                            while (ghost) {
                                int idx = __builtin_ctz(ghost);
                                FPN<FP> cost = FPN_Mul(ctrl.portfolio.positions[idx].quantity,
                                                       ctrl.portfolio.positions[idx].entry_price);
                                FPN<FP> fee = FPN_Mul(cost, ctrl.config.fee_rate);
                                ctrl.balance = FPN_AddSat(ctrl.balance, FPN_AddSat(cost, fee));
                                ctrl.portfolio.active_bitmap &= ~(1 << idx);
                                ghost &= ghost - 1;
                            }
                            live_position_bitmap = 0;
                            ctrl.balance = FPN_FromDouble<FP>(usdt_bal);
                        }
                    }

                    // periodic clock re-sync (~every 30 min at default poll_interval)
                    static int clock_sync_counter = 0;
                    if (++clock_sync_counter >= 100) {
                        BinanceOrderAPI_SyncClock(&order_api);
                        clock_sync_counter = 0;
                    }

                    // lightweight status line — shows real equity (USDT + BTC)
                    fprintf(stderr, "[LIVE] $%.2f %s pos:%d/%d live:%d bal:$%.2f btc:%.8f equity:$%.2f W:%d L:%d\n",
                            last_stream.price_d,
                            ctrl.regime.current_regime == 1 ? "TREND" :
                            ctrl.regime.current_regime == 2 ? "VOLAT" :
                            ctrl.regime.current_regime == 3 ? "TR_DN" : "RANGE",
                            __builtin_popcount(ctrl.portfolio.active_bitmap), 16,
                            __builtin_popcount(live_position_bitmap),
                            usdt_bal, btc_bal, usdt_bal + btc_value,
                            ctrl.wins, ctrl.losses);
                }

                PortfolioController_SaveSnapshot(&ctrl, snapshot_path);
                MetricsLog_SlowPath(&metrics, &ctrl, last_stream.price_d);
#ifdef MULTICORE_TUI
                // full TUI snapshot — piggybacking on slow-path cache state
                // L1 already warm from rolling stats, regime, balance, strategy dispatch
                // so this copy reads from L1 hits, not L2 misses (zero additional pollution)
                {
                    int back = !__atomic_load_n(&shared.active_idx, __ATOMIC_ACQUIRE);
                    int front = !back;
                    // carry graph history from front buffer before overwriting
                    TUISnapshot *bs = &shared.snapshots[back];
                    const TUISnapshot *fs = &shared.snapshots[front];
                    memcpy(bs->price_history, fs->price_history, sizeof(bs->price_history));
                    memcpy(bs->volume_history, fs->volume_history, sizeof(bs->volume_history));
                    memcpy(bs->pnl_history, fs->pnl_history, sizeof(bs->pnl_history));
                    bs->graph_head = fs->graph_head;
                    bs->graph_count = fs->graph_count;
                    TUI_CopySnapshot(bs, &ctrl, &last_stream);
                    // append current data point to graph ring buffers
                    bs->price_history[bs->graph_head] = bs->price;
                    bs->volume_history[bs->graph_head] = bs->volume;
                    bs->pnl_history[bs->graph_head] = bs->total_pnl;
                    bs->graph_head = (bs->graph_head + 1) % TUISnapshot::GRAPH_LEN;
                    if (bs->graph_count < TUISnapshot::GRAPH_LEN) bs->graph_count++;
#ifdef LATENCY_PROFILING
                    if (tui.hot_count > 0) {
                        bs->hot_avg_ns = (double)tui.hot_sum / tui.hot_count / tui.tsc_per_ns;
                        bs->hot_min_ns = (double)tui.hot_min / tui.tsc_per_ns;
                        bs->hot_max_ns = (double)tui.hot_max / tui.tsc_per_ns;
                        bs->hot_count  = tui.hot_count;
                        // percentiles from log2 histogram
                        {
                            uint64_t p50t = tui.hot_count / 2, p95t = tui.hot_count * 95 / 100;
                            uint64_t cum = 0;
                            bs->hot_p50_ns = 0;
                            bs->hot_p95_ns = 0;
                            for (int i = 0; i <= 20; i++) {
                                cum += tui.hot_hist[i];
                                if (!bs->hot_p50_ns && cum >= p50t)
                                    bs->hot_p50_ns = (1.5 * (1ULL << i)) / tui.tsc_per_ns;
                                if (!bs->hot_p95_ns && cum >= p95t)
                                    bs->hot_p95_ns = (1.5 * (1ULL << i)) / tui.tsc_per_ns;
                            }
                        }
                        // per-component breakdown
                        bs->bg_avg_ns = (double)tui.bg_sum / tui.hot_count / tui.tsc_per_ns;
                        bs->bg_max_ns = (double)tui.bg_max / tui.tsc_per_ns;
                        bs->eg_avg_ns = (double)tui.eg_sum / tui.hot_count / tui.tsc_per_ns;
                        bs->eg_max_ns = (double)tui.eg_max / tui.tsc_per_ns;
                        bs->eg_per_pos_ns = (tui.eg_pos_sum > 0)
                            ? (double)tui.eg_sum / tui.eg_pos_sum / tui.tsc_per_ns : 0;
                        bs->pc_avg_ns = (double)tui.pc_sum / tui.hot_count / tui.tsc_per_ns;
                        bs->pc_max_ns = (double)tui.pc_max / tui.tsc_per_ns;
                        if (tui.pc_fill_count > 0) {
                            bs->pc_fill_avg_ns = (double)tui.pc_fill_sum / tui.pc_fill_count / tui.tsc_per_ns;
                            bs->pc_fill_max_ns = (double)tui.pc_fill_max / tui.tsc_per_ns;
                            bs->pc_fill_count = tui.pc_fill_count;
                        }
                        if (tui.pc_nofill_count > 0) {
                            bs->pc_nofill_avg_ns = (double)tui.pc_nofill_sum / tui.pc_nofill_count / tui.tsc_per_ns;
                            bs->pc_nofill_max_ns = (double)tui.pc_nofill_max / tui.tsc_per_ns;
                            bs->pc_nofill_count = tui.pc_nofill_count;
                        }
                    }
                    if (tui.slow_count > 0) {
                        bs->slow_avg_ns = (double)tui.slow_sum / tui.slow_count / tui.tsc_per_ns;
                        bs->slow_min_ns = (double)tui.slow_min / tui.tsc_per_ns;
                        bs->slow_max_ns = (double)tui.slow_max / tui.tsc_per_ns;
                        bs->slow_count  = tui.slow_count;
                    }
#endif
                    __atomic_store_n(&shared.active_idx, back, __ATOMIC_RELEASE);
                }
#endif
            }
        }

        //==============================================================================================
        // TUI RENDER (single-threaded mode only)
        //==============================================================================================
#ifndef MULTICORE_TUI
#ifdef LATENCY_BENCH
        // bench mode: TUI disabled, dump stats to stderr periodically
        if (ctrl.total_ticks > 0 && (ctrl.total_ticks % 10000) == 0) {
            double hot_avg = (tui.hot_count > 0) ? (double)tui.hot_sum / tui.hot_count / tui.tsc_per_ns : 0;
            double hot_min_ns = (tui.hot_count > 0) ? (double)tui.hot_min / tui.tsc_per_ns : 0;
            double hot_max_ns = (tui.hot_count > 0) ? (double)tui.hot_max / tui.tsc_per_ns : 0;
            double slow_avg = (tui.slow_count > 0) ? (double)tui.slow_sum / tui.slow_count / tui.tsc_per_ns : 0;
            fprintf(stderr, "[BENCH] tick %lu | hot: avg %.0fns min %.0fns max %.0fns (%lu) | slow: avg %.0fns (%lu)\n",
                    (unsigned long)ctrl.total_ticks, hot_avg, hot_min_ns, hot_max_ns,
                    (unsigned long)tui.hot_count, slow_avg, (unsigned long)tui.slow_count);
        }
#else
        TUI_Render(&tui, &ctrl, &last_stream, ctrl.total_ticks);
#endif
#endif // !MULTICORE_TUI
    }

    //==================================================================================================
    // cleanup live trading
    //==================================================================================================
    if (ccfg.use_real_money)
        BinanceOrderAPI_Cleanup(&order_api);

    //==================================================================================================
    // session summary
    //==================================================================================================
    fprintf(stderr, "\n==================================================\n");
    fprintf(stderr, "  SESSION SUMMARY\n");
    fprintf(stderr, "==================================================\n");
    fprintf(stderr, "  Total ticks:      %lu\n", (unsigned long)ctrl.total_ticks);
    fprintf(stderr, "  Trade log:        %lu entries\n", (unsigned long)log.trade_count);
    fprintf(stderr, "  Open positions:   %d\n", Portfolio_CountActive(&ctrl.portfolio));
    fprintf(stderr, "  Unrealized P&L:   $%.4f\n", FPN_ToDouble(ctrl.portfolio_delta));
#ifdef LATENCY_PROFILING
    fprintf(stderr, "--------------------------------------------------\n");
    if (tui.hot_count > 0) {
        double hot_avg = (double)tui.hot_sum / tui.hot_count / tui.tsc_per_ns;
        double hot_min_ns = (double)tui.hot_min / tui.tsc_per_ns;
        double hot_max_ns = (double)tui.hot_max / tui.tsc_per_ns;
        double hot_p50 = 0, hot_p95 = 0;
        { uint64_t p50t = tui.hot_count/2, p95t = tui.hot_count*95/100, cum = 0;
          for (int i = 0; i <= 20; i++) { cum += tui.hot_hist[i];
            if (!hot_p50 && cum >= p50t) hot_p50 = (1.5*(1ULL<<i))/tui.tsc_per_ns;
            if (!hot_p95 && cum >= p95t) hot_p95 = (1.5*(1ULL<<i))/tui.tsc_per_ns; } }
        fprintf(stderr, "  Hot path:         avg %.0fns  min %.0fns  max %.0fns  p50 %.0fns  p95 %.0fns  (%lu ticks)\n",
                hot_avg, hot_min_ns, hot_max_ns, hot_p50, hot_p95, (unsigned long)tui.hot_count);
        double bg_avg = (double)tui.bg_sum / tui.hot_count / tui.tsc_per_ns;
        double bg_max_ns = (double)tui.bg_max / tui.tsc_per_ns;
        double eg_avg = (double)tui.eg_sum / tui.hot_count / tui.tsc_per_ns;
        double eg_max_ns = (double)tui.eg_max / tui.tsc_per_ns;
        double pc_avg = (double)tui.pc_sum / tui.hot_count / tui.tsc_per_ns;
        double pc_max_ns = (double)tui.pc_max / tui.tsc_per_ns;
        fprintf(stderr, "    buygate:        avg %.0fns  max %.0fns\n", bg_avg, bg_max_ns);
        double eg_per_pos = (tui.eg_pos_sum > 0)
            ? (double)tui.eg_sum / tui.eg_pos_sum / tui.tsc_per_ns : 0;
        fprintf(stderr, "    exitgate:       avg %.0fns  max %.0fns  (%.0fns/pos)\n", eg_avg, eg_max_ns, eg_per_pos);
        fprintf(stderr, "    pctick:         avg %.0fns  max %.0fns\n", pc_avg, pc_max_ns);
        if (tui.pc_nofill_count > 0) {
            double nf_avg = (double)tui.pc_nofill_sum / tui.pc_nofill_count / tui.tsc_per_ns;
            double nf_max = (double)tui.pc_nofill_max / tui.tsc_per_ns;
            fprintf(stderr, "      no-fill:      avg %.0fns  max %.0fns  (%lu)\n", nf_avg, nf_max, (unsigned long)tui.pc_nofill_count);
        }
        if (tui.pc_fill_count > 0) {
            double f_avg = (double)tui.pc_fill_sum / tui.pc_fill_count / tui.tsc_per_ns;
            double f_max = (double)tui.pc_fill_max / tui.tsc_per_ns;
            fprintf(stderr, "      fill:         avg %.0fns  max %.0fns  (%lu)\n", f_avg, f_max, (unsigned long)tui.pc_fill_count);
        }
    }
    if (tui.slow_count > 0) {
        double slow_avg = (double)tui.slow_sum / tui.slow_count / tui.tsc_per_ns;
        double slow_min_ns = (double)tui.slow_min / tui.tsc_per_ns;
        double slow_max_ns = (double)tui.slow_max / tui.tsc_per_ns;
        fprintf(stderr, "  Slow path:        avg %.0fns  min %.0fns  max %.0fns  (%lu cycles)\n",
                slow_avg, slow_min_ns, slow_max_ns, (unsigned long)tui.slow_count);
    }
#endif
    fprintf(stderr, "==================================================\n");

    // save snapshot WITH positions intact - they'll be resumed on next startup
    PortfolioController_SaveSnapshot(&ctrl, snapshot_path);
    fprintf(stderr, "  Snapshot saved (%d positions persisted)\n",
            Portfolio_CountActive(&ctrl.portfolio));

    //==================================================================================================
    // cleanup
    //==================================================================================================
#ifdef MULTICORE_TUI
    // signal TUI thread to exit, wait for clean terminal restore
    __atomic_store_n(&shared.quit_requested, 1, __ATOMIC_RELEASE);
    pthread_join(tui_tid, NULL);
#else
    TUI_Cleanup(&tui);
#endif
    TradeLog_Close(&log);
    MetricsLog_Close(&metrics);
    BinanceStream_Close(&bs);
    free(pool.slots);
    free(ctrl.rolling_long);

    return 0;
}
