#pragma once
// single source of truth for version string
// update HERE only — all renderers include this

#define ENGINE_VERSION_MAJOR 5
#define ENGINE_VERSION_MINOR 15
#define ENGINE_VERSION_PATCH 5
#define ENGINE_VERSION_STRING "5.15.5.F.4c.1"
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
