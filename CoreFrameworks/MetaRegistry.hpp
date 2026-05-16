// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
//======================================================================================================
// [FOREACH_REGISTRY — codebase-wide meta-registry of X-macro registries (WIP2d-1.B.0b)]
//======================================================================================================
// PURPOSE: codebase-wide Level-0 meta-registry per `meta-registry-pattern-for-codebase-registry-
// discipline.md`. Every X-macro registry in the codebase MUST have a row here. CI cross-check
// (tools/check_meta_registry.py) enforces — adding a new registry without registering it FAILS
// the build via warning escalation.
//
// Pulls forward `.F.4d` H15 codification one ship early: pre-WIP2d-1.B.0b the discipline was
// "documentation cross-references only"; post-WIP2d-1.B.0b the discipline is "codebase-wide CI
// enforcement." Closes Shortsighted #2 (meta-registry applied one ship early; drift was
// documentation-only until .F.4d).
//
// Row shape: X(registry_name, LEVEL, PARENT_NAME, description)
//   - registry_name:  the FOREACH_<X> macro identifier
//   - LEVEL:          0 = ROOT (codebase-wide; this registry itself); 1 = direct registry;
//                     2 = child of a Level-1 meta-registry; etc.
//   - PARENT_NAME:    for LEVEL > 0, the meta-registry that manages this registry's discipline.
//                     ROOT_NONE for LEVEL = 0 (the codebase-wide root itself).
//   - description:    one-line operator-facing description of the registry's purpose
//
// FUTURE ADD: at .F.4d, FOREACH_REGISTRY itself gets reorganized when more registries land
// (.F.4d FOREACH_DERIVED_FILTER + sidecar override + further meta-registries). This file is the
// codebase-wide enforcement seed; each addition is 1 row.
//
// SEE: DESIGN_SPECS/meta-registry-pattern-for-codebase-registry-discipline.md
//======================================================================================================
#ifndef META_REGISTRY_HPP
#define META_REGISTRY_HPP

#define ROOT_NONE 0  // sentinel: this registry is the ROOT (no parent)

