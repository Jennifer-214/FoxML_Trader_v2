#pragma once
// single source of truth for version string
// update HERE only — all renderers include this

#define ENGINE_VERSION_MAJOR 5
#define ENGINE_VERSION_MINOR 15
#define ENGINE_VERSION_PATCH 5
#define ENGINE_VERSION_STRING "5.15.5.F.4d.1.B.2.h1"

// .F.4d.1.B.2.h1 (v5.15.5.F.4d.1.B.2.h1 — hotfix; 2026-05-24)
// === LIVE-SAFETY HOTFIX — closes PARITY-026 ===
//
// Issue: CoreFrameworks/EngineSharded.hpp:742 calls only EventLoopState_Init at boot
// but NEVER follows with EventLoopState_ConfigureKillSwitch. Backtest/BacktestSharded.hpp:217-221
// correctly configures the kill switch from cfg.risk_cfg_flags + cfg.kill_switch_drawdown_pct.
// Result: operator sets kill_switch_enabled=1 + drawdown_pct=5.0 in engine.cfg, backtest
// replays trip the switch correctly, but LIVE TRADING IGNORES BOTH FIELDS. Worst-case silent
// unbounded loss for unattended live runs.
//
// Has been broken since the sharded path was built; sprint named v5.15-LIVE-READINESS made
// the gap especially ironic. Surfaced 2026-05-24 by an ad-hoc ML↔LIVE structural sweep
// (Caramel-prompted) that surfaced 4 sister train-serve-asymmetry CRITs at the boot surface.
//
// Fix: 5-LOC mirror of backtest discipline inserted at EngineSharded.hpp:742 after
// EventLoopState_Init. Build clean; 3230 tests pass (unchanged from .B.2 baseline);
// hot path UNTOUCHED.
//
// Sister findings closing at v5.15.5.F.4d.1.B.4 (NEW sub-ship; train-serve-execution-layer-parity
// structural extract via EngineCommon_BootPerCore + EngineCommon_SlowPathCycleOneCore shared
// helpers per TECH_DEBT-119): PARITY-027 (backtest no ML exit-prediction submit) / PARITY-028
// (backtest no ConfidenceScorer composite cfg bind) / PARITY-029 (backtest no Strategy_InitPerCore
// — pre-v5.4 F7 bug never closed on backtest) + 3 HIGH PARITY-030/031/B2 by 1 structural refactor.
//
// M5 meta-discipline codification queued at .B.4 ship close: NEW DESIGN_SPECS
// train-serve-execution-layer-parity.md (audit methodology) + shared-helper-extract-for-train-serve-mirror-close.md
// (the implementation pattern). DRAFT v0.1 already written 2026-05-24; Stage 3 first canonical
// at .B.4 ship.
//
// Full audit report: plans/v5.15-live-readiness/plan_checks/2026-05-24-train-serve-asymmetry-sweep.md
// PARITY-026 entry: DOCS/PARITY_ISSUES.md
// Operator workaround pre-hotfix: monitor drawdown manually in live trading (not safe for unattended).
//
// .F.4d.1.B.2 (v5.15.5.F.4d.1.B.2) — Cohort migration (2 of 3 in .B split; 2026-05-17).
//
// LANDED at .B.2:
// - 19 master per-core + 1 master global + 4 ML_CFG_FLAG BITMAP_BIT = 24 cohort fields
//   flag STAMP_BOUND_CFG_DERIVED metadata bit (Step 1 + Step 4 + ml_buy_threshold Step 3.1
//   + gap_acceptable_threshold registry row Step 2 partial)
// - FOREACH_ML_CFG_FLAG 5→6 sig migration (metadata_flags column; 4 consumer X-macros migrated)
// - Framework extended to walk FOREACH_ML_CFG_FLAG (Step 0.5b)
// - tt:: dispatch extended: SrcT/DstT/HasT for cfg_populate_inf_field (FPN→double conversion);
//   StampT/CfgT for cfg_drift_compare (coding-time discoveries 1 + 2)
// - wire_format_invariants helper extended to dual-mask shape (Stage 3 second reference)
// - 6 COHORT_GATE_* macros extracted at MlCfgFlagRegistry.hpp; FOREACH_STAMP_BOUND_CFG +
//   FOREACH_CFG_GATE_PER_CORE reference shared macros (Path γ #3 partial closure; 2 of 3
//   registries unified — CfgDriftCheck has distinct semantics preserved)
// - FOREACH_CFG_GATE_PER_CORE populated with 16 cohort gate entries (Step 5)
// - Winsor parse-time cross-field invariant validation (Step 6)
// - FPN<F> 6 comparison operators (==, !=, <, <=, >, >=) at FixedPointN.hpp (Step 6.5;
//   removes FPN_ToDouble workaround pattern codebase-wide)
// - GUI/SettingsPanel.hpp gap_acceptable_threshold manual render entry DELETED
// - Framework prefix drop at CfgGateRegistry.hpp template fns (Decision 5)
// - 4 test fixture updates (.A/.B.1 strengthened to .B.2 reality per /test-strength-audit)
// - 22 new substantive correctness tests (Step 9): FPN<F> operators + cohort gate
//   predicates under various cfg states + framework walker content verification +
//   Winsor invariant edge cases
//
// Tests: 3235 controller_test passed (3213 baseline + 22 new) + 17 depth_recorder_test
// GREEN. 5 binaries (test/gui/suite/tsan/asan) clean. Hot path UNTOUCHED. CI:
// check_meta_registry.py 3 PASS (65/65 enrolled); check_per_core_registry_integrity.py
// 6 PASS (0 Class 27 exemptions).
//
// DEFERRED to .B.3 (8 items; each with concrete TECH_DEBT entry — TECH_DEBT-09X series):
// 1. Step 2 manual cfg storage cleanup (gap_acceptable_threshold decl/default/parser at
//    ControllerConfig.hpp:889/:1729/:2554) — STRUCTURAL: FOREACH_GLOBAL_CFG_FIELD doesn't
//    auto-gen struct fields; deletion requires cfg-storage-discipline amendment + struct-gen
//    extension at .B.3 or .F.4f
// 2. Step 3 retroactive .A.7 cohort + bandit_blend_ratio bit-add (5 prefixed-only fields:
//    ml_tp_pct + ml_sl_pct + barrier_blend_mode + bandit_blend_ratio + per_horizon_barrier_blend)
//    — STRUCTURAL: only POST_CFG-prefixed inf fields exist; framework can't walk without
//    inf struct unification; .B.3 legacy POST_CFG deletion forces unification
// 3. Step 7.1/7.2/7.3 ModelInference struct-gen migrations (3 sites) — STRUCTURAL: tied
//    to .B.3 legacy registry deletion + struct-gen mechanism choice (approach A/B/C unresolved)
// 4. Step 7.4 production canonical body emit migration (ModelInference.hpp:1788) —
//    STRUCTURAL: coupled with stamp_format_version bump (Step 8); .B.3 legacy registry
//    deletion FORCES this migration (build BREAKS without it)
// 5. Step 7.5 StampHelper.hpp:156 STAMP_CFG_AUTOPOPULATE migration — STRUCTURAL: legacy
//    walker writes legacy struct fields; .B.3 deletion forces migration
// 6. Step 7.6 CoreModelZoo.hpp:243 drift walker migration — STRUCTURAL: reason-field
//    semantic preservation requires framework extension; .B.3 framework can grow reason
//    arg OR migration accepts behavior change
// 7. Step 8 stamp_format_version 5 sub-steps (constant extraction + bounds check + bump +
//    fixture test + DESIGN_SPEC amendment) — STRUCTURAL: coupled with Step 7.4
// 8. per_horizon_barrier_blend ML_CFG_FLAG STAMP_BOUND_CFG_DERIVED bit-add — STRUCTURAL:
//    no unprefixed inf field; same root cause as item 2 (inf struct unification at .B.3)
//
// 10 coding-time discoveries documented in postmortem at
// plans/v5.15-live-readiness/postmortems/2026-05-17-v5.15.5.F.4d.1.B.2-postmortem.md.
//
// Bug classes closed at .B.2:
// - Class 14 / 18 / 21 at cfg-derived consumer surface (cohort migration validates framework
//   non-empty across 3 registries; 24 fields auto-flow through unified consumer macros)
// - Path γ #3 across cohort-gate-predicate registries PARTIAL closure (2 of 3 registries
//   reference shared COHORT_GATE_* macros; CfgDriftCheck has distinct semantic preserved)
// - Class 24 strengthened (ml_buy_threshold pre-canonical parity gap closed)
//
// New DESIGN_SPECs amendments at ship close: cfg-derived-consumer-framework.md v1.2
// (first non-empty cohort walk); canonical-sister-extension-discipline.md v1.1 (2nd canonical);
// future-oriented-plan-template.md v1.1 (first NEW plan from inception).
//
// Per per-sub-ship cycle: postmortem at
// plans/v5.15-live-readiness/postmortems/2026-05-17-v5.15.5.F.4d.1.B.2-postmortem.md.

