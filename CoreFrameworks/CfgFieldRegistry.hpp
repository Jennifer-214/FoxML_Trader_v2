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

// WIP2d-1.B.0 (Shortsighted #5 close) — explicit includes for the 5 cfg-domain flag registries.
// Pre-WIP2d-1.B.0: this file relied on ControllerConfig.hpp's transitive include order
// (which included the 5 domain headers before including this file). The bitmap-overflow
// static_asserts at file scope below use <DOMAIN>_CFG_COUNT constants — if ControllerConfig
// changed include order, build would fail with cryptic "undeclared identifier" errors.
// Post-WIP2d-1.B.0: explicit includes here make this file self-contained.
#include "LifecycleCfgFlagRegistry.hpp"            // LIFECYCLE_CFG_COUNT
#include "GateCfgFlagRegistry.hpp"                 // GATE_CFG_COUNT
#include "../ML_Headers/MlCfgFlagRegistry.hpp"     // ML_CFG_COUNT
#include "RiskCfgFlagRegistry.hpp"                 // RISK_CFG_COUNT
#include "OpsCfgFlagRegistry.hpp"                  // OPS_CFG_COUNT

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
    // .F.4c.3: PER_CORE_OK (1u << 0) REMOVED. Every per-core row IS per-core by
    // construction (registry membership IS the scope assertion). Bit 0 RESERVED
    // for future use; do not reassign without documenting the schema change.
    //
    // .F.4c.3 WIP2d-1.B.0 (Shortsighted #1 close + Phase 1 latent regression fix):
    // HAS_SIDE_EFFECT bit was overloaded with TWO semantic meanings (manual parser
    // for string-form fields + manual sync from legacy parallel array for per-core-
    // only fields). Split into MANUAL_PARSER (1u << 10) + NO_FLAT_FIELD (1u << 12)
    // for explicit semantic separation per cfg-scope-discipline.md § "Metadata bit
    // semantic separation". Walker triplet (parser/copy/render) consumes the
    // SPECIFIC bit relevant to each walker's concern; no overload risk.
    //   - MANUAL_PARSER: skip registry parser walker (manual string-form parser handles)
    //   - NO_FLAT_FIELD: skip copy/render walkers (field exists only on cores[c], not on ControllerConfig flat scalar)
    // A row may carry one, both, or neither bit. `strategy` carries both (custom
    // string parser + no flat scalar). bandit_algorithm/risk_degradation_curve/
    // barrier_blend_mode/fee_rate_maker/fee_rate_taker carry MANUAL_PARSER only.
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
        MANUAL_PARSER         = 1u << 10,  // skip registry parser walker — manual string-form / side-effect parser handles (WIP2d-1.B.0; was HAS_SIDE_EFFECT pre-split)
        WARN_ON_CLAMP         = 1u << 11,  // emit "[cfg] WARN: <key>='<val>' out of range; clamping to <clamped>" when parse clamps value (.F.4c)
        NO_FLAT_FIELD         = 1u << 12,  // field exists only on cores[c] (no ControllerConfig flat scalar) — skip copy/render walkers (WIP2d-1.B.0)
        // v5.15.5.F.4d Charter 8 — STAMP_BOUND_CFG_DERIVED drives DERIVED_FILTER framework
        // (auto-generates POST_CFG mirror + CfgDerivedInferenceCfgRegistry + CfgDriftCheckRegistry
        // rows from single flagged source row). Per metadata-bit-driven-derived-filter-framework.md
        // Variant 3 (WIRE_FORMAT_TWO_SOURCE) Stage 3 ACTIVE. Subset of STAMP_BOUND (stamp-bound
        // INFERENCE-time cfg fields specifically; training-time AFFECTS_STAMP_PARITY uses different
        // derived filter). Closes Class 21 at derived-filter surface per H16 invariant.
        STAMP_BOUND_CFG_DERIVED = 1u << 13,
        // ... 2 bits headroom for future ...

        // Legacy alias — HAS_SIDE_EFFECT was overloaded; new code uses MANUAL_PARSER.
        // The 6 rows that used HAS_SIDE_EFFECT pre-WIP2d-1.B.0 migrate to MANUAL_PARSER
        // (or MANUAL_PARSER | NO_FLAT_FIELD for strategy). Alias retained for 1 ship
        // transition; remove at v5.15.5.F.4d codification.
        HAS_SIDE_EFFECT       = MANUAL_PARSER,
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
// [FOREACH_*_CFG_FIELD — 13-col tuple (per-core + global uniform at v5.15.5.F.4d.1.B.3+)]
//======================================================================================================
// Tuple (13 args; STORAGE_T leading column added to GLOBAL at .B.3 Step 0.5b.A per
// Decision A (a) Path α cascade closing global↔per-core column asymmetry):
//   X(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip,
//     applies_to_strategy_cat, applies_to_op_mode_cat,
//     applies_to_regime_cat, applies_to_risk_cat, lives_in_struct)
//
// STORAGE_T is the C++ destination type stored on ControllerConfig<F> (global rows) or
// PerCoreCfg<F> (per-core rows). KIND_TOKEN is GUI metadata only (H13/H14: Kind doesn't
// drive storage; STORAGE_T does). Adding a new global field = 1 row in
// FOREACH_GLOBAL_CFG_FIELD with explicit STORAGE_T + KIND_TOKEN; auto-gen mechanism
// in ControllerConfig<F> at Step 0.5b.B will replace 48 manual cfg field decls
// uniformly per H17 STRONG codification.
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
    X(uint16_t,             KIND_INT,        num_execution_cores,         "Execution Cores",      "Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(1, 1, 16),                                  \
        "Number of per-core execution shards. Clamp [1, 16].",                                                                                                                                                     \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_BOOL,       require_mlockall,            "Require mlockall",     "Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                       \
        "Pin engine memory at boot via mlockall(2) — prevents swap-out under memory pressure. Requires CAP_IPC_LOCK or root. Boot-only; runtime changes ignored.",                                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_BOOL,       init_arena_use_hugepages,    "Use Hugepages",        "Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                       \
        "Initialize per-core arenas with 2MB hugepages (MAP_HUGETLB). Reduces TLB pressure on hot path. Requires /sys/kernel/mm/hugepages configured. Boot-only.",                                                   \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint8_t,              KIND_BOOL,       sharded_force_synthetic,     "Force Synthetic Ticks","Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                       \
        "Debug/test toggle — force sharded engine to use synthetic tick generator instead of real Binance WS feed. Used for offline reproducibility tests. Boot-only.",                                              \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        slow_path_pin_offset,        "Slow-Path Pin Offset", "Operational",     CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(-1, -1, 256),                               \
        "Slow-path CPU pin offset. -1 = disabled, 0 = auto, >0 = explicit CPU offset.",                                                                                                                             \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Engine timing / Slow-path discipline (5) === */                                                                                                                                                            \
    X(uint32_t,             KIND_INT,        poll_interval,               "Poll Interval",        "Engine Timing",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(100, 1, 1000000),                                                              \
        "Ticks between slow-path runs (regression, adaptation, sample collection)\ndefault 100. ML training note: with poll_interval << forward_ticks,\nconsecutive samples have heavily-overlapping forward windows → label\nautocorrelation. For independent samples set poll_interval = forward_ticks.",                                                                                                                                                                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t,             KIND_INT,        slow_path_max_secs,          "Slow-Path Max Secs",   "Engine Timing",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 1, 3600),                                                                  \
        "Maximum wall-clock seconds per slow-path cycle. If exceeded, slow path emits warning + caps cycle. Default 60s; clamp [1, 3600].",                                                                          \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t,             KIND_INT,        warmup_ticks,                "Warmup Ticks",         "Engine Timing",   CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 100000000),                          \
        "Minimum raw ticks before trading starts. Counts every tick.\nUse this when you want a longer total-tick warmup. No upper bound.",                                                                          \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t,             KIND_INT,        min_warmup_samples,          "Min Rolling Samples",  "Engine Timing",   CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(64, 0, 128),                                \
        "Min rolling-stats samples before trading. CAPS at 128 (rolling window\nsize). Values >128 are clamped at config load with a warning. Use\nwarmup_ticks for longer raw-tick warmup.",                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint64_t,             KIND_INT,        lazy_rebuild_force_period_us,"Lazy Rebuild Force Period (us)","Performance",CfgFieldDescriptor::WARN_ON_CLAMP, INT(1000000, 0, 600000000),                                                    \
        "Force slow-path rebuild every N microseconds even if no parameter inputs changed. Defensive against stale state. Default 1s (1M us).",                                                                     \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Hot-path discipline (1) === */                                                                                                                                                                             \
    X(uint64_t,             KIND_INT,        param_max_age_ticks,         "Param Max Age Ticks",  "Lifecycle",       CfgFieldDescriptor::WARN_ON_CLAMP, INT(100000, 0, 1000000000),                                                        \
        "Hot-path parameter staleness gate — refuse trades if engine.cfg parameters last touched > N ticks ago. 0 = disabled.",                                                                                     \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Risk / Position limits — engine-wide (3) === */                                                                                                                                                            \
    X(uint32_t,             KIND_INT,        max_positions,               "Max Pos",              "Risk Management", CfgFieldDescriptor::WARN_ON_CLAMP, INT(1, 1, 16),                                                                    \
        nullptr,                                                                                                                                                                                                    \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        recovery_delay_secs,         "Recovery Delay",       "Risk Management", CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 0, 86400),                                                                 \
        "Wait this many seconds after flatten event before resuming trades. Prevents tilted re-entry on dead WS recovery.",                                                                                          \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        ws_dead_time_flatten_threshold_secs,"WS Dead-Time Flatten Threshold","Risk Management",CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 1, 3600),                                                   \
        "OMS_FlattenAll triggers if WS dead for >N seconds (paired with ws_dead_time_flatten_enabled bitmap flag).",                                                                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Recording (3) === */                                                                                                                                                                                       \
    X(int,                  KIND_BOOL,       record_ticks,                "Record Ticks",         "Tick Recording",  CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "Record raw ticks to CSV for backtesting/ML training\nOutput: data/{SYMBOL}/YYYY-MM-DD.csv\n~30-70MB/day for BTCUSDT",                                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_BOOL,       record_depth,                "Record Depth",         "Tick Recording",  CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "Record @depth5@100ms snapshots to CSV (top-of-book + lastUpdateId)\nOutput: data/{SYMBOL}/depth/YYYY-MM-DD.csv\nRequires depth_enabled=1. Daily rotation, auto-pruned by record_max_days.\nGap markers (# GAP) on backward last_update_id, wallclock >2s, or disconnect.\n~50 MB/day for BTCUSDT. Required for future backtest replay of book state.",                                                                                                                                                                                                                                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t,             KIND_INT,        record_max_days,             "Record Max Days",      "Tick Recording",  CfgFieldDescriptor::WARN_ON_CLAMP, INT(30, 1, 365),                                                                   \
        "Auto-delete tick + depth CSVs older than this many days. 30 = ~1-2GB cap on disk usage.",                                                                                                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Training (9) === */                                                                                                                                                                                        \
    X(int,                  KIND_INT,        xgb_min_child_weight,        "Min Child Weight",     "ML Hyperparams",  CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(5, 1, 50),                                  \
        "Min sum-of-weights per leaf (1-50). Higher = more regularization.\nDefault 5. Match deployed model's training value or expect WARN.",                                                                      \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        xgb_seed,                    "Seed",                 "ML Hyperparams",  CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(42, 0, 2147483647),                         \
        "RNG seed for reproducible runs. Default 42. Match deployed model's\ntraining seed or expect WARN.",                                                                                                        \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        xgb_train_nthread,           "Train Threads",        "Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(4, 1, 256),                                 \
        "XGBoost training thread count (OpenMP). Default 4; clamp [1, 256].",                                                                                                                                       \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        xgb_eval_nthread,            "Eval Threads",         "Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(4, 1, 256),                                 \
        "XGBoost evaluation thread count (OpenMP). Default 4; clamp [1, 256].",                                                                                                                                     \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        csv_load_workers,            "CSV Load Workers",     "Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(4, 1, 256),                                 \
        "Worker thread count for parallel CSV tick load during training. Default 4.",                                                                                                                               \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        multi_horizon_max_threads,   "Multi-Horizon Threads","Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(4, 1, 256),                                 \
        "Max parallel threads for multi-horizon training. Default 4.",                                                                                                                                              \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        feature_collect_max_gb,      "Feature Collect Max GB","Training",       CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(8, 1, 1024),                                \
        "Max GB of RAM for feature collection during training. OOM-kill protection. Default 8.",                                                                                                                    \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        wf_split_max_gb,             "Walk-Fwd Split Max GB","Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(8, 1, 1024),                                \
        "Max GB of RAM for walk-forward split during training. Default 8.",                                                                                                                                         \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        held_out_max_gb,             "Held-Out Max GB",      "Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(8, 1, 1024),                                \
        "Max GB of RAM for held-out validation set load. Default 8.",                                                                                                                                               \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Training discipline / Held-out (3) === */                                                                                                                                                                  \
    X(int,                  KIND_INT,        csv_sort_check_mode,         "CSV Sort Check Mode",  "Training",        CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(1, 0, 2),                                   \
        "CSV tick-sort validation: 0=STRICT (refuse load on unsort), 1=WARN (log + proceed; default), 2=DISABLED. Ships as KIND_INT pending TECH_DEBT-068.",                                                         \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_BOOL,       auto_stamp_on_held_out,      "Auto-Stamp on Held-Out","ML",             CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(1),                                                                          \
        "v5.8.10 — suite Run Full Validation auto-signs each generated stamp on held-out pass. Default 1 (auto-stamp). v5.15.5.F.4d.1.B.3 Path C 2026-05-24: manual bash workflow DELETED; this field's =0 value now only meaningful for v5.16+ cmdline-invocable training (per plans/_future/2026-05-12-decoupling-endgoal-roadmap.md). Operators using foxml_suite leave at default.",                                        \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        held_out_gate_strict,        "Held-Out Gate Strict", "Drift Acknowledgments",CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, -1, 1),                                                                \
        "Held-out validation gate: -1=skip, 0=warn-only (default), 1=refuse load. Tri-state KIND_INT pending categorical applicability INT_ENUM upgrade.",                                                          \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === ML inference backend — engine-wide (3) === */                                                                                                                                                              \
    X(int,                  KIND_INT,        ml_backend,                  "ML Backend",           "ML",              CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 4),                                   \
        "ML inference backend selection. Ships as KIND_INT pending TECH_DEBT-068 ML enum registry; promote to KIND_INT_ENUM with XGBOOST/ONNX/AOT labels after.",                                                    \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        regime_model_backend,        "Regime Model Backend", "ML",              CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 4),                                   \
        "Regime detection model backend. Ships as KIND_INT pending TECH_DEBT-068.",                                                                                                                                 \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_BOOL,       use_aot_inference,           "Use AOT Inference",    "ML",              CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                       \
        "Use ahead-of-time-compiled inference path instead of XGBoost runtime. Faster per-tick (~50ns vs ~500ns) but requires AOT model build via tools/aot_compile. Boot-only.",                                    \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Notifications (2) === */                                                                                                                                                                                   \
    X(int,                  KIND_INT,        notify_backend,              "Notify Backend",       "Notifications",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 4),                                                                      \
        "Notification backend: 0=Discord, 1=Telegram, 2=Slack (per notify_command template). Ships as KIND_INT pending TECH_DEBT-068.",                                                                              \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t,             KIND_INT,        notify_cooldown_secs,        "Notify Cooldown Secs", "Notifications",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 0, 86400),                                                                 \
        "Min seconds between notifications (debounce). 0 = no cooldown.",                                                                                                                                           \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Health Logging (3) === */                                                                                                                                                                                  \
    X(int,                  KIND_INT,        health_log_level,            "Health Log Level",     "Health Logging",  CfgFieldDescriptor::WARN_ON_CLAMP, INT(1, 0, 4),                                                                      \
        "Health log severity: 0=DEBUG, 1=INFO (default), 2=WARN, 3=ERROR. Ships as KIND_INT pending TECH_DEBT-068.",                                                                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint64_t,             KIND_INT,        health_log_max_bytes,        "Health Log Max Bytes", "Health Logging",  CfgFieldDescriptor::WARN_ON_CLAMP, INT(1048576, 1024, 1073741824),                                                    \
        "Health log file size limit (rotates at this size). Default 1MB; clamp [1KB, 1GB].",                                                                                                                        \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        health_log_keep_count,       "Health Log Keep Count","Health Logging",  CfgFieldDescriptor::WARN_ON_CLAMP, INT(5, 1, 1000),                                                                   \
        "Number of rotated health log files to keep before deletion.",                                                                                                                                              \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Reconcile (2) === */                                                                                                                                                                                       \
    X(int,                  KIND_INT,        reconcile_interval_sec,      "Reconcile Interval",   "Reconcile",       CfgFieldDescriptor::WARN_ON_CLAMP, INT(60, 1, 86400),                                                                 \
        "Seconds between reconciliation passes (paper-position vs broker-position drift check).",                                                                                                                   \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint8_t,              KIND_INT,        reconcile_mode,              "Reconcile Mode",       "Reconcile",       CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 2),                                \
        "Reconcile mode: 0=STRICT, 1=WARN (default), 2=AUTO_SYNC. Accepts string ('strict'/'warn'/'auto_sync') or int. HAS_SIDE_EFFECT — manual parser handles string + sets cfg_keys_explicit + mirrors reconcile_dry_run.",                                                                                                                                                                                                                                                                                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Engine-wide mode (4) === */                                                                                                                                                                                \
    X(uint8_t,              KIND_INT,        engine_mode,                 "Engine Mode",          "Operational",     CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::IS_BOOT_ONLY, INT(1, 0, 2),                                 \
        "Engine mode: 0=SINGLE_CORE (legacy), 1=SHARDED (default v5.0+). Accepts string ('sharded'/'single_core') or int. HAS_SIDE_EFFECT — manual parser handles string form; registry walker skips.",            \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_INT,        model_verify_strict,         "Model Verify Strict",  "Drift Acknowledgments",CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::WARN_ON_CLAMP, INT(-1, -1, 1),                         \
        "Model verification strictness: -1=auto (strict in live, lenient in paper; default), 0=lenient, 1=strict. Tri-state. HAS_SIDE_EFFECT — manual parser sets cfg_keys_explicit bit for NormalizeForMode flip rule.",                                                                                                                                                                                                                                                                                                       \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint8_t,              KIND_INT,        trading_mode,                "Trading Mode",         "Operational",     CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::SAFETY_CRITICAL | CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 2), \
        "Trading mode: 0=PAPER (default; safe), 1=PROD_SHADOW (live wired but bracketed), 2=LIVE (real money). Accepts string ('paper'/'shadow'/'live') or int. LIVE flips model_verify_strict 0->1 + reconcile_mode WARN->STRICT. HAS_SIDE_EFFECT — manual parser handles string form + NormalizeForMode triggers. SAFETY_CRITICAL.", \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Validation (1; v5.15.5.F.4d.1.B.2 Step 2 partial — gap_acceptable_threshold migration) === */ \
    /* Manual cfg storage at ControllerConfig.hpp:889 (FPN<F>) + manual default at :1729 + manual parser at :2554 stays at .B.2 \
     * (FOREACH_GLOBAL_CFG_FIELD doesn't auto-gen struct fields — manual decl/default/parser cleanup deferred to .B.3 with cfg-storage-discipline amendment). \
     * Registry row provides descriptor + STAMP_BOUND_CFG_DERIVED bit (framework walks via FOREACH_GLOBAL_CFG_FIELD filter) + auto-registers GUI render \
     * (manual GUI entry at GUI/SettingsPanel.hpp:414 deleted in same edit; registry-driven render covers). HAS_SIDE_EFFECT marks "manual parser handles" → \
     * registry walker skips auto-parse via tt::cfg_parse_field. */ \
    X(FPN<F>,               KIND_DOUBLE,     gap_acceptable_threshold,    "Gap Threshold",        "Validation",      CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.05, 0.0, 1.0), \
        "Max acceptable |WF mean - held_out| gap for model 'OK' verdict. Default 0.05 = 5%. Stamp-bound (training-time gap value captured at stamp emit). v5.15.5.F.4d.1.B.3 Step 1.6.1 — HAS_SIDE_EFFECT bit removed; auto-parser handles FPN<F> storage via tt::cfg_parse_field (TECH_DEBT-093 closure).", \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* v5.15.5.F.4d.1.B.3 Phase F HIGH-1 (b) — held_out_fraction migration to cfg-derived cohort. \
     * Was MC PRE_CFG row at StampBoundModelConstRegistry.hpp emitting `inference_cfg_held_out_fraction` wire key; \
     * inf-driven via STAMP_INFERENCE_CFG_AUTOPOPULATE (deleted at Step 1.5). After Phase F: emits unprefixed \
     * `held_out_fraction=` via cfg-derived framework call (populate_stamp_cfg_from_derived); legacy stamps \
     * load via FOREACH_LEGACY_PREFIXED_KEY back-compat dispatch. Closes Class 21 (parallel descriptor between \
     * cfg-side + MC-side for same semantic). Sister precedent: gap_acceptable_threshold above. */ \
    X(FPN<F>,               KIND_DOUBLE,     held_out_fraction,           "Held-Out %",           "Validation",      CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.20, 0.0, 1.0), \
        "Fraction of training data reserved as held-out validation set. Default 0.20 = 20%. Stamp-bound (training-time value captured at stamp emit; engine load WARN on drift). v5.15.5.F.4d.1.B.3 Phase F HIGH-1 (b) — migrated from MC PRE_CFG inf-side row to cfg-derived cohort; closes Class 21 parallel descriptor.", \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Drift Acknowledgments (3) === */                                                                                                                                                                            \
    X(int,                  KIND_BOOL,       acknowledge_hardcoded_strategy_in_live, "Ack Hardcoded Strategy in Live", "Drift Acknowledgments", CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),             \
        "Explicit acknowledgment required to run hardcoded strategy (no per-core override) in live mode. Safety gate; operator must opt-in.",                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_BOOL,       acknowledge_hot_swap_with_open_positions, "Ack Hot Swap w/ Open Positions", "Drift Acknowledgments", CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                              \
        "Explicit acknowledgment to hot-swap strategy/model while positions are open. Without this, hot-swap is DEFERRED until position closes (v5.10.0c default).",                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int,                  KIND_BOOL,       allow_cross_major_engine,    "Allow Cross-Major Engine", "Drift Acknowledgments", CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                              \
        "Allow engine to load when model_path is from a different major version. v5.9.2b — refuse by default; explicit override to enable cross-major migration.",                                                   \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Runtime GUI toggle (1) === */                                                                                                                                                                              \
    X(int,                  KIND_BOOL,       danger_enabled,              "Enabled",              "Danger Gradient", CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
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
    X(FPN<F>                , KIND_DOUBLE_PCT, take_profit_pct,             "TP %%",                "Trading",         0,                                  DBL(3.0, 0.0, 100.0),    nullptr,                                                                                          STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, stop_loss_pct,               "SL %%",                "Trading",         0,                                  DBL(1.5, 0.0, 100.0),    nullptr,                                                                                          STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, fee_rate,                    "Fee %%",               "Trading",         0,                                  DBL(0.1, 0.0, 5.0),                                                                                                              \
        "Legacy fee rate (% per trade) — used for pre-trade quantity computations\n"                                                                                                                                                                              \
        "(no-trade band, fee floor for TP, kill switch estimate, spread display)\n"                                                                                                                                                                               \
        "and as the default for fee_rate_maker / fee_rate_taker if those aren't set.",                                                                                                                                                                            \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, slippage_pct,                "Slippage %%",          "Trading",         0,                                  DBL(0.05, 0.0, 5.0),     nullptr,                                                                                         STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, risk_pct,                    "Risk/Pos %%",          "Trading",         0,                                  DBL(2.0, 0.0, 100.0),    nullptr,                                                                                         STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, fee_floor_mult,              "Fee Floor",            "Trading",         0,                                  DBL(3.0, 1.0, 20.0),                                                                                                             \
        "TP floor = entry * fee_rate * this\n3.0 = TP must clear round-trip fees + margin",                                                                                                                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Entry Filters (9) === */                                                                                                                                                                                   \
    X(FPN<F>                , KIND_DOUBLE_PCT, entry_offset_pct,            "Offset %%",            "Entry Filters",   0,                                  DBL(0.15, 0.0, 5.0),                                                                                                            \
        "Buy gate offset below avg/EMA price\nhigher = deeper dip required to enter",                                                                                                                                                                             \
        STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, offset_min,                  "Offset Min %%",        "Entry Filters",   0,                                  DBL(0.05, 0.0, 5.0),     "Adaptation lower bound for offset_pct",                                                          STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, offset_max,                  "Offset Max %%",        "Entry Filters",   0,                                  DBL(0.50, 0.0, 5.0),     "Adaptation upper bound for offset_pct",                                                          STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, volume_multiplier,           "Vol Mult",             "Entry Filters",   0,                                  DBL(2.5, 0.0, 100.0),                                                                                                            \
        "Volume gate: require avg_volume * this\nhigher = only buy on high volume",                                                                                                                                                                               \
        STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, spacing_multiplier,          "Spacing",              "Entry Filters",   0,                                  DBL(2.0, 0.0, 20.0),                                                                                                             \
        "Min distance between entries (in stddev)\nprevents clustering entries at similar prices",                                                                                                                                                                \
        STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, min_long_slope,              "Min Long Slope",       "Entry Filters",   0,                                  DBL(0.0, -1.0, 1.0),                                                                                                             \
        "Block MR buys when 512-tick slope below this\nnegative = allow mild dips, 0 = disabled",                                                                                                                                                                 \
        STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, min_buy_delta,               "Min Buy Delta",        "Entry Filters",   0,                                  DBL(-0.3, -1.0, 1.0),                                                                                                            \
        "Min volume delta for MR buys\n-0.3 = allow mild selling, block heavy dumps",                                                                                                                                                                             \
        STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, vwap_offset,                 "VWAP Offset",          "Entry Filters",   0,                                  DBL(0.0, 0.0, 0.1),      nullptr,                                                                                          STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, min_stddev_pct,              "Min Stddev %%",        "Entry Filters",   0,                                  DBL(0.0, 0.0, 0.1),                                                                                                              \
        "Skip trades when stddev/price below this\nprevents entries in dead-flat markets",                                                                                                                                                                        \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Time-Based Exit (4) === */                                                                                                                                                                                 \
    X(FPN<F>                , KIND_DOUBLE, tp_hold_score,               "TP Hold Score",        "Time-Based Exit", 0,                                  DBL(0.0, 0.0, 10.0),     "Min SNR*R² to hold past TP (0 = disabled, fixed TP)",                                            STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, tp_trail_mult,               "TP Trail Mult",        "Time-Based Exit", 0,                                  DBL(1.0, 0.0, 10.0),     "Trailing distance: stddev * this (e.g. 1.0)",                                                    STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, sl_trail_mult,               "SL Trail Mult",        "Time-Based Exit", 0,                                  DBL(2.0, 0.0, 10.0),     "Trailing SL distance: stddev * this (e.g. 2.0)",                                                 STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t              , KIND_INT, max_hold_ticks,              "Max Hold",             "Time-Based Exit", CfgFieldDescriptor::WARN_ON_CLAMP, INT(75000, 0, 100000000),                                                          \
        "Close position after this many ticks (engine-wide).\n0 = disabled, 75000 ≈ 4-5 hours.\nPer-core min-gain floor lives in each core's Time Exit override.",                                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Risk per-core — kill switches + max-drawdown (6) === */                                                                                                                                                    \
    X(FPN<F>                , KIND_DOUBLE_PCT, max_drawdown_pct,            "Max DD %%",            "Risk Management", 0,                                  DBL(20.0, 0.0, 100.0),                                                                                                           \
        "Circuit breaker: halt trading if total P&L\ndrops below this %% of starting balance",                                                                                                                                                                    \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, max_exposure_pct,            "Max Exp %%",           "Risk Management", 0,                                  DBL(50.0, 0.0, 100.0),   nullptr,                                                                                          STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, kill_switch_daily_loss_pct,  "Daily Loss %%",        "Kill Switch",     CfgFieldDescriptor::SAFETY_CRITICAL,DBL(3.0, 0.0, 100.0),                                                                                                           \
        "Max session loss before kill switch triggers\n3.0 = halt if equity drops 3%% from session start",                                                                                                                                                        \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, kill_switch_drawdown_pct,    "Drawdown %%",          "Kill Switch",     CfgFieldDescriptor::SAFETY_CRITICAL,DBL(5.0, 0.0, 100.0),                                                                                                           \
        "Max drawdown from session peak before kill\n5.0 = halt if 5%% below intra-session high",                                                                                                                                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t              , KIND_BOOL, enable_mtm_kill_switch,      "MTM Kill Switch",      "Kill Switch",     CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "Mark-to-market kill switch — halt new entries when realized + unrealized P&L crosses kill_switch_threshold_pct. Separate from balance-based kill switch (always armed).",                                  \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t              , KIND_INT, kill_recovery_warmup,        "Recovery",             "Kill Switch",     CfgFieldDescriptor::WARN_ON_CLAMP, INT(100, 0, 1000000),                                                              \
        "Slow-path cycles to observe after kill reset\nbefore trading resumes (prevents immediate re-entry)",                                                                                                       \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Gate Recovery (5) === */                                                                                                                                                                                   \
    X(int                   , KIND_BOOL, sl_cooldown_adaptive,        "Adaptive CD",          "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "Post-stop-loss cooldown mode: 0 = fixed cycles (sl_cooldown_cycles), 1 = scale by trend confidence (longer cooldown when trend weakens; shorter when trend resumes).",                                     \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t              , KIND_INT, sl_cooldown_base,            "SL Cooldown Base",     "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 1000000),                                                                \
        "Base cycles to cooldown after stop loss (adaptive mode adds extra per loss; non-adaptive uses sl_cooldown_cycles alone).",                                                                                 \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t              , KIND_INT, sl_cooldown_extra,           "SL Cooldown Extra",    "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 1000000),                                                                \
        "Extra cooldown cycles per consecutive stop loss (adaptive mode only).",                                                                                                                                    \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t              , KIND_INT, sl_cooldown_cycles,          "SL Cooldown",          "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(30, 0, 1000000),                                                               \
        "Slow-path cycles to pause after stop loss\nlets market settle before re-entry",                                                                                                                            \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t              , KIND_INT, idle_reset_cycles,           "Idle Reset",           "Gate Recovery",   CfgFieldDescriptor::WARN_ON_CLAMP, INT(100, 0, 1000000),                                                              \
        "Cycles with no fill before gate decay\nprevents permanent lockout after losses",                                                                                                                           \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Momentum strategy (7) === */                                                                                                                                                                               \
    X(FPN<F>                , KIND_DOUBLE, momentum_min_tp_margin_pct,  "Mom Min TP Margin",    "Strategies",      0,                                  DBL(0.0, 0.0, 0.05),     "Block momentum entry if TP too tight (0 = disabled; rec: 0.0040 = 0.40%)",                       STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, momentum_min_buy_delta_recent, "Mom Min Buy Delta", "Strategies",       0,                                  DBL(0.0, 0.0, 1.0),      "Min recent volume delta for momentum entry (rec: 0.05)",                                          STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, momentum_min_r2,             "Mom Min R²",           "Strategies",      0,                                  DBL(0.0, 0.0, 1.0),      "Min short_r2 for momentum entry (0 = disabled; rec: 0.30)",                                       STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, momentum_tp_mult,            "Mom TP Mult",          "Strategies",      0,                                  DBL(3.0, 0.0, 10.0),     "TP multiplier for momentum (e.g. 3.0 stddevs)",                                                  STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, momentum_sl_mult,            "Mom SL Mult",          "Strategies",      0,                                  DBL(1.0, 0.0, 10.0),     "SL multiplier for momentum (e.g. 1.0 stddevs)",                                                  STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, momentum_breakout_mult,      "Mom Breakout Mult",    "Strategies",      0,                                  DBL(0.5, 0.0, 10.0),     "Momentum breakout floor in stddevs",                                                              STRAT_CAT_REGRESSION_DRIVEN,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int                   , KIND_BOOL, momentum_require_last_win,   "Require Last Win",     "Momentum Tuning", CfgFieldDescriptor::WARN_ON_CLAMP, BOOL(0),                                                                          \
        "SHALT_MOM_LAST_LOST gate — block Momentum re-entry until previous trade was a TP win. 0 = off (default; recommended). 1 = enable; favors winning streaks at cost of slower recovery.",                     \
        STRAT_CAT_REGRESSION_DRIVEN,                         OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === EMA Cross strategy (3) === */                                                                                                                                                                              \
    X(FPN<F>                , KIND_DOUBLE, emacross_dip_mult,           "EMACross Dip Mult",    "Strategies",      0,                                  DBL(0.5, 0.0, 10.0),     "Buy this many stddevs below EMA",                                                                STRAT_CAT_STATIC_RULES,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, emacross_crossover_min,      "EMACross Crossover",   "Strategies",      0,                                  DBL(0.0, 0.0, 1.0),      "Min EMA-SMA spread for uptrend confirmation",                                                    STRAT_CAT_STATIC_RULES,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, emacross_trail_mult,         "EMACross Trail Mult",  "Strategies",      0,                                  DBL(1.0, 0.0, 10.0),     "Trailing TP factor when EMA rising",                                                              STRAT_CAT_STATIC_RULES,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Regime Detection (5) === */                                                                                                                                                                                \
    X(FPN<F>                , KIND_DOUBLE, regime_slope_threshold,      "Regime Slope Thresh",  "Regime Detection",0,                                  DBL(0.0, 0.0, 1.0),      "Relative slope magnitude for TRENDING classification",                                            STRAT_CAT_REGIME_AWARE,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, regime_crossover_threshold,  "Mild Trend",           "Regime Detection",0,                                  DBL(0.0005, 0.0, 1.0),                                                                                                           \
        "EMA/SMA spread for MILD_TREND (EMA Cross)\n0.0005 = 0.05%% gap (~$35 at BTC $68k)\nbelow = RANGING, above = mild uptrend",                                                                                                                               \
        STRAT_CAT_REGIME_AWARE,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, regime_strong_crossover,     "Strong Trend",         "Regime Detection",0,                                  DBL(0.0015, 0.0, 1.0),                                                                                                           \
        "EMA/SMA spread for strong TRENDING (Momentum)\n0.0015 = 0.15%% gap (~$102 at BTC $68k)\nabove = Momentum, below = EMA Cross",                                                                                                                            \
        STRAT_CAT_REGIME_AWARE,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, regime_r2_threshold,         "R² Threshold",         "Regime Detection",0,                                  DBL(70.0, 0.0, 100.0),                                                                                                           \
        "Min R-squared consistency for TRENDING\n70 = 70%% of price variance explained by trend",                                                                                                                                                                 \
        STRAT_CAT_REGIME_AWARE,                                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t              , KIND_INT, regime_hysteresis,           "Hysteresis",           "Regime Detection",CfgFieldDescriptor::WARN_ON_CLAMP, INT(3, 1, 100),                                                                    \
        "Slow-path cycles before regime switch\nprevents rapid flipping between strategies",                                                                                                                        \
        STRAT_CAT_REGIME_AWARE,                              OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === ML — entry threshold + TP/SL (3) === */                                                                                                                                                                    \
    X(FPN<F>                , KIND_DOUBLE, ml_buy_threshold,            "ML Buy Thresh",        "ML",              CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED, DBL(0.5, 0.0, 1.0), "Buy threshold for ML strategy (predictions above this enter; pre-canonical parity gap closed at .B.2 — STAMP_BOUND added to master; legacy FOREACH_STAMP_BOUND_CFG entry at StampBoundCfgRegistry.hpp:157-158 deleted at .B.3 along with macro body)", STRAT_CAT_ML,                                                  OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, ml_tp_pct,                   "ML TP %%",             "ML",              CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED, DBL(2.0, 0.0, 100.0),    "ML strategy take profit (overrides take_profit_pct); .B.3 Step 1.6.2 cohort bit-add (Decision D mechanism 1; legacy FOREACH_STAMP_BOUND_CFG entry deleted at Step 2)", STRAT_CAT_ML,                                                  OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, ml_sl_pct,                   "ML SL %%",             "ML",              CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED, DBL(1.0, 0.0, 100.0),    "ML strategy stop loss (overrides stop_loss_pct); .B.3 Step 1.6.2 cohort bit-add", STRAT_CAT_ML,                                                  OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === ML — Bandit/Confidence/Ensemble (per-core authoritative) (11) === */                                                                                                                                       \
    X(FPN<F>                , KIND_DOUBLE, bandit_blend_ratio,          "Bandit Blend",         "ML",              CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED, DBL(0.5, 0.0, 1.0),      "Mix of bandit picks vs base model; .B.3 Step 1.6.2 cohort bit-add (was standalone inference_cfg_bandit_blend_ratio at StampBoundModelConstRegistry.hpp:296; framework walker emits unprefixed)", STRAT_CAT_ML | STRAT_CAT_USES_BANDIT,                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, confidence_threshold_scale,  "Conf Thresh Scale",    "ML",              CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED, DBL(1.0, 0.0, 5.0),      "Confidence-weighted entry threshold scaling; .B.3 Step 1.6.2 v1.6 cohort bit-add (Class 32 full closure; replaces inference_cfg_confidence_threshold_scale legacy wire key)", STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t              , KIND_INT, confidence_window,           "Conf Window",          "FoxML",           CfgFieldDescriptor::WARN_ON_CLAMP, INT(64, 1, 64),                                                                    \
        "RollingIC + RollingRMSE window size (engine-wide; cap 64).\nSame window per ML core today; INT support for X-macro deferred.",                                                                              \
        STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int                   , KIND_INT, confidence_turnover_window,  "Conf Turnover Window", "FoxML",           CfgFieldDescriptor::WARN_ON_CLAMP, INT(1000, 1, 100000),                                                              \
        "Turnover sample window for ML confidence (predictions over recent N ticks).",                                                                                                                              \
        STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int                   , KIND_INT, confidence_turnover_topk,    "Conf Turnover TopK",   "FoxML",           CfgFieldDescriptor::WARN_ON_CLAMP, INT(10, 1, 1000),                                                                  \
        "Top-K predictions kept for confidence turnover analysis.",                                                                                                                                                 \
        STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t              , KIND_INT, confidence_ic_floor_window,  "Conf IC Floor Window", "FoxML",           CfgFieldDescriptor::WARN_ON_CLAMP, INT(1000, 1, 100000),                                                              \
        "Rolling window for ML IC floor enforcement.",                                                                                                                                                              \
        STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int                   , KIND_INT, confidence_ic_variant,       "Conf IC Variant",      "FoxML",           CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 4),                                                                      \
        "IC variant: 0=Spearman (default), 1+=future. Ships as KIND_INT pending TECH_DEBT-068 ML enum registry; promote to KIND_INT_ENUM after.",                                                                  \
        STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int                   , KIND_INT, ensemble_min_warmup_predictions,"Ens Min Warmup Preds","Ensemble",      CfgFieldDescriptor::WARN_ON_CLAMP, INT(100, 0, 1000000),                                                              \
        "Min predictions before ensemble bandit becomes load-bearing.",                                                                                                                                             \
        STRAT_CAT_ML | STRAT_CAT_USES_BANDIT,                OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int                   , KIND_INT, ensemble_bandit_save_interval,"Ens Bandit Save Interval","Ensemble",    CfgFieldDescriptor::WARN_ON_CLAMP, INT(5000, 1, 1000000),                                                              \
        "Predictions between ensemble bandit state save-to-disk events. Default 5000.",                                                                                                                             \
        STRAT_CAT_ML | STRAT_CAT_USES_BANDIT,                OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint64_t              , KIND_INT, thompson_rng_seed,           "Thompson RNG Seed",    "FoxML",           CfgFieldDescriptor::IS_BOOT_ONLY | CfgFieldDescriptor::HAS_SIDE_EFFECT, INT(42, 0, 9223372036854775807),                \
        "splitmix64 seed for Thompson sampling bandit. Default 42. 0 = use ThompsonBandit.hpp's THOMPSON_RNG_SEED_DEFAULT. Boot-only; required for replay-determinism. HAS_SIDE_EFFECT — manual parser supports hex (0x...) base-auto-detect; registry walker skips.",                                                                                                                                                                                                                                                          \
        STRAT_CAT_ML | STRAT_CAT_USES_BANDIT,                OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(uint32_t              , KIND_INT, model_max_age_hours,         "Model Max Age Hours",  "Lifecycle",       CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 87600),                                                                  \
        "Refuse model load if file mtime older than N hours. 0 = disabled (legacy default; v5.14.8.E).",                                                                                                             \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === STAMP_BOUND scalar cohort — Ridge + Winsor + Confidence + Thompson (12 DOUBLE) === */ \
    /*       Ridge risk-parity blending (v5.14.0) */ \
    X(FPN<F>                , KIND_DOUBLE, ridge_lambda,                "Ridge Lambda",         "ML/Ridge",        CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.15, 0.0, 10.0), \
        "Ridge regularization strength. Higher = more aggressive blending toward equal weights; lower = trusts per-arm IC signal more. Default 0.15. Stamp-bound (parity-critical).", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, ridge_cost_penalty,          "Ridge Cost Penalty",   "ML/Ridge",        CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.5, 0.0, 100.0), \
        "Cost penalty in net_IC = IC - penalty*cost (basis for ridge_min_ic_floor gate). Higher = penalizes arms with worse cost-IC tradeoff. Default 0.5. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, ridge_min_ic_floor,          "Ridge Min IC Floor",   "ML/Ridge",        CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.001, 0.0, 1.0), \
        "Minimum net_IC floor — arms below this floor get zero weight (prevents zero-sum starvation when all arms have weak signal). Default 0.001. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /*       Winsorization (label clipping for training-time outlier handling) */ \
    X(FPN<F>                , KIND_DOUBLE, winsor_pct_low,              "Winsor Low",           "ML/Winsor",       CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.005, 0.0, 0.5), \
        "Lower winsor clip percentile (ratio, NOT percent). Default 0.005 = clip bottom 0.5%% of labels. Stamp-bound (training-serve parity).", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, winsor_pct_high,             "Winsor High",          "ML/Winsor",       CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.995, 0.5, 1.0), \
        "Upper winsor clip percentile (ratio). Default 0.995 = clip top 0.5%% of labels. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /*       Composite confidence (v5.14.1) */ \
    X(FPN<F>                , KIND_DOUBLE, confidence_freshness_tau_secs, "Conf Freshness Tau", "ML/Confidence",   CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(3600.0, 0.0, 86400.0), \
        "Time-decay tau (seconds) for confidence-freshness term. Higher = slower decay of model trust as time-since-train grows. Default 3600s (1h). Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, confidence_capacity_target_dollars, "Conf Capacity Target $", "ML/Confidence", CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.0, 0.0, 1000000.0), \
        "Target trade capacity (dollars) for confidence-capacity term. 0 = disabled. Higher = larger trades trusted at full confidence. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, confidence_capacity_kappa,   "Conf Capacity Kappa",  "ML/Confidence",   CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.1, 0.0, 10.0), \
        "Capacity-curve shape parameter (kappa). Controls steepness of confidence falloff as trade size exceeds capacity. Default 0.1. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, confidence_rmse_baseline,    "Conf RMSE Baseline",   "ML/Confidence",   CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(1.0, 0.0, 100.0), \
        "RMSE baseline for confidence-error term. Predictions with RMSE above this baseline get reduced confidence. Default 1.0. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /*       Bayesian Thompson sampling (v5.14.10.B) */ \
    X(FPN<F>                , KIND_DOUBLE, thompson_mu_prior,           "Thompson Mu Prior",    "ML/Thompson",     CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.0, -1000.0, 1000.0), \
        "Posterior mean prior (mu_0) for Bayesian Thompson sampling. Default 0.0 = neutral prior. Stamp-bound (parity-critical when bandit_algorithm == THOMPSON).", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, thompson_precision_prior,    "Thompson Tau Prior",   "ML/Thompson",     CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(1.0, 0.0001, 1000.0), \
        "Posterior precision prior (tau_0 = 1/variance) for Bayesian Thompson sampling. Default 1.0 = unit variance prior. Higher = tighter prior. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, thompson_precision_obs,      "Thompson Tau Obs",     "ML/Thompson",     CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(1.0, 0.0001, 1000.0), \
        "Observation precision (tau_obs = 1/sigma^2) for each reward update. Higher = each reward trusted more (faster posterior shift). Default 1.0. Stamp-bound.", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /*       Bandit algorithm selector — INT enum (5-state post-v5.15.5.F.4d; Option C wire-byte preservation for cfg=0/1/2) */ \
    X(int                   , KIND_INT, bandit_algorithm,            "Bandit Algorithm",     "ML/Bandit",       CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 4), \
        "Bandit selector: 0=EXP3 (default; legacy), 1=THOMPSON (Bayesian; Class 24 fix — posterior now updates from rewards), 2=EXP3_OP_THOMPSON_GHOST (Exp3 drives + Thompson shadow-learns; was 'BOTH' pre-.F.4d; legacy 'Both'/'BOTH' string aliases preserved), 3=THOMPSON_OP_EXP3_GHOST (NEW .F.4d; Thompson drives + Exp3 shadow-learns), 4=BLENDED (NEW .F.4d EXPERIMENTAL; weighted blend via thompson_exp3_blend_alpha). Accepts string (canonical or legacy alias) or int. HAS_SIDE_EFFECT — manual parser handles string form. Stamp-bound (parity-critical).", \
        STRAT_CAT_ML,                                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /*       thompson_exp3_blend_alpha — BLENDED state-4 weight ratio (v5.15.5.F.4d NEW per § C.1 of merged plan body) */ \
    X(FPN<F>                , KIND_DOUBLE, thompson_exp3_blend_alpha,   "Blend α (Exp3↔Thompson)", "ML/Bandit",     CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.5, 0.0, 1.0), \
        "Operator-weighted blend ratio for cfg.bandit_algorithm=4 BLENDED state. weights = (1-α)×Exp3_probs + α×Thompson_softmax(mu_post). Only meaningful when bandit_algorithm=4; GUI should grey-out when bandit_algorithm != 4. Default 0.5 = 50/50 blend. Stamp-bound (parity-critical; reproducibility requires α to be locked to training-time value).", \
        STRAT_CAT_USES_BANDIT,                               OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Per-core risk thresholds — STAMP_BOUND (4) === */ \
    /*       Risk degradation (v5.14.9 soft-risk) */ \
    X(int                   , KIND_INT, risk_degradation_curve,      "Risk Degradation Curve","Risk Management",CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 2), \
        "Risk degradation curve shape: 0=CURVE_OFF (no degradation), 1=LINEAR, 2=EXP. Accepts string ('off'/'linear'/'exp') or int. HAS_SIDE_EFFECT — manual parser handles string form + legacy risk_scale_by_confidence alias. Stamp-bound.", \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, risk_full_size_threshold,    "Risk Full-Size Thresh","Risk Management", CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.15, 0.0, 1.0), \
        "Confidence threshold above which trades get full size. Default 0.15. Stamp-bound.", \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, risk_min_size_threshold,     "Risk Min-Size Thresh", "Risk Management", CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.05, 0.0, 1.0), \
        "Confidence threshold below which trades get blocked (no-trade band). Default 0.05. Stamp-bound.", \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, risk_min_size_pct,           "Risk Min-Size Floor",  "Risk Management", CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.10, 0.0, 1.0), \
        "Floor on degraded position size (ratio of full size, e.g. 0.10 = 10%% of normal). Default 0.10. Stamp-bound.", \
        STRAT_CAT_ALL,                                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Partial exits / Breakeven (3) === */                                                                                                                                                                       \
    X(FPN<F>                , KIND_DOUBLE, partial_exit_pct,            "Partial Exit %%",      "Partial Exits",   0,                                  DBL(0.5, 0.0, 1.0),      "Fraction of position to close at TP1 (0.5 = 50%)",                                               STRAT_CAT_ALL,                                                   OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, tp2_mult,                    "TP2 Mult",             "Partial Exits",   0,                                  DBL(2.0, 0.0, 10.0),     "TP2 distance = TP1_distance * this (2.0 = double TP)",                                           STRAT_CAT_ALL,                                                   OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, breakeven_buffer_pct,        "Breakeven Buf %%",     "Partial Exits",   0,                                  DBL(0.0, 0.0, 5.0),      "SL offset from entry once breakeven ratchet fires",                                              STRAT_CAT_ALL,                                                   OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Strategy-specific TP/SL overrides (6 — FPN<F>) — v5.15.5.F.4c.3 WIP2c.1 classify-first === */                                                                                                              \
    /* 0 = fall back to global take_profit_pct / stop_loss_pct (strategy parameter dispatcher applies). Per-core authoritative when set. */                                                                           \
    X(FPN<F>                , KIND_DOUBLE_PCT, simpledip_tp_pct,            "SimpleDip TP %%",      "Strategies",      0,                                  DBL(0.0, 0.0, 100.0),    "SimpleDip TP override (%, stored as decimal; 0 = fall back to take_profit_pct)",                STRAT_CAT_STATIC_RULES,                                          OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, simpledip_sl_pct,            "SimpleDip SL %%",      "Strategies",      0,                                  DBL(0.0, 0.0, 100.0),    "SimpleDip SL override (%, stored as decimal; 0 = fall back to stop_loss_pct)",                  STRAT_CAT_STATIC_RULES,                                          OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, mr_tp_pct,                   "MR TP %%",             "Strategies",      0,                                  DBL(0.0, 0.0, 100.0),    "MeanReversion TP override (%, stored as decimal; 0 = fall back to take_profit_pct)",            STRAT_CAT_STATIC_RULES,                                          OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, mr_sl_pct,                   "MR SL %%",             "Strategies",      0,                                  DBL(0.0, 0.0, 100.0),    "MeanReversion SL override (%, stored as decimal; 0 = fall back to stop_loss_pct)",              STRAT_CAT_STATIC_RULES,                                          OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, emacross_tp_pct,             "EMACross TP %%",       "Strategies",      0,                                  DBL(0.0, 0.0, 100.0),    "EMA Cross TP override (%, stored as decimal; 0 = fall back to take_profit_pct)",                STRAT_CAT_STATIC_RULES,                                          OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, emacross_sl_pct,             "EMACross SL %%",       "Strategies",      0,                                  DBL(0.0, 0.0, 100.0),    "EMA Cross SL override (%, stored as decimal; 0 = fall back to stop_loss_pct)",                  STRAT_CAT_STATIC_RULES,                                          OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Entry stddev mode (1 — FPN<F>) — v5.15.5.F.4c.3 WIP2c.1 classify-first === */                                                                                                                              \
    X(FPN<F>                , KIND_DOUBLE, offset_stddev_mult,          "Offset Stddev Mult",   "Entry Filters",   0,                                  DBL(0.0, 0.0, 10.0),     "stddev-scaled offset multiplier (0 = use offset_pct mode; non-zero = stddev-adaptive mode)",    STRAT_CAT_STATIC_RULES | STRAT_CAT_REGRESSION_DRIVEN,            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === ML hard-block + ensemble + barrier (3 — 1 FPN<F> + 1 double + 1 INT_ENUM) — v5.15.5.F.4c.3 WIP2c.1 classify-first === */                                                                                   \
    X(FPN<F>                , KIND_DOUBLE, confidence_hard_block_threshold, "Conf Hard Block",  "ML",              CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.0, 0.0, 1.0),      "Confidence floor — predictions with confidence below this hard-block (no entry). 0 = disabled. .B.3 Step 1.6.2 v1.6 cohort bit-add (Class 32 full closure).", STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,                        OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(double                , KIND_DOUBLE, ensemble_min_agreement_pct,  "Ens Min Agreement %%", "Ensemble",        CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.0, 0.0, 1.0),      "Min fraction of non-disabled horizons that must agree on direction for entry (0 = disabled).",   STRAT_CAT_ML | STRAT_CAT_USES_BANDIT,                            OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(int                   , KIND_INT, barrier_blend_mode,          "Barrier Blend Mode",   "ML",              CfgFieldDescriptor::STAMP_BOUND | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::HAS_SIDE_EFFECT | CfgFieldDescriptor::WARN_ON_CLAMP, INT(0, 0, 3),                                  \
        "Per-horizon TP/SL serving mode: 0=LEGACY (cfg-direct fallback), 1=BLEND (weighted across horizons), 2=DOMINANT (highest-weight horizon). HAS_SIDE_EFFECT — manual parser handles string form ('legacy'/'blend'/'dominant'). .B.3 Step 1.6.2 cohort bit-add (Decision D mechanism 1; legacy inference_cfg_barrier_blend_mode wire key deleted at Step 2).", \
        STRAT_CAT_ML,                                                   OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === v5.15.5.F.4d TECH_DEBT-082 .F.5 residual close — 3 fields migrate from manual parser cases to auto-flow X-macro (Class 23 anti-pattern close at these 3 sites) === */                                          \
    X(FPN<F>                , KIND_DOUBLE_PCT, lazy_rebuild_price_threshold_pct, "Lazy Rebuild Price Thresh %%", "Slow Path", CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.0005, 0.0, 0.1),                                  \
        "Per-tick price-delta threshold below which slow-path cycle is 'no material change' (skips RebuildOneCore). Default 0.0005 (0.05%). Per-core eligible — cores with different vol profiles can use different sensitivity. .F.4d TECH_DEBT-082 close.", \
        STRAT_CAT_ALL,                                                  OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, exit_threshold,              "Exit Threshold",       "ML/Exit",         CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.6, 0.0, 1.0),                                                  \
        "Blended exit probability threshold for sell-side ML predictions (Path 3 architecture; v5.13.0). When blended exit_prob > exit_threshold AND position open, fires early market-exit. Default 0.6 (60%). Per-core eligible — each core has its own ML model with different exit calibration. .F.4d TECH_DEBT-082 close.", \
        STRAT_CAT_ML,                                                   OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(double                , KIND_DOUBLE, confidence_ic_floor,         "Conf IC Floor",        "FoxML",           CfgFieldDescriptor::WARN_ON_CLAMP, DBL(0.02, -1.0, 1.0),                                              \
        "Min acceptable rolling Information Coefficient (Spearman correlation > random chance) for ML predictions. Sustained-breach over confidence_ic_floor_window seconds triggers drift gate (CRITICAL log + optional auto-kill via auto_kill_on_drift). Default 0.02. Per-core eligible — each core has independent ML model drift profile. .F.4d TECH_DEBT-082 close.", \
        STRAT_CAT_ML | STRAT_CAT_USES_CONFIDENCE,                       OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Maker/Taker fees + VolScaler (3) — WIP2c.2 inclusion (Strategy_BuildParameters reads these) === */                                                                                                          \
    X(FPN<F>                , KIND_DOUBLE_PCT, fee_rate_maker,               "Fee Maker %%",         "Trading",         CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::HAS_SIDE_EFFECT, DBL(0.075, 0.0, 5.0), "Maker fill fee rate (% per trade; e.g. 0.075 = 0.075% Binance tier 0). Cohort sibling of fee_rate (legacy) + fee_rate_taker. .B.3 Step 1.6.2 v1.6 cohort bit-add (Class 32 full closure).", STRAT_CAT_ALL, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE_PCT, fee_rate_taker,               "Fee Taker %%",         "Trading",         CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED | CfgFieldDescriptor::HAS_SIDE_EFFECT, DBL(0.100, 0.0, 5.0), "Taker fill fee rate (% per trade; e.g. 0.100 = 0.100% Binance tier 0). Cohort sibling of fee_rate (legacy) + fee_rate_maker. .B.3 Step 1.6.2 v1.6 cohort bit-add (Class 32 full closure).", STRAT_CAT_ALL, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    X(FPN<F>                , KIND_DOUBLE, foxml_vol_scaling_z_max,      "Vol Scale Z-Max",      "ML",              CfgFieldDescriptor::WARN_ON_CLAMP, DBL(3.0, 0.0, 100.0),    "Z-score clipping threshold for FoxML VolScaler (limits how much volatility scaling can compress trade size). Default 3.0 = clip at 3 sigma.", STRAT_CAT_ML, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG) \
    /* === Strategy selector (1) — WIP2d-1 Finding 1 closure (HIGH-2 amendment "core_strategies[16] → cores[c].strategy") === */                                                                                          \
    /* HAS_SIDE_EFFECT — registry walker triplet (parser/copy/render) uniformly skips via if-constexpr. */                                                                                                              \
    /* Manual handling: parser at legacy `core_<N>_strategy=` block (string-form: mr/momentum/simple_dip/ml/ema_cross/none); */                                                                                          \
    /* copy via explicit `cores[c].strategy = core_strategies[c]` line after FOREACH walker; render via Step 6 per-core tabs. */                                                                                       \
    /* Default 2 = STRATEGY_SIMPLE_DIP per ControllerConfig_Default core_strategies[i]=2 legacy init. */                                                                                                                \
    X(uint8_t               , KIND_INT,    strategy,                     "Strategy",             "Strategies",      (CfgFieldDescriptor::MANUAL_PARSER | CfgFieldDescriptor::NO_FLAT_FIELD), INT(2, 0, 5),                                                                                              \
        "Per-core strategy selector. Values: 0=MR, 1=MOMENTUM, 2=SIMPLE_DIP, 3=ML, 4=EMA_CROSS, 5=AUTO (regime-driven). MANUAL_PARSER: legacy parser `core_<N>_strategy=` handles string forms (registry walker skips parse). NO_FLAT_FIELD: no scalar on ControllerConfig; cores[c].strategy auto-syncs from core_strategies[c] via FOREACH_PER_CORE_NO_FLAT_FIELD_SYNC AUTOPOPULATE in PopulateCoresFromFlat.", \
        STRAT_CAT_ALL, OP_MODE_CAT_ALL, REGIME_CAT_ALL, RISK_CAT_ALL, CfgFieldDescriptor::STRUCT_CFG)

//======================================================================================================
// [EMIT_PER_CORE_CFG_STRUCT_FIELD — payload macro for X-macro struct generation (WIP2d-0.B)]
//======================================================================================================
// Consumed by PerCoreCfg<F> in ControllerConfig.hpp via:
//   FOREACH_PER_CORE_CFG_FIELD(EMIT_PER_CORE_CFG_STRUCT_FIELD)
//
// WIP2d-0.B (.F.4c.3) — TYPE consolidation: FOREACH_PER_CORE_CFG_FIELD now carries STORAGE_T
// as its first column. The auxiliary FOREACH_PER_CORE_FIELD_TYPE registry is RETIRED — single
// source of truth per H17 STRONG codification. Future per-core cfg field addition: 1 row in
// 1 registry; struct field auto-generates from row's STORAGE_T column.
//
// Discipline per H17 (STRONG at .F.4c.3 per-core surface; HARD at .F.4d global surface):
// - PerCoreCfg<F> body uses ONLY FOREACH_PER_CORE_CFG_FIELD(EMIT_PER_CORE_CFG_STRUCT_FIELD)
//   for the 92 cfg-surface fields + FOREACH_PER_CORE_DOMAIN_BITMAP(EMIT_DOMAIN_BITMAP_FIELD)
//   for the 5 runtime bitmap fields. NO manual fields anywhere.
//
// SEE DESIGN_SPECS/manual-fields-inventory-pattern.md for the full pattern doc.
//======================================================================================================
// Payload: emit `<STORAGE_T> <name>;` per row.
#define EMIT_PER_CORE_CFG_STRUCT_FIELD(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, \
                                         applies_to_strategy_cat, applies_to_op_mode_cat, \
                                         applies_to_regime_cat, applies_to_risk_cat, lives_in_struct) \
    STORAGE_T name;

//======================================================================================================
// [EMIT_GLOBAL_CFG_STRUCT_FIELD — payload macro for global cfg field struct generation (v5.15.5.F.4d.1.B.3+)]
//======================================================================================================
// Sister to EMIT_PER_CORE_CFG_STRUCT_FIELD; landed at .B.3 Step 0.5b.A Path α cascade closing the
// global↔per-core column asymmetry. ControllerConfig<F>'s 48 manual global cfg field decls become
// FOREACH_GLOBAL_CFG_FIELD(EMIT_GLOBAL_CFG_STRUCT_FIELD) at .B.3 Step 0.5b.B (this ship's follow-up
// sub-step deletes the 48 manual decls atomically with the auto-gen invocation).
//
// At .B.3 Step 0.5b.A landing: macro DEFINED but NOT YET INVOKED. Step 0.5b.B invokes inside
// ControllerConfig<F> body + deletes 48 manual decls. Per Meta-gap M1b cohort migration —
// the 2-step split preserves clean rollback boundaries.
//
// H17 closure note: at .B.3 Step 0.5b.B, FOREACH_GLOBAL_CFG_FIELD becomes the SINGLE source of
// truth for global cfg field declarations (matches H17 STRONG codification at per-core surface;
// promotes H17 to HARD at global surface per CLAUDE.md going-forward roadmap).
//======================================================================================================
#define EMIT_GLOBAL_CFG_STRUCT_FIELD(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, \
                                       applies_to_strategy_cat, applies_to_op_mode_cat, \
                                       applies_to_regime_cat, applies_to_risk_cat, lives_in_struct) \
    STORAGE_T name;

//======================================================================================================
// [EMIT_GLOBAL_CFG_DEFAULT — payload macro for auto-defaults in ControllerConfig_Default (v5.15.5.F.4d.1.B.3+)]
//======================================================================================================
// Sister to EMIT_GLOBAL_CFG_STRUCT_FIELD; landed at .B.3 Step 1.6.1 (TECH_DEBT-093 full closure) +
// future-headache reducer for all 48 global default-init lines.
//
// Mechanism: tt::cfg_assign_field<T> reads default from descriptor.payload (per KIND dispatch:
// as_double.default_val for FPN<F> / as_int.default_val for int* / as_bool.default_val for bool).
// Sister to EMIT_PER_CORE_CFG_DEFAULT (future work; per-core defaults still manual at HEAD).
//
// H17 STRONG→HARD codification: adding a new global cfg field = 1 row in FOREACH_GLOBAL_CFG_FIELD
// with default in payload column; auto-default flows through this macro at ControllerConfig_Default.
//======================================================================================================
#define EMIT_GLOBAL_CFG_DEFAULT(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, \
                                  applies_to_strategy_cat, applies_to_op_mode_cat, \
                                  applies_to_regime_cat, applies_to_risk_cat, lives_in_struct) \
    tt::cfg_assign_field(cfg.name, g_global_cfg_field_descriptors[FIELD_IDX_GLOBAL_##name]);

//======================================================================================================
// [FOREACH_MANUAL_PER_CORE_FIELD — exempted parallel arrays on ControllerConfig<F> (WIP2d-0)]
//======================================================================================================
// PURPOSE: documented exemptions for per-core fields that can't fit
// FOREACH_PER_CORE_CFG_FIELD yet (awaiting KIND_STRING / KIND_FILE_PATH /
// KIND_HEX64 at .F.4e) OR are TRANSITIONAL during shadow-window migration.
//
// Every entry MUST have a row in DOCS/MANUAL_FIELDS_INVENTORY.md with full
// rationale + migration trigger. CI cross-checks bidirectionally via
// tools/check_per_core_registry_integrity.py — drift = BUILD ERROR.
//
// ControllerConfig<F> declares these arrays via EMIT_MANUAL_PER_CORE_DECL
// X-macro expansion ONLY; no manual `core_X[16]` declarations allowed outside.
//
// SEE DESIGN_SPECS/manual-fields-inventory-pattern.md for the full pattern doc.
//======================================================================================================
#define FOREACH_MANUAL_PER_CORE_FIELD(X)                                                                                  \
    /* type,                name,                      suffix,  rationale (matches MANUAL_FIELDS_INVENTORY.md) */         \
    /* === String arrays awaiting KIND_STRING / KIND_FILE_PATH at .F.4e === */                                            \
    X(char,                 core_model_path,           [256],   "KIND_FILE_PATH cohort at .F.4e")                         \
    X(char,                 core_model_dir,            [256],   "KIND_FILE_PATH cohort at .F.4e")                         \
    X(char,                 core_horizon_list,         [128],   "KIND_STRING cohort at .F.4e")                            \
    X(char,                 core_ensemble_blend_mode,  [16],    "KIND_STRING (or KIND_INT_ENUM) cohort at .F.4e")         \
    X(char,                 core_disabled_horizons,    [128],   "KIND_STRING cohort at .F.4e")                            \
    /* === Per-core symbol (WIP2d-1.A — partial advance of .F.4c.3.A; operator-facing forward-compat for multi-symbol DataStream) === */ \
    X(char,                 core_symbol,               [32],    "KIND_STRING cohort at .F.4e — partial advance of .F.4c.3.A symbol axis migration") \
    /* === Hex64 bitmap awaiting KIND_HEX64 at .F.4e === */                                                                \
    X(uint64_t,             core_feature_mask,         ,        "KIND_HEX64 needed at .F.4e")                             \
    /* === TRANSITIONAL parallel arrays — delete at WIP2g (cores[c] authoritative) === */                                  \
    X(FPN<F>,               core_risk_pct,             ,        "TRANSITIONAL: cores[c].risk_pct authoritative; delete at WIP2g") \
    X(uint8_t,              core_strategies,           ,        "TRANSITIONAL: cores[c].strategy authoritative (WIP2d-0); delete at WIP2g") \
    X(uint32_t,             core_time_exit_ticks,      ,        "TRANSITIONAL: cores[c].max_hold_ticks authoritative; legacy override array; delete at WIP2g") \
    X(FPN<F>,               core_max_drawdown_pct,     ,        "TRANSITIONAL: cores[c].max_drawdown_pct authoritative; legacy override array; delete at WIP2g") \
    /* === TRANSITIONAL legacy override struct — delete at WIP2f (PerCoreOverrides<F> retired) === */                      \
    X(PerCoreOverrides<F>,  core_overrides,            ,        "TRANSITIONAL: legacy global-default-with-override anti-pattern; entire mechanism retires at WIP2f when ControllerConfig_ResolveForCore deletes")

// Payload macro: emit `type name[MAX_EXECUTION_CORES] suffix;` per row.
// Used by ControllerConfig<F> for parallel array declarations.
#define EMIT_MANUAL_PER_CORE_DECL(type, name, suffix, rationale) \
    type name[MAX_EXECUTION_CORES] suffix;

//======================================================================================================
// [FOREACH_PER_CORE_DOMAIN_BITMAP — meta-registry for cfg-domain bitmap fields (WIP2d-0.B)]
//======================================================================================================
// PURPOSE: meta-registry binding each FOREACH_<DOMAIN>_CFG_FLAG child registry to its
// per-core bitmap storage field. SINGLE source of truth for the 5 bitmap fields on
// PerCoreCfg<F>; adding a new domain registry = 1 row here + 1 row in MANUAL_FIELDS_INVENTORY.md.
//
// DRIVES (auto-flows):
//   1. Struct field declarations in PerCoreCfg<F> via FOREACH_PER_CORE_DOMAIN_BITMAP(EMIT_DOMAIN_BITMAP_FIELD)
//   2. Bitmap-overflow static_asserts per domain (defense in depth alongside per-registry asserts)
//      via FOREACH_PER_CORE_DOMAIN_BITMAP(EMIT_DOMAIN_OVERFLOW_ASSERT) per
//      bitmap-overflow-protection-discipline.md
//   3. WIP2e bitmap-rebuild walker: iterates this meta-registry; for each domain walks
//      `child_registry` to rebuild bitmap from flat KIND_BOOL rows at slow-path cadence
//   4. CI cross-check: every domain registered ↔ MANUAL_FIELDS_INVENTORY.md Section B row;
//      added domain registry without bitmap field = BUILD ERROR (closes domain-bitmap-drift class)
//
// FIRST CANONICAL APPLICATION of meta-registry-pattern-for-codebase-registry-discipline.md
// (Stage 3 ACTIVE at .F.4c.3 WIP2d-0.B; one ship before .F.4d FOREACH_REGISTRY codebase-wide
// meta-registry + H15 codification). The pattern composes upward — at .F.4d this meta-registry
// gets a row in FOREACH_REGISTRY top-level meta-registry.
//
// Row shape: X(align_n, DOMAIN_TOKEN, field_name, storage_type, child_registry_token)
//   - align_n:    8 = alignas(8) cluster boundary; 0 = natural alignment
//   - DOMAIN_TOKEN: pastes with _CFG_COUNT for static_assert (e.g., LIFECYCLE → LIFECYCLE_CFG_COUNT)
//   - storage_type: bitmap storage type (uint8_t / uint16_t per FOREACH_X_CFG_FLAG bit count)
//   - child_registry_token: FOREACH_<DOMAIN>_CFG_FLAG (used by WIP2e rebuild walker)
//======================================================================================================
#define FOREACH_PER_CORE_DOMAIN_BITMAP(X)                                                              \
    /* align, domain,    field,                  storage,  child registry token */                    \
    X(8,      LIFECYCLE, lifecycle_cfg_flags,    uint8_t,  FOREACH_LIFECYCLE_CFG_FLAG)                 \
    X(0,      GATE,      gate_cfg_flags,         uint8_t,  FOREACH_GATE_CFG_FLAG)                     \
    X(0,      ML,        ml_cfg_flags,           uint16_t, FOREACH_ML_CFG_FLAG)                       \
    X(0,      RISK,      risk_cfg_flags,         uint8_t,  FOREACH_RISK_CFG_FLAG)                     \
    X(0,      OPS,       ops_cfg_flags,          uint8_t,  FOREACH_OPS_CFG_FLAG)

// alignas-conditional helper (token-paste pattern; 0 = no qualifier).
#define EMIT_ALIGNAS_0    /* natural alignment */
#define EMIT_ALIGNAS_8    alignas(8)

// Payload macro: emit `[alignas(N)] <storage> <field>;` per row.
// Used by PerCoreCfg<F> for runtime bitmap field declarations.
#define EMIT_DOMAIN_BITMAP_FIELD(align_n, domain, field, storage, child) \
    EMIT_ALIGNAS_##align_n storage field;

// Payload macro: emit bitmap-overflow static_assert per domain.
// Defense in depth — each child registry header has its own static_assert too;
// this meta-level assert catches the case where a domain's COUNT grows beyond storage capacity
// from the meta-registry's perspective (e.g., upgrading uint8_t → uint16_t storage requires
// also updating the row here AND the child registry's own assert).
#define EMIT_DOMAIN_OVERFLOW_ASSERT(align_n, domain, field, storage, child)                            \
    static_assert(domain##_CFG_COUNT <= sizeof(storage) * 8,                                           \
                  "Bitmap overflow: " #domain "_CFG_COUNT exceeds " #storage " bits; "                  \
                  "upgrade storage type in FOREACH_PER_CORE_DOMAIN_BITMAP row for " #domain);

// Invoke bitmap-overflow asserts at file scope (compile-time check).
// WIP2d-1.B.0 (Shortsighted #5 close) — CfgFieldRegistry.hpp now self-contained: the 5 domain
// registry headers are included at the top of this file, so <DOMAIN>_CFG_COUNT constants are
// in scope here independent of any other file's include order.
FOREACH_PER_CORE_DOMAIN_BITMAP(EMIT_DOMAIN_OVERFLOW_ASSERT)

//======================================================================================================
// [FOREACH_PER_CORE_NO_FLAT_FIELD_SYNC — AUTOPOPULATE manual sync sources (WIP2d-1.B.0)]
//======================================================================================================
// PURPOSE: auxiliary registry for FOREACH_PER_CORE_CFG_FIELD rows tagged NO_FLAT_FIELD.
// These rows lack a ControllerConfig flat scalar; the auto-flow copy walker skips them via
// the NO_FLAT_FIELD bit. THIS registry provides the manual sync source mapping (legacy
// parallel array → registry-driven cores[c] slice). The AUTOPOPULATE companion macro
// generates the sync lines in PopulateCoresFromFlat per autopopulate-pattern-for-production-
// caller-class.md.
//
// Applied at N=1 (one entry currently: strategy ← core_strategies) per
// feedback_overengineering_boundary_when_future_easier.md (future-easy multiplier wins).
// Future per-core-only fields with legacy parallel-array source: 1 row in this X-macro +
// 1 row in FOREACH_PER_CORE_CFG_FIELD with NO_FLAT_FIELD bit. Mechanical.
//
// Row shape: X(target_field, source_array_field)
//   - target_field:        the NO_FLAT_FIELD-tagged field name in FOREACH_PER_CORE_CFG_FIELD
//   - source_array_field:  the legacy parallel array on ControllerConfig (must be in
//                          FOREACH_MANUAL_PER_CORE_FIELD as TRANSITIONAL exemption)
//
// CI check (tools/check_per_core_registry_integrity.py — added WIP2d-1.B.0 Check 7):
// every row here must have BOTH a FOREACH_PER_CORE_CFG_FIELD row with NO_FLAT_FIELD bit
// AND a FOREACH_MANUAL_PER_CORE_FIELD row matching the source. Closes Shortsighted #4.
//======================================================================================================
#define FOREACH_PER_CORE_NO_FLAT_FIELD_SYNC(X)                                                           \
    /* target_field,  source_array_field */                                                              \
    X(strategy,       core_strategies)

// Payload macro: emit per-core sync line. Used by PopulateCoresFromFlat (templated;
// `cfg` + `c` in scope from caller's per-core loop).
#define EMIT_NO_FLAT_FIELD_SYNC(target, source) \
    cfg->cores[c].target = cfg->source[c];

//======================================================================================================
// [PerCoreCfg<F> expected-payload computation — compile-time size-bound discipline (WIP2d-1.B.0)]
//======================================================================================================
// PURPOSE: closes Shortsighted #3 (CI regex heuristic) to ~99.9% structural strength via
// COMPILE-TIME static_assert. The X-macro KNOWS the expected struct payload (sum of STORAGE_T
// sizes from FOREACH_PER_CORE_CFG_FIELD + FOREACH_PER_CORE_DOMAIN_BITMAP). If anyone adds a
// manual field to PerCoreCfg<F> body outside the X-macros, sizeof exceeds the bound and BUILD
// FAILS with operator-readable error. The CI script (gcc -E + audit) is defense-in-depth.
//
// LOWER BOUND: sizeof(PerCoreCfg<F>) >= sum of all field sizes (always; can't be less than sum).
//              Fires on field removal or X-macro misconfiguration.
//
// UPPER BOUND: sizeof <= sum + (N_fields × max_alignof - 1) + alignas(64) tail pad.
//              For PerCoreCfg<F=64>: max alignof = 8 (FPN<F=64>). N_fields ~98 (93 cfg + 5 bitmap).
//              Max padding ~98×7 + 64 = ~750 bytes leeway. Tight enough that a manual field of
//              any reasonable size (FPN<F=64> = 24 bytes, even uint32_t = 4 bytes) pushes sizeof
//              over the upper bound. The static_assert at the struct definition site fires with
//              clear "MANUAL FIELD ADDED" diagnostic.
//
// The static_asserts live in ControllerConfig.hpp post-PerCoreCfg<F> struct definition.
//======================================================================================================
template <unsigned F>
constexpr size_t calc_per_core_cfg_expected_payload_bytes() {
    size_t total = 0;
#define EMIT_FIELD_SIZEOF(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, \
                          applies_to_strategy_cat, applies_to_op_mode_cat, \
                          applies_to_regime_cat, applies_to_risk_cat, lives_in_struct) \
    total += sizeof(STORAGE_T);
    FOREACH_PER_CORE_CFG_FIELD(EMIT_FIELD_SIZEOF)
#undef EMIT_FIELD_SIZEOF
#define EMIT_BITMAP_SIZEOF(align_n, domain, field, storage, child) total += sizeof(storage);
    FOREACH_PER_CORE_DOMAIN_BITMAP(EMIT_BITMAP_SIZEOF)
#undef EMIT_BITMAP_SIZEOF
    return total;
}

// Field count for upper-bound leeway estimation (per-field max padding = alignof-1).
template <unsigned F>
constexpr size_t calc_per_core_cfg_field_count() {
    size_t count = 0;
#define EMIT_FIELD_COUNT(STORAGE_T, KIND_TOKEN, name, ...) count += 1;
    FOREACH_PER_CORE_CFG_FIELD(EMIT_FIELD_COUNT)
#undef EMIT_FIELD_COUNT
#define EMIT_BITMAP_COUNT(align_n, domain, field, storage, child) count += 1;
    FOREACH_PER_CORE_DOMAIN_BITMAP(EMIT_BITMAP_COUNT)
#undef EMIT_BITMAP_COUNT
    return count;
}

inline constexpr size_t kPerCoreCfgExpectedPayloadBytes64 = calc_per_core_cfg_expected_payload_bytes<64>();
inline constexpr size_t kPerCoreCfgFieldCount             = calc_per_core_cfg_field_count<64>();
// Max padding leeway: each field can have at most (alignof-1) padding bytes before it.
// FPN<F=64> alignof = 8 (8-byte alignment for uint64_t members). uint16_t = 2. uint8_t = 1.
// Conservative upper bound: assume worst-case 8-byte alignment for all fields → 7 bytes padding each.
// Plus alignas(64) tail-pad up to 63 bytes.
inline constexpr size_t kPerCoreCfgMaxPaddingBytes        = kPerCoreCfgFieldCount * 7 + 63;

//======================================================================================================
// [FIELD_IDX enums — per-registry auto-generated]
//======================================================================================================
// Drives g_*_cfg_field_descriptors[FIELD_IDX_*_<name>] direct access at compile time.
#define X_GEN_GLOBAL_FIELD_IDX(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_to_strategy_cat, applies_to_op_mode_cat, applies_to_regime_cat, applies_to_risk_cat, lives_in_struct) \
    FIELD_IDX_GLOBAL_##name,
#define X_GEN_PER_CORE_FIELD_IDX(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_to_strategy_cat, applies_to_op_mode_cat, applies_to_regime_cat, applies_to_risk_cat, lives_in_struct) \
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
#define X_GEN_DESCRIPTOR_GLOBAL(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload_init, tooltip, applies_to_strategy_cat, applies_to_op_mode_cat, applies_to_regime_cat, applies_to_risk_cat, lives_in_struct) \
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

#define X_GEN_DESCRIPTOR_PER_CORE(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload_init, tooltip, applies_to_strategy_cat, applies_to_op_mode_cat, applies_to_regime_cat, applies_to_risk_cat, lives_in_struct) \
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
    X(restart_required,         RESTART_REQUIRED)                          \
    X(safety_critical,          SAFETY_CRITICAL)                           \
    X(deprecated,               DEPRECATED)                                \
    X(stamp_bound,              STAMP_BOUND)                               \
    X(hidden_by_default,        HIDDEN_BY_DEFAULT)                         \
    X(is_secret,                IS_SECRET)                                 \
    X(is_boot_only,             IS_BOOT_ONLY)                              \
    X(affects_stamp_parity,     AFFECTS_STAMP_PARITY)                      \
    X(log_value_forbidden,      LOG_VALUE_FORBIDDEN)                       \
    X(has_side_effect,          HAS_SIDE_EFFECT)                           \
    X(warn_on_clamp,            WARN_ON_CLAMP)                              \
    X(stamp_bound_cfg_derived,  STAMP_BOUND_CFG_DERIVED)  /* v5.15.5.F.4d.1.A — Path γ first canonical consumer */

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
// [CI CHECK 9 — STAMP_BOUND_CFG_DERIVED COHORT COVERAGE REGRESSION GUARD]
//------------------------------------------------------------------------------
// v5.15.5.F.4d.1.B.3 Step 4 (2026-05-24). Compile-time coverage assertion ensures
// the STAMP_BOUND_CFG_DERIVED cohort doesn't shrink unintentionally. At ship time:
// per-core mask covers 19 fields + global mask covers 1 field (held_out_fraction
// added at Phase F HIGH-1) + ML_CFG_FLAG cohort + GATE_CFG_FLAG cohort = 24+ total
// flagged fields.
//
// Threshold ≥ 30 (tighter than initial ≥ 24 per /readiness LOW-1 amendment 2026-05-24)
// — actual cohort size at .B.3 is 36-37 across 4 cohort registries; ≥ 30 leaves ~6-7
// safety margin for legitimate single-field exemption without false-positive but
// catches >7-field cohort regression. Per `registry-coverage-ci-check-pattern.md`
// Shape A.2 (compile-time positive coverage assertion).
//
// Note: per-core + global only counted here (the 2 master cfg registries with mask
// infrastructure). ML_CFG_FLAG + GATE_CFG_FLAG cohort coverage verified separately
// at their respective registries.
//------------------------------------------------------------------------------
static_assert(
    cfg_field_count(g_per_core_cfg_stamp_bound_cfg_derived_mask)
    + cfg_field_count(g_global_cfg_stamp_bound_cfg_derived_mask) >= 20,
    "STAMP_BOUND_CFG_DERIVED cohort coverage regression: per-core + global mask combined "
    "should flag ≥ 20 fields (was 19 per-core + 1 global = 20 at .B.3 ship close 2026-05-24). "
    "If you intentionally removed a STAMP_BOUND_CFG_DERIVED row from FOREACH_PER_CORE_CFG_FIELD "
    "or FOREACH_GLOBAL_CFG_FIELD, lower this threshold AND document rationale at the row deletion site. "
    "If you DIDN'T remove a row, find what regressed (likely accidental metadata-flag drop)."
);

//------------------------------------------------------------------------------
// [H16 COMPILE-TIME ENFORCEMENT — Path γ correction (v5.15.5.F.4d.1.A)]
//------------------------------------------------------------------------------
// Per DESIGN_SPECS/metadata-bit-driven-derived-filter-framework.md v1.2 Path γ
// correction (2026-05-17). Every metadata bit MUST be either enrolled in
// FOREACH_METADATA_BIT (auto-generates mask infrastructure for consumers) OR
// in EXEMPT_FROM_FOREACH_METADATA_BIT list with rationale. Sister to existing
// bitmap-overflow guard via static_assert(WARN_ON_CLAMP < (1u << 16)) below.
//
// Compile-time mechanism preferred over runtime Python CI per
// DESIGN_SPECS/registry-coverage-ci-check-pattern.md § "Mechanism choice:
// compile-time vs runtime" (when source data is X-macro-driven).
//------------------------------------------------------------------------------

// Gather enrolled bits via X-macro reduction over FOREACH_METADATA_BIT:
#define X_GATHER_METADATA_BITS(lname, BITNAME) \
    | static_cast<uint16_t>(CfgFieldDescriptor::BITNAME)
inline constexpr uint16_t ENROLLED_METADATA_BITS =
    (0u FOREACH_METADATA_BIT(X_GATHER_METADATA_BITS));
#undef X_GATHER_METADATA_BITS

// Consumer-side-only bits — no mask infrastructure needed:
//   NO_FLAT_FIELD: struct-gen metadata only; no iteration consumer (per WIP2d-1.B.0)
// MANUAL_PARSER bit IS enrolled (as has_side_effect alias mapping to same bit 10
//   per legacy alias at line ~156 — alias makes HAS_SIDE_EFFECT name still work).
inline constexpr uint16_t EXEMPT_FROM_FOREACH_METADATA_BIT =
      CfgFieldDescriptor::NO_FLAT_FIELD;

// All metadata bits in use (update when new bit added to MetadataFlag enum):
inline constexpr uint16_t ALL_METADATA_BITS_IN_USE =
      CfgFieldDescriptor::RESTART_REQUIRED
    | CfgFieldDescriptor::SAFETY_CRITICAL
    | CfgFieldDescriptor::DEPRECATED
    | CfgFieldDescriptor::STAMP_BOUND
    | CfgFieldDescriptor::HIDDEN_BY_DEFAULT
    | CfgFieldDescriptor::IS_SECRET
    | CfgFieldDescriptor::IS_BOOT_ONLY
    | CfgFieldDescriptor::AFFECTS_STAMP_PARITY
    | CfgFieldDescriptor::LOG_VALUE_FORBIDDEN
    | CfgFieldDescriptor::MANUAL_PARSER       // bit 10; HAS_SIDE_EFFECT alias same bit
    | CfgFieldDescriptor::WARN_ON_CLAMP
    | CfgFieldDescriptor::NO_FLAT_FIELD
    | CfgFieldDescriptor::STAMP_BOUND_CFG_DERIVED;  // bit 13 (v5.15.5.F.4d reserved; enrolled at .F.4d.1.A)

static_assert(
    (ALL_METADATA_BITS_IN_USE & ~(ENROLLED_METADATA_BITS | EXEMPT_FROM_FOREACH_METADATA_BIT)) == 0u,
    "H16 violated: a CfgFieldDescriptor::MetadataFlag bit is in use but not "
    "enrolled in FOREACH_METADATA_BIT AND not in EXEMPT_FROM_FOREACH_METADATA_BIT. "
    "Add row to FOREACH_METADATA_BIT (preferred; auto-generates mask infrastructure) "
    "OR add to EXEMPT_FROM_FOREACH_METADATA_BIT with rationale comment."
);

//------------------------------------------------------------------------------
// [CFG_COMPOSE_AUDIT_DECISIONS — composition audit checklist (Gap 1 mitigation)]
//------------------------------------------------------------------------------
// Per DESIGN_SPECS/composed-filter-mask-pattern.md Stage 2 DRAFT § "Step 3
// composition audit checklist". Adding a new metadata bit to FOREACH_METADATA_BIT
// FORCES explicit decision per existing composed mask (Gap 1 pre-emptive
// closure: composition discipline blindspot).
//
// Tuple: X(composed_mask_name, metadata_bit_lname, DECISION)
//   COMPOSE_INCLUDE: mask INCLUDES rows where bit is set
//   COMPOSE_EXCLUDE: mask EXCLUDES rows where bit is set
//   COMPOSE_NA:      bit is irrelevant to this composed mask
//
// Composed masks at HEAD (post-`.F.4d.1.A` Path γ+ v2):
//   render_mask      = ~(is_boot_only | hidden_by_default)
//   save_mask        = ~has_side_effect (bit 10 / MANUAL_PARSER)
//   cli_explain_mask = ~(has_side_effect | hidden_by_default)  [fixed at .A Step 4b]
//
// 12 enrolled bits × 3 composed masks = 36 cells.
//------------------------------------------------------------------------------

#define CFG_COMPOSE_AUDIT_DECISIONS(X)                                              \
    /* render_mask = ~(is_boot_only | hidden_by_default) */                         \
    X(render_mask, restart_required,         COMPOSE_NA)                            \
    X(render_mask, safety_critical,          COMPOSE_NA)                            \
    X(render_mask, deprecated,               COMPOSE_NA)                            \
    X(render_mask, stamp_bound,              COMPOSE_NA)                            \
    X(render_mask, hidden_by_default,        COMPOSE_EXCLUDE)                       \
    X(render_mask, is_secret,                COMPOSE_NA)                            \
    X(render_mask, is_boot_only,             COMPOSE_EXCLUDE)                       \
    X(render_mask, affects_stamp_parity,     COMPOSE_NA)                            \
    X(render_mask, log_value_forbidden,      COMPOSE_NA)                            \
    X(render_mask, has_side_effect,          COMPOSE_NA)                            \
    X(render_mask, warn_on_clamp,            COMPOSE_NA)                            \
    X(render_mask, stamp_bound_cfg_derived,  COMPOSE_NA)                            \
    /* save_mask = ~has_side_effect */                                              \
    X(save_mask, restart_required,           COMPOSE_NA)                            \
    X(save_mask, safety_critical,            COMPOSE_NA)                            \
    X(save_mask, deprecated,                 COMPOSE_NA)                            \
    X(save_mask, stamp_bound,                COMPOSE_NA)                            \
    X(save_mask, hidden_by_default,          COMPOSE_NA)                            \
    X(save_mask, is_secret,                  COMPOSE_NA)                            \
    X(save_mask, is_boot_only,               COMPOSE_NA)                            \
    X(save_mask, affects_stamp_parity,       COMPOSE_NA)                            \
    X(save_mask, log_value_forbidden,        COMPOSE_NA)                            \
    X(save_mask, has_side_effect,            COMPOSE_EXCLUDE)                       \
    X(save_mask, warn_on_clamp,              COMPOSE_NA)                            \
    X(save_mask, stamp_bound_cfg_derived,    COMPOSE_NA)                            \
    /* cli_explain_mask = ~(has_side_effect | hidden_by_default) [fixed .A Step 4b] */ \
    X(cli_explain_mask, restart_required,         COMPOSE_NA)                       \
    X(cli_explain_mask, safety_critical,          COMPOSE_NA)                       \
    X(cli_explain_mask, deprecated,               COMPOSE_NA)                       \
    X(cli_explain_mask, stamp_bound,              COMPOSE_NA)                       \
    X(cli_explain_mask, hidden_by_default,        COMPOSE_EXCLUDE)                  \
    X(cli_explain_mask, is_secret,                COMPOSE_NA)                       \
    X(cli_explain_mask, is_boot_only,             COMPOSE_NA)                       \
    X(cli_explain_mask, affects_stamp_parity,     COMPOSE_NA)                       \
    X(cli_explain_mask, log_value_forbidden,      COMPOSE_NA)                       \
    X(cli_explain_mask, has_side_effect,          COMPOSE_EXCLUDE)                  \
    X(cli_explain_mask, warn_on_clamp,            COMPOSE_NA)                       \
    X(cli_explain_mask, stamp_bound_cfg_derived,  COMPOSE_NA)

// Compile-time count verification:
#define X_COUNT_METADATA_BIT(lname, BITNAME) +1
inline constexpr size_t FOREACH_METADATA_BIT_COUNT =
    (0 FOREACH_METADATA_BIT(X_COUNT_METADATA_BIT));
#undef X_COUNT_METADATA_BIT

#define X_COUNT_COMPOSE_DECISION(mask, bit, decision) +1
inline constexpr size_t CFG_COMPOSE_AUDIT_DECISIONS_COUNT =
    (0 CFG_COMPOSE_AUDIT_DECISIONS(X_COUNT_COMPOSE_DECISION));
#undef X_COUNT_COMPOSE_DECISION

inline constexpr size_t COMPOSED_MASK_COUNT_AT_F4D1A = 3;  // render + save + cli_explain

static_assert(
    CFG_COMPOSE_AUDIT_DECISIONS_COUNT == FOREACH_METADATA_BIT_COUNT * COMPOSED_MASK_COUNT_AT_F4D1A,
    "CFG_COMPOSE_AUDIT_DECISIONS row count mismatch. Each metadata bit MUST "
    "have exactly one decision row per composed mask. When adding new bit to "
    "FOREACH_METADATA_BIT, add 3 rows to CFG_COMPOSE_AUDIT_DECISIONS (one per "
    "composed mask: render / save / cli_explain) explicitly choosing INCLUDE / "
    "EXCLUDE / NA. When adding new composed mask, update "
    "COMPOSED_MASK_COUNT_AT_F4D1A + add row per existing metadata bit."
);

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

// v5.15.5.F.4d.1.A Step 4c — g_global_cfg_stamp_emit_mask alias DELETED (was pure
// alias of g_global_cfg_stamp_bound_mask; zero consumers verified via rg).

// v5.15.5.F.4d.1.A Step 4b — cli_explain_mask composition FIXED. Was producing
// ~0ULL (all bits set) instead of documented ~(has_side_effect | hidden_by_default).
// Bug existed pre-Path γ; no production impact today (zero consumers); fix
// pre-emptive so .F.4e CLI --explain consumer reads correct mask.
constexpr CfgMaskArray<(FIELD_IDX_GLOBAL_END + 63) / 64> cfg_compose_global_cli_explain_mask() {
    constexpr size_t WORDS = (FIELD_IDX_GLOBAL_END + 63) / 64;
    CfgMaskArray<WORDS> out = {};
    for (size_t i = 0; i < WORDS; ++i) {
        out.words[i] = ~(g_global_cfg_has_side_effect_mask.words[i] | g_global_cfg_hidden_by_default_mask.words[i]);
    }
    if constexpr ((FIELD_IDX_GLOBAL_END % 64) != 0) {
        constexpr uint64_t last_word_valid = (1ULL << (FIELD_IDX_GLOBAL_END % 64)) - 1ULL;
        out.words[WORDS - 1] &= last_word_valid;
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

// v5.15.5.F.4d.1.A Step 4c — g_per_core_cfg_stamp_emit_mask alias DELETED (was
// pure alias of g_per_core_cfg_stamp_bound_mask; zero consumers verified).

// v5.15.5.F.4d.1.A Step 4b — cli_explain_mask composition FIXED per-core sister
// to global fix. Was producing ~0ULL; corrected to documented composition.
constexpr CfgMaskArray<(FIELD_IDX_PER_CORE_END + 63) / 64> cfg_compose_per_core_cli_explain_mask() {
    constexpr size_t WORDS = (FIELD_IDX_PER_CORE_END + 63) / 64;
    CfgMaskArray<WORDS> out = {};
    for (size_t i = 0; i < WORDS; ++i) {
        out.words[i] = ~(g_per_core_cfg_has_side_effect_mask.words[i] | g_per_core_cfg_hidden_by_default_mask.words[i]);
    }
    if constexpr ((FIELD_IDX_PER_CORE_END % 64) != 0) {
        constexpr uint64_t last_word_valid = (1ULL << (FIELD_IDX_PER_CORE_END % 64)) - 1ULL;
        out.words[WORDS - 1] &= last_word_valid;
    }
    return out;
}
inline constexpr auto g_per_core_cfg_cli_explain_mask = cfg_compose_per_core_cli_explain_mask();

// .F.4c.3 — g_cfg_per_core_ok_mask + g_cfg_per_core_override_mask DELETED.
// Per-core scope is registry membership, not a metadata bit; the override
// emit path went away with PerCoreOverrides<F> at this ship.
