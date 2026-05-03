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
#include "../DataStream/EngineTUI.hpp"  // TUISharedState, TUISnapshot for per-core core-config
#include "../Strategies/StrategyInterface.hpp"  // STRATEGY_* + NUM_STRATEGIES + SHORT_NAMES
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <dirent.h>     // v5.9.5f — opendir for model directory scan
#include <sys/stat.h>   // v5.9.5f — stat for role-file detection

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
    // v4.7.29: Adaptation, Trailing TP/SL, Time-Based Exit moved to per-core
    // tabs. These were exit/feedback policies that varied by strategy
    // (DIP wants short holds, EMA Cross wants long; Momentum wants tighter
    // R² gates, etc.). Set them per-core via each tab's override sections.
    // max_hold_ticks (uint32) stays global — INT support for X-macro is a
    // future extension; min_hold_gain_pct moved to per-core handles the
    // common case (different strategies want different hold-gain floors).
    {"max_hold_ticks",        "Max Hold",     "Time-Based Exit", CFG_INT,   "%d",
        "Close position after this many ticks (engine-wide).\n"
        "0 = disabled, 75000 ≈ 4-5 hours.\n"
        "Per-core min-gain floor lives in each core's Time Exit override."},
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
    // v4.7.29: Vol Sizing + No-Trade Band scale curves moved to per-core
    // tabs. Toggles stay global (engine-architectural enable/disable).
    {"vol_sizing_enabled",    "Vol Sizing##bool",   "Vol Sizing",      CFG_BOOL,  NULL,
        "Engine-wide enable for vol sizing.\nPer-core scale_min/scale_max overrides live in each core's Vol Sizing section."},
    {"no_trade_band_enabled", "No-Trade Band##bool","No-Trade Band",   CFG_BOOL,  NULL,
        "Engine-wide enable for the no-trade band.\nPer-core band multiplier override lives in each core's No-Trade Band section."},
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
    // (Momentum + EMA Cross strategy tuning consolidated into "Momentum
    //  Tuning" / "EMA Cross Tuning" sections below — v4.7.22 dedup pass.)
    // v4.7.29: Partial Exits geometry (split %, TP2 mult) moved to per-core.
    // breakeven_on_partial stays global (single bool toggle).
    {"breakeven_on_partial",  "Breakeven SL", "Partial Exits",   CFG_BOOL,  NULL,
        "Engine-wide: ratchet leg-B SL to entry after leg A TP1 hits.\n"
        "Per-core split % and TP2 mult overrides live in each core's Partial Exits section."},
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
    // v4.7.27: "Strategy" section dropped. Per-core dropdowns in each
    // core's tab are the canonical strategy assignment surface. cfg's
    // default_strategy=N still parses for legacy single_core boot.
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
    {"partial_exit_enabled",  "Partial Exits##toggle","Toggles",         CFG_BOOL,  NULL,   NULL},
    {"session_filter_enabled","Session Filter","Toggles",        CFG_BOOL,  NULL,   NULL},
    {"depth_enabled",         "Order Book",   "Toggles",         CFG_BOOL,  NULL,   NULL},
    {"min_book_imbalance",    "Book Imbal",   "Toggles",         CFG_FLOAT, "%.2f", NULL},
    // FoxML integration — engine-wide enable/disable + training-time defaults
    // (per-core FPN tuning lives in each ML core's "ML" override section).
    {"cost_gate_enabled",        "Cost Gate",         "FoxML",  CFG_BOOL,  NULL,
        "Engine-wide enable for cost gate (suppress entries when estimated\n"
        "trade cost exceeds TP target). Per-core FPN knobs live in each\n"
        "ML core's tab."},
    {"foxml_vol_scaling_enabled","Vol Scaling",        "FoxML",  CFG_BOOL,  NULL,
        "Engine-wide enable for FoxML vol scaling.\n"
        "Per-core Vol Z-Max override lives in each ML core's tab."},
    {"bandit_enabled",           "Bandit",            "FoxML",  CFG_BOOL,  NULL,
        "Engine-wide enable for Exp3-IX bandit blending.\n"
        "Per-core Blend Ratio override lives in each ML core's tab."},
    {"confidence_enabled",       "Confidence",        "FoxML",  CFG_BOOL,  NULL,
        "Engine-wide enable for dynamic ML threshold scaling.\n"
        "Per-core Tau / Scale overrides live in each ML core's tab."},
    {"confidence_window",        "Conf Window",       "FoxML",  CFG_INT,   "%d",
        "RollingIC + RollingRMSE window size (engine-wide; cap 64).\n"
        "Same window per ML core today; INT support for X-macro deferred."},
    // Validation — training-time held-out gating (engine-wide).
    {"held_out_fraction",        "Held-Out %",         "Validation", CFG_FLOAT, "%.2f",
        "Fraction of data reserved as held-out test set (training-time).\n"
        "Clamped [0.05, 0.30]. Engine-wide setup; one bundle per training run."},
    {"gap_acceptable_threshold", "Gap Threshold",      "Validation", CFG_FLOAT, "%.3f",
        "Max acceptable WF↔held-out generalization gap (training-time).\n"
        "Engine-wide quality bar for ALL trained models in this session."},
    // v4.7.31: ML model paths + barrier gate stay engine-wide for now.
    // ml_model_path is already overridable per-core via core_N_model_path;
    // regime / peak / valley paths don't have per-core storage yet —
    // adding it requires struct/parser changes across ControllerConfig
    // (deferred). Hidden when no core uses STRATEGY_ML by v4.7.30 filter.
    {"ml_model_path",            "Buy Model",         "Models", CFG_PATH,  NULL,
        "Path to XGBoost/LightGBM buy-signal model.\n"
        "Per-core override available via core_N_model_path in each ML core's tab."},
    {"regime_model_path",        "Regime Model",      "Models", CFG_PATH,  NULL,
        "Path to regime enrichment model (engine-wide). Per-core deferred."},
    {"barrier_gate_enabled",     "Barrier Gate",      "Barrier", CFG_BOOL, NULL,
        "Block entries before predicted price peaks (engine-wide).\nrequires trained peak/valley models"},
    {"peak_model_path",          "Peak Model",        "Barrier", CFG_PATH, NULL,
        "Path to P(will_peak) model (engine-wide). Per-core deferred."},
    {"valley_model_path",        "Valley Model",      "Barrier", CFG_PATH, NULL,
        "Path to P(will_valley) model (engine-wide). Per-core deferred."},
    // Per-core sharded engine — production since v4.x; legacy single_core is
    // deprecated and warns on boot. v4.7.26: removed the "Sharded Mode" toggle
    // from the GUI — sharded is the only path users should see. Cfg parser
    // still accepts engine_mode= for backwards compat with old cfg files;
    // users who really want legacy can hand-edit. No UI surface = no foot-gun.
    {"num_execution_cores",      "Cores",             "Per-Core", CFG_INT,  "%d",
        "Number of execution cores in sharded mode (1-16).\n"
        "Each core handles one position at a time (or two with partial exits).\n"
        "Recommended: physical core count - 2 (one for controller, one for OS).\n"
        "On AMD: pin all cores to the same CCD to avoid cross-die latency.\n"
        "RESTART REQUIRED to take effect."},
    // v4.7.31: "Core Strategies" + "Core Risk" summary sections removed.
    // These were duplicate views of per-core Strategy + Risk %% controls
    // already present in each per-core tab's "Core Configuration" section.
    // Cfg parser still accepts core_N_strategy / core_N_risk_pct.
    // v4.7.27: strategy-tuning sections moved EXCLUSIVELY to per-core tabs.
    // Pre-v4.7.27 the Global tab also exposed SimpleDip/MR/Momentum/EMA Cross
    // Tuning fields as "shared default with per-core override" — that
    // hierarchy was confusion bloat. Per-core sharded means each core IS
    // a strategy instance; "what controls Core 1?" should be answered by
    // Core 1's tab alone, not by mentally merging Global + override.
    // Cfg parser still accepts global keys (e.g. simpledip_tp_pct=4.0) for
    // backwards compat with older cfg files; resolver still treats them as
    // the fallback when no per-core override is set. Just no UI surface
    // here — set them via the per-core tab's Strategy-Specific section.
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
    // v5.9.5h — XGBoost training hyperparams (cfg-tunable subset).
    // Live engine doesn't TRAIN; these fields participate in load-time WARN
    // when stamp's recorded value differs from cfg's. Set them to MATCH the
    // values used to train the model you're deploying — eliminates startup
    // noise + provides explicit drift detection. Suppressible via
    // acknowledge_cross_binary_version_drift=1.
    {"xgb_subsample",            "Subsample",         "ML Hyperparams",  CFG_FLOAT, "%.2f",
        "Row subsample per tree (0.5-1.0). Lower = more variance reduction.\n"
        "Default 0.8. Set to MATCH the value used to train the deployed model;\n"
        "engine WARNs at boot if stamp's recorded value differs."},
    {"xgb_colsample_bytree",     "ColSample/Tree",    "ML Hyperparams",  CFG_FLOAT, "%.2f",
        "Column subsample per tree (0.5-1.0). Lower = less feature-importance\n"
        "bias. Default 0.8. Match deployed model's training value or expect WARN."},
    {"xgb_min_child_weight",     "Min Child Weight",  "ML Hyperparams",  CFG_INT,   "%d",
        "Min sum-of-weights per leaf (1-50). Higher = more regularization.\n"
        "Default 5. Match deployed model's training value or expect WARN."},
    {"xgb_seed",                 "Seed",              "ML Hyperparams",  CFG_INT,   "%d",
        "RNG seed for reproducible runs. Default 42. Match deployed model's\n"
        "training seed or expect WARN."},
    {"xgb_tree_method",          "Tree Method",       "ML Hyperparams",  CFG_PATH,  "%s",
        "XGBoost tree construction algorithm: hist (fast, default) | exact |\n"
        "approx | auto. Match deployed model's training method or expect WARN."},
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
    {"momentum_r2_min",   "MOM R² Min",  "Strategy-Specific", "%.2f",
        "Min R² to enter momentum trades on this core. 0 = inherit."},
    {"emacross_tp_pct",   "EMA TP %%",   "Strategy-Specific", "%.2f",
        "EMA-only TP override for this core. 0 = inherit."},
    {"emacross_sl_pct",   "EMA SL %%",   "Strategy-Specific", "%.2f",
        "EMA-only SL override for this core. 0 = inherit."},
    {"emacross_dip_mult", "EMA Dip σ",   "Strategy-Specific", "%.2f",
        "Buy this many stddevs below EMA in uptrends. 0 = inherit."},
    {"emacross_crossover_min", "EMA Cross Min", "Strategy-Specific", "%.4f",
        "Min EMA-SMA spread for uptrend confirmation. 0 = inherit."},
    {"emacross_trail_mult", "EMA Trail σ", "Strategy-Specific", "%.2f",
        "Trailing TP factor when EMA rising. 0 = inherit."},
    {"ml_tp_pct",         "ML TP %%",    "Strategy-Specific", "%.2f",
        "ML-only TP override for this core. 0 = inherit."},
    {"ml_sl_pct",         "ML SL %%",    "Strategy-Specific", "%.2f",
        "ML-only SL override for this core. 0 = inherit."},
    {"ml_buy_threshold",  "ML Threshold","Strategy-Specific", "%.3f",
        "ML buy threshold override for this core (0-1). 0 = inherit."},
    // v4.7.29: per-core adaptation overrides — adaptive feedback per core.
    // Different strategies want different reactivity: MR with deep adaptation,
    // Momentum with tighter R² gates, etc.
    {"filter_scale",      "Filter Scale", "Adaptation",       "%.2f",
        "How fast filters adapt to P&L regression for this core. 0 = inherit."},
    {"r2_threshold",      "R² Threshold", "Adaptation",       "%.2f",
        "Min R² to trust this core's regression model. 0 = inherit."},
    {"slope_scale_buy",   "Slope Scale",  "Adaptation",       "%.2f",
        "How much slope shifts buy threshold for this core. 0 = inherit."},
    {"max_shift",         "Max Shift",    "Adaptation",       "%.4f",
        "Max drift from initial buy conditions for this core. 0 = inherit."},
    {"offset_min",        "Offset Min %%","Adaptation",       "%.3f",
        "Most aggressive entry_offset_pct floor for this core. 0 = inherit."},
    {"offset_max",        "Offset Max %%","Adaptation",       "%.3f",
        "Most defensive entry_offset_pct ceiling for this core. 0 = inherit."},
    {"vol_mult_min",      "Vol Min",      "Adaptation",       "%.2f",
        "Most aggressive volume_multiplier floor for this core. 0 = inherit."},
    {"vol_mult_max",      "Vol Max",      "Adaptation",       "%.2f",
        "Most defensive volume_multiplier ceiling for this core. 0 = inherit."},
    // v4.7.29: trailing TP/SL exit ratchet, per core.
    {"tp_hold_score",     "Hold Score",   "Trailing",         "%.2f",
        "Min SNR*R² to activate trailing for this core. 0 = inherit."},
    {"tp_trail_mult",     "Trail TP",     "Trailing",         "%.2f",
        "Trailing TP distance multiplier for this core. 0 = inherit."},
    {"sl_trail_mult",     "Trail SL",     "Trailing",         "%.2f",
        "Trailing SL distance multiplier for this core. 0 = inherit."},
    // v4.7.29: time exit gain floor, per core (max_hold_ticks stays global).
    {"min_hold_gain_pct", "Min Gain %%",  "Time Exit",        "%.2f",
        "Only time-exit if gain below this %% for this core. 0 = inherit."},
    // v4.7.29: vol sizing curve, per core.
    {"vol_scale_min",     "Scale Min",    "Vol Sizing",       "%.2f",
        "Min position scale factor for this core. 0 = inherit."},
    {"vol_scale_max",     "Scale Max",    "Vol Sizing",       "%.2f",
        "Max position scale factor for this core. 0 = inherit."},
    // v4.7.29: no-trade band fee multiplier, per core.
    {"no_trade_band_mult","Band Mult",    "No-Trade Band",    "%.2f",
        "Signal must exceed fee_rate * this to trade for this core. 0 = inherit."},
    // v4.7.29: partial exit geometry, per core. partial_exit_enabled stays
    // global (engine-architectural).
    {"partial_exit_pct",  "TP1 Split",    "Partial Exits",    "%.2f",
        "Fraction to exit at TP1 for this core (0.5 = 50%). 0 = inherit."},
    {"tp2_mult",          "TP2 Mult",     "Partial Exits",    "%.2f",
        "TP2 distance = TP1 distance * this for this core. 0 = inherit."},
    // v4.7.31: ML / FoxML overrides — only render when this core uses ML.
    // Strategy filter (per_core_field_strategy) maps any "ml_*", "bandit_*",
    // "foxml_*", "confidence_*" prefix to STRATEGY_ML so they only show on
    // ML cores (or AUTO — no, AUTO doesn't route to ML per v4.7.30).
    {"foxml_vol_scaling_z_max",   "Vol Z-Max",      "ML",  "%.1f",
        "Z-score clipping for vol scaler on this core. 0 = inherit."},
    {"bandit_blend_ratio",        "Bandit Blend",   "ML",  "%.2f",
        "Max bandit influence fraction for this core (0.30 = 30%). 0 = inherit."},
    {"confidence_freshness_tau",  "Conf Tau (s)",   "ML",  "%.0f",
        "Freshness decay constant in seconds for this core. 0 = inherit."},
    {"confidence_threshold_scale","Conf Scale",     "ML",  "%.2f",
        "Confidence gate scale: effective_thr = base * (this - conf). 0 = inherit."},
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
    // v4.0.4 per-core "core configuration" — strategy / risk / model. These
    // can't share per_core_vals[] because they have heterogeneous types
    // (string + dropdown + float + path). Loaded by Settings_Load alongside
    // per_core_vals.
    int   per_core_strategy[MAX_GUI_CORES];   // chosen STRATEGY_* index for dropdown (-1 = unset / use cfg)
    float per_core_risk_pct[MAX_GUI_CORES];   // 0 = inherit (risk_pct / num_cores)
    char  per_core_model_path[MAX_GUI_CORES][256];
    char  per_core_model_dir[MAX_GUI_CORES][256];
    bool  loaded;
    char  cfg_path[256];
    // v5.9.5f — model directory cache. Refresh-button-driven (no per-frame
    // I/O — render thread blocking is forbidden, see /readiness check 17).
    // Populated by SettingsPanel_RescanModels which walks `models/` and
    // detects subdirs containing a recognizable role file. Reuses the same
    // file-detection logic as PastRuns_LoadOne (BacktestPanels.hpp:629).
    // Capped at 32 model dirs — enough for any realistic deployment.
    static constexpr int MODEL_SCAN_MAX = 32;
    int   model_scan_count;
    char  model_scan_dirs[MODEL_SCAN_MAX][96];   // subdir name only (e.g. "aggressive")
    char  model_scan_paths[MODEL_SCAN_MAX][256]; // full path "models/aggressive"
    bool  model_scan_done;                        // 1 after first scan
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
    size_t klen = strlen(search);

    // Line-anchored search: walk lines, match start-of-line. Naive
    // strstr matches `mr_tp_pct=` inside `core_0_mr_tp_pct=` — wrong key.
    // v4.0 per-core keys make that collision common; line anchoring is
    // load-bearing.
    char *pos = NULL;
    char *p = buf;
    while (*p) {
        // skip leading whitespace on this line
        char *line_start = p;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, search, klen) == 0) {
            pos = p;
            break;
        }
        // advance to next line
        p = line_start;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    if (!pos) {
        // Key not in file — append a new line at the end. The whole point
        // of the v4.0 per-core tabs is creating overrides that don't yet
        // exist; the prior "silently drop" behavior broke that flow.
        f = fopen(path, "a");
        if (f) {
            // make sure we start on a fresh line
            if (len > 0 && buf[len - 1] != '\n') fputc('\n', f);
            fprintf(f, "%s=%s\n", key, value);
            fclose(f);
        }
        return;
    }

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

