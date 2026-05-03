# Strategy implementation refactor — future-work tracker

**Status:** **CLOSED 2026-05-01.** Implemented in v5.8.0 (Phase 1 of
v5.8 Easy Additions sprint). The X-macro `FOREACH_STRATEGY(X)`
registry now drives strategy enum + name arrays + 4 lifecycle
dispatchers from a single row. Adding a strategy is 3 sites instead
of the 8 documented below: implement the strategy file, append one
`FOREACH_STRATEGY(X)` row in `Strategies/StrategyInterface.hpp`, add
GUI color (the only manual touch remaining).

See `DOCS/EASY_ADDITIONS_INVARIANTS.md` for the canonical spec +
`DOCS/changelogs/2026-05-01-v5.8-easy-additions.md` for the ship
narrative. The "Option A" proposed below is what shipped.

This file is preserved as the historical context for why the work
was undertaken — useful when explaining the X-macro pattern to
someone seeing it for the first time.

---

**Originally logged:** 2026-04-30, mid-v5.7 ship. User asked whether
plug-and-play extensibility could be tighter. Honest answer: yes, but
the current pattern works and the refactor would be pure
infrastructure with no functional payoff until a new strategy
actually lands.

## Current state — what adding a strategy touches today

For a new public-tier strategy `FooStrategy`:

1. **`Strategies/StrategyInterface.hpp`** — add `STRATEGY_FOO` constant.
   Add `"FOO"` to `STRATEGY_SHORT_NAMES[]` and full name to
   `STRATEGY_FULL_NAMES[]`. Bump `NUM_STRATEGIES`.
2. **`Strategies/FooStrategy.hpp`** — implementation file with
   `FooState<F>` struct and `FooStrategy_Init` / `_Adapt` /
   `_BuildParameters` / `_ExitAdjust` functions per
   `DOCS/STRATEGY_INTERFACE.md`.
3. **`Strategies/StrategyParameters.hpp`** — add `case STRATEGY_FOO:`
   to `Strategy_BuildParameters` dispatcher.
4. **`Strategies/StrategyLifecycle.hpp`** — add cases in
   `Strategy_InitPerCore`, `Strategy_AdaptPerCore`,
   `Strategy_ExitAdjustPerCore`.
5. **`CoreFrameworks/ControllerConfig.hpp`** — add `<name>` parser key
   for `core_N_strategy=foo` cfg syntax.
6. **`Strategies/RegimeDetector.hpp`** — if FOO should be
   regime-resolvable, extend `REGIME_STRATEGY_TABLE[]`.
7. **`GUI/DashboardPanels.hpp`** — add color to `strat_colors[]`,
   tooltip strings, etc.
8. **`tests/controller_test.cpp`** — add tests for FOO behavior.

8 touch points × 1-3 LOC each = ~30-60 LOC of integration code per new
strategy. Plus the implementation itself.

## Why this hasn't been refactored yet

- Only 5 strategies exist. The pattern scales linearly; pain isn't
  (yet) painful.
- Adding a strategy is rare (every few months).
- The X-macro candidate would touch all 5 existing strategies'
  integration sites. Real regression risk.
- v5.4.0 - v5.7.5 had higher-leverage work (lifecycle restoration,
  visibility, quality filters).

## Refactor candidates (in order of preference)

### Option A — X-macro registry (preferred)

One X-macro entry per strategy in `StrategyInterface.hpp`:

```cpp
#define FOREACH_STRATEGY(X) \
    X(MEAN_REVERSION, MR,   "MeanReversion", MeanReversionState, \
       MeanReversion_Init, MeanReversion_BuildParameters, \
       MeanReversion_Adapt, MeanReversion_ExitAdjustSharded) \
    X(MOMENTUM,       MOM,  "Momentum",      MomentumState, \
       Momentum_Init,        Momentum_BuildParameters, \
       Momentum_Adapt,       Momentum_ExitAdjustSharded) \
    X(SIMPLE_DIP,     DIP,  "SimpleDip",     SimpleDipState, \
       SimpleDip_Init,       SimpleDip_BuildParameters, \
       /* no adapt */ NULL, SimpleDip_ExitAdjustSharded) \
    X(ML,             ML,   "ML",            MLState, \
       ML_Init,              ML_BuildParameters, \
       ML_Adapt,             ML_ExitAdjustSharded) \
    X(EMA_CROSS,      EMA,  "EmaCross",      EmaCrossState, \
       EmaCross_Init,        EmaCross_BuildParameters, \
       EmaCross_Adapt,       EmaCross_ExitAdjustSharded)
```

