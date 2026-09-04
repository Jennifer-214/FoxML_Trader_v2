// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
//======================================================================================================
// [FILE]_[CoreFrameworks/MetaRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the codebase-wide Level-0 meta-registry — every X-macro registry has a row here (H15), CI-enforced]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_REGISTRY]
//======================================================================================================
#ifndef META_REGISTRY_HPP
#define META_REGISTRY_HPP

//======================================================================
// [REGISTRY]_[FOREACH_REGISTRY]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [FRAMEWORK_DISCIPLINE]]
// [REFERENCE]_[INVARIANT]_[[H15] [H19] [H18] [H21]]
// [REFERENCE]_[DESIGN_SPEC]_[[meta-registry-pattern-for-codebase-registry-discipline] [cfg-derived-consumer-framework.md]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ROOT registry of registries — enrollment (H15) + LEVEL/PARENT topology (H19), CI cross-checked]
// [COLUMN]_[registry_name]_[the FOREACH_<X> macro identifier]
// [COLUMN]_[LEVEL]_[0 = ROOT (this registry itself); 1 = direct registry; 2 = child of a Level-1 meta-registry]
// [COLUMN]_[PARENT_NAME]_[for LEVEL > 0, the meta-registry managing this registry's discipline; ROOT_NONE at LEVEL 0]
// [COLUMN]_[domain]_[quoted; what the rows are the COMPLETE SET OF, so a checker can ask "are these ALL the rows?". Closed vocabulary DERIVED from the registry-domain-vocab fence in the meta-registry DESIGN_SPEC — not listed here, so this comment cannot go stale against it. Label is lower-case DELIBERATELY: an upper-case one collides with the grammar category of the same name (the numeric-domain row) and trips the one-category-per-line rule; matches the existing lower-case position labels registry_name / description]
// [COLUMN]_[description]_[one-line operator-facing description of the registry's purpose]
// [REFERENCE]_[DECISION]_[[D-297] [D-421]]
// [REFERENCE]_[TECH_DEBT]_[[TECH_DEBT-84] [TECH_DEBT-237] [TECH_DEBT-238]]
//======================================================================
// [CODE]
//======================================================================
#define ROOT_NONE 0  // sentinel: this registry is the ROOT (no parent)

