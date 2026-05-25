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

## Scaler-aware training (v5.9.3+)

v5.9.3 added feature standardization (mean-centering + unit-variance
scaling) via the `.scaler` sidecar binary. Operator workflow change:

### Training a model with the scaler enabled

1. **Run training as usual** — `Train Model` button in foxml suite.
   The worker auto-computes the scaler over the post-WF compacted
   training matrix and persists `<model>.scaler` next to the
   `<model>.bin` file. No cfg flag needed.

2. **Read the worker log** for two pieces of info:
   - `[train] scaler persisted: /path/to/<model>.scaler`
   - `[train] scaler_sha256=<hex> — pass to stamp tool to bind`
   The status_msg in the GUI also shows `[+scaler]` suffix when
   persist succeeded.

3. **Bind the scaler to the stamp** — automatic via foxml_suite GUI auto-stamp flow.

   *(v5.15.5.F.4d.1.B.3 Path C 2026-05-24: legacy `tools/stamp_model.sh` bash CLI
   DELETED. Operator workflow now uses foxml_suite GUI auto-stamp via
   Backtest_RunFullValidation → tt::Stamp_AssembleAndEmit at
   `Backtest/BacktestEngine.hpp:1202`. Scaler fields auto-populate from training
   state; no manual bash invocation required. cmdline-invocable training queued
   for v5.16+ per `plans/_future/2026-05-12-decoupling-endgoal-roadmap.md`.)*

   **Procedure:** Run foxml_suite → Run Control panel → "Run Full Validation"
   button. Stamp emits at validation close with all cfg-derived + scaler fields
   auto-bound. Stamp body bytes are HMAC-signed via `held_out_stamp_secret` cfg
   field — set in `engine.cfg` (live) or `backtest.cfg` (training) BEFORE
   running validation.

4. **Verify the binding** at engine boot:
   - ML Status panel shows green "scaler: applied (registry_hash=...)"
   - Boot stderr emits: `[scaler] <model>.scaler: loaded (registry_hash=..., num_features=N)`
   - Entry log shows post-scaler features (mean ≈ 0, stddev ≈ 1
     vs raw values pre-v5.9.3)

### Training a model WITHOUT the scaler

The scaler is optional. To skip:
- Train via foxml_suite with scaler persist disabled (toggle in training panel)
- Stamp body lacks the field; engine loads with `has_scaler_fields=0`,
  `feature_scaler_present=0`. Apply path early-returns identity.
- ML Status panel shows sand "scaler: NONE (legacy v5 model)"

This is the default behavior for legacy v5.x models (which have no
scaler) and for any v5.9.3+ model where the operator deliberately
opts out. Behavior is bytewise-identical to pre-v5.9.3 — the scaler
infrastructure is fully forward-compat.

### Why scaling matters

Without standardization, XGBoost trees split on raw feature values.
Trees adapt to feature scales fine, but TWO classes of bugs are
silently worse without scaling:

1. **Numerical conditioning at the prediction layer:** very large
   features (e.g. price=60000) overwhelm small-magnitude features
   (e.g. ema_sma_spread=0.0015) in any non-tree model the codebase
   may add later (logistic regression, neural nets). Scaling
   future-proofs the feature pipeline.

2. **Drift detection:** post-scaler features have a known
   distribution (mean ≈ 0, stddev ≈ 1 on the training data). At
   inference, deviations >> 3 stddev signal market regime that
   the model wasn't trained on. Pre-scaler this is invisible.

### Scaler invalidation triggers

The scaler binds to a SPECIFIC training set + feature set + build.
It MUST be regenerated when:

- **`FOREACH_FEATURE` changes** (add/remove/reorder, version bump)
  → `FEATURE_REGISTRY_HASH` flips; sidecar's embedded hash mismatches
  build's hash; scaler refused at load. Retrain mandatory.
- **Feature compute fn body changes** (snapshot test in v5.9.2a
  catches this) → feature output values change; old scaler stats
  are wrong for new feature distribution. Retrain mandatory.
