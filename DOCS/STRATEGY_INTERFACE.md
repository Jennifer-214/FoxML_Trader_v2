# Strategy Interface Contract

How strategies plug into the engine. Every strategy must implement
this five-stage lifecycle (or explicitly mark stages "skipped — reason"
with documented justification). Architectures (legacy single_core,
sharded centralized, sharded per_core_slow, backtest) must call all
five stages in the right order, or explicitly mark stages skipped.

This contract was missing pre-v5.4 and the resulting drift caused the
v4.x sharded port to silently orphan four of the five stages across all
five strategies. See `DOCS/v5.4-regression-postmortem.md` for the
incident details and `plans/2026-04-29-strategy-restoration-master.md`
for the restoration plan.

## The five stages

```
                       per-engine               per-cadence
   ┌─────────┐ once    ┌─────────┐ N×           ┌──────────────────┐
   │  Init   │────────▶│ allocate│─────────────▶│      Adapt       │
   └─────────┘ at boot │  state  │              │  (read market,   │
                       └─────────┘              │   update state)  │
                                                └────────┬─────────┘
                                                         │
                                                         ▼
                                                ┌──────────────────┐
                                                │ BuildParameters  │
                                                │ (state→gate)     │
                                                └────────┬─────────┘
                                                         │
                                                         ▼ open positions
                                                ┌──────────────────┐
                                                │   ExitAdjust     │
                                                │ (state→ratchet)  │
                                                └──────────────────┘
                                                         ▲
                                                         │ on regime change
                                                ┌──────────────────┐
                                                │  RegimeAdjust    │
                                                │ (regime-aware    │
                                                │  retune)         │
                                                └──────────────────┘
```

### 1. `Strategy_Init`

**Cadence:** once per engine, at boot. Also called when an operator
hot-swaps the strategy on a running engine.

**Purpose:** allocate per-strategy state, set initial parameters from
cfg + initial market snapshot.

**Signature shape:**
```cpp
inline void Momentum_Init(MomentumState<F>* state,
                           const RollingStats<F>* rolling,
                           const ControllerConfig<F>* cfg);
```

**Architecture responsibility:**
- Each architecture must call `Strategy_Init` for every core that has
  a non-NONE strategy assigned, BEFORE the first `Adapt` or
  `BuildParameters` call.
- The state struct lives wherever the architecture stores per-engine
  data (PortfolioController for legacy; CoreContext for sharded).

**Skipping is allowed if:** the strategy has no state. SimpleDip is the
only one that's effectively stateless (its `_Adapt` is a no-op by
design — see "skipped stages" below).

### 2. `Strategy_Adapt`

**Cadence:** every slow-path cycle (every ~256 ticks by default; per-core
override via `core_N_poll_interval`).

**Purpose:** update strategy state from market signals (RollingStats,
regime, recent P&L). The strategy's "memory" — without it, the strategy
becomes a stateless function of the latest market snapshot.

**Signature shape:**
```cpp
inline void Momentum_Adapt(MomentumState<F>* state,
                            FPN<F> current_price,
                            FPN<F> portfolio_delta,
                            const RollingStats<F>* rolling,
                            const ControllerConfig<F>* cfg);
```

**Architecture responsibility:** call once per slow-path cadence per
core, AFTER updating RollingStats but BEFORE `BuildParameters`.

**Common state updates:**
- Adaptive threshold mults (e.g., `live_breakout_mult`, `live_offset_pct`)
- Recent-performance tracking (P&L regression, win/loss feedback)
- Trend confirmation counters (e.g., consecutive cycles above threshold)
- Idle-cycle squeeze (gates relaxing during low-activity windows)

**Skipping is allowed if:** the strategy is genuinely stateless. SimpleDip
intentionally has a no-op `_Adapt` (its only state, `recent_high`, is
read-only from rolling stats; nothing to update).

### 3. `Strategy_BuildParameters`

**Cadence:** every slow-path cycle, after `Adapt`.

**Purpose:** read state + market snapshot, emit `GateParameters` for the
hot path to consume via seqlock.

