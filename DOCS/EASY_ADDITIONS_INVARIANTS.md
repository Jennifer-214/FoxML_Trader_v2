# Easy Additions Invariants

**Read this before** adding a new strategy, ML feature, target, SHALT
code, halt reason, regime, or stateful GUI panel. Read also before
proposing a new "thing" category (some won't be worth standardizing —
this doc says which).

The codebase aims for one rule: **adding the next instance of an
extensibility category should touch as few sites as possible.** Most
common categories should be ≤3 sites. The X-macro registry is the
codebase's standard pattern.

---

## Standard X-macro pattern

Every category that's been standardized follows the same shape:

```cpp
// Define the registry as an X-macro list. Each entry = one row.
#define FOREACH_<CATEGORY>(X) \
    X(<id>, <name>, <metadata...>, <function pointer or sentinel>) \
    X(<id>, ...) \
    /* ... */

// Auto-generate consumers from the X-macro:
//   - enum constants
//   - name arrays for display
//   - dispatch tables (function pointer arrays)
//   - compile-time hash for fingerprint contribution

#define X(id, name, ...) <CATEGORY>_##id,
enum <Category>Id {
    FOREACH_<CATEGORY>(X)
    NUM_<CATEGORY>
};
#undef X
```

**Adding the N+1 instance:** append one row to `FOREACH_<CATEGORY>(X)`,
implement the function the row references, recompile. Every consumer
of the registry picks up the new entry automatically.

---

## Audited categories — current state and post-v5.8 target

| Category | Current sites | Post-v5.8 target | Phase | Notes |
|---|---|---|---|---|
| Strategies | 8 | 3 | v5.8.0 | strategy file + X-macro line + GUI color (color is the only manual step) |
| ML features | 5-7 | 2 | v5.8.2 | compute fn + X-macro line |
| SHALT codes | 4 | 1 | v5.8.3 | one X-macro row, names auto-generated |
| Controller halt_reason | 3 | 1 | v5.8.4 | same |
| Regimes | 5 | 1 | v5.8.5 | one X-macro row |
| Targets (label_table) | already 2 | 2 | n/a (existing) | reuses existing `label_table[]` in LabelFunctions.hpp |
| Stateful GUI panels | 4 | 2 | v5.8.4b | only 4 of 14 panels are stateful — only those benefit |
| Backtest metrics | 5 | 2 | v5.8.4c | includes `OPT_METRIC_*` enum + `Backtest_RunSweep` dispatch |
| Per-core overrides | 1 | 1 | done v5.0.x | already X-macroized as `PER_CORE_OVERRIDE_FIELDS` |

## Audited categories — DEFERRED (not standardized)

These were considered and rejected for v5.8. Each has a trigger
condition for revisiting.

| Category | Why deferred | Revisit when |
|---|---|---|
| Stateless GUI panels | `GUI_Panel_X(snap)` is already a 1-liner; abstraction adds complexity for no win | A stateless panel becomes stateful, OR 5+ are added at once |
| Snapshot fields (PerCoreSnap, TUISnapshot) | Each field is genuinely heterogeneous (different sources, types, update cadence). Abstraction cost > benefit | Individual cases — apply judgment per field |
| Cfg fields | Already covered by `CFG_PARSE_INT/FPN/PCT/STR/U32` macros | Group of 20+ related fields would benefit from a sub-X-macro |
| Health log categories | Already minimal touches (~2 sites: emit + log viewer filter) | Categories exceed 10 OR a category needs structured payload schema |
| REST endpoints (Binance) | Tightly coupled to single exchange. Belongs in broker-abstraction work | Adding a non-Binance broker |
| Order types | Currently small enum (MARKET_BUY/SELL); no pain | Adding OCO + LIMIT order types |
| Model backends | XGBoost only today | Adding LightGBM / PyTorch / etc |
| Test sections | Just `printf` banners; no abstraction needed | Never |
| Build flags | CMake-level; existing `-DUSE_*` pattern works | Never |

---

## Canonical signature audit (v5.8 Phase 0 finding)

For each category, every implementation must conform to a canonical
function signature. Drift between implementations = X-macro can't
write a uniform function pointer table.

### Strategy lifecycle (5 stages)

**`_Init` — uniform across all 5 strategies ✅**

```cpp
inline void <Name>_Init(
    <Name>State<F>* state,
    const RollingStats<F>* rolling,
    BuySideGateConditions<F>* buy_conds);
```

**`_Adapt` — drift detected ⚠**

4 of 5 strategies match:
```cpp
inline void <Name>_Adapt(
    <Name>State<F>* state,
    FPN<F> current_price,
    FPN<F> portfolio_delta,
    uint16_t active_bitmap,
    const BuySideGateConditions<F>* buy_conds,
    const ControllerConfig<F>* cfg);
```

`MLStrategy_Adapt` is the outlier — takes `const void* cfg` instead
of `const ControllerConfig<F>* cfg`. Likely was an include-cycle
workaround at the time of writing.