- **Training data changes** (different period, different symbol) →
  scaler stats reflect specific data distribution. Retrain
  recommended for distribution shifts.

`MODEL_FORMAT_VERSION` does NOT need to bump for any of these — the
forward-compat parser pattern handles backward compat (legacy stamps
without scaler fields still load).

## See also

- `Backtest/BacktestEngine.hpp` — `XGBoost_ComputeScalePosWeight`
  (binary), `XGBoost_ComputeMulticlassWeights` (multiclass)
  — these are computed inside the suite for consistency
- `DOCS/CLAUDE_ML_INVARIANTS.md` — rules covering scaler binding,
  stddev floor identity, 3-tier strict mode, atomic write contract
- `DOCS/PARITY_LIFECYCLE.md` — operator-facing change matrix
  including "Scaler sidecar" row
- `DOCS/PARITY_VERIFICATION_CHECKLIST.md` Surface F — operational
  checklist for verifying scaler-bound models work end-to-end
- The audit at `DOCS/V5_9_ML_HARDENING_AUDIT.md` finding #3
  for the full diagnosis of the v5.8 imbalance issue
- `ML_Headers/FeatureStandardizer.hpp` — implementation reference

## Multi-Horizon training (v5.10.0a-final / v5.11.41+)

Train N models in one pass, each predicting at a different forward
horizon (e.g. 1000 / 7500 / 15000 ticks). Engine ensemble auto-detects
horizon siblings and combines their predictions via Bandit-Exp3
weighted blend per regime.

### Operator workflow

In foxml_suite Training panel:

1. **Set horizons** in the Horizons (CSV) field — e.g. `1000,7500,15000`
2. **Set TP/SL barriers** — broadcast (single value) OR positional (N values matching horizon count)
3. **Set Run Name (prefix)** — e.g. `btc_5min_v1` (worker auto-appends `_horizon_<H>` per horizon)
4. **Click Collect Multi-Horizon** — collects features once, then loops over horizons recomputing labels (for inspecting per-horizon class distribution before training)
5. **Click Train Multi-Horizon** — trains N models sequentially. For each horizon: full XGBoost training + walk-forward CV + held-out validation + auto-stamp. Output saved to:
   ```
   models/<class>/<run_name>_horizon_<H>/<role>.json
   models/<class>/<run_name>_horizon_<H>/<role>.json.stamp
   models/<class>/<run_name>_horizon_<H>/summary.txt
   ```
6. **Past Runs panel** shows all N horizons grouped under `<run_name> [N horizons]` with per-horizon WF + held-out + gap metrics.

### Engine deployment

Set in `engine.cfg` (or per-core override):
```
core_0_model_dir = models/classification/btc_5min_v1
```
Engine ensemble auto-detect scans for `<base>_horizon_<H>` siblings; loads all matching horizons; runs Bandit-Exp3 weighted prediction blend per regime. Per-horizon weights persist across restarts in `data/bandit_state.json`.

### Stamping + signature verification

If `auto_stamp_secret` is set in cfg, all trained models are HMAC-signed.
Engine verifies signature at load (`held_out_gate_strict=1` refuses
mismatched stamps). Empty secret = devmode (engine accepts any stamp,
just checks format/SHA/registry hash). For production: set a long random
secret in engine.cfg + retain it across operator deployments.

### Caveats

- **`multi_horizon_max_threads` defaults to 1 (forced serial)** post-v5.11.45 due to libgomp/pthread interaction in XGBoost (see plans/_cross-cutting/2026-05-07-deferred-items.md "v5.11.45 landmine"). Setting `>=2` opt-in for parallel training; may segfault.
- **Multiclass labels with rare classes**: per-sample weight cap at 5.0 (v5.11.46) to prevent gradient overflow during XGBoost histogram building.
- **Stamps**: written ALWAYS post-v5.11.47 regardless of legacy `auto_stamp_on_held_out` cfg flag.
- **Horizon-mismatch refusal**: engine REFUSES to load a model from `<dir>_horizon_<H>` if the stamp's `label_lookahead_ticks` doesn't match `<H>` (catches dir rename / copy mistakes; v5.11.42).
