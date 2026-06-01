#pragma once
// single source of truth for version string
// update HERE only — all renderers include this

#define ENGINE_VERSION_MAJOR 5
#define ENGINE_VERSION_MINOR 15
#define ENGINE_VERSION_PATCH 5
#define ENGINE_VERSION_STRING "5.15.5.F.4d.1.E.0.6"

// PUBLIC RELEASE VERSION — display-only; distinct from the granular internal ENGINE_VERSION above.
// WHY two numbers: the engine version tracks the internal sprint cadence AND is WIRE-BOUND (embedded
// in model stamps / fingerprints for reproducibility + HMAC — StampHelper/CoreModelZoo/ModelInference),
// so it can't double as a clean public label. The release version is the public face (boot banner / GUI),
// bumped MANUALLY at MAJOR changes. It is NEVER written into a stamp/fingerprint/wire body (re-tagging a
// release must never perturb determinism — H9). Pre-1.0 until live-trading + the headless decoupling land.
#define RELEASE_VERSION_MAJOR 0
#define RELEASE_VERSION_MINOR 3
#define RELEASE_VERSION_STRING "0.3"

// .F.4d.1.E.0.6 (v5.15.5.F.4d.1.E.0.6 — tag monotonic after E.0.5 per D-88; plan keeps its semantic
// .E.0.1 name = the FP+replay+locale determinism NET, Net-2 of D-73). Ships the deterministic-input
// foundation that gates #11 (decimal numeric core) + .E.1 (Core→Node rename): F-056/57/58
// (deterministic sqrt under USE_NATIVE_128 + tests build the shipped flags + memcpy-not-pointer-pun)
// + F-054/55 (locale-immune replay parse) + recorder to_chars emit + F-076 zero-init cfg-fingerprint
// ctor (padded struct → ctor is the guard) + the LOCALE-DETERMINISM class close (boot pin + de-race +
// guard + locale-determinism-discipline.md + Classes 37/38/39). Standing CI: tools/check_determinism.sh
// (FP golden + locale + replay gates) wired into pre-commit Check F + tools/check_determinism_selftest.sh
// (the net guards the net — proves each gate catches its regression). Detached-stdin hang class closed
// (exec </dev/null on the hook + det scripts + a guard). SHIP-CLOSE caught + discarded stray agent-
// authored junk (a broken static_assert + a stray doc that a "green" stale build cache had masked) —
// clean rebuild from zero = 3241/0. Hot path UNTOUCHED.
// Postmortem at plans/v5.15-live-readiness/postmortems/2026-05-31-v5.15.5.F.4d.1.E.0.6-postmortem.md.
//
// .F.4d.1.E.0.4 (v5.15.5.F.4d.1.E.0.4) — Doc/meta-system rollout: memories → structured
// tag/frontmatter system (TECH_DEBT-115 memory slice; D-89). 85 memories migrated to
// metadata-nested tags + symmetric sister_specs (CP-1/WH-1 closed mechanically); check_doc_metadata
// extended (machine-portable memory-dir resolver + dual-tree sister resolution + --memories guard,
// wired into /capture-audit Check 12); NEW tools/migrate_memory_frontmatter.py (idempotent,
// undirected-graph, R6-void by preserve-all) + memory template + /doc-create memory type +
// 10 new concern tags + TAG_INDEX/find memory inclusion. META/no engine code; bodies byte-identical.
// Postmortem at plans/v5.15-live-readiness/postmortems/2026-05-30-v5.15.5.F.4d.1.E.0.4-postmortem.md.
//
// .F.4d.1.D (v5.15.5.F.4d.1.D) — Forward-promise verification CI (Check 11 Python detection
// logic IMPLEMENTED) + post-.B.8 ledger cleanup (2026-05-28). NEW `tools/check_forward_promise_audit.py`
// ~870 LOC (24 sentinel patterns + 24 verifier functions + ScanSpec dataclass + section parser +
// exemption mechanism + --since/--strict/--json/--include-archived modes). M7 7th canonical
// structural enforcement application. Phase D retroactive ledger writes: 9 TECH_DEBT entries to
// closed.md (-029/-105/-106/-107/-114/-116/-117/-118/-139) + PARITY-024 status flip + Class 18
// catalog recurrence_count 6→8 + TECH_DEBT-111 trigger update (v5.16+ FOREACH_CLI_MODE alignment).
// Phase F deterministic skill-tool integration cohort: pre-commit hook extension + 5 SKILL.md
// amendments (/capture-audit + /handoff + /close-session + /sync-workspace + /accept-handoff)
// — replaces LLM-orchestrated Check 11 invocation with explicit Python tool calls per
// `feedback_structural_enforcement_when_memory_insufficient`. Phase E DESIGN_SPECS amendments:
// M7 spec v1.3→v1.4 + canonical-sister-extension v1.0→v1.2 + memory Stage 2→3 promotion +
// worked_examples extension + Class 33 sub-shape catalog amendment. Long-term visions captured at
// `plans/_future/`: docs-as-meta-code (structured doc → tool consumption) + context-aware-CLAUDE.md
// (multi-file loaded by harness based on work context). Operator-collaboration cleanup:
// `feedback_address_user_as_caramel.md` memory file created (long-cited going-forward rule
// finally has canonical body). CLAUDE.md + CLAUDE.local.md compressed for cleaner always-loaded
// orientation (CLAUDE.md 514→423 lines; sprint state cells consolidated). Dogfood loop closure
// verified: Check 11 catches its OWN forward-promises mechanically at .D ship close. Sister to
// 2-cycle audit convergence at v1.0→v1.1 amendments + Phase F deterministic skill-tool integration
// cohort closes the LLM-non-determinism gap at handoff-related skill surfaces. Tests baseline
// preserved (3239/0 + 17/0; no engine code touched). Hot path UNTOUCHED. 5-binary build verify
// preserved baseline (no engine changes). 12 CI structural checks PASS (existing 10 in
// check_per_core_registry_integrity + 3 in check_meta_registry + B-Plus pre-commit hook + 5
// sister tools + NEW Check 11). Postmortem at
// `plans/v5.15-live-readiness/postmortems/2026-05-28-v5.15.5.F.4d.1.D-postmortem.md`.
//
// .F.4d.1.B.8 (v5.15.5.F.4d.1.B.8) — Class 26 sub-shape B (UNINDEXED-GLOBAL) accounting cohort
// closure + Stage 6 Check 10 (M7 6th canonical) + Phase H anti-pattern codification + skill amendments
// (2026-05-27). Mixed-ship: 4 HIGH consumer-site fixes + 1 MED display fix + 1 LOW dead-state cleanup
// + NEW CI check + 2 NEW Stage 2 DRAFT memory codifications + NEW Stage 2 DRAFT DESIGN_SPEC +
// 3 skill amendments (/accounting-audit + /capture-audit + /dod-audit).
//
// CRITICAL — Class 26 sub-shape B silent realized-P&L drift CLOSED structurally + mechanically:
//   - 4 HIGH consumer-site fixes (per-core fee_rate_taker substitution + H20 branchless ternary +
//     pre-resolved core_cfg ref per StrategyParameters.hpp:1762 sister-canonical):
//     * ControllerEventLoop.hpp:3605-3606 — EventLoop_TrailingSLRatchetOneCore
//     * ControllerEventLoop.hpp:3670-3671 — EventLoop_BreakevenOnProfitOneCore
//     * StrategyLifecycle.hpp:272-273 — Strategy_WriteRatchetSL (5 callers: MR/Momentum/EmaCross/
//       MLStrategy/ControllerEventLoop)
//     * ControllerEventLoop.hpp:3042-3043 — EventLoop_RebuildOneCore GUI diag (resolved_cfg aliased)
//   - 1 MED display fix: ShardedSnapshot.hpp:249-250 (TUI per-position net_pnl per-core fee_taker)
//   - 3 KEEP-AS-GLOBAL display sites annotated: ShardedSnapshot.hpp:139 + 330 + 331 (Settings panel
//     operator-facing semantic; per-core deviations surfaced via per_core_count panel)
//   - REGRESSION TESTS: tests/controller_test.cpp NEW "Class 26 sub-shape B: UNINDEXED-GLOBAL
//     accounting cohort closure" section (16 NEW assertions; 4 slots × 4 consumer patterns).
//     Tests 3223 → 3239.
//   - STAGE 6 STRUCTURAL ENFORCEMENT: tools/check_per_core_registry_integrity.py NEW Check 10
//     (UNINDEXED-GLOBAL detector for cfg.X / cfg->X / resolved_cfg.X UNINDEXED on per-core-with-global-
//     sister fields). Sister to Check 9 per canonical-sister-extension-discipline.md v1.1
//     CI-tooling-surface axis 2nd canonical. M7 6th canonical structural enforcement.
//   - Class 26 recurrence_count bumped 13 → 17.
//   - DOCUMENTED-RISK PARITY entry at ship close retroactively closes .B.7 forward-promise.
//
// LOW-1 Class 27 vestigial state cleanup: DELETED DrainerConstants.fee_rate_taker_d (B14
// leaves-first ordering; sizeof 24→16; alignof 8→4). TECH_DEBT-138 NEW + CLOSED same ship.
//
// SISTER-COHORT amendments (Phase D per /blindspot-scan HIGH-3): Class 26 + Class 27 + Class 25
// catalog cross-refs distinguishing sub-shapes A vs B; canonical-sister-extension-discipline.md
// v1.0→v1.1 (CI-tooling-surface axis); structural-enforcement-when-memory-insufficient.md
// v1.2→v1.3 (Check 9 retroactive + Check 10 added to canonical_applications).
//
// PHASE H — NEW anti-pattern codification + skill amendments (per Caramel directive "no defer"):
//   - 2 NEW Stage 2 DRAFT memories: sister-cohort-amendment-completeness + forward-promise-auto-write-verification
//   - NEW Stage 2 DRAFT DESIGN_SPEC: sister-cohort-amendment-completeness-discipline.md
//     (AMENDMENT-layer sister to canonical-sister-extension-discipline CREATION layer; recursive
//     enumeration with fixpoint termination criterion per B19 Option C fold)
//   - SKILL amendments: /accounting-audit category 2 sub-shape A/B + /capture-audit NEW Check 11
//     (forward-promise auto-write verification) + /dod-audit per-core surface sub-shape A/B
//   - CLAUDE.local.md going-forward rules: 2 NEW entries + MEMORY.md index updated
//
// AUDIT-DRIVEN 3-CYCLE CONVERGENCE: cycle 1 = 5-agent /precoding-audit-gate (13 findings →
// v1.1); cycle 2 = /readiness GREEN + /blindspot-scan YELLOW recursive (4 findings → v1.2);
// cycle 3 = /blindspot-scan inflection GREEN CONVERGED. Iteration trajectory 9→4→0 findings.
//
// VERIFICATION at ship close: 5 binaries clean / 3239 tests pass / hot path UNTOUCHED /
// 9 CI structural checks PASS (Check 10 NEW sanity-verified via revert-detect-reapply) /
// H4/H6/H7/H12/H15/H20 all PRESERVED per /dod-audit GREEN.
//
// Postmortem at plans/v5.15-live-readiness/postmortems/2026-05-27-v5.15.5.F.4d.1.B.8-postmortem.md.