#define FOREACH_REGISTRY(X)                                                                                                                              \
    /* registry_name,                       LEVEL,  PARENT_NAME,                       description */                                                    \
    X(FOREACH_REGISTRY,                     0,      ROOT_NONE,                         "Codebase-wide meta-registry of X-macro registries (this one).") \
    /* === Per-core cfg surface (.F.4c.3 framework — WIP2d-0 + WIP2d-0.B) === */                                                                          \
    X(FOREACH_GLOBAL_CFG_FIELD,             1,      FOREACH_REGISTRY,                  "Global cfg fields on ControllerConfig<F> (47 rows; system/training/mode/ack/etc.)") \
    X(FOREACH_PER_CORE_CFG_FIELD,           1,      FOREACH_REGISTRY,                  "Per-core cfg surface (93 rows; X-macro struct gen + tt:: dispatch).") \
    X(FOREACH_MANUAL_PER_CORE_FIELD,        1,      FOREACH_REGISTRY,                  "Per-core legacy parallel-array exemptions (12 rows; awaiting .F.4e KIND_STRING/_FILE_PATH/_HEX64).") \
    X(FOREACH_PER_CORE_DOMAIN_BITMAP,       1,      FOREACH_REGISTRY,                  "Meta-registry of cfg-domain bitmap storage (5 rows; binds LIFECYCLE/GATE/ML/RISK/OPS to PerCoreCfg<F> fields).") \
    X(FOREACH_PER_CORE_NO_FLAT_FIELD_SYNC,  1,      FOREACH_REGISTRY,                  "AUTOPOPULATE sync sources for NO_FLAT_FIELD-tagged rows (1 row; future per-core-only fields land here mechanically).") \
    X(FOREACH_METADATA_BIT,                 1,      FOREACH_REGISTRY,                  "Per-bit mask declarations for CfgFieldDescriptor::MetadataFlag (compile-time bitmap masks).") \
    /* === Cfg-domain bitmap child registries (managed by FOREACH_PER_CORE_DOMAIN_BITMAP) === */                                                          \
    X(FOREACH_LIFECYCLE_CFG_FLAG,           2,      FOREACH_PER_CORE_DOMAIN_BITMAP,    "Lifecycle cfg flags (8-bit bitmap; partial_exit_enabled / breakeven flags).") \
    X(FOREACH_GATE_CFG_FLAG,                2,      FOREACH_PER_CORE_DOMAIN_BITMAP,    "Entry/exit gate cfg flags (8-bit bitmap).") \
    X(FOREACH_ML_CFG_FLAG,                  2,      FOREACH_PER_CORE_DOMAIN_BITMAP,    "ML/confidence cfg flags (16-bit bitmap; bandit_enabled / composite_enabled / etc.).") \
    X(FOREACH_RISK_CFG_FLAG,                2,      FOREACH_PER_CORE_DOMAIN_BITMAP,    "Risk/sizing cfg flags (8-bit bitmap).") \
    X(FOREACH_OPS_CFG_FLAG,                 2,      FOREACH_PER_CORE_DOMAIN_BITMAP,    "Operational cfg flags (8-bit bitmap).") \
    /* === Stamp body registries (legacy; consolidates at .F.4d via STAMP_BOUND derived filter) === */                                                    \
    X(FOREACH_STAMP_BOUND_CFG,              1,      FOREACH_REGISTRY,                  "Stamp-bound cfg fields (legacy at .F.4c.3; consolidates into STAMP_BOUND derived filter at .F.4d).") \
    X(FOREACH_STAMP_BOUND_MODEL_CONST_PRE_CFG,  1,  FOREACH_REGISTRY,                  "Stamp-bound model-const fields PRE-CFG (HMAC body emit order; v5.14.8.A pattern).") \
    X(FOREACH_STAMP_BOUND_MODEL_CONST_POST_CFG, 1,  FOREACH_REGISTRY,                  "Stamp-bound model-const fields POST-CFG (HMAC body emit order; v5.14.8.A pattern).") \
    /* === Strategy + ML registries === */                                                                                                                \
    X(FOREACH_STRATEGY,                     1,      FOREACH_REGISTRY,                  "Strategy dispatch table (MR/MOM/DIP/ML/EMA + AUTO sentinel; CLAUDE.md item 13 X-macro).") \
    X(FOREACH_BANDIT_ALGORITHM,             1,      FOREACH_REGISTRY,                  "Bandit algorithm dispatch (Exp3-IX / Thompson; STAMP_BOUND).") \
    X(FOREACH_BANDIT_SIDE,                  1,      FOREACH_REGISTRY,                  "Bandit side meta-X-macro (buy/exit symmetric; v5.15.5.F.4d TECH_DEBT-084 first canonical for FOREACH_BANDIT_SIDE auto-mirror).") \
    X(FOREACH_FAILURE_MODE,                 1,      FOREACH_REGISTRY,                  "Failure mode bit-flag storage registry (FailureModeRegistry.hpp).") \
    /* === OMS state registries === */                                                                                                                    \
    X(FOREACH_OMS_FIELD,                    1,      FOREACH_REGISTRY,                  "OMS state field registry (AUTOPOPULATE / drift check / etc.).") \
    /* === WIP2d-1.B.0b — bulk registration of pre-existing registries (43 entries; closes Shortsighted #2 to 100%) === */
    X(FOREACH_STAMP_BOUND_MODEL_CONST           , 1,      FOREACH_REGISTRY                 , "Stamp-bound model-const fields (union of PRE_CFG + POST_CFG; canonical body emit order).") \
    X(FOREACH_STAMP_BOUND_MODEL_CONST_GROUPS    , 1,      FOREACH_REGISTRY                 , "Stamp-bound model-const grouped fields (Y3 dispatch group anchor).") \
    X(FOREACH_STAMP_BOUND_MODEL_CONST_STANDALONE, 1,      FOREACH_REGISTRY                 , "Stamp-bound model-const standalone (non-grouped) fields.") \
    X(FOREACH_BARRIER_BLEND_MODE                , 1,      FOREACH_REGISTRY                 , "Barrier blend mode dispatch (LEGACY/BLEND/DOMINANT).") \
    X(FOREACH_IC_VARIANT                        , 1,      FOREACH_REGISTRY                 , "IC variant enum (per-core confidence IC method choice).") \
    X(FOREACH_DEGRADATION_CURVE                 , 1,      FOREACH_REGISTRY                 , "Risk degradation curve enum (OFF/LINEAR/EXP/STEP).") \
    X(FOREACH_RECONCILE_MODE                    , 1,      FOREACH_REGISTRY                 , "Reconcile mode enum (STRICT/WARN/AUTO_SYNC).") \
    X(FOREACH_REGIME                            , 1,      FOREACH_REGISTRY                 , "Regime enum (RANGING/TRENDING/VOLATILE/MILD_TREND).") \
    X(FOREACH_TARGET                            , 1,      FOREACH_REGISTRY                 , "ML target enum (training-side label types).") \
    X(FOREACH_FEATURE                           , 1,      FOREACH_REGISTRY                 , "ML feature registry (compute fns + metadata).") \
    X(FOREACH_SHALT                             , 1,      FOREACH_REGISTRY                 , "SHALT codes (per-gate blocking conditions).") \
    X(FOREACH_HALT_REASON                       , 1,      FOREACH_REGISTRY                 , "Halt reason codes (kill switch / boot refusal / etc.).") \
    X(FOREACH_LIVE_READINESS_CHECK              , 1,      FOREACH_REGISTRY                 , "Live readiness boot checks (mlockall / model age / etc.).") \
    X(FOREACH_LIVES_IN_STRUCT                   , 1,      FOREACH_REGISTRY                 , "Cross-cfg-file LivesInStruct enum (STRUCT_CFG / BACKTEST_CFG / etc.).") \
    X(FOREACH_CORE_STATE_FLAG                   , 1,      FOREACH_REGISTRY                 , "Core state bit flags (MODEL_LOAD_FAILED / KILL_TRIPPED / etc.).") \
    X(FOREACH_PER_CORE_STATE_FLAG               , 1,      FOREACH_REGISTRY                 , "Per-core state bit flags (similar shape).") \
    X(FOREACH_PER_ARM_FLAG                      , 1,      FOREACH_REGISTRY                 , "Per-bandit-arm flags (HAS_BARRIER / etc.).") \
    X(FOREACH_EZOO_INIT_FLAG                    , 1,      FOREACH_REGISTRY                 , "EnsembleModelZoo init flags (MASK_EZOO_ACTIVE / etc.).") \
    X(FOREACH_SESSION_PHASE                     , 1,      FOREACH_REGISTRY                 , "Session phase enum (boot/warmup/active/winddown).") \
    X(FOREACH_CORE_CTX_INIT_FIELD               , 1,      FOREACH_REGISTRY                 , "Per-core context init fields.") \
    X(FOREACH_CORE_CTX_RESET_FIELD              , 1,      FOREACH_REGISTRY                 , "Per-core context reset fields.") \
    X(FOREACH_CORE_CTX_SUMMARY_FIELD            , 1,      FOREACH_REGISTRY                 , "Per-core context summary fields (TUI/snapshot).") \
    X(FOREACH_DISPLAY_META_FIELD                , 1,      FOREACH_REGISTRY                 , "Per-core display metadata fields.") \
    X(FOREACH_GATE_DIAG_PAIR                    , 1,      FOREACH_REGISTRY                 , "Gate diagnostic pairs (block/pass counters).") \
    X(FOREACH_SINGLE_ZOO_POST_LOAD              , 1,      FOREACH_REGISTRY                 , "CoreModelZoo post-load setup steps.") \
    X(FOREACH_ENSEMBLE_POST_LOAD                , 1,      FOREACH_REGISTRY                 , "EnsembleModelZoo post-load setup steps.") \
    X(FOREACH_OMS_PER_SLOT_FIELD                , 1,      FOREACH_REGISTRY                 , "OMS per-slot position fields.") \
    X(FOREACH_OMS_META_SLOT                     , 1,      FOREACH_REGISTRY                 , "OMS meta-slot fields (entry/exit tracking).") \
    X(FOREACH_OMS_STATE_FLAG                    , 1,      FOREACH_REGISTRY                 , "OMS state bit flags (LIVE_TRADING / PARTIAL_EXIT / etc.).") \
    X(FOREACH_OMS_STATE_MULTI_BIT               , 1,      FOREACH_REGISTRY                 , "OMS multi-bit state slots (EVENT_LOG_MODE / etc.).") \
    X(FOREACH_POSITION_FIELD                    , 1,      FOREACH_REGISTRY                 , "Position struct fields (full persistence).") \
    X(FOREACH_POSITION_FIELD_SKIP_PERSIST       , 1,      FOREACH_REGISTRY                 , "Position fields skipped from persistence.") \
    X(FOREACH_BACKTEST_METRIC                   , 1,      FOREACH_REGISTRY                 , "Backtest summary metrics.") \
    X(FOREACH_CALIB_LOG_COL                     , 1,      FOREACH_REGISTRY                 , "Calibration log CSV columns.") \
    X(FOREACH_TRADE_LOG_COL                     , 1,      FOREACH_REGISTRY                 , "Trade log CSV columns.") \
    X(FOREACH_CONFIDENCE_PERSIST_FIELD          , 1,      FOREACH_REGISTRY                 , "Confidence persistence fields.") \
    X(FOREACH_CFG_DERIVED_INFERENCE_CFG         , 1,      FOREACH_REGISTRY                 , "Cfg-derived inference cfg fields.") \
    X(FOREACH_CFG_DRIFT_CHECK                   , 1,      FOREACH_REGISTRY                 , "Stamp body drift check fields.") \
    X(FOREACH_ARCH_FIELD_DRIFT                  , 1,      FOREACH_REGISTRY                 , "Architectural field drift check.") \
    X(FOREACH_SLOW_PATH_GATE                    , 1,      FOREACH_REGISTRY                 , "Slow-path gate registry (cfg-flag eligibility canon).") \
    X(FOREACH_SP_SECTION                        , 1,      FOREACH_REGISTRY                 , "Slow-path section enum (regime/rebuild/etc.).") \
    X(FOREACH_PANEL                             , 1,      FOREACH_REGISTRY                 , "GUI panel registry.") \
    X(FOREACH_ROLLING_WINDOW                    , 1,      FOREACH_REGISTRY                 , "Rolling window template variant registry.")

#undef ROOT_NONE

#endif // META_REGISTRY_HPP
