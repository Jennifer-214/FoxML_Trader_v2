#pragma once
// SettingsPanel — data-driven config editor for engine.cfg
//
// ADDING A NEW SETTING:
//   1. add ONE entry to the field_defs[] array below (with tooltip)
//   2. done — loading, rendering, saving, and tooltips are all automatic
//
// field types:
//   CFG_FLOAT  — text input for float values (format string for precision)
//   CFG_INT    — text input for integer values
//   CFG_BOOL   — checkbox toggle (writes "0" or "1")

#include "imgui.h"
#include "FoxmlTheme.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>

//==========================================================================
// FIELD DESCRIPTOR — one entry per editable config field
//==========================================================================
enum CfgFieldType { CFG_FLOAT, CFG_INT, CFG_BOOL, CFG_PATH };

struct CfgFieldDef {
    const char *key;       // engine.cfg key name (e.g. "take_profit_pct")
    const char *label;     // GUI label (e.g. "TP %%")
    const char *section;   // collapsing header name (e.g. "Trading")
    CfgFieldType type;
    const char *fmt;       // printf format for floats (e.g. "%.2f")
    const char *tooltip;   // hover tooltip text (NULL = no tooltip)
};

// ── THE SINGLE SOURCE OF TRUTH ──
// adding a field: add ONE line here. loading + rendering + saving are automatic.
static const CfgFieldDef field_defs[] = {
    // Trading
    {"take_profit_pct",       "TP %%",        "Trading",         CFG_FLOAT, "%.2f", NULL},
    {"stop_loss_pct",         "SL %%",        "Trading",         CFG_FLOAT, "%.2f", NULL},
    {"fee_rate",              "Fee %%",       "Trading",         CFG_FLOAT, "%.2f",
        "Legacy fee rate (% per trade) — used for pre-trade quantity computations\n"
        "(no-trade band, fee floor for TP, kill switch estimate, spread display)\n"
        "and as the default for fee_rate_maker / fee_rate_taker if those aren't set."},
    {"fee_rate_maker",        "Maker %%",     "Trading",         CFG_FLOAT, "%.3f",
        "Maker fee rate (% per fill) — applied when order->is_maker=1 (POST_ONLY\n"
        "limit fill). Default 0.075 (Binance tier 0 BNB-discount). If not set,\n"
        "mirrors fee_rate. Setting ONLY this without fee_rate_taker triggers a\n"
        "[CFG] warning — both should be set explicitly or neither."},
    {"fee_rate_taker",        "Taker %%",     "Trading",         CFG_FLOAT, "%.3f",
        "Taker fee rate (% per fill) — applied when order->is_maker=0 (market\n"
        "fill, default for synchronous orders, or POST_ONLY limit that crossed\n"
        "the spread). Default 0.100 (Binance tier 0 BNB-discount). Backtest\n"
        "uses this rate exclusively (all-taker simulation)."},
    {"slippage_pct",          "Slippage %%",  "Trading",         CFG_FLOAT, "%.2f", NULL},
    {"risk_pct",              "Risk/Pos %%",  "Trading",         CFG_FLOAT, "%.1f", NULL},
    {"fee_floor_mult",        "Fee Floor",    "Trading",         CFG_FLOAT, "%.1f",
        "TP floor = entry * fee_rate * this\n3.0 = TP must clear round-trip fees + margin"},
    // Entry Filters
    {"entry_offset_pct",      "Offset %%",    "Entry Filters",   CFG_FLOAT, "%.3f",
        "Buy gate offset below avg/EMA price\nhigher = deeper dip required to enter"},
    {"volume_multiplier",     "Vol Mult",     "Entry Filters",   CFG_FLOAT, "%.2f",
        "Volume gate: require avg_volume * this\nhigher = only buy on high volume"},
    {"spacing_multiplier",    "Spacing",      "Entry Filters",   CFG_FLOAT, "%.2f",
        "Min distance between entries (in stddev)\nprevents clustering entries at similar prices"},
    {"offset_stddev_mult",    "Stddev Mult",  "Entry Filters",   CFG_FLOAT, "%.2f",
        "Multiplies stddev for offset calculation\nhigher = wider offset from avg (fewer entries)"},
    {"offset_stddev_min",     "Stddev Min",   "Entry Filters",   CFG_FLOAT, "%.2f", NULL},
    {"offset_stddev_max",     "Stddev Max",   "Entry Filters",   CFG_FLOAT, "%.2f", NULL},
    {"min_stddev_pct",        "Min Stddev %%","Entry Filters",   CFG_FLOAT, "%.5f",
        "Skip trades when stddev/price below this\nprevents entries in dead-flat markets"},
    {"min_long_slope",        "Min Long Slope","Entry Filters",  CFG_FLOAT, "%.6f",
        "Block MR buys when 512-tick slope below this\nnegative = allow mild dips, 0 = disabled"},
    {"min_buy_delta",         "Min Buy Delta","Entry Filters",   CFG_FLOAT, "%.2f",
        "Min volume delta for MR buys\n-0.3 = allow mild selling, block heavy dumps"},
    {"vwap_offset",           "VWAP Offset",  "Entry Filters",   CFG_FLOAT, "%.4f", NULL},
    // Adaptation
    {"filter_scale",          "Filter Scale", "Adaptation",      CFG_FLOAT, "%.2f",
        "How fast filters adapt to P&L regression\nhigher = more reactive to recent performance"},
    {"offset_min",            "Offset Min %%","Adaptation",      CFG_FLOAT, "%.2f", NULL},
    {"offset_max",            "Offset Max %%","Adaptation",      CFG_FLOAT, "%.2f", NULL},
    {"vol_mult_min",          "Vol Min",      "Adaptation",      CFG_FLOAT, "%.2f", NULL},
    {"vol_mult_max",          "Vol Max",      "Adaptation",      CFG_FLOAT, "%.2f", NULL},
    {"r2_threshold",          "R² Threshold", "Adaptation",      CFG_FLOAT, "%.2f", NULL},
    {"slope_scale_buy",       "Slope Scale",  "Adaptation",      CFG_FLOAT, "%.2f", NULL},
    {"max_shift",             "Max Shift",    "Adaptation",      CFG_FLOAT, "%.4f", NULL},
    // Trailing TP/SL
    {"tp_hold_score",         "Hold Score",   "Trailing TP/SL",  CFG_FLOAT, "%.2f",
        "SNR * R-squared threshold to activate trailing\nhigher = only trail strong consistent trends"},
    {"tp_trail_mult",         "Trail TP",     "Trailing TP/SL",  CFG_FLOAT, "%.2f",
        "Trailing TP distance as fraction of offset\nTP ratchets up as price rises"},
    {"sl_trail_mult",         "Trail SL",     "Trailing TP/SL",  CFG_FLOAT, "%.2f",
        "Trailing SL distance as fraction of offset\nSL ratchets up to lock in gains"},
    // Time-Based Exit
    {"max_hold_ticks",        "Max Hold",     "Time-Based Exit", CFG_INT,   "%d",
        "Close position after this many ticks\n0 = disabled, 75000 ≈ 4-5 hours"},
    {"min_hold_gain_pct",     "Min Gain %%",  "Time-Based Exit", CFG_FLOAT, "%.2f",
        "Only time-exit if gain below this %%\nprotects profitable positions from time exit"},
    // Risk Management
    {"max_drawdown_pct",      "Max DD %%",    "Risk Management", CFG_FLOAT, "%.1f",
        "Circuit breaker: halt trading if total P&L\ndrops below this %% of starting balance"},
    {"max_exposure_pct",      "Max Exp %%",   "Risk Management", CFG_FLOAT, "%.0f", NULL},
    {"max_positions",         "Max Pos",      "Risk Management", CFG_INT,   "%d",   NULL},
    // Kill Switch
    {"kill_switch_enabled",   "Enabled",      "Kill Switch",     CFG_BOOL,  NULL,   NULL},
    {"kill_switch_daily_loss_pct","Daily Loss %%","Kill Switch",  CFG_FLOAT, "%.2f",
        "Max session loss before kill switch triggers\n3.0 = halt if equity drops 3%% from session start"},
    {"kill_switch_drawdown_pct","Drawdown %%", "Kill Switch",     CFG_FLOAT, "%.2f",
        "Max drawdown from session peak before kill\n5.0 = halt if 5%% below intra-session high"},
    {"kill_recovery_warmup",  "Recovery",     "Kill Switch",      CFG_INT,   "%d",
        "Slow-path cycles to observe after kill reset\nbefore trading resumes (prevents immediate re-entry)"},
    // Vol Sizing
    {"vol_sizing_enabled",    "Enabled",      "Vol Sizing",      CFG_BOOL,  NULL,   NULL},
    {"vol_scale_min",         "Scale Min",    "Vol Sizing",      CFG_FLOAT, "%.2f",
        "Minimum position scale factor\n0.25 = never less than 25%% of base qty"},
    {"vol_scale_max",         "Scale Max",    "Vol Sizing",      CFG_FLOAT, "%.2f",
        "Maximum position scale factor\n2.0 = never more than 200%% of base qty"},
    // No-Trade Band
    {"no_trade_band_enabled", "Enabled",      "No-Trade Band",   CFG_BOOL,  NULL,   NULL},
    {"no_trade_band_mult",    "Fee Mult",     "No-Trade Band",   CFG_FLOAT, "%.2f",
        "Signal must exceed fee_rate * this to trade\n3.0 = dip must be 3x round-trip fee cost"},
    // Regime Detection
    {"regime_crossover_threshold","Mild Trend","Regime Detection",CFG_FLOAT,"%.5f",
        "EMA/SMA spread for MILD_TREND (EMA Cross)\n0.0005 = 0.05%% gap (~$35 at BTC $68k)\nbelow = RANGING, above = mild uptrend"},
    {"regime_strong_crossover","Strong Trend","Regime Detection",CFG_FLOAT,"%.5f",
        "EMA/SMA spread for strong TRENDING (Momentum)\n0.0015 = 0.15%% gap (~$102 at BTC $68k)\nabove = Momentum, below = EMA Cross"},
    {"regime_r2_threshold",   "R² Threshold", "Regime Detection", CFG_FLOAT, "%.1f",
        "Min R-squared consistency for TRENDING\n70 = 70%% of price variance explained by trend"},
    {"regime_vol_spike_ratio","Vol Spike",    "Regime Detection", CFG_FLOAT, "%.1f",
        "Short/long variance ratio for VOLATILE\n2.0 = short-window variance is 2x long-window"},
    {"regime_hysteresis",     "Hysteresis",   "Regime Detection", CFG_INT,   "%d",
        "Slow-path cycles before regime switch\nprevents rapid flipping between strategies"},
    // Momentum
    {"momentum_breakout_mult","Breakout",     "Momentum",        CFG_FLOAT, "%.2f",
        "Buy above avg by this many stddev\nhigher = require stronger breakout"},
    {"momentum_tp_mult",      "Mom TP",       "Momentum",        CFG_FLOAT, "%.2f",
        "Momentum TP distance in stddev units\nscaled by R-squared at fill time"},
    {"momentum_sl_mult",      "Mom SL",       "Momentum",        CFG_FLOAT, "%.2f",
        "Momentum SL distance in stddev units\nscaled by R-squared at fill time"},
    {"momentum_r2_min",       "R² Min",       "Momentum",        CFG_FLOAT, "%.2f",
        "Min R-squared to enter momentum trades\n0.4 = require 40%% trend consistency"},
    // EMA Cross
    {"emacross_dip_mult",     "Dip Mult",     "EMA Cross",       CFG_FLOAT, "%.2f",
        "Buy this many stddevs below EMA\n0.5 = half sigma dip"},
    {"emacross_crossover_min","Crossover Min", "EMA Cross",       CFG_FLOAT, "%.4f",
        "Min EMA-SMA spread to confirm uptrend\n0.0003 = 0.03%%"},
    {"emacross_trail_mult",   "Trail Mult",   "EMA Cross",       CFG_FLOAT, "%.2f",
        "Trailing TP factor when EMA rising\n1.5 = 50%% wider trail"},
    // Partial Exits
    {"partial_exit_pct",      "TP1 Split %%", "Partial Exits",   CFG_FLOAT, "%.2f",
        "Fraction to exit at TP1\n0.5 = 50%% exits early, 50%% rides TP2"},
    {"tp2_mult",              "TP2 Mult",     "Partial Exits",   CFG_FLOAT, "%.2f",
        "TP2 distance = TP1 distance * this\n2.0 = second leg targets double the gain"},
    {"breakeven_on_partial",  "Breakeven SL", "Partial Exits",   CFG_BOOL,  NULL,   NULL},
    // Gate Recovery
    {"idle_reset_cycles",     "Idle Reset",   "Gate Recovery",   CFG_INT,   "%d",
        "Cycles with no fill before gate decay\nprevents permanent lockout after losses"},
    {"sl_cooldown_cycles",    "SL Cooldown",  "Gate Recovery",   CFG_INT,   "%d",
        "Slow-path cycles to pause after stop loss\nlets market settle before re-entry"},
    {"sl_cooldown_adaptive",  "Adaptive CD",  "Gate Recovery",   CFG_BOOL,  NULL,   NULL},
    // Session Filters
    {"session_asian_mult",    "Asian",        "Session Filters",  CFG_FLOAT, "%.2f",
        "Volume gate multiplier 00-07 UTC\nhigher = more selective (fewer entries)"},
    {"session_european_mult", "European",     "Session Filters",  CFG_FLOAT, "%.2f", NULL},
    {"session_us_mult",       "US",           "Session Filters",  CFG_FLOAT, "%.2f",
        "Volume gate multiplier 13-20 UTC\nlower = less selective (best liquidity)"},
    {"session_overnight_mult","Overnight",    "Session Filters",  CFG_FLOAT, "%.2f", NULL},
    // Strategy
    {"default_strategy",      "Default##strat","Strategy",       CFG_INT,   "%d",
        "-2 = Full Auto (MR+EMA Cross+Momentum+SimpleDip)\n-1 = Legacy Auto (MR+Momentum only)\n 0 = Mean Reversion\n 1 = Momentum\n 2 = Simple Dip\n 3 = ML\n 4 = EMA Cross"},
    // EMA Gate
    {"gate_ema_enabled",      "EMA Enabled",  "EMA Gate",        CFG_BOOL,  NULL,   NULL},
    {"gate_ema_alpha",        "Alpha",        "EMA Gate",        CFG_FLOAT, "%.4f",
        "EMA smoothing factor\n0.99 = fast (responsive)\n0.997 = default\n0.999 = slow (stable)"},
    // Danger Gradient
    {"danger_enabled",        "Enabled",      "Danger Gradient",  CFG_BOOL,  NULL,   NULL},
    {"danger_warn_stddevs",   "Warn σ",       "Danger Gradient",  CFG_FLOAT, "%.1f",
        "Danger gradient starts at this many σ below avg\n3.0 = gate begins tightening at 3σ drop"},
    {"danger_crash_stddevs",  "Crash σ",      "Danger Gradient",  CFG_FLOAT, "%.1f",
        "Full gate kill at this many σ below avg\n6.0 = gate zeroed at 6σ drop (crash protection)"},
    // Tick Recording
    {"record_ticks",          "Record Ticks", "Tick Recording",  CFG_BOOL,  NULL,
        "Record raw ticks to CSV for backtesting/ML training\nOutput: data/{SYMBOL}/YYYY-MM-DD.csv\n~30-70MB/day for BTCUSDT"},
    {"record_depth",          "Record Depth", "Tick Recording",  CFG_BOOL,  NULL,
        "Record @depth5@100ms snapshots to CSV (top-of-book + lastUpdateId)\n"
        "Output: data/{SYMBOL}/depth/YYYY-MM-DD.csv\n"
        "Requires depth_enabled=1. Daily rotation, auto-pruned by record_max_days.\n"
        "Gap markers (# GAP) on backward last_update_id, wallclock >2s, or disconnect.\n"
        "~50 MB/day for BTCUSDT. Required for future backtest replay of book state."},
    {"record_max_days",       "Max Days",     "Tick Recording",  CFG_FLOAT, "%.0f",
        "Auto-delete tick + depth CSVs older than this many days\n30 = ~1-2GB cap on disk usage (more if depth recording is on)"},
    // Operational Monitoring (Phase 8b) — alerts on kill switch, orphans, disconnects
    {"notify_enabled",        "Notify",       "Operational Monitoring", CFG_BOOL,  NULL,
        "0 = file logs only (default)\n"
        "1 = route alerts through configured backend (kill switch trips, orphans,\n"
        "    disconnects). Also keeps the existing fprintfs — backend is additive."},
    {"notify_backend",        "Backend",      "Operational Monitoring", CFG_INT,   "%d",
        "0 = stderr (default — visible via tail -f or syslog)\n"
        "1 = command (popen-based shell template — see notify_command)\n"
        "Slack/Telegram/Discord/dunst/etc. all use backend=1 with a service-\n"
        "specific command template. No native HTTP backends — sidesteps the\n"
        "TLS-in-engine question entirely."},
    {"notify_command",        "Command",      "Operational Monitoring", CFG_PATH,  NULL,
        "Shell command template with up to two %s (subject, body).\n"
        "Examples (substitute YOUR_* with real URLs/tokens):\n"
        "  dunst:    notify-send 'Engine: %s' '%s'\n"
        "  Discord:  curl -s -X POST -H 'Content-Type: application/json' \\\n"
        "                -d '{\"content\":\"%s\\n%s\"}' YOUR_DISCORD_WEBHOOK\n"
        "  Slack:    curl -s -X POST -H 'Content-Type: application/json' \\\n"
        "                -d '{\"text\":\"%s: %s\"}' YOUR_SLACK_WEBHOOK\n"
        "  Telegram: curl -s 'https://api.telegram.org/botYOUR_TOKEN/sendMessage' \\\n"
        "                -d 'chat_id=YOUR_CHAT&text=%s: %s'\n"
        "  ntfy.sh:  curl -s -d '%s: %s' https://ntfy.sh/your-topic\n"
        "Recommend prepending `timeout 10 ` for safety against hung commands.\n"
        "%s placeholders MUST be wrapped in single quotes in the template — the\n"
        "engine escapes internal ' but does not add enclosing quotes."},
    {"notify_cooldown_secs",  "Cooldown s",   "Operational Monitoring", CFG_INT,   "%d",
        "Min seconds between alerts of the same kind (default 60).\n"
        "Different kinds are independent. Lower = more spam during disconnect storms.\n"
        "Used for both stderr and command backends."},
    // Toggles
    {"use_real_money",        "LIVE Trading", "Toggles",         CFG_BOOL,  NULL,   NULL},
    {"partial_exit_enabled",  "Partial Exits","Toggles",         CFG_BOOL,  NULL,   NULL},
    {"session_filter_enabled","Session Filter","Toggles",        CFG_BOOL,  NULL,   NULL},
    {"depth_enabled",         "Order Book",   "Toggles",         CFG_BOOL,  NULL,   NULL},
    {"min_book_imbalance",    "Book Imbal",   "Toggles",         CFG_FLOAT, "%.2f", NULL},
    // FoxML integration (Phase 6C)
    {"cost_gate_enabled",        "Cost Gate",         "FoxML",  CFG_BOOL,  NULL,
        "Suppress entries when estimated trade cost exceeds TP target\nuses spread + vol timing + market impact model"},
    {"foxml_vol_scaling_enabled","Vol Scaling",        "FoxML",  CFG_BOOL,  NULL,
        "Scale position size inversely with volatility\nhigh vol = smaller position (consistent risk per trade)"},
    {"foxml_vol_scaling_z_max",  "Vol Z-Max",         "FoxML",  CFG_FLOAT, "%.1f",
        "Z-score clipping threshold for vol scaler\n3.0 = cap at 3 sigma (FoxML default)"},
    {"bandit_enabled",           "Bandit",            "FoxML",  CFG_BOOL,  NULL,
        "Blend regime strategy pick with Exp3-IX bandit weights\nlearns which strategies actually profit over time"},
    {"bandit_blend_ratio",       "Blend Ratio",       "FoxML",  CFG_FLOAT, "%.2f",
        "Max bandit influence fraction (0.30 = 30%%)\nramps from 0%% to this over first 200 trades"},
    {"confidence_enabled",       "Confidence",        "FoxML",  CFG_BOOL,  NULL,
        "Dynamic ML threshold based on prediction quality\nraises threshold when IC/freshness/stability are low"},
    {"confidence_window",        "Conf Window",       "FoxML",  CFG_INT,   "%d",
        "RollingIC + RollingRMSE window size (default 32)\nlarger = smoother but slower to react to model changes\ncapped at ROLLING_IC_MAX_WINDOW=64"},
    {"confidence_freshness_tau", "Conf Tau (s)",      "FoxML",  CFG_FLOAT, "%.0f",
        "Freshness decay constant in seconds (default 300 = 5min)\ne^(-data_age/tau): data_age=tau gives 0.37 freshness"},
    {"confidence_threshold_scale","Conf Scale",        "FoxML",  CFG_FLOAT, "%.2f",
        "Gate formula: effective_thr = base * (this - conf)\ndefault 2.0 — conf=0 → 2x base (suppresses marginal signals)\nconf=1 → 1x base (full signal). Clamps at 1.0."},
    // Validation (Phase 7prep) — held-out test set + generalization gap
    {"held_out_fraction",        "Held-Out %",         "Validation", CFG_FLOAT, "%.2f",
        "Fraction of data reserved as held-out test set\n"
        "default 0.20 (20%% — last 2 months of 12-month dataset)\n"
        "code refuses to peek at this set during training/tuning\n"
        "explicit unlock required for final evaluation\n"
        "clamped to [0.05, 0.30] in HeldOutSplit_Make"},
    {"gap_acceptable_threshold", "Gap Threshold",      "Validation", CFG_FLOAT, "%.3f",
        "Max acceptable |walk-forward - held-out| generalization gap\n"
        "default 0.05 — gap above this = poor generalization (not OK)\n"
        "applied to both classification accuracy and regression Pearson r\n"
        "load-bearing: this is the WAS-IT-REAL test for trained models"},
    // Model Paths (Phase 7C)
    {"ml_model_path",            "Buy Model",         "Models", CFG_PATH,  NULL,
        "Path to XGBoost/LightGBM buy-signal model\ntrain in foxml_suite, load here"},
    {"regime_model_path",        "Regime Model",      "Models", CFG_PATH,  NULL,
        "Path to regime enrichment model\nMode A: regime signal enhancement"},
    // Barrier Gate (Phase 7E)
    {"barrier_gate_enabled",     "Barrier Gate",      "Barrier", CFG_BOOL, NULL,
        "Block entries before predicted price peaks\nrequires trained peak/valley models"},
    {"peak_model_path",          "Peak Model",        "Barrier", CFG_PATH, NULL,
        "Path to P(will_peak) model\ntrain with LABEL_WILL_PEAK in foxml_suite"},
    {"valley_model_path",        "Valley Model",      "Barrier", CFG_PATH, NULL,
        "Path to P(will_valley) model\ntrain with LABEL_WILL_VALLEY in foxml_suite"},
    // Per-core sharded engine (Phase 14 — experimental)
    {"engine_mode",              "Sharded Mode",      "Per-Core (Experimental)", CFG_BOOL, NULL,
        "Per-core risk-sharded execution (experimental).\n"
        "OFF (default): legacy single-threaded engine.\n"
        "ON: per-core architecture, controller core owns the portfolio,\n"
        "    each execution core is a mini-portfolio of one position.\n"
        "    Uses synthetic ticks until Binance feed wiring lands.\n"
        "RESTART REQUIRED to take effect."},
    {"num_execution_cores",      "Cores",             "Per-Core (Experimental)", CFG_INT,  "%d",
        "Number of execution cores in sharded mode (1-16).\n"
        "Each core handles one position at a time.\n"
        "Recommended: physical core count - 2 (one for controller, one for OS).\n"
        "On AMD: pin all cores to the same CCD to avoid cross-die latency.\n"
        "RESTART REQUIRED to take effect."},
    // Per-core strategy assignment
    {"core_0_strategy",          "Core 0",            "Core Strategies",  CFG_INT, "%d",
        "0=MR 1=Momentum 2=SimpleDip 3=ML 4=EMA Cross 255=None\nHot-swappable while running"},
    {"core_1_strategy",          "Core 1",            "Core Strategies",  CFG_INT, "%d", NULL},
    {"core_2_strategy",          "Core 2",            "Core Strategies",  CFG_INT, "%d", NULL},
    {"core_3_strategy",          "Core 3",            "Core Strategies",  CFG_INT, "%d", NULL},
    // Per-core risk allocation (0 = use shared risk_pct / num_cores)
    {"core_0_risk_pct",          "Core 0 Risk %%",    "Core Risk",        CFG_FLOAT, "%.1f",
        "Risk %% of total balance for core 0\n0 = use shared risk_pct / num_cores"},
    {"core_1_risk_pct",          "Core 1 Risk %%",    "Core Risk",        CFG_FLOAT, "%.1f", NULL},
    {"core_2_risk_pct",          "Core 2 Risk %%",    "Core Risk",        CFG_FLOAT, "%.1f", NULL},
    {"core_3_risk_pct",          "Core 3 Risk %%",    "Core Risk",        CFG_FLOAT, "%.1f", NULL},
    // Per-strategy TP/SL overrides (0 = use shared take_profit_pct / stop_loss_pct)
    {"simpledip_tp_pct",         "DIP TP %%",         "SimpleDip Tuning", CFG_FLOAT, "%.2f",
        "SimpleDip-specific take profit %%\n0 = use shared TP %%"},
    {"simpledip_sl_pct",         "DIP SL %%",         "SimpleDip Tuning", CFG_FLOAT, "%.2f",
        "SimpleDip-specific stop loss %%\n0 = use shared SL %%"},
    {"mr_tp_pct",                "MR TP %%",          "MeanReversion Tuning", CFG_FLOAT, "%.2f",
        "MeanReversion-specific take profit %%\n0 = use shared TP %%"},
    {"mr_sl_pct",                "MR SL %%",          "MeanReversion Tuning", CFG_FLOAT, "%.2f",
        "MeanReversion-specific stop loss %%\n0 = use shared SL %%"},
    {"momentum_tp_mult",         "MOM TP σ",          "Momentum Tuning",  CFG_FLOAT, "%.1f",
        "Momentum TP distance in stddevs\n3.0 = TP at entry + 3σ"},
    {"momentum_sl_mult",         "MOM SL σ",          "Momentum Tuning",  CFG_FLOAT, "%.1f",
        "Momentum SL distance in stddevs\n2.0 = SL at entry - 2σ"},
    {"momentum_breakout_mult",   "MOM Breakout",      "Momentum Tuning",  CFG_FLOAT, "%.2f",
        "Buy when price > avg + stddev * this"},
    {"emacross_tp_pct",          "EMA TP %%",         "EMA Cross Tuning", CFG_FLOAT, "%.2f",
        "EMA Cross-specific take profit %%\n0 = use shared TP %%"},
    {"emacross_sl_pct",          "EMA SL %%",         "EMA Cross Tuning", CFG_FLOAT, "%.2f",
        "EMA Cross-specific stop loss %%\n0 = use shared SL %%"},
    {"emacross_dip_mult",        "EMA Dip σ",         "EMA Cross Tuning", CFG_FLOAT, "%.2f",
        "Buy this many stddevs below EMA in uptrends"},
    {"emacross_crossover_min",   "EMA Cross Min",     "EMA Cross Tuning", CFG_FLOAT, "%.4f",
        "Min EMA-SMA spread for uptrend confirmation"},
    // Engine Timing — knobs that control sample cadence + warmup
    // (added 2026-04-25 — these matter for ML training experiments and were
    // previously cfg-only edits)
    {"poll_interval",            "Poll Interval",     "Engine Timing",   CFG_INT,   "%d",
        "Ticks between slow-path runs (regression, adaptation, sample collection)\n"
        "default 100. ML training note: with poll_interval << forward_ticks,\n"
        "consecutive samples have heavily-overlapping forward windows → label\n"
        "autocorrelation. For independent samples set poll_interval = forward_ticks."},
    {"warmup_ticks",             "Warmup Ticks",      "Engine Timing",   CFG_INT,   "%d",
        "Minimum raw ticks before trading starts. Counts every tick.\n"
        "Use this when you want a longer total-tick warmup. No upper bound."},
    {"min_warmup_samples",       "Min Rolling Samples","Engine Timing",  CFG_INT,   "%d",
        "Min rolling-stats samples before trading. CAPS at 128 (rolling window\n"
        "size). Values >128 are clamped at config load with a warning. Use\n"
        "warmup_ticks for longer raw-tick warmup."},
};
static constexpr int NUM_FIELDS = sizeof(field_defs) / sizeof(field_defs[0]);