Then auto-generate:
- `STRATEGY_*` constants via `X(name, _, _, _, _, _, _, _) STRATEGY_##name = i,`
- `STRATEGY_SHORT_NAMES[]` array
- `STRATEGY_FULL_NAMES[]` array
- `Strategy_BuildParameters` dispatcher cases
- `Strategy_InitPerCore` / `Strategy_AdaptPerCore` /
  `Strategy_ExitAdjustPerCore` dispatcher cases
- Cfg parser cases (string → ID lookup table)

Adding a strategy:
1. Append one line to `FOREACH_STRATEGY(X)` in StrategyInterface.hpp.
2. Implement the strategy file with the conventional function names.
3. Done. Tests + display name updates all auto-flow.

**Pros:**
- Idiomatic for this codebase (same pattern as `INT X-macro extension`
  for per-core overrides — shipped in v5.0.x).
- All dispatch tables stay in sync by construction.
- New strategy = one line + one file.

**Cons:**
- 5 existing strategies must conform to the conventional function
  signatures. Small refactor of 1-2 strategies whose signatures
  drift from convention.
- Macros are noisy in compiler errors.

**Estimated effort**: 6-8h. ~60% migration of existing strategies,
~40% getting all dispatchers to compile through the X-macro.

### Option B — vtable / function-pointer registry

Each strategy defines a static `Strategy<F>` struct holding function
pointers; dispatcher iterates a `g_strategies[]` table indexed by
strategy_id.

**Pros:**
- Pure C-compatible (could ease future binding to other languages).
- Trivial to add a strategy at runtime (theoretically — though we
  don't currently want this).

**Cons:**
- Adds an indirect call at every dispatch site. Hot-path-adjacent
  (slow path runs Strategy_BuildParameters every poll_interval).
  Branch-table dispatch (current `case` switch) compiles to a
  jump table — usually faster than indirect calls.
- More code than Option A for the same payoff.

### Option C — code generation from a YAML / JSON manifest

Strategies declared in a manifest, dispatcher code generated by a
script run at build time.

**Pros:**
- Strategy "registration" is declarative + viewable in one place.

**Cons:**
- New tool dependency (Python?).
- Build complexity: header generated by external tool.
- Doesn't fit this codebase's "everything is C++ headers" aesthetic.

**Recommendation if/when this lands: Option A.**

## Trigger conditions (when to revisit)

Pick this back up when ANY of:

1. **Adding a 6th strategy.** Sit through the 8-touch-point integration
   once more, count the time it actually took, decide if X-macro pays
   for itself.
2. **A regression bug traces to an out-of-sync dispatcher.** E.g.
   strategy added to `Strategy_BuildParameters` but forgotten in
   `Strategy_AdaptPerCore`. Pattern would have prevented it; if it
   bites, that's the trigger.
3. **A strategy refactor (e.g. swapping function signatures) requires
   touching 5 dispatchers in lockstep.** When you next find yourself
   doing exactly this and grumbling, Option A would have made it
   one-line.

If none of these happen in 6 months → keep deferring.

## Adjacent work that should NOT trigger this refactor

- **"Strategy quality dashboard"** (v5.7.6) — reads health log,
  doesn't add a strategy.
- **"MOM quality filters"** (v5.7.5) — modifies one existing strategy.
- **"Regime classifier tuning"** — modifies regime, not strategies.
- **"Public release snapshot"** — already handled via
  `Strategies/private/` + `__has_include`.
- **"Backtest experiment runner"** (planned) — no new strategies.

The refactor is specifically about "adding strategy N+1 friction." Do
NOT bundle it with unrelated work.

## Notes on the public/private split

Whatever refactor pattern lands, it must preserve:

- `Strategies/private/<Name>.hpp` files conditionally included via
  `__has_include` so public release snapshots still build.
- The X-macro entry can use a guard: `#if __has_include("private/EmaCross.hpp")`
  around the EMA_CROSS line. Public snapshots drop the line, dispatcher
  auto-omits the case, build clean.

## Slow-path "optimization" — separate concern

User mentioned "optimizing slow path" alongside this question. The
slow-path latency creep observed in v5.5.x was thermal throttling on
the i9-9980HK (5GHz turbo collapsing to ~46% under sustained load
when CPU hit 100°C) — verified via `cat /proc/cpuinfo` cur_freq
during the issue. NOT a slow-path code problem.

If sustained slow-path measurement is wanted: `chrt -f 90 taskset -c 4-7`
on the engine binary plus a CPU cooler upgrade is the path. No code
optimization will fix thermal-throttle-induced jitter.

This concern is independent of the strategy refactor; both can be
deferred without affecting each other.

---

## How to use this doc

When you next consider adding a strategy or refactoring the dispatcher,
read the trigger conditions section. If one fires, walk through Option
A's effort estimate. If not, leave this doc alone — it's a parking
spot, not a TODO.

Last reviewed: 2026-04-30. No trigger conditions met.