**Signature shape:**
```cpp
template <unsigned F, unsigned W = 128>
inline void Momentum_BuildParameters(
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out,
    const MomentumState<F>* state /* v5.4+: takes state */);
```

**Output fields populated:**
- `bg_price_threshold` — buy-gate price threshold (entry trigger)
- `bg_volume_threshold` — buy-gate volume requirement
- `sg_take_profit_price` — initial take-profit for new positions
- `sg_stop_loss_price` — initial stop-loss for new positions
- `tp_pct`, `sl_pct` — fallback rates (for per-fill adjustments)
- `trade_size` — position sizing (typically allocated_balance / entry_price)
- `flags` — `GATE_FLAG_BUY_ABOVE` (momentum) / `GATE_FLAG_BUY_BELOW` (MR/dip)
  / `GATE_FLAG_TP_ENABLED` / `GATE_FLAG_SL_ENABLED` /
  `GATE_FLAG_BUY_BLOCKED` (fee-floor or insufficient signal)
- `strategy_id` — STRATEGY_X enum (used by GUI display)

**Architecture responsibility:** call once per slow-path cadence per core,
AFTER `Adapt`, then push the result to ExecutionCore via seqlock
(`ExecutionCore_SetParameters`).

**Skipping is NOT allowed.** Every strategy must implement
`_BuildParameters`. This is the entry-time output; without it, the hot
path has no gate to evaluate.

### 4. `Strategy_ExitAdjust`

**Cadence:** every slow-path cycle, when there's at least one open position
on the core. Skipped when `active_bitmap & core_mask == 0`.

**Purpose:** trail SL/TP, enforce reward/risk floors, lock in profit on
strong moves.

**Signature shape:**
```cpp
inline void Momentum_ExitAdjust(Portfolio<F>* portfolio,
                                  FPN<F> current_price,
                                  const RollingStats<F>* rolling,
                                  const ControllerConfig<F>* cfg,
                                  /* v5.4+: */ pending_params_writer<F>* w);
```

**Output channel — IMPORTANT:**
- Strategy `_ExitAdjust` MUST write to `pending_params.ratchet_sl` (and
  in v5.4+, `pending_params.ratchet_tp` for trailing TP) — these reach
  the hot path via seqlock.
- Strategy `_ExitAdjust` MUST NOT write to `pos->stop_loss_price` or
  `pos->take_profit_price` — those fields are GUI-cosmetic. The hot
  path reads `core->live_sl + cached_params.ratchet_sl` for execution
  (see `ExecutionCore.hpp:268+322`). Direct writes to `pos->...` are
  no-op for execution.
- Pre-v5.4 implementations wrote `pos->...` and were silently dead.
  Phase 2 of v5.4.0 rewrote them.

**Fee-floor cap:** all ratchet writes go through a shared helper
(`Strategy_WriteRatchetSL` or similar) that applies the v5.1.7 fee-floor
cap (`ratchet_sl ≤ entry × (1 - 3 × fee_rate_taker)`). Strategies don't
need to enforce this themselves; the helper does it.

**Skipping is allowed if:** strategy has no trailing logic. SimpleDip
skips this (fixed TP/SL only).

### 5. `Strategy_RegimeAdjust` (a.k.a. `Regime_AdjustPositions`)

**Cadence:** when the regime classifier transitions to a new regime, for
each core with open positions.

**Purpose:** retune open positions' TP/SL for the new regime's risk
profile. E.g., entering TRENDING widens TP; entering VOLATILE tightens
SL.

**Signature shape:**
```cpp
inline void Regime_AdjustPositions(Portfolio<F>* portfolio,
                                     int slot,
                                     int old_regime, int new_regime,
                                     const RollingStats<F>* rolling,
                                     const ControllerConfig<F>* cfg,
                                     /* v5.4+: */ pending_params_writer<F>* w);
```

**Output channel:** writes to `pending_params.ratchet_sl` /
`pending_params.ratchet_tp`, NOT direct `pos->...` fields. Same rule as
`ExitAdjust`.

**Architecture responsibility:** detect regime transitions per core
(compare `current_regime` to a locally-cached `last_seen_regime`), call
`RegimeAdjust` on transition.

