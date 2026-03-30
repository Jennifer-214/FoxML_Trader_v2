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
    // Entry Filters
    {"entry_offset_pct",      "Offset %%",    "Entry Filters",   CFG_FLOAT, "%.3f"},
    {"volume_multiplier",     "Vol Mult",     "Entry Filters",   CFG_FLOAT, "%.2f"},
    {"spacing_multiplier",    "Spacing",      "Entry Filters",   CFG_FLOAT, "%.2f"},
    {"offset_stddev_mult",    "Stddev Mult",  "Entry Filters",   CFG_FLOAT, "%.2f"},
    // Trailing TP/SL
    {"tp_hold_score",         "Hold Score",   "Trailing TP/SL",  CFG_FLOAT, "%.2f"},
    {"tp_trail_mult",         "Trail TP",     "Trailing TP/SL",  CFG_FLOAT, "%.2f"},
    {"sl_trail_mult",         "Trail SL",     "Trailing TP/SL",  CFG_FLOAT, "%.2f"},
    // Risk Management
    {"max_drawdown_pct",      "Max DD %%",    "Risk Management", CFG_FLOAT, "%.1f"},
    {"max_exposure_pct",      "Max Exp %%",   "Risk Management", CFG_FLOAT, "%.0f"},
    {"max_positions",         "Max Pos",      "Risk Management", CFG_INT,   "%d"},
    // Momentum
    {"momentum_breakout_mult","Breakout",     "Momentum",        CFG_FLOAT, "%.2f"},
    {"momentum_tp_mult",      "Mom TP",       "Momentum",        CFG_FLOAT, "%.2f"},
    {"momentum_sl_mult",      "Mom SL",       "Momentum",        CFG_FLOAT, "%.2f"},
    // Strategy
    {"default_strategy",      "Default##strat","Strategy",       CFG_INT,   "%d"},
    // EMA Gate
    {"gate_ema_enabled",      "EMA Enabled",  "EMA Gate",        CFG_BOOL,  NULL},
    {"gate_ema_alpha",        "Alpha",        "EMA Gate",        CFG_FLOAT, "%.4f"},
    // Toggles
    {"use_real_money",        "LIVE Trading", "Toggles",         CFG_BOOL,  NULL},
    {"partial_exit_enabled",  "Partial Exits","Toggles",         CFG_BOOL,  NULL},
    {"session_filter_enabled","Session Filter","Toggles",        CFG_BOOL,  NULL},
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
        }
    }

    if (changed) {
        __atomic_store_n(reload_flag, 1, __ATOMIC_RELEASE);
        ImGui::TextColored(FoxmlColors::green_b, "saved + reloaded");
    }

    ImGui::End();
}
