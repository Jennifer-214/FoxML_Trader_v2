// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [CONTROLLER CONFIG]
//======================================================================================================
// configuration for the portfolio controller - all tunable parameters in one
// place parsed from a simple key=value text file, no JSON, no external libs
//======================================================================================================
#ifndef CONTROLLER_CONFIG_HPP
#define CONTROLLER_CONFIG_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/LinearRegression3X.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//======================================================================================================
// [ENGINE MODE]
//======================================================================================================
// Phase 13 of the per-core sharding migration. Selects which hot-path
// architecture the engine runs:
//   ENGINE_MODE_SINGLE_CORE (default): legacy single-threaded engine. all
//     existing behavior unchanged. PortfolioController_Tick walks the
//     portfolio bitmap on every tick.
//   ENGINE_MODE_SHARDED: experimental per-core risk-sharded engine. one
//     position per pinned cpu core, controller core drains events on its
//     own thread. branchless ~60ns hot path. requires num_execution_cores
//     to be set; defaults to 4.
//
// engine_mode is STARTUP-ONLY — changes via hot reload are ignored. switching
// modes requires a restart so the thread layout can be torn down and rebuilt.
//======================================================================================================
constexpr uint8_t ENGINE_MODE_SINGLE_CORE = 0;
constexpr uint8_t ENGINE_MODE_SHARDED = 1;

