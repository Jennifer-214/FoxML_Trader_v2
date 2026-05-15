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
// v5.14.9.F.5 — registry headers for auto-extended field_defs[] entries
#include "../CoreFrameworks/LifecycleCfgFlagRegistry.hpp"
#include "../CoreFrameworks/GateCfgFlagRegistry.hpp"
#include "../ML_Headers/MlCfgFlagRegistry.hpp"
#include "../CoreFrameworks/RiskCfgFlagRegistry.hpp"
#include "../CoreFrameworks/OpsCfgFlagRegistry.hpp"
// v5.15.5.F.4b — universal cfg field registry (KIND_DOUBLE/_PCT cohort auto-extend)
#include "../CoreFrameworks/CfgFieldRegistry.hpp"
// v5.15.5.F.4c — per-field render function pointer table needs ControllerConfig<F> in scope
#include "../CoreFrameworks/ControllerConfig.hpp"
// v5.15.5.F.4c — tt::cfg_parse/save/assign/diff_field for use in render table
#include "../CoreFrameworks/CfgFieldDispatch.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <dirent.h>     // v5.9.5f — opendir for model directory scan
#include <sys/stat.h>   // v5.9.5f — stat for role-file detection
// v5.15.5.F.4c — tt::cfg_render_field<T> dispatch trio completion
#include "../FixedPoint/FixedPointN.hpp"   // is_FPN_v, FPN_FromDouble, FPN_ToDouble
#include <type_traits>                     // is_floating_point_v, is_integral_v, is_array_v, is_unsigned_v

//==========================================================================
// [tt::cfg_render_field<T> — third sister of cfg_parse_field / cfg_save_field]
//==========================================================================
// v5.15.5.F.4c — completes the tt:: dispatch trio. Lives here (not in
// CoreFrameworks/CfgFieldDispatch.hpp) to keep ImGui dependency out of the
// parser path per CfgFieldDispatch.hpp:143-145 design note.
//
// 3-barrier discipline per DESIGN_SPECS/type-trait-dispatch-via-tt-namespace.md:
//   B1 — destination-by-reference; NO void*+offset overload
//   B2 — X-macro extractor passes cfg.name by reference; never &((char*)cfg)[offset]
//   B3 — compile-time type-family static_assert
//
// Per H13 (CLAUDE.md item 23) + H14: storage width comes from T via type-trait
// dispatch. desc.kind is read ONLY for GUI presentation discrimination
// (INT_ENUM dropdown vs BOOL checkbox vs INT slider) inside the integral
// branch — NEVER to determine storage width.
//==========================================================================
namespace tt {

    // Returns true if the field value changed this frame.
    template <typename T>
    inline bool cfg_render_field(T& field, const CfgFieldDescriptor& desc) {
        static_assert(is_FPN_v<T>
                   || std::is_floating_point_v<T>
                   || std::is_integral_v<T>
                   || std::is_array_v<T>,
                      "cfg field type not in recognized family — "
                      "extend tt::cfg_render_field<T> with a new branch. See "
                      "DESIGN_SPECS/type-trait-dispatch-via-tt-namespace.md.");

        // v5.15.5.F.4c.1 — ImGui widget-ID independence from display label.
        // Without PushID(cfg_field_name), two descriptors that happen to share
        // desc.label collide at ImGui's hash table → "name is already taken"
        // runtime error + one of the controls becomes non-functional. With
        // PushID(cfg_field_name), uniqueness comes from the X-macro-guaranteed
        // unique cfg_field_name; display label can repeat safely across
        // sections/per-core tabs. Closes the Class 24-shape regression caught
        // at paper-test 2026-05-14. PushID composes with outer PushID(core_id)
        // at per-core tab sites — full uniqueness tuple becomes
        // (core_id, cfg_field_name, label).
        ImGui::PushID(desc.cfg_field_name);

        bool changed = false;

        if constexpr (is_FPN_v<T>) {
            double v = FPN_ToDouble(field);
            if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) v *= 100.0;
            float vf = static_cast<float>(v);
            float lo = static_cast<float>(desc.payload.as_double.clamp_min);
            float hi = static_cast<float>(desc.payload.as_double.clamp_max);
            const char* fmt = (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) ? "%.2f%%" : "%.4f";
            changed = ImGui::SliderFloat(desc.label, &vf, lo, hi, fmt);
            if (changed) {
                if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) vf /= 100.0f;
                field = FPN_FromDouble<T::F>(static_cast<double>(vf));
            }
        } else if constexpr (std::is_floating_point_v<T>) {
            double v = static_cast<double>(field);
            if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) v *= 100.0;
            float vf = static_cast<float>(v);
            float lo = static_cast<float>(desc.payload.as_double.clamp_min);
            float hi = static_cast<float>(desc.payload.as_double.clamp_max);
            const char* fmt = (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) ? "%.2f%%" : "%.4f";
            changed = ImGui::SliderFloat(desc.label, &vf, lo, hi, fmt);
            if (changed) {
                if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) vf /= 100.0f;
                field = static_cast<T>(static_cast<double>(vf));
            }
        } else if constexpr (std::is_array_v<T>) {
            // char[N] — InputText. KIND_STRING / KIND_FILE_PATH migrate at .F.4e.
            changed = ImGui::InputText(desc.label, field, std::extent_v<T>);
        } else if constexpr (std::is_integral_v<T>) {
            // Sub-dispatch on desc.kind for GUI presentation. Storage width comes
            // from T at compile time per H13 (Kind NEVER drives storage).
            if (desc.kind == CfgFieldDescriptor::KIND_INT_ENUM) {
                int v = static_cast<int>(field);
                changed = ImGui::Combo(desc.label, &v,
                                       desc.payload.as_int_enum.labels,
                                       desc.payload.as_int_enum.count);
                if (changed) field = static_cast<T>(v);
            } else if (desc.kind == CfgFieldDescriptor::KIND_BOOL) {
                bool b = (field != 0);
                changed = ImGui::Checkbox(desc.label, &b);
                if (changed) field = static_cast<T>(b ? 1 : 0);
            } else {
                // KIND_INT (any width). Per-row static_assert at registry expansion
                // site (H14 / CLAUDE.md item 20) verifies clamp ∈ numeric_limits<T>.
                // For a future uint64 field needing clamp_max > INT_MAX, extend with
                // an InputScalar(ImGuiDataType_U64) branch (none in .F.4c scope).
                int v = static_cast<int>(field);
                int lo = static_cast<int>(desc.payload.as_int.clamp_min);
                int hi = static_cast<int>(desc.payload.as_int.clamp_max);
                changed = ImGui::SliderInt(desc.label, &v, lo, hi);
                if (changed) field = static_cast<T>(v);
            }
        }

        if (desc.tooltip && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", desc.tooltip);
        }
        // v5.15.5.F.4c.1 — Reset/Modified UI deferred to follow-up.
        // tt::cfg_assign_field<T> + tt::cfg_diff_field<T> primitives shipped
        // at .F.4c are READY for consumer wiring. Inline Modified-badge at
        // this site requires resolving an FPN<F> operator== ambiguity vs
        // ImGui's ImTextureRef in engine_gui's main.cpp compile unit (see
        // build error from .F.4c.1 attempt). Resolution + per-section Reset
        // button placement need operator UX input post paper-test of the
        // 18-row cohort + ImGui PushID fix.
        ImGui::PopID();
        return changed;
    }

}  // namespace tt

