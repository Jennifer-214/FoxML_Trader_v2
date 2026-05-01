# Recurring bug patterns

The complement to `tests/INVARIANTS_MAP.md`. That doc tracks
**positive** invariants ("X must hold true"); this doc tracks
**negative** patterns ("this class of bug keeps showing up — here's
the detection signature, here's where it bites").

Each pattern has:
- **Class N** identifier — stable, referenced from postmortems
- **Symptom** — what the user sees
- **Root cause** — why it happens
- **Detection** — exact grep / script to find new instances
- **Known instances** — file:line of past occurrences + commit that fixed
- **Prevention** — what to add to readiness/dust skills or to tests

When a new instance is found, add it under "Known instances" with
the fix commit. When a new class emerges (>2 fixes of the same
shape), add a new Class entry.

Read this doc before any architectural sprint, especially anything
that mentions "split", "shard", "decouple", "extract", "centralize",
or "per-core". Run each Class's detection script as a pre-coding
gate.

---

## Class 1 — Strategy lifecycle orphans

**Symptom:** strategy adaptive behavior (regression-driven filter
tightening, trailing SL ratchet, regime-driven retune) silently
absent. Strategies "appear to work" because their entry gate fires,
but everything past entry behaves like a dumb cfg-static strategy.

**Root cause:** sharded port wired the entry point
(`Strategy_BuildParameters` dispatcher) but never plumbed the rest of
the lifecycle (`_Init`, `_Adapt`, `_BuySignal`, `_ExitAdjust`,
`Regime_AdjustPositions`). State structs (`MomentumState` etc.) were
defined but never allocated per-core; legacy callers were the only
ones invoking them.

**Detection:**
```bash
# Find functions called in legacy PortfolioController but not in the
# sharded entry points
tools/calls_graph_diff.sh
```
Functions with zero call sites in `engine_sharded` / `controller_event_loop`
but present in `portfolio_controller` are candidate orphans.

**Known instances:**
- v5.4.0 — all 5 strategies' `_Init`/`_Adapt`/`_ExitAdjust`/`_BuySignal`
  were orphaned. Fixed in commits `ad4fbb7..6049fa5` (Phase 1-2.5).

**Prevention:**
- Readiness skill Check 13 (strategy lifecycle completeness) — load
  before any plan touching strategies.
- `DOCS/STRATEGY_INTERFACE.md` — canonical 5-stage doc.
- `tools/calls_graph_diff.sh` — run as pre-merge gate when sharding
  any subsystem.

---

## Class 2 — Display ↔ execution divergence

**Symptom:** GUI shows a number that has nothing to do with what
will actually trigger an exit. e.g., displays SL=$50000 but the hot
path will fire at SL=$50500 (the ratchet floor). User makes
decisions on stale display data.

**Root cause:** GUI reads a "logical" field (`pos->stop_loss_price`)
that was the source of truth in legacy. Sharded hot path reads a
DIFFERENT field (`core->live_sl + cached_params.ratchet_sl`) for
the same decision. Both fields exist; both compile; both have
plausible-looking writes. Only the hot path's write matters; the
GUI's read is dead.

**Detection:**
```bash
# For each Position field referenced in GUI/, find the hot-path read
grep -rn "pos->stop_loss_price\|pos->take_profit_price" \
    GUI/ DataStream/ CoreFrameworks/
# Then for each, check if hot path (ExecutionCore.hpp / SG_Evaluate)
# reads the same field — if not, it's a divergence
```

**Known instances:**
- v5.4.0 Phase 4 — Positions panel read `pos->stop_loss_price` while
  hot path used `max(live_sl, ratchet_sl)`. Fixed in `b3b77a6`.
- v5.4.1 / v5.4.2 — `snap->fees`, `snap->maker_fills_count`,
  `snap->taker_fills_count`, `snap->total_maker_fees`,
  `snap->total_taker_fees` set in legacy `EngineTUI.hpp` but never
  in sharded `ShardedSnapshot.hpp`. Fixed in `f82d94f` + `7b04ac1`.

**Prevention:**
- Readiness skill Check 12 (display ↔ execution invariant).
- Dust skill Scan 8 (dead-write detection).
- Audit script: `grep -oE "snap->[a-z_]+" EngineTUI.hpp` and
  `grep -oE "snap->[a-z_]+" ShardedSnapshot.hpp`; legacy-only
  fields are candidates.

### Sub-pattern 2c — Display predicate is a strict subset of hot-path predicate

**Symptom:** GUI says "READY" but no fire happens, or shows "off"
with no explanation. Operator looking at the dashboard cannot tell
whether the engine is correctly inactive or silently broken.

**Root cause:** The hot path enforces N conditions for an entry/exit
to fire (e.g. `price_ok & volume_check & ~blocked & permission &
~any_active`). The GUI's "READY" predicate checks fewer than N. Any
condition checked by the hot path but NOT the GUI produces "looks
ready, isn't ready" misleading state.

**Detection:**
```bash
# Inventory hot-path entry predicate terms
grep -A30 "Inlined BG_Evaluate" CoreFrameworks/ExecutionCore.hpp | \
    grep -oE "[a-z_]+_ok|[a-z_]+_check|[a-z_]+_required" | sort -u
# Inventory display predicate terms
grep -A20 "READY\|wait\|in pos" GUI/DashboardPanels.hpp | \
    grep -oE "price_ok|volume_ok|blocked|permission|any_active" | sort -u
# Diff = silent terms.
```

**Known instances:**
- v5.6.0 — Buy Gate top table only checked `price_ok`; ignored
  `BUY_BLOCKED`, `permission`, `volume_required`. Fee-floor BUY_BLOCKED
  (StrategyParameters.hpp:884) silently dropped DIP entries.
- v5.6.0 — `halt_reason = 10` (book-imbalance) was set in the
  controller but `halt_names[]` only had indices 0-9; entire
  imbalance veto was invisible.

**Prevention:**
- v5.6.0 enforces a "predicate ↔ display matrix" in
  `DOCS/EXECUTION_DISPLAY_INVARIANTS.md`. New hot-path predicate
  terms MUST add a corresponding GUI surface in the same PR.
- `controller_test.cpp` predicate-parity test asserts the display
  Status string matches the hot-path mask outcome under each
  isolated condition.
- Single-source rule: numeric thresholds shown in GUI must read the
  SAME variable the controller checks. No display-side recomputation.

---

## Class 3 — Drain count under partials

**Symptom:** Cores beyond `num_cores` under partials silently never
trade. Submit commands stranded in queues forever.

**Root cause:** `OMS_PushSubmit` keys `submit_queues[]` by
`portfolio_slot` (0..2N-1 under partials, where N=num_cores). But
`OMS_DrainSubmit(num_cores=N)` iterates queues 0..N-1. Mismatch.
Confused by the `core_id` parameter name on `OMS_PushSubmit` —
under partials it's actually carrying the portfolio slot, not the
core index.

**Detection:**
```bash
grep -rn "OMS_DrainSubmit\b" CoreFrameworks/ | \
    grep -v "* 2\|partial_exit"
# Any caller passing num_cores without the *2 multiplier under
# partials is a candidate.
```

**Known instances:**
- v5.4.1 — `EngineSharded.hpp:1842` and `ShardedBacktestDriver.hpp:208`
  drained N queues, missed N..2N-1. Fixed in `f82d94f`. Regression
  test at `controller_test.cpp` v5.4.1.B2.

**Prevention:**
- Naming smell: `OMS_PushSubmit`'s `core_id` parameter is misnamed.
  Future refactor should rename to `slot_id` and add a static_assert
  that `submit_queues[]` is sized for `MAX_PORTFOLIO_POSITIONS`,
  not `MAX_EXECUTION_CORES`.

---

## Class 4 — Snapshot save/load asymmetry

**Symptom:** Per-core stats reset on engine restart even though the
file exists and the user expected continuity. Stats panel shows
zero W/L until the next post-restart trade.

**Root cause:** Field added to `CoreContext` in vN.M after the
snapshot save/load was authored. Save was updated, load was forgotten
(or vice versa). The save-only fields silently get truncated on next
load; the load-only fields read garbage from disk past the saved
extent.

**Detection:**
```bash
# Save-side fields
grep -oE "fwrite\(&ctx\.[a-z_]+" ShardedSnapshotPersist.hpp | sort -u
# Load-side fields
grep -oE "fread\(&s\.[a-z_]+" ShardedSnapshotPersist.hpp | sort -u
# Any imbalance is suspect
```

**Known instances:**
- v5.4.3 (this commit) — `core_gross_wins` and `core_gross_losses`
  added in v4.7.25 but never persisted. After restart, Stats panel's
  avg_win / avg_loss / profit_factor / expectancy all read zero
  until next trade.
- v5.4.3 — `idle_cycles` (death-spiral counter) not persisted.

**Prevention:**
- Bump `SHARDED_SNAPSHOT_VERSION` whenever a CoreContext field is
  added that needs persistence.
- Readiness check: when a plan adds a `CoreContext<F>` field, require
  explicit answer to "should this be persisted?" — yes/no/deferred,
  no implicit "no answer."

---

## Class 5 — Reset Paper completeness

**Symptom:** Click "Reset Paper", expect blank slate, but the next
trade exhibits subtle stale behavior — entry blocked by stale
cooldown, adaptive feedback contaminated by pre-reset state, etc.

**Root cause:** Reset handler in `EngineSharded.hpp` zeroes balance,
realized_pnl, and a hand-curated list of per-core fields. New fields
added to CoreContext after the handler was written are silently
NOT zeroed. Reset becomes "mostly fresh" instead of fully fresh.

**Detection:**
```bash
# Compare CoreContext field declarations with what reset zeros
grep -oE "FPN<F>\s+[a-z_]+|uint[0-9]+_t\s+[a-z_]+" \
    CoreFrameworks/ControllerEventLoop.hpp | head -100
# Then find what's reset
grep -A40 "paper_reset_in_progress" CoreFrameworks/EngineSharded.hpp
```

**Known instances:**
- v5.4.3 — `sl_cooldown_remaining` not reset. Post-reset, a core
  with prior SL exit stays zero-gated for sl_cooldown_cycles ticks
  (no UI indicator).
- v5.4.3 — `idle_cycles` not reset. Death-spiral pnl_feeder reset
  threshold not fresh after reset.
- Pre-fix history: v4.7.26 had to add `partner_pending_pnl /
  partner_pending_active / core_gross_wins / core_gross_losses`
  resets after similar issues — recurring class.

**Prevention:**
- Reset handler should iterate via X-macro or struct-zero-clear
  pattern to avoid drift. Adding a field shouldn't require remembering
  to also touch the reset handler.
- Test: after Reset Paper, every CoreContext field should equal its
  Init-time default. Simple property test catches future regressions.

---

## Class 6 — OMS counter persistence

**Symptom:** session-cumulative counters on the OMS (fee totals,
maker/taker breakdown, fill counts) reset to zero on engine restart
even though `balance` and `realized_pnl` continue from the snapshot.
After restart, the GUI's fees tooltip / session forensics drop the
session totals and the user can't reconcile cumulative spend.

**Root cause:** `ShardedSnapshotPersist.hpp` save/load was authored
for the financial-state primitives (balance, realized_pnl, peak,
kill_switch_tripped) and never expanded as the OMS grew counter
fields. Maker/taker / fee-totals were added in Phase 8; never
propagated into the snapshot file.

**Detection:**
```bash
# Fields on OMS struct that look like cumulative counters
grep -E "uint(32|64)_t|FPN<F>" CoreFrameworks/OrderManager.hpp \
    | grep -iE "total|count|fee|fill" | head -20
# What's actually persisted
grep "fwrite(&state->oms->" CoreFrameworks/ShardedSnapshotPersist.hpp
# Diff: counters that exist but aren't written are candidates
```

**Known instances:**
- v5.4.4 — `total_fees`, `total_maker_fees`, `total_taker_fees`,
  `maker_fills_count`, `taker_fills_count` not persisted. Snapshot
  version bumped 5→6.

**Prevention:**
- Same as Class 4: bump SHARDED_SNAPSHOT_VERSION when adding any OMS
  counter that needs continuity, with a save/load symmetry check.
- Future refactor: snapshot save/load should iterate fields from a
  schema struct rather than open-coded fwrite/fread. A schema
  mismatch then becomes a static_assert at compile time.

---

## Class 7 — Threading topology violations (audited clean post-v5.4.x)

**Symptom:** would manifest as data races on per-core fields under
TSan stress. Pre-fixes in v4.7.x already converted shared mutating
state to per-core or atomic. Round 2 audit (2026-04-30) flagged two
candidate violations; both turned out to be false alarms:

1. `EventLoop_QueueParameters` writes `pending_params` from producer
   while per-core thread also writes — flagged. Verification: the
   function is only called by an experiment test, never in the live
   drainer or per-core slow path. False alarm.

2. `OnEvent` writes `ctx->idle_cycles = 0` from drainer while
   per-core thread increments it — flagged. Verification: `OnEvent`
   in mode 1 (default since v4.7.x) early-returns at line 1083
   before reaching the increment. The write is unreachable in
   production. Classified as inert dead code; cleanup can fold it
   into the mode-0 legacy block (low priority).

**Prevention:**
- `./build.sh tsan` clean run on `engine` synthetic mode is the
  durable validation. v5.0.5 confirmed clean; rerun before any
  new threading work.
- Future audits: distinguish "field written by multiple threads" from
  "fields written by multiple threads at the same time" — many
  per-core fields appear to have multiple writers but the writes are
  serialized by mode/cadence/topology gating.

---

## Class 8 — User-configurable features silently inactive in sharded

**Symptom:** user flips a cfg flag, expects behavior change, sees
none. TUI may even display "enabled" status. The cfg field is parsed,
stored, displayed — but the runtime decision path that should consume
it doesn't exist in the sharded code, only in the legacy
PortfolioController.

**Root cause:** the sharded port migrated the structural execution
path (slow-path → strategy → gate parameters → hot path) but did not
port every modulator / gating layer. Cost gating (CostModel) and vol
scaling (VolScaler) were two such layers — fully implemented in
legacy, fully orphaned in sharded.

**Detection:**
```bash
# For each cfg field that's marked "enabled" or has explicit gating
# semantics, check if it's read in the sharded path
for field in $(grep -oE "[a-z_]+_enabled" CoreFrameworks/ControllerConfig.hpp | sort -u); do
    legacy_reads=$(grep -c "config.$field\|cfg.$field" CoreFrameworks/PortfolioController.hpp 2>/dev/null)
    sharded_reads=$(grep -rh "config.$field\|cfg.$field" \
        CoreFrameworks/EngineSharded.hpp \
        CoreFrameworks/ControllerEventLoop.hpp \
        Strategies/ 2>/dev/null | wc -l)
    if [ $legacy_reads -gt 0 ] && [ $sharded_reads -eq 0 ]; then
        echo "ORPHAN: $field (legacy=$legacy_reads, sharded=0)"
    fi
done
```

**Known instances:**
- v5.4.4 (DOCUMENTED, NOT YET FIXED) —
  - `cost_gate_enabled`: legacy reads at PortfolioController.hpp:1751.
    Sharded zero reads. CostModel evaluates expected cost vs
    expected gain at entry; if `cost > k × gain`, vetoes the entry.
    Sharded skip means cost-aware entry filtering is dead.
  - `foxml_vol_scaling_enabled`: legacy reads at
    PortfolioController.hpp:1168, 1789. Sharded zero reads. Scales
    risk_pct by recent volatility (cuts size in high-vol regimes).
    Sharded skip means user's risk_pct is constant regardless of
    volatility.

**Prevention:**
- Readiness skill check: when a plan touches an `*_enabled` cfg
  field, require explicit "where is this consumed" answer for both
  legacy AND sharded paths. Block ship if sharded path is empty.
- Dust scan: extend Scan 9 (orphaned function detection) to also
  scan for orphaned cfg-enabled fields.
- Long-term fix: port CostModel + VolScaler integration into the
  sharded `Strategy_BuildParameters` dispatcher path. Tracked as a
  v5.5+ feature ship.

---

## Class 9 — Shutdown blocking on operations the user didn't want

**Symptom:** Ctrl+C / window-close hangs the terminal for tens of
seconds (or indefinitely). User can't tell if the engine is dead or
working. Process appears stuck.

**Root cause:** A "graceful" cleanup step on the shutdown path is
trying to do something the user didn't ask for — flatten positions
to zero, close exchange orders, drain queues to empty — and is
blocking the join sequence waiting for that work to complete.

**Detection:**
```bash
# Functions called between SIGINT delivery and pthread_join in the
# shutdown sequence
grep -n -A2 "shutdown requested\|joining threads" CoreFrameworks/EngineSharded.hpp
# Anything between the signal-flag check and the first thread join
# is a candidate hang point.
```

**Known instances:**
- v5.4.5 — `EngineSharded_ForceCloseOnShutdown` blocked the join
  sequence for up to 30s while submitting market SELLs and waiting
  for fills. User intent was "positions persist across restart"
  (engine runs 24/7), not "flatten on exit." Fixed: replaced the
  force-close call with a single warning log; positions persist via
  snapshot. Force-close logic preserved in codebase for callers that
  explicitly want it.

**Prevention:**
- Shutdown path sequence should be: (1) save state, (2) join threads,
  (3) close files. Do NOT introduce blocking work between (1) and (2)
  without an explicit cfg gate. If you add a "graceful X" step, give
  it a `cfg.X_on_shutdown` toggle defaulting to off.
- Test: shutdown with N open positions completes within S seconds
  (S < 5). Property test catches future regressions.

---

## Class 10 — Strategy-regime mismatch

**Symptom:** A strategy fires entries in regimes where its
contract doesn't make sense (e.g. MOM buying breakouts in RANGING
markets where every breakout reverts), accumulating fee-only losing
trades.