//==========================================================================
// PER-CORE OVERRIDE FIELDS — v4.0
//
// One row per overridable field. The actual cfg key is built at render time:
// "core_<N>_<key_suffix>". 0 / blank means "inherit from Global tab".
//
// Adding a per-core field: add ONE entry here + ONE line in
// PerCoreOverrides + ONE line in ControllerConfig_ResolveForCore + ONE
// parser case in ControllerConfig_Load. Four sites total.
//==========================================================================
struct PerCoreFieldDef {
    const char *key_suffix;   // e.g. "take_profit_pct" → cfg key core_0_take_profit_pct
    const char *label;
    const char *section;      // "Trading" / "Entry Filters" / "Strategy-Specific"
    const char *fmt;
    const char *tooltip;
};

static const PerCoreFieldDef per_core_fields[] = {
    // Trading overrides
    {"take_profit_pct",   "TP %%",       "Trading",           "%.2f",
        "Override global TP %. 0 = inherit from Global tab."},
    {"stop_loss_pct",     "SL %%",       "Trading",           "%.2f",
        "Override global SL %. 0 = inherit from Global tab."},
    {"fee_floor_mult",    "Fee Floor",   "Trading",           "%.1f",
        "Override global fee floor multiplier. 0 = inherit."},
    // Entry filter overrides
    {"entry_offset_pct",   "Offset %%",   "Entry Filters",    "%.3f",
        "Override global buy gate offset. 0 = inherit."},
    {"volume_multiplier",  "Vol Mult",    "Entry Filters",    "%.2f",
        "Override global volume gate multiplier. 0 = inherit."},
    {"spacing_multiplier", "Spacing",     "Entry Filters",    "%.2f",
        "Override global entry spacing (in stddev). 0 = inherit."},
    {"offset_stddev_mult", "Stddev Mult", "Entry Filters",    "%.2f",
        "Override global stddev mult for offset. 0 = inherit."},
    // Strategy-specific overrides — only consulted when this core runs the
    // matching strategy. Useful for A/B testing same-strategy variants
    // across cores.
    {"simpledip_tp_pct",  "DIP TP %%",   "Strategy-Specific", "%.2f",
        "DIP-only TP override for this core. 0 = inherit."},
    {"simpledip_sl_pct",  "DIP SL %%",   "Strategy-Specific", "%.2f",
        "DIP-only SL override for this core. 0 = inherit."},
    {"mr_tp_pct",         "MR TP %%",    "Strategy-Specific", "%.2f",
        "MR-only TP override for this core. 0 = inherit."},
    {"mr_sl_pct",         "MR SL %%",    "Strategy-Specific", "%.2f",
        "MR-only SL override for this core. 0 = inherit."},
    {"momentum_tp_mult",  "MOM TP σ",    "Strategy-Specific", "%.2f",
        "MOM-only TP stddev multiplier for this core. 0 = inherit."},
    {"momentum_sl_mult",  "MOM SL σ",    "Strategy-Specific", "%.2f",
        "MOM-only SL stddev multiplier for this core. 0 = inherit."},
    {"emacross_tp_pct",   "EMA TP %%",   "Strategy-Specific", "%.2f",
        "EMA-only TP override for this core. 0 = inherit."},
    {"emacross_sl_pct",   "EMA SL %%",   "Strategy-Specific", "%.2f",
        "EMA-only SL override for this core. 0 = inherit."},
    {"ml_tp_pct",         "ML TP %%",    "Strategy-Specific", "%.2f",
        "ML-only TP override for this core. 0 = inherit."},
    {"ml_sl_pct",         "ML SL %%",    "Strategy-Specific", "%.2f",
        "ML-only SL override for this core. 0 = inherit."},
    {"ml_buy_threshold",  "ML Threshold","Strategy-Specific", "%.3f",
        "ML buy threshold override for this core (0-1). 0 = inherit."},
};
static constexpr int NUM_PER_CORE_FIELDS =
    sizeof(per_core_fields) / sizeof(per_core_fields[0]);
