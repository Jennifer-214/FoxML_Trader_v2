# 2026-04-25 (later) — Label-type-aware metrics: primitives + sample panel + Train Model

Branch: `experiment/phase5-zoo`. Continuing from `2b27707` (changelog) +
`fcf9616` (equity_curve fix). Rollback tag `pre-label-type-fix` set before
this work began.

This is **part 1 of 2** of a structural fix for "every metric/display site
assumed binary classification." Walk-Forward path, Overfit detector, and
docs land in part 2.

---

## The bug class

The codebase has the right primitive — `label_table[t].num_classes` already
encodes 0=binary / 1=regression / ≥2=multiclass — and Train Model's *training*
side correctly branched on it for selecting XGBoost objective. But every
*metric* and *display* site was written under "binary classification"
assumptions and never updated when regression and multiclass labels were added.

Symptom seen 2026-04-25 morning: ran Forward P&L (regression label), got
`+: 0 / -: 2,254,869 / Ratio: 0.0%` in the sample panel, `Train Accuracy:
0.2%`, and Walk-Forward showing `0.0% / 0.0% / 0.0%` for every fold. None
of those numbers were meaningful — they were binary-classification metrics
computed on continuous regression labels (every label below the 0.5 binary
threshold → counted as "negative"; every prediction binarized at 0.5 →
useless for continuous output).

Root cause is the same shape as the equity_curve spinner from yesterday:
**a primitive existed, a few sites consulted it, but extending the codebase
with a new label kind didn't propagate the awareness everywhere it was
needed.** Different file, same bug class.

---

## Part 1 (this commit)

### `LabelType_*` helpers (`Backtest/LabelFunctions.hpp`)

Single source of truth for "what kind of label is this." Reads `num_classes`
from `label_table[]`. Helpers:

```cpp
LabelType_NumClasses(int t)    // 0 / 1 / ≥2
LabelType_IsBinary(int t)      // num_classes == 0
LabelType_IsRegression(int t)  // num_classes == 1
LabelType_IsMulticlass(int t)  // num_classes >= 2
LabelType_KindName(int t)      // "binary" / "regression" / "multiclass"
```

Every metric/display site that touches label values should branch on these.
The CLAUDE.md doc (part 2) makes this rule explicit.

### Regression metrics (`Backtest/BacktestEngine.hpp`)

New companions to the existing `WalkForward_ComputeAccuracy`:

- `WalkForward_ComputeMSE` — mean squared error
- `WalkForward_ComputeCorrelation` — Pearson r in [-1, +1]
- `WalkForward_ComputeMulticlassAccuracy` — argmax over softmax probs

Pearson r is the load-bearing regression metric: a model predicting always-zero
gets low MSE on small-magnitude targets while having zero predictive power.
r captures actual signal, MSE captures fit quality. Read both.

### Sample panel display (`Backtest/BacktestPanels.hpp`)

Three branches now, by label kind:

- **Binary**: existing `+ / - / neutral / ratio` display, plus tooltip note
  about scale_pos_weight auto-application.
- **Regression**: `Samples: N | range: [min, max] | mean: M | σ: S`. Stores
  values on `state` for later post-train context.
- **Multiclass**: per-class histogram `c0: N (P%) | c1: N (P%) | ...`.

Each kind has a kind-specific tooltip explaining what to look for and what
imbalance/spread implies.

### Train Model in-sample metric (`Backtest/BacktestPanels.hpp`)

Replaced the open-coded sign-agreement proxy for regression. Now uses the
new metric helpers:

- Binary: `WalkForward_ComputeAccuracy` (already correct, just centralized)
- Multiclass: `WalkForward_ComputeMulticlassAccuracy` (already correct)
- Regression: `WalkForward_ComputeMSE` + `WalkForward_ComputeCorrelation`

Status message + display reflect the kind. Regression shows
`Model saved (MSE: M, corr: r)` and `Train MSE: M | Pearson r: r`. Binary
and multiclass keep the existing accuracy-based display.

`TrainingPanelState` extended with `train_mse`, `train_correlation`,
`train_label_min/max/mean/stddev` — used by the regression display path.

---

## Part 2 (still to ship)

These are partially scoped but not yet implemented. Doing them in a separate
commit so part 1 lands as a coherent self-contained unit.

- **Walk-Forward path** (`Backtest/BacktestEngine.hpp` ~line 961): currently
  hardcodes `binary:logistic` and runs `WalkForward_ComputeAccuracy` per fold.
  Needs to pick objective by label kind, skip the neutral-filter when not
  binary (regression labels can legitimately be ~0.5), and compute appropriate
  metric per fold. Will require extending `WalkForwardFoldResult` to hold
  metric-kind-aware fields.
- **Overfit detector** (`Backtest/OverfitDetection.hpp`): accuracy-threshold
  memorization checks don't apply to regression. Either compute via train_MSE
  / val_MSE divergence ratio, or skip with `kind=regression: not applicable`
  status. Decision pending.
- **Walk-Forward results display panel**: read kind, format Acc% / MSE / Corr
  appropriately, update column headers.
- **CLAUDE.md "Label-type-aware metric invariant"**: doc the rule explicitly,
  with the 2026-04-25 postmortem inline so the next person extending labels
  sees what to do.

---

## Anti-drift verification

- [x] `ML_Headers/ModelInference.hpp::ModelFeatures_Pack` UNCHANGED
- [x] `ML_Headers/RollingStats.hpp::RollingStats_Push` UNCHANGED
- [x] `CoreFrameworks/ExecutionCore.hpp::ExecutionCore_Tick` UNCHANGED
- [x] `FEAT_*` constants UNCHANGED
- [x] `controller_test` 279/279 passing
- [x] All 3 targets build clean (engine, engine_gui, foxml_suite)

## Rollback

```bash
git reset --hard pre-label-type-fix   # back to 2b27707 (yesterday's state)
git reset --hard pre-zoo              # back to before all Phase 5 work
```
