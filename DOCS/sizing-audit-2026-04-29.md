# Sizing Audit — 2026-04-29 (Strategy P4)

Walked the per-trade notional path to verify the $750 leg-notional
observed in the v5.1.2 overnight soak isn't an under-sizing bug.

## The math

```
starting_balance = $10000          (engine.cfg)
risk_pct         = 60%             (engine.cfg)
num_cores        = 4
partial_exit_pct = 0.50            (engine.cfg, 50/50 split)
fee_rate_taker   = 0.10%           (engine.cfg)
```

Per-core allocation (from `EngineSharded.hpp` startup, mirrored on hot
reload at line 1119):

```
default_per_core = total_balance × default_risk / num_cores
                 = $10000 × 0.60 / 4
                 = $1500 per core
```

Per-tick at entry $75709, strategy emits:

```
intended_qty = allocated_balance / expected_entry
             = $1500 / $75709
             ≈ 0.01981 BTC
```

Partial split (`Strategy_BuildParameters` post-cap → drainer maps event):

```
leg A qty    = 0.01981 × 0.50 = 0.00990 BTC → $749.83 notional
leg B qty    = 0.01981 × 0.50 = 0.00990 BTC → $749.83 notional
```

Trade log shows `trade_size=0.00981778`, $743 notional — matches ✓ (small
rounding from the actual fill price).

## Fee-budget check

For a $750 leg at 0.1% taker fee × 2 sides:

```
fees per leg per round-trip = $750 × 0.001 × 2 = $1.50
breakeven gross requires    = $1.50 (gross_profit ≥ fees)
breakeven move %            = $1.50 / $750 = 0.20% gross
```

So a winning trade needs **at least 0.20% gross move** before fees just
to break even. With cfg `core_1_take_profit_pct=1.00` (1% TP), the
configured target is well above the floor. But if an exit fires earlier
(at +0.1% via trailing-SL ratchet, which is what we observed), the
trade is guaranteed-net-negative.

## Verdict: sizing is correct, exit logic was the bleed

The $750 notional is the math the cfg specified. **No undersizing
bug** — flipping `risk_pct=60%` to a higher value would just amplify
both wins and losses proportionally, not change the +0.1% bleed.

The bleed root cause was the trailing-SL ratchet firing at +0.1%
gain (well below the configured 1% TP), guaranteeing net-negative
after round-trip fees. **Fixed in v5.1.7** with fee-floor guards on
`EventLoop_TrailingSLRatchetOneCore` + `EmaCross_ExitAdjust` that cap
ratchet_sl at `entry × (1 − 3 × fee_rate_taker)`.

## What's NOT broken (don't fix)

- Per-core risk distribution: `total × risk_pct / num_cores` is the
  right baseline; per-core override via `core_N_risk_pct` works as
  documented
- Partial split: `partial_exit_pct=0.5` produces 50/50 legs as expected
- `OMS_PushSubmit` quantity propagation: trade log size matches
  computed intended_qty within rounding

## Optional future enhancement (deferred)

`core_N_min_notional` cfg would refuse entries below a per-core minimum
notional, defending against accidental risk_pct → 0 misconfig. Trivial
to add when the use case shows up. Not blocking; not done today.

## Companion docs

- v5.1.7 changelog — trailing-SL fee-floor guards (the actual fix)
- v5.1.10 — runtime BUY_BLOCKED gate (catches dynamic-TP-collapse case)
- `DOCS/CLAUDE_INVARIANTS.md` — Position Exit Invariants section