**Root causes (any of):**
1. Hardcoded strategy assignment (`core_N_strategy=momentum`) —
   strategy fires regardless of regime classification.
2. Regime hysteresis flicker — classifier briefly flips to TRENDING
   during noise, AUTO core enters MOM, classifier returns to
   RANGING, position sits at fees-only loss.
3. Classifier-threshold mis-tuning — classifier decides TRENDING in
   actually-ranging markets.
4. Strategy filter too loose — strategy's BuySignal accepts
   marginal entries that can't survive fees + slippage.

**Detection:**
```bash
# Per-strategy x per-regime quality breakdown from health.jsonl
jq -s 'group_by(.cat=="entry") | .[] | select(.[0].cat=="entry") |
  group_by(.msg | capture("strat=(?<s>[0-9]+) regime=(?<r>[0-9]+)").s + ":" + .r) |
  map({key: .[0].msg, count: length})' health.jsonl
# Look for cells where MOM has many entries in regime=0 (RANGING)
# with negative net bps in the matching exits.

# Or use the v5.7.6 GUI Strategy Quality panel — same data via Refresh.
```

**Known instances:**
- v5.7.0 — 2026-04-30 paper run: Core 0 hardcoded MOM entered in
  RANGING regimes, took 16+ near-flat trades that lost fees only.
  Audit (`DOCS/changelogs/2026-04-30-regime-classifier-audit.md`)
  confirmed the regime classifier itself was healthy — Core 0's
  hardcoded assignment bypassed regime gating entirely.

**Prevention:**
- v5.7.2 — boot guard refuses live mode with hardcoded strategies
  unless `acknowledge_hardcoded_strategy_in_live=1` is set explicitly.
  Paper mode warns. Boot abort path emits health log
  `cat="engine"` `boot_abort` line.
- v5.7.5 — MOM-specific quality filters
  (`momentum_min_tp_margin_pct`, `momentum_min_r2`,
  `momentum_min_buy_delta_recent`) gated cfg-side. Default off
  preserves pre-v5.7 behavior; operator opts in after observing
  v5.7.6 quality dashboard data.
- v5.7.6 — per-strategy quality dashboard panel surfaces the
  pattern at-a-glance: any strategy showing many entries in a
  "wrong" regime with negative net bps is the smoking gun.

**Adjacent**: see `DOCS/STRATEGY_REFACTOR_IDEAS.md` for the longer-
term observation that adding MORE strategies will increase the
chance of strategy-regime miscalibration. The X-macro refactor
proposed there would NOT fix this class — it just makes new
strategies easier to add. Class-10 prevention is regime-gating +
filters + observability, all already in place post-v5.7.
