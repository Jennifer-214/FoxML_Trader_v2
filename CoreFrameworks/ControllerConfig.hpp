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
#include <errno.h>      // v5.11.18a: ERANGE detection in feature_mask hex parser

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

// v4.7.39: engine_arch — controls slow-path threading model under sharded
// mode. STARTUP-ONLY (changes ignored on hot reload).
//   ENGINE_ARCH_CENTRALIZED (default): slow-path runs on producer thread,
//     iterating per-core work in one loop. Existing behavior since v4.x.
//   ENGINE_ARCH_PER_CORE_SLOW: spawns N pthreads at boot, each running
//     a per-core slow-path loop. Producer keeps global work (RollingStats
//     pushes, snapshot save, GUI publish, global KillSwitchEvaluate).
//     Per-core slow-paths handle: strategy dispatch, regime classify,
//     gate parameter rebuild, time-exit, trailing SL, post-fill drain,
//     warmup permission, per-core kill switch, swap/drag/manual-close.
//   See plans/2026-04-28-per-core-slow-path-master.md for full design.
constexpr uint8_t ENGINE_ARCH_CENTRALIZED   = 0;
constexpr uint8_t ENGINE_ARCH_PER_CORE_SLOW = 1;

//======================================================================================================
// [PER-CORE OVERRIDES — v4.0]
//======================================================================================================
// One slot per execution core (16 max). Each field shadows a same-named
// `ControllerConfig` field; non-zero overrides the global; zero (default)
// means "inherit from global". Resolved per-core via
// `ControllerConfig_ResolveForCore(global, core_id)` on every slow-path
// rebuild.
//
// Why this exists: pre-4.0, each strategy *type* had its own override
// (`mr_tp_pct`, `momentum_tp_mult`, etc.) but those overrides were
// account-wide — Core 0 running MR @ 4% TP and Core 1 running MR @ 6% TP
// was impossible. Per-core overrides let you A/B identical strategies
// with different tunings on different cores, without touching the global
// defaults.
//
// Cfg syntax: `core_N_<field>=<value>`
//   core_0_take_profit_pct=4.0
//   core_1_take_profit_pct=6.0
//   core_2_mr_tp_pct=3.5
//   core_0_ml_buy_threshold=0.6
//
// Adding a field: extend this struct + add the override line in
// `ControllerConfig_ResolveForCore` + add a parser case in
// `ControllerConfig_Load`'s per-core block + surface in the Settings
// panel's per-core tab. Four sites, all in this file + SettingsPanel.hpp.
//======================================================================================================
// v4.7.24: per-core override fields driven by ONE X-macro list. Adding a
// new override = ONE line in this macro. Struct members, init zeroing,
// resolver overwrite logic, and cfg parser ALL auto-derive from this list.
//
// PCT(name)  — percentage field; cfg writes 4.0, stored as 0.04. Parser
//              divides by 100.0 at load.
// RAW(name)  — raw FPN field; cfg writes 3.0, stored as 3.0. Parser uses
//              atof directly.
//
// Field NAMES MUST MATCH same-named members of ControllerConfig exactly —
// the resolver does a direct field-by-field overwrite by name.
//
// Pre-v4.7.24 this required 4 separate edits across this file (struct,
// init, resolver, parser) — easy to forget one site and silently lose
// the override on a new field. The X-macro collapses that to one line.
#define PER_CORE_OVERRIDE_FIELDS(PCT, RAW) \
    /* Trading */ \
    PCT(take_profit_pct) \
    PCT(stop_loss_pct) \
    RAW(fee_floor_mult) \
    /* Entry filters */ \
    PCT(entry_offset_pct) \
    RAW(volume_multiplier) \
    RAW(spacing_multiplier) \
    RAW(offset_stddev_mult) \
    /* Strategy-specific */ \
    PCT(simpledip_tp_pct) \
    PCT(simpledip_sl_pct) \
    PCT(mr_tp_pct) \
    PCT(mr_sl_pct) \
    RAW(momentum_tp_mult) \
    RAW(momentum_sl_mult) \
    RAW(momentum_r2_min) \
    PCT(emacross_tp_pct) \
    PCT(emacross_sl_pct) \
    RAW(emacross_dip_mult) \
    RAW(emacross_crossover_min) \
    RAW(emacross_trail_mult) \
    PCT(ml_tp_pct) \
    PCT(ml_sl_pct) \
    RAW(ml_buy_threshold) \
    /* v4.7.29: Adaptation overrides — adaptive feedback per core */ \
    RAW(filter_scale) \
    RAW(r2_threshold) \
    RAW(slope_scale_buy) \
    RAW(max_shift) \
    PCT(offset_min) \
    PCT(offset_max) \
    RAW(vol_mult_min) \
    RAW(vol_mult_max) \
    /* v4.7.29: Trailing TP/SL overrides — exit ratchet per core */ \
    RAW(tp_hold_score) \
    RAW(tp_trail_mult) \
    RAW(sl_trail_mult) \
    /* v4.7.29: Time exit override — holding period per core */ \
    PCT(min_hold_gain_pct) \
    /* v4.7.29: Vol sizing overrides — position scale curve per core */ \
    RAW(vol_scale_min) \
    RAW(vol_scale_max) \
    /* v4.7.29: No-trade band override — entry gate strictness per core */ \
    RAW(no_trade_band_mult) \
    /* v4.7.29: Partial exit geometry overrides — TP1 split + TP2 mult per core */ \
    RAW(partial_exit_pct) \
    RAW(tp2_mult) \
    /* v4.7.31: ML/FoxML overrides — different ML cores can have different */ \
    /* confidence behavior, vol-z scaling, bandit blend ratios, etc. */ \
    RAW(foxml_vol_scaling_z_max) \
    RAW(bandit_blend_ratio) \
    RAW(confidence_freshness_tau) \
    RAW(confidence_threshold_scale)

// v4.7.40: INT-typed per-core overrides. Separate macro because INT fields
// are uint32_t (not FPN<F>) — different declaration + parser. 0 = inherit
// (caller checks `if (override == 0) use cfg.field`). All sentinel-friendly
// (cfg INT defaults are non-zero meaningful values).
#define PER_CORE_OVERRIDE_INT_FIELDS(INT) \
    /* v4.7.40: per-core slow-path cadence. Each engine can poll faster or */ \
    /* slower than the global poll_interval — fast strategies (momentum) */ \
    /* may want tighter rebuilds; slow strategies (MR) tolerate longer. */ \
    INT(poll_interval)

template <unsigned F> struct PerCoreOverrides {
#define _DECL_OV_FIELD(name) FPN<F> name;
    PER_CORE_OVERRIDE_FIELDS(_DECL_OV_FIELD, _DECL_OV_FIELD)
#undef _DECL_OV_FIELD
// v4.7.40: INT-typed overrides. uint32_t storage; 0 = inherit.
#define _DECL_OV_INT_FIELD(name) uint32_t name;
    PER_CORE_OVERRIDE_INT_FIELDS(_DECL_OV_INT_FIELD)
#undef _DECL_OV_INT_FIELD
};

