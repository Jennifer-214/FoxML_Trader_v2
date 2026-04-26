# 2026-04-25 (evening) — Phase 7 prep: Validation infrastructure + writeup template

Branch: `experiment/live-readiness`. Fifth phase after `phase5d-regression-tests`,
Phase 8a, Phase 8b, and Phase 6prep. Continues from `phase6prep-complete`.

Five commits, each tagged `phase7prep-c1` … `phase7prep-c5`.

## What ships

Held-out validation infrastructure + framework + writeup template, all
ready for Phase 7 finalize when a model with non-zero validation Pearson r
exists. **No behavior change for live engine** — the infrastructure is
foxml_suite-side; live engine reads it only via `expected.cfg` mismatch
checks.

| Capability | Description |
|---|---|
| `HeldOutSplit` primitive | Lock-token discipline for held-out test set. Code refuses to access test indices unless explicitly unlocked. |
| `Backtest_RunFullValidation` framework | Walk-forward on train+val + held-out eval (stub) + gap math. Held-out training itself is Phase 7 finalize. |
| `held_out_fraction` cfg | Portion reserved (default 20%, clamped [0.05, 0.30]). |
| `gap_acceptable_threshold` cfg | Max acceptable WF→held-out gap (default 5%). The was-it-real test. |
| `expected.cfg` sync | Discipline values save with model bundle; live engine logs at load. |
| README "Trained Model Results" template | Placeholder section ready to fill when results are real. |

Default cfg = no behavior change. Existing engine.cfg files load without
warnings; existing `expected.cfg` bundles work unchanged (new fields are
optional, default-fall-through if missing).

## Commits

### c1 (`9cf0ccc`) — HeldOutSplit primitive

New file `Backtest/HeldOutSplit.hpp` — struct + Make/Unlock/TestAccessAllowed/
Relock/GenToken helpers. ~140 LOC.

Lock-token discipline:
- `HeldOutSplit_Make(total, fraction)` returns a struct with `locked=1`
  and a 32-char hex token.