**Skipping is allowed if:** the architecture doesn't expose regime to
strategies. Centralized regime (v5.4 Phase 3.2) makes this stage
mandatory for AUTO-routed cores.

## Cross-architecture naming map

The v4.x sharded port renamed/subsumed some legacy functions. The
naming drift contributed to the orphaning — easy to assume
"_BuildParameters subsumes everything _Adapt + _BuySignal did" when
really it only subsumes `_BuySignal`. This map makes the
correspondence explicit:

| Legacy (PortfolioController) | Sharded (StrategyParameters dispatcher) | Note |
|---|---|---|
| `Strategy_Init`        | `Strategy_InitPerCore` (Phase 1.2 dispatcher) | same per-strategy `_Init`; the dispatcher routes by `strategy_id` |
| `Strategy_Adapt`       | called from `EventLoop_RebuildOneCore` (Phase 2 wiring) | same function, different call site |
| `Strategy_BuySignal`   | **subsumed by** `Strategy_BuildParameters` | conceptually similar; emits gate via different output type (`BuySideGateConditions` legacy vs `GateParameters` sharded) |
| `Strategy_BuildParameters` | (new in sharded; absent in legacy) | sharded-only — emits `GateParameters` directly via seqlock |
| `Strategy_ExitAdjust`  | called from `EventLoop_RebuildOneCore` (Phase 2 wiring), output channel **changed** to `pending_params.ratchet_sl` | same function name, different output semantics — sharded version writes to ratchet, NOT `pos->stop_loss_price` |
| `Regime_AdjustPositions` | called on regime transition (Phase 3.1 wiring), output channel changed to `pending_params.ratchet_sl/tp` | same |

**Lesson:** when porting code across architectures, document the
correspondence at the time of the port. "We renamed X to Y" + "Z now
subsumes W" needs to live in this kind of map, not just in the commit
message. Otherwise the next person reads only the new architecture and
assumes the legacy functions were obsolete (silently orphaning them).

## Architecture compliance matrix

| Stage | Legacy single_core (PortfolioController) | Sharded centralized | Sharded per_core_slow (default) | Backtest (ShardedBacktestDriver) |
|---|---|---|---|---|
| Init | ✅ | ⚠️ wired in v5.4.0 Phase 1 | ⚠️ wired in v5.4.0 Phase 1 | ⚠️ wired in v5.4.0 Phase 1 |
| Adapt | ✅ | ⚠️ wired in v5.4.0 Phase 2 | ⚠️ wired in v5.4.0 Phase 2 | ⚠️ wired in v5.4.0 Phase 2 |
| BuildParameters | ✅ | ✅ | ✅ | ✅ |
| ExitAdjust | ✅ (writes pos->...) | ⚠️ rewritten v5.4.0 Phase 2 (writes ratchet) | ⚠️ rewritten v5.4.0 Phase 2 (writes ratchet) | ⚠️ rewritten v5.4.0 Phase 2 (writes ratchet) |
| RegimeAdjust | ✅ | ⚠️ wired v5.4.0 Phase 3 | ⚠️ wired v5.4.0 Phase 3 | ⚠️ wired v5.4.0 Phase 3 |

⚠️ = pending v5.4.0 ship. Pre-v5.4 status = ❌ orphaned.

## Hot path field source of truth

| Field hot path uses for execution | Where it's written |
|---|---|
| `core->live_sl` (set on entry) | ExecutionCore_Tick lines 423-425 (entry-time only) |
| `core->live_tp` (set on entry) | ExecutionCore_Tick lines 417-419 (entry-time only) |
| `core->cached_params.ratchet_sl` (post-entry trail) | Slow-path: `pending_params.ratchet_sl` → seqlock push → cached_params |
| `core->cached_params.ratchet_tp` (v5.4+) | Slow-path: `pending_params.ratchet_tp` → seqlock push → cached_params |
| `core->cached_params.bg_price_threshold` (entry trigger) | `Strategy_BuildParameters` output, seqlock pushed |
| `core->cached_params.flags` (BUY_ABOVE/BELOW, BLOCKED, etc.) | `Strategy_BuildParameters` output, seqlock pushed |

