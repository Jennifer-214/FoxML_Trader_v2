# ML Feature Interface

**Read this when adding a new ML feature.** The contract is small
because v5.8.2 ships the X-macro registry that handles the
plumbing.

---

## Canonical signature

```cpp
template <unsigned F>
inline FPN<F> ML_Compute_<Name>(const FeatureComputeCtx<F>* ctx);
```

Returns the feature value as `FPN<F>`. Returns `FPN_Zero<F>()` when
the feature can't be computed yet (cold start, missing data).

## FeatureComputeCtx

The bundle of all available inputs. Each feature reads what it
needs:

```cpp
template <unsigned F>
struct FeatureComputeCtx {
    const RollingStats<F, 128>*       short_rolling;
    const RollingStats<F, 512>*       long_rolling;
    const RollingStats<F, 256>*       medium_rolling;
    const RollingStats<F, 1024>*      baseline_rolling;
    const FPN<F>*                     ema_price;
    const RORRegressor<F>*            ror;
    const FlowState*                  flow;
    const BookImbalanceHistory<F, 1024>* book_imb;
    const SpreadState<F, 1024>*       spread;
    const LargeTradeState<F, 1024>*   large_trade;
    const CumDeltaState<F>*           cumdelta;
    uint64_t                           timestamp_us;
};
```

When v5.8.2 ships the registry, this struct is the authoritative
input bundle. New compute functions take `const FeatureComputeCtx<F>*`
and read whatever fields they need.

## Recipe — adding a new feature

```
1. Add the compute function to ML_Headers/FeatureRegistry.hpp:

   template <unsigned F>
   inline FPN<F> ML_Compute_MyFeature(const FeatureComputeCtx<F>* ctx) {
       if (!ctx->short_rolling || ctx->short_rolling->count == 0) {
           return FPN_Zero<F>();  // safe default during warmup
       }
       return /* your computation */;
   }

2. Append one line to FOREACH_FEATURE(X):

   X(MY_FEATURE, "my_feature", 1, ENABLED, ML_Compute_MyFeature, "what it does")
   //                          ^                                    ^
   //                          version (bump on formula change)     1-line note
   //                          ENABLED | DISABLED — compile-time gate

3. Run: ./build.sh test

4. FEATURE_REGISTRY_HASH bumped → existing models reject at load
   (forces retrain). Train a new model:
     - Set up engine.cfg with the right model path
     - Run Backtest_RunFullValidation against your data
     - Auto-stamp produces models/<name>/model.bin + model.bin.stamp
       carrying the new hash.

DONE. 2 sites total: function + X-macro line.
```

## Versioning

Every X-macro line carries a version number:

```cpp
X(MY_FEATURE, "my_feature", 1, ENABLED, ML_Compute_MyFeature, "...")
//                          ^
//                          bump to 2 when you change the formula
```

Bumping the version flips `FEATURE_REGISTRY_HASH`. Existing models
fail load-time check. Forces retrain.

**When to bump:**
- Computation formula changes (different math, different windows)
- Required input fields change (now needs `ema_price` it didn't before)
- Default safe-return value changes

**When NOT to bump:**
- Adding a comment / renaming the local variable
- Optimizing the implementation but preserving outputs bit-for-bit

## Disabling a feature (compile-time)

```cpp
X(MY_FEATURE, "my_feature", 1, DISABLED, ML_Compute_MyFeature, "...")
//                              ^
//                              compile-time gate — feature compiled out
```

`Features_PackAll` skips DISABLED entries. Zero inference cost for
disabled features. The compute function still exists in the source
but isn't called.

**Use case:** experimental features kept in source but not in the
active set. Operator can re-enable by editing the X-macro line +
recompiling + retraining.

## Why fingerprint contribution matters

`FEATURE_REGISTRY_HASH` is computed at compile time as the FNV-1a
hash of all enabled-feature names + versions. Stamp body's
`feature_registry_hash` field carries the value at training time.
Load-time check rejects models trained against a different feature
set.

This makes "I added a feature but forgot to retrain" a compile-time
+ load-time error rather than a silent live-prediction-from-stale-
features bug.

## What NOT to do

- **Don't create a parallel feature registry.** The X-macro is the
  single source of truth.
- **Don't reference globals or statics in `ML_Compute_*`.** The
  compute function MUST be a pure function of `ctx`. Mutable state
  belongs in `RegimeState` / `RollingStats` / etc., which is what
  the ctx exposes.
- **Don't compute features on the hot path.** Feature compute runs
  on the slow path inside `Strategy_BuildParameters` (when ML is
  enabled) at slow-path cadence (~24-100 ticks). Hot path reads
  `cached_params`, doesn't recompute features.
- **Don't add features that can't be computed offline (during
  backtest).** Train-serve parity requires the compute function to
  produce identical outputs from the same input data, regardless of
  whether the data is live or replay. If your feature reads
  wall-clock time, exchange order book, etc., document explicitly
  in the note field and verify backtest produces matching values.

## Related

- `DOCS/STRATEGY_INTERFACE.md` — how strategies consume features
- `DOCS/EASY_ADDITIONS_INVARIANTS.md` — the X-macro pattern doc
- `DOCS/CLAUDE_INTEGRATION.md` — broader integration recipes
- `Strategies/RegimeDetector.hpp` — `RegimeSignals` (different
  extensibility surface — for regime classification, not ML)