**Fix for v5.8.0:** add a thin adapter
`MLStrategy_Adapt_Canonical` that casts the void* and calls the
real function. The X-macro references the adapter. Real function
preserved for legacy callers.

**`_BuildParameters` — uniform across SimpleDip / MeanReversion / Momentum / EmaCross ✅**

```cpp
inline void <Name>_BuildParameters(
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out,
    <Name>State<F>* state = nullptr);
```

**`ML_BuildParameters` is different shape ⚠** — takes additional
`const RollingStats<F, WL>* rolling_long` parameter (ML uses long-
window features). The dispatcher in `Strategy_BuildParameters`
handles this via case-by-case dispatch.

**Fix for v5.8.0:** the X-macro can reference each strategy's
`_BuildParameters` directly via case-block dispatch (preserving
ML's wider signature) rather than via uniform function pointer.
Slightly less clean than full table-dispatch but matches existing
pattern.

**`_ExitAdjustSharded` — uniform across all 5 ✅**

Verified by `tools/calls_graph_diff.sh` — all 5 wired correctly
in `StrategyLifecycle.hpp`.

### ML feature compute (proposed canonical, no drift today)

```cpp
template <unsigned F>
inline FPN<F> ML_Compute_<Name>(const FeatureComputeCtx<F>* ctx);
```

`FeatureComputeCtx` is the bundle of all available inputs (rolling,
EMA, ROR, flow, depth, spread). Each compute fn reads what it needs,
returns FPN<F>. `FPN_Zero` is the safe "I don't have data yet" return.

### Target / label functions (already canonical via label_table)

```cpp
inline float Label_<Name>(
    const HistoricalTick* ticks,
    int n_ticks,
    int idx,
    /* label-specific params */);
```

Returns float label value. label_kind dictates interpretation
(0=binary, 1=regression, ≥2=multiclass).

---

## Public/private split

Some implementations live in `Strategies/private/` (alpha-flavored).
Currently this is just `EmaCross.hpp`. The X-macro should
conditionally include via `__has_include`:

```cpp
#define FOREACH_STRATEGY(X) \
    X(MEAN_REVERSION, /* ... */) \
    X(MOMENTUM,       /* ... */) \
    X(SIMPLE_DIP,     /* ... */) \
    X(ML,             /* ... */) \
    EMACROSS_X_LINE(X)

#if __has_include("private/EmaCross.hpp")
#  define EMACROSS_X_LINE(X) \
    X(EMA_CROSS, /* ... */)
#else
#  define EMACROSS_X_LINE(X) /* nothing */
#endif
```

**Honest caveat (2026-05-01):** the existing README claim "rm -rf
Strategies/private/ and the build still passes" is aspirational.
Removing private/ today breaks compilation because `EmaCrossState<F>`
references in StrategyParameters.hpp + StrategyLifecycle.hpp aren't
guarded. v5.8.0 Phase 0 added `__has_include` guards around the
`#include` statements, but the type-references still need
`#ifdef HAS_EMACROSS` guards for the public-release snapshot to work.
Filing as future work in `STRATEGY_REFACTOR_IDEAS.md`.

---

## Recurring bug pattern this prevents

**Class 1 — Strategy lifecycle orphans** (see
`DOCS/RECURRING_BUG_PATTERNS.md`). Adding a strategy stage in code
but forgetting the dispatcher wiring → silent dead behavior.
v5.4.0 had this in all 5 strategies for `_Init`, `_Adapt`,
`_BuySignal`, `_ExitAdjust`. The X-macro registry forces the
dispatcher entry to exist or fail compilation.

**Prevention:** readiness Check 14 (function-pointer table
correctness) requires:
- Variant selection audit (which `_BuildParameters` did existing
  dispatcher reference?)
- Signature uniformity audit (this doc)
- `tools/calls_graph_diff.sh` before AND after
- Loop test that walks every X-macro entry asserting non-null
  function pointers
- Hash snapshot test for any hash that contributes to fingerprints

---

## Future categories to consider (not in v5.8)

If any of the following becomes painful in a future ship, revisit:

- **Risk gate types** — currently mixed (cfg-driven thresholds,
  hardcoded checks). Could become a registry with checker function
  pointers.
- **Notification channels** — when alerting infrastructure lands.
- **Backtest output formats** — CSV / JSONL / stamp metadata.
  Currently each is its own writer.
- **Model backends** — XGBoost only today. Future LightGBM /
  PyTorch would need an X-macro at the `Model_Load`/`Model_Predict`
  layer.

---

## Maintenance discipline

- **Don't add a category to the audit table without a real cost-benefit.**
  Theoretical "we might add 5 of these someday" doesn't justify the
  refactor.
- **Don't remove an entry from the deferred table without a trigger.**
  The triggers are in this doc for a reason.
- **When you add a new entry to a registry, verify** the
  `calls_graph_diff.sh` baseline still shows clean (no new orphans),
  the loop test passes, and any hash snapshot tests get their values
  updated deliberately.