//==========================================================================
// [PER-FIELD RENDER FUNCTION POINTER TABLE — v5.15.5.F.4c]
//==========================================================================
// Implements the type-erased dispatch layer of the bitmap dispatcher framework
// (see CoreFrameworks/CfgFieldRegistry.hpp + DESIGN_SPECS/universal-registry-
// bitmap-dispatcher-pattern.md). X-macro generates one static render_<name>()
// function per cfg field; each calls tt::cfg_render_field<T> with T deduced
// from the actual ControllerConfig<F>::<name> field type (Barrier 2 per Class
// 23 prevention discipline).
//
// Templated on F so per-precision instantiations share the table; F=64 is the
// canonical engine precision. Static-storage-duration constexpr array → .rodata.
//
// Consumer (the bitmap walker in SettingsTab below):
//   CFG_FIELD_FOR_EACH_SET_BIT(g_global_cfg_render_mask.words, idx, {
//       GlobalCfgRenderTable<64>::fns[idx](cfg, g_global_cfg_field_descriptors[idx], cfg_path);
//   });
//   CFG_FIELD_FOR_EACH_SET_BIT(g_per_core_cfg_render_mask.words, idx, {
//       PerCoreCfgRenderTable<64>::fns[idx](cfg, g_per_core_cfg_field_descriptors[idx], cfg_path);
//   });
//
// .F.4c.3 — split into GlobalCfgRenderTable + PerCoreCfgRenderTable per the
// two-registry architecture. Each table's `fns[]` sized to its registry's
// FIELD_IDX_*_END sentinel. Today both render against `gui_engine_cfg` (the
// flat ControllerConfig<F> instance — fields haven't moved yet). Step 2 will
// restructure to PerCoreCfgRenderTable receiving cfg.cores[c] reference.
//==========================================================================
template <unsigned F>
struct GlobalCfgRenderTable {
    // v5.15.5.F.4c — render-and-persist fn signature: each call dispatches via
    // cfg_render_and_persist<T> wrapper (render + save + per-edit file write).
    using RenderFn = bool (*)(ControllerConfig<F>&, const CfgFieldDescriptor&, const char*);

    #define X_GEN_GLOBAL_RENDER_FN(KIND_TOKEN, name, label, section, meta, payload, tooltip, \
                                     applies_to_strategy, applies_to_op_mode, \
                                     applies_to_regime, applies_to_risk, lives_in_struct) \
        static bool render_##name(ControllerConfig<F>& cfg, const CfgFieldDescriptor& desc, const char* cfg_path) { \
            return cfg_render_and_persist(cfg.name, desc, cfg_path); \
        }
    FOREACH_GLOBAL_CFG_FIELD(X_GEN_GLOBAL_RENDER_FN)
    #undef X_GEN_GLOBAL_RENDER_FN

    #define X_GEN_GLOBAL_RENDER_PTR(KIND_TOKEN, name, label, section, meta, payload, tooltip, ...) \
        &GlobalCfgRenderTable<F>::render_##name,
    static constexpr RenderFn fns[FIELD_IDX_GLOBAL_END] = {
        FOREACH_GLOBAL_CFG_FIELD(X_GEN_GLOBAL_RENDER_PTR)
    };
    #undef X_GEN_GLOBAL_RENDER_PTR
};