| Field that is GUI-cosmetic only (display) | Hot path does NOT read for execution |
|---|---|
| `pos->stop_loss_price` | Strategy `_ExitAdjust` legacy writes (orphaned in sharded pre-v5.4) |
| `pos->take_profit_price` | Strategy `_ExitAdjust` legacy writes (orphaned in sharded pre-v5.4) |
| `pos->original_tp` / `pos->original_sl` | Snapshots of entry-time gate values; used by trailing logic to compute "above original_tp" trigger |

**Invariant:** code that intends to affect an open position's TP/SL at
execution-time MUST write to `pending_params.ratchet_sl` or
`pending_params.ratchet_tp`, not `pos->...`.

## Hot path discipline

**Hot path** = `ExecutionCore_Tick`, `BG_Evaluate`, `SG_Evaluate`. These
run per-tick at single-digit ns per call. They MUST NOT:
- Allocate
- Acquire any lock except the seqlock retry on `cached_params`
- Read fields outside the cached set + position snapshot
- Branch on configuration that isn't pre-resolved into flags

**Slow path** = `EventLoop_RebuildOneCore` and friends. These run per-cadence
(~256 ticks) per core. Cost budget is microseconds. Strategy lifecycle
stages 2-5 all run on the slow path.

## Observability via Health log

Each lifecycle stage should emit at least one `Health_Log` per call when
enabled (cfg `health_log_path` set). Recommended categories:

| Stage | Health log category | Level | Per call |
|---|---|---|---|
| Init | `strategy_init` | INFO | once at boot |
| Adapt | `strategy_adapt` | DEBUG | per cadence (rate-limit if too chatty) |
| BuildParameters | `strategy_build` | DEBUG | per cadence |
| ExitAdjust | `strategy_exit_adjust` | INFO | per call (rare — only when positions open) |
| RegimeAdjust | `regime_adjust` | INFO | per regime transition |

Plus the standalone `regime` category that captures regime classifier
inputs/outputs per cadence.

## Filter recipes (jq)

```bash
# All regime transitions for core 0
jq 'select(.cat == "regime" and .core == 0)' health.jsonl

# All ExitAdjust calls (rare events)
jq 'select(.cat == "strategy_exit_adjust")' health.jsonl

# Engine-wide events only (no per-core noise)
jq 'select(.core == -1)' health.jsonl

# Last 100 events of any kind
tail -100 health.jsonl | jq .

# Count regime transitions per core in the last hour
jq -r 'select(.cat == "regime") | .core' health.jsonl | sort | uniq -c

# When did core 0 last transition off RANGING?
jq 'select(.cat == "regime" and .core == 0 and (.msg | contains("new=0") | not))' health.jsonl | tail -1
```

## How to add a new strategy

### Post-v5.8.0 (after FOREACH_STRATEGY X-macro registry ships)

```
1. cp DOCS/STRATEGY_TEMPLATE.hpp Strategies/<Name>.hpp
   (or Strategies/private/<Name>.hpp for alpha-flavored)

2. Replace <Name> token everywhere in the new file. Implement the
   four lifecycle functions (Init / Adapt / BuildParameters /
   ExitAdjustSharded). Match the canonical signatures (see
   DOCS/EASY_ADDITIONS_INVARIANTS.md).

3. Append one row to FOREACH_STRATEGY(X) in StrategyInterface.hpp:

     X(<NAME>, "<short>", "<full>", <Name>State, \
       <Name>_Init, <Name>_BuildParameters, \
       <Name>_Adapt, <Name>_ExitAdjustSharded)

4. Append a strategy color to strat_colors[NUM_STRATEGIES] in
   GUI/DashboardPanels.hpp (this is the only manual edit beyond the
   X-macro line).

5. Run: ./build.sh test
   - Compile errors → typo in step 3 (function name doesn't match
     the actual definition). Fix.
   - Tests should pass; the X-macro auto-generates dispatch tables
     so no manual wiring is needed.

6. Run: ./tools/calls_graph_diff.sh
   - Output should be CLEAN. If new orphans appear, your
     X-macro line references a function name that doesn't exist OR
     a stage isn't being dispatched.

7. Add tests for the new strategy's behavior in
   tests/controller_test.cpp under the // === EXTENSIBILITY ===
   group (or strategy-specific group).

DONE. ~3 sites total: file + X-macro line + GUI color.
```