// .F.4d.1.B.7 (v5.15.5.F.4d.1.B.7) — Folded bugfix + cleanup + C1 file-size umbrella close-out
// (2026-05-27). Mixed-ship per operator directive ("fold .B.8 into .7 i thnk").
//
// CRITICAL — Class 26 silent trading-logic bug CLOSED structurally + mechanically:
//   - HOTFIX: CoreFrameworks/EngineSharded/Async.hpp:814+853 — `cfg.cores[i]` → `cfg.cores[slot]`
//     (`i` was ring-pop counter from inner `for (int i = 0; i < MAX_EVENTS_PER_DRAIN_PER_CORE; ++i)`
//     at Async.hpp:768; should have been outer per-core `slot` at Async.hpp:765). Silent
//     miscalibration of per-core partial_exit_pct + tp2_mult when overrides not set;
//     introduced at mechanical migration commit ea08210 (.F.4c.3 WIP2d-1 Phase 2).
//   - REGRESSION TEST: tests/controller_test.cpp NEW "Class 26: drainer per-core cfg slot integrity"
//     section (8 new assertions; 4 slots × 2 fields). Tests 3215 → 3223.
//   - STAGE 6 STRUCTURAL ENFORCEMENT: tools/check_per_core_registry_integrity.py NEW Check 9
//     (paired-access mismatch detector flags `cfg.core_overrides[X]` + `cfg.cores[Y]` co-located
//     within 5 lines where X != Y). Sanity-verified by reverting fix + tool catching exact site;
//     re-applied fix; tool CLEAN post-fix. M7 4th canonical structural enforcement.
//   - Class 26 recurrence_count bumped 11 → 13; catalog amendment includes detection-signature note.
//   - FORWARD ADVISORY: prior partial_exit_pct/tp2_mult calibration sweeps may have produced
//     tainted results (operators tuned against silently-miscalibrated behavior). DOCUMENTED-RISK
//     PARITY entry at ship close; verification deferred to .F professionalization audit sweep.
//
// CLEANUP (TECH_DEBT-132 + -134 + -131 partial):
//   - TECH_DEBT-132 NEW + CLOSED: 2 dead helpers deleted from CoreFrameworks/ControllerEventLoop.hpp:2225+2259
//     (EventLoop_UpdateRollingStateAllCores + EventLoop_RebuildAllParameters_PerCore; zero production
//     callers post-.B.4 SHARDED full-surface deletion). 2 sister stale comment refs cleaned up at
//     EngineSharded/Run.hpp:872 + Strategies/StrategyParameters.hpp:1429.
//   - TECH_DEBT-134 NEW + CLOSED: 5 stale "centralized arch" comments cleaned up at
//     EngineCommon.hpp:12+814 + ControllerEventLoop.hpp:88+95+2169 + 1 sister "all 3 callers"
//     reference fix.
//   - TECH_DEBT-131 PARTIAL_CLOSURE: operator-facing-doc cohort (8 line-anchor citations across
//     KNOWN_ISSUES.md:143+319 + PARITY_ISSUES.md:544+553+610+655+697+798+803+810) updated with
//     post-.B.6 sub-file path annotations preserving original refs for historical context.
//     Scope expansion documented as Class 33 recurrence (operator-facing-doc cohort missed at
//     .B.6 close despite codified `feedback_operator_facing_doc_cohort_at_cfg_deletion`).
//     Source-file cohort (original 8 sites) STILL OPEN — next stale-comment audit triggers.
//   - tools/calls_graph_diff.sh fix: SHARDED_FILES list updated to include EngineSharded/{Boot,
//     SlowPath,Async,Run}.hpp sub-files (post-.B.6 INDEX shim made tool blind to subfolder content).
//
// DOC-SYSTEM AMENDMENTS:
//   - doc-tag-vocabulary.md: +12 CONCERN tags (ssot/cpp17/header-only/shared-state/etc) +
//     7 SURFACE tags (doc-pipeline/plan-pipeline/header-split/etc). Closes 24 /metadata-audit
//     HIGH undefined-tag findings.
//   - structural-enforcement-when-memory-insufficient.md: ci-tools → ci-tooling (consumer
//     alignment with canonical vocab).
//   - implementation-layer-blindspot-taxonomy.md: B17 (forward-decl namespace shadow) Stage
//     2 DRAFT → Stage 3 first canonical (Class 34 recurrence_count = 2 threshold met).
//   - cpp17-inline-variable-for-header-shared-state.md: tags axis-correction (remove
//     `data-discipline` from CONCERN).
//   - cfg-field-categorization-discipline.md: tags axis-correction + sister_specs YAML
//     single-line for rg greppability.
//
// C1 CLOSE-OUT (file-size discipline umbrella cancellation per operator 2026-05-27):
//   - 5 TECH_DEBT entries CLOSED as `wontfix-per-ai-workflow`:
//     - TECH_DEBT-029 (Source file length reduction)
//     - TECH_DEBT-114 (controller_test.cpp split; TECH_DEBT-127 stays open as test-reliability surface)
//     - TECH_DEBT-116 (TECH_DEBT.md split)
//     - TECH_DEBT-117 (RECURRING_BUG_PATTERNS.md split — closed as `done-incidentally`)
//     - TECH_DEBT-118 (/readiness SKILL.md split)
//   - RATIONALE: AI-driven solo workflow removes navigation-cost motivation. Test 5K rule
//     RETAINED for test-reliability concern. Subfolder pattern (`.B.6` first canonical) stays
//     Stage 3 FROZEN for future cohort use if human contributors join project OR AI tooling
//     changes meaningfully.
//   - 6 plan body drafts DELETED: subplans/2026-05-25-v5.15.5.F.4d.1.B.{7,8,9,10,11}-*.md +
//     subplans/2026-05-25-v5.15.5.F.4d.1.B-file-size-maintenance.md
//   - CLAUDE.md File-size split discipline section SCOPED ("applied selectively per workflow").
//   - CLAUDE.local.md going-forward rule AMENDED with AI-workflow scoping note.
//   - file-size-split-discipline.md v1.3 → v1.4: NEW "AI-driven workflow scoping" section
//     (self-contained 3-5 paragraph rationale per /dod-audit F3 finding; preserves existing
//     discipline body + threshold table for future re-activation).
//
// NEW SPRINT SCAFFOLDS (forward planning per `feedback_plan_right_not_fast`):
//   - .E NEW: v5.15.5.F.4d.1.E per-core drainer architecture sub-sprint umbrella (3 sub-ships
//     .E.1 Foundation + .E.2 Migration + .E.3 Cleanup per future-roadmap doc; ~5-10 days
//     focused; HIGH-RISK; CLOSES Class 26 surface at drainer body STRUCTURALLY per recurrence_count
//     13 MANDATORY structural fix policy). Scaffold at subplans/2026-05-27-v5.15.5.F.4d.1.E-
//     per-core-drainer-architecture-SCAFFOLD.md.
//   - .F (formerly proposed as .E): v5.15.5.F.4d.1.F comprehensive professionalization audit
//     sweep + triage. Placement reordered to AFTER .E per-core drainer (audit catches per-core
//     drainer issues + verifies framework consolidation across NEW architecture). Scaffold at
//     subplans/2026-05-27-v5.15.5.F.4d.1.F-professionalization-audit-sweep-SCAFFOLD.md.
//
// AUDITS FIRED PRE-CODING (Option C 3-agent gate per operator "better safe than sorry"):
//   - /parity-check GREEN — zero parity surfaces touched (partial_exit_pct + tp2_mult not
//     STAMP_BOUND); fix corrects bug without drift; replay determinism IMPROVED post-fix.
//   - /trace-deps GREEN — TECH_DEBT-132 dead-helper delete CONFIRMED SAFE; B17 promotion
//     eligibility VERIFIED; Class 26 sister-bug surface CONFIRMED 2 sites only.
//   - /dod-audit YELLOW — F1 ci-tools reframing (APPLIED inline; consumer alignment fix);
//     F2 calls_graph_diff.sh fix shape (Option a glob expand per operator decision); F3-F6
//     non-blocking polish APPLIED inline.
//
// VERIFICATION at ship close:
//   - Build: 5 binaries (test + gui + suite + tsan + asan) clean.
//   - Tests: 3223 controller_test pass / 0 fail (+8 from .B.6 baseline; new Class 26
//     regression test section). depth_recorder_test 17/0.
//   - Hot path UNTOUCHED — tools/calls_graph_diff.sh verify GREEN (post-fix; tool now
//     scans EngineSharded sub-files).
//   - CI: check_per_core_registry_integrity.py 8 PASS (Check 9 NEW CLEAN; sanity-verified
//     by revert-detect-reapply test).
//   - /latency-track on Async.hpp fix: NONE (2-char index swap; zero latency impact;
//     HOT_PATH_CHANGELOG entry not needed).
//
// FOLLOWUP queued (post-`.B.7` triage / future ships):
//   - DEFERRED Phase D polish: D.3 B15 promotion eligibility decision / D.5 decision-log-template
//     type fix / D.7 bidirectional sister-link sweep (4 confirmed + estimated ~10-20 likely;
//     blocked on permission grant for tools/check_doc_metadata.py --bidirectional) /
//     D.8 11 Stage 2→3 promotion candidates triage (per /metadata-audit findings)
//   - TECH_DEBT-131 source-file cohort cleanup (original 8 sites STILL OPEN; partial-closure
//     status; next stale-comment audit OR /metadata-audit quarterly cadence triggers)
//   - DOCUMENTED-RISK PARITY entry for partial_exit_pct/tp2_mult historical calibration
//     tainted-results advisory (verification deferred to .F audit sweep)
//   - Stage 6 escalation candidates surfaced (queued for .D or .E): B-Plus v0.5 mechanical-
//     migration audit (flag commits with ≥10 search-replace migrations w/o /dod-audit re-fire)
//
// Postmortem at plans/v5.15-live-readiness/postmortems/2026-05-27-v5.15.5.F.4d.1.B.7-postmortem.md.