template <unsigned F>
struct PerCoreCfgRenderTable {
    // Per-core render table. .F.4c.3 — still consumes ControllerConfig<F>&
    // (fields haven't moved yet); Step 2 restructures to PerCoreCfg<F>& cores[c].
    using RenderFn = bool (*)(ControllerConfig<F>&, const CfgFieldDescriptor&, const char*);

    #define X_GEN_PER_CORE_RENDER_FN(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, \
                                       applies_to_strategy, applies_to_op_mode, \
                                       applies_to_regime, applies_to_risk, lives_in_struct) \
        static bool render_##name(ControllerConfig<F>& cfg, const CfgFieldDescriptor& desc, const char* cfg_path) { \
            return cfg_render_and_persist(cfg.name, desc, cfg_path); \
        }
    FOREACH_PER_CORE_CFG_FIELD(X_GEN_PER_CORE_RENDER_FN)
    #undef X_GEN_PER_CORE_RENDER_FN

    #define X_GEN_PER_CORE_RENDER_PTR(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, ...) \
        &PerCoreCfgRenderTable<F>::render_##name,
    static constexpr RenderFn fns[FIELD_IDX_PER_CORE_END] = {
        FOREACH_PER_CORE_CFG_FIELD(X_GEN_PER_CORE_RENDER_PTR)
    };
    #undef X_GEN_PER_CORE_RENDER_PTR
};

// Forward decl — cfg_write_field is defined later in this file (line ~640).
// Template instantiation of cfg_render_and_persist<T> below happens at call sites
// (the bitmap walker), at which point cfg_write_field is already in scope.
static inline void cfg_write_field(const char *path, const char *key, const char *value);

