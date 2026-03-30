#pragma once
// SettingsPanel — data-driven config editor for engine.cfg
//
// ADDING A NEW SETTING:
//   1. add ONE entry to the field_defs[] array below
//   2. done — loading, rendering, and saving are all automatic
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
enum CfgFieldType { CFG_FLOAT, CFG_INT, CFG_BOOL };

struct CfgFieldDef {
    const char *key;       // engine.cfg key name (e.g. "take_profit_pct")
    const char *label;     // GUI label (e.g. "TP %%")
    const char *section;   // collapsing header name (e.g. "Trading")
    CfgFieldType type;
    const char *fmt;       // printf format for floats (e.g. "%.2f")
};

// ── THE SINGLE SOURCE OF TRUTH ──
// adding a field: add ONE line here. loading + rendering + saving are automatic.
static const CfgFieldDef field_defs[] = {
    // Trading
    {"take_profit_pct",       "TP %%",        "Trading",         CFG_FLOAT, "%.2f"},
    {"stop_loss_pct",         "SL %%",        "Trading",         CFG_FLOAT, "%.2f"},
    {"fee_rate",              "Fee %%",       "Trading",         CFG_FLOAT, "%.2f"},
    {"slippage_pct",          "Slippage %%",  "Trading",         CFG_FLOAT, "%.2f"},
    {"risk_pct",              "Risk/Pos %%",  "Trading",         CFG_FLOAT, "%.1f"},
    {"fee_floor_mult",        "Fee Floor",    "Trading",         CFG_FLOAT, "%.1f"},
    // Entry Filters
    {"entry_offset_pct",      "Offset %%",    "Entry Filters",   CFG_FLOAT, "%.3f"},
    {"volume_multiplier",     "Vol Mult",     "Entry Filters",   CFG_FLOAT, "%.2f"},
    {"spacing_multiplier",    "Spacing",      "Entry Filters",   CFG_FLOAT, "%.2f"},
    {"offset_stddev_mult",    "Stddev Mult",  "Entry Filters",   CFG_FLOAT, "%.2f"},
    {"offset_stddev_min",     "Stddev Min",   "Entry Filters",   CFG_FLOAT, "%.2f"},
    {"offset_stddev_max",     "Stddev Max",   "Entry Filters",   CFG_FLOAT, "%.2f"},
    {"min_stddev_pct",        "Min Stddev %%","Entry Filters",   CFG_FLOAT, "%.5f"},
    {"min_long_slope",        "Min Long Slope","Entry Filters",  CFG_FLOAT, "%.6f"},
    {"min_buy_delta",         "Min Buy Delta","Entry Filters",   CFG_FLOAT, "%.2f"},
    {"vwap_offset",           "VWAP Offset",  "Entry Filters",   CFG_FLOAT, "%.4f"},
    // Adaptation
    {"filter_scale",          "Filter Scale", "Adaptation",      CFG_FLOAT, "%.2f"},
    {"offset_min",            "Offset Min %%","Adaptation",      CFG_FLOAT, "%.2f"},
    {"offset_max",            "Offset Max %%","Adaptation",      CFG_FLOAT, "%.2f"},
    {"vol_mult_min",          "Vol Min",      "Adaptation",      CFG_FLOAT, "%.2f"},
    {"vol_mult_max",          "Vol Max",      "Adaptation",      CFG_FLOAT, "%.2f"},
    {"r2_threshold",          "R² Threshold", "Adaptation",      CFG_FLOAT, "%.2f"},
    {"slope_scale_buy",       "Slope Scale",  "Adaptation",      CFG_FLOAT, "%.2f"},
    {"max_shift",             "Max Shift",    "Adaptation",      CFG_FLOAT, "%.4f"},
    // Trailing TP/SL
    {"tp_hold_score",         "Hold Score",   "Trailing TP/SL",  CFG_FLOAT, "%.2f"},
    {"tp_trail_mult",         "Trail TP",     "Trailing TP/SL",  CFG_FLOAT, "%.2f"},
    {"sl_trail_mult",         "Trail SL",     "Trailing TP/SL",  CFG_FLOAT, "%.2f"},
    // Time-Based Exit
    {"max_hold_ticks",        "Max Hold",     "Time-Based Exit", CFG_INT,   "%d"},
    {"min_hold_gain_pct",     "Min Gain %%",  "Time-Based Exit", CFG_FLOAT, "%.2f"},
    // Risk Management
    {"max_drawdown_pct",      "Max DD %%",    "Risk Management", CFG_FLOAT, "%.1f"},
    {"max_exposure_pct",      "Max Exp %%",   "Risk Management", CFG_FLOAT, "%.0f"},
    {"max_positions",         "Max Pos",      "Risk Management", CFG_INT,   "%d"},
    // Kill Switch
    {"kill_switch_enabled",   "Enabled",      "Kill Switch",     CFG_BOOL,  NULL},
    {"kill_switch_daily_loss_pct","Daily Loss %%","Kill Switch",  CFG_FLOAT, "%.2f"},
    {"kill_switch_drawdown_pct","Drawdown %%", "Kill Switch",     CFG_FLOAT, "%.2f"},
    {"kill_recovery_warmup",  "Recovery",     "Kill Switch",      CFG_INT,   "%d"},
    // Vol Sizing
    {"vol_sizing_enabled",    "Enabled",      "Vol Sizing",      CFG_BOOL,  NULL},
    {"vol_scale_min",         "Scale Min",    "Vol Sizing",      CFG_FLOAT, "%.2f"},
    {"vol_scale_max",         "Scale Max",    "Vol Sizing",      CFG_FLOAT, "%.2f"},
    // No-Trade Band
    {"no_trade_band_enabled", "Enabled",      "No-Trade Band",   CFG_BOOL,  NULL},
    {"no_trade_band_mult",    "Fee Mult",     "No-Trade Band",   CFG_FLOAT, "%.2f"},
    // Regime Detection
    {"regime_crossover_threshold","EMA/SMA Gap","Regime Detection",CFG_FLOAT,"%.5f"},
    {"regime_r2_threshold",   "R² Threshold", "Regime Detection", CFG_FLOAT, "%.1f"},
    {"regime_vol_spike_ratio","Vol Spike",    "Regime Detection", CFG_FLOAT, "%.1f"},
    {"regime_hysteresis",     "Hysteresis",   "Regime Detection", CFG_INT,   "%d"},
    // Momentum
    {"momentum_breakout_mult","Breakout",     "Momentum",        CFG_FLOAT, "%.2f"},
    {"momentum_tp_mult",      "Mom TP",       "Momentum",        CFG_FLOAT, "%.2f"},
    {"momentum_sl_mult",      "Mom SL",       "Momentum",        CFG_FLOAT, "%.2f"},
    {"momentum_r2_min",       "R² Min",       "Momentum",        CFG_FLOAT, "%.2f"},
    // Partial Exits
    {"partial_exit_pct",      "TP1 Split %%", "Partial Exits",   CFG_FLOAT, "%.2f"},
    {"tp2_mult",              "TP2 Mult",     "Partial Exits",   CFG_FLOAT, "%.2f"},
    {"breakeven_on_partial",  "Breakeven SL", "Partial Exits",   CFG_BOOL,  NULL},
    // Gate Recovery
    {"idle_reset_cycles",     "Idle Reset",   "Gate Recovery",   CFG_INT,   "%d"},
    {"sl_cooldown_cycles",    "SL Cooldown",  "Gate Recovery",   CFG_INT,   "%d"},
    {"sl_cooldown_adaptive",  "Adaptive CD",  "Gate Recovery",   CFG_BOOL,  NULL},
    // Session Filters
    {"session_asian_mult",    "Asian",        "Session Filters",  CFG_FLOAT, "%.2f"},
    {"session_european_mult", "European",     "Session Filters",  CFG_FLOAT, "%.2f"},
    {"session_us_mult",       "US",           "Session Filters",  CFG_FLOAT, "%.2f"},
    {"session_overnight_mult","Overnight",    "Session Filters",  CFG_FLOAT, "%.2f"},
    // Strategy
    {"default_strategy",      "Default##strat","Strategy",       CFG_INT,   "%d"},
    // EMA Gate
    {"gate_ema_enabled",      "EMA Enabled",  "EMA Gate",        CFG_BOOL,  NULL},
    {"gate_ema_alpha",        "Alpha",        "EMA Gate",        CFG_FLOAT, "%.4f"},
    // Toggles
    {"use_real_money",        "LIVE Trading", "Toggles",         CFG_BOOL,  NULL},
    {"partial_exit_enabled",  "Partial Exits","Toggles",         CFG_BOOL,  NULL},
    {"session_filter_enabled","Session Filter","Toggles",        CFG_BOOL,  NULL},
    {"depth_enabled",         "Order Book",   "Toggles",         CFG_BOOL,  NULL},
    {"min_book_imbalance",    "Book Imbal",   "Toggles",         CFG_FLOAT, "%.2f"},
};
static constexpr int NUM_FIELDS = sizeof(field_defs) / sizeof(field_defs[0]);