static constexpr int MAX_GUI_CORES = 16;

//==========================================================================
// SETTINGS STATE — auto-generated from field_defs (no manual struct)
//==========================================================================
struct SettingsState {
    float float_vals[NUM_FIELDS];  // storage for float/int fields
    int   bool_vals[NUM_FIELDS];   // storage for bool fields
    char  path_vals[NUM_FIELDS][512]; // storage for path fields (Phase 8b: 256→512 to fit notify_command templates)
    // v4.0 per-core override storage. Indexed [core][field]. Floats only —
    // every per-core override is FPN<F> in the cfg.
    float per_core_vals[MAX_GUI_CORES][NUM_PER_CORE_FIELDS];
    bool  loaded;
    char  cfg_path[256];
};

//==========================================================================
// CFG FILE I/O
//==========================================================================
static inline void cfg_write_field(const char *path, const char *key, const char *value) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[16384];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    buf[len] = '\0';
    fclose(f);

    char search[128];
    snprintf(search, sizeof(search), "%s=", key);
    char *pos = strstr(buf, search);
    if (!pos) return;

    char *eol = pos;
    while (*eol && *eol != '\n' && *eol != '\r') eol++;

    char newbuf[16384];
    size_t prefix_len = pos - buf;
    memcpy(newbuf, buf, prefix_len);
    int written = snprintf(newbuf + prefix_len, sizeof(newbuf) - prefix_len, "%s=%s", key, value);
    size_t suffix_start = eol - buf;
    memcpy(newbuf + prefix_len + written, buf + suffix_start, len - suffix_start);

    f = fopen(path, "w");
    if (f) {
        fwrite(newbuf, 1, prefix_len + written + (len - suffix_start), f);
        fclose(f);
    }
}

