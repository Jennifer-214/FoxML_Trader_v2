# 2026-04-25 (later) — Label-type-aware metrics across the validation/display stack

Branch: `experiment/phase5-zoo`. Continuing from `2b27707` (changelog) +
`fcf9616` (equity_curve fix). Rollback tag `pre-label-type-fix` set before
this work began.

Shipped in two commits:
- **Part 1** (`cd2936d`): primitives + sample panel + Train Model
- **Part 2** (this commit): Walk-Forward path + Overfit detector + display + CLAUDE.md doc

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

## Part 2 (shipped in second commit)

### Walk-Forward path (`Backtest/BacktestEngine.hpp`)

`Backtest_RunWalkForward` now takes a `label_type` parameter (default
`LABEL_WIN_LOSS` for backward compat) and branches throughout:

- **Neutral filter at start** — only runs when label kind is binary. Regression
  labels can legitimately be ~0.5 (it's a valid continuous value, not a
  sentinel) and would have been incorrectly stripped before. Multiclass labels
  are integers, never 0.5, so the filter was harmless but cleared anyway for
  clarity.
- **XGBoost objective + `num_class`** — picked by kind:
  - Binary → `binary:logistic` + per-fold `scale_pos_weight`
  - Multiclass → `multi:softprob` + `num_class=K`
  - Regression → `reg:squarederror`, no class-weight concept
- **Per-fold metric** — picked by kind. Binary uses `WalkForward_ComputeAccuracy`,
  multiclass uses `WalkForward_ComputeMulticlassAccuracy` (argmax over softmax
  probs), regression uses `WalkForward_ComputeMSE` + `WalkForward_ComputeCorrelation`.
- **Per-fold log line** — format reflects kind: `train_acc/val_acc` for
  classification, `train_mse/val_mse | corr train/val` for regression.
- **Aggregate results** — `WalkForwardResults` extended with
  `mean_val_mse`, `mean_val_correlation`, `mean_train_correlation`,
  `label_kind`, `num_classes`. Accuracy aggregates stay populated for
  classification; correlation/MSE aggregates populate for regression.

`WalkForwardFoldResult` extended with `train_mse`, `val_mse`,
`train_correlation`, `val_correlation`. Existing `train_accuracy`/`val_accuracy`
fields continue to hold accuracy for classification (zero for regression).

### Overfit detector (`Backtest/OverfitDetection.hpp`)

Added `OverfitDetection_CheckRegression` and
`OverfitDetection_CheckRegressionDefaults`. Reuses the `OverfitReport` struct
but interprets `train_accuracy`/`val_accuracy` fields as Pearson correlations
when label kind is regression. Two checks:

- **Memorization**: `|train_corr| >= 0.99` → flagged. Tick-scale BTC
  prediction never legitimately produces train correlation that high.
- **Train/val correlation gap**: `train_corr - val_corr >= 0.20` → flagged.
  Analog of the binary "20% accuracy gap" check.

New thresholds `OVERFIT_TRAIN_CORR_THRESHOLD` / `OVERFIT_TRAIN_VAL_CORR_GAP`
match the binary thresholds' shape. Tunable separately.

Walk-Forward dispatches by kind: regression folds go through the new
correlation-based detector; classification folds use the existing
accuracy-based one. Both populate the same `fr->overfit` struct, so the
display layer reads `wf->label_kind` to know how to format the reason text.

### Walk-Forward results display panel (`Backtest/BacktestPanels.hpp`)

Reads `wf->label_kind` and formats accordingly:

- **Aggregate row**:
  - Classification: `Val Accuracy: NN.N% +/- M.M% (train: NN.N%)` — existing
  - Regression: `Val Pearson r: 0.NNNN (train: 0.NNNN, MSE: 0.NNNNNN)` —
    Pearson r is the load-bearing metric; MSE is contextual.
- **Tooltips** — kind-specific. Regression tooltip explicitly explains the
  signal-vs-noise scale (`|r| < 0.05` = no signal, `0.05–0.2` = weak, `> 0.2`
  = strong at tick scale).
- **Per-fold table** — column headers and content:
  - Classification: `Train | Val | Gap | Status` (existing)
  - Regression: `Train r | Val r | Val MSE | Status` with `Val r` colored by
    magnitude (green > 0.20, yellow 0.05–0.20, red < 0.05).
- **Overfit warning text** — same structure for both kinds; reason text comes
  from the kind-specific detector and is displayed verbatim.

### CLAUDE.md "Label-type-aware metric invariant" (Safety Invariants section)

New subsection codifies the rule explicitly:

> Every metric, display, training, or validation site that touches label
> values MUST consult `label_table[t].num_classes` (via `LabelType_*` helpers)
> and branch on the kind.

Includes the 2026-04-25 postmortem inline (regression labels through binary
display = nonsense numbers), a table of the four label kinds with their
metric semantics, and the six concrete sites that must branch on kind. Notes
that enforcement is still on the human — the compiler doesn't catch a new
metric site that simply doesn't call the helpers — and flags `enum class
LabelKind` as deferred future hardening for compile-time exhaustive checks.

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
