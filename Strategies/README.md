# Strategies/

Per-strategy code. Two tiers based on alpha-sensitivity:

## Public tier — `Strategies/*.hpp`

Generic, textbook-pattern strategies safe to publish:

| File | What it does |
|---|---|
| `MeanReversion.hpp` | Buy below avg, sell back to avg |
| `Momentum.hpp` | Buy on breakout above EMA + slope confirmation |
| `SimpleDip.hpp` | Buy below recent_high × (1 - dip_pct) |
| `MLStrategy.hpp` | XGBoost inference + BarrierGate + ConfidenceScorer |
| `RegimeDetector.hpp` | Score-based regime classifier (RANGING/TRENDING/VOLATILE/MILD_TREND) |
| `StrategyInterface.hpp` | `STRATEGY_*` constants + interface contract |
| `StrategyParameters.hpp` | Dispatcher: routes core to its assigned strategy |

These contain the *interface* + *generic implementations*. Specific
tuning values (e.g. `dip_pct=0.0015`) come from `engine.cfg`, not from
constants embedded here.

## Private tier — `Strategies/private/*.hpp`

Strategies with alpha-flavored implementation details that should NOT
be published. Conditionally included via `__has_include` so the build
stays clean if `private/` is empty or missing.

| File | What it does |
|---|---|
| `EmaCross.hpp` | EMA cross with custom dip/trail/SL adjust logic |

### How the conditional include works

`CoreFrameworks/PortfolioController.hpp`:
```cpp
#if __has_include("../Strategies/private/EmaCross.hpp")
#include "../Strategies/private/EmaCross.hpp"
#endif
```

If `Strategies/private/` is missing (e.g. public release snapshot),
the build still compiles. The strategy just isn't available — engine
falls back to other strategies for the cores that would have used it.

### Public release workflow

When snapshotting for the public repo:
1. `git checkout` a clean copy
2. `rm -rf Strategies/private/`
3. Verify build still passes (`./build.sh test`)
4. Push to public remote

Anyone who wants to add their own private strategy can drop a file
into `Strategies/private/` and reference it from
`StrategyParameters.hpp` dispatcher with a similar conditional pattern.

## Adding a new strategy

See `DOCS/CLAUDE_INTEGRATION.md` ("New strategy" section) for the
5-step integration recipe.