// .F.4d.1.B.6 (v5.15.5.F.4d.1.B.6) — EngineSharded.hpp subfolder split (first canonical of
// subfolder pattern at source-code level) + 2 NEW DESIGN_SPECs + 2 NEW anti-pattern Classes +
// 2 NEW M4 pillars + file-size-split-discipline.md v1.3 (code-LOC counting methodology) +
// tests/test_common.hpp shared infrastructure extract (formerly queued as .B.5 PARTIAL;
// folded into .B.6 per session pivot 2026-05-27).
//
// FILE SPLIT (3,202 → 96 INDEX shim + 4 sub-files in CoreFrameworks/EngineSharded/):
//   - Boot.hpp (67 total / 12 code) — 2 inline globals (g_engine_sharded_shutdown +
//     g_engine_sharded_gui_quit_ptr) + EngineSharded_SignalHandler (extern "C" inline)
//   - SlowPath.hpp (188/78) — EngineSharded_SlowPath_DrainPostFill (hoist of drain_post_fill
//     lambda) + EngineSharded_SlowPath_DrainManualCloses (MERGED LIVE+NO-OP per Decision H;
//     #ifdef USE_IMGUI_GUI moved INSIDE function body for single source of truth)
//   - Async.hpp (905/460) — g_engine_drainer_cycle_hist inline global +
//     EngineSharded_Async_FanOut (hoist of fan_out lambda; 25 explicit args including 6
//     block-scope-static refs: tick_rings/cores/g_tick_rec/g_depth_shared/g_shared/g_candle_acc) +
//     EngineSharded_Async_DrainWithSubmit (hoist of drain_with_submit lambda; 4 args) +
//     ShardedSnapshot_Save explicit include for C++17 two-phase lookup compliance
//   - Run.hpp (2,436/1,406) — g_sharded_order_lat inline global + EngineSharded_CalibrateTscGhz
//     + EngineSharded_PinThread + EngineSharded_GetSiblingCPU + EngineSharded_SmartSlowPathPins
//     + EngineSharded_DumpLatency<F> template + EngineSharded_Run<F, BENCH> orchestrator
//
// ALL SUB-FILES UNDER 1,500 CODE-LOC THRESHOLD (per code-LOC counting methodology landed at
// file-size-split-discipline.md v1.3). Run.hpp at 1,406 code-LOC has 94-line headroom.
//
// DECISIONS APPLIED:
//   - Decision A — subfolder pattern (sub-folder named after original + top-level shim)
//   - Decision B — lambda hoisting to template<unsigned F> named functions (BENCH NOT
//     propagated; all Live/Backtest dispatch is cfg-flag-driven, not BENCH-driven)
//   - Decision C — 4 globals migrated `static` → C++17 `inline` (single shared storage
//     across TUs; sister to tests/test_common.hpp pattern)
//   - Decision D — shim include order Boot → SlowPath → Async → Run (verified via
//     /trace-deps B7 include topology cycle check)
//   - Decision E — INDEX shim shape (4 sub-file #includes + license + comprehensive
//     doc-comment header preserving original cold-pickup context)
//   - Decision G — SH_* color macros stay atomic with their TUI render block usage
//     (not hoisted separately; stay inside EngineSharded_Run body)
//   - Decision H — drain_manual_closes LIVE + NO-OP MERGED into single function
//     (#ifdef inside body; single source of truth; sister to .B.4 EngineCommon_BootPerCore
//     dual-cfg shape)
//
// SHIP-CLOSE CODIFICATIONS LANDED:
//   - NEW DESIGN_SPEC: cpp17-inline-variable-for-header-shared-state.md (Stage 3 first
//     canonical; 2 canonical applications: tests/test_common.hpp + Boot.hpp)
//   - NEW DESIGN_SPEC: single-source-of-truth-discipline.md (Stage 3 first canonical;
//     Decision H merge as worked example)
//   - file-size-split-discipline.md v1.2 → v1.3 (subfolder pattern Stage 3 first canonical
//     reference + code-LOC counting methodology section)
//   - NEW Class 34: Forward-decl namespace shadow (RECURRING_BUG_PATTERNS catalog;
//     detection signature + 2 instances at .B.6 Phase B.3 + B.2)
//   - NEW Class 35: Block-scope statics inaccessible from hoisted header functions
//     (RECURRING_BUG_PATTERNS catalog; detection signature + 1 instance at .B.6 Phase B.2)
//   - Class 32 (mega-file accumulation): recurrence_count increment + this ship's 3,202→96
//     instance documented
//   - B17 + B18 NEW pillars in implementation-layer-blindspot-taxonomy.md (Stage 2 DRAFT)
//   - 3 NEW memory rules: feedback_cpp17_inline_variable_for_shared_state_across_tus +
//     feedback_single_source_of_truth_discipline + feedback_count_code_loc_not_total_lines
//     + 2 sister memories (feedback_forward_decl_at_global_scope_not_namespace +
//     feedback_enumerate_block_scope_statics_before_hoist)
//   - CLAUDE.local.md going-forward rules: 5 NEW entries
//
// PATH NOT TAKEN — Phase B.4.1 full Run.hpp sub-split REVERTED (commit 6323c17):
// Triggered by total-LOC threshold check (Run.hpp 2,436 total = 62% "over"). After agent
// completed ~30 min of sub-sub-file work, the code-LOC methodology gap surfaced — Run.hpp
// at 1,406 code-LOC was ALREADY UNDER threshold. Work reverted; methodology lesson codified
// in file-size-split-discipline.md v1.3. Audit-rigor miss: 4-pillar self-audit pillar 4
// (novel alternative consideration) should have surfaced "different counting methodology"
// before triggering split work. Lesson worth remembering across the sprint.
//
// QUEUED REMAINING file-size cleanup (post-code-LOC re-analysis 2026-05-27 — scope CUT
// IN HALF; 6 files genuinely need splits, 6 files dropped as comment-heavy-but-code-fine):
//   - .B.7 BacktestPanels.hpp (4,639 code-LOC; 3× over; biggest remaining)
//   - .B.8 ControllerEventLoop + CoreModelZoo + BacktestEngine bundle (3 files; each
//     ~1.7-1.8k code-LOC just over threshold)
//   - .B.9 DashboardPanels + PortfolioController bundle (2 files; barely over)
//   - DROPPED — 6 marginal files UNDER code-LOC threshold (ControllerConfig + EngineTUI +
//     ModelInference + StrategyParameters + SettingsPanel + OrderManager)
//
// TECH_DEBT UPDATES:
//   - TECH_DEBT-029 → PARTIAL_CLOSURE (1 of 6 actual splits per code-LOC analysis;
//     full close at .B.9 once 3 remaining ships land)
//   - TECH_DEBT-130 NEW — 4 defensive nullptr guards in Async.hpp fan_out body
//     (runtime-dead under USE_IMGUI_GUI; slow-path cadence; optional __builtin_expect
//     polish OR delete; future post-paper-test if perf data shows need)
//   - TECH_DEBT-131 NEW — 7 stale `EngineSharded.hpp:LINENO` comment refs in 5 sibling
//     files (cosmetic comment drift; cleanup at next stale-comment sweep)
//
// PHASE C COMPREHENSIVE VERIFICATION — GREEN:
//   - Build: 5 binaries clean (test + gui + suite + tsan + asan)
//   - Tests: 3,215 controller_test + 17 depth_recorder_test (both pass / 0 fail; baseline preserved)
//   - Hot path UNTOUCHED — ExecutionCore.hpp + Strategies/ no diff vs pre-B.6 baseline
//     (11 occurrences of BG_Evaluate/SG_Evaluate/ExecutionCore_Tick preserved verbatim)
//   - External callers: 30 files include EngineSharded.hpp; ZERO bypass shim
//     (all use shim path; subfolder split is transparent)
//   - /parity-check GREEN — lambda hoists are byte-equivalent under capture→arg rewrite;
//     wire format + HMAC + train-serve helpers UNTOUCHED
//   - /dod-audit GREEN — cache-line alignment preserved (H6); branchless preserved (H7);
//     bit-packing preserved (H14); subfolder pattern correctly applied
//   - /blindspot-scan GREEN — B7 include topology cycle check PASS (Boot → Async → SlowPath
//     → Run order works); 2 NEW pillar candidates B17/B18 surfaced + codified
//   - /bug-check 7/8 Classes CLEAN + 1 YELLOW (Class 28 4 nullptr guards; not blocker; queued
//     as TECH_DEBT-130)
//
// PHASE D first-canonical viability gate: PASS — subfolder pattern is GREEN for .B.7-.B.9
// propagation (3 remaining file-size cleanup ships; ~3-6 days focused work).
//
// Postmortem at plans/v5.15-live-readiness/postmortems/2026-05-27-v5.15.5.F.4d.1.B.6-postmortem.md.

