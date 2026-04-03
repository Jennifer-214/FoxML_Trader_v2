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
    {"fee_rate",              "Fee %%",       "Trading",         CFG_FLOAT, "%.2f", NULL},
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
    {"record_max_days",       "Max Days",     "Tick Recording",  CFG_FLOAT, "%.0f",
        "Auto-delete tick CSVs older than this many days\n30 = ~1-2GB cap on disk usage"},
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
};
static constexpr int NUM_FIELDS = sizeof(field_defs) / sizeof(field_defs[0]);

//==========================================================================
// SETTINGS STATE — auto-generated from field_defs (no manual struct)
//==========================================================================
struct SettingsState {
    float float_vals[NUM_FIELDS];  // storage for float/int fields
    int   bool_vals[NUM_FIELDS];   // storage for bool fields
    char  path_vals[NUM_FIELDS][256]; // storage for path fields
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
    FILE *f = fopen(s->cfg_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        for (int i = 0; i < NUM_FIELDS; i++) {
            size_t klen = strlen(field_defs[i].key);
            if (strncmp(p, field_defs[i].key, klen) == 0 && p[klen] == '=') {
                const char *val = p + klen + 1;
                if (field_defs[i].type == CFG_PATH) {
                    // strip trailing whitespace/newline
                    strncpy(s->path_vals[i], val, 255);
                    s->path_vals[i][255] = '\0';
                    char *end = s->path_vals[i] + strlen(s->path_vals[i]) - 1;
                    while (end > s->path_vals[i] && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
                } else if (field_defs[i].type == CFG_BOOL)
                    s->bool_vals[i] = atoi(val);
                else if (field_defs[i].type == CFG_INT)
                    s->float_vals[i] = (float)atoi(val);
                else
                    s->float_vals[i] = (float)atof(val);
                break;
            }
        }
    }
    fclose(f);
    s->loaded = true;
}

//==========================================================================
// RENDER — auto-generates UI from field_defs
//==========================================================================
static inline void GUI_Panel_Settings(SettingsState *s, volatile sig_atomic_t *reload_flag) {
    ImGui::Begin("Settings");

    if (!s->loaded) Settings_Load(s);

    ImGui::TextColored(FoxmlColors::primary, "ENGINE SETTINGS");
    ImGui::TextColored(FoxmlColors::comment, "edit + press Enter to apply");
    ImGui::Separator();

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

    if (changed) {
        __atomic_store_n(reload_flag, 1, __ATOMIC_RELEASE);
        ImGui::TextColored(FoxmlColors::green_b, "saved + reloaded");
    }

    ImGui::End();
}