// .F.4d.1.B.1 (v5.15.5.F.4d.1.B.1) — Framework consolidation ship (2026-05-17).
// First of 3 split sub-ships of v5.15.5.F.4d.1.B (.B.1 framework / .B.2 cohort migration /
// .B.3 legacy empty-out) per /readiness audit recommendation + CRIT-1 wider-scope acceptance
// (Path γ-class structural critique #2 caught at Batch 1 audit: original .B v1.2 β4 sparse
// sidecar duplicated canonical FOREACH_CFG_DERIVED_INFERENCE_CFG; eliminated entirely).
//
// LANDED at .B.1 (framework infrastructure):
// - NEW MemHeaders/CfgGateRegistry.hpp (gate-type sparse sidecar; H18 first canonical of
//   gate-type vs severity-type sidecar; FOREACH_CFG_GATE_PER_CORE + _GLOBAL empty at .B.1;
//   populate at .B.2 cohort migration)
// - NEW 3 derived-filter consumer template fns in cfg_derived:: namespace
//   (populate_inference_cfg_from_derived + populate_stamp_cfg_from_derived +
//   drift_check_from_derived) wrapped in macros INFERENCE_CFG_POPULATE_FROM_DERIVED +
//   STAMP_CFG_POPULATE_FROM_DERIVED + DRIFT_CHECK_FROM_DERIVED
// - EXTEND tt:: dispatch quartet → septet in CoreFrameworks/CfgFieldDispatch.hpp
//   (NEW: cfg_emit_field + cfg_populate_inf_field + cfg_drift_compare)
// - NEW 2 DESIGN_SPECs Stage 3 ACTIVE: canonical-sister-extension-discipline +
//   cfg-derived-consumer-framework
// - NEW 1 DESIGN_SPEC Stage 3 ACTIVE: future-oriented-plan-template (.B.1 plan body v1.1
//   retrofit demonstrates the template's required sections)
// - NEW skill /plan-draft Stage 2 DRAFT (scaffolds plan bodies from template)
// - PROMOTE /anti-spaghetti SKILL.md to Stage 3 ACTIVE (first canonical run validated at
//   Batch 2 audit)
// - 4 NEW going-forward rules in CLAUDE.local.md (audit canonical sister + plans cite
//   sister + anti-spaghetti cadence + new plans use template)
// - 4 NEW memory files codifying the discipline + MEMORY.md index update
// - 5-row addition to FOREACH_REGISTRY meta-registry (+2 sidecars enrolled; H15)
// - 14 walker integration tests (controller_test.cpp ~26140; vacuous PASS at 0-row walker)
//
// DEFERRED to .B.3 (per coding-time discovery; documented in .B.3 plan body Step 1.5):
// - StampHelper.hpp:183 INFERENCE_CFG_AUTOPOPULATE → _FROM_DERIVED swap (PARITY-020 regression
//   risk if swapped at .B.1 with 0-row walker)
// - controller_test.cpp:24962-25047 A.7 test swap (same reason)
//
// Tests: 3215 controller_test passed (3196 + 19 new) + 17 depth_recorder_test GREEN.
// 5 binaries (test/gui/suite/tsan/asan) clean. Hot path UNTOUCHED. CI:
// check_meta_registry.py 3 checks PASS (65/65 enrolled); check_per_core_registry_integrity.py
// 6 structural checks PASS (0 Class 27 exemptions).
//
// Closes Class 14/18/21 structurally at gate-type-sidecar surface; sets up .B.2 cohort
// migration as 1-row mechanical changes per migrated field.
//
// 3-tier defense against future parallel-infrastructure drift now in force:
//   (1) future-oriented-plan-template required sections at draft time (canonical sister
//       audit + design space alternatives + bug class closure tracking);
//   (2) /precoding-audit-gate before tag (5 audits + canonical-sister discipline);
//   (3) /anti-spaghetti quarterly + post-codification cadence.
//
// Per per-sub-ship cycle discipline: postmortem at
// plans/v5.15-live-readiness/postmortems/2026-05-17-v5.15.5.F.4d.1.B.1-postmortem.md.