// .F.4d.1.B.4 (v5.15.5.F.4d.1.B.4) — Train-serve execution-layer parity structural extract
// + B-full SHARDED centralized-arch full surface deletion + Phase Cx-cfg-cohort closure
// (cfg field categorization correction across 11+ instances) (2026-05-27).
//
// LANDED across multiple WIPs (engine commits):
//
// WIP-12 framework layer (commit ae8dfad) — B-Plus v0.4 generator mode (~250 LOC Python
// addition); B14/B15 NEW pillars in implementation-layer-blindspot-taxonomy.md Stage 2 DRAFT;
// 4 NEW sister memories codified (multi-surface deletion ordering / unconditionalization latent
// assumption / operator-facing doc cohort / archived changelog preservation); 2 memory
// amendments; Class 33 NEW catalog entry; CLAUDE.local.md 5 going-forward rules + 1 amendment;
// /readiness Checks 41/42/43; /precoding-audit-gate + /handoff + /blindspot-scan skill
// amendments. M7 Stage 5 → Stage 6 cadence-locked trigger met.
//
// WIP-13 BACKTEST migration (commit 708044d) — Phase C.4 atomic ADD EngineCommon_SlowPathCycleAllCores
// call + DELETE 6 trio targets per Decision F. CATASTROPHIC double-fire risk surface; ADD + DELETE
// atomic. -11 LOC.
//
// WIP-14a B-full SHARDED operator-facing doc + stale comment cleanup (commit 85b54bc) —
// cosmetic leaves-first; sister to WIP-14b compile-critical surface delete.
//
// WIP-14b B-full SHARDED centralized-arch compile-critical surface delete (commit d53ef53) —
// 9 files / 367 lines deleted / 84 inserted = net -283 LOC; 12-step leaves-first ordering per
// B14 multi-surface deletion ordering pillar (FIRST CANONICAL Stage 3); Class 18 cohort wrapper
// deletion via F17 (3 sister wrappers EventLoop_TimeExit/_TrailingSLRatchet/_BreakevenOnProfit);
// D18 full surface deletion per `feedback_backwards_compat_not_default_concern` FIRST CANONICAL
// application; closes PARITY-026/027/028/029/030/031 + multiple cfg/parser/GUI/test surfaces.
//
// WIP-15 PARITY-031 ordering closure + parity_harness extension (commit 4c48d5d) — Phase C.4.5
// 4-consumer fc_ctx.regime_state cohort closed via per-core BACKTEST_REGIME_SAMPLE_CORE read
// pattern (sister to LIVE per-core regime classification); parity_harness Phase C.6 extension
// adds --pay-fees-in-bnb CLI + cross-path total_fees BPS equality check; partial close
// TECH_DEBT-120/-122 (full close at .F.5.C).
//
// WIP-16 Phase Cx-cfg-cohort closure (commits b8bba2b + 9d18eac engine + ccb3692 workspace) —
// cfg field categorization correction across 11+ instances per Path 2 v5 final scope:
// - 9 GLOBAL_ONLY_READERS per-core registry rows DELETED at FOREACH_PER_CORE_CFG_FIELD
//   (kill_recovery_warmup + sl_cooldown_base/extra/cycles + idle_reset_cycles + model_max_age_hours
//   + lazy_rebuild_price_threshold_pct + enable_mtm_kill_switch + sl_cooldown_adaptive)
// - 7 of those 9 fields MIGRATED to FOREACH_GLOBAL_CFG_FIELD with operational manual values as
//   registry payload (H17 STRONG→HARD progression at global surface per roadmap)
// - 2 H14 violations CLOSED via cfg-flag bitmap migration (enable_mtm_kill_switch +
//   sl_cooldown_adaptive → MASK_RISK_CFG_MTM_KILL_SWITCH_ENABLED + MASK_RISK_CFG_SL_COOLDOWN_ADAPTIVE_ENABLED
//   in risk_cfg_flags bitmap; sister to MASK_RISK_CFG_KILL_SWITCH_ENABLED PARITY-026 hotfix)
// - NEW EMIT_PER_CORE_CFG_DEFAULT_GLOBAL_MIRROR walker (lands "future work" noted at
//   CfgFieldRegistry.hpp:739 pre-cycle comment); auto-populates per-core registry rows' global
//   manual struct field defaults at ControllerConfig_Default<F> time
// - Class 25 cosmetic consumer fix at EngineCommon.hpp:618 (exit_threshold per-core scope; value-
//   equivalent; future-proofs against per-core override addition)
// - regime_hysteresis PortfolioController.hpp:358/:2023 cosmetic legacy single_core migration
// - Registry default precedence v1.1 procedure applied: regime_hysteresis registry payload bumped
//   INT(3)→INT(5) to match operational manual; 11 manual init lines DELETED (auto-populate via
//   walker)
//
// NEW DESIGN_SPECS landed:
// - framework-patterns/cfg-field-categorization-discipline.md (Stage 2 DRAFT v1.0 → Stage 3
//   first-canonical at this ship close per pattern-codification-lifecycle.md; 4-category decision
//   tree + 5-step re-categorization migration + sister-pattern co-location + DOD audit)
//
// NEW SISTER MEMORIES codified:
// - feedback_cfg_field_categorization_at_registry_add_time + feedback_categorize_by_consumer_pattern_not_field_name
//   + feedback_operator_pushback_as_audit_signal + feedback_no_question_boxes (M7 escalation
//   candidate worked example) + feedback_motivated_collaborator_for_caramel "right not fast"
//   articulation amendment.
//
// NEW SKILL EXTENSIONS:
// - /precoding-audit-gate Stage 4 synthesis extension (M7 4th canonical structural enforcement of
//   `feedback_audit_canonical_sister_before_new_infra` at synthesis-stage planning surface)
// - /readiness Check 44 sidecar (cfg field categorization plan-time verification; 5-question
//   consumer-pattern verify + 4-category decision tree + 5-step re-categorization migration)
//
// CI INFRASTRUCTURE:
// - check_per_core_registry_integrity.py Check 8 scaffold (M7 4th canonical; mechanical detection
//   patterns at sister mini-ship per token-budget pragmatism; discipline ENFORCED at /readiness
//   Check 44 plan-time + Stage 4 synthesis stage + DESIGN_SPEC layer)
//
// CLAUDE.md / CLAUDE.local.md STRUCTURAL ENFORCEMENT:
// - CLAUDE.md "How to..." table row: substantive plan body amendment triggers
//   /precoding-audit-gate re-fire BEFORE coding
// - CLAUDE.local.md going-forward rule index entry: audit re-fire at substantive plan amendment
//   (sister to feedback_iteration_spiral_signals_audit_meta_gap recognition trigger)
// - /precoding-audit-gate SKILL.md WHEN-TO-USE expanded with substantive-amendment trigger
//
// META-DISCIPLINE PROMOTIONS at .B.4 ship close:
// - M5 train-serve EXECUTION-LAYER parity Stage 2 DRAFT → Stage 3 first-canonical (this ship's
//   EngineCommon_BootPerCore + EngineCommon_SlowPathCycleOneCore + SlowPathCycleAllCores extract
//   = canonical reference; sister memory feedback_train_serve_execution_layer_meta_gap)
// - M6 body-content-enumeration-at-plan-time Stage 2 DRAFT → Stage 3 first-canonical
// - M7 structural-enforcement-when-memory-insufficient Stage 5 → Stage 6 cadence-locked
//   (B-Plus v0.2/v0.3/v0.4 = 3 canonical applications; cfg-field-categorization-discipline +
//   CI Check 8 + /readiness Check 44 = 4th canonical application)
// - B14 multi-surface deletion ordering Stage 2 DRAFT → Stage 3 first-canonical (engine_arch
//   51-site cohort delete at WIP-14b = canonical reference)
// - B15 unconditionalization latent assumption STAYS Stage 2 DRAFT (1st instance only at
//   EngineSharded.hpp:2484 boot-spawn gate; Stage 3 promotion deferred to 2nd canonical per
//   feedback_proactive_novel_alternative_consideration 2-instance threshold)
//
// CLASS N CATALOG UPDATES at .B.4 ship close:
// - Class 14 stays 12 (B-Plus v0.3/v0.4 prevents new fabrications structurally at COMMIT layer)
// - Class 18 recurrence_count 7 → 8 (F17 sister-wrapper cohort-deletion at WIP-14b)
// - Class 25 recurrence_count 2 → 3 (CONFIRMED RECURRING; exit_threshold cosmetic fix at WIP-16)
// - Class 26 recurrence_count 1 → 11 (MANDATORY structural fix threshold met; 10 new worked
//   instances at WIP-16 Phase Cx-D + Cx-T/U closure)
// - Class 28 canonical closures entry (8-branch elimination at WIP-14b = NET branchless improvement
//   per H20 discipline)
// - Class 33 NEW catalog entry (consumer-enumeration undercount on deletion; codified at WIP-12;
//   sister to Class 14 flipped; recurrence_count = 2 instances)
//
// 7+ PARITY ENTRIES CLOSED:
// - PARITY-026 (kill_switch live-safety hole; closed by hotfix .B.2.h1 pre-cycle)
// - PARITY-027/028/029/030 (boot-time train-serve asymmetry; closed by EngineCommon_BootPerCore
//   structural extract at WIP-7/8/9)
// - PARITY-031 (regime sample ordering; closed by Phase C.4.5 at WIP-15 via per-core read pattern)
// - PARITY-032 (BREAKEVEN_ON_PROFIT cached-gate dispatch; closed by D1-B at WIP-11)
//
// MULTIPLE TECH_DEBT ENTRIES CLOSED/PARTIAL:
// - TECH_DEBT-119 CLOSED structurally (EngineCommon extract = closes 4 CRITs + 3 HIGHs)
// - TECH_DEBT-120/-122 PARTIAL close at Phase C.6 parity_harness extension; full close at .F.5.C
//
// SCOPE EXPANSION at v1.7.6 mid-cycle (operator-driven; no defer pattern):
// - Phase Cx-cfg-cohort NEW (per operator question on regime_hysteresis; cascaded to comprehensive
//   cfg field categorization correction; 5 path iterations Path 1 → 2 → 2 v3 → 2 v4 → 2 v5
//   final; iteration spiral itself became evidence for structural enforcement codification at
//   Cx-K Stage 4 synthesis + Cx-L/M/N CLAUDE.md amendment-trigger updates)
//
// Tests: 3215 pass / 0 fail (-2 from .B.3 baseline; engine_arch round-trip tests intentionally
// deleted at WIP-14b per topology test surface removal). Build: 6 dirs PASS (test/gui/suite/tsan/
// asan/lat). 7 CI checks PASS (Check 8 pending sister-ship mechanical impl).
// Hot path UNTOUCHED (per H7/H8 budgets; verified via calls_graph_diff.sh verify).
//
// Postmortem at plans/v5.15-live-readiness/postmortems/2026-05-27-v5.15.5.F.4d.1.B.4-postmortem.md.

