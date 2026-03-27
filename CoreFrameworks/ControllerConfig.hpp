// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [CONTROLLER CONFIG]
//======================================================================================================
// configuration for the portfolio controller - all tunable parameters in one place
// parsed from a simple key=value text file, no JSON, no external libs
//======================================================================================================
#ifndef CONTROLLER_CONFIG_HPP
#define CONTROLLER_CONFIG_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/LinearRegression3X.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
  FPN<F> offset_stddev_mult;  // stddev-scaled offset multiplier (0 = use percentage mode)
  FPN<F> offset_stddev_min;   // adaptation lower bound for stddev mode (e.g. 0.5)
  FPN<F> offset_stddev_max;   // adaptation upper bound for stddev mode (e.g. 4.0)
  FPN<F> min_long_slope;      // min long-window price slope to allow buys (0 = disabled)
  // trailing take-profit (disabled by default)
  FPN<F> tp_hold_score;       // min SNR*R² to hold past TP (0 = disabled, fixed TP)
  FPN<F> tp_trail_mult;       // trailing distance: stddev * this (e.g. 1.0)
  FPN<F> sl_trail_mult;       // trailing SL distance: stddev * this (e.g. 2.0)
  FPN<F> fee_floor_mult;      // TP floor = entry × fee_rate × this (default 3.0, try 5.0 for wider)
  // risk ratios
  FPN<F> min_sl_tp_ratio;     // min SL/TP distance ratio (0.5 = 2:1 reward/risk floor)
  FPN<F> ror_tp_bonus;        // TP multiplier when ROR positive (1.2 = 20% wider)
  FPN<F> momentum_tp_r2_min;  // TP scale at R²=0 (0.5 = half base TP, conservative on uncertainty)
  FPN<F> momentum_sl_r2_max;  // SL scale at R²=0 (1.5 = wider SL in choppy markets)
  // adaptation speed
  FPN<F> squeeze_decay;       // idle squeeze rate per cycle (0.10 = 10% of gap)
  FPN<F> offset_adapt_scale;  // P&L regression → offset shift (0.001)
  FPN<F> stddev_adapt_scale;  // P&L regression → stddev/breakout shift (0.1)
  FPN<F> vol_adapt_scale;     // P&L regression → volume shift (0.1)
  FPN<F> breakout_min;        // momentum breakout floor in stddevs (0.5)
  uint32_t slow_path_max_secs; // wall-time floor between slow paths (3 seconds)
  // time-based exit (disabled by default)
  uint32_t max_hold_ticks;    // close position if held longer than this (0 = disabled)
  FPN<F> min_hold_gain_pct;   // only time-exit if gain < this % (e.g. 0.001 = 0.1%)
  // regime detection
  FPN<F> regime_slope_threshold;  // relative slope magnitude for TRENDING (e.g. 0.001 = 0.1%/tick)
  FPN<F> regime_r2_threshold;     // min R² for TRENDING (e.g. 0.70)
  FPN<F> regime_volatile_stddev;  // stddev/price ratio for VOLATILE (legacy, kept for compat)
  FPN<F> regime_vol_spike_ratio;  // variance ratio threshold: short/long variance > this = volatile spike
  uint32_t regime_hysteresis;     // slow-path cycles before regime switch (e.g. 5)
  uint32_t min_warmup_samples;   // min rolling stats samples before trading (0 = use warmup_ticks only)
  // post-SL cooldown
  uint32_t sl_cooldown_cycles;   // slow-path cycles to pause buying after SL (0 = disabled)
  // momentum strategy
  FPN<F> momentum_breakout_mult;  // buy when price > avg + stddev * this (e.g. 1.5)
  FPN<F> momentum_tp_mult;        // TP multiplier for momentum (e.g. 3.0 stddevs)
  FPN<F> momentum_sl_mult;        // SL multiplier for momentum (e.g. 1.0 stddevs)
  // volume spike detection
  FPN<F> spike_threshold;         // volume spike ratio (current/max) to trigger (e.g. 5.0 = 5x)
  FPN<F> spike_spacing_reduction; // spacing multiplier during spike (e.g. 0.5 = half normal)
  // slippage simulation
  FPN<F> slippage_pct;           // simulated slippage on entry/exit (e.g. 0.0005 = 0.05%)
  // live trading
  int use_real_money;            // 0=paper (default), 1=real orders via REST API
};
//======================================================================================================
template <unsigned F> inline ControllerConfig<F> ControllerConfig_Default() {
  ControllerConfig<F> cfg;
  cfg.poll_interval = 100;
  cfg.warmup_ticks = 128; // minimum raw ticks before trading
  cfg.min_warmup_samples = 0; // min slow-path samples in rolling window (0 = warmup_ticks only)
  cfg.r2_threshold = FPN_FromDouble<F>(0.30);
  cfg.slope_scale_buy = FPN_FromDouble<F>(0.50);
  cfg.max_shift = FPN_FromDouble<F>(0.0001); // 0.01% of price — e.g. $7 at BTC $70k
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
  cfg.max_positions = 1;       // single slot — exchange BTC balance IS the position
  cfg.offset_stddev_mult = FPN_Zero<F>();         // 0 = disabled, use percentage mode
  cfg.offset_stddev_min = FPN_FromDouble<F>(0.5); // 0.5 stddev - most aggressive
  cfg.offset_stddev_max = FPN_FromDouble<F>(4.0); // 4.0 stddev - most defensive
  cfg.min_long_slope = FPN_Zero<F>();             // 0 = disabled
  cfg.tp_hold_score = FPN_Zero<F>();              // 0 = disabled, use fixed TP
  cfg.tp_trail_mult = FPN_FromDouble<F>(1.0);     // trail 1 stddev below price
  cfg.sl_trail_mult = FPN_FromDouble<F>(2.0);     // trail SL 2 stddevs below price
  cfg.fee_floor_mult = FPN_FromDouble<F>(3.0);    // TP floor = entry × fee_rate × 3
  cfg.min_sl_tp_ratio = FPN_FromDouble<F>(0.5);   // 2:1 reward/risk floor
  cfg.ror_tp_bonus = FPN_FromDouble<F>(1.2);      // 20% wider TP on accelerating trend
  cfg.momentum_tp_r2_min = FPN_FromDouble<F>(0.5); // TP scale at R²=0
  cfg.momentum_sl_r2_max = FPN_FromDouble<F>(1.5); // SL scale at R²=0
  cfg.squeeze_decay = FPN_FromDouble<F>(0.10);     // 10% of gap per cycle
  cfg.offset_adapt_scale = FPN_FromDouble<F>(0.001);
  cfg.stddev_adapt_scale = FPN_FromDouble<F>(0.1);
  cfg.vol_adapt_scale = FPN_FromDouble<F>(0.1);
  cfg.breakout_min = FPN_FromDouble<F>(0.5);       // 0.5 stddev floor
  cfg.slow_path_max_secs = 3;
  cfg.max_hold_ticks = 0;                          // 0 = disabled
  cfg.min_hold_gain_pct = FPN_FromDouble<F>(0.001); // 0.1% — only time-exit if below this gain
  // regime detection
  cfg.regime_slope_threshold = FPN_FromDouble<F>(0.00002); // 0.002%/tick ≈ $180 move over 128-tick window at BTC $70k
  cfg.regime_r2_threshold    = FPN_FromDouble<F>(0.70);   // 70% consistency for trending
  cfg.regime_volatile_stddev = FPN_FromDouble<F>(0.0005); // 0.05% stddev/price (legacy compat)
  cfg.regime_vol_spike_ratio = FPN_FromDouble<F>(2.0);   // variance spike: 2x baseline = volatile
  cfg.regime_hysteresis      = 5;                          // 5 slow-path cycles before switch
  cfg.sl_cooldown_cycles     = 5;                          // 5 slow-path cycles pause after SL
  // momentum strategy
  cfg.momentum_breakout_mult = FPN_FromDouble<F>(1.5);    // buy 1.5σ above avg
  cfg.momentum_tp_mult       = FPN_FromDouble<F>(3.0);    // wider TP for trends
  cfg.momentum_sl_mult       = FPN_FromDouble<F>(1.0);    // tighter SL than MR
  // volume spike detection
  cfg.spike_threshold         = FPN_FromDouble<F>(5.0);    // 5x rolling max triggers spike
  cfg.spike_spacing_reduction = FPN_FromDouble<F>(0.5);    // half spacing on spike
  cfg.slippage_pct = FPN_Zero<F>();                        // 0 = disabled (backward compat)
  cfg.use_real_money = 0;                                  // 0 = paper trading (default safe)
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

    if (strcmp(key, "poll_interval") == 0)
      cfg.poll_interval = (uint32_t)atol(val);
    else if (strcmp(key, "warmup_ticks") == 0)
      cfg.warmup_ticks = (uint32_t)atol(val);
    else if (strcmp(key, "r2_threshold") == 0)
      cfg.r2_threshold = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "slope_scale_buy") == 0)
      cfg.slope_scale_buy = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "max_shift") == 0)
      cfg.max_shift = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "take_profit_pct") == 0)
      cfg.take_profit_pct = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "stop_loss_pct") == 0)
      cfg.stop_loss_pct = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "starting_balance") == 0)
      cfg.starting_balance = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "fee_rate") == 0)
      cfg.fee_rate = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "risk_pct") == 0)
      cfg.risk_pct = FPN_FromDouble<F>(
          atof(val) / 100.0); // needs to be current portfolio value, or total
                              // equity holding it goes 1000 -> 900 -> 800
    else if (strcmp(key, "volume_multiplier") == 0)
      cfg.volume_multiplier = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "entry_offset_pct") == 0)
      cfg.entry_offset_pct = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "spacing_multiplier") == 0)
      cfg.spacing_multiplier = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "offset_min") == 0)
      cfg.offset_min = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "offset_max") == 0)
      cfg.offset_max = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "vol_mult_min") == 0)
      cfg.vol_mult_min = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "vol_mult_max") == 0)
      cfg.vol_mult_max = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "filter_scale") == 0)
      cfg.filter_scale = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "max_drawdown_pct") == 0)
      cfg.max_drawdown_pct = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "max_exposure_pct") == 0)
      cfg.max_exposure_pct = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "max_positions") == 0) {
      int v = atoi(val); if (v < 1) v = 1; if (v > 16) v = 16;
      cfg.max_positions = (uint32_t)v;
    }
    else if (strcmp(key, "offset_stddev_mult") == 0) {
      double v = atof(val); if (v < 0) v = 0;
      cfg.offset_stddev_mult = FPN_FromDouble<F>(v);
    }
    else if (strcmp(key, "offset_stddev_min") == 0) {
      double v = atof(val); if (v < 0) v = 0;
      cfg.offset_stddev_min = FPN_FromDouble<F>(v);
    }
    else if (strcmp(key, "offset_stddev_max") == 0) {
      double v = atof(val); if (v < 0) v = 0;
      cfg.offset_stddev_max = FPN_FromDouble<F>(v);
    }
    else if (strcmp(key, "min_long_slope") == 0)
      cfg.min_long_slope = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "tp_hold_score") == 0) {
      double v = atof(val); if (v < 0) v = 0;
      cfg.tp_hold_score = FPN_FromDouble<F>(v);
    }
    else if (strcmp(key, "tp_trail_mult") == 0) {
      double v = atof(val); if (v < 0) v = 0;
      cfg.tp_trail_mult = FPN_FromDouble<F>(v);
    }
    else if (strcmp(key, "sl_trail_mult") == 0) {
      double v = atof(val); if (v < 0) v = 0;
      cfg.sl_trail_mult = FPN_FromDouble<F>(v);
    }
    else if (strcmp(key, "fee_floor_mult") == 0) {
      double v = atof(val); if (v < 1) v = 1;
      cfg.fee_floor_mult = FPN_FromDouble<F>(v);
    }
    else if (strcmp(key, "min_sl_tp_ratio") == 0)
      cfg.min_sl_tp_ratio = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "ror_tp_bonus") == 0)
      cfg.ror_tp_bonus = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "momentum_tp_r2_min") == 0)
      cfg.momentum_tp_r2_min = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "momentum_sl_r2_max") == 0)
      cfg.momentum_sl_r2_max = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "squeeze_decay") == 0)
      cfg.squeeze_decay = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "offset_adapt_scale") == 0)
      cfg.offset_adapt_scale = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "stddev_adapt_scale") == 0)
      cfg.stddev_adapt_scale = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "vol_adapt_scale") == 0)
      cfg.vol_adapt_scale = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "breakout_min") == 0)
      cfg.breakout_min = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "slow_path_max_secs") == 0)
      cfg.slow_path_max_secs = (uint32_t)atol(val);
    else if (strcmp(key, "max_hold_ticks") == 0)
      cfg.max_hold_ticks = (uint32_t)atol(val);
    else if (strcmp(key, "min_hold_gain_pct") == 0)
      cfg.min_hold_gain_pct = FPN_FromDouble<F>(atof(val) / 100.0);
    // regime detection
    else if (strcmp(key, "regime_slope_threshold") == 0)
      cfg.regime_slope_threshold = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "regime_r2_threshold") == 0)
      cfg.regime_r2_threshold = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "regime_volatile_stddev") == 0)
      cfg.regime_volatile_stddev = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "regime_vol_spike_ratio") == 0)
      cfg.regime_vol_spike_ratio = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "regime_hysteresis") == 0)
      cfg.regime_hysteresis = (uint32_t)atol(val);
    else if (strcmp(key, "min_warmup_samples") == 0)
      cfg.min_warmup_samples = (uint32_t)atol(val);
    else if (strcmp(key, "sl_cooldown_cycles") == 0)
      cfg.sl_cooldown_cycles = (uint32_t)atol(val);
    // momentum strategy
    else if (strcmp(key, "momentum_breakout_mult") == 0)
      cfg.momentum_breakout_mult = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "momentum_tp_mult") == 0)
      cfg.momentum_tp_mult = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "momentum_sl_mult") == 0)
      cfg.momentum_sl_mult = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "spike_threshold") == 0)
      cfg.spike_threshold = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "spike_spacing_reduction") == 0)
      cfg.spike_spacing_reduction = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "slippage_pct") == 0)
      cfg.slippage_pct = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "use_real_money") == 0)
      cfg.use_real_money = atoi(val);
  }

  fclose(f);
  return cfg;
}
//======================================================================================================
//======================================================================================================
#endif // CONTROLLER_CONFIG_HPP