- `HeldOutSplit_TestAccessAllowed(*s)` returns 0 until unlocked.
- `HeldOutSplit_Unlock(*s, token)` requires matching token.
- `HeldOutSplit_Relock(*s)` regenerates token (old token can't reuse).

Token implementation: double FNV-1a-64 with distinct seeds, concatenated
as 16+16 hex chars. The plan said "SHA256-hex" but the field is 33 bytes
(only 32 hex chars + null). Switched to FNV to keep the primitive
dependency-free (no OpenSSL link in the test path). Same friction-grade
properties — hard to guess (~128 bits entropy), trivial to forge with
source access. Documented in the header.

Fraction range clamped to `[0.05, 0.30]`. Out-of-range inputs CLAMP
rather than reject — suite recovers gracefully from typos.

### c2 (`99ac494`) — Backtest_RunFullValidation framework

New `FullValidationResults` struct + `Backtest_RunFullValidation` function
in `BacktestEngine.hpp`. Forward-declares `Backtest_RunWalkForward` so it
can be called from the new function (the actual definition lives ~150
lines down — natural placement, just needs the forward decl).

Framework:
- Lock-status check refuses to run if `split->locked`.
- Sliced view: shallow copy of `BacktestResults` with `sample_count`
  capped at `trainval_end_idx`. Backtest_RunWalkForward never reads the
  held-out region. No data is copied — slice is just a count cap.
- Walk-forward CV on slice, label-kind-aware metrics inherited.
- Gap math: `|WF mean - held_out|`, label-kind-aware.

Held-out training itself = Phase 7 finalize. For 7prep this section is
stubbed — `held_out_count=0`, `ran_held_out=0`, metrics zero. Gap
computation has consistent zero-baseline. `gap_acceptable` is gated on
`ran_held_out` so a stubbed framework run never falsely claims
"validated OK" — correctly signals "not yet validated."

When 7 finalize ships, fills in held-out training inside the existing
function — no caller-side change needed. Tests pinning the framework
(lock check, slice math, gap formula) continue to pass.

### c3 (`cc1cd23`) — Cfg + Settings + expected.cfg sync

Two new cfg fields:
- `held_out_fraction` (FPN<F>, default 0.20)
- `gap_acceptable_threshold` (FPN<F>, default 0.05)

Settings panel: new "Validation" section with both fields and tooltips
explaining the discipline semantics.

engine.cfg: documented entries with default values inline.

**Tier 2 amendment applied — `expected.cfg` sync:**
- foxml_suite Save Run writes both fields to the model bundle's
  `expected.cfg`. Reproducibility info travels with the model.
- `CoreModelZoo_VerifyExpected` reads both fields and logs them
  informationally at model load time. Lighter touch than the existing
  barrier_gate / ml_buy_threshold mismatch checks. Records the
  discipline values without enforcing — tighten to enforced if drift
  becomes a concern.

This is the load-bearing piece of Phase 7prep's discipline: when a
future engineer loads a saved model, the logs SHOW the validation
regime the model was trained under. If live cfg has been changed,
the user sees it.

### c4 (`306593a`) — tests

12 assertions across 4 groups. Per Tier 2 amendment to test sidecar
Group 4: actually invokes `Backtest_RunFullValidation` to verify
framework dispatch (lock check, slice handling, gap_acceptable signal),
not just compute the gap manually.

Groups:
- HeldOutSplit math (4): split shapes, lock_token format, fraction clamp
- Lock-token discipline (3): Test access gate, unlock with correct/wrong token
- RunFullValidation framework (3): refuses on locked, no-crash on empty
  data, gap_acceptable=0 in stub mode (not-yet-validated signal)
- Cfg backward compat (2): defaults preserved, explicit values parse

Test counts:
- controller_test: 322 → 334 (+12)
- depth_recorder_test: 17/17 (Phase 8a baseline)

### c5 (this commit) — README template + CLAUDE.md + changelog

README "Trained Model Results" section added between "ML inference" and
"order management system". Template-only — TBDs throughout. When Phase
7 finalize ships actual results, the structure is in place — just fill
in numbers.

Includes:
- Methodology (walk-forward + held-out + gap computation)
- Walk-forward fold table (TBD)
- Held-out test results (TBD)
- Strategy comparison (vanilla SimpleDip vs SimpleDip + ML gate)
- Equity curve placeholder
- Reproducibility (fingerprint, expected.cfg, data range, engine version)

CLAUDE.md: new "Held-Out Validation Discipline" subsection under Safety
Invariants. 6-point rule:
1. Held-out test set locked by default
2. Walk-forward CV ONLY on train+val portion
3. Held-out evaluation runs ONCE per locked split (relock for re-eval)
4. Generalization gap is the was-it-real test (>threshold = don't ship)
5. expected.cfg saves discipline values for reproducibility
6. Token is friction not security — resist "ergonomic" weakening

## Plan amendments applied

Per cross-plan analysis 2026-04-25 evening:

1. **expected.cfg writer/reader sync** (Tier 2) — applied in c3.
   Both new cfg fields save to bundle and log informationally on load.
2. **lock_token width** (Tier 2) — pinned at 32 hex chars (16+16 from
   double FNV-1a-64). The plan said "SHA256-hex" but field is 33 bytes;
   chose dependency-free FNV-1a over truncated SHA256 to keep the
   primitive standalone.
3. **`Backtest_RunWalkForward` signature change** (Tier 2) —
   superseded by amendment: didn't change WF signature. Instead,
   `Backtest_RunFullValidation` creates a sliced view of `BacktestResults`
   internally. Existing call site (`BacktestPanels.hpp:995`) unchanged.
4. **Test Group 4 actually validates `Backtest_RunFullValidation`**
   (Tier 2) — applied in c4. Group 3 (renumbered from sidecar's Group 4)
   invokes the function with locked + unlocked + degenerate inputs and
   verifies framework behavior (lock refusal, no-crash, gap_acceptable
   gating).
5. **Held-out lock token failure modes** (Tier 2) — partially: code
   doesn't crash on null inputs; user-deletes-token scenario doesn't
   apply since 7prep ships in-memory only (no file persistence).
   Defer file-persistence + recovery semantics to Phase 7 finalize.

## Known limitations / deferred to Phase 7 finalize

- **Filling in the README numbers** — requires actual evaluation results
  (model with non-zero validation Pearson r).
- **Held-out training pipeline** — `Backtest_RunFullValidation` stubs
  the held-out eval. Phase 7 finalize fills in: train XGBoost on full
  train+val with WF-selected hyperparameters, predict on held-out,
  compute real metrics. Framework + tests already in place.
- **Token file persistence** — current ship is in-memory only.
  `models/{run_name}/heldout.token` save/load (per original plan) is
  Phase 7 finalize.
- **Tagging release v3.10.0** — only when results are publishable.
- **HN post / writeup with outcome** — when results are real.
- **expected.cfg mismatch enforcement for new fields** — currently
  informational. Tighten to enforced-mismatch (refuse load on drift)
  if drift becomes a concern.

## Anti-drift verified

Every commit in c1-c5:
- `ML_Headers/ModelInference.hpp::ModelFeatures_Pack` unchanged
- `ML_Headers/RollingStats.hpp::RollingStats_Push` unchanged
- `CoreFrameworks/ExecutionCore.hpp::ExecutionCore_Tick` unchanged
- `FEAT_*` constants unchanged
- `controller_test` 334/334 (post-Phase-7prep baseline)
- `depth_recorder_test` 17/17 (Phase 8a baseline)
- All 4 main targets build clean
- Default cfg + existing expected.cfg files → identical behavior

## Tags

`phase7prep-c1` … `phase7prep-c5` mark each commit. `phase7prep-complete`
tags this final commit.