static inline void Settings_Load(SettingsState *s) {
    // zero per-core overrides up front; populated below if cfg has them
    for (int c = 0; c < MAX_GUI_CORES; ++c)
        for (int j = 0; j < NUM_PER_CORE_FIELDS; ++j)
            s->per_core_vals[c][j] = 0.0f;

    FILE *f = fopen(s->cfg_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        // try matching against global field_defs first
        bool matched = false;
        for (int i = 0; i < NUM_FIELDS; i++) {
            size_t klen = strlen(field_defs[i].key);
            if (strncmp(p, field_defs[i].key, klen) == 0 && p[klen] == '=') {
                const char *val = p + klen + 1;
                if (field_defs[i].type == CFG_PATH) {
                    // strip trailing whitespace/newline
                    strncpy(s->path_vals[i], val, 511);
                    s->path_vals[i][511] = '\0';
                    char *end = s->path_vals[i] + strlen(s->path_vals[i]) - 1;
                    while (end > s->path_vals[i] && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
                } else if (field_defs[i].type == CFG_BOOL)
                    s->bool_vals[i] = atoi(val);
                else if (field_defs[i].type == CFG_INT)
                    s->float_vals[i] = (float)atoi(val);
                else
                    s->float_vals[i] = (float)atof(val);
                matched = true;
                break;
            }
        }
        if (matched) continue;

        // v4.0 per-core override: parse `core_<N>_<suffix>=<value>`
        if (strncmp(p, "core_", 5) == 0) {
            int core_idx = atoi(p + 5);
            const char *us = p + 5;
            while (*us && *us != '_') us++;
            if (*us == '_' && core_idx >= 0 && core_idx < MAX_GUI_CORES) {
                const char *suffix = us + 1;
                for (int j = 0; j < NUM_PER_CORE_FIELDS; ++j) {
                    size_t slen = strlen(per_core_fields[j].key_suffix);
                    if (strncmp(suffix, per_core_fields[j].key_suffix, slen) == 0 &&
                        suffix[slen] == '=') {
                        s->per_core_vals[core_idx][j] = (float)atof(suffix + slen + 1);
                        break;
                    }
                }
            }
        }
    }
    fclose(f);
    s->loaded = true;
}

//==========================================================================
// GLOBAL TAB — renders the auto-generated field_defs[] layout
//==========================================================================
static inline bool Settings_RenderGlobalTab(SettingsState *s) {
    bool changed = false;
    const char *current_section = NULL;

    for (int i = 0; i < NUM_FIELDS; i++) {
        const CfgFieldDef *fd = &field_defs[i];

        // auto collapsing headers by section name
        if (!current_section || strcmp(current_section, fd->section) != 0) {
            current_section = fd->section;
            bool default_open = (strcmp(fd->section, "Trading") == 0 ||
                                strcmp(fd->section, "Entry Filters") == 0 ||
                                strcmp(fd->section, "EMA Gate") == 0);
            if (!ImGui::CollapsingHeader(fd->section,
                    default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0))
            {
                // skip all fields in this collapsed section
                while (i + 1 < NUM_FIELDS && strcmp(field_defs[i + 1].section, fd->section) == 0)
                    i++;
                continue;
            }
        }

        if (fd->type == CFG_FLOAT) {
            ImGui::SetNextItemWidth(80);
            ImGui::InputFloat(fd->label, &s->float_vals[i], 0, 0, fd->fmt);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                char v[32];
                snprintf(v, 32, fd->fmt, s->float_vals[i]);
                cfg_write_field(s->cfg_path, fd->key, v);
                changed = true;
            }
        } else if (fd->type == CFG_INT) {
            int iv = (int)s->float_vals[i];
            ImGui::SetNextItemWidth(80);
            ImGui::InputInt(fd->label, &iv, 0, 0);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                s->float_vals[i] = (float)iv;
                char v[16];
                snprintf(v, 16, "%d", iv);
                cfg_write_field(s->cfg_path, fd->key, v);
                changed = true;
            }
        } else if (fd->type == CFG_BOOL) {
            bool bv = s->bool_vals[i] != 0;
            if (ImGui::Checkbox(fd->label, &bv)) {
                s->bool_vals[i] = bv ? 1 : 0;
                cfg_write_field(s->cfg_path, fd->key, bv ? "1" : "0");
                changed = true;
            }
            // warning label for dangerous toggles
            if (bv && strcmp(fd->key, "use_real_money") == 0) {
                ImGui::SameLine();
                ImGui::TextColored(FoxmlColors::red_b, "REAL MONEY");
            }
            if (bv && strcmp(fd->key, "gate_ema_enabled") == 0) {
                ImGui::SameLine();
                ImGui::TextColored(FoxmlColors::green_b, "ACTIVE");
            }
        } else if (fd->type == CFG_PATH) {
            ImGui::SetNextItemWidth(200);
            ImGui::InputText(fd->label, s->path_vals[i], 256);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                cfg_write_field(s->cfg_path, fd->key, s->path_vals[i]);
                changed = true;
            }
        }

        // hover tooltip from field_defs — inline, no separate lookup chain
        if (fd->tooltip)
            ImGui::SetItemTooltip("%s", fd->tooltip);
    }
    return changed;
}

