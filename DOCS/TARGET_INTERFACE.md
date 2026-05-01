# Target / Label Interface

**Read this when adding a new ML target.** The contract reuses the
existing `label_table[]` registry in `Backtest/LabelFunctions.hpp`
— that file's comment says it best: "adding a new label type = 1
function + 1 table entry."

---

## Existing registry (already X-macro-shaped)

`Backtest/LabelFunctions.hpp` defines:

```cpp
#define LABEL_WIN_LOSS           0
#define LABEL_BARRIER            1
#define LABEL_FORWARD_PNL        2
#define LABEL_REGIME             3
#define LABEL_VOL_BARRIER        4
#define LABEL_WILL_PEAK          5
#define LABEL_WILL_VALLEY        6
#define LABEL_PEAK_VALLEY_STABLE 7

static const LabelTableEntry label_table[] = {
    { LABEL_WIN_LOSS,    "win_loss",     "Win/Loss",     /* fn ptr */, ... },
    { LABEL_BARRIER,     "barrier",      "Barrier",      ... },
    /* etc */
};
```

This is essentially the `FOREACH_TARGET(X)` registry; v5.8/v5.9 don't
need to introduce a new pattern.

## Canonical signature

```cpp
inline float Label_<Name>(
    const HistoricalTick* ticks,
    int n_ticks,
    int idx,
    /* label-specific params: tp_pct, sl_pct, horizon, etc. */);
```

Returns `float` value. Interpretation by `label_kind`:
- 0 = binary (0.0 or 1.0)
- 1 = regression (any float)
- ≥2 = multiclass (integer cast as float, 0..N-1)

`HistoricalTick` is defined in `LabelFunctions.hpp:25`. Labels look
forward in tick history, so they MUST be computed in a backtest
post-processing pass after the full sample set exists.

## Recipe — adding a new target

```
1. Add label compute function to Backtest/LabelFunctions.hpp:

   inline float Label_MyTarget(
       const HistoricalTick* ticks, int n, int idx,
       float horizon_ticks /* params */)
   {
       /* scan forward from idx, compute value */
       return /* float */;
   }

2. Add #define LABEL_MY_TARGET = next_id at the top of the file:

   #define LABEL_MY_TARGET 8  // (if 7 is the current highest)

3. Append entry to label_table[]:

   { LABEL_MY_TARGET, "my_target", "My Target", Label_MyTarget,
     /* label_kind: 0=binary, 1=regression, 2+=multiclass */,
     /* default params */ }

4. Run: ./build.sh test

5. Train per-target via Backtest_RunFullValidation(..., LABEL_MY_TARGET).
   Stamp records target_kind. CoreModelZoo loads to its slot.

DONE. 2 sites total: function + table entry.
```

## Multi-target deployment (post-v5.9.2e)

When v5.9.2e ships target ranking + multi-target training:

- Each target gets its own stamped model
- Path convention: `models/<label_name>/model.bin` + matching `.stamp`
  (where `<label_name>` is `label_table[i].name`)
- Stamp body's `target_kind` field carries the LABEL_* constant
- `verify_model_stamp` rejects load if `target_kind` doesn't match
  the caller's expected kind
- `CoreModelZoo` loads multiple target-specific models per core (one
  per role: barrier, regime, exit, buy_signal — extend with target-
  specific roles)
- Strategy at inference picks ranked-best target's model OR blends

## label_kind semantics

`label_kind` dictates training objective + metric interpretation:

| Kind | Training objective | Metric (in WalkForwardFoldResult) |
|---|---|---|
| 0 binary | `binary:logistic` | accuracy, AUC |
| 1 regression | `reg:squarederror` | MSE, Pearson correlation |
| 2+ multiclass | `multi:softprob` (num_class = label_kind) | multiclass accuracy |

Existing helpers:
- `XGBoost_ComputeScalePosWeight` — class-imbalance for binary
- `XGBoost_ComputeMulticlassWeights` — class-imbalance for multiclass
- `WalkForward_ComputeAccuracy` / `_ComputeMSE` / `_ComputeCorrelation` /
  `_ComputeMulticlassAccuracy` — metric computation per kind

## What NOT to do

- **Don't compute labels at training time using look-ahead data** that
  wouldn't be available in production. Labels look forward in TIME
  but only into data that already existed at sample-construction
  time — never into future samples.
- **Don't bypass `label_table`** by hardcoding label kind in your
  training script. Consumers (BacktestEngine, ModelInference, GUI
  display) all dispatch by `label_table[i]` — going around it =
  drift.
- **Don't add a target whose label kind doesn't fit 0/1/≥2.** If you
  need a different shape (multi-output regression, structured output,
  etc.), extend the existing kind enum + update all the metric
  helpers. Don't shoehorn into "regression" with magic post-
  processing.

## Related

- `Backtest/LabelFunctions.hpp` — the registry itself
- `Backtest/BacktestEngine.hpp` — `Backtest_RunFullValidation` takes
  `label_type` parameter
- `ML_Headers/CoreModelZoo.hpp` — multi-model loader (used for
  per-target deployment)
- `ML_Headers/ModelInference.hpp` — `verify_model_stamp` (rejects
  target_kind mismatch post-v5.9.0)
- `DOCS/EASY_ADDITIONS_INVARIANTS.md` — the X-macro pattern overall
