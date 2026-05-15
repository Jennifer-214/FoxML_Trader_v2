// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
//======================================================================================================
// [UNIVERSAL CFG FIELD REGISTRY — TWO-REGISTRY ARCHITECTURE]
//======================================================================================================
// v5.15.5.F.4c.3 — global vs per-core registry split. Single FOREACH_CFG_FIELD
// retired in favor of two registries with DISJOINT scope:
//
//   FOREACH_GLOBAL_CFG_FIELD(X)   — system / training / recording / engine-wide
//                                   mode / acknowledgments / notifications /
//                                   logging / reconcile (~47 rows). Consumed
//                                   against `ControllerConfig<F>` flat fields.
//
//   FOREACH_PER_CORE_CFG_FIELD(X) — trading / strategy / entry / exit / ML /
//                                   risk-gate / regime-detection / per-core
//                                   kill switches (~79 rows). Consumed against
//                                   `cfg.cores[c].<field>` per-core struct
//                                   instance (`PerCoreCfg<F>`).
//
// PER_CORE_OK metadata bit REMOVED — every per-core row IS per-core by
// construction (registry membership IS the scope assertion). Anti-pattern
// closed at this ship per `DESIGN_SPECS/cfg-scope-discipline.md` § "Forbidden
// patterns": no "global default + per-core override" mechanism.
//
// Architecture: 12-col Option D X-macro tuple per
//   DESIGN_SPECS/registry-tuple-as-single-source-of-truth.md.
// Per-instance dimension: this is the first canonical application of
//   DESIGN_SPECS/per-instance-registry-pattern.md (per-core; future axes:
//   per-symbol, per-strategy, per-horizon, per-regime — each becomes a sister
//   FOREACH_<AXIS>_CFG_FIELD using the same template).
// Type-trait dispatch via tt:: namespace per CLAUDE.md item 23 +
//   DESIGN_SPECS/type-trait-dispatch-via-tt-namespace.md.
// Categorical applicability columns per
//   DESIGN_SPECS/categorical-tag-applicability-pattern.md.
// 3-barrier structural fix for Class 23 (DOCS/RECURRING_BUG_PATTERNS.md) —
// Kind enum is METADATA-ONLY; type dispatch happens via tt::cfg_*_field<T>
// with T deduced from the cfg field reference (NEVER via reinterpret_cast).
//
// DESCRIPTOR SCHEMA LOCKED AT .F.4b — subsequent sub-ships ADD ROWS / SPLIT
// REGISTRIES only; no schema changes to CfgFieldDescriptor struct layout.
// (Per "design upfront + ship in waves" decision 2026-05-14.)
//
// .F.4c.3 changes:
//   • Replace single FOREACH_CFG_FIELD with FOREACH_GLOBAL_CFG_FIELD +
//     FOREACH_PER_CORE_CFG_FIELD per locked scope classification
//     (plans/plan_checks/cfg-field-scope-classification-2026-05-15.md).
//   • Drop PER_CORE_OK from MetadataFlag enum (now redundant; bit 0 RESERVED).
//   • Drop 3 rows entirely: `default_strategy` (legacy single_core; per-core
//     IS canonical surface), `pay_fees_in_bnb` (operator-side cfg; doesn't
//     affect engine behavior), `reconcile_dry_run` (DEPRECATED at v5.14.4;
//     engine reads reconcile_mode).
//   • Template-parameterize bitmap dispatcher framework (CfgMaskArray<N> +
//     cfg_compute_mask + cfg_field_count + composed-mask helpers) to operate
//     on arbitrary descriptor arrays per per-registry application.
//   • Per-registry mask arrays (g_global_cfg_*_mask + g_per_core_cfg_*_mask).
//   • Per-registry composed masks (g_global_cfg_render_mask +
//     g_per_core_cfg_render_mask + save/stamp_emit/cli_explain analogues).
//   • cfg_field_names_unique templated to operate per-registry (each registry
//     internally unique; cross-registry name collision is ALLOWED only with
//     explicit per-axis rationale per cfg-scope-discipline § "Anti-patterns").
//
// .F.4d will add: STAMP_BOUND derived filter framework + Layer 5b per-core
// hash + sidecar override pattern + meta-registry consumer.
// .F.4e will add: KIND_STRING + KIND_FILE_PATH + cfg.example auto-gen + 5
// GUI metadata derived filters.
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

    //---- MetadataFlag enum (uint16_t) — 11 bits used; 5 bits headroom ----
    // .F.4c.3: PER_CORE_OK (1u << 0) REMOVED. Every per-core row IS per-core by
    // construction (registry membership IS the scope assertion). Bit 0 RESERVED
    // for future use; do not reassign without documenting the schema change.
    enum MetadataFlag : uint16_t {
        // 1u << 0 RESERVED (was PER_CORE_OK, removed at v5.15.5.F.4c.3)
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
        // ... 5 bits headroom for future ...
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
// [FOREACH_*_CFG_FIELD — 12-col Option D tuple]
//======================================================================================================
// Tuple (12 args):
//   X(KIND_TOKEN, name, label, section, meta, payload, tooltip,
//     applies_to_strategy_cat, applies_to_op_mode_cat,
//     applies_to_regime_cat, applies_to_risk_cat, lives_in_struct)
//
// payload macro: DBL(default, min, max) for KIND_DOUBLE / KIND_DOUBLE_PCT
//                INT(default, min, max) for KIND_INT
//                BOOL(default) for KIND_BOOL
//                INT_ENUM(default, labels_array, count) for KIND_INT_ENUM
//
// .F.4c.3: scope classification per
//   plans/plan_checks/cfg-field-scope-classification-2026-05-15.md
// (LOCKED 2026-05-15). Total: 47 GLOBAL + 79 PER_CORE + 3 REMOVED = 129.
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

//======================================================================================================
// [FOREACH_GLOBAL_CFG_FIELD — system / training / recording / engine-wide mode / ack / notify / logging]
//======================================================================================================
// 47 rows. Operator sets once for the whole engine; not per-core.
//======================================================================================================
#define FOREACH_GLOBAL_CFG_FIELD(X)                                                                                                                                                                                  \
    /* === System / Operational (5) === */                                                                                                                                                                            \
    X(KIND_INT,        num_execution_cores,         "Execution Cores",      "Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(1, 1, 16),                                  \
        "Number of per-core execution shards (per-core engine_arch). Clamp [1, 16].",                                                                                                                               \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       require_mlockall,            "Require mlockall",     "Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                       \
        "Pin engine memory at boot via mlockall(2) — prevents swap-out under memory pressure. Requires CAP_IPC_LOCK or root. Boot-only; runtime changes ignored.",                                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       init_arena_use_hugepages,    "Use Hugepages",        "Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                       \
        "Initialize per-core arenas with 2MB hugepages (MAP_HUGETLB). Reduces TLB pressure on hot path. Requires /sys/kernel/mm/hugepages configured. Boot-only.",                                                   \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       sharded_force_synthetic,     "Force Synthetic Ticks","Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                       \
        "Debug/test toggle — force sharded engine to use synthetic tick generator instead of real Binance WS feed. Used for offline reproducibility tests. Boot-only.",                                              \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        slow_path_pin_offset,        "Slow-Path Pin Offset", "Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(-1, -1, 256),                               \
        "Slow-path CPU pin offset. -1 = disabled, 0 = auto, >0 = explicit CPU offset.",                                                                                                                             \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Engine timing / Slow-path discipline (5) === */                                                                                                                                                            \
    X(KIND_INT,        poll_interval,               "Poll Interval",        "Engine Timing",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(100, 1, 1000000),                                                              \
        "Ticks between slow-path runs (regression, adaptation, sample collection)\ndefault 100. ML training note: with poll_interval << forward_ticks,\nconsecutive samples have heavily-overlapping forward windows → label\nautocorrelation. For independent samples set poll_interval = forward_ticks.",                                                                                                                                                                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        slow_path_max_secs,          "Slow-Path Max Secs",   "Engine Timing",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 1, 3600),                                                                  \
        "Maximum wall-clock seconds per slow-path cycle. If exceeded, slow path emits warning + caps cycle. Default 60s; clamp [1, 3600].",                                                                          \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        warmup_ticks,                "Warmup Ticks",         "Engine Timing",   CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 100000000),                          \
        "Minimum raw ticks before trading starts. Counts every tick.\nUse this when you want a longer total-tick warmup. No upper bound.",                                                                          \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        min_warmup_samples,          "Min Rolling Samples",  "Engine Timing",   CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(64, 0, 128),                                \
        "Min rolling-stats samples before trading. CAPS at 128 (rolling window\nsize). Values >128 are clamped at config load with a warning. Use\nwarmup_ticks for longer raw-tick warmup.",                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        lazy_rebuild_force_period_us,"Lazy Rebuild Force Period (us)","Performance",CfgFieldDescriptor::WARN_ON_CLAMP, INT(1000000, 0, 600000000),                                                    \
        "Force slow-path rebuild every N microseconds even if no parameter inputs changed. Defensive against stale state. Default 1s (1M us).",                                                                     \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Hot-path discipline (1) === */                                                                                                                                                                             \
    X(KIND_INT,        param_max_age_ticks,         "Param Max Age Ticks",  "Lifecycle",       CfgFieldDescriptor::WARN_ON_CLAMP, INT(100000, 0, 1000000000),                                                        \
        "Hot-path parameter staleness gate — refuse trades if engine.cfg parameters last touched > N ticks ago. 0 = disabled.",                                                                                     \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Risk / Position limits — engine-wide (3) === */                                                                                                                                                            \
    X(KIND_INT,        max_positions,               "Max Pos",              "Risk Management", CfgFieldDescriptor::WARN_ON_CLAMP, INT(1, 1, 16),                                                                    \
        nullptr,                                                                                                                                                                                                    \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        recovery_delay_secs,         "Recovery Delay",       "Risk Management", CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 0, 86400),                                                                 \
        "Wait this many seconds after flatten event before resuming trades. Prevents tilted re-entry on dead WS recovery.",                                                                                          \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        ws_dead_time_flatten_threshold_secs,"WS Dead-Time Flatten Threshold","Risk Management",CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 1, 3600),                                                   \
        "OMS_FlattenAll triggers if WS dead for >N seconds (paired with ws_dead_time_flatten_enabled bitmap flag).",                                                                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Recording (3) === */                                                                                                                                                                                       \
    X(KIND_BOOL,       record_ticks,                "Record Ticks",         "Tick Recording",  CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "Record raw ticks to CSV for backtesting/ML training\nOutput: data/{SYMBOL}/YYYY-MM-DD.csv\n~30-70MB/day for BTCUSDT",                                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       record_depth,                "Record Depth",         "Tick Recording",  CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "Record @depth5@100ms snapshots to CSV (top-of-book + lastUpdateId)\nOutput: data/{SYMBOL}/depth/YYYY-MM-DD.csv\nRequires depth_enabled=1. Daily rotation, auto-pruned by record_max_days.\nGap markers (# GAP) on backward last_update_id, wallclock >2s, or disconnect.\n~50 MB/day for BTCUSDT. Required for future backtest replay of book state.",                                                                                                                                                                                                                                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        record_max_days,             "Record Max Days",      "Tick Recording",  CfgFieldDescriptor::WARN_ON_CLAMP, INT(30, 1, 365),                                                                   \
        "Auto-delete tick + depth CSVs older than this many days. 30 = ~1-2GB cap on disk usage.",                                                                                                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Training (9) === */                                                                                                                                                                                        \
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
    /* === Training discipline / Held-out (3) === */                                                                                                                                                                  \
    X(KIND_INT,        csv_sort_check_mode,         "CSV Sort Check Mode",  "Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(1, 0, 2),                                   \
        "CSV tick-sort validation: 0=STRICT (refuse load on unsort), 1=WARN (log + proceed; default), 2=DISABLED. Ships as KIND_INT pending TECH_DEBT-068.",                                                         \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       auto_stamp_on_held_out,      "Auto-Stamp on Held-Out","ML",             CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(1),                                                                          \
        "v5.8.10 — suite Run Full Validation auto-signs each generated stamp on held-out pass. Default 1 (auto-stamp); set 0 only for manual tools/stamp_model.sh workflow.",                                        \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        held_out_gate_strict,        "Held-Out Gate Strict", "Drift Acknowledgments",CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, -1, 1),                                                                \
        "Held-out validation gate: -1=skip, 0=warn-only (default), 1=refuse load. Tri-state KIND_INT pending categorical applicability INT_ENUM upgrade.",                                                          \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === ML inference backend — engine-wide (3) === */                                                                                                                                                              \
    X(KIND_INT,        ml_backend,                  "ML Backend",           "ML",              CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 4),                                   \
        "ML inference backend selection. Ships as KIND_INT pending TECH_DEBT-068 ML enum registry; promote to KIND_INT_ENUM with XGBOOST/ONNX/AOT labels after.",                                                    \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        regime_model_backend,        "Regime Model Backend", "ML",              CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 4),                                   \
        "Regime detection model backend. Ships as KIND_INT pending TECH_DEBT-068.",                                                                                                                                 \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       use_aot_inference,           "Use AOT Inference",    "ML",              CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                       \
        "Use ahead-of-time-compiled inference path instead of XGBoost runtime. Faster per-tick (~50ns vs ~500ns) but requires AOT model build via tools/aot_compile. Boot-only.",                                    \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Notifications (2) === */                                                                                                                                                                                   \
    X(KIND_INT,        notify_backend,              "Notify Backend",       "Notifications",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 4),                                                                      \
        "Notification backend: 0=Discord, 1=Telegram, 2=Slack (per notify_command template). Ships as KIND_INT pending TECH_DEBT-068.",                                                                              \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        notify_cooldown_secs,        "Notify Cooldown Secs", "Notifications",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 0, 86400),                                                                 \
        "Min seconds between notifications (debounce). 0 = no cooldown.",                                                                                                                                           \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Health Logging (3) === */                                                                                                                                                                                  \
    X(KIND_INT,        health_log_level,            "Health Log Level",     "Health Logging",  CfgFieldDescriptor::WARN_ON_CLAMP, INT(1, 0, 4),                                                                      \
        "Health log severity: 0=DEBUG, 1=INFO (default), 2=WARN, 3=ERROR. Ships as KIND_INT pending TECH_DEBT-068.",                                                                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        health_log_max_bytes,        "Health Log Max Bytes", "Health Logging",  CfgFieldDescriptor::WARN_ON_CLAMP, INT(1048576, 1024, 1073741824),                                                    \
        "Health log file size limit (rotates at this size). Default 1MB; clamp [1KB, 1GB].",                                                                                                                        \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        health_log_keep_count,       "Health Log Keep Count","Health Logging",  CfgFieldDescriptor::WARN_ON_CLAMP, INT(5, 1, 1000),                                                                   \
        "Number of rotated health log files to keep before deletion.",                                                                                                                                              \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Reconcile (2) === */                                                                                                                                                                                       \
    X(KIND_INT,        reconcile_interval_sec,      "Reconcile Interval",   "Reconcile",       CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 1, 86400),                                                                 \
        "Seconds between reconciliation passes (paper-position vs broker-position drift check).",                                                                                                                   \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        reconcile_mode,              "Reconcile Mode",       "Reconcile",       CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 2),                                \
        "Reconcile mode: 0=STRICT, 1=WARN (default), 2=AUTO_SYNC. Accepts string ('strict'/'warn'/'auto_sync') or int. HAS_SIDE_EFFECT — manual parser handles string + sets cfg_keys_explicit + mirrors reconcile_dry_run.",                                                                                                                                                                                                                                                                                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Engine-wide mode (4) === */                                                                                                                                                                                \
    X(KIND_INT,        engine_mode,                 "Engine Mode",          "Operational",     CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::IS_BOOT_ONLY, INT(1, 0, 2),                                 \
        "Engine mode: 0=SINGLE_CORE (legacy), 1=SHARDED (default v5.0+). Accepts string ('sharded'/'single_core') or int. HAS_SIDE_EFFECT — manual parser handles string form; registry walker skips.",            \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        engine_arch,                 "Engine Arch",          "Operational",     CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::IS_BOOT_ONLY, INT(1, 0, 1),                                 \
        "Slow-path threading model: 0=CENTRALIZED, 1=PER_CORE_SLOW (default). Accepts string ('per_core_slow'/'centralized') or int. HAS_SIDE_EFFECT — manual parser handles string form.",                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        model_verify_strict,         "Model Verify Strict",  "Drift Acknowledgments",CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::WARN_ON_CLAMP, INT(-1, -1, 1),                         \
        "Model verification strictness: -1=auto (strict in live, lenient in paper; default), 0=lenient, 1=strict. Tri-state. HAS_SIDE_EFFECT — manual parser sets cfg_keys_explicit bit for NormalizeForMode flip rule.",                                                                                                                                                                                                                                                                                                       \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        trading_mode,                "Trading Mode",         "Operational",     CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::SAFETY_CRITICAL | CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 2), \
        "Trading mode: 0=PAPER (default; safe), 1=PROD_SHADOW (live wired but bracketed), 2=LIVE (real money). Accepts string ('paper'/'shadow'/'live') or int. LIVE flips model_verify_strict 0->1 + reconcile_mode WARN->STRICT. HAS_SIDE_EFFECT — manual parser handles string form + NormalizeForMode triggers. SAFETY_CRITICAL.", \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Drift Acknowledgments (3) === */                                                                                                                                                                            \
    X(KIND_BOOL,       acknowledge_hardcoded_strategy_in_live, "Ack Hardcoded Strategy in Live", "Drift Acknowledgments", CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),             \
        "Explicit acknowledgment required to run hardcoded strategy (no per-core override) in live mode. Safety gate; operator must opt-in.",                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       acknowledge_hot_swap_with_open_positions, "Ack Hot Swap w/ Open Positions", "Drift Acknowledgments", CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                              \
        "Explicit acknowledgment to hot-swap strategy/model while positions are open. Without this, hot-swap is DEFERRED until position closes (v5.10.0c default).",                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       allow_cross_major_engine,    "Allow Cross-Major Engine", "Drift Acknowledgments", CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                              \
        "Allow engine to load when model_path is from a different major version. v5.9.2b — refuse by default; explicit override to enable cross-major migration.",                                                   \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Runtime GUI toggle (1) === */                                                                                                                                                                              \
    X(KIND_BOOL,       danger_enabled,              "Enabled",              "Danger Gradient", CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        nullptr,                                                                                                                                                                                                    \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG)

//======================================================================================================
// [FOREACH_PER_CORE_CFG_FIELD — trading / strategy / entry / exit / ML / risk-gate / regime-detection]
//======================================================================================================
// 79 rows. Each per-core row lives at `cfg.cores[c].<field>` (one instance per
// execution core; up to MAX_EXECUTION_CORES = 16). PER_CORE_OK metadata bit
// is REMOVED — registry membership IS the scope assertion.
//======================================================================================================
#define FOREACH_PER_CORE_CFG_FIELD(X)                                                                                                                                                                                \
    /* === Trading (6) === */                                                                                                                                                                                         \
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
    /* === Entry Filters (9) === */                                                                                                                                                                                   \
    X(KIND_DOUBLE_PCT, entry_offset_pct,            "Offset %%",            "Entry Filters",   0,                                  DBL(0.15, 0.0, 5.0),                                                                                                            \
        "Buy gate offset below avg/EMA price\nhigher = deeper dip required to enter",                                                                                                                                                                             \
        STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, offset_min,                  "Offset Min %%",        "Entry Filters",   0,                                  DBL(0.05, 0.0, 5.0),     "Adaptation lower bound for offset_pct",                                                          STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, offset_max,                  "Offset Max %%",        "Entry Filters",   0,                                  DBL(0.50, 0.0, 5.0),     "Adaptation upper bound for offset_pct",                                                          STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
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
    /* === Time-Based Exit (4) === */                                                                                                                                                                                 \
    X(KIND_DOUBLE,     tp_hold_score,               "TP Hold Score",        "Time-Based Exit", 0,                                  DBL(0.0, 0.0, 10.0),     "Min SNR*R² to hold past TP (0 = disabled, fixed TP)",                                            STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     tp_trail_mult,               "TP Trail Mult",        "Time-Based Exit", 0,                                  DBL(1.0, 0.0, 10.0),     "Trailing distance: stddev * this (e.g. 1.0)",                                                    STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     sl_trail_mult,               "SL Trail Mult",        "Time-Based Exit", 0,                                  DBL(2.0, 0.0, 10.0),     "Trailing SL distance: stddev * this (e.g. 2.0)",                                                 STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        max_hold_ticks,              "Max Hold",             "Time-Based Exit", CfgFieldDescriptor::WARN_ON_CLAMP, INT(75000, 0, 100000000),                                                          \
        "Close position after this many ticks (engine-wide).\n0 = disabled, 75000 ≈ 4-5 hours.\nPer-core min-gain floor lives in each core's Time Exit override.",                                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Risk per-core — kill switches + max-drawdown (6) === */                                                                                                                                                    \
    X(KIND_DOUBLE_PCT, max_drawdown_pct,            "Max DD %%",            "Risk Management", 0,                                  DBL(20.0, 0.0, 100.0),                                                                                                           \
        "Circuit breaker: halt trading if total P&L\ndrops below this %% of starting balance",                                                                                                                                                                    \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, max_exposure_pct,            "Max Exp %%",           "Risk Management", 0,                                  DBL(50.0, 0.0, 100.0),   nullptr,                                                                                          STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, kill_switch_daily_loss_pct,  "Daily Loss %%",        "Kill Switch",     CfgFieldDescriptor::SAFETY_CRITICAL,DBL(3.0, 0.0, 100.0),                                                                                                           \
        "Max session loss before kill switch triggers\n3.0 = halt if equity drops 3%% from session start",                                                                                                                                                        \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, kill_switch_drawdown_pct,    "Drawdown %%",          "Kill Switch",     CfgFieldDescriptor::SAFETY_CRITICAL,DBL(5.0, 0.0, 100.0),                                                                                                           \
        "Max drawdown from session peak before kill\n5.0 = halt if 5%% below intra-session high",                                                                                                                                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       enable_mtm_kill_switch,      "MTM Kill Switch",      "Kill Switch",     CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "Mark-to-market kill switch — halt new entries when realized + unrealized P&L crosses kill_switch_threshold_pct. Separate from balance-based kill switch (always armed).",                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        kill_recovery_warmup,        "Recovery",             "Kill Switch",     CfgFieldDescriptor::WARN_ON_CLAMP, INT(100, 0, 1000000),                                                              \
        "Slow-path cycles to observe after kill reset\nbefore trading resumes (prevents immediate re-entry)",                                                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Gate Recovery (5) === */                                                                                                                                                                                   \
    X(KIND_BOOL,       sl_cooldown_adaptive,        "Adaptive CD",          "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "Post-stop-loss cooldown mode: 0 = fixed cycles (sl_cooldown_cycles), 1 = scale by trend confidence (longer cooldown when trend weakens; shorter when trend resumes).",                                     \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        sl_cooldown_base,            "SL Cooldown Base",     "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 1000000),                                                                \
        "Base cycles to cooldown after stop loss (adaptive mode adds extra per loss; non-adaptive uses sl_cooldown_cycles alone).",                                                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        sl_cooldown_extra,           "SL Cooldown Extra",    "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 1000000),                                                                \
        "Extra cooldown cycles per consecutive stop loss (adaptive mode only).",                                                                                                                                    \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        sl_cooldown_cycles,          "SL Cooldown",          "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(30, 0, 1000000),                                                               \
        "Slow-path cycles to pause after stop loss\nlets market settle before re-entry",                                                                                                                            \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        idle_reset_cycles,           "Idle Reset",           "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(100, 0, 1000000),                                                              \
        "Cycles with no fill before gate decay\nprevents permanent lockout after losses",                                                                                                                           \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Momentum strategy (7) === */                                                                                                                                                                               \
    X(KIND_DOUBLE,     momentum_min_tp_margin_pct,  "Mom Min TP Margin",    "Strategies",      0,                                  DBL(0.0, 0.0, 0.05),     "Block momentum entry if TP too tight (0 = disabled; rec: 0.0040 = 0.40%)",                       STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     momentum_min_buy_delta_recent, "Mom Min Buy Delta", "Strategies",       0,                                  DBL(0.0, 0.0, 1.0),      "Min recent volume delta for momentum entry (rec: 0.05)",                                          STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     momentum_min_r2,             "Mom Min R²",           "Strategies",      0,                                  DBL(0.0, 0.0, 1.0),      "Min short_r2 for momentum entry (0 = disabled; rec: 0.30)",                                       STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     momentum_tp_mult,            "Mom TP Mult",          "Strategies",      0,                                  DBL(3.0, 0.0, 10.0),     "TP multiplier for momentum (e.g. 3.0 stddevs)",                                                  STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     momentum_sl_mult,            "Mom SL Mult",          "Strategies",      0,                                  DBL(1.0, 0.0, 10.0),     "SL multiplier for momentum (e.g. 1.0 stddevs)",                                                  STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     momentum_breakout_mult,      "Mom Breakout Mult",    "Strategies",      0,                                  DBL(0.5, 0.0, 10.0),     "Momentum breakout floor in stddevs",                                                              STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_BOOL,       momentum_require_last_win,   "Require Last Win",     "Momentum Tuning", CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "SHALT_MOM_LAST_LOST gate — block Momentum re-entry until previous trade was a TP win. 0 = off (default; recommended). 1 = enable; favors winning streaks at cost of slower recovery.",                     \
        STRAT_CAT_REGRESSION_DRIVEN,                         OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === EMA Cross strategy (3) === */                                                                                                                                                                              \
    X(KIND_DOUBLE,     emacross_dip_mult,           "EMACross Dip Mult",    "Strategies",      0,                                  DBL(0.5, 0.0, 10.0),     "Buy this many stddevs below EMA",                                                                STRAT_CAT_STATIC_RULES,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     emacross_crossover_min,      "EMACross Crossover",   "Strategies",      0,                                  DBL(0.0, 0.0, 1.0),      "Min EMA-SMA spread for uptrend confirmation",                                                    STRAT_CAT_STATIC_RULES,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     emacross_trail_mult,         "EMACross Trail Mult",  "Strategies",      0,                                  DBL(1.0, 0.0, 10.0),     "Trailing TP factor when EMA rising",                                                              STRAT_CAT_STATIC_RULES,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Regime Detection (5) === */                                                                                                                                                                                \
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
    X(KIND_INT,        regime_hysteresis,           "Hysteresis",           "Regime Detection",CfgFieldDescriptor::WARN_ON_CLAMP, INT(3, 1, 100),                                                                    \
        "Slow-path cycles before regime switch\nprevents rapid flipping between strategies",                                                                                                                        \
        STRAT_CAT_REGIME_AWARE,                              OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === ML — entry threshold + TP/SL (3) === */                                                                                                                                                                    \
    X(KIND_DOUBLE,     ml_buy_threshold,            "ML Buy Thresh",        "ML",              0,                                  DBL(0.5, 0.0, 1.0),      "Buy threshold for ML strategy (predictions above this enter)",                                    STRAT_CAT_ML,                                                  OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, ml_tp_pct,                   "ML TP %%",             "ML",              0,                                  DBL(2.0, 0.0, 100.0),    "ML strategy take profit (overrides take_profit_pct)",                                            STRAT_CAT_ML,                                                  OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, ml_sl_pct,                   "ML SL %%",             "ML",              0,                                  DBL(1.0, 0.0, 100.0),    "ML strategy stop loss (overrides stop_loss_pct)",                                                STRAT_CAT_ML,                                                  OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === ML — Bandit/Confidence/Ensemble (per-core authoritative) (11) === */                                                                                                                                       \
    X(KIND_DOUBLE,     bandit_blend_ratio,          "Bandit Blend",         "ML",              0,                                  DBL(0.5, 0.0, 1.0),      "Mix of bandit picks vs base model",                                                              STRAT_CAT_ML | STRAT_CAT_USES_BANDIT,                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     confidence_threshold_scale,  "Conf Thresh Scale",    "ML",              0,                                  DBL(1.0, 0.0, 5.0),      "Confidence-weighted entry threshold scaling",                                                    STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
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
    X(KIND_INT,        thompson_rng_seed,           "Thompson RNG Seed",    "FoxML",           CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::HAS_SIDE_EFFECT, INT(42, 0, 9223372036854775807),                \
        "splitmix64 seed for Thompson sampling bandit. Default 42. 0 = use ThompsonBandit.hpp's THOMPSON_RNG_SEED_DEFAULT. Boot-only; required for replay-determinism. HAS_SIDE_EFFECT — manual parser supports hex (0x...) base-auto-detect; registry walker skips.",                                                                                                                                                                                                                                                          \
        STRAT_CAT_ML | STRAT_CAT_USES_BANDIT,                OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        model_max_age_hours,         "Model Max Age Hours",  "Lifecycle",       CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 87600),                                                                  \
        "Refuse model load if file mtime older than N hours. 0 = disabled (legacy default; v5.14.8.E).",                                                                                                             \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === STAMP_BOUND scalar cohort — Ridge + Winsor + Confidence + Thompson (12 DOUBLE) === */ \
    /*       Ridge risk-parity blending (v5.14.0) */ \
    X(KIND_DOUBLE,     ridge_lambda,                "Ridge Lambda",         "ML/Ridge",        CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.15, 0.0, 10.0), \
        "Ridge regularization strength. Higher = more aggressive blending toward equal weights; lower = trusts per-arm IC signal more. Default 0.15. Stamp-bound (parity-critical).", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     ridge_cost_penalty,          "Ridge Cost Penalty",   "ML/Ridge",        CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.5, 0.0, 100.0), \
        "Cost penalty in net_IC = IC - penalty*cost (basis for ridge_min_ic_floor gate). Higher = penalizes arms with worse cost-IC tradeoff. Default 0.5. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     ridge_min_ic_floor,          "Ridge Min IC Floor",   "ML/Ridge",        CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.001, 0.0, 1.0), \
        "Minimum net_IC floor — arms below this floor get zero weight (prevents zero-sum starvation when all arms have weak signal). Default 0.001. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /*       Winsorization (label clipping for training-time outlier handling) */ \
    X(KIND_DOUBLE,     winsor_pct_low,              "Winsor Low",           "ML/Winsor",       CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.005, 0.0, 0.5), \
        "Lower winsor clip percentile (ratio, NOT percent). Default 0.005 = clip bottom 0.5%% of labels. Stamp-bound (training-serve parity).", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     winsor_pct_high,             "Winsor High",          "ML/Winsor",       CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.995, 0.5, 1.0), \
        "Upper winsor clip percentile (ratio). Default 0.995 = clip top 0.5%% of labels. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /*       Composite confidence (v5.14.1) */ \
    X(KIND_DOUBLE,     confidence_freshness_tau_secs, "Conf Freshness Tau", "ML/Confidence",   CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(3600.0, 0.0, 86400.0), \
        "Time-decay tau (seconds) for confidence-freshness term. Higher = slower decay of model trust as time-since-train grows. Default 3600s (1h). Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     confidence_capacity_target_dollars, "Conf Capacity Target $", "ML/Confidence", CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.0, 0.0, 1000000.0), \
        "Target trade capacity (dollars) for confidence-capacity term. 0 = disabled. Higher = larger trades trusted at full confidence. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     confidence_capacity_kappa,   "Conf Capacity Kappa",  "ML/Confidence",   CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.1, 0.0, 10.0), \
        "Capacity-curve shape parameter (kappa). Controls steepness of confidence falloff as trade size exceeds capacity. Default 0.1. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     confidence_rmse_baseline,    "Conf RMSE Baseline",   "ML/Confidence",   CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(1.0, 0.0, 100.0), \
        "RMSE baseline for confidence-error term. Predictions with RMSE above this baseline get reduced confidence. Default 1.0. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /*       Bayesian Thompson sampling (v5.14.10.B) */ \
    X(KIND_DOUBLE,     thompson_mu_prior,           "Thompson Mu Prior",    "ML/Thompson",     CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.0, -1000.0, 1000.0), \
        "Posterior mean prior (mu_0) for Bayesian Thompson sampling. Default 0.0 = neutral prior. Stamp-bound (parity-critical when bandit_algorithm == THOMPSON).", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     thompson_precision_prior,    "Thompson Tau Prior",   "ML/Thompson",     CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(1.0, 0.0001, 1000.0), \
        "Posterior precision prior (tau_0 = 1/variance) for Bayesian Thompson sampling. Default 1.0 = unit variance prior. Higher = tighter prior. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     thompson_precision_obs,      "Thompson Tau Obs",     "ML/Thompson",     CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(1.0, 0.0001, 1000.0), \
        "Observation precision (tau_obs = 1/sigma^2) for each reward update. Higher = each reward trusted more (faster posterior shift). Default 1.0. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /*       Bandit algorithm selector — INT enum (3-state today; .F.4c.2 expands to 5 ghost-training states) */ \
    X(KIND_INT,        bandit_algorithm,            "Bandit Algorithm",     "ML/Bandit",       CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 2), \
        "Bandit selector: 0=EXP3 (default; legacy), 1=THOMPSON (Bayesian; non-stationary-friendly), 2=BOTH (parallel A/B telemetry). Accepts string ('exp3'/'thompson'/'both') or int. HAS_SIDE_EFFECT — manual parser handles string form. v5.15.5.F.4c.2 will expand to 5 ghost-training states. Stamp-bound (parity-critical).", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Per-core risk thresholds — STAMP_BOUND (4) === */ \
    /*       Risk degradation (v5.14.9 soft-risk) */ \
    X(KIND_INT,        risk_degradation_curve,      "Risk Degradation Curve","Risk Management",CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 2), \
        "Risk degradation curve shape: 0=CURVE_OFF (no degradation), 1=LINEAR, 2=EXP. Accepts string ('off'/'linear'/'exp') or int. HAS_SIDE_EFFECT — manual parser handles string form + legacy risk_scale_by_confidence alias. Stamp-bound.", \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     risk_full_size_threshold,    "Risk Full-Size Thresh","Risk Management", CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.15, 0.0, 1.0), \
        "Confidence threshold above which trades get full size. Default 0.15. Stamp-bound.", \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     risk_min_size_threshold,     "Risk Min-Size Thresh", "Risk Management", CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.05, 0.0, 1.0), \
        "Confidence threshold below which trades get blocked (no-trade band). Default 0.05. Stamp-bound.", \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     risk_min_size_pct,           "Risk Min-Size Floor",  "Risk Management", CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.10, 0.0, 1.0), \
        "Floor on degraded position size (ratio of full size, e.g. 0.10 = 10%% of normal). Default 0.10. Stamp-bound.", \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Partial exits / Breakeven (3) === */                                                                                                                                                                       \
    X(KIND_DOUBLE,     partial_exit_pct,            "Partial Exit %%",      "Partial Exits",   0,                                  DBL(0.5, 0.0, 1.0),      "Fraction of position to close at TP1 (0.5 = 50%)",                                               STRAT_CAT_ALL,                                                   OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     tp2_mult,                    "TP2 Mult",             "Partial Exits",   0,                                  DBL(2.0, 0.0, 10.0),     "TP2 distance = TP1_distance * this (2.0 = double TP)",                                           STRAT_CAT_ALL,                                                   OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, breakeven_buffer_pct,        "Breakeven Buf %%",     "Partial Exits",   0,                                  DBL(0.0, 0.0, 5.0),      "SL offset from entry once breakeven ratchet fires",                                              STRAT_CAT_ALL,                                                   OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Strategy-specific TP/SL overrides (6 — FPN<F>) — v5.15.5.F.4c.3 WIP2c.1 classify-first === */                                                                                                              \
    /* 0 = fall back to global take_profit_pct / stop_loss_pct (strategy parameter dispatcher applies). Per-core authoritative when set. */                                                                           \
    X(KIND_DOUBLE_PCT, simpledip_tp_pct,            "SimpleDip TP %%",      "Strategies",      0,                                  DBL(0.0, 0.0, 100.0),    "SimpleDip TP override (%, stored as decimal; 0 = fall back to take_profit_pct)",                STRAT_CAT_STATIC_RULES,                                          OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, simpledip_sl_pct,            "SimpleDip SL %%",      "Strategies",      0,                                  DBL(0.0, 0.0, 100.0),    "SimpleDip SL override (%, stored as decimal; 0 = fall back to stop_loss_pct)",                  STRAT_CAT_STATIC_RULES,                                          OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, mr_tp_pct,                   "MR TP %%",             "Strategies",      0,                                  DBL(0.0, 0.0, 100.0),    "MeanReversion TP override (%, stored as decimal; 0 = fall back to take_profit_pct)",            STRAT_CAT_STATIC_RULES,                                          OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, mr_sl_pct,                   "MR SL %%",             "Strategies",      0,                                  DBL(0.0, 0.0, 100.0),    "MeanReversion SL override (%, stored as decimal; 0 = fall back to stop_loss_pct)",              STRAT_CAT_STATIC_RULES,                                          OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, emacross_tp_pct,             "EMACross TP %%",       "Strategies",      0,                                  DBL(0.0, 0.0, 100.0),    "EMA Cross TP override (%, stored as decimal; 0 = fall back to take_profit_pct)",                STRAT_CAT_STATIC_RULES,                                          OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, emacross_sl_pct,             "EMACross SL %%",       "Strategies",      0,                                  DBL(0.0, 0.0, 100.0),    "EMA Cross SL override (%, stored as decimal; 0 = fall back to stop_loss_pct)",                  STRAT_CAT_STATIC_RULES,                                          OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Entry stddev mode (1 — FPN<F>) — v5.15.5.F.4c.3 WIP2c.1 classify-first === */                                                                                                                              \
    X(KIND_DOUBLE,     offset_stddev_mult,          "Offset Stddev Mult",   "Entry Filters",   0,                                  DBL(0.0, 0.0, 10.0),     "stddev-scaled offset multiplier (0 = use offset_pct mode; non-zero = stddev-adaptive mode)",    STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN,            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === ML hard-block + ensemble + barrier (3 — 1 FPN<F> + 1 double + 1 INT_ENUM) — v5.15.5.F.4c.3 WIP2c.1 classify-first === */                                                                                   \
    X(KIND_DOUBLE,     confidence_hard_block_threshold, "Conf Hard Block",  "ML",              CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.0, 0.0, 1.0),      "Confidence floor — predictions with confidence below this hard-block (no entry). 0 = disabled.", STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     ensemble_min_agreement_pct,  "Ens Min Agreement %%", "Ensemble",        CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.0, 0.0, 1.0),      "Min fraction of non-disabled horizons that must agree on direction for entry (0 = disabled).",   STRAT_CAT_ML | STRAT_CAT_USES_BANDIT,                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_INT,        barrier_blend_mode,          "Barrier Blend Mode",   "ML",              CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 3),                                  \
        "Per-horizon TP/SL serving mode: 0=LEGACY (cfg-direct fallback), 1=BLEND (weighted across horizons), 2=DOMINANT (highest-weight horizon). HAS_SIDE_EFFECT — manual parser handles string form ('legacy'/'blend'/'dominant').", \
        STRAT_CAT_ML,                                                   OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Maker/Taker fees + VolScaler (3) — WIP2c.2 inclusion (Strategy_BuildParameters reads these) === */                                                                                                          \
    X(KIND_DOUBLE_PCT, fee_rate_maker,               "Fee Maker %%",         "Trading",         CfgFieldDescriptor::HAS_SIDE_EFFECT,                                  DBL(0.075, 0.0, 5.0),    "Maker fill fee rate (% per trade; e.g. 0.075 = 0.075% Binance tier 0). Cohort sibling of fee_rate (legacy) + fee_rate_taker.", STRAT_CAT_ALL, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE_PCT, fee_rate_taker,               "Fee Taker %%",         "Trading",         CfgFieldDescriptor::HAS_SIDE_EFFECT,                                  DBL(0.100, 0.0, 5.0),    "Taker fill fee rate (% per trade; e.g. 0.100 = 0.100% Binance tier 0). Cohort sibling of fee_rate (legacy) + fee_rate_maker.", STRAT_CAT_ALL, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(KIND_DOUBLE,     foxml_vol_scaling_z_max,      "Vol Scale Z-Max",      "ML",              CfgFieldDescriptor::WARN_ON_CLAMP, DBL(3.0, 0.0, 100.0),    "Z-score clipping threshold for FoxML VolScaler (limits how much volatility scaling can compress trade size). Default 3.0 = clip at 3 sigma.", STRAT_CAT_ML, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG)

//======================================================================================================
// [FIELD_IDX enums — per-registry auto-generated]
//======================================================================================================
// Drives g_*_cfg_field_descriptors[FIELD_IDX_*_<name>] direct access at compile time.
#define X_GEN_GLOBAL_FIELD_IDX(KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_to_strategy_cat, applies_to_op_mode_cat, applies_to_regime_cat, applies_to_risk_cat, lives_in_struct) \
    FIELD_IDX_GLOBAL_##name,
#define X_GEN_PER_CORE_FIELD_IDX(KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_to_strategy_cat, applies_to_op_mode_cat, applies_to_regime_cat, applies_to_risk_cat, lives_in_struct) \
    FIELD_IDX_PER_CORE_##name,

enum CfgGlobalFieldIdx : uint16_t {
    FOREACH_GLOBAL_CFG_FIELD(X_GEN_GLOBAL_FIELD_IDX)
    FIELD_IDX_GLOBAL_END  // sentinel; equals global registry entry count
};

enum CfgPerCoreFieldIdx : uint16_t {
    FOREACH_PER_CORE_CFG_FIELD(X_GEN_PER_CORE_FIELD_IDX)
    FIELD_IDX_PER_CORE_END  // sentinel; equals per-core registry entry count
};

#undef X_GEN_GLOBAL_FIELD_IDX
#undef X_GEN_PER_CORE_FIELD_IDX

//======================================================================================================
// [g_*_cfg_field_descriptors — auto-generated arrays]
//======================================================================================================
// Single source of truth for descriptor data; consumers index via FIELD_IDX_GLOBAL_<name>
// or FIELD_IDX_PER_CORE_<name>.
#define X_GEN_DESCRIPTOR_GLOBAL(KIND_TOKEN, name, label, section, meta, payload_init, tooltip, applies_to_strategy_cat, applies_to_op_mode_cat, applies_to_regime_cat, applies_to_risk_cat, lives_in_struct) \
    CfgFieldDescriptor{                                                                                  \
        /* kind            */ CfgFieldDescriptor::KIND_TOKEN,                                            \
        /* lives_in_struct */ static_cast<uint8_t>(lives_in_struct),                                     \
        /* metadata_flags  */ static_cast<uint16_t>(meta),                                               \
        /* _reserved       */ 0,                                                                         \
        /* field_idx       */ FIELD_IDX_GLOBAL_##name,                                                   \
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

#define X_GEN_DESCRIPTOR_PER_CORE(KIND_TOKEN, name, label, section, meta, payload_init, tooltip, applies_to_strategy_cat, applies_to_op_mode_cat, applies_to_regime_cat, applies_to_risk_cat, lives_in_struct) \
    CfgFieldDescriptor{                                                                                  \
        /* kind            */ CfgFieldDescriptor::KIND_TOKEN,                                            \
        /* lives_in_struct */ static_cast<uint8_t>(lives_in_struct),                                     \
        /* metadata_flags  */ static_cast<uint16_t>(meta),                                               \
        /* _reserved       */ 0,                                                                         \
        /* field_idx       */ FIELD_IDX_PER_CORE_##name,                                                 \
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
inline constexpr CfgFieldDescriptor g_global_cfg_field_descriptors[] = {
    FOREACH_GLOBAL_CFG_FIELD(X_GEN_DESCRIPTOR_GLOBAL)
};

inline constexpr CfgFieldDescriptor g_per_core_cfg_field_descriptors[] = {
    FOREACH_PER_CORE_CFG_FIELD(X_GEN_DESCRIPTOR_PER_CORE)
};

#undef X_GEN_DESCRIPTOR_GLOBAL
#undef X_GEN_DESCRIPTOR_PER_CORE

// Verify array sizes match sentinels.
static_assert(sizeof(g_global_cfg_field_descriptors) / sizeof(g_global_cfg_field_descriptors[0]) == FIELD_IDX_GLOBAL_END,
              "g_global_cfg_field_descriptors size must equal FIELD_IDX_GLOBAL_END");
static_assert(sizeof(g_per_core_cfg_field_descriptors) / sizeof(g_per_core_cfg_field_descriptors[0]) == FIELD_IDX_PER_CORE_END,
              "g_per_core_cfg_field_descriptors size must equal FIELD_IDX_PER_CORE_END");

// v5.15.5.F.4c.1 — defense-in-depth: every cfg_field_name MUST be unique within
// a registry. FIELD_IDX_*_<name> enum already enforces this at the preprocessor
// level (duplicate name = duplicate enumerator = build error); this static_assert
// is explicit documentation of the contract + safety net against macro mistakes.
//
// .F.4c.3 — extended to operate per-registry. Cross-registry name collision is
// ALLOWED only with explicit per-axis rationale (see DESIGN_SPECS/cfg-scope-discipline.md
// § "Anti-patterns" — discouraged but not enforced; mostly an error indicator
// for accidental over-classification).
//
// The GUI/SettingsPanel.hpp tt::cfg_render_field<T> wrapper uses
// ImGui::PushID(desc.cfg_field_name) to make ImGui widget IDs independent of
// display labels (so two rows with the same operator-facing label don't collide
// at ImGui's hash table). That guarantee REQUIRES cfg_field_name uniqueness.
// Violation = "name is already taken" runtime error + non-functional controls.
//
// O(N²) at compile time over ~80 rows per registry = trivial.
template <size_t N>
constexpr bool cfg_field_names_unique(const CfgFieldDescriptor (&a)[N]) {
    for (size_t i = 0; i < N; ++i)
        for (size_t j = i + 1; j < N; ++j)
            if (__builtin_strcmp(a[i].cfg_field_name, a[j].cfg_field_name) == 0)
                return false;
    return true;
}
static_assert(cfg_field_names_unique(g_global_cfg_field_descriptors),
              "FOREACH_GLOBAL_CFG_FIELD has duplicate cfg_field_name — violates "
              "row-uniqueness contract + breaks .F.4c.1 ImGui PushID collision "
              "avoidance. Find + rename the duplicate.");
static_assert(cfg_field_names_unique(g_per_core_cfg_field_descriptors),
              "FOREACH_PER_CORE_CFG_FIELD has duplicate cfg_field_name — violates "
              "row-uniqueness contract + breaks .F.4c.1 ImGui PushID collision "
              "avoidance. Find + rename the duplicate.");

// Note: orthogonality with FOREACH_CFG_DERIVED_INFERENCE_CFG (v5.15.5.A.7,
// MemHeaders/CfgDerivedInferenceCfgRegistry.hpp). That registry reads SAME cfg
// fields but writes to inf.inference_cfg_* for stamp emit (different consumer).
// The two are orthogonal by design. Verified at .F.4d via reverse-drift CI script.

//======================================================================================================
// [BITMAP DISPATCHER FRAMEWORK — TEMPLATED FOR PER-REGISTRY APPLICATION]
//======================================================================================================
// .F.4c.3 — template-parameterized on (N_FIELDS, descriptor array). Each
// registry instantiates its own mask arrays + composed views. Same template
// body composes against either FOREACH_GLOBAL_CFG_FIELD or
// FOREACH_PER_CORE_CFG_FIELD (or any future per-instance registry per
// DESIGN_SPECS/per-instance-registry-pattern.md).
//
// Per H6: these arrays are populated ONCE at static-init time (single-threaded;
// before any thread spawns) then read-only forever. No cross-thread WRITE → no
// alignas(64) discipline needed.
//
// Per H7: bitmap iteration is branchless within the inner loop (__builtin_ctzll +
// `word &= word - 1` for next-bit-clear). Outer `while (word)` is loop control,
// not data-dependent dispatch.
//
// Per CLAUDE.md framework discipline (item 31): adding a new metadata bit = 1 row
// in FOREACH_METADATA_BIT below; mask arrays auto-generate for BOTH registries
// via X-macro instantiation pass.
//======================================================================================================

// CfgMaskArray<N_WORDS> — fixed-size mask wrapper. N_WORDS is computed per
// registry as (N_FIELDS + 63) / 64.
template <size_t N_WORDS>
struct CfgMaskArray {
    static constexpr size_t WORDS = N_WORDS;
    uint64_t words[N_WORDS];

    constexpr uint64_t operator[](size_t i) const { return words[i]; }
    constexpr uint64_t& operator[](size_t i)      { return words[i]; }
};

// cfg_compute_mask<Bit>(arr) — walks descriptor array at compile time; produces
// per-bit mask into .rodata. Zero runtime init cost.
template <uint16_t Bit, size_t N>
constexpr CfgMaskArray<(N + 63) / 64> cfg_compute_mask(const CfgFieldDescriptor (&arr)[N]) {
    constexpr size_t WORDS = (N + 63) / 64;
    CfgMaskArray<WORDS> result = {};
    for (size_t i = 0; i < N; ++i) {
        if (arr[i].metadata_flags & Bit) {
            result.words[i / 64] |= (1ULL << (i % 64));
        }
    }
    return result;
}

// cfg_field_count(mask) — popcount over a mask array. constexpr so call
// sites with literal mask arrays fold to a single immediate at compile time.
template <size_t N_WORDS>
constexpr size_t cfg_field_count(const CfgMaskArray<N_WORDS>& mask) {
    size_t n = 0;
    for (size_t i = 0; i < N_WORDS; ++i) {
        n += static_cast<size_t>(__builtin_popcountll(mask.words[i]));
    }
    return n;
}

// FOREACH_METADATA_BIT(X) — tuple: X(lowercase_name, UPPERCASE_BIT_NAME).
// .F.4c.3 — PER_CORE_OK removed (redundant under per-core authoritative registry).
// Each remaining row adds a per-bit precomputed mask array per registry.
#define FOREACH_METADATA_BIT(X)                                            \
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

// Per-registry per-bit precomputed mask arrays — X-macro generated.
// Each lands in .rodata as a compile-time constant.
#define X_GEN_GLOBAL_MASK(lname, BITNAME) \
    inline constexpr auto g_global_cfg_##lname##_mask = \
        cfg_compute_mask<CfgFieldDescriptor::BITNAME>(g_global_cfg_field_descriptors);
FOREACH_METADATA_BIT(X_GEN_GLOBAL_MASK)
#undef X_GEN_GLOBAL_MASK

#define X_GEN_PER_CORE_MASK(lname, BITNAME) \
    inline constexpr auto g_per_core_cfg_##lname##_mask = \
        cfg_compute_mask<CfgFieldDescriptor::BITNAME>(g_per_core_cfg_field_descriptors);
FOREACH_METADATA_BIT(X_GEN_PER_CORE_MASK)
#undef X_GEN_PER_CORE_MASK

//------------------------------------------------------------------------------
// [PER-LivesInStruct-VALUE BITMAP MASKS — per-registry application]
//------------------------------------------------------------------------------
// Forward-compat for `.F.4i` BACKTEST cohort + future training/secrets cohorts.
// Pattern is the metadata-bit-mask analogue but for an enum VALUE (equality
// check) rather than a bit (bitwise AND).
//------------------------------------------------------------------------------

// FOREACH_LIVES_IN_STRUCT(X) — tuple: X(lowercase_name, UPPERCASE_VALUE_NAME).
// Mirrors the LivesInStruct enum.
#define FOREACH_LIVES_IN_STRUCT(X)                                          \
    X(struct_cfg,            STRUCT_CFG)                                    \
    X(struct_backtest_cfg,   STRUCT_BACKTEST_CFG)                           \
    X(struct_controller_cfg, STRUCT_CONTROLLER_CFG)                         \
    X(struct_secrets_cfg,    STRUCT_SECRETS_CFG)                            \
    X(struct_training_cfg,   STRUCT_TRAINING_CFG)

// Compile-time per-LivesInStruct-value mask computation.
// NOTE: dispatch via EQUALITY (lives_in_struct == Value) not bitwise AND;
// each row contributes to exactly one mask.
template <uint8_t Value, size_t N>
constexpr CfgMaskArray<(N + 63) / 64> cfg_compute_lives_in_struct_mask(const CfgFieldDescriptor (&arr)[N]) {
    constexpr size_t WORDS = (N + 63) / 64;
    CfgMaskArray<WORDS> result = {};
    for (size_t i = 0; i < N; ++i) {
        if (arr[i].lives_in_struct == Value) {
            result.words[i / 64] |= (1ULL << (i % 64));
        }
    }
    return result;
}

// Per-registry per-value mask declarations.
#define X_GEN_GLOBAL_LIVES_IN_STRUCT_MASK(lname, VALUE) \
    inline constexpr auto g_global_cfg_##lname##_mask = \
        cfg_compute_lives_in_struct_mask<CfgFieldDescriptor::VALUE>(g_global_cfg_field_descriptors);
FOREACH_LIVES_IN_STRUCT(X_GEN_GLOBAL_LIVES_IN_STRUCT_MASK)
#undef X_GEN_GLOBAL_LIVES_IN_STRUCT_MASK

#define X_GEN_PER_CORE_LIVES_IN_STRUCT_MASK(lname, VALUE) \
    inline constexpr auto g_per_core_cfg_##lname##_mask = \
        cfg_compute_lives_in_struct_mask<CfgFieldDescriptor::VALUE>(g_per_core_cfg_field_descriptors);
FOREACH_LIVES_IN_STRUCT(X_GEN_PER_CORE_LIVES_IN_STRUCT_MASK)
#undef X_GEN_PER_CORE_LIVES_IN_STRUCT_MASK

//------------------------------------------------------------------------------
// [CFG_FIELD_FOR_EACH_SET_BIT — iteration macro]
//------------------------------------------------------------------------------
// Invokes `body` per set bit in `mask`. `idx_var` is bound to FIELD_IDX_*
// value of each set bit. Branchless inner loop via __builtin_ctzll (single
// TZCNT on Haswell+) + `word &= word - 1` next-bit-clear.
//
// Accepts either CfgMaskArray<N>.words (raw uint64_t array) directly OR the
// .words member of a CfgMaskArray<N>. Word count derived from sizeof.
//
// Usage:
//   CFG_FIELD_FOR_EACH_SET_BIT(g_global_cfg_deprecated_mask.words, idx, {
//       fprintf(stderr, "deprecated: %s\n", g_global_cfg_field_descriptors[idx].cfg_field_name);
//   });
#define CFG_FIELD_FOR_EACH_SET_BIT(mask, idx_var, body)                    \
    for (size_t _w = 0; _w < sizeof(mask) / sizeof((mask)[0]); _w++) {     \
        uint64_t _word = (mask)[_w];                                       \
        while (_word) {                                                    \
            const size_t _bit = static_cast<size_t>(__builtin_ctzll(_word));\
            const size_t idx_var = _w * 64 + _bit;                         \
            do { body; } while (0);                                        \
            _word &= _word - 1;                                            \
        }                                                                  \
    }

//------------------------------------------------------------------------------
// [Per-registry composed-filter masks]
//------------------------------------------------------------------------------

// Global registry — composed views

constexpr CfgMaskArray<(FIELD_IDX_GLOBAL_END + 63) / 64> cfg_compose_global_render_mask() {
    constexpr size_t WORDS = (FIELD_IDX_GLOBAL_END + 63) / 64;
    CfgMaskArray<WORDS> out = {};
    for (size_t i = 0; i < WORDS; ++i) {
        out.words[i] = ~(g_global_cfg_is_boot_only_mask.words[i] | g_global_cfg_hidden_by_default_mask.words[i]);
    }
    if constexpr ((FIELD_IDX_GLOBAL_END % 64) != 0) {
        constexpr uint64_t last_word_valid = (1ULL << (FIELD_IDX_GLOBAL_END % 64)) - 1ULL;
        out.words[WORDS - 1] &= last_word_valid;
    }
    return out;
}
inline constexpr auto g_global_cfg_render_mask = cfg_compose_global_render_mask();

constexpr CfgMaskArray<(FIELD_IDX_GLOBAL_END + 63) / 64> cfg_compose_global_save_mask() {
    constexpr size_t WORDS = (FIELD_IDX_GLOBAL_END + 63) / 64;
    CfgMaskArray<WORDS> out = {};
    for (size_t i = 0; i < WORDS; ++i) {
        out.words[i] = ~g_global_cfg_has_side_effect_mask.words[i];
    }
    if constexpr ((FIELD_IDX_GLOBAL_END % 64) != 0) {
        constexpr uint64_t last_word_valid = (1ULL << (FIELD_IDX_GLOBAL_END % 64)) - 1ULL;
        out.words[WORDS - 1] &= last_word_valid;
    }
    return out;
}
inline constexpr auto g_global_cfg_save_mask = cfg_compose_global_save_mask();

inline constexpr auto g_global_cfg_stamp_emit_mask = g_global_cfg_stamp_bound_mask;

constexpr CfgMaskArray<(FIELD_IDX_GLOBAL_END + 63) / 64> cfg_compose_global_cli_explain_mask() {
    constexpr size_t WORDS = (FIELD_IDX_GLOBAL_END + 63) / 64;
    CfgMaskArray<WORDS> out = {};
    for (size_t i = 0; i < WORDS - 1; ++i) {
        out.words[i] = ~0ULL;
    }
    if constexpr ((FIELD_IDX_GLOBAL_END % 64) == 0) {
        out.words[WORDS - 1] = ~0ULL;
    } else {
        out.words[WORDS - 1] = (1ULL << (FIELD_IDX_GLOBAL_END % 64)) - 1ULL;
    }
    return out;
}
inline constexpr auto g_global_cfg_cli_explain_mask = cfg_compose_global_cli_explain_mask();

// Per-core registry — composed views

constexpr CfgMaskArray<(FIELD_IDX_PER_CORE_END + 63) / 64> cfg_compose_per_core_render_mask() {
    constexpr size_t WORDS = (FIELD_IDX_PER_CORE_END + 63) / 64;
    CfgMaskArray<WORDS> out = {};
    for (size_t i = 0; i < WORDS; ++i) {
        out.words[i] = ~(g_per_core_cfg_is_boot_only_mask.words[i] | g_per_core_cfg_hidden_by_default_mask.words[i]);
    }
    if constexpr ((FIELD_IDX_PER_CORE_END % 64) != 0) {
        constexpr uint64_t last_word_valid = (1ULL << (FIELD_IDX_PER_CORE_END % 64)) - 1ULL;
        out.words[WORDS - 1] &= last_word_valid;
    }
    return out;
}
inline constexpr auto g_per_core_cfg_render_mask = cfg_compose_per_core_render_mask();

constexpr CfgMaskArray<(FIELD_IDX_PER_CORE_END + 63) / 64> cfg_compose_per_core_save_mask() {
    constexpr size_t WORDS = (FIELD_IDX_PER_CORE_END + 63) / 64;
    CfgMaskArray<WORDS> out = {};
    for (size_t i = 0; i < WORDS; ++i) {
        out.words[i] = ~g_per_core_cfg_has_side_effect_mask.words[i];
    }
    if constexpr ((FIELD_IDX_PER_CORE_END % 64) != 0) {
        constexpr uint64_t last_word_valid = (1ULL << (FIELD_IDX_PER_CORE_END % 64)) - 1ULL;
        out.words[WORDS - 1] &= last_word_valid;
    }
    return out;
}
inline constexpr auto g_per_core_cfg_save_mask = cfg_compose_per_core_save_mask();

inline constexpr auto g_per_core_cfg_stamp_emit_mask = g_per_core_cfg_stamp_bound_mask;

constexpr CfgMaskArray<(FIELD_IDX_PER_CORE_END + 63) / 64> cfg_compose_per_core_cli_explain_mask() {
    constexpr size_t WORDS = (FIELD_IDX_PER_CORE_END + 63) / 64;
    CfgMaskArray<WORDS> out = {};
    for (size_t i = 0; i < WORDS - 1; ++i) {
        out.words[i] = ~0ULL;
    }
    if constexpr ((FIELD_IDX_PER_CORE_END % 64) == 0) {
        out.words[WORDS - 1] = ~0ULL;
    } else {
        out.words[WORDS - 1] = (1ULL << (FIELD_IDX_PER_CORE_END % 64)) - 1ULL;
    }
    return out;
}
inline constexpr auto g_per_core_cfg_cli_explain_mask = cfg_compose_per_core_cli_explain_mask();

// .F.4c.3 — g_cfg_per_core_ok_mask + g_cfg_per_core_override_mask DELETED.
// Per-core scope is registry membership, not a metadata bit; the override
// emit path went away with PerCoreOverrides<F> at this ship.