// Map cfg strategy name (e.g. "simple_dip") to STRATEGY_* index. Returns -1
// on unknown. Mirror of the parser block in ControllerConfig_Load.
//==========================================================================
// MODEL DIRECTORY SCAN (v5.9.5f)
//==========================================================================
// Walk `models/` and collect subdirectories that contain a recognizable
// role file. Reuses the same role-detection list as PastRuns_LoadOne
// (BacktestPanels.hpp:629) and Verify Stamp (BacktestPanels.hpp:1001-1005).
// Result populates SettingsState.model_scan_* — feeds the Model Dir
// Combo dropdown in per-core tabs.
//
// Refresh-button-driven (no per-frame I/O): operator clicks "Refresh
// Models" or panel auto-rescans on first appearing. ImGui render thread
// stays free of opendir/stat (per /readiness check 17 hardening).
static inline void Settings_RescanModels(SettingsState *s) {
    s->model_scan_count = 0;
    s->model_scan_done = true;
    DIR *dir = opendir("models");
    if (!dir) return;
    static const char* role_files[] = {
        "barrier.json", "buy_signal.json", "regime.json",
        "barrier.xgb",  "buy_signal.xgb",  "regime.xgb",
        "barrier.bin",  "buy_signal.bin",  "regime.bin",
        nullptr
    };
    struct dirent *de;
    while ((de = readdir(dir)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        char dir_path[256];
        snprintf(dir_path, sizeof(dir_path), "models/%s", de->d_name);
        struct stat st;
        if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        // Detect at least one role file inside
        bool has_role = false;
        for (int i = 0; role_files[i]; ++i) {
            char full[400];
            snprintf(full, sizeof(full), "%s/%s", dir_path, role_files[i]);
            struct stat fst;
            if (stat(full, &fst) == 0 && S_ISREG(fst.st_mode)) {
                has_role = true;
                break;
            }
        }
        if (!has_role) continue;
        if (s->model_scan_count >= SettingsState::MODEL_SCAN_MAX) break;
        size_t n = strnlen(de->d_name, sizeof(s->model_scan_dirs[0]) - 1);
        memcpy(s->model_scan_dirs[s->model_scan_count], de->d_name, n);
        s->model_scan_dirs[s->model_scan_count][n] = '\0';
        size_t pn = strnlen(dir_path, sizeof(s->model_scan_paths[0]) - 1);
        memcpy(s->model_scan_paths[s->model_scan_count], dir_path, pn);
        s->model_scan_paths[s->model_scan_count][pn] = '\0';
        s->model_scan_count++;
    }
    closedir(dir);
}

static inline int settings_strategy_name_to_id(const char *name) {
    if (strcmp(name, "mr") == 0 || strcmp(name, "mean_reversion") == 0) return 0;  // STRATEGY_MEAN_REVERSION
    if (strcmp(name, "momentum") == 0 || strcmp(name, "mom") == 0)      return 1;  // STRATEGY_MOMENTUM
    if (strcmp(name, "simple_dip") == 0 || strcmp(name, "dip") == 0)    return 2;  // STRATEGY_SIMPLE_DIP
    if (strcmp(name, "ml") == 0)                                        return 3;  // STRATEGY_ML
    if (strcmp(name, "ema_cross") == 0 || strcmp(name, "ema") == 0)     return 4;  // STRATEGY_EMA_CROSS
    if (strcmp(name, "auto") == 0)                                      return 5;  // STRATEGY_AUTO
    if (strcmp(name, "none") == 0)                                      return -1;
    return -1;
}

// v5.8.4b: uniform `void X_Init(StateT*, const char*)` signature for the
// FOREACH_PANEL(X) registry in GuiThread.hpp. Settings has lazy file
// load (Settings_Load fires on first GUI_Panel_Settings render when
// !s->loaded), so Init's job is just: zero the struct + stash cfg_path
// so Settings_Load knows where to read.
static inline void Settings_Init(SettingsState *s, const char *cfg_path) {
    *s = SettingsState{};
    if (cfg_path) {
        size_t n = strlen(cfg_path);
        if (n >= sizeof(s->cfg_path)) n = sizeof(s->cfg_path) - 1;
        memcpy(s->cfg_path, cfg_path, n);
        s->cfg_path[n] = '\0';
    }
}

static inline void Settings_Load(SettingsState *s) {
    // zero per-core overrides up front; populated below if cfg has them
    for (int c = 0; c < MAX_GUI_CORES; ++c) {
        for (int j = 0; j < NUM_PER_CORE_FIELDS; ++j)
            s->per_core_vals[c][j] = 0.0f;
        s->per_core_strategy[c] = -1;
        s->per_core_risk_pct[c] = 0.0f;
        s->per_core_model_path[c][0] = '\0';
        s->per_core_model_dir[c][0]  = '\0';
    }

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
                bool pc_matched = false;
                for (int j = 0; j < NUM_PER_CORE_FIELDS; ++j) {
                    size_t slen = strlen(per_core_fields[j].key_suffix);
                    if (strncmp(suffix, per_core_fields[j].key_suffix, slen) == 0 &&
                        suffix[slen] == '=') {
                        s->per_core_vals[core_idx][j] = (float)atof(suffix + slen + 1);
                        pc_matched = true;
                        break;
                    }
                }
                if (pc_matched) continue;
                // v4.0.4 core-configuration keys (heterogeneous types — not
                // in per_core_fields[]).
                if (strncmp(suffix, "strategy=", 9) == 0) {
                    char nm[32];
                    strncpy(nm, suffix + 9, sizeof(nm) - 1);
                    nm[sizeof(nm) - 1] = '\0';
                    char *end = nm + strlen(nm) - 1;
                    while (end > nm && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
                    s->per_core_strategy[core_idx] = settings_strategy_name_to_id(nm);
                } else if (strncmp(suffix, "risk_pct=", 9) == 0) {
                    s->per_core_risk_pct[core_idx] = (float)atof(suffix + 9);
                } else if (strncmp(suffix, "model_path=", 11) == 0) {
                    strncpy(s->per_core_model_path[core_idx], suffix + 11, 255);
                    s->per_core_model_path[core_idx][255] = '\0';
                    char *end = s->per_core_model_path[core_idx] + strlen(s->per_core_model_path[core_idx]) - 1;
                    while (end > s->per_core_model_path[core_idx] && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
                } else if (strncmp(suffix, "model_dir=", 10) == 0) {
                    strncpy(s->per_core_model_dir[core_idx], suffix + 10, 255);
                    s->per_core_model_dir[core_idx][255] = '\0';
                    char *end = s->per_core_model_dir[core_idx] + strlen(s->per_core_model_dir[core_idx]) - 1;
                    while (end > s->per_core_model_dir[core_idx] && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
                }
            }
        }
    }
    fclose(f);
    s->loaded = true;

    // v4.7.22: post-load defaults for fields the cfg may not have written.
    // Without this, the widget shows 0 even though the engine boots with
    // the default. Only patch num_execution_cores here — engine_mode is
    // boolean and we can't distinguish "missing from cfg" from "explicitly
    // 0", so flipping it would override user intent.
    for (int i = 0; i < NUM_FIELDS; ++i) {
        if (strcmp(field_defs[i].key, "num_execution_cores") == 0 &&
            s->float_vals[i] < 1.0f) {
            s->float_vals[i] = 4.0f;  // matches ControllerConfig_Default
        }
    }
}

// v4.7.23: map a Global-tab section to the strategy it applies to.
// Returns -1 if the section is strategy-agnostic (Trading defaults, Risk
// Management, Regime Detection, etc. — always visible). Otherwise returns
// the STRATEGY_* constant; the section is hidden when no configured core
// uses that strategy.
static inline int global_section_strategy(const char *section) {
    if (strcmp(section, "SimpleDip Tuning")     == 0) return STRATEGY_SIMPLE_DIP;
    if (strcmp(section, "MeanReversion Tuning") == 0) return STRATEGY_MEAN_REVERSION;
    if (strcmp(section, "Momentum Tuning")      == 0) return STRATEGY_MOMENTUM;
    if (strcmp(section, "EMA Cross Tuning")     == 0) return STRATEGY_EMA_CROSS;
    if (strcmp(section, "FoxML")                == 0) return STRATEGY_ML;
    if (strcmp(section, "Validation")           == 0) return STRATEGY_ML;
    if (strcmp(section, "Models")               == 0) return STRATEGY_ML;
    if (strcmp(section, "Barrier")              == 0) return STRATEGY_ML;
    return -1;
}

// True when any configured core is running this strategy.
// v4.7.30: AUTO routes to MR/Momentum/SimpleDip/EMA Cross only (not ML).
// Pre-v4.7.30 treated AUTO as "matches everything" — that surfaced ML
// sections (FoxML/Validation/Models/Barrier) in Global whenever any
// core was AUTO, even though AUTO never routes to ML.
//
// Source of truth: SettingsState's per_core_strategy[] (user intent —
// what they have configured, may differ from live until Apply pressed).
static inline bool any_core_uses_strategy(const SettingsState *s, int strat) {
    for (int c = 0; c < MAX_GUI_CORES; ++c) {
        int sid = s->per_core_strategy[c];
        if (sid < 0) continue;
        if (sid == strat) return true;
        // AUTO routes to MR/MOM/EMA/DIP only — NOT ML.
        if (sid == STRATEGY_AUTO && strat != STRATEGY_ML) return true;
    }
    return false;
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
            // v4.7.23: hide strategy-specific sections when no configured
            // core uses that strategy. AUTO cores match all strategies.
            int sec_strat = global_section_strategy(current_section);
            if (sec_strat >= 0 && !any_core_uses_strategy(s, sec_strat)) {
                while (i + 1 < NUM_FIELDS &&
                       strcmp(field_defs[i + 1].section, current_section) == 0)
                    i++;
                continue;
            }
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
//
// v4.0.4: optional `shared` + `snap` enable the "Core Configuration" section
// at the top of each per-core tab — strategy hot-swap dropdown, risk_pct,
// model_path, model_dir. Folded in from the standalone "Per-Core Strategy"
// panel so per-core knobs live in one place. Pass NULL to skip the
// hot-swap UI (tests / non-sharded callers).
// v4.7.23: map a per-core override key to the strategy it applies to.
// Returns -1 if the field is strategy-agnostic (TP/SL/entry-filter overrides
// always apply regardless of which strategy the core runs). Returns the
// STRATEGY_* constant for fields that ONLY apply when the core runs that
// strategy — those get hidden from the per-core tab when the core's resolved
// strategy doesn't match.
//
// Used by Settings_RenderPerCoreTab to scope the "Strategy-Specific" section
// to fields relevant to THIS core's strategy. AUTO cores show all (since
// AUTO routes to any strategy at runtime).
static inline int per_core_field_strategy(const char *key_suffix) {
    if (strncmp(key_suffix, "simpledip_", 10) == 0) return STRATEGY_SIMPLE_DIP;
    if (strncmp(key_suffix, "mr_",         3) == 0) return STRATEGY_MEAN_REVERSION;
    if (strncmp(key_suffix, "momentum_",   9) == 0) return STRATEGY_MOMENTUM;
    if (strncmp(key_suffix, "emacross_",   9) == 0) return STRATEGY_EMA_CROSS;
    if (strncmp(key_suffix, "ml_",         3) == 0) return STRATEGY_ML;
    // v4.7.31: ML-related ecosystem fields. ConfidenceScorer / Bandit /
    // FoxML vol scaling / Cost Gate / Barrier — all consumed only by
    // STRATEGY_ML cores, so the per-core overrides should only render
    // when this core's strategy is ML.
    if (strncmp(key_suffix, "confidence_", 11) == 0) return STRATEGY_ML;
    if (strncmp(key_suffix, "bandit_",      7) == 0) return STRATEGY_ML;
    if (strncmp(key_suffix, "foxml_",       6) == 0) return STRATEGY_ML;
    return -1;  // strategy-agnostic
}

// True when this per-core field should be VISIBLE on the tab for a core
// running `core_strategy`. STRATEGY_NONE shows nothing strategy-specific
// (only the agnostic overrides).
//
// v4.7.30: AUTO routes to MR/Momentum/SimpleDip/EMA Cross only — NOT ML.
// Pre-v4.7.30 AUTO showed ALL strategy-specific fields including ML's,
// which never matter for an AUTO core. Now AUTO matches everything except ML.
static inline bool per_core_field_visible(const char *key_suffix, int core_strategy) {
    int field_strat = per_core_field_strategy(key_suffix);
    if (field_strat < 0) return true;       // agnostic
    if (core_strategy == STRATEGY_AUTO) {
        return field_strat != STRATEGY_ML;
    }
    return core_strategy == field_strat;
}

static inline bool Settings_RenderPerCoreTab(SettingsState *s, int core_id,
                                              TUISharedState *shared = NULL,
                                              const TUISnapshot *snap = NULL) {
    bool changed = false;

    ImGui::TextColored(FoxmlColors::comment,
        "Empty (0.00) means \"inherit from Global tab\". "
        "Set any override to use that value for this core only.");

    // v4.0.4 — Core Configuration section. Strategy + risk + model path,
    // pulling from cfg-only fields (not per_core_fields[] which is float-
    // only). Strategy persists immediately on Apply via cfg_write_field
    // and signals the engine via swap_strategy_requested[] for hot-swap.
    if (ImGui::CollapsingHeader("Core Configuration", ImGuiTreeNodeFlags_DefaultOpen)) {
        // ---- strategy dropdown + Apply ----
        // Determine the live ACTIVE strategy from snapshot if present.
        // Otherwise read what's in cfg.
        int active_sid = -1;
        if (snap && snap->sharded_mode_active && core_id < snap->per_core_count) {
            active_sid = snap->per_core[core_id].strategy_id_display;
        } else if (s->per_core_strategy[core_id] >= 0) {
            active_sid = s->per_core_strategy[core_id];
        }
        // Initialize dropdown to the active strategy on first sight so it
        // doesn't default to "MR" for every core.
        int *chosen = &s->per_core_strategy[core_id];
        if (*chosen < 0 && active_sid >= 0) *chosen = active_sid;
        if (*chosen < 0) *chosen = 0;  // fallback for cores w/o cfg + no snapshot

        ImGui::Text("Strategy:");
        ImGui::SameLine();
        if (active_sid >= 0 && active_sid < NUM_STRATEGIES) {
            ImGui::TextColored(FoxmlColors::primary, "active=%s",
                               STRATEGY_SHORT_NAMES[active_sid]);
        } else {
            ImGui::TextDisabled("(no live core)");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::PushID("strat_combo");
        ImGui::Combo("##strat", chosen, STRATEGY_SHORT_NAMES, NUM_STRATEGIES);
        ImGui::PopID();
        ImGui::SameLine();
        bool same_as_active = (*chosen == active_sid);
        if (same_as_active) {
            ImGui::BeginDisabled();
            ImGui::Button("Active");
            ImGui::EndDisabled();
        } else {
            ImGui::PushID("strat_apply");
            if (ImGui::Button("Apply")) {
                if (shared && *chosen >= 0 && *chosen < NUM_STRATEGIES) {
                    __atomic_store_n(&shared->swap_strategy_requested[core_id],
                                     (uint8_t)*chosen, __ATOMIC_RELEASE);
                }
                static const char* strat_cfg_names[NUM_STRATEGIES] = {
                    "mr", "momentum", "simple_dip", "ml", "ema_cross", "auto"
                };
                if (*chosen >= 0 && *chosen < NUM_STRATEGIES) {
                    char key[64];
                    snprintf(key, sizeof(key), "core_%d_strategy", core_id);
                    cfg_write_field(s->cfg_path, key, strat_cfg_names[*chosen]);
                    changed = true;
                }
            }
            ImGui::PopID();
        }
        // pending swap status (when Apply was pressed but core still has open pos)
        if (shared) {
            uint8_t pending = __atomic_load_n(&shared->swap_strategy_requested[core_id],
                                              __ATOMIC_ACQUIRE);
            if (pending != STRATEGY_NONE) {
                ImGui::SameLine();
                const char* pname = pending < NUM_STRATEGIES ? STRATEGY_SHORT_NAMES[pending] : "?";
                ImGui::TextColored(FoxmlColors::yellow,
                    "swap → %s pending (waiting for position close)", pname);
            }
        }

        // ---- risk_pct ----
        ImGui::SetNextItemWidth(80);
        ImGui::PushID("risk");
        ImGui::InputFloat("Risk %", &s->per_core_risk_pct[core_id], 0, 0, "%.1f");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            char key[64];
            snprintf(key, sizeof(key), "core_%d_risk_pct", core_id);
            char val[32];
            snprintf(val, sizeof(val), "%.2f", s->per_core_risk_pct[core_id]);
            cfg_write_field(s->cfg_path, key, val);
            changed = true;
        }
        ImGui::PopID();
        ImGui::SetItemTooltip("Override per-core risk %% (0 = inherit risk_pct/num_cores). "
                              "Stored as `core_%d_risk_pct=N.NN` in cfg.", core_id);

        // ---- model_path / model_dir (ML cores) ----
        ImGui::SetNextItemWidth(360);
        ImGui::PushID("mpath");
        ImGui::InputText("Model Path", s->per_core_model_path[core_id], 256);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            char key[64];
            snprintf(key, sizeof(key), "core_%d_model_path", core_id);
            cfg_write_field(s->cfg_path, key, s->per_core_model_path[core_id]);
            changed = true;
        }
        ImGui::PopID();
        ImGui::SetItemTooltip("Single model file. Used by STRATEGY_ML cores. "
                              "Use Model Dir below for a CoreModelZoo with role auto-discovery.");

        // v5.9.5f — Model Dir is now a Combo populated from a scan of
        // `models/` (operator no longer types paths). Falls back to
        // InputText if scan found nothing (so operator isn't blocked).
        // Auto-rescans on first render; refresh button below for explicit.
        if (!s->model_scan_done) Settings_RescanModels(s);
        ImGui::SetNextItemWidth(360);
        ImGui::PushID("mdir");
        if (s->model_scan_count > 0) {
            // Find current selection (match by dir name OR full path)
            int cur_sel = -1;  // -1 = "(none)" entry
            for (int i = 0; i < s->model_scan_count; ++i) {
                if (strcmp(s->per_core_model_dir[core_id],
                            s->model_scan_paths[i]) == 0 ||
                    strcmp(s->per_core_model_dir[core_id],
                            s->model_scan_dirs[i]) == 0) {
                    cur_sel = i;
                    break;
                }
            }
            const char *preview = (cur_sel >= 0)
                ? s->model_scan_dirs[cur_sel]
                : (s->per_core_model_dir[core_id][0]
                    ? s->per_core_model_dir[core_id] : "(none)");
            if (ImGui::BeginCombo("Model Dir", preview)) {
                // (none) entry to clear the field
                bool sel_none = (s->per_core_model_dir[core_id][0] == '\0');
                if (ImGui::Selectable("(none)", sel_none)) {
                    s->per_core_model_dir[core_id][0] = '\0';
                    char key[64];
                    snprintf(key, sizeof(key), "core_%d_model_dir", core_id);
                    cfg_write_field(s->cfg_path, key, "");
                    changed = true;
                }
                for (int i = 0; i < s->model_scan_count; ++i) {
                    bool is_selected = (i == cur_sel);
                    if (ImGui::Selectable(s->model_scan_dirs[i], is_selected)) {
                        size_t n = strnlen(s->model_scan_paths[i],
                                            sizeof(s->per_core_model_dir[core_id]) - 1);
                        memcpy(s->per_core_model_dir[core_id],
                                s->model_scan_paths[i], n);
                        s->per_core_model_dir[core_id][n] = '\0';
                        char key[64];
                        snprintf(key, sizeof(key), "core_%d_model_dir", core_id);
                        cfg_write_field(s->cfg_path, key,
                                        s->per_core_model_dir[core_id]);
                        changed = true;
                    }
                    if (is_selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        } else {
            // No models found — fall back to InputText so operator can
            // type a path manually if needed.
            ImGui::InputText("Model Dir", s->per_core_model_dir[core_id], 256);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                char key[64];
                snprintf(key, sizeof(key), "core_%d_model_dir", core_id);
                cfg_write_field(s->cfg_path, key, s->per_core_model_dir[core_id]);
                changed = true;
            }
        }
        ImGui::PopID();
        ImGui::SetItemTooltip("Directory containing barrier/buy_signal/regime/exit roles. "
                              "Takes precedence over Model Path when set.\n\n"
                              "v5.9.5f: dropdown populated from `models/` scan.\n"
                              "Click '↻ Rescan models/' below if you've added models\n"
                              "since the panel opened.");
        ImGui::SameLine();
        ImGui::PushID("mdir_refresh");
        if (ImGui::SmallButton("↻")) {
            Settings_RescanModels(s);
        }
        ImGui::PopID();
        ImGui::SetItemTooltip("Rescan models/ directory");
    }

    // v4.7.23: resolve this core's strategy for the strategy-aware filter.
    // Prefer the LIVE active strategy from the snapshot; fall back to the
    // user's pending-but-not-applied dropdown choice; last resort STRATEGY_NONE
    // (hides strategy-specific fields entirely until something is configured).
    int core_strategy = STRATEGY_NONE;
    if (snap && snap->sharded_mode_active && core_id < snap->per_core_count) {
        core_strategy = snap->per_core[core_id].strategy_id_display;
    } else if (s->per_core_strategy[core_id] >= 0) {
        core_strategy = s->per_core_strategy[core_id];
    }

    const char *current_section = NULL;
    bool current_section_open = false;
    for (int j = 0; j < NUM_PER_CORE_FIELDS; ++j) {
        const PerCoreFieldDef *pcf = &per_core_fields[j];
        if (!current_section || strcmp(current_section, pcf->section) != 0) {
            current_section = pcf->section;
            // v4.7.23: pre-scan section for any visible field. If none match
            // the core's strategy, skip the whole section silently.
            bool any_visible = false;
            for (int k = j; k < NUM_PER_CORE_FIELDS &&
                            strcmp(per_core_fields[k].section, current_section) == 0; ++k) {
                if (per_core_field_visible(per_core_fields[k].key_suffix, core_strategy)) {
                    any_visible = true;
                    break;
                }
            }
            if (!any_visible) {
                while (j + 1 < NUM_PER_CORE_FIELDS &&
                       strcmp(per_core_fields[j + 1].section, current_section) == 0)
                    ++j;
                continue;
            }
            ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_DefaultOpen;
            current_section_open = ImGui::CollapsingHeader(current_section, fl);
            if (!current_section_open) {
                while (j + 1 < NUM_PER_CORE_FIELDS &&
                       strcmp(per_core_fields[j + 1].section, current_section) == 0)
                    ++j;
                continue;
            }
        }
        if (!current_section_open) continue;
        // v4.7.23: skip individual fields that don't match this core's strategy.
        if (!per_core_field_visible(pcf->key_suffix, core_strategy)) continue;
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
                                       int live_core_count = 0,
                                       TUISharedState *shared = NULL,
                                       const TUISnapshot *snap = NULL) {
    ImGui::Begin("Settings");

    if (!s->loaded) Settings_Load(s);

    // v4.7.22: when the engine is running with N cores live, sync the
    // num_execution_cores widget to that value so it doesn't read stale-
    // 0 or a stale cfg value while the live count is authoritative.
    if (live_core_count > 0 && live_core_count <= MAX_GUI_CORES) {
        for (int i = 0; i < NUM_FIELDS; ++i) {
            if (strcmp(field_defs[i].key, "num_execution_cores") == 0) {
                s->float_vals[i] = (float)live_core_count;
                break;
            }
        }
    }

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
            // Defensive ID scope per tab — section labels in Global may
            // collide with per_core_fields section labels (both have
            // "Trading", "Entry Filters") even though only one tab renders
            // at a time. Cheap insurance.
            ImGui::PushID("global_tab");
            if (Settings_RenderGlobalTab(s)) changed = true;
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        for (int c = 0; c < num_cores; ++c) {
            char tab_label[16];
            // v4.7.41 (Phase G): "Engine N" reframes each tab as a strategy
            // engine (slow + hot pair) rather than just an exec core.
            snprintf(tab_label, sizeof(tab_label), "Engine %d", c);
            if (ImGui::BeginTabItem(tab_label)) {
                ImGui::PushID(c + 1000);  // distinct from any field index
                if (Settings_RenderPerCoreTab(s, c, shared, snap)) changed = true;
                ImGui::PopID();
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
