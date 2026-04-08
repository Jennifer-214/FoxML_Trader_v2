// run_backtest_dispatch.cpp — phase 13 production dispatch verification
//
// Drives Backtest_Run end-to-end on a real tick file in BOTH single_core and
// sharded modes. Reports final P&L, trade count, ticks processed, and
// elapsed wall time for each mode side by side.
//
// Usage:
//   ./build/run_backtest_dispatch <data_file.csv>
//
// Both modes share the same config except for engine_mode + num_execution_cores.
// All other knobs are read from a hardcoded default config inside.
//
// Goal: prove that
//   1. The dispatch in Backtest_Run actually routes based on engine_mode
//   2. Single_core mode is byte-identical to the legacy behavior (same as
//      running ./build/foxml_suite without phase 13 changes)
//   3. Sharded mode runs to completion without crashing
//   4. Both modes process the same number of ticks
//   5. Their P&L numbers can be compared (within the 0.1% tolerance the plan
//      sets, though strategy-port differences may cause larger gaps until the
//      MR / Momentum stubs are filled in)

#include "Backtest/BacktestEngine.hpp"
#include "Backtest/BacktestSharded.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace tt;

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <data_file.csv>\n", argv[0]);
        return 2;
    }

    const char* data_path = argv[1];

    // Build a baseline config: SimpleDip strategy (the only one ported in
    // sharded mode), reasonable defaults for everything else.
    ControllerConfig<BACKTEST_FP> base_cfg = ControllerConfig_Default<BACKTEST_FP>();
    base_cfg.default_strategy = STRATEGY_SIMPLE_DIP;
    base_cfg.starting_balance = FPN_FromDouble<BACKTEST_FP>(10000.0);
    base_cfg.fee_rate = FPN_FromDouble<BACKTEST_FP>(0.001);
    base_cfg.risk_pct = FPN_FromDouble<BACKTEST_FP>(0.10);
    base_cfg.entry_offset_pct = FPN_FromDouble<BACKTEST_FP>(0.001);
    base_cfg.take_profit_pct = FPN_FromDouble<BACKTEST_FP>(0.005);
    base_cfg.stop_loss_pct = FPN_FromDouble<BACKTEST_FP>(0.0025);
    base_cfg.volume_multiplier = FPN_FromDouble<BACKTEST_FP>(0.5);
    base_cfg.poll_interval = 64;
    base_cfg.warmup_ticks = 128;

    BacktestRunConfig run = {};
    strncpy(run.data_paths[0], data_path, sizeof(run.data_paths[0]) - 1);
    run.num_data_files = 1;
    strncpy(run.config_path, "", sizeof(run.config_path) - 1);
    run.use_config_override = 1;
    run.config_override = base_cfg;
    run.collect_features = 0;
    run.label_type = 0;
    run.label_tp_pct = 0;
    run.label_sl_pct = 0;
    run.label_forward_ticks = 0;

    static BacktestResults legacy_results;
    static BacktestResults sharded_results;
    BacktestResults_Init(&legacy_results);
    BacktestResults_Init(&sharded_results);

    volatile int progress = 0;
    volatile int cancel = 0;

    //----- LEGACY (single_core) -----
    printf("\n=== LEGACY single_core mode ===\n");
    run.config_override.engine_mode = ENGINE_MODE_SINGLE_CORE;
    Backtest_Run(&legacy_results, &run, &progress, &cancel, nullptr, nullptr);

    //----- SHARDED -----
    printf("\n=== SHARDED mode ===\n");
    progress = 0;
    run.config_override.engine_mode = ENGINE_MODE_SHARDED;
    run.config_override.num_execution_cores = 4;
    Backtest_Run(&sharded_results, &run, &progress, &cancel, nullptr, nullptr);

    //----- COMPARE -----
    printf("\n========================================\n");
    printf("HEAD TO HEAD COMPARISON\n");
    printf("========================================\n");
    printf("                       LEGACY            SHARDED\n");
    printf("ticks processed   %12lu     %12lu\n",
           (unsigned long)legacy_results.stats.ticks_processed,
           (unsigned long)sharded_results.stats.ticks_processed);
    printf("total trades      %12u     %12u\n",
           legacy_results.stats.total_trades,
           sharded_results.stats.total_trades);
    printf("wins / losses     %6u/%-5u      %6u/%-5u\n",
           legacy_results.stats.wins, legacy_results.stats.losses,
           sharded_results.stats.wins, sharded_results.stats.losses);
    printf("win rate          %12.2f%%    %12.2f%%\n",
           legacy_results.stats.win_rate * 100.0,
           sharded_results.stats.win_rate * 100.0);
    printf("total P&L         %12.4f     %12.4f\n",
           legacy_results.stats.total_pnl,
           sharded_results.stats.total_pnl);
    printf("return %%          %12.4f     %12.4f\n",
           legacy_results.stats.return_pct,
           sharded_results.stats.return_pct);
    printf("max drawdown      %12.4f     %12.4f\n",
           legacy_results.stats.max_drawdown,
           sharded_results.stats.max_drawdown);
    printf("max drawdown %%    %12.4f     %12.4f\n",
           legacy_results.stats.max_drawdown_pct,
           sharded_results.stats.max_drawdown_pct);
    printf("elapsed (ms)      %12.1f     %12.1f\n",
           legacy_results.stats.elapsed_ms,
           sharded_results.stats.elapsed_ms);

    double pnl_diff = sharded_results.stats.total_pnl - legacy_results.stats.total_pnl;
    double pnl_diff_pct = legacy_results.stats.total_pnl != 0.0
        ? (pnl_diff / legacy_results.stats.total_pnl) * 100.0
        : 0.0;
    int trades_diff = (int)sharded_results.stats.total_trades - (int)legacy_results.stats.total_trades;

    printf("\nP&L diff       : %+.4f (%.2f%%)\n", pnl_diff, pnl_diff_pct);
    printf("Trades diff    : %+d\n", trades_diff);

    BacktestResults_Free(&legacy_results);
    BacktestResults_Free(&sharded_results);

    return 0;
}