//==========================================================================
// PER-CORE TAB — renders one core's PerCoreOverrides editor
//==========================================================================
// Each row is one override. Empty/0 = inherit from Global. The current value
// from the Global tab is shown next to the input as a small grey hint.
static inline bool Settings_RenderPerCoreTab(SettingsState *s, int core_id) {
    bool changed = false;

    ImGui::TextColored(FoxmlColors::comment,
        "Empty (0.00) means \"inherit from Global tab\". "
        "Set any override to use that value for this core only.");

    const char *current_section = NULL;
    for (int j = 0; j < NUM_PER_CORE_FIELDS; ++j) {
        const PerCoreFieldDef *pcf = &per_core_fields[j];
        if (!current_section || strcmp(current_section, pcf->section) != 0) {
            current_section = pcf->section;
            ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_DefaultOpen;
            if (!ImGui::CollapsingHeader(pcf->section, fl)) {
                while (j + 1 < NUM_PER_CORE_FIELDS &&
                       strcmp(per_core_fields[j + 1].section, pcf->section) == 0)
                    ++j;
                continue;
            }
        }
        ImGui::PushID(j);  // disambiguate same-named labels across the 18 rows
        ImGui::SetNextItemWidth(80);
        ImGui::InputFloat(pcf->label, &s->per_core_vals[core_id][j], 0, 0, pcf->fmt);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            char key[64];
            snprintf(key, sizeof(key), "core_%d_%s", core_id, pcf->key_suffix);
            char val[32];
            snprintf(val, sizeof(val), pcf->fmt, s->per_core_vals[core_id][j]);
            cfg_write_field(s->cfg_path, key, val);
            changed = true;
        }
        if (pcf->tooltip)
            ImGui::SetItemTooltip("%s", pcf->tooltip);
        ImGui::PopID();
    }
    return changed;
}

