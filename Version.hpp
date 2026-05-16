#pragma once
// single source of truth for version string
// update HERE only — all renderers include this

#define ENGINE_VERSION_MAJOR 5
#define ENGINE_VERSION_MINOR 15
#define ENGINE_VERSION_PATCH 5
#define ENGINE_VERSION_STRING "5.15.5.F.4c.3"
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
