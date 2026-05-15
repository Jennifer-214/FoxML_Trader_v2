// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
//======================================================================================================
// [UNIVERSAL CFG FIELD REGISTRY]
//======================================================================================================
// v5.15.5.F.4b — single source of truth for cfg field declarations across consumers
// (parser, save, GUI render, per-core override emission, drift check, cfg.example doc).
//
// Architecture: 12-col Option D X-macro tuple per
//   DESIGN_SPECS/registry-tuple-as-single-source-of-truth.md.
// Type-trait dispatch via tt:: namespace per CLAUDE.md item 23 +
//   DESIGN_SPECS/type-trait-dispatch-via-tt-namespace.md.
// Categorical applicability columns per
//   DESIGN_SPECS/categorical-tag-applicability-pattern.md.
// 3-barrier structural fix for Class 23 (DOCS/RECURRING_BUG_PATTERNS.md) —
// Kind enum is METADATA-ONLY; type dispatch happens via tt::cfg_*_field<T>
// with T deduced from the cfg field reference (NEVER via reinterpret_cast).
//
// .F.4b SCOPE: KIND_DOUBLE + KIND_DOUBLE_PCT only (~40 cfg fields).
// .F.4c   adds: KIND_INT + KIND_INT_ENUM + KIND_BOOL + STAMP_BOUND derived filter cutover.
// .F.4d   adds: KIND_STRING + KIND_FILE_PATH + cfg.example auto-gen + reverse-drift CI script.
// .F.4e+  adds: ResolvedCoreCfg + per-core override + categorical audit + cross-cfg-file rows.
//
// DESCRIPTOR SCHEMA LOCKS AT .F.4b — subsequent sub-ships ADD ROWS only;
// no schema changes. (Per "design upfront + ship in waves" decision 2026-05-14.)
//======================================================================================================
#pragma once
#include <cstdint>
#include <cstddef>      // size_t
#include "../Strategies/StrategyCategories.hpp"   // STRAT_CAT_*
#include "../Strategies/OpModeCategories.hpp"     // OP_MODE_CAT_*

// Forward declarations for category masks not yet populated at .F.4b
// (defaulted to _CAT_ALL until v5.16+ specializes regime/risk dimensions).
enum RegimeCategoryDefault : uint16_t { REGIME_CAT_ALL = 0xFFFFu };
enum RiskCategoryDefault   : uint16_t { RISK_CAT_ALL   = 0xFFFFu };

//======================================================================================================
// [CFG FIELD DESCRIPTOR]
//======================================================================================================
struct CfgFieldDescriptor {
    //---- Kind enum (uint8_t) — METADATA-ONLY ----
    // Drives GUI presentation (slider vs textbox, format string, % suffix, clamp coercion).
    // Does NOT drive type dispatch — that happens via tt::cfg_*_field<T> with T deduced
    // from the destination field. See DESIGN_SPECS/type-trait-dispatch-via-tt-namespace.md
    // + DOCS/RECURRING_BUG_PATTERNS.md Class 23.
    enum Kind : uint8_t {
        KIND_DOUBLE       = 0,   // raw double or FPN<F>; clamp_min/max in payload.as_double
        KIND_DOUBLE_PCT   = 1,   // double formatted as percent in GUI (×100 + "%" suffix); stored as fraction
        KIND_INT          = 2,   // signed/unsigned int (.F.4c)
        KIND_INT_ENUM     = 3,   // int with enum labels (radio/dropdown) (.F.4c)
        KIND_BOOL         = 4,   // 0/1; scalar or bitmap bit (.F.4c)
        KIND_STRING       = 5,   // char[N] / std::string (.F.4d)
        KIND_FILE_PATH    = 6,   // string + file picker hint (.F.4d)
        // RESERVED for future:
        // KIND_RANGE_INT    = 7,   // hyperparameter sweep (v5.15.6.C)
        // KIND_RANGE_DOUBLE = 8,   // hyperparameter sweep (v5.15.6.C)
    };

    //---- MetadataFlag enum (uint16_t) — 12 bits used; 4 bits headroom ----
    enum MetadataFlag : uint16_t {
        PER_CORE_OK           = 1u << 0,   // emit per-core override (.F.4g consumer)
        RESTART_REQUIRED      = 1u << 1,   // GUI badge: "restart needed" (.F.4d render)
        SAFETY_CRITICAL       = 1u << 2,   // GUI warning + confirmation prompt (.F.4d)
        DEPRECATED            = 1u << 3,   // GUI: strikethrough + tooltip (.F.4d)
        STAMP_BOUND           = 1u << 4,   // include in stamp drift check + FOREACH_STAMP_BOUND_CFG_DERIVED filter (.F.4c)
        HIDDEN_BY_DEFAULT     = 1u << 5,   // GUI: collapsed section (.F.4d)
        IS_SECRET             = 1u << 6,   // password-masking in GUI; never logged (v5.15.6.B)
        IS_BOOT_ONLY          = 1u << 7,   // changes after boot are ignored — restart required (.F.4d)
        AFFECTS_STAMP_PARITY  = 1u << 8,   // training-time concern; bound to model stamp (.F.4c)
        LOG_VALUE_FORBIDDEN   = 1u << 9,   // value never appears in logs (privacy/security; v5.15.6.B)
        HAS_SIDE_EFFECT       = 1u << 10,  // parser-time side effect prevents registry walker handling — manual parser block keeps logic (.F.4c)
        WARN_ON_CLAMP         = 1u << 11,  // emit "[cfg] WARN: <key>='<val>' out of range; clamping to <clamped>" when parse clamps value (.F.4c)
        // ... ~4 bits headroom for future ...
    };

    //---- LivesInStruct enum (uint8_t) — cross-cfg-file routing ----
    // .F.4b only populates STRUCT_CFG; full enum declared for v5.15.6 forward-compat.
    enum LivesInStruct : uint8_t {
        STRUCT_CFG             = 0,  // engine.cfg → ControllerConfig<F>
        STRUCT_BACKTEST_CFG    = 1,  // backtest.cfg → BacktestCfg (.F.4i)
        STRUCT_CONTROLLER_CFG  = 2,  // controller.cfg → ControllerCfg (v5.15.6.A)
        STRUCT_SECRETS_CFG     = 3,  // secrets.cfg → SecretsCfg (v5.15.6.B)
        STRUCT_TRAINING_CFG    = 4,  // training cfg → TrainingCfg (v5.15.6.C)
    };

    //---- Header (8 bytes) ----
    Kind          kind;             // 1 byte
    uint8_t       lives_in_struct;  // 1 byte (LivesInStruct enum value)
    uint16_t      metadata_flags;   // 2 bytes
    uint16_t      _reserved = 0;    // 2 bytes — future use; default-init 0 per CLAUDE.md item 27
    uint16_t      field_idx;        // 2 bytes (FIELD_IDX_<name>; for sidecar-table lookup)

    //---- String pointers (32 bytes) ----
    const char*   cfg_field_name;   // matches Cfg::<name>; used by parser strcmp + save key lookup
    const char*   label;            // GUI label (e.g. "TP %%")
    const char*   section;          // GUI section heading (e.g. "Trading")
    const char*   tooltip;          // GUI tooltip + cfg.example comment (multi-line via R"(...)" allowed)

    //---- Categorical applicability masks (10 bytes — see categorical-tag-applicability-pattern.md) ----
    uint32_t      applies_to_strategy_cat;   // STRAT_CAT_* bitmap (32 max categories)
    uint16_t      applies_to_op_mode_cat;    // OP_MODE_CAT_* bitmap (16 max)
    uint16_t      applies_to_regime_cat;     // REGIME_CAT_* bitmap (default = REGIME_CAT_ALL until v5.16)
    uint16_t      applies_to_risk_cat;       // RISK_CAT_* bitmap (default = RISK_CAT_ALL until v5.16)