// .F.4d.1.B.3 (v5.15.5.F.4d.1.B.3) — Legacy empty-out + Path C bash CLI deletion +
// Phase K cleanups (2026-05-24). Closes .F.4d.1.B split (.B.1 framework + .B.2 cohort
// migration + .B.3 legacy empty-out).
//
// LANDED at .B.3 across 6 engine commits + hotfix tag:
//
// A1 KILL_SWITCH HOTFIX (commit 6fd0ba3; GPG-signed tag v5.15.5.F.4d.1.B.2.h1-killswitch-fix):
// - Closes PARITY-026 — Live kill_switch was DEAD in production (EngineSharded.hpp:742
//   missing EventLoopState_ConfigureKillSwitch call mirror of BacktestSharded:217-221;
//   broken since sharded path was built; ~14 months silent live-safety hole)
// - 5-LOC mirror-the-backtest fix; surfaced by 2026-05-24 ML↔LIVE structural sweep
//
// WIP-9 (commit 8caac1f) — Steps 1.6.6.a/b + 6.10 + 2 + 3 + 4:
// - 12-row STAMP-side h->inference_cfg_X → h->X rename at CfgDriftCheckRegistry.hpp
//   (3 already done at Phase F WIP-8 HIGH-1(b) cascade; 12 remaining done at WIP-9)
// - 11 COHORT_GATE substitutions: 4 to existing COHORT_GATE_BANDIT_BLEND_STATE_4 +
//   COHORT_GATE_PER_HORIZON_BARRIER (Step 1.6.6.a) + 7 to 2 NEW COHORT_GATE_BANDIT_ENABLED
//   + COHORT_GATE_COST_GATE_ENABLED extracted at MlCfgFlagRegistry.hpp:122-123 (Step 6.10)
// - FPN_ToDouble() wrapping on 9 FPN<F> h-> fields post-rename (ModelHandle cfg-derived
//   auto-gen produces FPN<F> fields; drift compare macro requires double cast)
// - Closes Path γ #3 PARTIAL → MOSTLY (3 of 3 cohort registries reference shared COHORT_GATE_*
//   macros; per_horizon_barrier_blend ternary at expected-value column line 311 + group-bit
//   STAMP_HAS preserved with rationale)
// - DELETED ML_Headers/StampBoundCfgRegistry.hpp (full file; FOREACH_STAMP_BOUND_CFG body +
//   STAMP_CFG_AUTOPOPULATE + FOREACH_STAMP_BOUND_CFG_COUNT)
// - DELETED MemHeaders/CfgDerivedInferenceCfgRegistry.hpp (full file)
// - Removed 2 #includes (StampHelper.hpp:55 + ModelInference.hpp:29) + 2 FOREACH_REGISTRY
//   rows at MetaRegistry.hpp:52/99
// - 1 test assertion deleted at controller_test.cpp:25028 (FOREACH_CFG_DERIVED_INFERENCE_CFG_COUNT)
//   per /test-deletion-justification — replaced by Step 4 CI Check 9 compile-time coverage
// - NEW CI Check 9 static_assert at CfgFieldRegistry.hpp:1156-1180 — STAMP_BOUND_CFG_DERIVED
//   cohort coverage regression guard (compile-time; ≥ 20 across per-core + global masks)
//
// WIP-10 (commit c519104) — Path C cleanup Steps 6.5-6.9:
// - DOCS/ML_TRAINING.md operator-recipe section updated to foxml_suite GUI auto-stamp
//   workflow (away from deleted tools/stamp_model.sh bash CLI)
// - 5 source-comment + tooltip sweeps (CfgFieldRegistry tooltip + ControllerConfig doc/init
//   + BacktestPanels 3 sites) — Path C deletion context added to operator-visible help
// - DELETED 4 bash-CLI test blocks at controller_test.cpp:9339/11323/13321/13453 per
//   /test-deletion-justification; 21 assertions replaced by structural-guarantee comments
//   cross-ref to single-emitter discipline + cfg-derived consumer framework auto-flow
// - Fixed tools/validate_feature_mask.sh dead branch (Surface 3 stamp_model.sh refs)
// - Fixed foxml_suite Optimizer panel cfg default ("engine.cfg" → "backtest.cfg" restoring
//   parity with RunControl_Init:160; closes foxml_suite agent HIGH-3 finding)
//
// WIP-11 (commit 101b4aa) — Step 5.5 drift guardrail:
// - foxml_suite.cpp boot-time engine.cfg/backtest.cfg byte-diff check with operator-visible
//   WARN block if divergent. Closes foxml_suite agent CRIT-1 (stop-gap until structural
//   fix at v5.15.6.A/B/C per TECH_DEBT-123)
//
// Step 8.6 (commit b4843b0) — 49-globals registry-default sweep (OWN COMMIT per plan body):
// - 12 MATCH cases: deleted redundant manual defaults that equaled registry payload defaults
//   (poll_interval, max_positions, init_arena_use_hugepages, acknowledge_hardcoded_strategy_in_live,
//    ml_backend, regime_model_backend, record_ticks+record_depth+record_max_days, notify_backend+
//    notify_cooldown_secs, held_out_gate_strict+allow_cross_major_engine+auto_stamp_on_held_out,
//    acknowledge_hot_swap_with_open_positions, xgb_min_child_weight+xgb_seed, xgb_train_nthread,
//    ws_dead_time_flatten_threshold_secs, trading_mode, sharded_force_synthetic,
//    lazy_rebuild_force_period_us, use_aot_inference, wf_split_max_gb)
// - 16 DIFFER cases: inline rationale comments added (KEEP-MANUAL with reason per
//   feedback_motivated_collaborator_for_caramel — operator-policy values preserved, registry-
//   default updates deferred to follow-up sub-ship to avoid test-fixture+muscle-memory breakage)
//
// FRAMEWORK CONSOLIDATION CLOSURE (per Charter):
// After .B.3 ships: framework discipline at cfg/stamp/drift surface STRUCTURALLY COMPLETE.
// Adding a new cfg field = 1 row in master registry; parser + GUI render + tooltip + per-core
// override emission + stamp emit + drift check all auto-flow via cfg-derived consumer framework.
//
// BUG CLASSES STRUCTURALLY CLOSED at .B.3:
// - Class 11 (Extensibility friction at cfg surface)
// - Class 12 (Wired-but-unexercised ML paths) — cfg-derived consumer activated across 4 cohort registries
// - Class 14 (Plan calls struct field that doesn't exist) — by-construction via auto-gen
// - Class 18 (Mirror state/code at inf struct + wire-format-emit + double-emit surfaces)
// - Class 21 (Multiple parallel descriptors) at cfg-derived layer
// - Class 24 (Capability-cfg surface mismatch) for per_horizon_barrier_blend
//
// NEW BUG CLASSES CODIFIED at .B.3 ship close:
// - Class 31 (Wire-format duplicate-key emit from sister registries; 10 instances closed)
// - Class 32 (Prefixed/unprefixed struct field mirror in X-macro auto-gen registry; 10 closed;
//   ~5 sibling instances at model-state cohort tracked via TECH_DEBT-104)
//
// META-DISCIPLINE CODIFIED at .B.3 ship close:
// - M5 train-serve EXECUTION-LAYER parity (DESIGN_SPECS/meta-disciplines/train-serve-execution-layer-parity.md
//   Stage 2 DRAFT v0.1 — first canonical at v5.15.5.F.4d.1.B.4 ship close via EngineCommon_BootPerCore
//   + EngineCommon_SlowPathCycleOneCore shared helper extract)
//
// 6 NEW PARITY ENTRIES (PARITY-026 through PARITY-031; train-serve asymmetry surfaced by
// 2026-05-24 ML↔LIVE sweep): PARITY-026 closed by hotfix; 027/028/029/030/031 target .B.4 structural close.
//
// 6 NEW TECH_DEBT ENTRIES (TECH_DEBT-119 through TECH_DEBT-124; named homes per closure matrix):
// - TECH_DEBT-119 EngineCommon extract → .B.4 (closes 4 CRITs + 3 HIGHs structurally)
// - TECH_DEBT-120 parity_harness per_core_slow coverage → .F.5.C
// - TECH_DEBT-121 live bandit_state_prior_path → .F.5.A
// - TECH_DEBT-122 parity_harness rename/extend → .F.5.C
// - TECH_DEBT-123 foxml_suite cfg-source-of-truth structural fix → v5.15.6.A/B/C
// - TECH_DEBT-124 cross-tool emit-site CI guard → defer (defensive)
//
// Tests: 3208 pass / 0 fail (-22 from .A baseline; intentional /test-deletion-justification
// for deleted bash-CLI test blocks + FOREACH_CFG_DERIVED count assertion; replacement coverage
// via in-process round-trip + CI Check 9 + single-emitter discipline + cfg-derived consumer
// framework auto-flow).
// Build: 5 binaries (test/gui/suite/tsan/asan) clean. 6 CI checks PASS. Hot path UNTOUCHED.
//
// NEW .B.4 sub-ship queued: plans/v5.15-live-readiness/subplans/
// 2026-05-24-v5.15.5.F.4d.1.B.4-train-serve-execution-layer-parity.md (DRAFT v1.0).
// Closes PARITY-027/028/029/030/031 + TECH_DEBT-119 structurally via EngineCommon_BootPerCore +
// EngineCommon_SlowPathCycleOneCore shared helper extract per pattern at
// DESIGN_SPECS/refactor-patterns/shared-helper-extract-for-train-serve-mirror-close.md (DRAFT v0.1).
//
// Postmortem at plans/v5.15-live-readiness/postmortems/2026-05-24-v5.15.5.F.4d.1.B.3-postmortem.md.
//
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