### Pre-v5.8.0 (current state — before X-macro registry ships)

1. Define `<Strategy>State<F>` struct in `Strategies/<Strategy>.hpp`
2. Implement all five lifecycle stages (or document skips)
3. Add dispatcher entry in `Strategies/StrategyParameters.hpp`
4. Add `STRATEGY_<NAME>` enum value
5. Add per-core wiring in `Strategy_InitPerCore` / `_FreePerCore`
6. Add tests covering each stage (Group A in `DOCS/v5.4-test-inventory.md`)
7. Verify all four architectures (legacy, centralized, per_core_slow, backtest)
   call all five stages — run `tools/calls_graph_diff.sh` to confirm no
   stage is orphaned

(8 sites currently. v5.8.0 reduces this to 3 via the X-macro registry.)

## Canonical signatures (audited 2026-05-01)

For the X-macro to write a uniform function pointer table, every
strategy's lifecycle functions must conform to canonical signatures.
Drift = compile-time failure when X-macro tries to assign mismatched
types to a `void (*)(...)` declared with the canonical type.

**Audit results (2026-05-01):**

- `_Init` — uniform across all 5 strategies ✅
- `_Adapt` — drift in MLStrategy (takes `const void* cfg` for include-cycle workaround). v5.8.0 must add an adapter wrapper `MLStrategy_Adapt_Canonical` for the X-macro to point at. Real function preserved for legacy callers.
- `_BuildParameters` — uniform for SimpleDip/MeanReversion/Momentum/EmaCross. ML_BuildParameters takes additional `rolling_long` parameter. v5.8.0 either uses case-block dispatch (preserves wider signature) or wraps ML in an adapter. Case-block is simpler.
- `_ExitAdjustSharded` — uniform across all 5 ✅

## Naming consistency rule (readiness skill enforcement)

When a function exists in multiple architectures (legacy + sharded +
backtest) and does the same job, **it MUST have the same name across
all of them**. If the architectures need genuinely different functions,
the cross-architecture naming map (above) MUST document the
correspondence.

This rule prevents two failure modes:

1. **Silent duplication.** Two functions doing the same job under
   different names = code that ages independently → drift → bugs that
   only surface in one architecture.
2. **Silent orphaning.** A function gets renamed in one architecture
   but the old name is left behind in the others. Calls to the old
   name silently become no-ops (the v4.x port pattern that orphaned
   `_Adapt`/`_BuySignal`/`_ExitAdjust`).

**The readiness skill MUST flag** (Phase 6.1):
- Plans that introduce a function with a name pattern matching an
  existing one in a different architecture without explicit cross-arch
  naming-map update
- Plans that rename a function in one architecture without renaming
  it in all architectures that use the same logical operation
- Plans that "add" a function whose purpose duplicates an existing
  function in another architecture (suggest reusing instead)

**Concrete enforcement test** (Phase 6.4): for each strategy, verify
that all per-architecture call paths use the same function name for
the same lifecycle stage. E.g., legacy calls `Momentum_Adapt`, sharded
also calls `Momentum_Adapt` (after Phase 2 wiring). If the names
diverged, the test fails.

## How NOT to add a strategy

Don't write a `_BuildParameters` and call it done. The
`Init`/`Adapt`/`ExitAdjust`/`RegimeAdjust` stages exist for a reason.
If you're confident a stage isn't needed, mark it explicitly:

```cpp
// SimpleDip has no _ExitAdjust — fixed TP/SL by design.
// Documented as SKIPPED in DOCS/STRATEGY_INTERFACE.md compliance matrix.
```

The readiness skill (post-Phase 6) will flag missing-without-marker as
a regression.