// v5.9.2c — CSV tick-sort validation modes (csv_sort_check_mode field).
// Backtest path checks tick timestamp ordering post-load; live engine
// doesn't load CSV (Binance WS is in-order by construction).
#define CSV_SORT_WARN   0   // default — log violations, proceed
#define CSV_SORT_STRICT 1   // refuse load on any violation
#define CSV_SORT_AUTO   2   // sort in-place + INFO log of violation count

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
                           // Phase 8: legacy field. Pre-Phase-8 behavior preserved
                           // when fee_rate_maker == fee_rate_taker == fee_rate.
                           // Backtest fingerprint hashes this field (NOT the new
                           // maker/taker fields) — preserves bundle compatibility.
  // Phase 8 — bifurcated maker/taker fee rates. Live engine uses these per fill
  // based on order->is_maker (set from Binance executionReport "m" field).
  // Backtest simulates as all-taker (is_maker=0 always). Documented divergence.
  FPN<F> fee_rate_maker;   // maker fill fee rate (e.g. 0.00075 = 0.075% Binance tier 0)
  FPN<F> fee_rate_taker;   // taker fill fee rate (e.g. 0.00100 = 0.100% Binance tier 0)
  // v4.3.2 (Track C.1) — pay fees in BNB on Binance gives a 25% discount
  // on both maker and taker. When set, fee_rate_maker and fee_rate_taker
  // are scaled by 0.75 at engine boot. User must also enable BNB fee
  // payment in Binance UI (one-time account setting). Logged at boot so
  // the config is visible. 0 = disabled (default).
  uint32_t pay_fees_in_bnb;
  // Fee_Compute helper — defined after the struct so all fee math sites
  // share one implementation. See note in main file just below struct.
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
  // v5.9.2c — CSV tick-sort validation mode. Backtest path checks that
  // ticks are timestamp-monotonic post-load; live engine doesn't load
  // CSV (Binance WS is in-order by construction). Mode values:
  //   0 = WARN   (default — log violations, proceed with potentially-
  //               garbage features). Backward-compat with pre-v5.9.2c.
  //   1 = STRICT (refuse load on any violation; abort backtest run)
  //   2 = AUTO   (sort the array in-place, log INFO with violation count)
  // Without this guard, unsorted CSVs (concatenated daily exports,
  // mistyped tick replays) silently produce garbage rolling stats /
  // ROR / tick-rate features at training time.
  int csv_sort_check_mode;
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
  // v5.7.5 — MOM quality filters. All default to 0 (filter off,
  // current behavior preserved). Operator opts in after observing
  // v5.7.6 quality dashboard data. Each filter sets BUY_BLOCKED +
  // a SHALT_MOM_* code when triggered, surfaced in the v5.6 GUI.
  // See DOCS/changelogs/2026-04-30-regime-classifier-audit.md for
  // why these are defensive depth even after Phase 2's hardcoded
  // strategy boot guard.
  FPN<F> momentum_min_tp_margin_pct;     // SHALT_MOM_TP_TOO_TIGHT — require tp_pct >= this (recommended: 0.0040 = 0.40%)
  FPN<F> momentum_min_buy_delta_recent;  // SHALT_MOM_NO_FLOW — require recent volume_delta >= this (rec: 0.05)
  FPN<F> momentum_min_r2;                // SHALT_MOM_LOW_R2 — require short_r2 >= this (rec: 0.30)
  int momentum_require_last_win;         // SHALT_MOM_LAST_LOST — 1 = block re-entry until previous trade was TP win (rec: 0=off)
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
  // v5.7.2: explicit acknowledgment that the operator wants to run a
  // hardcoded (non-AUTO) strategy in live mode. Default 0 — the boot
  // path refuses to start with use_real_money=1 AND any
  // core_N_strategy != auto unless this flag is set. AUTO (regime-
  // gated) is preferred for live capital because hardcoded strategies
  // fire regardless of regime. Setting this to 1 is the operator
  // saying "I know what I'm doing" and is logged.
  int acknowledge_hardcoded_strategy_in_live;
  // v5.11.3 — mlockall failure handling. 1 (default) = HFT-correct: fatal
  // exit if pages can't be locked into RAM (deployment must have enough
  // RLIMIT_MEMLOCK + CAP_IPC_LOCK). 0 = laptop / dev: warn and continue
  // when mlockall fails. Setting this to 0 in production trades determinism
  // for portability — operator should explicitly opt out.
  int require_mlockall;
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
  // v5.9.1 (V5_9_AUDIT-#21) — hard-block entries when raw confidence is below
  // this threshold, INDEPENDENT of damping. With low IC (noisy predictions),
  // damped threshold can become pathological; trades fire on essentially-
  // random predictions. Hard floor is the safety net.
  // Default 0.0 = disabled (preserves pre-v5.9.1 behavior). Operator opts in
  // (audit-recommended value 0.05). Only consulted when confidence_enabled=1.
  // Surfaced via SHALT_LOW_CONFIDENCE on the strategy_halt_reason channel.
  FPN<F>   confidence_hard_block_threshold;
  // Phase 7 prep — held-out validation infrastructure. Used by foxml_suite
  // when training/evaluating a model. Live engine reads via expected.cfg
  // mismatch checks (CoreModelZoo).
  FPN<F>   held_out_fraction;        // 0.20 = 20% of data reserved for final-test
                                     // (clamped to [0.05, 0.30] in HeldOutSplit_Make)
  FPN<F>   gap_acceptable_threshold; // max acceptable |WF mean - held_out| gap
                                     // (default 0.05 — gap above this = poor generalization)
  // v5.2.0 (held-out gate Phase 1) — model attestation infrastructure.
  // Each .bin model can have a paired .stamp file with hash+signature
  // attesting that held-out validation passed. When held_out_gate_strict=1,
  // CoreModelZoo_LoadFromDir refuses to load .bin files without a valid
  // stamp. Defaults: strict=0 (gate disabled — existing models still load),
  // secret="" (which makes verify_model_stamp accept any stamp signature
  // — useful for dev). Set both for production live deploy.
  int    held_out_gate_strict;       // 0=warn-only (default), 1=refuse load, -1=skip
  // v5.9.2b — allow loading a model whose stamp's engine_version differs
  // by major version from current build (e.g. stamp says 5.x but engine
  // is 6.x). Cross-major can introduce hot-path predicate changes that
  // make a v5-trained model misbehave at v6 inference. Default 0 (refuse
  // cross-major in both strict + non-strict modes); operator opts in
  // explicitly with allow_cross_major_engine=1 + held_out_gate_strict=0.
  // Within-major loads (5.7 → 5.9) are always permitted.
  int    allow_cross_major_engine;
  char   held_out_stamp_secret[128]; // HMAC-SHA256 secret for stamp signing/verify
  // v5.3.2 — auto-stamp on held-out completion. When 1, Backtest_RunFullValidation
  // calls stamp_write_for_model after a successful held-out training pass, using
  // held_out_stamp_secret + gap_acceptable_threshold above. Default 0 to preserve
  // current behavior (manual stamping via tools/stamp_model.sh); flip to 1 after
  // configuring the secret to enable hands-off stamp generation in foxml_suite.
  int    auto_stamp_on_held_out;
  // v5.4.0 (Phase 0.1) — operational health log. Always-available
  // structured JSONL diagnostic log. Replaces ad-hoc env-gated stderr
  // traces. When enabled, engine init configures MemHeaders/HealthLog.hpp
  // with these values; subsequent Health_Log() calls append to the path.
  //   health_log_path: "" (default) = disabled; non-empty = JSONL output path
  //   health_log_level: 0 = info (always), 1 = debug, 2 = trace
  char   health_log_path[256];
  int    health_log_level;
  // v5.9.4 — health log rotation (in-process, atomic rename pattern).
  // Without rotation, long soaks (30+ days) accumulate >5GB of JSONL,
  // operator-unfriendly. Rotates via rename to <path>.<unix_ts>,
  // keeps last `keep_count` rotated files, deletes oldest.
  //   health_log_max_bytes: 0 = no rotation (default for back-compat);
  //                         >0 = rotate when current file exceeds size
  //   health_log_keep_count: 0 = keep nothing (default; only the
  //                              active file lives); N = keep last N
  //                              rotated files (typical: 7)
  uint64_t health_log_max_bytes;
  int      health_log_keep_count;
  // v5.9.4 — cross-binary version drift acknowledgment. By default,
  // engine boot WARNs (stderr) when a loaded model's stamp engine_version
  // differs from current ENGINE_VERSION_STRING by major.minor. Operators
  // who deliberately deploy a v5.8 model on a v5.9 engine (or v5.9.1 on
  // v5.9.4) flip this to 1 to suppress the WARN. The cross-major check
  // (different MAJOR) is a separate refusal gate; this only suppresses
  // the patch/minor drift WARN. See DOCS/PARITY_LIFECYCLE.md.
  int    acknowledge_cross_binary_version_drift;
  // v5.9.5i — suppress inference cfg drift WARN/REFUSE on stamp ↔ runtime
  // mismatch. Tier 1 fields (freshness_tau, confidence_threshold_scale,
  // barrier_gate_enabled) REFUSE in strict mode (held_out_gate_strict=1),
  // WARN otherwise. Tier 2 (confidence_hard_block_threshold, bandit,
  // fees) WARN only. Setting this flag = 1 silences both tiers.
  // Default 0 (vocal). Suppresses only inference cfg drift; cross-binary
  // version drift uses its own flag above.
  int    acknowledge_inference_cfg_drift;
  // v5.10.0c — hot model swap behavior when a position is open.
  //   0 (default) = swap is deferred until the position closes naturally
  //                 (next slow-path retries; safer — entry & exit use
  //                 the same model);
  //   1           = swap proceeds immediately, open positions exit on
  //                 whatever model fires next (operator opt-in for
  //                 emergency model retraction).
  // Setting this flag = 1 acknowledges that a single position may have
  // its entry and exit driven by different models, which can produce
  // unintuitive realized P&L. Default 0 keeps the entry-exit symmetry.
  int    acknowledge_hot_swap_with_open_positions;
  // v5.10.0e — runtime IC drift detection. Sampled post-fill on ML cores;
  // sustained breach (avg IC over `confidence_ic_floor_window` seconds
  // below `confidence_ic_floor`) emits CRITICAL log. When `auto_kill_on_drift=1`
  // also trips the per-core kill_switch — engine stops opening new
  // positions on that core; existing positions exit naturally.
  //
  // confidence_ic_floor       — min acceptable rolling IC. Default 0.02
  //                              (Spearman correlation; > random chance).
  // confidence_ic_floor_window — sustained-breach window in SECONDS.
  //                              Default 86400 (24 hours).
  // auto_kill_on_drift        — 0 (default) = log only; 1 = also trip
  //                              per-core kill_switch on first breach.
  double   confidence_ic_floor;
  uint32_t confidence_ic_floor_window;
  int      auto_kill_on_drift;
  // v5.2.1 (live reconciliation Phase 1) — exchange-truth sync at boot
  // (and optionally on heartbeat). LIVE-mode-only — paper mode skips
  // reconcile entirely.
  //   reconcile_interval_sec: 0 = boot-only (default, sufficient for
  //                              most cases), >0 = poll cadence in
  //                              seconds for heartbeat reconciliation
  //                              (defends against silent WS-missed fills).
  //   reconcile_dry_run:      1 = log what would change but don't apply
  //                              (recommended for first live deploy
  //                              against testnet)
  //                              0 = full reconcile (production)
  // Default dry_run=1 is intentional friction — flip to 0 deliberately.
  int    reconcile_interval_sec;
  int    reconcile_dry_run;
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
  // v4.7.39 (per-core slow-path migration): slow-path threading model
  // under sharded mode. STARTUP-ONLY. See ENGINE_ARCH_* constants above.
  uint8_t engine_arch; // ENGINE_ARCH_CENTRALIZED (default) or PER_CORE_SLOW
  // v5.0.2: slow-path CPU pin policy. STARTUP-ONLY.
  //   < 0  → do not pin slow-paths (OS-scheduled — original v5.0 behavior)
  //   == 0 → auto: pin slow-path c to (drainer_cpu + 1 + c) mod nproc.
  //          Drainer pins to (num_cores + 1) so default base is num_cores + 2.
  //          Wraps via modulo if base + num_cores > nproc.
  //   > 0  → explicit base: pin slow-path c to (offset + c) mod nproc.
  // Default 0 (auto). Slow-paths are jitter-tolerant so HT-sharing with
  // spare cores is acceptable. Set to -1 to disable (e.g. for benchmarks
  // comparing pinned vs unpinned).
  int slow_path_pin_offset;
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
  // v5.9.0c — explicit-set bitmap for core_strategies. Bit i set = `core_i_strategy=`
  // appeared in the cfg file (operator deliberate choice). Bit i clear = field
  // absent in cfg, default applied. Surfaces "default vs deliberate" tri-state
  // in TUI per V5_9_AUDIT-#5. The today-bug class: backtest.cfg lacked
  // per-core strategy lines → all cores defaulted to SIMPLE_DIP → operator
  // saw "0!" hardcoded warnings, couldn't tell defaulted from deliberate.
  uint16_t core_strategies_explicit_set;
  // v5.9.0c — captured cfg file path. ControllerConfig_Load stores the path
  // it parsed; the engine header panel displays this so operators see at
  // boot which cfg drove the configuration. Distinct binaries (engine_gui
  // reads engine.cfg, foxml_suite reads backtest.cfg) → "loaded cfg path"
  // makes the difference visible.
  char source_cfg_path[256];
  // Per-core risk allocation. core_risk_pct[i] is the fraction of total
  // balance this core can risk on a single trade. Default 0 = use the
  // shared risk_pct / num_cores. Non-zero = use this specific percentage.
  // Config syntax: core_0_risk_pct=20.0 (stored as 0.20).
  FPN<F> core_risk_pct[16];    // MAX_EXECUTION_CORES
  // Phase 3: per-core kill switch overrides + global tunables.
  // core_max_drawdown_pct[i] overrides the shared max_drawdown_pct for
  // this specific core. Default 0 = use shared. Config syntax:
  // core_0_max_drawdown_pct=15.0 (stored as 0.15).
  FPN<F> core_max_drawdown_pct[16];
  // min_kill_loss: absolute USDT floor for the per-core kill switch. The
  // trip fires only when BOTH dd_pct exceeds threshold AND drop exceeds
  // this floor. Without it, a tiny allocation ($10) loses $0.50, dd=5%,
  // and the kill trips on rounding noise. Default $5. Config syntax:
  // min_kill_loss=5.0
  FPN<F> min_kill_loss;
  // enable_mtm_kill_switch: 1 = include unrealized P&L in kill eval (mark
  // to market every slow path); 0 = realized-only (legacy behavior). MTM
  // catches "position riding down with no SL hit yet" scenarios. Default 1.
  uint32_t enable_mtm_kill_switch;
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
  // v5.10.0a.G.6 — per-core multi-horizon ensemble cfg (string-typed, can't
  // fit X-macro pattern). Default empty = inherit from global cfg.horizon_list /
  // cfg.ensemble_blend_mode. Auto-detect (G.5) takes priority over both;
  // these are overrides for operators who want explicit per-core control
  // (e.g., core 0 deploys 5-horizon ensemble while core 1 stays single-model).
  //
  //   core_0_horizon_list=100,500,1000        # CSV; per-core horizon set
  //   core_0_ensemble_blend_mode=weighted     # selection | weighted (default)
  //   core_0_disabled_horizons=100            # CSV; kill-switch per horizon
  //                                           # (skips predict; bandit weight frozen)
  char core_horizon_list[16][128];
  char core_ensemble_blend_mode[16][16];
  char core_disabled_horizons[16][128];
  // v5.11.18a — per-core feature mask. Bit i set = feature index i is
  // computed + packed for this core; bit i clear = packed as 0.0f
  // (NOT skipped; sparse-zero array contract per parity-check finding).
  // Default 0xFFFFFFFFFFFFFFFF (all features enabled) — preserves
  // pre-v5.11.18a behavior bytewise.
  //
  // Config syntax: `core_0_feature_mask=0xFFFFFFFFFFFFFFFF` (matches the
  // existing per-core field convention: core_N_strategy, core_N_risk_pct,
  // etc.). Parser accepts 0x-prefixed hex (any case) or plain decimal.
  // Stored uint64_t.
  // Parity binding (Surface G stamp body extension at v5.11.18a, ML
  // wiring at v5.11.18) ensures runtime-mask vs training-mask drift is
  // caught at model load.
  //
  // Safety: v5.11.18a ships this cfg field + stamp infra ONLY. Behavior
  // change (Features_PackAll respecting the mask) lands in v5.11.18.
  // Default-on bitmap means any cfg without `feature_mask_<N>=` lines
  // produces identical features to pre-v5.11.18a builds.
  uint64_t core_feature_mask[16];
  // Per-core full-tunable overrides (v4.0). One slot per execution core
  // (16 max). Each PerCoreOverrides field shadows a same-named field on
  // ControllerConfig — non-zero overrides global; zero inherits.
  // Resolved on every slow-path rebuild via
  // ControllerConfig_ResolveForCore. See PerCoreOverrides comment block.
  PerCoreOverrides<F> core_overrides[16];
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

  // v5.9.5h — XGBoost training hyperparams (cfg-tunable subset).
  // max_depth/learning_rate/n_estimators are operator-tunable via Train
  // Model GUI panel only (NOT cfg-bound — they're per-experiment, not
  // per-deploy). The 5 fields below were hardcoded pre-v5.9.5h; now
  // operator-tunable via cfg. Defaults match pre-v5.9.5h hardcoded
  // values bytewise. Stamp body records what trained the model
  // (Surface G `has_xgb_hyperparams` flag); engine load-WARN fires
  // when stamp's value differs from cfg's at boot.
  FPN<F>   xgb_subsample;          // row subsample per tree (0.5-1.0); default 0.8
  FPN<F>   xgb_colsample_bytree;   // column subsample per tree (0.5-1.0); default 0.8
  int      xgb_min_child_weight;   // min sum-of-weights per leaf (1-50); default 5
  int      xgb_seed;               // RNG seed for reproducible runs; default 42
  char     xgb_tree_method[16];    // hist | exact | approx | auto; default "hist"

  // v5.10.0 Item D — hardware-aware cfg. Operator-tunable thread counts
  // and RAM budgets; defaults match v5.9.5j-final behavior bytewise so
  // upgrades don't silently flip defaults. Operator opts in to multi-thread
  // training / parallel CSV / larger budgets.
  //
  // Thread counts (default=1 matches current hardcoded behavior at
  // BacktestEngine.hpp:1352, 1638). Setting >1 breaks bytewise reproducibility;
  // boot-time WARN fires when operator sets >1 to make the tradeoff explicit.
  int      xgb_train_nthread;      // XGBoost Train Model worker nthread; default 4 (matches pre-v5.10 BacktestPanels.hpp hardcoded)
  int      xgb_eval_nthread;       // XGBoost WF/HeldOut eval nthread; default 1 (deterministic per-fold)
  int      csv_load_workers;       // parallel CSV worker threads (Item C); default 1 (serial)

  // RAM budgets (advisory soft caps — emit WARN at boot if dataset projects
  // to exceed; no hard refuse since the streaming label compute closes
  // OOM regardless). Operator hint when sizing the box.
  int      feature_collect_max_gb; // soft cap on feature_matrix RAM; default 12
  int      wf_split_max_gb;        // soft cap on WF split RAM; default 8
  int      held_out_max_gb;        // soft cap on held-out RAM; default 4

  // v5.10.0a Item #4 — multi-horizon training. Comma-separated list of
  // forward-tick horizons; Train Model worker iterates and trains one
  // model per horizon. Empty (default) = single-horizon (uses
  // label_forward_ticks from per-run state). Cfg field rather than
  // RunConfig because operator typically standardizes horizons across
  // training experiments.
  // Example: horizon_list=100,500,1000,5000 → 4 trainings, saved as
  // <model_dir>/horizon_100/<role>.json etc.
  // LITE in v5.10.0a: trains + saves N models; operator manually picks
  // one to deploy. Ensemble inference (load all N at runtime, blend
  // predictions) deferred to v5.10.0a.x — needs stamp body extension +
  // multi-model load in CoreModelZoo, both genuinely complex.
  static constexpr int HORIZON_LIST_MAX = 8;
  int      horizon_list[HORIZON_LIST_MAX];  // 0 = unused slot
  int      horizon_count;                    // number of populated slots

  // v5.10.0a.G.6 — global ensemble cfg (per-core overrides via
  // core_N_ensemble_blend_mode / core_N_horizon_list / core_N_disabled_horizons).
  // Used when ensemble auto-detect (G.5) finds horizon siblings on disk
  // OR operator explicitly populates horizon_list.
  //
  //   ensemble_blend_mode    "weighted" (default) | "selection"
  //                          weighted = G.7 Bandit-Exp3 per-regime weighted blend
  //                          selection = G.4 argmax-confidence (single horizon per tick)
  //   ensemble_bandit_eta    Bandit learning rate (0.01 .. 1.0); higher = faster
  //                          adaptation but more variance. Default 0.1 conservative.
  //   ensemble_min_warmup_predictions  predictions per regime before weights trusted
  //                                    (uniform during warmup). Default 100.
  //   ensemble_min_agreement_pct       safety: ≥X fraction of non-disabled horizons
  //                                    must predict same direction OR skip entry.
  //                                    Default 0.6 (60% agreement). Set 0 to disable.
  char     ensemble_blend_mode[16];
  double   ensemble_bandit_eta;
  int      ensemble_min_warmup_predictions;
  double   ensemble_min_agreement_pct;
  // v5.10.0a.G.8 — trade-close reward multiplier. Real money signal
  // (TP/SL hit) gets weighted ×N over the slow-path lookback rewards
  // (which are hypothetical "would have been correct" signals).
  // Default 4.0 — operator-tunable. Higher = trust trade outcomes more
  // (faster convergence to deployable weights); lower = faster cold-
  // start learning from dense lookback signals.
  double   ensemble_trade_reward_mult;
  // v5.10.0a.G.9 — bandit state persistence cadence. Every N total
  // bandit updates (across all regimes), flush bandit_state.json to
  // <model_dir>/bandit_state.json. Default 5000 — with ~1 update per
  // poll_interval ticks, that's roughly 1 save per 500K ticks (≈ 1
  // save/hour at typical tick rates). Set to 0 to disable periodic
  // saves (state still saved on engine clean shutdown).
  int      ensemble_bandit_save_interval;
};