//==========================================================================
// RENDER — tabbed: Global + Core 0..N
//==========================================================================
// live_core_count > 0 → use it (number of cores actually registered with the
// engine). 0 → fall back to the cfg's num_execution_cores field. Reflects
// running cores, not cfg-only intent — engine doesn't add/remove cores live.
static inline void GUI_Panel_Settings(SettingsState *s,
                                       volatile sig_atomic_t *reload_flag,
                                       int live_core_count = 0) {
    ImGui::Begin("Settings");

    if (!s->loaded) Settings_Load(s);

    ImGui::TextColored(FoxmlColors::primary, "ENGINE SETTINGS");
    ImGui::TextColored(FoxmlColors::comment, "edit + press Enter to apply");
    ImGui::Separator();

    // Tabs match live registered cores when available; else fall back to
    // cfg num_execution_cores; else default 4. Avoids the "I have 4 cores
    // but only 1 tab" bug when num_execution_cores is missing from cfg
    // (cfg defaults to 4 on the engine side, but Settings_Load only sees
    // what's literally written in the file).
    int num_cores = 0;
    if (live_core_count > 0 && live_core_count <= MAX_GUI_CORES) {
        num_cores = live_core_count;
    } else {
        for (int i = 0; i < NUM_FIELDS; ++i) {
            if (strcmp(field_defs[i].key, "num_execution_cores") == 0) {
                num_cores = (int)s->float_vals[i];
                break;
            }
        }
        if (num_cores < 1) num_cores = 4;  // safe default = engine's default
        if (num_cores > MAX_GUI_CORES) num_cores = MAX_GUI_CORES;
    }

    bool changed = false;
    if (ImGui::BeginTabBar("##settings_tabs")) {
        if (ImGui::BeginTabItem("Global")) {
            if (Settings_RenderGlobalTab(s)) changed = true;
            ImGui::EndTabItem();
        }
        for (int c = 0; c < num_cores; ++c) {
            char tab_label[16];
            snprintf(tab_label, sizeof(tab_label), "Core %d", c);
            if (ImGui::BeginTabItem(tab_label)) {
                if (Settings_RenderPerCoreTab(s, c)) changed = true;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    if (changed) {
        __atomic_store_n(reload_flag, 1, __ATOMIC_RELEASE);
        ImGui::TextColored(FoxmlColors::green_b, "saved + reloaded");
    }

    ImGui::End();
}
