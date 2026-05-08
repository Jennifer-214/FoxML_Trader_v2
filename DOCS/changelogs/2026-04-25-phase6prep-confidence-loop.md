# 2026-04-25 (evening) — Phase 6 prep: Confidence loop tunables + tests + doc

Branch: `experiment/live-readiness`. Fourth phase after `phase5d-regression-tests`,
Phase 8a, and Phase 8b. Continues from `phase8b-complete`.

Three commits, each tagged `phase6prep-c1` … `phase6prep-c3`. **Smaller phase
than originally scoped** — most wiring already exists from prior FoxML work.
Phase 6prep just makes 3 hardcoded values tunable, locks the math with
tests, and documents the loop's safe-by-default behavior on noise-floor
models.

## What ships

The confidence loop infrastructure was already wired pre-Phase-6prep:
`RollingIC` (Spearman rank correlation of pred vs realized), `RollingRMSE`
(prediction calibration stability), `ConfidenceScorer` composing them with
freshness decay, `cfg.confidence_enabled` flag, fill-handler push, slow-
path gate decision, TUI/GUI display fields. All shipped during earlier
Phase 6/7C work, just never tested.

Phase 6prep adds:
- 3 tunable cfg fields (window / freshness_tau / threshold_scale) for
  the previously-hardcoded values
- 12 test assertions locking the math + gate formula + cfg parsing
- CLAUDE.md "Confidence Loop Invariant" subsection
- Confidence-loop double-arithmetic added to FPN "Known violations" list

Default cfg = no behavior change. Pre-existing engine.cfg files load
without modification, behavior identical to pre-Phase-6prep.

## Behavior matrix

| `confidence_enabled` | `strategy_id` | `min_book_imbalance` | What happens |
|---|---|---|---|
| 0 (default) | (any) | (any) | Confidence loop inactive. Pre-Phase-6prep behavior. |
| 1 | non-ML strategy | (any) | Loop is active in code path but the gate guard `strategy_id == STRATEGY_ML` skips it. |
| 1 | STRATEGY_ML, conf ≈ 0 (noise-floor model) | (any) | `effective_thr ≈ 2 * base_thr`, gate effectively never fires. Safe-by-default. |
| 1 | STRATEGY_ML, conf > 0 (real signal) | (any) | `effective_thr` scales between `base * scale` (low conf) and `base` (high conf). Gate fires per actual signal quality. |

## Commits

### c1 (`0b66f8d`) — tunable cfg parameters

3 cfg fields added with defaults that exactly reproduce the pre-Phase-6prep
hardcoded values:

| Field | Type | Default | Was |
|---|---|---|---|
| `confidence_window` | uint32 | 32 | `CONFIDENCE_IC_WINDOW_DEFAULT` |
| `confidence_freshness_tau` | FPN<F> | 300.0 | `CONFIDENCE_FRESHNESS_TAU_DEFAULT` |
| `confidence_threshold_scale` | FPN<F> | 2.0 | hardcoded `2.0` in gate formula |

Sites updated:
- `ConfidenceScorer_Init` at `PortfolioController.hpp:~344` reads window+tau
  from cfg. The `ConfidenceScorer_Init` helper's existing fallback to
  `CONFIDENCE_*_DEFAULT` when cfg values are 0/non-positive preserves
  backward compat for cfg files missing the new fields.
- Confidence gate formula at `PortfolioController.hpp:~1618`:
  `effective_thr = base * (scale - conf)` with `scale` read from cfg.

Settings panel: 3 new field_defs under "FoxML" section with tooltips.
engine.cfg: documented section with default values inline.

### c2 (`f0e50e8`) — tests

12 assertions locking the math + gate formula + cfg parsing.

Group 1 — RollingIC math (4):
- empty IC returns 0 (no divide-by-zero)
- perfectly correlated → IC ≈ 1.0 (Spearman of monotonic = 1)
- uncorrelated → |IC| < 0.3 (with deterministic LCG seed)
- window rolls — only last N pairs counted

Group 2 — ConfidenceScorer composition (3):
- noise → conf < 0.3 (clamped to MIN_IC_DEFAULT)
- perfect identity → conf > 0.95 (IC=1, RMSE=0, freshness=1)
- stale data (age=tau) → conf decays to ~37% (e^-1)