//======================================================================================================
// [CONFIG]
//======================================================================================================
template <unsigned F> struct ControllerConfig {
  uint32_t poll_interval;  // ticks between slow-path runs
  uint32_t warmup_ticks;   // ticks to observe before trading
  FPN<F> r2_threshold;     // min R^2 to trust regression
  FPN<F> slope_scale_buy;  // how much slope shifts buy price threshold
  FPN<F> max_shift;        // max drift from initial buy conditions
  FPN<F> take_profit_pct;  // per-position take profit (e.g. 0.03 = 3%)
  FPN<F> stop_loss_pct;    // per-position stop loss (e.g. 0.015 = 1.5%)
  FPN<F> starting_balance; // paper trading starting balance (e.g. 10000.0)
  FPN<F> fee_rate;         // per-trade fee rate (e.g. 0.001 = 0.1% for Binance)
  FPN<F> risk_pct; // fraction of balance to risk per position (e.g. 0.02 = 2%)
  // market microstructure filters (initial values - adapted at runtime by P&L
  // regression)
  FPN<F> volume_multiplier; // buy only when tick volume >= this * rolling_avg
                            // (e.g. 3.0)
  FPN<F> entry_offset_pct;  // buy gate offset below rolling mean (e.g. 0.0015 =
                            // 0.15%)
  FPN<F> spacing_multiplier; // min entry spacing = stddev * this (e.g. 2.0)
  // adaptation clamps - how far the filters can drift from their initial values
  FPN<F>
      offset_min; // min entry_offset_pct (most aggressive, e.g. 0.0005 = 0.05%)
  FPN<F> offset_max; // max entry_offset_pct (most defensive, e.g. 0.005 = 0.5%)
  FPN<F> vol_mult_min; // min volume_multiplier (most aggressive, e.g. 1.5)
  FPN<F> vol_mult_max; // max volume_multiplier (most defensive, e.g. 6.0)
  FPN<F> filter_scale; // how much P&L slope shifts the filters (e.g. 0.50)
  // risk management
  FPN<F> max_drawdown_pct; // halt trading if total P&L drops below this % of
                           // starting balance (e.g. 0.10 = 10%)
  FPN<F> max_exposure_pct; // max fraction of balance deployed in positions
                           // (e.g. 0.50 = 50%)
  uint32_t max_positions;  // max simultaneous open positions (1-16, default 1)
  // enhanced buy signal (disabled by default = backward compatible)
  FPN<F> offset_stddev_mult; // stddev-scaled offset multiplier (0 = use
                             // percentage mode)
  FPN<F> offset_stddev_min; // adaptation lower bound for stddev mode (e.g. 0.5)
  FPN<F> offset_stddev_max; // adaptation upper bound for stddev mode (e.g. 4.0)
  FPN<F> min_long_slope;    // min long-window price slope to allow buys (0 =
                            // disabled)
  FPN<F> min_buy_delta;     // min volume delta for MR buys (-0.3 = allow mild
                            // selling, block heavy)
  FPN<F> vwap_offset; // buy below VWAP - (VWAP * this) (0 = disabled, 0.001 =
                      // 0.1% below)
  FPN<F> min_stddev_pct;  // skip trades when stddev/price < this (0 = disabled,
                          // 0.0003 = 0.03%)
  FPN<F> momentum_r2_min; // min R² to enter momentum trades (0 = disabled, 0.4
                          // recommended)
  // trailing take-profit (disabled by default)
  FPN<F> tp_hold_score;  // min SNR*R² to hold past TP (0 = disabled, fixed TP)
  FPN<F> tp_trail_mult;  // trailing distance: stddev * this (e.g. 1.0)
  FPN<F> sl_trail_mult;  // trailing SL distance: stddev * this (e.g. 2.0)
  FPN<F> fee_floor_mult; // TP floor = entry × fee_rate × this (default 3.0,
                         // try 5.0 for wider)
  // risk ratios
  FPN<F>
      min_sl_tp_ratio; // min SL/TP distance ratio (0.5 = 2:1 reward/risk floor)
  FPN<F> ror_tp_bonus; // TP multiplier when ROR positive (1.2 = 20% wider)
  FPN<F> momentum_tp_r2_min; // TP scale at R²=0 (0.5 = half base TP,
                             // conservative on uncertainty)
  FPN<F>
      momentum_sl_r2_max; // SL scale at R²=0 (1.5 = wider SL in choppy markets)
  // adaptation speed
  FPN<F> squeeze_decay;      // idle squeeze rate per cycle (0.10 = 10% of gap)
  FPN<F> offset_adapt_scale; // P&L regression → offset shift (0.001)
  FPN<F> stddev_adapt_scale; // P&L regression → stddev/breakout shift (0.1)
  FPN<F> vol_adapt_scale;    // P&L regression → volume shift (0.1)
  FPN<F> breakout_min;       // momentum breakout floor in stddevs (0.5)
  uint32_t slow_path_max_secs; // wall-time floor between slow paths (3 seconds)
  // time-based exit (disabled by default)
  uint32_t
      max_hold_ticks; // close position if held longer than this (0 = disabled)
  FPN<F>
      min_hold_gain_pct; // only time-exit if gain < this % (e.g. 0.001 = 0.1%)
  // regime detection
  FPN<F> regime_slope_threshold;     // relative slope magnitude for TRENDING
                                     // (legacy, kept for compat)
  FPN<F> regime_crossover_threshold; // EMA/SMA spread for mild trend (e.g.
                                     // 0.0005 = EMA Cross)
  FPN<F> regime_strong_crossover;    // EMA/SMA spread for strong trend (e.g.
                                     // 0.0015 = Momentum)
  FPN<F> regime_r2_threshold;        // min R² for TRENDING (e.g. 0.70)
  FPN<F> regime_volatile_stddev;     // stddev/price ratio for VOLATILE (legacy,
                                     // kept for compat)
  FPN<F> regime_vol_spike_ratio;     // variance ratio threshold: short/long
                                     // variance > this = volatile spike
  uint32_t regime_hysteresis;  // slow-path cycles before regime switch (e.g. 5)
  uint32_t min_warmup_samples; // min rolling stats samples before trading (0 =
                               // use warmup_ticks only). CAPS AT W=128: this
                               // gates on rolling.count which is bounded by
                               // the rolling window size. Values > 128 are
                               // CLAMPED at config load with a warning. If you
                               // want a longer total-tick warmup, use
                               // warmup_ticks instead — it counts raw ticks
                               // and has no upper bound.
  // post-SL cooldown
  uint32_t sl_cooldown_cycles; // slow-path cycles to pause buying after SL (0 =
                               // disabled)
  int sl_cooldown_adaptive;  // 0 = fixed cycles, 1 = scale by trend confidence
                             // at SL time
  uint32_t sl_cooldown_base; // minimum cooldown cycles (even on spikes)
  uint32_t
      sl_cooldown_extra; // max additional cycles (scaled by trend confidence)
  // gate death spiral recovery
  uint32_t idle_reset_cycles; // slow-path cycles with no fill before gate decay
                              // (0 = disabled)
  // momentum strategy
  FPN<F>
      momentum_breakout_mult; // buy when price > avg + stddev * this (e.g. 1.5)
  FPN<F> momentum_tp_mult;    // TP multiplier for momentum (e.g. 3.0 stddevs)
  FPN<F> momentum_sl_mult;    // SL multiplier for momentum (e.g. 1.0 stddevs)
  // EMA cross strategy
  FPN<F> emacross_dip_mult;      // buy this many stddevs below EMA (e.g. 0.5)
  FPN<F> emacross_crossover_min; // min EMA-SMA spread for uptrend confirmation
  FPN<F> emacross_trail_mult;    // trailing TP factor when EMA rising
  // volume spike detection
  FPN<F> spike_threshold;         // volume spike ratio (current/max) to trigger
                                  // (e.g. 5.0 = 5x)
  FPN<F> spike_spacing_reduction; // spacing multiplier during spike (e.g. 0.5 =
                                  // half normal)
  // partial exits (scaling out)
  int partial_exit_enabled; // 0 = full exits only, 1 = split into two legs at
                            // fill time
  FPN<F>
      partial_exit_pct; // fraction to exit at TP1 (0.5 = 50%, rest rides TP2)
  FPN<F> tp2_mult; // TP2 = TP1_distance * this (2.0 = double the TP distance)
  int breakeven_on_partial; // 1 = move remaining SL to entry after TP1 hit
  int breakeven_on_profit;  // 1 = ratchet SL to breakeven when position crosses
                            // net profit
  FPN<F> breakeven_buffer_pct; // SL offset from entry once breakeven ratchet
                               // fires (0.001 = +0.1% above entry, -0.001 =
                               // allow 0.1% loss)
  // slippage simulation
  FPN<F> slippage_pct; // simulated slippage on entry/exit (e.g. 0.0005 = 0.05%)
  // session awareness
  int session_filter_enabled; // 0 = disabled, 1 = apply per-session gate
                              // multipliers
  FPN<F> session_asian_mult; // gate multiplier during Asian session (00-07 UTC)
  FPN<F> session_european_mult; // gate multiplier during European session
                                // (07-13 UTC)
  FPN<F> session_us_mult;       // gate multiplier during US session (13-20 UTC)
  FPN<F> session_overnight_mult; // gate multiplier during overnight (20-00 UTC)
  // order book (L2 depth)
  int depth_enabled; // 0 = trade stream only, 1 = also subscribe to depth
  FPN<F> min_book_imbalance; // require bid bias to buy (0 = disabled, 0.10 =
                             // 10% bid excess)
  // EMA gate (proactive entry — reacts in 1-2s instead of 5s)
  int gate_ema_enabled;  // 0=use rolling avg (legacy), 1=use EMA for gate price
  FPN<F> gate_ema_alpha; // EMA smoothing factor (0.997 = ~333 tick window)
  FPN<F> gate_ema_one_minus_alpha; // precomputed 1.0 - alpha (avoid subtraction
                                   // on hot path)
  // strategy selection
  int default_strategy; // -1=regime auto, 0=MR, 1=Momentum, 2=SimpleDip
  // live trading
  int use_real_money; // 0=paper (default), 1=real orders via REST API
  // kill switch (sticky — stays active until session reset or manual TUI 'k')
  int kill_switch_enabled; // 0=disabled, 1=enabled
  FPN<F>
      kill_switch_daily_loss_pct; // max daily loss before kill (e.g. 0.03 = 3%)
  FPN<F> kill_switch_drawdown_pct; // max drawdown from session peak before kill
                                   // (e.g. 0.05 = 5%)
  uint32_t kill_recovery_warmup; // slow-path cycles to observe after kill reset
                                 // before trading
  // vol-scaled position sizing
  int vol_sizing_enabled; // 0=disabled, 1=scale qty inversely with volatility
  FPN<F> vol_scale_min; // min scale factor (e.g. 0.25 = never less than 25% of
                        // base qty)
  FPN<F> vol_scale_max; // max scale factor (e.g. 2.0 = never more than 200% of
                        // base qty)
  // no-trade band (cost-aware signal strength gate)
  int no_trade_band_enabled; // 0=disabled, 1=suppress entries when signal <
                             // fee_rate * mult
  FPN<F> no_trade_band_mult; // signal must exceed fee_rate * this to trade
                             // (e.g. 3.0)
  // ML inference
  int ml_backend;              // 0=disabled, 1=xgboost, 2=lightgbm
  char ml_model_path[256];     // path to buy-signal model file
  FPN<F> ml_buy_threshold;     // prediction > this = buy signal (e.g. 0.6)
  FPN<F> ml_tp_pct;            // TP % for ML positions (e.g. 0.015 = 1.5%)
  FPN<F> ml_sl_pct;            // SL % for ML positions (e.g. 0.008 = 0.8%)
  int regime_model_backend;    // 0=disabled, 1=xgboost, 2=lightgbm
  char regime_model_path[256]; // path to regime enrichment model
  FPN<F> regime_model_weight;  // score weight in Regime_Classify (e.g. 2)
  // danger gradient (hot-path crash protection)
  int danger_enabled;          // 0=disabled, 1=enabled
  FPN<F> danger_warn_stddevs;  // gradient starts at this many stddevs below avg
                               // (e.g. 3.0)
  FPN<F> danger_crash_stddevs; // full gate kill at this many stddevs below avg
                               // (e.g. 6.0)
  // tick recording (writes raw ticks to CSV for backtesting/ML training)
  int record_ticks; // 0=disabled (default), 1=record to
                    // data/{symbol}/YYYY-MM-DD.csv
  // depth recording (Phase 8a c5): writes @depth5@100ms snapshots to
  // data/{symbol}/depth/YYYY-MM-DD.csv. Requires depth_enabled=1 (recorder
  // is fed by depth_thread_fn). Off by default — opt-in for replay/audit.
  int record_depth; // 0=disabled (default), 1=record depth snapshots
  uint32_t
      record_max_days; // auto-prune CSVs older than this (default 30, ~2GB cap)

  // operational alerts (Phase 8b): route kill switch, orphan, disconnect
  // events through a configurable backend. All off by default.
  int notify_enabled;             // 0=disabled (default), 1=route alerts
  int notify_backend;             // 0=stderr (default), 1=command (popen-based)
  char notify_command[512];       // shell template with up to 2 %s (subject, body)
                                  // examples in engine.cfg / SettingsPanel tooltip
  uint32_t notify_cooldown_secs;  // per-event-kind cooldown (default 60)
  // FoxML integration — Phase 6C (all default OFF, zero behavior change when
  // disabled)
  int cost_gate_enabled; // 0=disabled, 1=estimate trade cost via CostModel,
                         // suppress if unprofitable
  int foxml_vol_scaling_enabled;  // 0=disabled, 1=scale risk_pct by VolScaler
                                  // inverse-vol on slow path
  FPN<F> foxml_vol_scaling_z_max; // z-score clipping threshold for VolScaler
                                  // (default 3.0)
  int bandit_enabled; // 0=disabled, 1=blend regime strategy with Exp3-IX bandit
                      // weights
  FPN<F> bandit_blend_ratio; // bandit influence fraction at full ramp (default
                             // 0.30)
  int confidence_enabled;    // 0=disabled, 1=dynamic ml_buy_threshold from
                             // confidence scoring
  // Phase 6 prep — tunable confidence loop parameters. Defaults preserve the
  // pre-Phase-6prep hardcoded values. Only consulted when confidence_enabled=1.
  uint32_t confidence_window;       // RollingIC + RollingRMSE window (default 32)
  FPN<F>   confidence_freshness_tau; // freshness decay constant in seconds (default 300)
  FPN<F>   confidence_threshold_scale; // gate formula: effective_thr = base * (this - conf)
                                       // (default 2.0 — clamps at 1.0 in code)
  // Prediction normalization — Phase 7F (default OFF)
  int prediction_normalize; // 0=disabled, 1=z-score normalize predictions
                            // (activates after 100)
  // Barrier gate — Phase 7E (default OFF)
  int barrier_gate_enabled;    // 0=disabled, 1=block entries before predicted
                               // price peaks
  char peak_model_path[256];   // path to P(will_peak) model
  char valley_model_path[256]; // path to P(will_valley) model
  // Phase 5c stupid-proofing: when a model is loaded from a run bundle
  // (core_N_model_dir), the engine reads models/{dir}/expected.cfg and
  // compares ML-relevant fields against the live config. mismatches are
  // a) warnings (default), b) load failures (strict=1), or c) ignored (=-1).
  // strict mode is recommended for production deployment; default mode
  // for development so a single missing expected.cfg doesn't break startup.
  int model_verify_strict;     // 0=warn (default), 1=strict, -1=skip
  // Per-core sharding (Phase 13) — STARTUP-ONLY, ignored by hot reload
  uint8_t
      engine_mode; // ENGINE_MODE_SINGLE_CORE (default) or ENGINE_MODE_SHARDED
  uint16_t num_execution_cores; // sharded mode only, ignored in single_core
                                // mode (default 4, cap 16)
  // Phase 14: when 1, sharded mode forces the synthetic tick generator
  // (sawtooth around $60k) instead of connecting to Binance. Useful for
  // latency demos that need reliable trade firing without depending on
  // current market volatility. Default 0 = use real Binance feed.
  uint8_t sharded_force_synthetic;
  // Per-core strategy assignment for sharded mode. core_strategies[i] is the
  // STRATEGY_* constant for execution core i. Default: all STRATEGY_SIMPLE_DIP.
  // Config syntax: core_0_strategy=simple_dip, core_1_strategy=ema_cross, etc.
  // Accepted names: mr, momentum, simple_dip, ml, ema_cross, none.
  uint8_t core_strategies[16]; // MAX_EXECUTION_CORES
  // Per-core risk allocation. core_risk_pct[i] is the fraction of total
  // balance this core can risk on a single trade. Default 0 = use the
  // shared risk_pct / num_cores. Non-zero = use this specific percentage.
  // Config syntax: core_0_risk_pct=20.0 (stored as 0.20).
  FPN<F> core_risk_pct[16];    // MAX_EXECUTION_CORES
  // Per-core ML model path. Each core running STRATEGY_ML can load its
  // own model. Default empty = use shared ml_model_path. Config syntax:
  // core_0_model_path=models/aggressive.xgb
  char core_model_path[16][256];
  // Per-core ML model directory. When set, the engine auto-discovers
  // role-specific models in this directory (barrier.json/.xgb,
  // buy_signal.json/.xgb, regime.json/.xgb, exit.json/.xgb) and loads
  // them into a CoreModelZoo. Missing files = role disabled.
  // When BOTH model_dir and model_path are set, model_dir wins (zoo
  // supersedes legacy single-model). Config syntax:
  // core_0_model_dir=models/aggressive/
  char core_model_dir[16][256];
  // Per-strategy TP/SL overrides. Default 0 = fall back to the shared
  // take_profit_pct / stop_loss_pct. Non-zero = use this instead.
  // Momentum already has momentum_tp_mult / momentum_sl_mult (stddev mults).
  FPN<F> simpledip_tp_pct;    // SimpleDip TP override (%, stored as decimal)
  FPN<F> simpledip_sl_pct;    // SimpleDip SL override
  FPN<F> mr_tp_pct;           // MeanReversion TP override
  FPN<F> mr_sl_pct;           // MeanReversion SL override
  FPN<F> emacross_tp_pct;     // EMA Cross TP override
  FPN<F> emacross_sl_pct;     // EMA Cross SL override
  // OMS phase 03: which path EventLoop_OnEvent takes when a TradeEvent
  // arrives. mode 0 (legacy): OnEvent mutates the portfolio + balance
  // directly, same as phase 02. mode 1 (event log): OnEvent just bumps
  // total_events_processed and routes to OMS, the OMS callback does the
  // portfolio mutation + balance update + kill switch peak + trade log
  // write. mode 1 is what the head-to-head test exercises and what
  // production runs after the soak. default 0 so existing tests stay
  // green during the migration window.
  uint32_t oms_event_log_mode; // 0 = legacy (default), 1 = event log
};
//======================================================================================================
template <unsigned F> inline ControllerConfig<F> ControllerConfig_Default() {
  ControllerConfig<F> cfg;
  cfg.poll_interval = 100;
  cfg.warmup_ticks = 128; // minimum raw ticks before trading
  cfg.min_warmup_samples =
      0; // min slow-path samples in rolling window (0 = warmup_ticks only)
  cfg.r2_threshold = FPN_FromDouble<F>(0.30);
  cfg.slope_scale_buy = FPN_FromDouble<F>(0.50);
  cfg.max_shift =
      FPN_FromDouble<F>(0.0001); // 0.01% of price — e.g. $7 at BTC $70k
  cfg.take_profit_pct = FPN_FromDouble<F>(0.03);
  cfg.stop_loss_pct = FPN_FromDouble<F>(0.015);
  cfg.starting_balance =
      FPN_FromDouble<F>(1000000.0); // 1M default so tests arent balance-limited
  cfg.fee_rate = FPN_FromDouble<F>(0.001); // 0.1% per trade (Binance default)
  cfg.risk_pct = FPN_FromDouble<F>(0.02);  // risk 2% of balance per position
  cfg.volume_multiplier = FPN_FromDouble<F>(3.0);
  cfg.entry_offset_pct = FPN_FromDouble<F>(0.0015);
  cfg.spacing_multiplier = FPN_FromDouble<F>(2.0);
  cfg.offset_min = FPN_FromDouble<F>(0.0005);     // 0.05% - most aggressive
  cfg.offset_max = FPN_FromDouble<F>(0.005);      // 0.5%  - most defensive
  cfg.vol_mult_min = FPN_FromDouble<F>(1.5);      // 1.5x  - most aggressive
  cfg.vol_mult_max = FPN_FromDouble<F>(6.0);      // 6.0x  - most defensive
  cfg.filter_scale = FPN_FromDouble<F>(0.50);     // how fast filters adapt
  cfg.max_drawdown_pct = FPN_FromDouble<F>(0.10); // halt at 10% drawdown
  cfg.max_exposure_pct =
      FPN_FromDouble<F>(0.50); // max 50% of balance in positions
  cfg.max_positions = 1; // single slot — exchange BTC balance IS the position
  cfg.offset_stddev_mult = FPN_Zero<F>(); // 0 = disabled, use percentage mode
  cfg.offset_stddev_min =
      FPN_FromDouble<F>(0.5); // 0.5 stddev - most aggressive
  cfg.offset_stddev_max = FPN_FromDouble<F>(4.0); // 4.0 stddev - most defensive
  cfg.min_long_slope = FPN_Zero<F>();             // 0 = disabled
  cfg.min_buy_delta = FPN_FromDouble<F>(
      -0.3); // allow mild selling, block heavy (-0.3 threshold)
  cfg.min_stddev_pct =
      FPN_Zero<F>(); // 0 = disabled (set in engine.cfg for live: 0.0003)
  cfg.momentum_r2_min =
      FPN_Zero<F>(); // 0 = disabled (set in engine.cfg for live: 0.4)
  cfg.tp_hold_score = FPN_Zero<F>();          // 0 = disabled, use fixed TP
  cfg.tp_trail_mult = FPN_FromDouble<F>(1.0); // trail 1 stddev below price
  cfg.sl_trail_mult = FPN_FromDouble<F>(2.0); // trail SL 2 stddevs below price
  cfg.fee_floor_mult =
      FPN_FromDouble<F>(3.0);      // TP floor = entry × fee_rate × 3
  cfg.vwap_offset = FPN_Zero<F>(); // 0 = disabled (backward compat)
  cfg.min_sl_tp_ratio = FPN_FromDouble<F>(0.5); // 2:1 reward/risk floor
  cfg.ror_tp_bonus =
      FPN_FromDouble<F>(1.2); // 20% wider TP on accelerating trend
  cfg.momentum_tp_r2_min = FPN_FromDouble<F>(0.5); // TP scale at R²=0
  cfg.momentum_sl_r2_max = FPN_FromDouble<F>(1.5); // SL scale at R²=0
  cfg.squeeze_decay = FPN_FromDouble<F>(0.10);     // 10% of gap per cycle
  cfg.offset_adapt_scale = FPN_FromDouble<F>(0.001);
  cfg.stddev_adapt_scale = FPN_FromDouble<F>(0.1);
  cfg.vol_adapt_scale = FPN_FromDouble<F>(0.1);
  cfg.breakout_min = FPN_FromDouble<F>(0.5); // 0.5 stddev floor
  cfg.slow_path_max_secs = 3;
  cfg.max_hold_ticks = 0; // 0 = disabled
  cfg.min_hold_gain_pct =
      FPN_FromDouble<F>(0.001); // 0.1% — only time-exit if below this gain
  // regime detection
  cfg.regime_slope_threshold =
      FPN_FromDouble<F>(0.00002); // legacy (unused by crossover classifier)
  cfg.regime_crossover_threshold = FPN_FromDouble<F>(
      0.0005); // 0.05% EMA-SMA gap = mild trend (~$35 at BTC $70k)
  cfg.regime_strong_crossover = FPN_FromDouble<F>(
      0.0015); // 0.15% EMA-SMA gap = strong trend (~$102 at BTC $68k)
  cfg.regime_r2_threshold =
      FPN_FromDouble<F>(0.70); // 70% consistency for trending
  cfg.regime_volatile_stddev =
      FPN_FromDouble<F>(0.0005); // 0.05% stddev/price (legacy compat)
  cfg.regime_vol_spike_ratio =
      FPN_FromDouble<F>(2.0);   // variance spike: 2x baseline = volatile
  cfg.regime_hysteresis = 5;    // 5 slow-path cycles before switch
  cfg.idle_reset_cycles = 30;   // ~90s idle before gate decay to initial
  cfg.sl_cooldown_cycles = 5;   // 5 slow-path cycles pause after SL
  cfg.sl_cooldown_adaptive = 0; // 0 = fixed, 1 = adaptive (backward compat)
  cfg.sl_cooldown_base = 2;     // min cooldown (spike recovery)
  cfg.sl_cooldown_extra = 8;    // max extra (strong downtrend)
  // momentum strategy
  cfg.momentum_breakout_mult = FPN_FromDouble<F>(1.5); // buy 1.5σ above avg
  cfg.momentum_tp_mult = FPN_FromDouble<F>(3.0);       // wider TP for trends
  cfg.momentum_sl_mult = FPN_FromDouble<F>(1.0);       // tighter SL than MR
  // EMA cross strategy
  cfg.emacross_dip_mult = FPN_FromDouble<F>(0.5);         // buy 0.5σ below EMA
  cfg.emacross_crossover_min = FPN_FromDouble<F>(0.0003); // 0.03% min spread
  cfg.emacross_trail_mult =
      FPN_FromDouble<F>(1.5); // 1.5x trail when EMA rising
  // volume spike detection
  cfg.spike_threshold = FPN_FromDouble<F>(5.0); // 5x rolling max triggers spike
  cfg.spike_spacing_reduction = FPN_FromDouble<F>(0.5); // half spacing on spike
  cfg.partial_exit_enabled = 0; // 0 = disabled (backward compat)
  cfg.partial_exit_pct = FPN_FromDouble<F>(0.5); // 50% at TP1, 50% rides
  cfg.tp2_mult = FPN_FromDouble<F>(2.0);         // TP2 = 2x TP1 distance
  cfg.breakeven_on_partial = 1; // move SL to entry after TP1 hit
  cfg.breakeven_on_profit =
      0; // 0 = disabled, 1 = ratchet SL to breakeven on profit
  cfg.breakeven_buffer_pct =
      FPN_FromDouble<F>(0.0005);    // +0.05% above entry (lock in tiny profit)
  cfg.slippage_pct = FPN_Zero<F>(); // 0 = disabled (backward compat)
  cfg.session_filter_enabled = 0;   // 0 = disabled (backward compat)
  cfg.session_asian_mult =
      FPN_FromDouble<F>(1.5); // wider gates in low-vol Asian session
  cfg.session_european_mult = FPN_FromDouble<F>(1.0); // normal during European
  cfg.session_us_mult = FPN_FromDouble<F>(0.8); // tighter gates, best liquidity
  cfg.session_overnight_mult =
      FPN_FromDouble<F>(1.3);             // wider gates, declining volume
  cfg.depth_enabled = 0;                  // 0 = disabled (backward compat)
  cfg.min_book_imbalance = FPN_Zero<F>(); // 0 = disabled
  // EMA gate
  cfg.gate_ema_enabled = 0; // 0 = disabled (backward compat)
  cfg.gate_ema_alpha = FPN_FromDouble<F>(0.997); // ~333-tick effective window
  cfg.gate_ema_one_minus_alpha = FPN_FromDouble<F>(0.003); // 1.0 - 0.997
  cfg.default_strategy = -1; // -1 = regime auto (backward compat)
  cfg.use_real_money = 0;    // 0 = paper trading (default safe)
  // kill switch
  cfg.kill_switch_enabled = 1; // on by default — safety first
  cfg.kill_switch_daily_loss_pct =
      FPN_FromDouble<F>(0.03); // 3% daily loss triggers kill
  cfg.kill_switch_drawdown_pct =
      FPN_FromDouble<F>(0.05); // 5% drawdown from session peak
  cfg.kill_recovery_warmup =
      50; // 50 slow-path cycles observation after kill reset
  // vol-scaled sizing
  cfg.vol_sizing_enabled = 0; // off by default (backward compat)
  cfg.vol_scale_min = FPN_FromDouble<F>(0.25);
  cfg.vol_scale_max = FPN_FromDouble<F>(2.0);
  // no-trade band
  cfg.no_trade_band_enabled = 0; // off by default (backward compat)
  cfg.no_trade_band_mult = FPN_FromDouble<F>(3.0);
  // ML inference (disabled by default — zero overhead when off)
  cfg.ml_backend = 0;
  cfg.ml_model_path[0] = '\0';
  cfg.ml_buy_threshold = FPN_FromDouble<F>(0.6);
  cfg.ml_tp_pct = FPN_FromDouble<F>(0.015); // 1.5% TP
  cfg.ml_sl_pct = FPN_FromDouble<F>(0.008); // 0.8% SL
  cfg.regime_model_backend = 0;
  cfg.regime_model_path[0] = '\0';
  cfg.regime_model_weight = FPN_FromDouble<F>(2.0);
  // danger gradient
  cfg.danger_enabled = 1;
  cfg.danger_warn_stddevs =
      FPN_FromDouble<F>(3.0); // gradient starts at 3σ below avg
  cfg.danger_crash_stddevs =
      FPN_FromDouble<F>(6.0); // full gate kill at 6σ below avg
  // tick recording (disabled by default — no disk usage unless explicitly
  // enabled)
  cfg.record_ticks = 0;
  cfg.record_depth = 0; // Phase 8a c5 — opt-in
  cfg.record_max_days = 30;
  // Phase 8b — operational alerts (all opt-in; default = no behavior change)
  cfg.notify_enabled = 0;
  cfg.notify_backend = 0;          // stderr
  cfg.notify_command[0] = '\0';
  cfg.notify_cooldown_secs = 60;
  // FoxML integration — Phase 6C (all OFF by default, zero behavior change)
  cfg.cost_gate_enabled = 0;
  cfg.foxml_vol_scaling_enabled = 0;
  cfg.foxml_vol_scaling_z_max = FPN_FromDouble<F>(3.0);
  cfg.bandit_enabled = 0;
  cfg.bandit_blend_ratio = FPN_FromDouble<F>(0.30);
  cfg.confidence_enabled = 0;
  // Phase 6 prep — defaults match the pre-amend hardcoded values
  cfg.confidence_window           = 32;                          // CONFIDENCE_IC_WINDOW_DEFAULT
  cfg.confidence_freshness_tau    = FPN_FromDouble<F>(300.0);    // CONFIDENCE_FRESHNESS_TAU_DEFAULT
  cfg.confidence_threshold_scale  = FPN_FromDouble<F>(2.0);      // hardcoded `2.0` in gate formula
  cfg.prediction_normalize = 0;
  cfg.barrier_gate_enabled = 0;
  cfg.model_verify_strict = 0;  // 0=warn, 1=strict (fail on mismatch), -1=skip
  cfg.peak_model_path[0] = '\0';
  cfg.valley_model_path[0] = '\0';
  // Per-core sharding (Phase 13) — safe defaults: legacy mode, 4 cores if opted
  // in
  cfg.engine_mode = ENGINE_MODE_SINGLE_CORE;
  cfg.num_execution_cores = 4;
  cfg.sharded_force_synthetic = 0;
  for (int i = 0; i < 16; ++i) cfg.core_strategies[i] = 2;  // STRATEGY_SIMPLE_DIP
  for (int i = 0; i < 16; ++i) cfg.core_risk_pct[i] = FPN_Zero<F>();  // 0 = shared
  for (int i = 0; i < 16; ++i) cfg.core_model_path[i][0] = '\0';    // empty = shared
  for (int i = 0; i < 16; ++i) cfg.core_model_dir[i][0] = '\0';     // empty = use model_path or shared
  cfg.simpledip_tp_pct  = FPN_Zero<F>();  // 0 = use shared take_profit_pct
  cfg.simpledip_sl_pct  = FPN_Zero<F>();
  cfg.mr_tp_pct         = FPN_Zero<F>();
  cfg.mr_sl_pct         = FPN_Zero<F>();
  cfg.emacross_tp_pct   = FPN_Zero<F>();
  cfg.emacross_sl_pct   = FPN_Zero<F>();
  // OMS phase 03 — default to legacy OnEvent path so existing tests and
  // the production engine before the migration soak stay on the known-good
  // code.
  cfg.oms_event_log_mode = 0;
  return cfg;
}
//======================================================================================================
// [CONFIG PARSER]
//======================================================================================================
// simple key=value text file parser, no JSON, no external libs
// returns defaults if file is missing or unreadable
//======================================================================================================
template <unsigned F>
inline ControllerConfig<F> ControllerConfig_Load(const char *filepath) {
  ControllerConfig<F> cfg = ControllerConfig_Default<F>();

  FILE *f = fopen(filepath, "r");
  if (!f)
    return cfg;

  char line[256];
  while (fgets(line, sizeof(line), f)) {
    // strip \r\n
    int len = 0;
    while (line[len] && line[len] != '\n' && line[len] != '\r')
      len++;
    line[len] = '\0';

    // skip empty lines and comments
    if (len == 0 || line[0] == '#')
      continue;

    // find '='
    int eq_pos = -1;
    for (int i = 0; i < len; i++) {
      if (line[i] == '=') {
        eq_pos = i;
        break;
      }
    }
    if (eq_pos < 0)
      continue;

    // null-terminate key, value starts after '='
    line[eq_pos] = '\0';
    char *key = line;
    char *val = &line[eq_pos + 1];

// table-driven parser: FPN fields parsed as atof(val) directly
// adding a new field = add ONE line to the matching table below
#define CFG_PARSE_FPN(name)                                                    \
  if (strcmp(key, #name) == 0) {                                               \
    cfg.name = FPN_FromDouble<F>(atof(val));                                   \
    continue;                                                                  \
  }

// FPN fields parsed as atof(val) / 100.0 (percentage: config says 15.0, stored
// as 0.15)
#define CFG_PARSE_PCT(name)                                                    \
  if (strcmp(key, #name) == 0) {                                               \
    cfg.name = FPN_FromDouble<F>(atof(val) / 100.0);                           \
    continue;                                                                  \
  }

// uint32_t fields
#define CFG_PARSE_U32(name)                                                    \
  if (strcmp(key, #name) == 0) {                                               \
    cfg.name = (uint32_t)atol(val);                                            \
    continue;                                                                  \
  }

// int fields
#define CFG_PARSE_INT(name)                                                    \
  if (strcmp(key, #name) == 0) {                                               \
    cfg.name = atoi(val);                                                      \
    continue;                                                                  \
  }

// FPN fields with min-zero clamp
#define CFG_PARSE_FPN_POS(name)                                                \
  if (strcmp(key, #name) == 0) {                                               \
    double v = atof(val);                                                      \
    if (v < 0)                                                                 \
      v = 0;                                                                   \
    cfg.name = FPN_FromDouble<F>(v);                                           \
    continue;                                                                  \
  }

    //--- FPN raw (value used directly) ---
    CFG_PARSE_FPN(r2_threshold)
    CFG_PARSE_FPN(slope_scale_buy)
    CFG_PARSE_FPN(max_shift)
    CFG_PARSE_FPN(starting_balance)
    CFG_PARSE_FPN(volume_multiplier)
    CFG_PARSE_FPN(spacing_multiplier)
    CFG_PARSE_FPN(vol_mult_min)
    CFG_PARSE_FPN(vol_mult_max)
    CFG_PARSE_FPN(filter_scale)
    CFG_PARSE_FPN(min_long_slope)
    CFG_PARSE_FPN(min_buy_delta)
    CFG_PARSE_FPN(vwap_offset)
    CFG_PARSE_FPN(min_stddev_pct)
    CFG_PARSE_FPN(momentum_r2_min)
    CFG_PARSE_FPN(min_sl_tp_ratio)
    CFG_PARSE_FPN(ror_tp_bonus)
    CFG_PARSE_FPN(momentum_tp_r2_min)
    CFG_PARSE_FPN(momentum_sl_r2_max)
    CFG_PARSE_FPN(squeeze_decay)
    CFG_PARSE_FPN(offset_adapt_scale)
    CFG_PARSE_FPN(stddev_adapt_scale)
    CFG_PARSE_FPN(vol_adapt_scale)
    CFG_PARSE_FPN(breakout_min)
    CFG_PARSE_FPN(regime_slope_threshold)
    CFG_PARSE_FPN(regime_crossover_threshold)
    CFG_PARSE_FPN(regime_strong_crossover)
    CFG_PARSE_FPN(regime_volatile_stddev)
    CFG_PARSE_FPN(regime_vol_spike_ratio)
    CFG_PARSE_FPN(momentum_breakout_mult)
    CFG_PARSE_FPN(momentum_tp_mult)
    CFG_PARSE_FPN(momentum_sl_mult)
    CFG_PARSE_FPN(emacross_dip_mult)
    CFG_PARSE_FPN(emacross_crossover_min)
    CFG_PARSE_FPN(emacross_trail_mult)
    CFG_PARSE_FPN(spike_threshold)
    CFG_PARSE_FPN(spike_spacing_reduction)
    CFG_PARSE_FPN(session_asian_mult)
    CFG_PARSE_FPN(session_european_mult)
    CFG_PARSE_FPN(session_us_mult)
    CFG_PARSE_FPN(session_overnight_mult)

    //--- FPN percentage (config says 15.0, stored as 0.15) ---
    CFG_PARSE_PCT(take_profit_pct)
    CFG_PARSE_PCT(stop_loss_pct)
    CFG_PARSE_PCT(fee_rate)
    CFG_PARSE_PCT(risk_pct)
    CFG_PARSE_PCT(entry_offset_pct)
    CFG_PARSE_PCT(offset_min)
    CFG_PARSE_PCT(offset_max)
    CFG_PARSE_PCT(max_drawdown_pct)
    CFG_PARSE_PCT(max_exposure_pct)
    CFG_PARSE_PCT(min_hold_gain_pct)
    CFG_PARSE_PCT(regime_r2_threshold)
    CFG_PARSE_PCT(slippage_pct)
    CFG_PARSE_PCT(kill_switch_daily_loss_pct)
    CFG_PARSE_PCT(kill_switch_drawdown_pct)
    CFG_PARSE_PCT(ml_tp_pct)
    CFG_PARSE_PCT(ml_sl_pct)

    //--- FPN with min-zero clamp ---
    CFG_PARSE_FPN_POS(offset_stddev_mult)
    CFG_PARSE_FPN_POS(offset_stddev_min)
    CFG_PARSE_FPN_POS(offset_stddev_max)
    CFG_PARSE_FPN_POS(tp_hold_score)
    CFG_PARSE_FPN_POS(tp_trail_mult)
    CFG_PARSE_FPN_POS(sl_trail_mult)
    // fee_floor_mult: min 1.0 (special case)
    if (strcmp(key, "fee_floor_mult") == 0) {
      double v = atof(val);
      if (v < 1)
        v = 1;
      cfg.fee_floor_mult = FPN_FromDouble<F>(v);
      continue;
    }

    //--- uint32_t ---
    CFG_PARSE_U32(poll_interval)
    CFG_PARSE_U32(warmup_ticks)
    CFG_PARSE_U32(slow_path_max_secs)
    CFG_PARSE_U32(max_hold_ticks)
    CFG_PARSE_U32(regime_hysteresis)
    CFG_PARSE_U32(min_warmup_samples)
    CFG_PARSE_U32(idle_reset_cycles)
    CFG_PARSE_U32(sl_cooldown_cycles)
    CFG_PARSE_U32(sl_cooldown_base)
    CFG_PARSE_U32(sl_cooldown_extra)
    CFG_PARSE_U32(kill_recovery_warmup)
    // max_positions: clamped 1-16 (special case)
    if (strcmp(key, "max_positions") == 0) {
      int v = atoi(val);
      if (v < 1)
        v = 1;
      if (v > 16)
        v = 16;
      cfg.max_positions = (uint32_t)v;
      continue;
    }

    //--- int ---
    CFG_PARSE_INT(sl_cooldown_adaptive)
    CFG_PARSE_INT(partial_exit_enabled)
    CFG_PARSE_INT(breakeven_on_partial)
    CFG_PARSE_INT(breakeven_on_profit)
    CFG_PARSE_PCT(breakeven_buffer_pct)
    CFG_PARSE_INT(depth_enabled)
    CFG_PARSE_INT(use_real_money)
    CFG_PARSE_INT(session_filter_enabled)
    CFG_PARSE_INT(gate_ema_enabled)
    CFG_PARSE_INT(default_strategy)
    CFG_PARSE_INT(kill_switch_enabled)
    CFG_PARSE_INT(vol_sizing_enabled)
    CFG_PARSE_INT(no_trade_band_enabled)
    CFG_PARSE_INT(ml_backend)
    CFG_PARSE_INT(regime_model_backend)

    //--- partial exit + depth + EMA FPN ---
    CFG_PARSE_FPN(partial_exit_pct)
    CFG_PARSE_FPN(tp2_mult)
    CFG_PARSE_FPN(min_book_imbalance)
    CFG_PARSE_FPN(vol_scale_min)
    CFG_PARSE_FPN(vol_scale_max)
    CFG_PARSE_FPN(no_trade_band_mult)
    CFG_PARSE_FPN(ml_buy_threshold)
    CFG_PARSE_FPN(regime_model_weight)
    CFG_PARSE_INT(danger_enabled)
    CFG_PARSE_FPN(danger_warn_stddevs)
    CFG_PARSE_FPN(danger_crash_stddevs)

    //--- tick recording ---
    CFG_PARSE_INT(record_ticks)
    CFG_PARSE_INT(record_depth)
    CFG_PARSE_U32(record_max_days)

    //--- operational alerts (Phase 8b) ---
    CFG_PARSE_INT(notify_enabled)
    CFG_PARSE_INT(notify_backend)
    CFG_PARSE_U32(notify_cooldown_secs)
    // notify_command is a string — no macro for that, inline parse below
    if (strcmp(key, "notify_command") == 0) {
        strncpy(cfg.notify_command, val, sizeof(cfg.notify_command) - 1);
        cfg.notify_command[sizeof(cfg.notify_command) - 1] = '\0';
        // strip trailing newline if any (the cfg parser usually does this,
        // but be defensive — bad commands break alerts silently otherwise)
        size_t nl = strlen(cfg.notify_command);
        while (nl > 0 && (cfg.notify_command[nl-1] == '\n' ||
                          cfg.notify_command[nl-1] == '\r')) {
            cfg.notify_command[--nl] = '\0';
        }
        continue;
    }

    //--- FoxML integration (Phase 6C) ---
    CFG_PARSE_INT(cost_gate_enabled)
    CFG_PARSE_INT(foxml_vol_scaling_enabled)
    CFG_PARSE_FPN(foxml_vol_scaling_z_max)
    CFG_PARSE_INT(bandit_enabled)
    CFG_PARSE_FPN(bandit_blend_ratio)
    CFG_PARSE_INT(confidence_enabled)
    CFG_PARSE_U32(confidence_window)
    CFG_PARSE_FPN(confidence_freshness_tau)
    CFG_PARSE_FPN(confidence_threshold_scale)
    CFG_PARSE_INT(prediction_normalize)
    CFG_PARSE_INT(barrier_gate_enabled)
    CFG_PARSE_INT(model_verify_strict)

    // Per-core sharding (Phase 13) — engine_mode accepts both string and int
    // forms. The GUI SettingsPanel uses CFG_BOOL which writes "0"/"1"; manual
    // edits to engine.cfg can use "single_core"/"sharded" for clarity.
    if (strcmp(key, "engine_mode") == 0) {
      if (strcmp(val, "sharded") == 0 || strcmp(val, "1") == 0)
        cfg.engine_mode = ENGINE_MODE_SHARDED;
      else
        cfg.engine_mode = ENGINE_MODE_SINGLE_CORE;
      continue;
    }
    // num_execution_cores: clamped to [1, 16] (special case to enforce the cap)
    if (strcmp(key, "num_execution_cores") == 0) {
      int v = atoi(val);
      if (v < 1)
        v = 1;
      if (v > 16)
        v = 16;
      cfg.num_execution_cores = (uint16_t)v;
      continue;
    }
    if (strcmp(key, "sharded_force_synthetic") == 0) {
      cfg.sharded_force_synthetic = (uint8_t)(atoi(val) != 0 ? 1 : 0);
      continue;
    }
    // Per-core risk: core_0_risk_pct=20.0 means core 0 risks 20% of balance
    if (strncmp(key, "core_", 5) == 0 && strstr(key, "_risk_pct")) {
      int core_idx = atoi(key + 5);
      if (core_idx >= 0 && core_idx < 16) {
        cfg.core_risk_pct[core_idx] = FPN_FromDouble<F>(atof(val) / 100.0);
      }
      continue;
    }
    // Per-core model path: core_0_model_path=models/aggressive.xgb
    if (strncmp(key, "core_", 5) == 0 && strstr(key, "_model_path")) {
      int core_idx = atoi(key + 5);
      if (core_idx >= 0 && core_idx < 16) {
        strncpy(cfg.core_model_path[core_idx], val,
                sizeof(cfg.core_model_path[core_idx]) - 1);
        cfg.core_model_path[core_idx][sizeof(cfg.core_model_path[core_idx]) - 1] = '\0';
      }
      continue;
    }
    // Per-core model dir: core_0_model_dir=models/aggressive/
    // when set, engine auto-discovers role-specific models in the directory
    // (barrier.json, buy_signal.json, regime.json, exit.json) and loads each
    // present file into the per-core CoreModelZoo.
    if (strncmp(key, "core_", 5) == 0 && strstr(key, "_model_dir")) {
      int core_idx = atoi(key + 5);
      if (core_idx >= 0 && core_idx < 16) {
        strncpy(cfg.core_model_dir[core_idx], val,
                sizeof(cfg.core_model_dir[core_idx]) - 1);
        cfg.core_model_dir[core_idx][sizeof(cfg.core_model_dir[core_idx]) - 1] = '\0';
      }
      continue;
    }
    // Per-core strategy: core_0_strategy=simple_dip, core_1_strategy=none, etc.
    if (strncmp(key, "core_", 5) == 0 && strstr(key, "_strategy")) {
      int core_idx = atoi(key + 5);
      if (core_idx >= 0 && core_idx < 16) {
        uint8_t sid = 0xFF; // STRATEGY_NONE
        if (strcmp(val, "mr") == 0 || strcmp(val, "mean_reversion") == 0) sid = 0;
        else if (strcmp(val, "momentum") == 0 || strcmp(val, "mom") == 0) sid = 1;
        else if (strcmp(val, "simple_dip") == 0 || strcmp(val, "dip") == 0) sid = 2;
        else if (strcmp(val, "ml") == 0) sid = 3;
        else if (strcmp(val, "ema_cross") == 0 || strcmp(val, "ema") == 0) sid = 4;
        else if (strcmp(val, "none") == 0) sid = 0xFF;
        else sid = (uint8_t)atoi(val);  // numeric fallback
        cfg.core_strategies[core_idx] = sid;
      }
      continue;
    }
    // Per-strategy TP/SL overrides (percentage, parsed with /100)
    CFG_PARSE_PCT(simpledip_tp_pct)
    CFG_PARSE_PCT(simpledip_sl_pct)
    CFG_PARSE_PCT(mr_tp_pct)
    CFG_PARSE_PCT(mr_sl_pct)
    CFG_PARSE_PCT(emacross_tp_pct)
    CFG_PARSE_PCT(emacross_sl_pct)
    // OMS phase 03 — accept both string and int values for clarity in cfg files
    if (strcmp(key, "oms_event_log_mode") == 0) {
      if (strcmp(val, "legacy") == 0)
        cfg.oms_event_log_mode = 0;
      else if (strcmp(val, "event_log") == 0)
        cfg.oms_event_log_mode = 1;
      else
        cfg.oms_event_log_mode = (uint32_t)atoi(val);
      continue;
    }

    // ML model paths (string fields — not atof)
    if (strcmp(key, "ml_model_path") == 0) {
      strncpy(cfg.ml_model_path, val, sizeof(cfg.ml_model_path) - 1);
      cfg.ml_model_path[sizeof(cfg.ml_model_path) - 1] = '\0';
      continue;
    }
    if (strcmp(key, "regime_model_path") == 0) {
      strncpy(cfg.regime_model_path, val, sizeof(cfg.regime_model_path) - 1);
      cfg.regime_model_path[sizeof(cfg.regime_model_path) - 1] = '\0';
      continue;
    }
    if (strcmp(key, "peak_model_path") == 0) {
      strncpy(cfg.peak_model_path, val, sizeof(cfg.peak_model_path) - 1);
      cfg.peak_model_path[sizeof(cfg.peak_model_path) - 1] = '\0';
      continue;
    }
    if (strcmp(key, "valley_model_path") == 0) {
      strncpy(cfg.valley_model_path, val, sizeof(cfg.valley_model_path) - 1);
      cfg.valley_model_path[sizeof(cfg.valley_model_path) - 1] = '\0';
      continue;
    }

    // EMA alpha: parse alpha and precompute 1-alpha
    if (strcmp(key, "gate_ema_alpha") == 0) {
      double a = atof(val);
      cfg.gate_ema_alpha = FPN_FromDouble<F>(a);
      cfg.gate_ema_one_minus_alpha = FPN_FromDouble<F>(1.0 - a);
      continue;
    }

#undef CFG_PARSE_FPN
#undef CFG_PARSE_PCT
#undef CFG_PARSE_U32
#undef CFG_PARSE_INT
#undef CFG_PARSE_FPN_POS
  }

  fclose(f);

  // post-load validation/clamping. min_warmup_samples gates on rolling.count
  // which caps at the short rolling window size (W=128). Values above 128
  // mean "warmup never completes" — user-hostile silent failure. Clamp +
  // explain so the user understands what happened and what to use instead.
  // (Took us multiple hours of debugging Friday night before we figured this
  // out — the field name implied "ticks" but actually means "rolling window
  // samples." See CLAUDE.md "Label-type-aware metric invariant" for the
  // sibling rule about consulting source-of-truth helpers.)
  const uint32_t ROLLING_WINDOW_SHORT = 128; // matches RollingStats<F> default W
  if (cfg.min_warmup_samples > ROLLING_WINDOW_SHORT) {
    fprintf(stderr,
            "[CFG] WARNING: min_warmup_samples=%u exceeds rolling window size "
            "%u and would cause warmup to never complete. Clamped to %u.\n"
            "      If you want a longer total-tick warmup, use warmup_ticks "
            "instead (counts raw ticks, no upper bound).\n",
            cfg.min_warmup_samples, ROLLING_WINDOW_SHORT, ROLLING_WINDOW_SHORT);
    cfg.min_warmup_samples = ROLLING_WINDOW_SHORT;
  }

  return cfg;
}
//======================================================================================================
//======================================================================================================
#endif // CONTROLLER_CONFIG_HPP