//==========================================================================
// [cfg_render_and_persist<T> — per-edit persistence helper — v5.15.5.F.4c]
//==========================================================================
// Wraps the canonical "render-changed → file-written" flow:
//   1. tt::cfg_render_field<T> dispatches to ImGui widget; returns true if value changed
//   2. tt::cfg_save_field<T> formats current value to text buffer (locale-pinned)
//   3. cfg_write_field writes the single key=value pair to the cfg file
//
// Preserves the current per-edit persistence behavior (parallel-array system
// writes immediately on change). Used by the bitmap-dispatch walker as the
// per-row action: change-detection + format + persist in one call.
//
// Returns true if the value changed this frame (caller may track for global
// dirty state or visual "modified" indicators).
//==========================================================================
template <typename T>
inline bool cfg_render_and_persist(T& field, const CfgFieldDescriptor& desc, const char* cfg_path) {
    const bool changed = tt::cfg_render_field(field, desc);
    if (changed) {
        char buf[512];  // matches path_vals[] capacity (handles longest STRING field comfortably)
        tt::cfg_save_field(field, desc, buf, sizeof(buf));
        cfg_write_field(cfg_path, desc.cfg_field_name, buf);
    }
    return changed;
}

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
    // v5.15.5.F.4b — take_profit_pct, stop_loss_pct, fee_rate migrated to
    // FOREACH_CFG_FIELD (auto-extended below). fee_rate_maker / fee_rate_taker
    // stay manual (parser has explicit_set side effect).
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
    // v5.15.5.F.4b — slippage_pct, risk_pct, fee_floor_mult migrated to
    // FOREACH_CFG_FIELD (auto-extended below).
    // Entry Filters
    // v5.15.5.F.4b — entry_offset_pct, volume_multiplier, spacing_multiplier,
    // min_stddev_pct, min_long_slope, min_buy_delta, vwap_offset migrated to
    // FOREACH_CFG_FIELD (auto-extended below). offset_stddev_* stay manual
    // (not in .F.4b registry cohort).
    {"offset_stddev_mult",    "Stddev Mult",  "Entry Filters",   CFG_FLOAT, "%.2f",
        "Multiplies stddev for offset calculation\nhigher = wider offset from avg (fewer entries)"},
    {"offset_stddev_min",     "Stddev Min",   "Entry Filters",   CFG_FLOAT, "%.2f", NULL},
    {"offset_stddev_max",     "Stddev Max",   "Entry Filters",   CFG_FLOAT, "%.2f", NULL},
    // v4.7.29: Adaptation, Trailing TP/SL, Time-Based Exit moved to per-core
    // tabs. These were exit/feedback policies that varied by strategy
    // (DIP wants short holds, EMA Cross wants long; Momentum wants tighter
    // R² gates, etc.). Set them per-core via each tab's override sections.
    // max_hold_ticks (uint32) stays global — INT support for X-macro is a
    // future extension; min_hold_gain_pct moved to per-core handles the
    // common case (different strategies want different hold-gain floors).
    // v5.15.5.F.4c — max_hold_ticks migrated to FOREACH_CFG_FIELD (KIND_INT;
    // HIGH-6 tooltip byte-identity preserved).
    // Risk Management
    // v5.15.5.F.4b — max_drawdown_pct, max_exposure_pct migrated to FOREACH_CFG_FIELD.
    // v5.15.5.F.4c — max_positions migrated to FOREACH_CFG_FIELD (KIND_INT; clamp [1,16]).
    // Kill Switch
    // kill_switch_enabled migrated to FOREACH_RISK_CFG_FLAG (v5.14.9.F.5; auto-extended below)
    // v5.15.5.F.4b — kill_switch_daily_loss_pct, kill_switch_drawdown_pct migrated to FOREACH_CFG_FIELD.
    // v5.15.5.F.4c — kill_recovery_warmup migrated to FOREACH_CFG_FIELD (KIND_INT; HIGH-6 tooltip preserved).
    // v4.7.29: Vol Sizing + No-Trade Band scale curves moved to per-core
    // tabs. Toggles stay global (engine-architectural enable/disable).
    // vol_sizing_enabled + no_trade_band_enabled migrated to FOREACH_<DOMAIN>_CFG_FLAG
    // (v5.14.9.F.5 RISK + GATE; auto-extended below)
    // Regime Detection
    // v5.15.5.F.4b — regime_crossover_threshold, regime_strong_crossover,
    // regime_r2_threshold migrated to FOREACH_CFG_FIELD (auto-extended below).
    {"regime_vol_spike_ratio","Vol Spike",    "Regime Detection", CFG_FLOAT, "%.1f",
        "Short/long variance ratio for VOLATILE\n2.0 = short-window variance is 2x long-window"},
    // v5.15.5.F.4c — regime_hysteresis migrated to FOREACH_CFG_FIELD (KIND_INT; HIGH-6 tooltip preserved).
    // (Momentum + EMA Cross strategy tuning consolidated into "Momentum
    //  Tuning" / "EMA Cross Tuning" sections below — v4.7.22 dedup pass.)
    // v4.7.29: Partial Exits geometry (split %, TP2 mult) moved to per-core.
    // breakeven_on_partial migrated to FOREACH_LIFECYCLE_CFG_FLAG (v5.14.9.F.5; auto-extended below)
    // Gate Recovery
    // v5.15.5.F.4c — idle_reset_cycles + sl_cooldown_cycles migrated to FOREACH_CFG_FIELD (KIND_INT; HIGH-6 tooltips preserved).
    // v5.15.5.F.4c — sl_cooldown_adaptive migrated to FOREACH_CFG_FIELD (KIND_BOOL).
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
    // gate_ema_enabled migrated to FOREACH_GATE_CFG_FLAG (v5.14.9.F.5; auto-extended below)
    {"gate_ema_alpha",        "Alpha",        "EMA Gate",        CFG_FLOAT, "%.4f",
        "EMA smoothing factor\n0.99 = fast (responsive)\n0.997 = default\n0.999 = slow (stable)"},
    // Danger Gradient
    // v5.15.5.F.4c — danger_enabled migrated to FOREACH_CFG_FIELD (KIND_BOOL).
    {"danger_warn_stddevs",   "Warn σ",       "Danger Gradient",  CFG_FLOAT, "%.1f",
        "Danger gradient starts at this many σ below avg\n3.0 = gate begins tightening at 3σ drop"},
    {"danger_crash_stddevs",  "Crash σ",      "Danger Gradient",  CFG_FLOAT, "%.1f",
        "Full gate kill at this many σ below avg\n6.0 = gate zeroed at 6σ drop (crash protection)"},
    // Tick Recording
    // v5.15.5.F.4c — record_ticks + record_depth migrated to FOREACH_CFG_FIELD (KIND_BOOL;
    // HIGH-6 tooltip byte-identity preserved via single-string-literal concatenation).
    {"record_max_days",       "Max Days",     "Tick Recording",  CFG_FLOAT, "%.0f",
        "Auto-delete tick + depth CSVs older than this many days\n30 = ~1-2GB cap on disk usage (more if depth recording is on)"},
    // Operational Monitoring (Phase 8b) — alerts on kill switch, orphans, disconnects
    // notify_enabled migrated to FOREACH_OPS_CFG_FLAG (v5.14.9.F.5; auto-extended below)
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
    // partial_exit_enabled / session_filter_enabled / depth_enabled migrated to
    // FOREACH_<DOMAIN>_CFG_FLAG (v5.14.9.F.5 LIFECYCLE + OPS + GATE; auto-extended below)
    {"min_book_imbalance",    "Book Imbal",   "Toggles",         CFG_FLOAT, "%.2f", NULL},
    // FoxML integration — engine-wide enable/disable + training-time defaults
    // (per-core FPN tuning lives in each ML core's "ML" override section).
    // cost_gate_enabled / foxml_vol_scaling_enabled / bandit_enabled / confidence_enabled
    // migrated to FOREACH_<DOMAIN>_CFG_FLAG (v5.14.9.F.5; auto-extended below):
    //   cost_gate_enabled → GATE; bandit_enabled / confidence_enabled / foxml_vol_scaling_enabled → ML
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
    // barrier_gate_enabled migrated to FOREACH_GATE_CFG_FLAG (v5.14.9.F.5; auto-extended below)
    {"peak_model_path",          "Peak Model",        "Barrier", CFG_PATH, NULL,
        "Path to P(will_peak) model (engine-wide). Per-core deferred."},
    {"valley_model_path",        "Valley Model",      "Barrier", CFG_PATH, NULL,
        "Path to P(will_valley) model (engine-wide). Per-core deferred."},
    // Per-core sharded engine — production since v4.x; legacy single_core is
    // deprecated and warns on boot. v4.7.26: removed the "Sharded Mode" toggle
    // from the GUI — sharded is the only path users should see. Cfg parser
    // still accepts engine_mode= for backwards compat with old cfg files;
    // users who really want legacy can hand-edit. No UI surface = no foot-gun.
    // v5.15.5.F.4c — num_execution_cores migrated to FOREACH_CFG_FIELD (KIND_INT; clamp [1, 16]).
    // The field_defs[] entry is DELETED at .F.4c; live_core_count sync + per-core tab count
    // now read/write s->gui_engine_cfg.num_execution_cores directly.
    {"num_execution_cores_PLACEHOLDER","Cores",       "Per-Core", CFG_INT,  "%d",
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
    // v5.15.5.F.4c — poll_interval + warmup_ticks + min_warmup_samples migrated to FOREACH_CFG_FIELD
    // (KIND_INT; HIGH-6 tooltip byte-identity preserved; warmup_ticks + min_warmup_samples tagged IS_BOOT_ONLY).
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
    //==========================================================================
    // v5.14.9.F.5 — AUTO-EXTENDED FROM FOREACH_<DOMAIN>_CFG_FLAG REGISTRIES
    //==========================================================================
    // Registry is the SINGLE SOURCE OF TRUTH for these 21 boolean cfg flags.
    // Adding a new flag = 1 row in the appropriate registry → field_defs[]
    // auto-extends → widget appears with correct label / section / tooltip.
    // See CoreFrameworks/{Lifecycle,Gate,Risk,Ops}CfgFlagRegistry.hpp +
    // ML_Headers/MlCfgFlagRegistry.hpp for full lists.
    #define X(name, legacy_field, display_label, section, doc) \
        {#legacy_field, display_label, section, CFG_BOOL, NULL, doc},
    FOREACH_LIFECYCLE_CFG_FLAG(X)
    FOREACH_GATE_CFG_FLAG(X)
    FOREACH_ML_CFG_FLAG(X)
    FOREACH_RISK_CFG_FLAG(X)
    FOREACH_OPS_CFG_FLAG(X)
    #undef X

    //==========================================================================
    // v5.15.5.F.4c — EMIT_CFG_FIELD_DEF_FROM_REGISTRY auto-extension REMOVED.
    //==========================================================================
    // FOREACH_CFG_FIELD rows now render via the bitmap-dispatch walker in
    // Settings_RenderGlobalTab (calls CfgRenderTable<64>::fns[idx] which
    // dispatches via cfg_render_and_persist → tt::cfg_render_field<T> +
    // tt::cfg_save_field<T> + cfg_write_field). Single source of truth (the
    // typed `gui_engine_cfg` instance) replaces the parallel-array indirection.
    // See DESIGN_SPECS/universal-registry-bitmap-dispatcher-pattern.md.
    //
    // field_defs[] retains: hardcoded entries (manual fields not in FOREACH_CFG_FIELD)
    // + 5 FOREACH_*_CFG_FLAG bitmap-flag X-macro entries. STRING/FILE_PATH fields
    // (notify_command, model paths, etc.) stay in hardcoded field_defs[] entries
    // until .F.4e migrates KIND_STRING/_FILE_PATH to FOREACH_CFG_FIELD.
    //==========================================================================
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
    // v5.15.5.F.4c — GUI's typed mirror of engine.cfg (Option 2 GUI refactor: direct
    // ControllerConfig<F> access for scalar Kinds replaces parallel-array indirection).
    // GUI thread owns this instance; engine HP/SP threads own their own; thread isolation
    // per CLAUDE.local.md going-forward rule "GUI ↔ HP/SP thread isolation" + DESIGN_SPECS/
    // universal-registry-bitmap-dispatcher-pattern.md § "GUI ↔ engine thread isolation".
    // File is the canonical channel: GUI edits gui_engine_cfg + persists via cfg_write_field;
    // engine reloads from file on reload_flag signal. NEVER pointer-share across threads.
    // F=64 matches the canonical engine precision; the engine binary uses one F at compile time.
    ControllerConfig<64> gui_engine_cfg{};

    // v5.15.5.F.4c — parallel-array layer (legacy; scalar-Kind entries delete as cohort migration
    // progresses; KIND_STRING/_FILE_PATH path_vals[] survives until .F.4e per TECH_DEBT-063).
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

    // v5.15.5.F.4c — populate gui_engine_cfg via ControllerConfig_Load (same path as engine
    // boot uses). GUI's typed mirror; engine has its own. File is canonical channel.
    // Note: ControllerConfig_Load reads from the same file as the parallel-array walk below
    // — slight redundancy during transition (~5-10ms cost; once per GUI session); cleaned
    // up at item 8 (Delete old paths) once all scalar Kinds migrate.
    s->gui_engine_cfg = ControllerConfig_Load<64>(s->cfg_path);

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
    // v5.15.5.F.4c — num_execution_cores moved to gui_engine_cfg (FOREACH_CFG_FIELD).
    // Apply default-fallback when cfg-file omitted the key (Settings_Load left it at 0
    // since ControllerConfig_Load itself defaults to 4; this is the GUI-side safety net).
    if (s->gui_engine_cfg.num_execution_cores < 1) {
        s->gui_engine_cfg.num_execution_cores = 4;  // matches ControllerConfig_Default
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

        // v5.15.5.F.4c.1 — ImGui widget-ID uniqueness via fd->key for legacy
        // field_defs[] path (sister fix to tt::cfg_render_field<T>'s PushID
        // wrapper above). Closes label-collision class for the residual
        // hardcoded field_defs[] rows that have not yet migrated to
        // FOREACH_CFG_FIELD (KIND_STRING / KIND_FILE_PATH cohort — .F.4e scope).
        ImGui::PushID(fd->key);

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
        ImGui::PopID();
    }

    //==========================================================================
    // v5.15.5.F.4c.3 — Bitmap-dispatch walker for two-registry architecture
    //==========================================================================
    // Two walks: first FOREACH_GLOBAL_CFG_FIELD rows, then FOREACH_PER_CORE_CFG_FIELD.
    // Today both render into `gui_engine_cfg` (flat ControllerConfig<F> instance);
    // Step 2 of .F.4c.3 will restructure per-core rows to render against
    // cfg.cores[c] in dedicated per-core tabs.
    //
    // Section-grouping mirrors the field_defs[] loop above; per-section
    // CollapsingHeader; strategy filter via global_section_strategy +
    // any_core_uses_strategy; default_open whitelist for Trading / Entry Filters /
    // EMA Gate. Rows are declared section-grouped within each registry, so
    // iteration in FIELD_IDX_* order = section-grouped order.
    //==========================================================================
    {
        const char *cfg_walker_section = NULL;
        bool cfg_walker_skip_section = false;

        // === Global registry walk ===
        CFG_FIELD_FOR_EACH_SET_BIT(g_global_cfg_render_mask.words, idx, {
            const CfgFieldDescriptor &desc = g_global_cfg_field_descriptors[idx];

            if (!cfg_walker_section || strcmp(cfg_walker_section, desc.section) != 0) {
                cfg_walker_section = desc.section;
                cfg_walker_skip_section = false;

                int sec_strat = global_section_strategy(cfg_walker_section);
                if (sec_strat >= 0 && !any_core_uses_strategy(s, sec_strat)) {
                    cfg_walker_skip_section = true;
                    continue;
                }

                bool default_open = (strcmp(desc.section, "Trading") == 0 ||
                                      strcmp(desc.section, "Entry Filters") == 0 ||
                                      strcmp(desc.section, "EMA Gate") == 0);
                if (!ImGui::CollapsingHeader(desc.section,
                        default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                    cfg_walker_skip_section = true;
                    continue;
                }
            }
            if (cfg_walker_skip_section) continue;

            ImGui::SetNextItemWidth(80);
            if (GlobalCfgRenderTable<64>::fns[idx](s->gui_engine_cfg, desc, s->cfg_path)) {
                changed = true;
            }
        });

        // === Per-core registry walk ===
        // .F.4c.3 Step 1: per-core rows still render against `gui_engine_cfg`
        // flat fields. Step 2 will move these into per-core tabs against
        // `gui_engine_cfg.cores[c]`.
        cfg_walker_section = NULL;
        cfg_walker_skip_section = false;
        CFG_FIELD_FOR_EACH_SET_BIT(g_per_core_cfg_render_mask.words, idx, {
            const CfgFieldDescriptor &desc = g_per_core_cfg_field_descriptors[idx];

            if (!cfg_walker_section || strcmp(cfg_walker_section, desc.section) != 0) {
                cfg_walker_section = desc.section;
                cfg_walker_skip_section = false;

                int sec_strat = global_section_strategy(cfg_walker_section);
                if (sec_strat >= 0 && !any_core_uses_strategy(s, sec_strat)) {
                    cfg_walker_skip_section = true;
                    continue;
                }

                bool default_open = (strcmp(desc.section, "Trading") == 0 ||
                                      strcmp(desc.section, "Entry Filters") == 0 ||
                                      strcmp(desc.section, "EMA Gate") == 0);
                if (!ImGui::CollapsingHeader(desc.section,
                        default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                    cfg_walker_skip_section = true;
                    continue;
                }
            }
            if (cfg_walker_skip_section) continue;

            ImGui::SetNextItemWidth(80);
            if (PerCoreCfgRenderTable<64>::fns[idx](s->gui_engine_cfg, desc, s->cfg_path)) {
                changed = true;
            }
        });
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

        // v5.10.0c — "Apply (live)" hot-swap button. Writes the current
        // Model Dir into TUISharedState's pending_model_path[core_id]
        // then atomic-stores the request flag. Engine slow-path consumer
        // reads with __ATOMIC_ACQUIRE, frees+reloads ml_zoos[c], swaps
        // the handle. Mirrors the strategy hot-swap pattern at
        // SettingsPanel.hpp:945.
        ImGui::SameLine();
        bool can_swap = (shared != nullptr) &&
                        (s->per_core_model_dir[core_id][0] != '\0') &&
                        (core_id < 16);
        uint8_t pending_swap = (shared && core_id < 16)
            ? __atomic_load_n(&shared->swap_model_path_requested[core_id],
                              __ATOMIC_ACQUIRE)
            : 0;
        if (pending_swap) {
            ImGui::BeginDisabled();
            ImGui::Button("swapping...");
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Hot-swap pending — engine slow-path consuming.\n"
                                   "If a position is open and "
                                   "acknowledge_hot_swap_with_open_positions=0,\n"
                                   "swap is deferred until close.");
        } else if (!can_swap) {
            ImGui::BeginDisabled();
            ImGui::Button("Apply (live)");
            ImGui::EndDisabled();
            ImGui::SetItemTooltip(
                shared == nullptr
                    ? "Hot-swap unavailable (no shared state)"
                    : "Set Model Dir first");
        } else {
            ImGui::PushID("mdir_apply_live");
            if (ImGui::Button("Apply (live)")) {
                // Write-then-flag pattern: copy path, then atomic-store flag.
                // Reader uses __ATOMIC_ACQUIRE so the path is visible.
                size_t n = strnlen(s->per_core_model_dir[core_id], 255);
                memcpy(shared->pending_model_path[core_id],
                        s->per_core_model_dir[core_id], n);
                shared->pending_model_path[core_id][n] = '\0';
                __atomic_store_n(&shared->swap_model_path_requested[core_id],
                                 (uint8_t)1, __ATOMIC_RELEASE);
            }
            ImGui::PopID();
            ImGui::SetItemTooltip(
                "Hot-swap to this Model Dir without restarting the engine.\n"
                "Engine reloads all 4 roles (barrier, regime, exit, buy_signal)\n"
                "from the new dir on the next slow-path cycle.\n\n"
                "If a position is open at swap time, behavior depends on cfg:\n"
                "  acknowledge_hot_swap_with_open_positions=0 (default):\n"
                "    swap is deferred until position closes (safer)\n"
                "  acknowledge_hot_swap_with_open_positions=1:\n"
                "    swap proceeds immediately, position exits on new model");
        }
    }

    // v5.11.59 — resolve this core's strategy for the strategy-aware filter.
    // Prefer the operator's DROPDOWN selection (configuration intent) over the
    // live snapshot (current engine state). Reasoning: when the operator picks
    // ML in the dropdown, they want to see ML fields immediately to configure
    // them — not stay looking at EMA fields just because the engine is still
    // running EMA pre-Apply. Fall back to snapshot if no dropdown choice yet,
    // last resort STRATEGY_NONE.
    int core_strategy = STRATEGY_NONE;
    if (s->per_core_strategy[core_id] >= 0) {
        core_strategy = s->per_core_strategy[core_id];
    } else if (snap && snap->sharded_mode_active && core_id < snap->per_core_count) {
        core_strategy = snap->per_core[core_id].strategy_id_display;
    }

    // v5.11.61 — ML Ensemble panel. Surfaces what
    // EnsembleModelZoo_AutoDetectFromDir found at boot (per-horizon
    // detection, scaler/stamp state, current bandit weights per regime).
    // Read-only display + checkbox writes core_N_disabled_horizons CSV
    // back to cfg (operator must restart or 'r' reload to apply). Only
    // renders when this core is ML (strategy filter) AND ensemble is active.
    if (core_strategy == STRATEGY_ML && snap && snap->sharded_mode_active &&
        core_id < snap->per_core_count &&
        snap->per_core[core_id].ensemble_active) {
        if (ImGui::CollapsingHeader("ML Ensemble", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto &pcs = snap->per_core[core_id];
            int n_h = (int)pcs.ensemble_n_horizons;
            ImGui::Text("Blend mode: %s   n_horizons: %d",
                        pcs.ensemble_blend_mode, n_h);
            const char* regime_names[] = {"RANGING", "TRENDING", "VOLATILE", "MILD_TREND", "(r4)"};
            int last_r = pcs.ensemble_last_predicted_regime;
            int last_h = pcs.ensemble_last_predicted_horizon_idx;
            if (last_r >= 0 && last_r < 5 && last_h >= 0 && last_h < n_h) {
                ImGui::TextColored(FoxmlColors::comment,
                    "Last predict: regime=%s, dominant horizon=%d",
                    regime_names[last_r], pcs.ensemble_horizon_ticks[last_h]);
            }
            ImGui::Separator();

            // Header row
            if (ImGui::BeginTable("ensemble_horizons", 7,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("Horizon", ImGuiTableColumnFlags_WidthFixed, 80);
                for (int r = 0; r < 4; ++r) {
                    ImGui::TableSetupColumn(regime_names[r],
                        ImGuiTableColumnFlags_WidthFixed, 80);
                }
                ImGui::TableSetupColumn("Updates", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableHeadersRow();

                bool any_toggle = false;
                uint32_t new_mask = pcs.ensemble_disabled_horizon_mask;
                for (int h = 0; h < n_h; ++h) {
                    int H = pcs.ensemble_horizon_ticks[h];
                    bool enabled = !(pcs.ensemble_disabled_horizon_mask & (1u << h));
                    ImGui::TableNextRow();
                    // Enabled checkbox — writes core_N_disabled_horizons CSV
                    ImGui::TableNextColumn();
                    ImGui::PushID(h);
                    bool prev_enabled = enabled;
                    if (ImGui::Checkbox("##en", &enabled)) {
                        if (prev_enabled != enabled) {
                            if (enabled) new_mask &= ~(1u << h);
                            else         new_mask |=  (1u << h);
                            any_toggle = true;
                        }
                    }
                    ImGui::PopID();
                    // Horizon ticks
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", H);
                    // Per-regime bandit weight columns (4 main regimes)
                    for (int r = 0; r < 4; ++r) {
                        ImGui::TableNextColumn();
                        double w = pcs.ensemble_weights[r][h];
                        // Tint by weight: high weight = primary color
                        if (w > 0.4) {
                            ImGui::TextColored(FoxmlColors::primary, "%.3f", w);
                        } else if (w > 0.1) {
                            ImGui::Text("%.3f", w);
                        } else {
                            ImGui::TextDisabled("%.3f", w);
                        }
                    }
                    // Updates count column (use first regime's count as proxy)
                    ImGui::TableNextColumn();
                    int n_upd = pcs.ensemble_n_updates_per_regime[0];
                    ImGui::Text("%d", n_upd);
                }
                ImGui::EndTable();

                // If operator toggled any checkbox, recompute the
                // disabled_horizons CSV and write to cfg. Engine picks
                // it up on next 'r' reload or restart.
                if (any_toggle) {
                    char csv[128] = {0};
                    size_t off = 0;
                    int n_disabled = 0;
                    for (int h = 0; h < n_h; ++h) {
                        if (new_mask & (1u << h)) {
                            int wrote = snprintf(csv + off, sizeof(csv) - off,
                                "%s%d", n_disabled == 0 ? "" : ",",
                                pcs.ensemble_horizon_ticks[h]);
                            if (wrote > 0) off += wrote;
                            n_disabled++;
                        }
                    }
                    char key[64];
                    snprintf(key, sizeof(key), "core_%d_disabled_horizons", core_id);
                    cfg_write_field(s->cfg_path, key, csv);
                    fprintf(stderr, "[settings] wrote %s=%s — press 'r' in "
                                    "TUI or restart engine to apply\n",
                            key, csv[0] ? csv : "(none)");
                    changed = true;
                }
            }
            ImGui::TextColored(FoxmlColors::comment,
                "Toggle requires engine restart OR 'r' hot-reload to apply.\n"
                "Bandit weights drift toward better-performing horizons per regime.");
        }
    } else if (core_strategy == STRATEGY_ML && snap && snap->sharded_mode_active &&
               core_id < snap->per_core_count &&
               !snap->per_core[core_id].ensemble_active) {
        // ML core but no ensemble active — surface why
        if (ImGui::CollapsingHeader("ML Ensemble", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextColored(FoxmlColors::comment,
                "Ensemble not active — single-zoo path or no _horizon_<N> "
                "siblings detected at base path.");
            ImGui::TextColored(FoxmlColors::comment,
                "If you trained multi-horizon models, check that "
                "core_%d_model_dir points at the BASE path "
                "(without _horizon_<H> suffix) and engine.log shows "
                "[sharded] core %d: ensemble active.", core_id, core_id);
        }
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
        // v5.15.5.F.4c — sync live_core_count into gui_engine_cfg (was: float_vals[i]).
        s->gui_engine_cfg.num_execution_cores = (uint16_t)live_core_count;
    }

    ImGui::TextColored(FoxmlColors::primary, "ENGINE SETTINGS");
    ImGui::TextColored(FoxmlColors::comment, "edit + press Enter to apply");
    ImGui::Separator();

    // v5.11.10 — font scale slider. Session-only (no cfg persistence yet).
    // ImGuiIO::FontGlobalScale is a runtime multiplier on the loaded font
    // size; values < 1.0 shrink, > 1.0 enlarge. Slightly blurry on
    // non-integer scales (mitigated by Hack Nerd Font's hinting). For
    // crisp text on a different size, rebuild the font atlas at startup
    // with a smaller pixel size — deferred polish.
    {
        // v5.11.37 — slider min lowered 0.7 → 0.5 for 1080p operators.
        // Default 0.6 (set at GuiThread.hpp boot). Persists across
        // sessions via data/foxml_gui_state.txt. Operator drags →
        // immediate FontGlobalScale change → atomic-rename write to
        // disk. Next boot reads back.
        float font_scale = ImGui::GetIO().FontGlobalScale;
        ImGui::PushItemWidth(140.0f);
        if (ImGui::SliderFloat("font scale", &font_scale, 0.5f, 1.5f, "%.2fx")) {
            ImGui::GetIO().FontGlobalScale = font_scale;
            // Persist. Atomic write: tmp + rename so a concurrent boot
            // never sees a partial file. Best-effort; failures are
            // silent (worst case = next boot uses default 0.6).
            mkdir("data", 0755);  // EEXIST is fine
            FILE* tmp = fopen("data/foxml_gui_state.txt.tmp", "w");
            if (tmp) {
                fprintf(tmp, "font_scale=%.3f\n", font_scale);
                fclose(tmp);
                rename("data/foxml_gui_state.txt.tmp",
                       "data/foxml_gui_state.txt");
            }
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::TextColored(FoxmlColors::comment,
            "(persisted: data/foxml_gui_state.txt)");
        ImGui::Separator();
    }

    // Tabs match live registered cores when available; else fall back to
    // cfg num_execution_cores; else default 4. Avoids the "I have 4 cores
    // but only 1 tab" bug when num_execution_cores is missing from cfg
    // (cfg defaults to 4 on the engine side, but Settings_Load only sees
    // what's literally written in the file).
    int num_cores = 0;
    if (live_core_count > 0 && live_core_count <= MAX_GUI_CORES) {
        num_cores = live_core_count;
    } else {
        // v5.15.5.F.4c — read num_cores from gui_engine_cfg (was: field_defs[]+float_vals[]).
        num_cores = (int)s->gui_engine_cfg.num_execution_cores;
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