    //---- Runtime cfg gating (8 bytes) ----
    const char*   requires_cfg;     // gating expression ("bandit_algorithm == THOMPSON"); null if always applicable

    //---- Payload union (32 bytes — clamp_min/max inline; no sparse-table indirection) ----
    union {
        struct { double  default_val; double  clamp_min; double  clamp_max; }                                              as_double;   // KIND_DOUBLE, KIND_DOUBLE_PCT
        struct { int64_t default_val; int64_t clamp_min; int64_t clamp_max; }                                              as_int;      // KIND_INT
        struct { int default_val; const char* const* labels; uint8_t count; uint8_t _pad[3]; uint64_t _pad2[2]; }          as_int_enum; // KIND_INT_ENUM (.F.4c)
        struct { uint8_t default_val; uint8_t _pad[7]; uint64_t _pad2[3]; }                                                as_bool;     // KIND_BOOL (.F.4c)
        struct { const char* default_val; size_t buf_len; uint64_t _pad[2]; }                                              as_string;   // KIND_STRING (.F.4d)
    } payload;
};

// Cache-line budget per latency-vs-cache-decision-framework.md:
// 128B (2 cache lines) accepts inline clamp_min/max; cfg metadata is read at boot
// (parser) + 60 Hz (GUI render) — cache-warm; not latency-critical.
static_assert(sizeof(CfgFieldDescriptor) <= 128,
              "CfgFieldDescriptor must fit two cache lines (128B). "
              "Cache impact is hygiene-level (GUI render is 60 Hz, cache-warm). "
              "See DESIGN_SPECS/latency-vs-cache-decision-framework.md.");

// Bitmap overflow guards per DESIGN_SPECS/bitmap-overflow-protection-discipline.md.
// CLAUDE.local.md going-forward rule "Bitmap overflow static_assert is mandatory" (2026-05-14).
static_assert(CfgFieldDescriptor::WARN_ON_CLAMP < (1u << 16),
              "CfgFieldDescriptor::MetadataFlag bitmap overflowed uint16_t — upgrade to uint32_t");

//======================================================================================================
// [FOREACH_CFG_FIELD — 12-col Option D tuple]
//======================================================================================================
// Tuple (12 args):
//   X(KIND_TOKEN, name, label, section, meta, payload, tooltip,
//     applies_to_strategy_cat, applies_to_op_mode_cat,
//     applies_to_regime_cat, applies_to_risk_cat, lives_in_struct)
//
// payload macro: DBL(default, min, max) for KIND_DOUBLE / KIND_DOUBLE_PCT
//                (other Kind payload macros land at .F.4c/.F.4d)
//
// .F.4b INITIAL POPULATION (~40 KIND_DOUBLE/_PCT fields).
// All initial rows: lives_in_struct = STRUCT_CFG, op_mode = OP_MODE_CAT_ALL,
// regime/risk = _CAT_ALL. applies_to_strategy_cat refined per cohort at .F.4h.
// Tooltips initial-populated; HIGH-6 byte-identity preservation refined at T12 (T7.4).
//======================================================================================================

// Payload helper macros (one per Kind family):
#define DBL(default_val, clamp_min, clamp_max) { .as_double = { (default_val), (clamp_min), (clamp_max) } }
// v5.15.5.F.4c — KIND_INT / KIND_BOOL / KIND_INT_ENUM payload macros.
// INT: signed/unsigned widths (int8/16/32/64) all unified under KIND_INT per
// H13/H14 (Kind = GUI metadata; T deduced via X-macro extractor handles width).
// Storage-width safety: per-row static_assert that clamp fits destination type's
// numeric_limits is enforced at the FOREACH_CFG_FIELD walker site.
#define INT(default_val, clamp_min, clamp_max) \
    { .as_int = { (int64_t)(default_val), (int64_t)(clamp_min), (int64_t)(clamp_max) } }
#define BOOL(default_val) { .as_bool = { (uint8_t)(default_val) } }
#define INT_ENUM(default_val, labels_array, count) \
    { .as_int_enum = { (int)(default_val), (labels_array), (uint8_t)(count) } }

// NOTE: tooltips for fields PRE-EXISTING in GUI/SettingsPanel.hpp:46-289 field_defs[]
// preserved BYTE-IDENTICAL via raw strings. Fields NEW to GUI (no pre-existing entry)
// have author-supplied tooltips. HIGH-6 tooltip-preservation discipline per
// plan + DESIGN_SPECS/registry-tuple-as-single-source-of-truth.md.

