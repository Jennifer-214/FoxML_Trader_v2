# ML training — class imbalance handling

This doc explains how to train a properly-weighted XGBoost model when
class distribution is uneven, which is the common case for binary
barrier labels and 3-class peak/valley/stable softmax labels.

The training script lives outside this repo (Python / XGBoost).
foxml_suite reads the trained `.bin` model + `.stamp` body. This doc
is the contract between the trainer and the engine — get the weights
wrong and live performance silently degrades.

## Why this matters

The engine doesn't know what label distribution your training data
had. It just loads weights, runs `XGBoosterPredict`, and trades on
the output. If the trainer used uniform sample weights against a
4.2 / 48.3 / 47.5 distribution (the v5.8 paper-test
distribution observed in PEAK_VALLEY_STABLE), the model:

- Memorizes the majority classes (peak ~48%, valley ~47%)
- Mostly gets stable (~4%) wrong
- Reports >90% train accuracy from majority-class-correct alone
- Scores poorly on the minority class at inference time

Live engine never sees this — operator sees "high accuracy in
training" + "no entries firing" and assumes the engine is broken.
Real fix is upstream in the trainer.

## Inverse-frequency sample weights

Standard correction: weight each sample by `1 / class_frequency`.
Equivalent to "every class contributes equally to the loss
regardless of population."

```python
import numpy as np
from collections import Counter

def inverse_freq_weights(y):
    counts = Counter(y)
    total = len(y)
    n_classes = len(counts)
    # weight[c] = total / (n_classes * count[c]) — sklearn's
    # `compute_class_weight("balanced", ...)` formula
    weights = {c: total / (n_classes * counts[c]) for c in counts}
    return np.array([weights[label] for label in y])

sample_weight = inverse_freq_weights(y_train)
# pass to xgboost via:
dtrain = xgb.DMatrix(X_train, label=y_train, weight=sample_weight)
```

For the 4.2/48.3/47.5 distribution that gives weights of roughly
**7.94 / 0.69 / 0.70**. The minority class punches 11x harder in
the loss; majority classes are slightly damped.

## Multi-class `scale_pos_weight` (binary only)

XGBoost's built-in `scale_pos_weight` parameter is binary only.
For PEAK_VALLEY_STABLE (3 classes) it does nothing. Use
`sample_weight` as above. For binary labels (WIN_LOSS, BARRIER,
WILL_PEAK, WILL_VALLEY):

```python
n_pos = (y_train == 1).sum()
n_neg = (y_train == 0).sum()
spw = n_neg / max(n_pos, 1)
booster = xgb.train(
    params={"objective": "binary:logistic",
            "scale_pos_weight": spw,
            ...},
    dtrain=dtrain, ...)
```

The engine has helper `XGBoost_ComputeScalePosWeight` that does
this on the suite side. Look at `Backtest/BacktestEngine.hpp` for
the signature; it produces the same weight the Python trainer
would.

## Held-out + walk-forward — weights are training-only

`sample_weight` only applies to the training fold. For
held-out and walk-forward evaluation, accuracy / Pearson r are
computed unweighted (per-sample equal contribution). This is
deliberate: validation accuracy on a balanced metric tells you
how well the model would do on unseen data of the same
distribution as production.

If your held-out set has a different class distribution than
training, that's a separate concern — investigate
class-stratified split before adjusting weights.

## How to detect imbalance from foxml_suite

1. Run a backtest with sample collection enabled
2. Open the **Past Runs** panel — the row for that run will show
   "ML Samples: N"
3. Run the trainer offline
4. Check the trainer's class-distribution log (you'll see
   numbers like `class 0: 4.2%, class 1: 48.3%, class 2: 47.5%`)
5. If any class is below ~10%, weight matters. Use the formula
   above.

## Known distributions (as of v5.9)

| Dataset | Label kind | Distribution | Weight strategy |
|---|---|---|---|
| 30-day BTCUSDT v5.8 paper | PEAK_VALLEY_STABLE (3-class) | 4.2 / 48.3 / 47.5 | inverse-freq sample_weight |
| 30-day BTCUSDT v5.8 paper | BARRIER (binary) | ~3% positives | scale_pos_weight ≈ 32 |
| 30-day BTCUSDT v5.8 paper | WIN_LOSS (binary) | ~50/50 | no weighting needed |

These distributions shift when you change `label_tp_pct` /
`label_sl_pct` / `label_forward_ticks` — re-check after every
cfg change.

## Anti-patterns

### "I'll use class_weight='balanced'"

`xgb.XGBClassifier` accepts this parameter, but the
underlying `xgb.train` does not. Use explicit `sample_weight`
to be sure the booster sees the weights.

### "I'll up-sample the minority class"

Synthetic up-sampling (SMOTE, simple replication) on time-series
features causes look-ahead bias. The replicated samples leak
their feature values into folds containing them. Don't do it.

### "I'll just lower the threshold"

Lowering `ml_buy_threshold` to compensate for low-quality
minority-class predictions doesn't fix the underlying problem
— you're just making the model bet more often on coin flips.
Live performance gets worse.

### "I'll use sample_weight but with smaller magnitudes"

Inverse-freq is mathematically correct: it makes the loss
balanced. Half-strength (sqrt of inverse-freq) is heuristic
and harder to reason about. Stick with the formula.

## See also

- `Backtest/BacktestEngine.hpp` — `XGBoost_ComputeScalePosWeight`
  (binary), `XGBoost_ComputeMulticlassWeights` (multiclass)
  — these are computed inside the suite for consistency
- `DOCS/CLAUDE_ML_INVARIANTS.md` rule 8 — Features_PackAll
  validates output (post-v5.9.0); the trainer must produce
  features that pass the same validation
- The audit at `DOCS/V5_9_ML_HARDENING_AUDIT.md` finding #3
  for the full diagnosis of the v5.8 imbalance issue