//======================================================================================================
// [FEE_COMPUTE — Phase 8 maker/taker helper]
//======================================================================================================
// Apply the correct fee rate based on whether the fill was a maker or taker.
// Single source of truth for fee math on a per-fill basis.
//
// Caller-side discipline (CLAUDE.md "Maker/Taker Accuracy" invariant in c7):
//   - ENTRY fees: pass order->is_maker from the matching fill
//   - EXIT fees from market sells (TP/SL hits): pass is_maker=0 (always taker)
//   - EXIT fees from limit sells (Phase 9 hybrid execution, deferred): pass
//     order->is_maker from the matching exit fill
//
// Backtest path: pass is_maker=0 always — backtest simulates as all-taker
// (documented divergence; backtest maker simulation is Phase 9 work).
//
// In legacy cfg mode (only fee_rate set, mirrored to maker+taker), both
// branches return identical values → behavior matches pre-Phase-8.
template <unsigned F>
inline FPN<F> Fee_Compute(const ControllerConfig<F>* cfg, FPN<F> notional, int is_maker) {
    FPN<F> rate = is_maker ? cfg->fee_rate_maker : cfg->fee_rate_taker;
    return FPN_Mul(notional, rate);
}

//======================================================================================================
// [PER-CORE CFG RESOLVE — v4.0]
//======================================================================================================
// Build a stack-local copy of the global cfg with per-core overrides applied.
// Strategies receive the resolved config and don't need to know about the
// override mechanism. Cost: one ~12.5KB struct copy + 18 conditionals per
// rebuild. With 4 cores at 5Hz slow path = 20 copies/sec, ~250KB/sec — well
// inside any reasonable budget.
//
// Zero in any override field means "inherit global". This matches the
// existing strategy-type-override convention (mr_tp_pct=0 → use take_profit_pct).
//======================================================================================================
template <unsigned F>
inline ControllerConfig<F> ControllerConfig_ResolveForCore(
    const ControllerConfig<F>& global, int core_id) {
    ControllerConfig<F> resolved = global;
    if (core_id < 0 || core_id >= 16) return resolved;
    const PerCoreOverrides<F>& ov = global.core_overrides[core_id];
    // v4.7.24: resolver auto-derives from PER_CORE_OVERRIDE_FIELDS. Adding
    // a new field = 1 line in the macro list, not a line here.
#define _RESOLVE_OV_FIELD(name) if (!FPN_IsZero(ov.name)) resolved.name = ov.name;
    PER_CORE_OVERRIDE_FIELDS(_RESOLVE_OV_FIELD, _RESOLVE_OV_FIELD)
#undef _RESOLVE_OV_FIELD
// v4.7.40: INT overrides — 0 = inherit (caller's config field already
// has the global default; non-zero overrides it).
#define _RESOLVE_OV_INT_FIELD(name) if (ov.name != 0) resolved.name = ov.name;
    PER_CORE_OVERRIDE_INT_FIELDS(_RESOLVE_OV_INT_FIELD)
#undef _RESOLVE_OV_INT_FIELD
    return resolved;
}