// .F.4d.1.A (v5.15.5.F.4d.1.A) — Path γ+ v2 framework infra ship (2026-05-17).
// First sub-ship of v5.15.5.F.4d.1 umbrella (TECH_DEBT-085 Thread A FULL framework
// consolidation). Path γ correction landed at pre-coding audit gate: `.A` v1.2
// originally proposed building DerivedFilterFramework.hpp parallel walker macros +
// DerivedFilterRoster.hpp Level-1 meta-registry. /merge-scan caught the structural
// critique: proposed runtime walker duplicates existing compile-time infrastructure
// at CfgFieldRegistry.hpp:1020-1159 (FOREACH_METADATA_BIT + cfg_compute_mask +
// CFG_FIELD_FOR_EACH_SET_BIT auto-gen masks + branchless TZCNT; since .F.4c.3).
// Path γ pivots: 1-row addition to FOREACH_METADATA_BIT + ~50 LOC consumer over
// existing infra + reusable wire_format invariants helper + composition audit registry
// + H16 compile-time static_assert + cli_explain_mask bug fix + stamp_emit_mask alias
// delete + 3 DRIFT-MAJOR DESIGN_SPECs status amendments + 2 NEW DESIGN_SPECs Stage 3
// first reference (composed-filter-mask-pattern + wire-format-canonical-body-invariants-
// helper) + 6 TECH_DEBT entries opened (-087/-088/-089/-090/-091/-092). Per Caramel
// triage 2026-05-17 + auto-pick-future-oriented principle + per-sub-ship cycle locked.
//
// Tests: 3196 controller_test passed (3174 + 22 new) + 17 depth_recorder_test GREEN.
// 5 binaries (test/gui/suite/tsan/asan) clean. Hot path UNTOUCHED (calls_graph_diff
// empty). CI: check_meta_registry.py 3 checks PASS (63/63 enrolled);
// check_per_core_registry_integrity.py 6 structural checks PASS (0 Class 27 exemptions).
//
// Closes Class 11/14/18/21/22 structurally at framework layer; strengthens Class 28
// (branchless TZCNT); pre-emptive Gap 1 closure (composition discipline blindspot).
// .F.4d MERGED predecessor (engine 545b087 + GPG-signed tag).
// .F.4d.1.B successor (migration + consumer cycle).
// Sub-master: plans/v5.15-live-readiness/subplans/2026-05-16-v5.15.5.F.4d.1-thread-a-framework-full.md
//
// Thread B (FULL — bandit/thompson 5-state + dispatch tables + Class 24/28/29/30 closures):
//   1. Pattern 5 Thompson_Update branchless dispatch + sink fns (noop_thompson_update +
//      real_thompson_update; ezoo->buy_thompson_update_fn + exit_thompson_update_fn fields).
//   2. FOREACH_BANDIT_SIDE meta-X-macro (NEW header ML_Headers/bandit_dispatch_table.hpp) +
//      g_buy_reward_dispatch + g_exit_reward_dispatch tables auto-derived via ?: chain over
//      FOREACH_BANDIT_ALGORITHM metadata (exp3_up, thompson_up); 3 leaf reward fns
//      (exp3_only / thompson_only / both) templated on BanditSide for if-constexpr field
//      selection (no runtime branch on side). First canonical of FOREACH_BANDIT_SIDE pattern.
//   3. FOREACH_BANDIT_ALGORITHM 7-arg metadata 3→5 states (EXP3 / THOMPSON /
//      EXP3_OP_THOMPSON_GHOST / THOMPSON_OP_EXP3_GHOST / BLENDED). Auto-derived
//      BANDIT_EXP3_UPDATE_MASK (0x1D) + BANDIT_THOMPSON_UPDATE_MASK (0x1E) constants via
//      X-macro reduction over metadata bits. Option C wire-byte preservation: cfg=0/1/2
//      unchanged from pre-.F.4d encoding.
//   4. Class 24 closure at attribution surface — Thompson_Update wired via
//      g_buy_reward_dispatch / g_exit_reward_dispatch dispatch tables; pre-fix Thompson posterior
//      was silently frozen despite cfg.bandit_algorithm setting (capability-cfg surface mismatch).
//      Reward attribution callers at StrategyParameters.hpp:1154 (TickRewardsFromLookback) +
//      ControllerEventLoop.hpp:1695 (TradeCloseReward) + :1755 (exit-side) route via dispatch.
//   5. Order::flags_packed bandit context bits 17-25 (Pattern 4 decision-time-data-binding;
//      sister to MASK_ORDER_PRE_RESOLVED at bit 16). MBS_OrderBanditActiveState /
//      MBS_OrderBanditRegime / MBS_OrderBanditChosenArm accessors. Set at submit time
//      via MBS_OrderSetBanditContext; read at calib log emit time. 5th canonical of
//      multi-bit-state-encoding INVARIANT.
//   6. FOREACH_OMS_PER_SLOT_FIELD 3→5 rows (Class 30 latent drift closure) — last_exit_fee
//      enrolled (previously sibling-array unregistered drift risk from `.F.4c.3` r-4) +
//      bandit_reward_bps[MAX_PORTFOLIO_POSITIONS] NEW per-slot reward signal.
//   7. § F Pattern 5 path consolidation — OmsState ezoo_refs[MAX_EXECUTION_CORES] +
//      core_cfg_refs[MAX_EXECUTION_CORES] per-core arrays (engine-wide single OmsState;
//      per-core lookup indexed by Order::core_id at calib log emit time).
//      EngineSharded boot wires oms.ezoo_refs[i] + core_cfg_refs[i] alongside per-core
//      state.cores[i].ensemble_handle. real_on_exit_calibration body extended with
//      bandit context decode + per-slot reward attribution + per-core ezoo/cfg cast +
//      Bandit_GetProbabilities for exp3_probs + null-coalesced Thompson posterior telemetry.
//   8. § M Calib log columns 9→47 — 6 bandit-context singletons (bandit_algorithm +
//      regime_id_at_emit + chosen_arm + reward_bps_attributed + thompson_telemetry_arm +
//      thompson_exp3_blend_alpha) + 32 per-arm cols (8 arms × {exp3_w, thompson_mu,
//      thompson_prec, thompson_pulls}; arm-major layout; hand-written per sidecar M.2
//      preprocessor token-paste robustness fallback).
//   9. Class 28 cmov sweep (6 sites) — Bandit_Update max-find + Thompson_Sample argmax +
//      ModelInference PredictBest argmax + WeightedBlend argmax + RollingTurnover argmax;
//      __builtin_expect rare on Thompson_Update bounds guard.
//  10. Class 25 consumer sweep at reward attribution surface — TickRewardsFromLookback +
//      TradeCloseReward + ControllerEventLoop exit-side. PerCoreCfg<F>* threaded through.
//  11. thompson_exp3_blend_alpha NEW per-core cfg field (FOREACH_PER_CORE_CFG_FIELD row) +
//      5 drift rows (PARITY-026 close) + 5 POST_CFG entries + 5 cfg→inf wiring rows +
//      stamp emit row (only emits when bandit_algorithm==4 — preserves HMAC byte
//      equivalence for legacy stamps).
//  12. thompson_exit_bandits hand-mirror (renamed to exit_thompson_bandits per
//      TECH_DEBT-084) + new MASK_EZOO_EXIT_THOMPSON_READY init flag.
//  13. 15 Step 9 tests (26 assertions) — Class 24 closure regression + dispatch table
//      verification + cmov regression + 5-state byte-equivalence guards + boundary tests
//      (BLENDED α=0 reduces to EXP3; α=1 reduces to Thompson softmax) + exit-side mirror.
//
// Thread A foundation (FULL CLOSURE deferred to TECH_DEBT-085 .F.4d.1 dedicated ship):
//   - H15-H20 codified in CLAUDE.md hard invariants table (H15 every X-macro registry
//     enrolled in FOREACH_REGISTRY; H16 every metadata bit has derived filter or exemption;
//     H17 cfg struct fields generated from FOREACH_CFG_FIELD; H18 sidecar override pattern;
//     H19 LEVEL > 0 registries declare PARENT; H20 branchless preferred for SP/HP dispatch).
//   - 4 Thread A DESIGN_SPECs exist + Stage 3 ACTIVE: metadata-bit-driven-derived-filter-
//     framework + meta-registry-pattern-for-codebase-registry-discipline + sidecar-override-
//     pattern-for-registry-auto-flows + framework-composition-overview.
//   - FOREACH_BANDIT_SIDE enrolled in FOREACH_REGISTRY meta-registry (H15 first canonical).
//   - STAMP_BOUND_CFG_DERIVED metadata bit reserved (bit 13 on CfgFieldDescriptor::MetadataFlag)
//     for future DERIVED_FILTER framework consumer (no migration this ship).
//   - CLAUDE.md item 31 (framework-driven extensibility meta-principle) codified.
//   - 5 new multi-bit-state-encoding canonicals tracked: Order::pre_resolved sub-struct (canonical #4
//     at .F.4c.3) + Order::flags_packed bandit context bits 17-25 (canonical #5 this ship);
//     canonicals 6/7/8 (DriftOverride / RegistryRosterEntry / ManualFieldInventoryEntry) defer
//     to TECH_DEBT-085 along with FOREACH_DRIFT_OVERRIDE sidecar.
//
// 3 substantial TECH_DEBT fold-ins (closed this ship):
//   - TECH_DEBT-082 — 3 .F.5 residual fields (lazy_rebuild_price_threshold_pct + exit_threshold +
//     confidence_ic_floor) migrate from flat ControllerConfig + manual strcmp/atof parser cases
//     to FOREACH_PER_CORE_CFG_FIELD X-macro registry rows (auto-flow parser via tt::cfg_*_field<T>
//     dispatch). Class 23 manual-parser anti-pattern closure at 3 sites. Closes .F.5 charter
//     residual completely.
//   - TECH_DEBT-083 — IWYU sweep: 7 headers add explicit <cstdint> (CoreModelZoo + ModelInference +
//     RewardTracker + WelfordStats + MeanReversion + Momentum + RegimeDetector). 8th header
//     (StampBoundModelConstRegistry) already fixed inline during session. Closes latent class
//     structurally (any future include-order change won't expose new chain-breakers).
//   - TECH_DEBT-084 — FOREACH_BANDIT_SIDE cascade rename (6 patterns in collision-safe order
//     across 14 files; word-boundary sed; 200+ refs): thompson_exit_bandits → exit_thompson_bandits +
//     last_predicted_thompson_arm → last_predicted_buy_thompson_arm + MASK_EZOO_THOMPSON_READY →
//     MASK_EZOO_BUY_THOMPSON_READY + EnsembleModelZoo_InitThompsonBandits →
//     EnsembleModelZoo_InitBuyThompsonBandits + thompson_bandits → buy_thompson_bandits +
//     thompson_update_fn → buy_thompson_update_fn. Plus persistence file path rename
//     (thompson_state.json → buy_thompson_state.json + thompson_exit_state.json →
//     exit_thompson_state.json) with Load-side back-compat alias for existing on-disk model bundles.
//     Closes naming asymmetry; future 3rd-side axis addition becomes mechanical.
//
// Bug classes closed this ship: Class 24 (sister at reward attribution) + Class 25 (consumer
// sweep) + Class 28 (6 cmov sites) + Class 30 (sibling-array enrollment drift). Class 27 +
// Class 29 preserved via decision-time-data-binding discipline. PARITY-026 closed (4
// STAMP_BOUND bandit/thompson fields gained drift-check rows; plus 1 NEW blend_alpha).
//
// Tests: 3174 controller_test (3148 baseline + 26 Step 9 assertions) + 17 depth_recorder_test;
// all 5 binaries (test, gui, suite, tsan, asan) build clean; check_per_core_registry_integrity.py
// 6 checks PASS (Check 7 Class 27 prevention: 0 Section C exemptions); check_meta_registry.py
// 3 checks PASS (63 macros / 63 enrolled). Hot path UNTOUCHED.
//
// Residual logged as TECH_DEBT-085 (full Thread A consolidation; ~15-25h focused; dedicated
// .F.4d.1 ship after .F.4d): DerivedFilterFramework.hpp 3 macro variants + StampBoundDerivedFilter.hpp
// first canonical + Layer 5b hash lock + 24-row FOREACH_STAMP_BOUND_CFG migration with always-emit
// semantic shift + legacy registry empty-out + FOREACH_ML_CFG_FLAG 5→6 arg sig migration (12 rows
// gain metadata_flags column) + FOREACH_DRIFT_OVERRIDE sparse sidecar + bit-packed DriftOverride +
// RegistryRosterEntry + ManualFieldInventoryEntry (canonicals 6/7/8) + CI Checks 9-12 + v5.14 stamp
// fixture regression test + 12+ consumer migration sites + CFG_DRIFT_AUTOPOPULATE companion macro.
//
// .F.4c.3 (v5.15.5.F.4c.3) — Architectural cfg split + Class 27/28 structural closure +
// comprehensively branchless OrderManager (2026-05-15). B.1 final ship close.
//
// Major architectural wins (12 class closures + 12 structural improvements):
//   1. Class 27 (OMS scalar cfg-mirror cache) — STRUCTURAL CLOSE via Order pre_resolved
//      sub-struct + OMS scalar field deletion (fee_rate/maker/taker/slippage_pct DELETED).
//      CI Check 7 enforces; 0 Section C exemptions at ship close.
//   2. Class 28 (branchy SP/HP data-dependent dispatch) — first canonicals: HandleFill
//      Pattern 1 1D type dispatch table (BUY/SELL); was_win_bitmap mask-select (Pattern 3);
//      ShardedLiveSafety force-close bitmap iteration; FlattenAll partial_on shift; Reconcile
//      branchless bitmap-search for origin_core_id via match-mask + tzcnt (Pattern 3 sub-variant
//      first canonical). 5+ Pattern 3 instances; 1 Pattern 1 instance; Pattern 5 first canonical.
//   3. Silent zero-fee-rate class — closed via Order_BindPreResolved + TT_ASSERT_PRE_RESOLVED_BOUND
//      runtime warn at HandleFill entry. Production catches forgotten-bind via __builtin_expect
//      branch (~0ns predicted-correctly).
//   4. SubmitCommand POD canonical arg — unified OMS submit-API + SPSC wire-format (option l
//      per orchestration-helper-with-pod-args-pattern.md 2nd canonical). 21 caller migrations.
//   5. DrainPostFill recompute-from-cfg gap — closed via OMS::last_exit_fee[] sibling array
//      (HandleFill SELL writes authoritative exit_fee from Order pre_resolved; DrainPostFill
//      READS the canonical value). Per decision-time-data-binding-pattern.md.
//   6. Pattern 5 sink-fn-pointer architecture for optional side effects — eliminates
//      `if (oms->trade_log)` + `if (oms->calibration_log_file)` callsite branches. Default =
//      noop fns; set-to-real at boot when subsystem enables. NEW DESIGN_SPEC at .F.4c.3 close.
//   7. AccountMakerTakerFee fully branchless (mask-select counter incs + cmov fee buckets).
//   8. ShardedLiveSafety_ForceClose + FlattenAll: `const PerCoreCfg<F>* cores` REQUIRED param
//      (per cfg-scope-discipline § "consumer over per-core array" — first canonical).
//   9. Reconcile_ApplyMissedFills nullable cfg pointer pattern (per cfg-scope-discipline
//      § "recovery-path nullable pointer" — first canonical).
//  10. EventLoopState_FeeRate dead getter deleted; OrderManager_Init `fee_rate` param removed
//      (~37 callers migrated); OMS_INIT_AUTOPOPULATE clean.
//  11. flags_packed widened uint16_t → uint32_t (padding absorbs; cache-neutral; same 320B Order
//      size) + MASK_ORDER_PRE_RESOLVED bit.
//  12. EventLoop_OnEvent combined-mask collapse (3 guard branches → 1) + branchless slippage
//      application (Pattern 3 mask-select on entry/exit sign; FPN_Negate).
//
// Comprehensive branchless OrderManager body (r-6 phases 1 + 2):
//   - All inner data-dependent if/else converted to mask-select / cmov / fn-pointer dispatch
//   - Only remaining branches: H20 exception #4 (bounds guards + slot-race + `__builtin_expect`)
//   - Pattern 5 sink-fn-pointer eliminates trade_log + calibration_log_file callsite branches
//
// Deferred (logged as TECH_DEBT for future ships):
//   - Compile-time SubmitCommand required-field enforcement (C++17 friend-scope wall; awaits
//     C++20 concepts ship)
//   - C++20 upgrade as dedicated infrastructure ship post-v5.15 umbrella
//   - Per-symbol axis as expected evolution per per-instance-registry-pattern.md
//
// Tests: 3148 controller_test + 17 depth_recorder_test + 856 parity_harness — all GREEN.
// CI Check 7 PASS with 0 Section C exemptions. Hot path UNTOUCHED (calls_graph_diff verify at r-7).
//
// .F.4c.1 (v5.15.5.F.4c.1) — Paper-test polish + Class 24 codification (2026-05-15).
// Three structural changes:
//   1. ImGui widget-ID structural fix at tt::cfg_render_field<T> + residual field_defs[]
//      path (GUI/SettingsPanel.hpp) — wraps every ImGui call in PushID(cfg_field_name)
//      so display labels can repeat safely across sections/per-core tabs without
//      colliding at ImGui's hash table. Closes paper-test runtime "name is already
//      taken" regression caught 2026-05-14. Compile-time cfg_field_names_unique()
//      static_assert in CfgFieldRegistry.hpp documents the contract.
//   2. 18-row STAMP_BOUND cohort migration to FOREACH_CFG_FIELD (15 FPN<F> doubles
//      + 3 INT_ENUMs: ridge_lambda/cost_penalty/min_ic_floor, winsor_pct_low/high,
//      confidence_freshness_tau_secs/capacity_target_dollars/capacity_kappa/
//      rmse_baseline, thompson_mu_prior/precision_prior/precision_obs,
//      bandit_algorithm + risk_degradation_curve + trading_mode + 3 risk thresholds).
//      All STAMP_BOUND-pre-tagged for .F.4d derived filter consumption. INT_ENUM
//      fields with string-form parsers (bandit_algorithm/risk_degradation_curve/
//      trading_mode) tagged HAS_SIDE_EFFECT so registry walker skips parse —
//      manual parser blocks preserved. + reconcile_dry_run DEPRECATED row.
//   3. Class 24 (Capability-cfg surface mismatch) codified in
//      DOCS/RECURRING_BUG_PATTERNS.md + cfg↔ML surface-alignment audit
//      going-forward rule in CLAUDE.local.md. Closes the recurring class
//      structurally — every ML capability now answers four columns (cfg parse /
//      Settings render / stamp tag / per-core override) at PR-review time.
// Reset+Modified UI deferred to follow-up (FPN<F> operator== vs ImGui ImTextureRef
// ambiguity needs investigation; tt::cfg_assign/cfg_diff primitives ready).
// 3144 tests pass (matches .F.4c baseline). Hot path UNTOUCHED. Per-ship
// Version.hpp bump discipline maintained.