// THE DOMAIN COLUMN (D-421 step 5) — every row declares what its rows are the COMPLETE SET OF.
//
// H15 (Check 1) already asks the codebase-wide complement: "is every FOREACH_ macro enrolled
// here?" It is the ONE check in the tree that scans the CODEBASE rather than the rows, which is
// why it is the right thing to extend. But it stops at the registry boundary. Inside a registry,
// nothing asks whether the ROWS are all the rows — and that is where the silent-drop class lives:
// node_gross_wins added and never persisted (TECH_DEBT-196), ic.actuals unpersisted while its
// sibling was (D-421 step 1), 22 NodeContext members with no declared status at all until step 2.
//
// The DOMAIN column makes that question mechanical and UNIFORM, so check_meta_registry.py grows
// ONE dispatching check rather than ~8 bespoke per-registry guards.
//
// THE VOCABULARY IS NOT LISTED HERE — ON PURPOSE. It lives in ONE place, the
// ```registry-domain-vocab``` fence in
//   DESIGN_SPECS/framework-patterns/meta-registry-pattern-for-codebase-registry-discipline.md
// and check_meta_registry.py DERIVES the closed set from that fence (the same discipline
// check_code_tag_blocks.py uses for its category set). Folding a new domain kind is ONE token there
// and ZERO edits to the tool — and ZERO edits here. An enumerated copy in this comment would be a
// THIRD home for the same vocabulary and would go stale the first time a token is folded, which is
// the Class-18 mirror shape this registry exists to police. Read the fence; do not mirror it.
//
// The one rule worth repeating, because it is the rule and not the taxonomy: DECLARING NOTHING
// FAILS. A registry either names a computable domain or states why it cannot. That single property
// would have caught FOREACH_HALT_REASON, FOREACH_BACKTEST_METRIC and FOREACH_LIVES_IN_STRUCT at
// introduction. `UNCLASSIFIED` is a baseline-gated migration marker, not a domain — a NEW row
// carrying it REDs (tools/lib/meta_registry_domain_baseline.txt, shrinking to empty).
//
// Deliberately a COLUMN, not an H18 sidecar: H18's sidecar pattern is for SPARSE custom semantics,
// and a sidecar makes "declared nothing" the silent default — the exact case that must fail.
#define FOREACH_REGISTRY(X)                                                                                                                              \
    /* registry_name,                       LEVEL,  PARENT_NAME,                       DOMAIN,       description */                                       \
    X(FOREACH_REGISTRY,                     0,      ROOT_NONE,                         "CHECK:tools/check_meta_registry.py::Check 1",              "Codebase-wide meta-registry of X-macro registries (this one).") \
    /* === Per-core cfg surface (.F.4c.3 framework — WIP2d-0 + WIP2d-0.B) === */                                                                          \
    X(FOREACH_GLOBAL_CFG_FIELD,             1,      FOREACH_REGISTRY,                  "UNCLASSIFIED",              "Global cfg fields on ControllerConfig<F> (47 rows; system/training/mode/ack/etc.)") \
    X(FOREACH_PER_NODE_CFG_FIELD,           1,      FOREACH_REGISTRY,                  "UNCLASSIFIED",              "Per-core cfg surface (93 rows; X-macro struct gen + tt:: dispatch).") \
    X(FOREACH_MANUAL_PER_NODE_FIELD,        1,      FOREACH_REGISTRY,                  "UNCLASSIFIED",              "Per-core legacy parallel-array exemptions (12 rows; awaiting .F.4e KIND_STRING/_FILE_PATH/_HEX64).") \
    X(FOREACH_PER_NODE_DOMAIN_BITMAP,       1,      FOREACH_REGISTRY,                  "UNCLASSIFIED",              "Meta-registry of cfg-domain bitmap storage (5 rows; binds LIFECYCLE/GATE/ML/RISK/OPS to PerNodeCfg<F> fields).") \
    X(FOREACH_PER_NODE_NO_FLAT_FIELD_SYNC,  1,      FOREACH_REGISTRY,                  "UNCLASSIFIED",              "AUTOPOPULATE sync sources for NO_FLAT_FIELD-tagged rows (1 row; future per-core-only fields land here mechanically).") \
    X(FOREACH_PER_NODE_ARRAY_OVERRIDE,      1,      FOREACH_REGISTRY,                  "UNCLASSIFIED",              "Legacy capital parallel-array -> nodes[c] merge (2 rows: risk_pct/max_drawdown_pct; raw-copy last-wins, 0=inherit; E.1.1 item-4/B; retires WIP2g/E.1.2).") \
    X(FOREACH_METADATA_BIT,                 1,      FOREACH_REGISTRY,                  "UNCLASSIFIED",              "Per-bit mask declarations for CfgFieldDescriptor::MetadataFlag (compile-time bitmap masks).") \
    /* === Cfg-domain bitmap child registries (managed by FOREACH_PER_NODE_DOMAIN_BITMAP) === */                                                          \
    X(FOREACH_LIFECYCLE_CFG_FLAG,           2,      FOREACH_PER_NODE_DOMAIN_BITMAP,    "UNCLASSIFIED",              "Lifecycle cfg flags (8-bit bitmap; partial_exit_enabled / breakeven flags).") \
    X(FOREACH_GATE_CFG_FLAG,                2,      FOREACH_PER_NODE_DOMAIN_BITMAP,    "UNCLASSIFIED",              "Entry/exit gate cfg flags (8-bit bitmap).") \
    X(FOREACH_ML_CFG_FLAG,                  2,      FOREACH_PER_NODE_DOMAIN_BITMAP,    "UNCLASSIFIED",              "ML/confidence cfg flags (16-bit bitmap; bandit_enabled / composite_enabled / etc.).") \
    X(FOREACH_RISK_CFG_FLAG,                2,      FOREACH_PER_NODE_DOMAIN_BITMAP,    "UNCLASSIFIED",              "Risk/sizing cfg flags (8-bit bitmap).") \
    X(FOREACH_OPS_CFG_FLAG,                 2,      FOREACH_PER_NODE_DOMAIN_BITMAP,    "UNCLASSIFIED",              "Operational cfg flags (8-bit bitmap).") \
    /* === Stamp body registries (legacy; consolidates at .F.4d via STAMP_BOUND derived filter) === */                                                    \
    /* v5.15.5.F.4d.1.B.3 Step 3 (2026-05-24): FOREACH_STAMP_BOUND_CFG row DELETED at .B.3 — registry body deleted at Step 2; cfg_derived::populate_stamp_cfg_from_derived<F> framework call supersedes. */ \
    X(FOREACH_STAMP_BOUND_MODEL_CONST_PRE_CFG,  1,  FOREACH_REGISTRY,                  "UNCLASSIFIED",              "Stamp-bound model-const fields PRE-CFG (HMAC body emit order; v5.14.8.A pattern).") \
    X(FOREACH_STAMP_BOUND_MODEL_CONST_POST_CFG, 1,  FOREACH_REGISTRY,                  "UNCLASSIFIED",              "Stamp-bound model-const fields POST-CFG (HMAC body emit order; v5.14.8.A pattern).") \
    /* === Strategy + ML registries === */                                                                                                                \
    X(FOREACH_STRATEGY,                     1,      FOREACH_REGISTRY,                  "UNCLASSIFIED",              "Strategy dispatch table (MR/MOM/DIP/ML/EMA + AUTO sentinel; CLAUDE.md item 13 X-macro).") \
    X(FOREACH_BANDIT_ALGORITHM,             1,      FOREACH_REGISTRY,                  "UNCLASSIFIED",              "Bandit algorithm dispatch (Exp3-IX / Thompson; STAMP_BOUND).") \
    X(FOREACH_BANDIT_SIDE,                  1,      FOREACH_REGISTRY,                  "UNCLASSIFIED",              "Bandit side meta-X-macro (buy/exit symmetric; v5.15.5.F.4d TECH_DEBT-084 first canonical for FOREACH_BANDIT_SIDE auto-mirror).") \
    X(FOREACH_FAILURE_MODE,                 1,      FOREACH_REGISTRY,                  "UNCLASSIFIED",              "Failure mode bit-flag storage registry (FailureModeRegistry.hpp).") \
    /* === OMS state registries === */                                                                                                                    \
    X(FOREACH_OMS_FIELD,                    1,      FOREACH_REGISTRY,                  "UNCLASSIFIED",              "OMS state field registry (AUTOPOPULATE / drift check / etc.).") \
    /* === WIP2d-1.B.0b — bulk registration of pre-existing registries (43 entries; closes Shortsighted #2 to 100%) === */ \
    X(FOREACH_STAMP_BOUND_MODEL_CONST           , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Stamp-bound model-const fields (union of PRE_CFG + POST_CFG; canonical body emit order).") \
    X(FOREACH_STAMP_BOUND_MODEL_CONST_GROUPS    , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Stamp-bound model-const grouped fields (Y3 dispatch group anchor).") \
    X(FOREACH_STAMP_BOUND_MODEL_CONST_STANDALONE, 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Stamp-bound model-const standalone (non-grouped) fields.") \
    /* .E.0.10 (2026-06-11): enroll 2 pre-existing registries Check-1 flagged — H15 past-milestone close (the "convenient now" item) */ \
    /* FOREACH_LEGACY_PREFIXED_KEY row RETIRED 2026-07-17 (TECH_DEBT-238): registry DELETED with the */ \
    /* TECH_DEBT-237 pre-epoch stamp floor (H21 dead-code removal — the floor refuses every stamp    */ \
    /* that could carry the 16 legacy inference_cfg_* keys; tombstone at ML_Headers/ModelInference.hpp). */ \
    X(FOREACH_STAMP_RESULT_FIELD_EXCLUSION      , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "ModelStampResult struct-field exclusion sidecar (3 xgb_* MODEL_CONST<->master-cfg name collisions; H18; check_struct_field_uniqueness.py).") \
    X(FOREACH_BARRIER_BLEND_MODE                , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Barrier blend mode dispatch (LEGACY/BLEND/DOMINANT).") \
    X(FOREACH_IC_VARIANT                        , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "IC variant enum (per-core confidence IC method choice).") \
    X(FOREACH_DEGRADATION_CURVE                 , 1,      FOREACH_REGISTRY                 , "SSOT",              "Risk degradation curve enum (OFF/LINEAR/EXP/STEP).") \
    X(FOREACH_RECONCILE_MODE                    , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Reconcile mode enum (STRICT/WARN/AUTO_SYNC).") \
    X(FOREACH_REGIME                            , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Regime enum (RANGING/TRENDING/VOLATILE/MILD_TREND).") \
    X(FOREACH_TARGET                            , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "ML target enum (training-side label types).") \
    X(FOREACH_FEATURE                           , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "ML feature registry (compute fns + metadata).") \
    X(FOREACH_SHALT                             , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "SHALT codes (per-gate blocking conditions).") \
    X(FOREACH_HALT_REASON                       , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Halt reason codes (kill switch / boot refusal / etc.).") \
    X(FOREACH_LIVE_READINESS_CHECK              , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Live readiness boot checks (mlockall / model age / etc.).") \
    X(FOREACH_LIVES_IN_STRUCT                   , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Cross-cfg-file LivesInStruct enum (STRUCT_CFG / BACKTEST_CFG / etc.).") \
    X(FOREACH_NODE_STATE_FLAG                   , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Core state bit flags (MODEL_LOAD_FAILED / KILL_TRIPPED / etc.).") \
    X(FOREACH_PER_NODE_STATE_FLAG               , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Per-core state bit flags (similar shape).") \
    X(FOREACH_PER_ARM_FLAG                      , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Per-bandit-arm flags (HAS_BARRIER / etc.).") \
    X(FOREACH_EZOO_INIT_FLAG                    , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "EnsembleModelZoo init flags (MASK_EZOO_ACTIVE / etc.).") \
    X(FOREACH_SESSION_PHASE                     , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Session phase enum (boot/warmup/active/winddown).") \
    X(FOREACH_NODE_CTX_FIELD                    , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Per-core context init/reset fields (unified D-297; reset = RST-flagged subset view).") \
    X(FOREACH_NODE_CTX_SUMMARY_FIELD            , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Per-core context summary fields (TUI/snapshot).") \
    X(FOREACH_DISPLAY_META_FIELD                , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Per-core display metadata fields.") \
    X(FOREACH_GATE_DIAG_PAIR                    , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Gate diagnostic pairs (block/pass counters).") \
    X(FOREACH_SINGLE_ZOO_POST_LOAD              , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "NodeModelZoo post-load setup steps.") \
    X(FOREACH_ENSEMBLE_POST_LOAD                , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "EnsembleModelZoo post-load setup steps.") \
    X(FOREACH_OMS_PER_SLOT_FIELD                , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "OMS per-slot position fields.") \
    X(FOREACH_OMS_META_SLOT                     , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "OMS meta-slot fields (entry/exit tracking).") \
    X(FOREACH_OMS_STATE_FLAG                    , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "OMS state bit flags (LIVE_TRADING / PARTIAL_EXIT / etc.).") \
    X(FOREACH_OMS_STATE_MULTI_BIT               , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "OMS multi-bit state slots (EVENT_LOG_MODE / etc.).") \
    X(FOREACH_POSITION_FIELD                    , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Position struct fields (full persistence).") \
    X(FOREACH_POSITION_FIELD_SKIP_PERSIST       , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Position fields skipped from persistence.") \
    X(FOREACH_BACKTEST_METRIC                   , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Backtest summary metrics.") \
    X(FOREACH_CALIB_LOG_COL                     , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Calibration log CSV columns.") \
    X(FOREACH_TRADE_LOG_COL                     , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Trade log CSV columns.") \
    X(FOREACH_CONFIDENCE_PERSIST_FIELD          , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Confidence persistence fields.") \
    X(FOREACH_REGIME_PERSIST_FIELD              , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "RegimeState snapshot-persist fields (7-field sharded delegate; E.1.2 Step-2, mirrors FOREACH_CONFIDENCE_PERSIST_FIELD).") \
    X(FOREACH_FEEDER_PERSIST_FIELD              , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "RegressionFeederX snapshot-persist fields (3-field sharded delegate; E.1.2 Step-2).") \
    X(FOREACH_NODE_PERSIST_FIELD                , 1,      FOREACH_REGISTRY                 , "STRUCT:NodeContext<64>",              "The ordered per-node persist WIRE SPEC (E.1.2 D-305; 29 rows incl. PAD/BIT + the regime/feeder/confidence DELEGATE rows at their wire positions; hybrid flat+delegates per D-291/BLK-1; SAVE/READ/COMMIT projections drive ShardedSnapshotPersist).") \
    X(FOREACH_NODE_CTX_PERSIST_EXEMPT           , 1,      FOREACH_REGISTRY                 , "STRUCT:NodeContext<64>",              "The COMPLEMENT of FOREACH_NODE_PERSIST_FIELD (E.1.2 D-421): the 22 NodeContext<F> members deliberately NOT on the wire, each with a closed-set falsifiable CATEGORY + a refutable rationale. Exists because a coverage registry over a HAND-declared struct answers 'are these rows right?' and never 'are these ALL the rows?' — subtracting BOTH registries from clang's real member list is what makes a never-enrolled field visible (tools/check_node_ctx_partition.py; UNACCOUNTED / STALE-EXEMPT / CONTRADICTION).") \
    /* v5.15.5.F.4d.1.B.3 Step 3 (2026-05-24): FOREACH_CFG_DERIVED_INFERENCE_CFG row DELETED at .B.3 — registry file deleted at Step 2; cfg-derived consumer framework + StampBoundDerivedFilter at .B.1+ supersede. */ \
    X(FOREACH_CFG_DRIFT_CHECK                   , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Stamp body drift check fields (legacy; folds into framework at .B.3).") \
    X(FOREACH_CFG_GATE_PER_NODE                 , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Per-row gate_when override sidecar for STAMP_BOUND_CFG_DERIVED-flagged per-core cfg fields (.B.1+; H18 first canonical of gate-type sidecar; empty at .B.1; populates at .B.2 cohort migration).") \
    X(FOREACH_CFG_GATE_GLOBAL                   , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Per-row gate_when override sidecar for STAMP_BOUND_CFG_DERIVED-flagged global cfg fields (.B.1+; sister to FOREACH_CFG_GATE_PER_NODE).") \
    X(FOREACH_STAMP_BOUND_DERIVED_COHORT        , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Action-parameterized meta-walker for STAMP_BOUND_CFG_DERIVED cohort (.B.3 Step 1.6.5b; dispatches to 4 cfg registries: per_node + global + ml_cfg_flag + gate_cfg_flag); single source of truth for cohort coverage; used by 4 cfg-derived consumer template fns + STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN struct-gen meta-walker. Drift impossible by construction per FOREACH_<COHORT>_COHORT pattern at cfg-derived-consumer-framework.md v1.2.") \
    X(FOREACH_ARCH_FIELD_DRIFT                  , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Architectural field drift check.") \
    X(FOREACH_SLOW_PATH_GATE                    , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Slow-path gate registry (cfg-flag eligibility canon).") \
    X(FOREACH_SP_SECTION                        , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Slow-path section enum (regime/rebuild/etc.).") \
    X(FOREACH_PANEL                             , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "GUI panel registry.") \
    X(FOREACH_ROLLING_WINDOW                    , 1,      FOREACH_REGISTRY                 , "UNCLASSIFIED",              "Rolling window template variant registry.") \
    X(FOREACH_KILL_TRIP_SITE                    , 1,      FOREACH_REGISTRY                 , "SSOT",              "GLOBAL fatal-trip sites of AggregatorState::kill_trip_request (D-479 as amended; the KTS_* enum + name table the OEVT_RING_FULL_FATAL marker + the Health_Log CRITICAL record persist; enum:KillTripSite in the H21 ledger).")

#undef ROOT_NONE
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]_[purpose + enforcement history]
//----------------------------------------------------------------------
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
// FUTURE ADD: at .F.4d, FOREACH_REGISTRY itself gets reorganized when more registries land
// (.F.4d FOREACH_DERIVED_FILTER + sidecar override + further meta-registries). This file is the
// codebase-wide enforcement seed; each addition is 1 row.
//
// SEE: DESIGN_SPECS/meta-registry-pattern-for-codebase-registry-discipline.md
//======================================================================
// [DERIVED]   (tool-refreshed — ROW_COUNT/CONSUMERS generators land with the drift-gate generalization; empty skeleton is correct, D-327)
//======================================================================
// [END_REGISTRY]_[FOREACH_REGISTRY]
//======================================================================

#endif // META_REGISTRY_HPP