//==========================================================================
// SETTINGS STATE — auto-generated from field_defs (no manual struct)
//==========================================================================
struct SettingsState {
    float float_vals[NUM_FIELDS];  // storage for float/int fields
    int   bool_vals[NUM_FIELDS];   // storage for bool fields
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
                if (field_defs[i].type == CFG_BOOL)
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
        }

        // hover tooltips — SetItemTooltip handles hover detection + delay internally
        {
            const char *k = fd->key;
            if      (strcmp(k, "default_strategy") == 0)
                ImGui::SetItemTooltip("-1 = Regime Auto (MR + Momentum)\n 0 = Mean Reversion\n 1 = Momentum\n 2 = Simple Dip");
            else if (strcmp(k, "entry_offset_pct") == 0)
                ImGui::SetItemTooltip("Buy gate offset below avg/EMA price\nhigher = deeper dip required to enter");
            else if (strcmp(k, "volume_multiplier") == 0)
                ImGui::SetItemTooltip("Volume gate: require avg_volume * this\nhigher = only buy on high volume");
            else if (strcmp(k, "spacing_multiplier") == 0)
                ImGui::SetItemTooltip("Min distance between entries (in stddev)\nprevents clustering entries at similar prices");
            else if (strcmp(k, "offset_stddev_mult") == 0)
                ImGui::SetItemTooltip("Multiplies stddev for offset calculation\nhigher = wider offset from avg (fewer entries)");
            else if (strcmp(k, "tp_hold_score") == 0)
                ImGui::SetItemTooltip("SNR * R-squared threshold to activate trailing\nhigher = only trail strong consistent trends");
            else if (strcmp(k, "tp_trail_mult") == 0)
                ImGui::SetItemTooltip("Trailing TP distance as fraction of offset\nTP ratchets up as price rises");
            else if (strcmp(k, "sl_trail_mult") == 0)
                ImGui::SetItemTooltip("Trailing SL distance as fraction of offset\nSL ratchets up to lock in gains");
            else if (strcmp(k, "max_drawdown_pct") == 0)
                ImGui::SetItemTooltip("Circuit breaker: halt trading if total P&L\ndrops below this %% of starting balance");
            else if (strcmp(k, "momentum_breakout_mult") == 0)
                ImGui::SetItemTooltip("Buy above avg by this many stddev\nhigher = require stronger breakout");
            else if (strcmp(k, "momentum_tp_mult") == 0)
                ImGui::SetItemTooltip("Momentum TP distance in stddev units\nscaled by R-squared at fill time");
            else if (strcmp(k, "momentum_sl_mult") == 0)
                ImGui::SetItemTooltip("Momentum SL distance in stddev units\nscaled by R-squared at fill time");
            else if (strcmp(k, "gate_ema_alpha") == 0)
                ImGui::SetItemTooltip("EMA smoothing factor\n0.99 = fast (responsive)\n0.997 = default\n0.999 = slow (stable)");
            // regime detection
            else if (strcmp(k, "regime_crossover_threshold") == 0)
                ImGui::SetItemTooltip("EMA/SMA spread to classify TRENDING\n0.0005 = 0.05%% gap (~$35 at BTC $70k)");
            else if (strcmp(k, "regime_r2_threshold") == 0)
                ImGui::SetItemTooltip("Min R-squared consistency for TRENDING\n70 = 70%% of price variance explained by trend");
            else if (strcmp(k, "regime_vol_spike_ratio") == 0)
                ImGui::SetItemTooltip("Short/long variance ratio for VOLATILE\n2.0 = short-window variance is 2x long-window");
            else if (strcmp(k, "regime_hysteresis") == 0)
                ImGui::SetItemTooltip("Slow-path cycles before regime switch\nprevents rapid flipping between strategies");
            // risk infrastructure
            else if (strcmp(k, "kill_switch_daily_loss_pct") == 0)
                ImGui::SetItemTooltip("Max session loss before kill switch triggers\n3.0 = halt if equity drops 3%% from session start");
            else if (strcmp(k, "kill_switch_drawdown_pct") == 0)
                ImGui::SetItemTooltip("Max drawdown from session peak before kill\n5.0 = halt if 5%% below intra-session high");
            else if (strcmp(k, "kill_recovery_warmup") == 0)
                ImGui::SetItemTooltip("Slow-path cycles to observe after kill reset\nbefore trading resumes (prevents immediate re-entry)");
            else if (strcmp(k, "vol_scale_min") == 0)
                ImGui::SetItemTooltip("Minimum position scale factor\n0.25 = never less than 25%% of base qty");
            else if (strcmp(k, "vol_scale_max") == 0)
                ImGui::SetItemTooltip("Maximum position scale factor\n2.0 = never more than 200%% of base qty");
            else if (strcmp(k, "no_trade_band_mult") == 0)
                ImGui::SetItemTooltip("Signal must exceed fee_rate * this to trade\n3.0 = dip must be 3x round-trip fee cost");
            // entry filters
            else if (strcmp(k, "min_stddev_pct") == 0)
                ImGui::SetItemTooltip("Skip trades when stddev/price below this\nprevents entries in dead-flat markets");
            else if (strcmp(k, "min_long_slope") == 0)
                ImGui::SetItemTooltip("Block MR buys when 512-tick slope below this\nnegative = allow mild dips, 0 = disabled");
            else if (strcmp(k, "min_buy_delta") == 0)
                ImGui::SetItemTooltip("Min volume delta for MR buys\n-0.3 = allow mild selling, block heavy dumps");
            else if (strcmp(k, "momentum_r2_min") == 0)
                ImGui::SetItemTooltip("Min R-squared to enter momentum trades\n0.4 = require 40%% trend consistency");
            // time-based exit
            else if (strcmp(k, "max_hold_ticks") == 0)
                ImGui::SetItemTooltip("Close position after this many ticks\n0 = disabled, 75000 ≈ 4-5 hours");
            else if (strcmp(k, "min_hold_gain_pct") == 0)
                ImGui::SetItemTooltip("Only time-exit if gain below this %%\nprotects profitable positions from time exit");
            // gate recovery
            else if (strcmp(k, "idle_reset_cycles") == 0)
                ImGui::SetItemTooltip("Cycles with no fill before gate decay\nprevents permanent lockout after losses");
            else if (strcmp(k, "sl_cooldown_cycles") == 0)
                ImGui::SetItemTooltip("Slow-path cycles to pause after stop loss\nlets market settle before re-entry");
            // partial exits
            else if (strcmp(k, "partial_exit_pct") == 0)
                ImGui::SetItemTooltip("Fraction to exit at TP1\n0.5 = 50%% exits early, 50%% rides TP2");
            else if (strcmp(k, "tp2_mult") == 0)
                ImGui::SetItemTooltip("TP2 distance = TP1 distance * this\n2.0 = second leg targets double the gain");
            // session filters
            else if (strcmp(k, "session_asian_mult") == 0)
                ImGui::SetItemTooltip("Volume gate multiplier 00-07 UTC\nhigher = more selective (fewer entries)");
            else if (strcmp(k, "session_us_mult") == 0)
                ImGui::SetItemTooltip("Volume gate multiplier 13-20 UTC\nlower = less selective (best liquidity)");
            // adaptation
            else if (strcmp(k, "filter_scale") == 0)
                ImGui::SetItemTooltip("How fast filters adapt to P&L regression\nhigher = more reactive to recent performance");
            else if (strcmp(k, "fee_floor_mult") == 0)
                ImGui::SetItemTooltip("TP floor = entry * fee_rate * this\n3.0 = TP must clear round-trip fees + margin");
        }
    }

    if (changed) {
        __atomic_store_n(reload_flag, 1, __ATOMIC_RELEASE);
        ImGui::TextColored(FoxmlColors::green_b, "saved + reloaded");
    }

    ImGui::End();
}