#define FOREACH_CFG_FIELD(X)                                                                                                                                                                                       \
    /* === Trading (PCT cohort) === */                                                                                                                                                                              \
    X(KIND_DOUBLE_PCT, take_profit_pct,             "TP %%",                "Trading",         0,                                  DBL(3.0, 0.0, 100.0),    nullptr,                                                                                          STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, stop_loss_pct,               "SL %%",                "Trading",         0,                                  DBL(1.5, 0.0, 100.0),    nullptr,                                                                                          STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, fee_rate,                    "Fee %%",               "Trading",         0,                                  DBL(0.1, 0.0, 5.0),                                                                                                              \
        "Legacy fee rate (% per trade) — used for pre-trade quantity computations\n"                                                                                                                                                                              \
        "(no-trade band, fee floor for TP, kill switch estimate, spread display)\n"                                                                                                                                                                               \
        "and as the default for fee_rate_maker / fee_rate_taker if those aren't set.",                                                                                                                                                                            \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, slippage_pct,                "Slippage %%",          "Trading",         0,                                  DBL(0.05, 0.0, 5.0),     nullptr,                                                                                         STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, risk_pct,                    "Risk/Pos %%",          "Trading",         0,                                  DBL(2.0, 0.0, 100.0),    nullptr,                                                                                         STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     fee_floor_mult,              "Fee Floor",            "Trading",         0,                                  DBL(3.0, 1.0, 20.0),                                                                                                             \
        "TP floor = entry * fee_rate * this\n3.0 = TP must clear round-trip fees + margin",                                                                                                                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Entry Filters === */                                                                                                                                                                                     \
    X(KIND_DOUBLE_PCT, entry_offset_pct,            "Offset %%",            "Entry Filters",   CfgFieldDescriptor::PER_CORE_OK,    DBL(0.15, 0.0, 5.0),                                                                                                            \
        "Buy gate offset below avg/EMA price\nhigher = deeper dip required to enter",                                                                                                                                                                             \
        STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, offset_min,                  "Offset Min %%",        "Entry Filters",   CfgFieldDescriptor::PER_CORE_OK,    DBL(0.05, 0.0, 5.0),     "Adaptation lower bound for offset_pct",                                                          STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, offset_max,                  "Offset Max %%",        "Entry Filters",   CfgFieldDescriptor::PER_CORE_OK,    DBL(0.50, 0.0, 5.0),     "Adaptation upper bound for offset_pct",                                                          STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     volume_multiplier,           "Vol Mult",             "Entry Filters",   0,                                  DBL(2.5, 0.0, 100.0),                                                                                                            \
        "Volume gate: require avg_volume * this\nhigher = only buy on high volume",                                                                                                                                                                               \
        STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     spacing_multiplier,          "Spacing",              "Entry Filters",   0,                                  DBL(2.0, 0.0, 20.0),                                                                                                             \
        "Min distance between entries (in stddev)\nprevents clustering entries at similar prices",                                                                                                                                                                \
        STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     min_long_slope,              "Min Long Slope",       "Entry Filters",   0,                                  DBL(0.0, -1.0, 1.0),                                                                                                             \
        "Block MR buys when 512-tick slope below this\nnegative = allow mild dips, 0 = disabled",                                                                                                                                                                 \
        STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     min_buy_delta,               "Min Buy Delta",        "Entry Filters",   0,                                  DBL(-0.3, -1.0, 1.0),                                                                                                            \
        "Min volume delta for MR buys\n-0.3 = allow mild selling, block heavy dumps",                                                                                                                                                                             \
        STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     vwap_offset,                 "VWAP Offset",          "Entry Filters",   0,                                  DBL(0.0, 0.0, 0.1),      nullptr,                                                                                          STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     min_stddev_pct,              "Min Stddev %%",        "Entry Filters",   0,                                  DBL(0.0, 0.0, 0.1),                                                                                                              \
        "Skip trades when stddev/price below this\nprevents entries in dead-flat markets",                                                                                                                                                                        \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Time-Based Exit === */                                                                                                                                                                                   \
    X(KIND_DOUBLE,     tp_hold_score,               "TP Hold Score",        "Time-Based Exit", CfgFieldDescriptor::PER_CORE_OK,    DBL(0.0, 0.0, 10.0),     "Min SNR*R² to hold past TP (0 = disabled, fixed TP)",                                            STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     tp_trail_mult,               "TP Trail Mult",        "Time-Based Exit", CfgFieldDescriptor::PER_CORE_OK,    DBL(1.0, 0.0, 10.0),     "Trailing distance: stddev * this (e.g. 1.0)",                                                    STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     sl_trail_mult,               "SL Trail Mult",        "Time-Based Exit", CfgFieldDescriptor::PER_CORE_OK,    DBL(2.0, 0.0, 10.0),     "Trailing SL distance: stddev * this (e.g. 2.0)",                                                 STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Risk Management === */                                                                                                                                                                                   \
    X(KIND_DOUBLE_PCT, max_drawdown_pct,            "Max DD %%",            "Risk Management", 0,                                  DBL(20.0, 0.0, 100.0),                                                                                                           \
        "Circuit breaker: halt trading if total P&L\ndrops below this %% of starting balance",                                                                                                                                                                    \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, max_exposure_pct,            "Max Exp %%",           "Risk Management", 0,                                  DBL(50.0, 0.0, 100.0),   nullptr,                                                                                          STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Kill Switch === */                                                                                                                                                                                       \
    X(KIND_DOUBLE_PCT, kill_switch_daily_loss_pct,  "Daily Loss %%",        "Kill Switch",     CfgFieldDescriptor::SAFETY_CRITICAL,DBL(3.0, 0.0, 100.0),                                                                                                           \
        "Max session loss before kill switch triggers\n3.0 = halt if equity drops 3%% from session start",                                                                                                                                                        \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, kill_switch_drawdown_pct,    "Drawdown %%",          "Kill Switch",     CfgFieldDescriptor::SAFETY_CRITICAL,DBL(5.0, 0.0, 100.0),                                                                                                           \
        "Max drawdown from session peak before kill\n5.0 = halt if 5%% below intra-session high",                                                                                                                                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Momentum (Strategies/Momentum.hpp) === */                                                                                                                                                                \
    X(KIND_DOUBLE,     momentum_min_tp_margin_pct,  "Mom Min TP Margin",    "Strategies",      CfgFieldDescriptor::PER_CORE_OK,    DBL(0.0, 0.0, 0.05),     "Block momentum entry if TP too tight (0 = disabled; rec: 0.0040 = 0.40%)",                       STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     momentum_min_buy_delta_recent, "Mom Min Buy Delta", "Strategies",       CfgFieldDescriptor::PER_CORE_OK,    DBL(0.0, 0.0, 1.0),      "Min recent volume delta for momentum entry (rec: 0.05)",                                          STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     momentum_min_r2,             "Mom Min R²",           "Strategies",      CfgFieldDescriptor::PER_CORE_OK,    DBL(0.0, 0.0, 1.0),      "Min short_r2 for momentum entry (0 = disabled; rec: 0.30)",                                       STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     momentum_tp_mult,            "Mom TP Mult",          "Strategies",      0,                                  DBL(3.0, 0.0, 10.0),     "TP multiplier for momentum (e.g. 3.0 stddevs)",                                                  STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     momentum_sl_mult,            "Mom SL Mult",          "Strategies",      0,                                  DBL(1.0, 0.0, 10.0),     "SL multiplier for momentum (e.g. 1.0 stddevs)",                                                  STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     momentum_breakout_mult,      "Mom Breakout Mult",    "Strategies",      0,                                  DBL(0.5, 0.0, 10.0),     "Momentum breakout floor in stddevs",                                                              STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === EMA Cross (Strategies/EmaCross.hpp) === */                                                                                                                                                               \
    X(KIND_DOUBLE,     emacross_dip_mult,           "EMACross Dip Mult",    "Strategies",      0,                                  DBL(0.5, 0.0, 10.0),     "Buy this many stddevs below EMA",                                                                STRAT_CAT_STATIC_RULES,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     emacross_crossover_min,      "EMACross Crossover",   "Strategies",      0,                                  DBL(0.0, 0.0, 1.0),      "Min EMA-SMA spread for uptrend confirmation",                                                    STRAT_CAT_STATIC_RULES,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     emacross_trail_mult,         "EMACross Trail Mult",  "Strategies",      0,                                  DBL(1.0, 0.0, 10.0),     "Trailing TP factor when EMA rising",                                                              STRAT_CAT_STATIC_RULES,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Regime Detection (matches existing GUI section) === */                                                                                                                                                   \
    X(KIND_DOUBLE,     regime_slope_threshold,      "Regime Slope Thresh",  "Regime Detection",0,                                  DBL(0.0, 0.0, 1.0),      "Relative slope magnitude for TRENDING classification",                                            STRAT_CAT_REGIME_AWARE,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     regime_crossover_threshold,  "Mild Trend",           "Regime Detection",0,                                  DBL(0.0005, 0.0, 1.0),                                                                                                           \
        "EMA/SMA spread for MILD_TREND (EMA Cross)\n0.0005 = 0.05%% gap (~$35 at BTC $68k)\nbelow = RANGING, above = mild uptrend",                                                                                                                               \
        STRAT_CAT_REGIME_AWARE,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     regime_strong_crossover,     "Strong Trend",         "Regime Detection",0,                                  DBL(0.0015, 0.0, 1.0),                                                                                                           \
        "EMA/SMA spread for strong TRENDING (Momentum)\n0.0015 = 0.15%% gap (~$102 at BTC $68k)\nabove = Momentum, below = EMA Cross",                                                                                                                            \
        STRAT_CAT_REGIME_AWARE,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, regime_r2_threshold,         "R² Threshold",         "Regime Detection",0,                                  DBL(70.0, 0.0, 100.0),                                                                                                           \
        "Min R-squared consistency for TRENDING\n70 = 70%% of price variance explained by trend",                                                                                                                                                                 \
        STRAT_CAT_REGIME_AWARE,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === ML / Bandit / Confidence === */                                                                                                                                                                          \
    X(KIND_DOUBLE,     ml_buy_threshold,            "ML Buy Thresh",        "ML",              0,                                  DBL(0.5, 0.0, 1.0),      "Buy threshold for ML strategy (predictions above this enter)",                                    STRAT_CAT_ML,                                                  OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, ml_tp_pct,                   "ML TP %%",             "ML",              0,                                  DBL(2.0, 0.0, 100.0),    "ML strategy take profit (overrides take_profit_pct)",                                            STRAT_CAT_ML,                                                  OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, ml_sl_pct,                   "ML SL %%",             "ML",              0,                                  DBL(1.0, 0.0, 100.0),    "ML strategy stop loss (overrides stop_loss_pct)",                                                STRAT_CAT_ML,                                                  OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     bandit_blend_ratio,          "Bandit Blend",         "ML",              CfgFieldDescriptor::PER_CORE_OK,    DBL(0.5, 0.0, 1.0),      "Mix of bandit picks vs base model",                                                              STRAT_CAT_ML | STRAT_CAT_USES_BANDIT,                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     confidence_threshold_scale,  "Conf Thresh Scale",    "ML",              CfgFieldDescriptor::PER_CORE_OK,    DBL(1.0, 0.0, 5.0),      "Confidence-weighted entry threshold scaling",                                                    STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Partial exits / Misc === */                                                                                                                                                                              \
    X(KIND_DOUBLE,     partial_exit_pct,            "Partial Exit %%",      "Partial Exits",   0,                                  DBL(0.5, 0.0, 1.0),      "Fraction of position to close at TP1 (0.5 = 50%)",                                               STRAT_CAT_ALL,                                                   OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     tp2_mult,                    "TP2 Mult",             "Partial Exits",   0,                                  DBL(2.0, 0.0, 10.0),     "TP2 distance = TP1_distance * this (2.0 = double TP)",                                           STRAT_CAT_ALL,                                                   OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, breakeven_buffer_pct,        "Breakeven Buf %%",     "Partial Exits",   0,                                  DBL(0.0, 0.0, 5.0),      "SL offset from entry once breakeven ratchet fires",                                              STRAT_CAT_ALL,                                                   OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === C1a: Operational — boot-time-immutable system tuning (v5.15.5.F.4c) === */                                                                                                                                \
    X(KIND_BOOL,       require_mlockall,            "Require mlockall",     "Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                       \
        "Pin engine memory at boot via mlockall(2) — prevents swap-out under memory pressure. Requires CAP_IPC_LOCK or root. Boot-only; runtime changes ignored.",                                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       init_arena_use_hugepages,    "Use Hugepages",        "Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                       \
        "Initialize per-core arenas with 2MB hugepages (MAP_HUGETLB). Reduces TLB pressure on hot path. Requires /sys/kernel/mm/hugepages configured. Boot-only.",                                                   \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === C1a: Drift Acknowledgments (v5.15.5.F.4c) === */                                                                                                                                                          \
    X(KIND_BOOL,       acknowledge_hardcoded_strategy_in_live, "Ack Hardcoded Strategy in Live", "Drift Acknowledgments", CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),             \
        "Explicit acknowledgment required to run hardcoded strategy (no per-core override) in live mode. Safety gate; operator must opt-in.",                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       acknowledge_hot_swap_with_open_positions, "Ack Hot Swap w/ Open Positions", "Drift Acknowledgments", CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                              \
        "Explicit acknowledgment to hot-swap strategy/model while positions are open. Without this, hot-swap is DEFERRED until position closes (v5.10.0c default).",                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       allow_cross_major_engine,    "Allow Cross-Major Engine", "Drift Acknowledgments", CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                              \
        "Allow engine to load when model_path is from a different major version. v5.9.2b — refuse by default; explicit override to enable cross-major migration.",                                                   \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === C1b: Runtime operator toggles — HIGH-6 tooltip byte-identity preserved (v5.15.5.F.4c) === */                                                                                                              \
    X(KIND_BOOL,       danger_enabled,              "Enabled",              "Danger Gradient", CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        nullptr,                                                                                                                                                                                                    \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       record_ticks,                "Record Ticks",         "Tick Recording",  CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "Record raw ticks to CSV for backtesting/ML training\nOutput: data/{SYMBOL}/YYYY-MM-DD.csv\n~30-70MB/day for BTCUSDT",                                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       record_depth,                "Record Depth",         "Tick Recording",  CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "Record @depth5@100ms snapshots to CSV (top-of-book + lastUpdateId)\nOutput: data/{SYMBOL}/depth/YYYY-MM-DD.csv\nRequires depth_enabled=1. Daily rotation, auto-pruned by record_max_days.\nGap markers (# GAP) on backward last_update_id, wallclock >2s, or disconnect.\n~50 MB/day for BTCUSDT. Required for future backtest replay of book state.",                                                                                                                                                                                                                                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === C1c: Parser-only basic bools — strategy/op-mode/ML metadata (v5.15.5.F.4c) === */                                                                                                                          \
    X(KIND_BOOL,       momentum_require_last_win,   "Require Last Win",     "Momentum Tuning", CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "SHALT_MOM_LAST_LOST gate — block Momentum re-entry until previous trade was a TP win. 0 = off (default; recommended). 1 = enable; favors winning streaks at cost of slower recovery.",                     \
        STRAT_CAT_REGRESSION_DRIVEN,                         OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       enable_mtm_kill_switch,      "MTM Kill Switch",      "Kill Switch",     CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "Mark-to-market kill switch — halt new entries when realized + unrealized P&L crosses kill_switch_threshold_pct. Separate from balance-based kill switch (always armed).",                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       use_aot_inference,           "Use AOT Inference",    "ML",              CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                       \
        "Use ahead-of-time-compiled inference path instead of XGBoost runtime. Faster per-tick (~50ns vs ~500ns) but requires AOT model build via tools/aot_compile. Boot-only.",                                    \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       pay_fees_in_bnb,             "Pay Fees in BNB",      "Trading",         CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "Binance fee discount — pay trading fees in BNB tokens for 25% discount. Operator-side cfg only; actual discount applied by Binance per account settings.",                                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === C1d: Misc bool migrations — gate recovery, debug toggle, ML training (v5.15.5.F.4c) === */                                                                                                                 \
    X(KIND_BOOL,       sl_cooldown_adaptive,        "Adaptive CD",          "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "Post-stop-loss cooldown mode: 0 = fixed cycles (sl_cooldown_cycles), 1 = scale by trend confidence (longer cooldown when trend weakens; shorter when trend resumes).",                                     \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       sharded_force_synthetic,     "Force Synthetic Ticks","Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                       \
        "Debug/test toggle — force sharded engine to use synthetic tick generator instead of real Binance WS feed. Used for offline reproducibility tests. Boot-only.",                                              \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       auto_stamp_on_held_out,      "Auto-Stamp on Held-Out","ML",             CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(1),                                                                          \
        "v5.8.10 — suite Run Full Validation auto-signs each generated stamp on held-out pass. Default 1 (auto-stamp); set 0 only for manual tools/stamp_model.sh workflow.",                                        \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === C2a: Lifecycle / persistence / cooldown ints (parser-only; v5.15.5.F.4c) === */                                                                                                                            \
    X(KIND_INT,        slow_path_max_secs,          "Slow-Path Max Secs",   "Engine Timing",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 1, 3600),                                                                  \
        "Maximum wall-clock seconds per slow-path cycle. If exceeded, slow path emits warning + caps cycle. Default 60s; clamp [1, 3600].",                                                                          \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        sl_cooldown_base,            "SL Cooldown Base",     "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 1000000),                                                                \
        "Base cycles to cooldown after stop loss (adaptive mode adds extra per loss; non-adaptive uses sl_cooldown_cycles alone).",                                                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        sl_cooldown_extra,           "SL Cooldown Extra",    "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 1000000),                                                                \
        "Extra cooldown cycles per consecutive stop loss (adaptive mode only).",                                                                                                                                    \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        recovery_delay_secs,         "Recovery Delay",       "Risk Management", CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 0, 86400),                                                                 \
        "Wait this many seconds after flatten event before resuming trades. Prevents tilted re-entry on dead WS recovery.",                                                                                          \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        param_max_age_ticks,         "Param Max Age Ticks",  "Lifecycle",       CfgFieldDescriptor::WARN_ON_CLAMP, INT(100000, 0, 1000000000),                                                        \
        "Hot-path parameter staleness gate — refuse trades if engine.cfg parameters last touched > N ticks ago. 0 = disabled.",                                                                                     \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        model_max_age_hours,         "Model Max Age Hours",  "Lifecycle",       CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 87600),                                                                  \
        "Refuse model load if file mtime older than N hours. 0 = disabled (legacy default; v5.14.8.E).",                                                                                                             \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        lazy_rebuild_force_period_us,"Lazy Rebuild Force Period (us)","Performance",CfgFieldDescriptor::WARN_ON_CLAMP, INT(1000000, 0, 600000000),                                                    \
        "Force slow-path rebuild every N microseconds even if no parameter inputs changed. Defensive against stale state. Default 1s (1M us).",                                                                     \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        ws_dead_time_flatten_threshold_secs,"WS Dead-Time Flatten Threshold","Risk Management",CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 1, 3600),                                                   \
        "OMS_FlattenAll triggers if WS dead for >N seconds (paired with ws_dead_time_flatten_enabled bitmap flag).",                                                                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === C2b: Engine timing / persistence / risk ints — HIGH-6 tooltip byte-identity preserved (v5.15.5.F.4c) === */                                                                                                \
    X(KIND_INT,        poll_interval,               "Poll Interval",        "Engine Timing",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(100, 1, 1000000),                                                              \
        "Ticks between slow-path runs (regression, adaptation, sample collection)\ndefault 100. ML training note: with poll_interval << forward_ticks,\nconsecutive samples have heavily-overlapping forward windows → label\nautocorrelation. For independent samples set poll_interval = forward_ticks.",                                                                                                                                                                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        warmup_ticks,                "Warmup Ticks",         "Engine Timing",   CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 100000000),                          \
        "Minimum raw ticks before trading starts. Counts every tick.\nUse this when you want a longer total-tick warmup. No upper bound.",                                                                          \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        min_warmup_samples,          "Min Rolling Samples",  "Engine Timing",   CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(64, 0, 128),                                \
        "Min rolling-stats samples before trading. CAPS at 128 (rolling window\nsize). Values >128 are clamped at config load with a warning. Use\nwarmup_ticks for longer raw-tick warmup.",                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        max_hold_ticks,              "Max Hold",             "Time-Based Exit", CfgFieldDescriptor::PER_CORE_OK | CfgFieldDescriptor::WARN_ON_CLAMP, INT(75000, 0, 100000000),                       \
        "Close position after this many ticks (engine-wide).\n0 = disabled, 75000 ≈ 4-5 hours.\nPer-core min-gain floor lives in each core's Time Exit override.",                                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        max_positions,               "Max Pos",              "Risk Management", CfgFieldDescriptor::WARN_ON_CLAMP, INT(1, 1, 16),                                                                    \
        nullptr,                                                                                                                                                                                                    \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        kill_recovery_warmup,        "Recovery",             "Kill Switch",     CfgFieldDescriptor::WARN_ON_CLAMP, INT(100, 0, 1000000),                                                              \
        "Slow-path cycles to observe after kill reset\nbefore trading resumes (prevents immediate re-entry)",                                                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        regime_hysteresis,           "Hysteresis",           "Regime Detection",CfgFieldDescriptor::WARN_ON_CLAMP, INT(3, 1, 100),                                                                    \
        "Slow-path cycles before regime switch\nprevents rapid flipping between strategies",                                                                                                                        \
        STRAT_CAT_REGIME_AWARE,                              OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        idle_reset_cycles,           "Idle Reset",           "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(100, 0, 1000000),                                                              \
        "Cycles with no fill before gate decay\nprevents permanent lockout after losses",                                                                                                                           \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        sl_cooldown_cycles,          "SL Cooldown",          "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(30, 0, 1000000),                                                               \
        "Slow-path cycles to pause after stop loss\nlets market settle before re-entry",                                                                                                                            \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === C3: ML / training int migrations — mostly BOOT_ONLY (training params) (v5.15.5.F.4c) === */                                                                                                                \
    X(KIND_INT,        xgb_min_child_weight,        "Min Child Weight",     "ML Hyperparams",  CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(5, 1, 50),                                  \
        "Min sum-of-weights per leaf (1-50). Higher = more regularization.\nDefault 5. Match deployed model's training value or expect WARN.",                                                                      \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        xgb_seed,                    "Seed",                 "ML Hyperparams",  CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(42, 0, 2147483647),                         \
        "RNG seed for reproducible runs. Default 42. Match deployed model's\ntraining seed or expect WARN.",                                                                                                        \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        xgb_train_nthread,           "Train Threads",        "Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(4, 1, 256),                                 \
        "XGBoost training thread count (OpenMP). Default 4; clamp [1, 256].",                                                                                                                                       \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        xgb_eval_nthread,            "Eval Threads",         "Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(4, 1, 256),                                 \
        "XGBoost evaluation thread count (OpenMP). Default 4; clamp [1, 256].",                                                                                                                                     \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        csv_load_workers,            "CSV Load Workers",     "Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(4, 1, 256),                                 \
        "Worker thread count for parallel CSV tick load during training. Default 4.",                                                                                                                               \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        multi_horizon_max_threads,   "Multi-Horizon Threads","Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(4, 1, 256),                                 \
        "Max parallel threads for multi-horizon training. Default 4.",                                                                                                                                              \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        feature_collect_max_gb,      "Feature Collect Max GB","Training",       CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(8, 1, 1024),                                \
        "Max GB of RAM for feature collection during training. OOM-kill protection. Default 8.",                                                                                                                    \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        wf_split_max_gb,             "Walk-Fwd Split Max GB","Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(8, 1, 1024),                                \
        "Max GB of RAM for walk-forward split during training. Default 8.",                                                                                                                                         \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        held_out_max_gb,             "Held-Out Max GB",      "Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(8, 1, 1024),                                \
        "Max GB of RAM for held-out validation set load. Default 8.",                                                                                                                                               \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        confidence_window,           "Conf Window",          "FoxML",           CfgFieldDescriptor::WARN_ON_CLAMP, INT(64, 1, 64),                                                                    \
        "RollingIC + RollingRMSE window size (engine-wide; cap 64).\nSame window per ML core today; INT support for X-macro deferred.",                                                                              \
        STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        confidence_turnover_window,  "Conf Turnover Window", "FoxML",           CfgFieldDescriptor::WARN_ON_CLAMP, INT(1000, 1, 100000),                                                              \
        "Turnover sample window for ML confidence (predictions over recent N ticks).",                                                                                                                              \
        STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        confidence_turnover_topk,    "Conf Turnover TopK",   "FoxML",           CfgFieldDescriptor::WARN_ON_CLAMP, INT(10, 1, 1000),                                                                  \
        "Top-K predictions kept for confidence turnover analysis.",                                                                                                                                                 \
        STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        confidence_ic_floor_window,  "Conf IC Floor Window", "FoxML",           CfgFieldDescriptor::WARN_ON_CLAMP, INT(1000, 1, 100000),                                                              \
        "Rolling window for ML IC floor enforcement.",                                                                                                                                                              \
        STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        confidence_ic_variant,       "Conf IC Variant",      "FoxML",           CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 4),                                                                      \
        "IC variant: 0=Spearman (default), 1+=future. Ships as KIND_INT pending TECH_DEBT-068 ML enum registry; promote to KIND_INT_ENUM after.",                                                                  \
        STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        ensemble_min_warmup_predictions,"Ens Min Warmup Preds","Ensemble",      CfgFieldDescriptor::WARN_ON_CLAMP, INT(100, 0, 1000000),                                                              \
        "Min predictions before ensemble bandit becomes load-bearing.",                                                                                                                                             \
        STRAT_CAT_ML | STRAT_CAT_USES_BANDIT,                OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        ensemble_bandit_save_interval,"Ens Bandit Save Interval","Ensemble",    CfgFieldDescriptor::WARN_ON_CLAMP, INT(5000, 1, 1000000),                                                              \
        "Predictions between ensemble bandit state save-to-disk events. Default 5000.",                                                                                                                             \
        STRAT_CAT_ML | STRAT_CAT_USES_BANDIT,                OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        csv_sort_check_mode,         "CSV Sort Check Mode",  "Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(1, 0, 2),                                   \
        "CSV tick-sort validation: 0=STRICT (refuse load on unsort), 1=WARN (log + proceed; default), 2=DISABLED. Ships as KIND_INT pending TECH_DEBT-068.",                                                         \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        thompson_rng_seed,           "Thompson RNG Seed",    "FoxML",           CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::HAS_SIDE_EFFECT, INT(42, 0, 9223372036854775807),                \
        "splitmix64 seed for Thompson sampling bandit. Default 42. 0 = use ThompsonBandit.hpp's THOMPSON_RNG_SEED_DEFAULT. Boot-only; required for replay-determinism. HAS_SIDE_EFFECT — manual parser supports hex (0x...) base-auto-detect; registry walker skips.",                                                                                                                                                                                                                                                          \
        STRAT_CAT_ML | STRAT_CAT_USES_BANDIT,                OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === C4 + C5: Notify / health / reconcile / operational + INT_ENUM candidates (v5.15.5.F.4c) === */                                                                                                              \
    X(KIND_INT,        default_strategy,            "Default Strategy",     "Strategies",      CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 8),                                   \
        "Legacy single_core default strategy ID (STRATEGY_*). Per-core strategy override at core_<N>_strategy is the canonical surface. Ships as KIND_INT pending TECH_DEBT-068.",                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        ml_backend,                  "ML Backend",           "ML",              CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 4),                                   \
        "ML inference backend selection. Ships as KIND_INT pending TECH_DEBT-068 ML enum registry; promote to KIND_INT_ENUM with XGBOOST/ONNX/AOT labels after.",                                                    \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        regime_model_backend,        "Regime Model Backend", "ML",              CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 4),                                   \
        "Regime detection model backend. Ships as KIND_INT pending TECH_DEBT-068.",                                                                                                                                 \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        record_max_days,             "Record Max Days",      "Tick Recording",  CfgFieldDescriptor::WARN_ON_CLAMP, INT(30, 1, 365),                                                                   \
        "Auto-delete tick + depth CSVs older than this many days. 30 = ~1-2GB cap on disk usage.",                                                                                                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        notify_backend,              "Notify Backend",       "Notifications",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 4),                                                                      \
        "Notification backend: 0=Discord, 1=Telegram, 2=Slack (per notify_command template). Ships as KIND_INT pending TECH_DEBT-068.",                                                                              \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        notify_cooldown_secs,        "Notify Cooldown Secs", "Notifications",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 0, 86400),                                                                 \
        "Min seconds between notifications (debounce). 0 = no cooldown.",                                                                                                                                           \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        health_log_level,            "Health Log Level",     "Health Logging",  CfgFieldDescriptor::WARN_ON_CLAMP, INT(1, 0, 4),                                                                      \
        "Health log severity: 0=DEBUG, 1=INFO (default), 2=WARN, 3=ERROR. Ships as KIND_INT pending TECH_DEBT-068.",                                                                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        health_log_max_bytes,        "Health Log Max Bytes", "Health Logging",  CfgFieldDescriptor::WARN_ON_CLAMP, INT(1048576, 1024, 1073741824),                                                    \
        "Health log file size limit (rotates at this size). Default 1MB; clamp [1KB, 1GB].",                                                                                                                        \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        health_log_keep_count,       "Health Log Keep Count","Health Logging",  CfgFieldDescriptor::WARN_ON_CLAMP, INT(5, 1, 1000),                                                                   \
        "Number of rotated health log files to keep before deletion.",                                                                                                                                              \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        reconcile_interval_sec,      "Reconcile Interval",   "Reconcile",       CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 1, 86400),                                                                 \
        "Seconds between reconciliation passes (paper-position vs broker-position drift check).",                                                                                                                   \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        slow_path_pin_offset,        "Slow-Path Pin Offset", "Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(-1, -1, 256),                               \
        "Slow-path CPU pin offset. -1 = disabled, 0 = auto, >0 = explicit CPU offset.",                                                                                                                             \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        num_execution_cores,         "Execution Cores",      "Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(1, 1, 16),                                  \
        "Number of per-core execution shards (per-core engine_arch). Clamp [1, 16].",                                                                                                                               \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        held_out_gate_strict,        "Held-Out Gate Strict", "Drift Acknowledgments",CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, -1, 1),                                                                \
        "Held-out validation gate: -1=skip, 0=warn-only (default), 1=refuse load. Tri-state KIND_INT pending categorical applicability INT_ENUM upgrade.",                                                          \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === C5 HAS_SIDE_EFFECT registry rows — documented in registry, walker skips parse, manual block preserved (v5.15.5.F.4c) === */                                                                                \
    X(KIND_INT,        reconcile_mode,              "Reconcile Mode",       "Reconcile",       CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 2),                                \
        "Reconcile mode: 0=STRICT, 1=WARN (default), 2=AUTO_SYNC. Accepts string ('strict'/'warn'/'auto_sync') or int. HAS_SIDE_EFFECT — manual parser handles string + sets cfg_keys_explicit + mirrors reconcile_dry_run.",                                                                                                                                                                                                                                                                                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        engine_mode,                 "Engine Mode",          "Operational",     CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::IS_BOOT_ONLY, INT(1, 0, 2),                                 \
        "Engine mode: 0=SINGLE_CORE (legacy), 1=SHARDED (default v5.0+). Accepts string ('sharded'/'single_core') or int. HAS_SIDE_EFFECT — manual parser handles string form; registry walker skips.",            \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        engine_arch,                 "Engine Arch",          "Operational",     CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::IS_BOOT_ONLY, INT(1, 0, 1),                                 \
        "Slow-path threading model: 0=CENTRALIZED, 1=PER_CORE_SLOW (default). Accepts string ('per_core_slow'/'centralized') or int. HAS_SIDE_EFFECT — manual parser handles string form.",                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        model_verify_strict,         "Model Verify Strict",  "Drift Acknowledgments",CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::WARN_ON_CLAMP, INT(-1, -1, 1),                         \
        "Model verification strictness: -1=auto (strict in live, lenient in paper; default), 0=lenient, 1=strict. Tri-state. HAS_SIDE_EFFECT — manual parser sets cfg_keys_explicit bit for NormalizeForMode flip rule.",                                                                                                                                                                                                                                                                                                       \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG)

//======================================================================================================
// [FIELD_IDX — auto-generated index enum]
//======================================================================================================
// Drives g_cfg_field_descriptors[FIELD_IDX_<name>] direct access at compile time.
#define X_GEN_FIELD_IDX(KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_to_strategy_cat, applies_to_op_mode_cat, applies_to_regime_cat, applies_to_risk_cat, lives_in_struct) \
    FIELD_IDX_##name,

enum CfgFieldIdx : uint16_t {
    FOREACH_CFG_FIELD(X_GEN_FIELD_IDX)
    FIELD_IDX_END  // sentinel; equals registry entry count
};
#undef X_GEN_FIELD_IDX

//======================================================================================================
// [g_cfg_field_descriptors — auto-generated array]
//======================================================================================================
// Single source of truth for descriptor data; consumers index via FIELD_IDX_<name>.
#define X_GEN_DESCRIPTOR(KIND_TOKEN, name, label, section, meta, payload_init, tooltip, applies_to_strategy_cat, applies_to_op_mode_cat, applies_to_regime_cat, applies_to_risk_cat, lives_in_struct) \
    CfgFieldDescriptor{                                                                                  \
        /* kind            */ CfgFieldDescriptor::KIND_TOKEN,                                            \
        /* lives_in_struct */ static_cast<uint8_t>(lives_in_struct),                                     \
        /* metadata_flags  */ static_cast<uint16_t>(meta),                                               \
        /* _reserved       */ 0,                                                                         \
        /* field_idx       */ FIELD_IDX_##name,                                                          \
        /* cfg_field_name  */ #name,                                                                     \
        /* label           */ label,                                                                     \
        /* section         */ section,                                                                   \
        /* tooltip         */ tooltip,                                                                   \
        /* applies_strat   */ static_cast<uint32_t>(applies_to_strategy_cat),                            \
        /* applies_op_mode */ static_cast<uint16_t>(applies_to_op_mode_cat),                             \
        /* applies_regime  */ static_cast<uint16_t>(applies_to_regime_cat),                              \
        /* applies_risk    */ static_cast<uint16_t>(applies_to_risk_cat),                               \
        /* requires_cfg    */ nullptr,                                                                   \
        /* payload         */ payload_init,                                                              \
    },

// v5.15.5.F.4c — constexpr enables compile-time mask computation in the bitmap
// dispatcher framework below. All descriptor members are trivially constexpr-init
// (string literals + integer constants + union aggregate init).
inline constexpr CfgFieldDescriptor g_cfg_field_descriptors[] = {
    FOREACH_CFG_FIELD(X_GEN_DESCRIPTOR)
};
#undef X_GEN_DESCRIPTOR

// Verify array size matches sentinel.
static_assert(sizeof(g_cfg_field_descriptors) / sizeof(g_cfg_field_descriptors[0]) == FIELD_IDX_END,
              "g_cfg_field_descriptors size must equal FIELD_IDX_END");

// Note: orthogonality with FOREACH_CFG_DERIVED_INFERENCE_CFG (v5.15.5.A.7,
// MemHeaders/CfgDerivedInferenceCfgRegistry.hpp). That registry reads SAME cfg
// fields but writes to inf.inference_cfg_* for stamp emit (different consumer).
// FOREACH_CFG_FIELD here is the cfg I/O surface; the two are orthogonal by
// design. Verified at .F.4d via reverse-drift CI script.

//======================================================================================================
// [BITMAP DISPATCHER FRAMEWORK — v5.15.5.F.4c]
//======================================================================================================
// Precomputed per-metadata-bit bitmap masks over the cfg field set + iteration
// helpers + composed-filter primitives. Enables:
//   - O(1) per-bit-category popcount stats: `cfg_field_count(g_cfg_deprecated_mask)`
//     returns "how many cfg fields are flagged DEPRECATED" without iterating.
//   - Composed filter views via bitwise ops: `render_mask = ~(boot_only | hidden_by_default)`.
//   - Branchless next-set-bit iteration via __builtin_ctzll for walker consumers.
//   - Foundation for .F.4d FOREACH_DERIVED_FILTER meta-registry (this layer is the
//     storage primitive; .F.4d adds the declarative roster + sidecar override pattern).
//
// Per H6: these arrays are populated ONCE at static-init time (single-threaded;
// before any thread spawns) then read-only forever. No cross-thread WRITE → no
// alignas(64) discipline needed. Sizes are modest (~24 bytes per array; ~288 bytes
// total) so cache pressure is negligible at 60Hz GUI consumer rate.
//
// Per H7: bitmap iteration is branchless within the inner loop (__builtin_ctzll +
// `word &= word - 1` for next-bit-clear). Outer `while (word)` is loop control,
// not data-dependent dispatch.
//
// Per CLAUDE.md framework discipline (item 31): adding a new metadata bit = 1 row
// in FOREACH_METADATA_BIT below; mask array + init code auto-generate via X-macro.
//======================================================================================================

// Mask array sizing — 1 uint64_t word per 64 fields, rounded up.
static constexpr size_t CFG_MASK_WORDS = (FIELD_IDX_END + 63) / 64;

// FOREACH_METADATA_BIT(X) — tuple: X(lowercase_name, UPPERCASE_BIT_NAME).
// Each row adds a per-bit precomputed mask array + init logic. Order matches the
// MetadataFlag enum at lines 61-73 of this file.
#define FOREACH_METADATA_BIT(X)                                            \
    X(per_core_ok,          PER_CORE_OK)                                   \
    X(restart_required,     RESTART_REQUIRED)                              \
    X(safety_critical,      SAFETY_CRITICAL)                               \
    X(deprecated,           DEPRECATED)                                    \
    X(stamp_bound,          STAMP_BOUND)                                   \
    X(hidden_by_default,    HIDDEN_BY_DEFAULT)                             \
    X(is_secret,            IS_SECRET)                                     \
    X(is_boot_only,         IS_BOOT_ONLY)                                  \
    X(affects_stamp_parity, AFFECTS_STAMP_PARITY)                          \
    X(log_value_forbidden,  LOG_VALUE_FORBIDDEN)                           \
    X(has_side_effect,      HAS_SIDE_EFFECT)                               \
    X(warn_on_clamp,        WARN_ON_CLAMP)

// Wrapper struct holding a fixed-size mask array. Used because raw C-array
// return types are awkward in C++ constexpr functions; the wrapper makes the
// type explicit + operator[] gives natural array-like access at consumer sites.
struct CfgMaskArray {
    uint64_t words[CFG_MASK_WORDS];

    constexpr uint64_t operator[](size_t i) const { return words[i]; }
    constexpr uint64_t& operator[](size_t i)      { return words[i]; }
};

// Compile-time mask computation — single template-parameterized constexpr function.
// Walks g_cfg_field_descriptors[] at compile time; produces the per-bit mask
// directly into .rodata. Zero runtime init cost. Compiler may fold the resulting
// constants into immediates at consumer sites.
template <uint16_t Bit>
inline constexpr CfgMaskArray cfg_compute_mask() {
    CfgMaskArray result = {};
    for (size_t i = 0; i < FIELD_IDX_END; i++) {
        if (g_cfg_field_descriptors[i].metadata_flags & Bit) {
            result.words[i / 64] |= (1ULL << (i % 64));
        }
    }
    return result;
}

// Per-bit precomputed mask arrays — X-macro-generated constexpr declarations.
// Each is compile-time computed from the descriptor array; lands in .rodata.
#define X_GEN_MASK_CONSTEXPR(lname, BITNAME) \
    inline constexpr CfgMaskArray g_cfg_##lname##_mask = cfg_compute_mask<CfgFieldDescriptor::BITNAME>();
FOREACH_METADATA_BIT(X_GEN_MASK_CONSTEXPR)
#undef X_GEN_MASK_CONSTEXPR

//------------------------------------------------------------------------------
// [PER-LivesInStruct-VALUE BITMAP MASKS — v5.15.5.F.4c]
//------------------------------------------------------------------------------
// Forward-compat for `.F.4j` BACKTEST cohort + future training/secrets cohorts.
// Pattern is the metadata-bit-mask analogue but for an enum VALUE (equality
// check) rather than a bit (bitwise AND). Each row contributes to exactly ONE
// mask (its lives_in_struct value).
//
// Consumer: GUI subsystems each render their own struct's fields via a per-
// struct walker (see DESIGN_SPECS/universal-registry-bitmap-dispatcher-pattern.md
// § "ML-side (separate registries; same pattern)" — same pattern extends to
// any registry needing per-enum-value filtering).
//------------------------------------------------------------------------------

// FOREACH_LIVES_IN_STRUCT(X) — tuple: X(lowercase_name, UPPERCASE_VALUE_NAME).
// Mirrors the LivesInStruct enum at lines 77-83 of this file.
#define FOREACH_LIVES_IN_STRUCT(X)                                          \
    X(struct_cfg,            STRUCT_CFG)                                    \
    X(struct_backtest_cfg,   STRUCT_BACKTEST_CFG)                           \
    X(struct_controller_cfg, STRUCT_CONTROLLER_CFG)                         \
    X(struct_secrets_cfg,    STRUCT_SECRETS_CFG)                            \
    X(struct_training_cfg,   STRUCT_TRAINING_CFG)

// Compile-time per-LivesInStruct-value mask computation.
// NOTE: dispatch via EQUALITY (lives_in_struct == Value) not bitwise AND;
// each row contributes to exactly one mask.
template <uint8_t Value>
inline constexpr CfgMaskArray cfg_compute_lives_in_struct_mask() {
    CfgMaskArray result = {};
    for (size_t i = 0; i < FIELD_IDX_END; i++) {
        if (g_cfg_field_descriptors[i].lives_in_struct == Value) {
            result.words[i / 64] |= (1ULL << (i % 64));
        }
    }
    return result;
}

// Per-value mask declarations — X-macro-generated.
#define X_GEN_LIVES_IN_STRUCT_MASK(lname, VALUE) \
    inline constexpr CfgMaskArray g_cfg_##lname##_mask = \
        cfg_compute_lives_in_struct_mask<CfgFieldDescriptor::VALUE>();
FOREACH_LIVES_IN_STRUCT(X_GEN_LIVES_IN_STRUCT_MASK)
#undef X_GEN_LIVES_IN_STRUCT_MASK

// Bitmap iteration macro — invokes `body` per set bit in `mask`.
// `idx_var` is bound to FIELD_IDX_* value of each set bit. Branchless inner loop
// via __builtin_ctzll (single TZCNT on Haswell+) + `word &= word - 1` next-bit-clear.
//
// Accepts either CfgMaskArray (via operator[]) or raw uint64_t[CFG_MASK_WORDS].
//
// Usage:
//   CFG_FIELD_FOR_EACH_SET_BIT(g_cfg_deprecated_mask, idx, {
//       fprintf(stderr, "deprecated: %s\n", g_cfg_field_descriptors[idx].cfg_field_name);
//   });
#define CFG_FIELD_FOR_EACH_SET_BIT(mask, idx_var, body)                    \
    for (size_t _w = 0; _w < CFG_MASK_WORDS; _w++) {                       \
        uint64_t _word = (mask)[_w];                                       \
        while (_word) {                                                    \
            const size_t _bit = static_cast<size_t>(__builtin_ctzll(_word));\
            const size_t idx_var = _w * 64 + _bit;                         \
            do { body; } while (0);                                        \
            _word &= _word - 1;                                            \
        }                                                                  \
    }

// Popcount over a mask array — operator-clarity stat helper. constexpr so call
// sites with literal mask arrays fold to a single immediate at compile time.
// Example: `cfg_field_count(g_cfg_deprecated_mask)` → "how many cfg fields are
// flagged DEPRECATED in this build" (one-shot; no iteration needed at runtime).
inline constexpr size_t cfg_field_count(const CfgMaskArray& mask) {
    size_t n = 0;
    for (size_t i = 0; i < CFG_MASK_WORDS; i++) {
        n += static_cast<size_t>(__builtin_popcountll(mask.words[i]));
    }
    return n;
}

// Composed-filter helper — returns `~(boot_only | hidden_by_default)`, the canonical
// "show in GUI" filter. Used by the SettingsPanel render walker. constexpr so the
// composed mask is itself a compile-time constant. Other consumers compose their
// own masks via bitwise ops (e.g., CLI list-changed: changed_from_default_mask &
// ~hidden_by_default_mask).
inline constexpr CfgMaskArray cfg_compose_render_mask() {
    CfgMaskArray out = {};
    for (size_t i = 0; i < CFG_MASK_WORDS; i++) {
        out.words[i] = ~(g_cfg_is_boot_only_mask.words[i] | g_cfg_hidden_by_default_mask.words[i]);
    }
    // Mask off bits beyond FIELD_IDX_END in the last word (~0 sets all bits;
    // any set bit beyond FIELD_IDX_END would cause an out-of-bounds descriptor read
    // in iteration consumers).
    if constexpr ((FIELD_IDX_END % 64) != 0) {
        constexpr uint64_t last_word_valid = (1ULL << (FIELD_IDX_END % 64)) - 1ULL;
        out.words[CFG_MASK_WORDS - 1] &= last_word_valid;
    }
    return out;
}

// Pre-computed composed render mask — compile-time constant in .rodata.
inline constexpr CfgMaskArray g_cfg_render_mask = cfg_compose_render_mask();

// Additional composed views for future consumers — each lands in .rodata at compile time.

// Save path filter — all rows EXCEPT HAS_SIDE_EFFECT (manual save logic owns those).
inline constexpr CfgMaskArray cfg_compose_save_mask() {
    CfgMaskArray out = {};
    for (size_t i = 0; i < CFG_MASK_WORDS; i++) {
        out.words[i] = ~g_cfg_has_side_effect_mask.words[i];
    }
    if constexpr ((FIELD_IDX_END % 64) != 0) {
        constexpr uint64_t last_word_valid = (1ULL << (FIELD_IDX_END % 64)) - 1ULL;
        out.words[CFG_MASK_WORDS - 1] &= last_word_valid;
    }
    return out;
}
inline constexpr CfgMaskArray g_cfg_save_mask = cfg_compose_save_mask();

// Stamp emit filter — STAMP_BOUND rows only. Consumed by .F.4d stamp emit migration
// (replaces FOREACH_STAMP_BOUND_CFG manual walker; per derived filter framework).
inline constexpr CfgMaskArray g_cfg_stamp_emit_mask = g_cfg_stamp_bound_mask;

// CLI explain filter — all rows (including BOOT_ONLY / HIDDEN_BY_DEFAULT — operator wants
// full visibility via `engine --explain-cfg` even if GUI hides them). Future .F.4e consumer
// per TECH_DEBT-066.
inline constexpr CfgMaskArray cfg_compose_cli_explain_mask() {
    CfgMaskArray out = {};
    // All valid bits set; mask off bits beyond FIELD_IDX_END.
    for (size_t i = 0; i < CFG_MASK_WORDS - 1; i++) {
        out.words[i] = ~0ULL;
    }
    if constexpr ((FIELD_IDX_END % 64) == 0) {
        out.words[CFG_MASK_WORDS - 1] = ~0ULL;
    } else {
        out.words[CFG_MASK_WORDS - 1] = (1ULL << (FIELD_IDX_END % 64)) - 1ULL;
    }
    return out;
}
inline constexpr CfgMaskArray g_cfg_cli_explain_mask = cfg_compose_cli_explain_mask();

// Per-core override emit filter — PER_CORE_OK rows only. Consumed by .F.4g per-core
// override path (currently in PerCoreOverrides parallel structure).
inline constexpr CfgMaskArray g_cfg_per_core_override_mask = g_cfg_per_core_ok_mask;