//======================================================================================================
template <unsigned F> inline ControllerConfig<F> ControllerConfig_Default() {
  ControllerConfig<F> cfg;
  cfg.poll_interval = 100;
  cfg.warmup_ticks = 128; // minimum raw ticks before trading
  cfg.min_warmup_samples =
      0; // min slow-path samples in rolling window (0 = warmup_ticks only)
  cfg.csv_sort_check_mode = CSV_SORT_WARN; // v5.9.2c — default: log + proceed
  cfg.r2_threshold = FPN_FromDouble<F>(0.30);
  cfg.slope_scale_buy = FPN_FromDouble<F>(0.50);
  cfg.max_shift =
      FPN_FromDouble<F>(0.0001); // 0.01% of price — e.g. $7 at BTC $70k
  cfg.take_profit_pct = FPN_FromDouble<F>(0.03);
  cfg.stop_loss_pct = FPN_FromDouble<F>(0.015);
  cfg.starting_balance =
      FPN_FromDouble<F>(1000000.0); // 1M default so tests arent balance-limited
  cfg.fee_rate = FPN_FromDouble<F>(0.001); // 0.1% per trade (Binance default)
  // Phase 8 — Binance tier 0 BNB-discount default rates. Live engine uses
  // these per-fill based on order->is_maker. If user sets only fee_rate
  // (legacy mode), backward-compat clause below mirrors to both.
  cfg.fee_rate_maker = FPN_FromDouble<F>(0.00075); // 0.075% maker tier 0
  cfg.fee_rate_taker = FPN_FromDouble<F>(0.00100); // 0.100% taker tier 0
  cfg.pay_fees_in_bnb = 0;                          // v4.3.2: 1 = apply BNB 25% discount
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
  // v5.7.5 — MOM quality filters default 0 (off, preserves pre-v5.7 behavior)
  cfg.momentum_min_tp_margin_pct    = FPN_Zero<F>();
  cfg.momentum_min_buy_delta_recent = FPN_Zero<F>();
  cfg.momentum_min_r2               = FPN_Zero<F>();
  cfg.momentum_require_last_win     = 0;
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
  cfg.acknowledge_hardcoded_strategy_in_live = 0;  // v5.7.2 — operator
                                                    // must set to 1 to
                                                    // run hardcoded
                                                    // strategies live
  cfg.require_mlockall = 1;  // v5.11.3 — HFT-correct default; set to 0
                              // for laptop dev where RLIMIT_MEMLOCK is tight
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
  // v5.9.1 — hard-block floor. 0.0 = disabled (pre-v5.9.1 behavior).
  // Operator opts in (audit-recommended 0.05) for the noise-floor protection.
  cfg.confidence_hard_block_threshold = FPN_FromDouble<F>(0.0);
  // Phase 7 prep — held-out validation defaults
  cfg.held_out_fraction           = FPN_FromDouble<F>(0.20);     // 20% reserved
  cfg.gap_acceptable_threshold    = FPN_FromDouble<F>(0.05);     // 5% max gap for "OK"
  cfg.held_out_gate_strict        = 0;                            // gate off by default (warn-only)
  cfg.allow_cross_major_engine    = 0;                            // v5.9.2b — refuse cross-major by default
  cfg.held_out_stamp_secret[0]    = '\0';                         // empty = accept-any (dev)
  cfg.auto_stamp_on_held_out      = 1;                            // v5.8.10: default 1 (suite Run Full Validation auto-stamps); set 0 only for manual tools/stamp_model.sh workflow
  cfg.health_log_path[0]          = '\0';                         // empty = disabled
  cfg.health_log_max_bytes        = 0;                            // 0 = no rotation (back-compat)
  cfg.health_log_keep_count       = 0;                            // 0 = no retained rotated files
  cfg.acknowledge_cross_binary_version_drift = 0;                 // v5.9.4 — default WARN on minor drift
  cfg.acknowledge_inference_cfg_drift = 0;                        // v5.9.5i — default REFUSE/WARN on inference cfg drift
  cfg.acknowledge_hot_swap_with_open_positions = 0;               // v5.10.0c — default DEFER swap until position close
  cfg.confidence_ic_floor                       = 0.02;           // v5.10.0e — Spearman correlation > random
  cfg.confidence_ic_floor_window                = 86400u;          // v5.10.0e — 24h sustained-breach window
  cfg.auto_kill_on_drift                        = 0;              // v5.10.0e — log only by default; opt-in to auto-kill
  // v5.9.5h — XGBoost training hyperparams (cfg-tunable subset).
  // Defaults match pre-v5.9.5h hardcoded values bytewise; non-tuning
  // operators get identical training output post-upgrade.
  cfg.xgb_subsample           = FPN_FromDouble<F>(0.8);
  cfg.xgb_colsample_bytree    = FPN_FromDouble<F>(0.8);
  cfg.xgb_min_child_weight    = 5;
  cfg.xgb_seed                = 42;
  strncpy(cfg.xgb_tree_method, "hist", sizeof(cfg.xgb_tree_method) - 1);
  cfg.xgb_tree_method[sizeof(cfg.xgb_tree_method) - 1] = '\0';
  // v5.10.0 Item D — hardware-aware cfg. Defaults match pre-v5.10
  // hardcoded behavior. Two distinct defaults reflect two distinct
  // pre-v5.10 hardcoded sites:
  //   - Train Model worker (BacktestPanels.hpp:2056) was nthread=4 for
  //     faster GUI iter (exploratory; reproducibility not required)
  //   - WF + HeldOut (BacktestEngine.hpp:1352, 1638) were nthread=1
  //     for deterministic per-fold output (validation parity)
  // Setting these NOW separable. Operators wanting all-deterministic
  // workflow set both to 1; operators with bigger boxes can bump both.
  cfg.xgb_train_nthread       = 4;   // matches BacktestPanels.hpp:2056 pre-v5.10
  cfg.xgb_eval_nthread        = 1;   // matches BacktestEngine.hpp:1352, 1638
  cfg.csv_load_workers        = 1;   // serial CSV load (matches pre-v5.10 behavior)
  cfg.feature_collect_max_gb  = 12;  // advisory cap; WARN-only
  cfg.wf_split_max_gb         = 8;
  cfg.held_out_max_gb         = 4;
  // v5.10.0a — multi-horizon training. Default empty = single-horizon
  // (Train Model uses TrainingPanel's label_forward_ticks). Operator opts
  // in by setting cfg.horizon_list=100,500,1000.
  for (int i = 0; i < ControllerConfig<F>::HORIZON_LIST_MAX; ++i)
      cfg.horizon_list[i] = 0;
  cfg.horizon_count = 0;
  // v5.10.0a.G.6 — ensemble cfg defaults. blend_mode "weighted" engages
  // G.7 Bandit-Exp3 path when ensemble active; "selection" stays on G.4
  // argmax-confidence. Other defaults are conservative (low eta, modest
  // warmup, 60% agreement gate).
  strncpy(cfg.ensemble_blend_mode, "weighted",
          sizeof(cfg.ensemble_blend_mode) - 1);
  cfg.ensemble_blend_mode[sizeof(cfg.ensemble_blend_mode) - 1] = '\0';
  cfg.ensemble_bandit_eta = 0.1;
  cfg.ensemble_min_warmup_predictions = 100;
  cfg.ensemble_min_agreement_pct = 0.6;
  cfg.ensemble_trade_reward_mult = 4.0;
  cfg.ensemble_bandit_save_interval = 5000;  // v5.10.0a.G.9
  // v5.10.0a.G.6 — per-core ensemble cfg defaults (empty = inherit global)
  // v5.11.18a — per-core feature_mask defaults (all-bits-on = no masking)
  for (int i = 0; i < 16; ++i) {
      cfg.core_horizon_list[i][0] = '\0';
      cfg.core_ensemble_blend_mode[i][0] = '\0';
      cfg.core_disabled_horizons[i][0] = '\0';
      cfg.core_feature_mask[i] = 0xFFFFFFFFFFFFFFFFULL;  // all features enabled
  }
  cfg.health_log_level            = 0;                            // 0=info, 1=debug, 2=trace
  cfg.reconcile_interval_sec      = 0;                            // 0 = boot-only
  cfg.reconcile_dry_run           = 1;                            // safer default; flip to 0 deliberately
  cfg.prediction_normalize = 0;
  cfg.barrier_gate_enabled = 0;
  cfg.model_verify_strict = 0;  // 0=warn, 1=strict (fail on mismatch), -1=skip
  cfg.peak_model_path[0] = '\0';
  cfg.valley_model_path[0] = '\0';
  // Per-core sharding (Phase 13+) — DEFAULT IS SHARDED. Sharded is the
  // production engine: per-core ExecutionCore + per-core PortfolioController
  // + central OMS, branchless ~60ns hot path, risk distributed across cores.
  // ENGINE_MODE_SINGLE_CORE remains available for benchmark/regression
  // baselines but is DEPRECATED and emits a runtime warning at startup.
  // Adding new features in legacy-only paths = silent production gap;
  // see CLAUDE.md "Cross-Mode Init Placement" invariant.
  cfg.engine_mode = ENGINE_MODE_SHARDED;
  // v5.0.0 (Phase F): per_core_slow is the new default. Each engine = a
  // self-contained strategy unit (slow + hot pthread pair). Centralized
  // architecture available as opt-out via engine_arch=centralized for
  // benchmark / regression / legacy use. Train-serve parity preserved
  // structurally: all 3 callers (centralized live, per_core_slow live,
  // backtest) execute the same OneCore helpers on the same state.cores[c].
  cfg.engine_arch = ENGINE_ARCH_PER_CORE_SLOW;
  cfg.slow_path_pin_offset = 0;  // 0 = auto-derive (drainer_cpu + 1)
  cfg.num_execution_cores = 4;
  cfg.sharded_force_synthetic = 0;
  for (int i = 0; i < 16; ++i) cfg.core_strategies[i] = 2;  // STRATEGY_SIMPLE_DIP
  cfg.core_strategies_explicit_set = 0;                       // v5.9.0c: no bits set = all defaulted
  cfg.source_cfg_path[0] = '\0';                              // v5.9.0c: populated by ControllerConfig_Load
  for (int i = 0; i < 16; ++i) cfg.core_risk_pct[i] = FPN_Zero<F>();  // 0 = shared
  // Phase 3: per-core kill switch overrides default to 0 (= use shared).
  for (int i = 0; i < 16; ++i) cfg.core_max_drawdown_pct[i] = FPN_Zero<F>();
  cfg.min_kill_loss = FPN_FromDouble<F>(5.0);   // $5 absolute-loss floor for trip
  cfg.enable_mtm_kill_switch = 1;                // mark-to-market enabled by default
  for (int i = 0; i < 16; ++i) cfg.core_model_path[i][0] = '\0';    // empty = shared
  for (int i = 0; i < 16; ++i) cfg.core_model_dir[i][0] = '\0';     // empty = use model_path or shared
  // v4.0 per-core overrides — zero in every field = "inherit global".
  // v4.7.24: zeroing auto-derives from PER_CORE_OVERRIDE_FIELDS macro.
  for (int i = 0; i < 16; ++i) {
#define _ZERO_OV_FIELD(name) cfg.core_overrides[i].name = FPN_Zero<F>();
    PER_CORE_OVERRIDE_FIELDS(_ZERO_OV_FIELD, _ZERO_OV_FIELD)
#undef _ZERO_OV_FIELD
// v4.7.40: zero INT overrides too (0 = inherit).
#define _ZERO_OV_INT_FIELD(name) cfg.core_overrides[i].name = 0;
    PER_CORE_OVERRIDE_INT_FIELDS(_ZERO_OV_INT_FIELD)
#undef _ZERO_OV_INT_FIELD
  }
  cfg.simpledip_tp_pct  = FPN_Zero<F>();  // 0 = use shared take_profit_pct
  cfg.simpledip_sl_pct  = FPN_Zero<F>();
  cfg.mr_tp_pct         = FPN_Zero<F>();
  cfg.mr_sl_pct         = FPN_Zero<F>();
  cfg.emacross_tp_pct   = FPN_Zero<F>();
  cfg.emacross_sl_pct   = FPN_Zero<F>();
  // OMS phase 03 — mode 1: OMS owns portfolio mutation + per-core
  // accounting (via FillRecord drained post-Tick). Required for partials
  // (mode 0 used event.core_id directly as portfolio slot, which breaks
  // when slot != core_id under paired-leg geometry). Mode 0 left in
  // place for tests that explicitly want the legacy OnEvent path; new
  // production code paths default to 1.
  cfg.oms_event_log_mode = 1;
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

  // v5.9.0c — capture the cfg path so the engine header panel can display
  // it. Operators distinguish engine.cfg vs backtest.cfg load at-a-glance.
  if (filepath) {
    size_t n = strlen(filepath);
    if (n >= sizeof(cfg.source_cfg_path)) n = sizeof(cfg.source_cfg_path) - 1;
    memcpy(cfg.source_cfg_path, filepath, n);
    cfg.source_cfg_path[n] = '\0';
  }

  // Phase 8: track whether the user explicitly set maker/taker rates in
  // the cfg file. Can't infer from value comparison alone — explicit
  // values matching defaults would falsely trigger legacy-mirroring.
  int maker_explicitly_set = 0;
  int taker_explicitly_set = 0;

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
    CFG_PARSE_FPN(momentum_min_tp_margin_pct)     // v5.7.5
    CFG_PARSE_FPN(momentum_min_buy_delta_recent)  // v5.7.5
    CFG_PARSE_FPN(momentum_min_r2)                // v5.7.5
    CFG_PARSE_INT(momentum_require_last_win)      // v5.7.5
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
    // Phase 8: track explicit-set for the post-parse legacy-mirror decision.
    // Inline parse instead of CFG_PARSE_PCT macro (which would `continue;`
    // before setting the flag). Same divide-by-100 semantics.
    if (strcmp(key, "fee_rate_maker") == 0) {
        cfg.fee_rate_maker = FPN_FromDouble<F>(atof(val) / 100.0);
        maker_explicitly_set = 1;
        continue;
    }
    if (strcmp(key, "fee_rate_taker") == 0) {
        cfg.fee_rate_taker = FPN_FromDouble<F>(atof(val) / 100.0);
        taker_explicitly_set = 1;
        continue;
    }
    CFG_PARSE_PCT(risk_pct)
    CFG_PARSE_PCT(entry_offset_pct)
    CFG_PARSE_PCT(offset_min)
    CFG_PARSE_PCT(offset_max)
    CFG_PARSE_PCT(max_drawdown_pct)
    // Phase 3: kill switch tunables
    CFG_PARSE_FPN_POS(min_kill_loss)
    CFG_PARSE_U32(enable_mtm_kill_switch)
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
    CFG_PARSE_U32(pay_fees_in_bnb)  // v4.3.2 Track C.1 — Binance BNB 25% fee discount
    CFG_PARSE_U32(warmup_ticks)
    CFG_PARSE_U32(slow_path_max_secs)
    CFG_PARSE_U32(max_hold_ticks)
    CFG_PARSE_U32(regime_hysteresis)
    CFG_PARSE_U32(min_warmup_samples)
    CFG_PARSE_INT(csv_sort_check_mode)
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
    CFG_PARSE_INT(acknowledge_hardcoded_strategy_in_live)  // v5.7.2
    CFG_PARSE_INT(require_mlockall)  // v5.11.3
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
    // v5.9.1 (V5_9_AUDIT-#13) — tau<=0 silently fell back to default in
    // ConfidenceScorer_Init. Reject at parse time so the operator sees
    // the bad value before it's silently overridden during boot.
    if (strcmp(key, "confidence_freshness_tau") == 0) {
        double v = atof(val);
        // v5.9.1 (V5_9_AUDIT-#13) — reject tau<=0 (silent default fallback).
        // v5.9.2b — also clamp to [60.0, 3600.0] (1 minute to 1 hour).
        // Out-of-range values silently degraded the confidence damping
        // contract: tau=1e6 effectively disabled freshness decay; tau=10
        // produced near-zero confidence after just a few seconds. Both
        // are pathological. Range matches the model lifetime envelope.
        if (v <= 0.0 || v < 60.0 || v > 3600.0) {
            fprintf(stderr,
                "[cfg] confidence_freshness_tau=%.3f out of range [60.0, 3600.0]; "
                "using default 300.0. See DOCS/CLAUDE_INTEGRATION.md.\n",
                v);
            continue;
        }
        cfg.confidence_freshness_tau = FPN_FromDouble<F>(v);
        continue;
    }
    CFG_PARSE_FPN(confidence_threshold_scale)
    CFG_PARSE_FPN_POS(confidence_hard_block_threshold)
    CFG_PARSE_FPN(held_out_fraction)
    CFG_PARSE_FPN(gap_acceptable_threshold)
    CFG_PARSE_INT(allow_cross_major_engine)
    // v5.9.5h — XGBoost hyperparam parsers
    CFG_PARSE_FPN_POS(xgb_subsample)
    CFG_PARSE_FPN_POS(xgb_colsample_bytree)
    CFG_PARSE_INT(xgb_min_child_weight)
    CFG_PARSE_INT(xgb_seed)
    if (strcmp(key, "xgb_tree_method") == 0) {
        size_t n = strlen(val);
        if (n >= sizeof(cfg.xgb_tree_method)) n = sizeof(cfg.xgb_tree_method) - 1;
        memcpy(cfg.xgb_tree_method, val, n);
        cfg.xgb_tree_method[n] = '\0';
        continue;
    }
    // v5.10.0 Item D — hardware-aware cfg parsers. CFG_PARSE_INT clamps
    // negatives to defaults; we want >=0 (0 = auto-detect via nproc, NOT
    // currently implemented; reserved for future). Setting nthread or
    // workers > 1 emits a one-shot WARN at boot (handled in engine boot
    // path, not parser).
    CFG_PARSE_INT(xgb_train_nthread)
    CFG_PARSE_INT(xgb_eval_nthread)
    CFG_PARSE_INT(csv_load_workers)
    CFG_PARSE_INT(feature_collect_max_gb)
    CFG_PARSE_INT(wf_split_max_gb)
    CFG_PARSE_INT(held_out_max_gb)
    // v5.10.0a.G.6 — global ensemble cfg parsers (string + numeric).
    if (strcmp(key, "ensemble_blend_mode") == 0) {
        // Validate against known modes; reject unknown with WARN.
        if (strcmp(val, "weighted") == 0 || strcmp(val, "selection") == 0) {
            strncpy(cfg.ensemble_blend_mode, val,
                    sizeof(cfg.ensemble_blend_mode) - 1);
            cfg.ensemble_blend_mode[sizeof(cfg.ensemble_blend_mode) - 1] = '\0';
        } else {
            fprintf(stderr, "[cfg] ensemble_blend_mode='%s' unknown; "
                    "valid: weighted|selection. Keeping default '%s'.\n",
                    val, cfg.ensemble_blend_mode);
        }
        continue;
    }
    if (strcmp(key, "ensemble_bandit_eta") == 0) {
        double v = atof(val);
        // Clamp to safe range; out-of-range silently produces uninformative
        // bandits (eta=0 = no learning; eta>1 = unstable).
        if (v < 0.01) v = 0.01;
        if (v > 1.0)  v = 1.0;
        cfg.ensemble_bandit_eta = v;
        continue;
    }
    CFG_PARSE_INT(ensemble_min_warmup_predictions)
    if (strcmp(key, "ensemble_min_agreement_pct") == 0) {
        double v = atof(val);
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        cfg.ensemble_min_agreement_pct = v;
        continue;
    }
    if (strcmp(key, "ensemble_trade_reward_mult") == 0) {
        double v = atof(val);
        // Clamp to sane range; 0 disables trade-close rewards entirely
        // (only slow-path lookback feeds bandit). Upper bound prevents
        // a single trade dominating thousands of slow-path signals.
        if (v < 0.0) v = 0.0;
        if (v > 100.0) v = 100.0;
        cfg.ensemble_trade_reward_mult = v;
        continue;
    }
    if (strcmp(key, "ensemble_bandit_save_interval") == 0) {
        int v = atoi(val);
        // 0 = disable periodic saves (still saves on shutdown); negative
        // → clamp to 0. Upper bound prevents accidental every-tick saves.
        if (v < 0) v = 0;
        if (v > 10000000) v = 10000000;
        cfg.ensemble_bandit_save_interval = v;
        continue;
    }
    // v5.10.0a — horizon_list CSV parser. Comma-separated ints, max
    // HORIZON_LIST_MAX entries. Caller can't use CFG_PARSE_INT (single
    // int) or CFG_PARSE_FPN. Custom branch.
    if (strcmp(key, "horizon_list") == 0) {
        int n = 0;
        const char* p = val;
        while (*p && n < ControllerConfig<F>::HORIZON_LIST_MAX) {
            // skip whitespace + commas
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (!*p) break;
            char* end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p) break;  // parse failure
            if (v > 0 && v <= 1000000)  // sanity: 1 to 1M ticks
                cfg.horizon_list[n++] = (int)v;
            p = end;
        }
        cfg.horizon_count = n;
        if (n == 0) {
            fprintf(stderr, "[cfg] horizon_list='%s' parsed 0 valid horizons; "
                    "expected CSV like '100,500,1000'. Multi-horizon disabled.\n",
                    val);
        }
        continue;
    }
    if (strcmp(key, "held_out_gate_strict") == 0) {
        cfg.held_out_gate_strict = atoi(val);
        continue;
    }
    if (strcmp(key, "held_out_stamp_secret") == 0) {
        size_t n = strlen(val);
        if (n >= sizeof(cfg.held_out_stamp_secret)) n = sizeof(cfg.held_out_stamp_secret) - 1;
        memcpy(cfg.held_out_stamp_secret, val, n);
        cfg.held_out_stamp_secret[n] = '\0';
        continue;
    }
    if (strcmp(key, "auto_stamp_on_held_out") == 0) {
        cfg.auto_stamp_on_held_out = atoi(val);
        continue;
    }
    if (strcmp(key, "health_log_path") == 0) {
        size_t n = strlen(val);
        if (n >= sizeof(cfg.health_log_path)) n = sizeof(cfg.health_log_path) - 1;
        memcpy(cfg.health_log_path, val, n);
        cfg.health_log_path[n] = '\0';
        continue;
    }
    if (strcmp(key, "health_log_level") == 0) {
        cfg.health_log_level = atoi(val);
        continue;
    }
    if (strcmp(key, "health_log_max_bytes") == 0) {
        cfg.health_log_max_bytes = (uint64_t)strtoull(val, nullptr, 10);
        continue;
    }
    if (strcmp(key, "health_log_keep_count") == 0) {
        cfg.health_log_keep_count = atoi(val);
        if (cfg.health_log_keep_count < 0) cfg.health_log_keep_count = 0;
        continue;
    }
    CFG_PARSE_INT(acknowledge_cross_binary_version_drift)
    CFG_PARSE_INT(acknowledge_inference_cfg_drift)  // v5.9.5i
    CFG_PARSE_INT(acknowledge_hot_swap_with_open_positions)  // v5.10.0c
    if (strcmp(key, "confidence_ic_floor") == 0) {
        cfg.confidence_ic_floor = atof(val);
        continue;
    }
    if (strcmp(key, "confidence_ic_floor_window") == 0) {
        cfg.confidence_ic_floor_window = (uint32_t)strtoul(val, nullptr, 10);
        continue;
    }
    CFG_PARSE_INT(auto_kill_on_drift)  // v5.10.0e
    if (strcmp(key, "reconcile_interval_sec") == 0) {
        cfg.reconcile_interval_sec = atoi(val);
        continue;
    }
    if (strcmp(key, "reconcile_dry_run") == 0) {
        cfg.reconcile_dry_run = atoi(val);
        continue;
    }
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
    // v4.7.39: engine_arch — slow-path threading model (sharded only).
    // Accepts string forms "centralized"/"per_core_slow" or int "0"/"1".
    if (strcmp(key, "engine_arch") == 0) {
      if (strcmp(val, "per_core_slow") == 0 || strcmp(val, "1") == 0)
        cfg.engine_arch = ENGINE_ARCH_PER_CORE_SLOW;
      else
        cfg.engine_arch = ENGINE_ARCH_CENTRALIZED;
      continue;
    }
    // v5.0.2: slow_path_pin_offset — int. <0 disables, 0 auto, >0 explicit.
    if (strcmp(key, "slow_path_pin_offset") == 0) {
      cfg.slow_path_pin_offset = atoi(val);
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
    // Phase 3: per-core kill switch override. core_0_max_drawdown_pct=15.0
    // means core 0 trips at 15% drawdown (overrides shared max_drawdown_pct).
    // Match must come before _max checks (substring "_max" is in
    // "_max_drawdown_pct"). Specific suffix match keeps it safe.
    if (strncmp(key, "core_", 5) == 0 && strstr(key, "_max_drawdown_pct")) {
      int core_idx = atoi(key + 5);
      if (core_idx >= 0 && core_idx < 16) {
        cfg.core_max_drawdown_pct[core_idx] = FPN_FromDouble<F>(atof(val) / 100.0);
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
        else if (strcmp(val, "auto") == 0 || strcmp(val, "regime") == 0) sid = 5;  // v4.0.3 STRATEGY_AUTO
        else if (strcmp(val, "none") == 0) sid = 0xFF;
        else sid = (uint8_t)atoi(val);  // numeric fallback
        cfg.core_strategies[core_idx] = sid;
        // v5.9.0c — set explicit bit so TUI can distinguish deliberate from defaulted.
        cfg.core_strategies_explicit_set |= (uint16_t)(1u << core_idx);
      }
      continue;
    }
    // Per-core overrides (v4.0). Parses `core_N_<field>=<value>` for any
    // PerCoreOverrides field. Empty/0 = inherit global; resolver handles
    // the fallback. Two categories — pct (atof/100, e.g. take_profit_pct)
    // and raw FPN (atof, e.g. ml_buy_threshold).
    //
    // v4.7.24: parser auto-derives from PER_CORE_OVERRIDE_FIELDS macro.
    // Adding a new override field = ONE line in the macro list near the
    // top of this header. The struct, init, resolver, and parser all
    // pick up the new field automatically.
    if (strncmp(key, "core_", 5) == 0) {
      int core_idx = -1;
      const char* suffix = nullptr;
      // parse core_N_<suffix>
      const char* p = key + 5;
      core_idx = atoi(p);
      while (*p && *p != '_') p++;
      if (*p == '_' && core_idx >= 0 && core_idx < 16) {
        suffix = p + 1;
        PerCoreOverrides<F>& ov = cfg.core_overrides[core_idx];
#define _PARSE_OV_PCT(name) if (strcmp(suffix, #name) == 0) { ov.name = FPN_FromDouble<F>(atof(val)/100.0); continue; }
#define _PARSE_OV_RAW(name) if (strcmp(suffix, #name) == 0) { ov.name = FPN_FromDouble<F>(atof(val));       continue; }
        PER_CORE_OVERRIDE_FIELDS(_PARSE_OV_PCT, _PARSE_OV_RAW)
#undef _PARSE_OV_PCT
#undef _PARSE_OV_RAW
// v4.7.40: INT overrides — atoi parse, 0 = inherit.
#define _PARSE_OV_INT(name) if (strcmp(suffix, #name) == 0) { ov.name = (uint32_t)atoi(val); continue; }
        PER_CORE_OVERRIDE_INT_FIELDS(_PARSE_OV_INT)
#undef _PARSE_OV_INT
        // v5.10.0a.G.6 — string-typed per-core ensemble fields. X-macro
        // doesn't support string types; explicit branches here. All three
        // default empty (inherit global).
        if (strcmp(suffix, "horizon_list") == 0) {
            strncpy(cfg.core_horizon_list[core_idx], val,
                    sizeof(cfg.core_horizon_list[core_idx]) - 1);
            cfg.core_horizon_list[core_idx][
                sizeof(cfg.core_horizon_list[core_idx]) - 1] = '\0';
            continue;
        }
        if (strcmp(suffix, "ensemble_blend_mode") == 0) {
            if (strcmp(val, "weighted") == 0 || strcmp(val, "selection") == 0) {
                strncpy(cfg.core_ensemble_blend_mode[core_idx], val,
                        sizeof(cfg.core_ensemble_blend_mode[core_idx]) - 1);
                cfg.core_ensemble_blend_mode[core_idx][
                    sizeof(cfg.core_ensemble_blend_mode[core_idx]) - 1] = '\0';
            } else {
                fprintf(stderr, "[cfg] core_%d_ensemble_blend_mode='%s' unknown; "
                        "valid: weighted|selection. Falling back to global.\n",
                        core_idx, val);
            }
            continue;
        }
        if (strcmp(suffix, "disabled_horizons") == 0) {
            strncpy(cfg.core_disabled_horizons[core_idx], val,
                    sizeof(cfg.core_disabled_horizons[core_idx]) - 1);
            cfg.core_disabled_horizons[core_idx][
                sizeof(cfg.core_disabled_horizons[core_idx]) - 1] = '\0';
            continue;
        }
        // v5.11.18a — per-core feature_mask (uint64_t, hex or decimal).
        // Accepts `0xDEADBEEFCAFEBABE` (any case), `0XDEADBEEF`, or plain
        // decimal `18446744073709551615`. Out-of-range / unparseable
        // values fall back to default 0xFFFF..F (all features enabled)
        // with a WARN — never silently zero-out the mask, which would
        // disable ALL features for that core.
        if (strcmp(suffix, "feature_mask") == 0) {
            char* end = nullptr;
            errno = 0;
            uint64_t parsed;
            if ((val[0] == '0' && (val[1] == 'x' || val[1] == 'X'))) {
                parsed = strtoull(val + 2, &end, 16);
            } else {
                parsed = strtoull(val, &end, 10);
            }
            if (end == val || (end != nullptr && *end != '\0' && *end != '\n')
                    || errno == ERANGE) {
                fprintf(stderr, "[cfg] core_%d_feature_mask='%s' unparseable; "
                        "expected 0xHEX or decimal. Falling back to all-on "
                        "(0xFFFFFFFFFFFFFFFF).\n", core_idx, val);
                cfg.core_feature_mask[core_idx] = 0xFFFFFFFFFFFFFFFFULL;
            } else {
                cfg.core_feature_mask[core_idx] = parsed;
                if (parsed == 0ULL) {
                    fprintf(stderr, "[cfg] WARN: core_%d_feature_mask=0x0 "
                            "disables ALL features for this core. Did you "
                            "mean 0xFFFFFFFFFFFFFFFF (all enabled)?\n",
                            core_idx);
                }
            }
            continue;
        }
      }
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

  // v5.9.0c — surface "all per-core strategies defaulted" silent failure.
  // V5_9_AUDIT-#5. The today-bug: backtest.cfg lacked core_N_strategy=
  // lines → all 16 cores fell to STRATEGY_SIMPLE_DIP default → operator
  // saw "0!" hardcoded warnings, couldn't distinguish defaulted from
  // deliberate. Stderr WARN at boot makes the silent fallback visible.
  //
  // Fires only when num_execution_cores > 0 (we have actual cores) AND
  // explicit_set bitmap is zero (no core_N_strategy= lines parsed).
  if (cfg.num_execution_cores > 0 && cfg.core_strategies_explicit_set == 0) {
    fprintf(stderr,
        "[cfg] WARN: %s has no `core_N_strategy=` lines. "
        "All %u cores defaulting to SIMPLE_DIP (per ControllerConfig_Default). "
        "If this is unintended (e.g., you copied backtest.cfg without "
        "the per-core fields), add `core_0_strategy=mr` etc. to the cfg.\n",
        filepath ? filepath : "(unknown cfg)",
        (unsigned)cfg.num_execution_cores);
  }

  // Phase 8 — backward-compat for fee_rate_maker / fee_rate_taker.
  //
  // Three valid cfg shapes:
  //   1. Only fee_rate set (legacy): mirror to both maker + taker.
  //      Live engine effectively becomes "all-fee_rate" — same as pre-Phase-8.
  //   2. fee_rate_maker + fee_rate_taker set explicitly: live engine uses
  //      per-fill rates based on order->is_maker.
  //   3. Mixed: fee_rate set AND exactly ONE of maker/taker explicitly set.
  //      The other stays at its DEFAULT, which is almost certainly wrong —
  //      WARN loudly so the user can fix.
  //
  // Use the explicit-set flags tracked during parse (above). This handles
  // the case where the user explicitly sets maker/taker to values that
  // happen to equal Default() — value-comparison can't distinguish.
  {
    int legacy_set = !FPN_IsZero(cfg.fee_rate);

    if (!maker_explicitly_set && !taker_explicitly_set && legacy_set) {
      // Legacy mode: only fee_rate set in cfg, mirror to both.
      cfg.fee_rate_maker = cfg.fee_rate;
      cfg.fee_rate_taker = cfg.fee_rate;
      fprintf(stderr,
              "[CFG] fee_rate=%.5f → mirrored to maker+taker (legacy mode)\n",
              FPN_ToDouble(cfg.fee_rate));
    } else if (legacy_set && (maker_explicitly_set ^ taker_explicitly_set)) {
      // Mixed-cfg WARNING — almost certainly user error.
      fprintf(stderr,
              "[CFG] WARNING: fee_rate=%.5f set, but only one of "
              "fee_rate_maker (%.5f) / fee_rate_taker (%.5f) explicitly set. "
              "The other stayed at its default. If you meant to set both, "
              "set both explicitly. If you meant legacy mode, remove the "
              "explicitly-set one.\n",
              FPN_ToDouble(cfg.fee_rate),
              FPN_ToDouble(cfg.fee_rate_maker),
              FPN_ToDouble(cfg.fee_rate_taker));
    }
    // else: both maker+taker set explicitly (case 2 — silent, working as
    // intended) OR neither set + no legacy fee_rate (zero everywhere, fine).
  }

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