Group 3 — Gate formula (3):
- conf=0, scale=2 → 2× base (max suppression)
- conf=1, scale=2 → 1× base (full signal)
- clamps at 1.0

Group 4 — Cfg backward compat (2):
- missing fields keep defaults (32 / 300 / 2.0)
- explicit values parse correctly (20 / 120 / 1.5)

Test data uses a Numerical Recipes LCG (`s = s * 1664525u + 1013904223u`)
instead of platform-dependent `rand()` — passes identically on glibc,
musl, macOS. Per Tier 2 amendment to phase6-prep-confidence-loop.md.

The Group 3 gate formula test uses a local lambda inlining the formula
directly, rather than driving `PortfolioController_Tick`. Tests pin the
math; production integration is verified manually on testnet.

Test counts:
- controller_test: 310 → 322 (+12)
- depth_recorder_test: 17/17 (Phase 8a baseline)

### c3 (this commit) — CLAUDE.md doc

New "Confidence Loop Invariant" subsection under Safety Invariants
covering:
1. Every fill pushes (pred, realized_return) into ConfidenceScorer
2. Compute on slow path, never hot path (Spearman is O(W²) ranking)
3. Effective-threshold formula + tests-must-update-together rule
4. Safe-by-default on noise-floor models (conf ≈ 0.01 → gate inert)
5. last_confidence is slow-path/display only, never hot path
6. Tunables list with defaults

"Known violations to fix" list (FPN-Only Accounting section) extended
with the confidence-loop double arithmetic. Pre-existing — Phase 6prep
just made `scale` cfg-tunable, didn't introduce new violations. Documented
explicitly so future readers know it's intentional, not an oversight.

The originally-planned "Snapshot sync rule update" step is dropped per
phase6prep amendment #1 — CLAUDE.md was already updated to the simplified
"thin wrapper" form during earlier Phase 5d work. No-op step removed.

## Plan amendments applied

Per cross-plan analysis 2026-04-25 evening:

1. CLAUDE.md "Snapshot sync rule" already updated, drop that step (amendment #1).
2. Confidence-loop double arithmetic at PortfolioController.hpp:1618
   is pre-existing FPN-only violation — added to CLAUDE.md "Known
   violations" list in c3 (amendment #2). Not a regression introduced by
   Phase 6prep.
3. Test sidecar Group 1 used platform-dependent `rand()` — replaced
   with deterministic LCG in c2 (amendment #3).
4. Test sidecar Group 3 had placeholder `ASSERT_TRUE(/* ... */)` —
   replaced with direct formula test via local lambda (amendment #4).

## Known limitations / deferred to Phase 6 finalize (gated on signal)

- **Comparing confidence-weighted vs raw prediction performance** — needs
  a model with non-zero validation Pearson r. Park here, return when
  signal exists.
- **Tuning `confidence_threshold_scale`** — the 2.0 default is a magic
  number. Tuning requires real signal to A/B against. The cfg field
  exists; tuning is just parameter sweep work when there's signal.
- **FPN-ifying the confidence loop math** — would resolve the Known
  violations entry. Out of scope for 6prep; do once during a future
  FPN-cleanup pass.
- **Per-core ConfidenceScorers** — current design is one per controller.
  When/if multi-core models with different signal characteristics emerge,
  add per-core scorers. Defer until that's a real need.

## Anti-drift verified

Every commit in c1-c3:
- `ML_Headers/ModelInference.hpp::ModelFeatures_Pack` unchanged
- `ML_Headers/RollingStats.hpp::RollingStats_Push` unchanged
- `CoreFrameworks/ExecutionCore.hpp::ExecutionCore_Tick` unchanged
- `FEAT_*` constants unchanged
- `controller_test` 322/322
- `depth_recorder_test` 17/17
- All 4 main targets build clean
- Default cfg → identical behavior to pre-Phase-6prep (tests confirm)

## Tags

`phase6prep-c1` … `phase6prep-c3` mark each commit. `phase6prep-complete`
tags this final commit.
