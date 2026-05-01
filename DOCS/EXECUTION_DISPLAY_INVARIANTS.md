# Execution / Display Invariants

**Read this file before adding any new term to the hot-path entry or
exit predicates, or any new GUI surface that claims to show "why a
trade did or didn't fire."**

The invariants here protect against the bug class observed on
2026-04-30: GUI showed "READY" while the hot path was silently
refusing to fire (e.g. `GATE_FLAG_BUY_BLOCKED` set due to fee-floor).
The display layer's truth model was a strict subset of the hot
path's. After v5.6 these invariants are enforced; before re-introducing
any of them, check this doc.

## The cardinal invariant

**Every term in the hot-path entry / exit predicate MUST have a
corresponding GUI surface.** "I added a new condition to BG_Evaluate
but didn't update the GUI" is a regression even if all tests pass —
operators looking at the dashboard will be misled about what the
engine is doing.

The hot-path predicates today (`ExecutionCore.hpp:280-360`):

```cpp
// Entry gate
bg_fires  = price_ok & volume_check & ~blocked_mask
can_enter = ~any_active & permission & bg_fires

// Exit gate (per leg)
sg_fires_a = (tp_enabled & tp_hit_a) | (sl_enabled & sl_hit_a)
can_exit_a = active & sg_fires_a
// (sg_fires_b similar, branch-gated on active_b)
```

Every term on the right-hand side appears in this table. If you add a
new term, you MUST add a row to this table and a corresponding GUI
surface in the same PR. **The presence of an unmatched term is
sufficient grounds to reject a PR.**

## Predicate ↔ display matrix

| Term | Source field | PerCoreSnap field | GUI surface | Status |
|---|---|---|---|---|
| `price_ok` (BG) | `cached_params.bg_price_threshold` + `flags & BUY_ABOVE` | `buy_gate_price`, `gate_direction` | Buy Gate top table (Status column) | ✅ |
| `volume_ok` (BG) | `cached_params.bg_volume_threshold` | `bg_volume_threshold_display` (v5.6.1) | Buy Gate collapsing header (v5.6.1) | ⚠ to-add |
| `volume_required` (BG) | `cached_params.flags & GATE_FLAG_VOLUME_REQUIRED` | `gate_flags` (v5.6.0) | Buy Gate Status (decoded from gate_flags) | ⚠ to-add |
| `~blocked_mask` (BG) | `cached_params.flags & GATE_FLAG_BUY_BLOCKED` | `gate_flags` (v5.6.0) | Buy Gate Status: "blocked" | ⚠ to-add (v5.6.0) |
| `permission` (entry) | `core->permission` (atomic) | `permission` (v5.6.1) | Buy Gate Status: "PERM_OFF" badge (v5.6.1) | ⚠ to-add |
| `~any_active` (entry) | `core->execution_active \| active_b` | `positions[i].idx` | Buy Gate Status: "in pos" + Positions panel | ✅ |
| `tp_enabled` (SG) | `cached_params.flags & GATE_FLAG_TP_ENABLED` | `gate_flags` (v5.6.0) | Positions: ✓/✗ TP indicator (v5.6.5) | ⚠ to-add |
| `sl_enabled` (SG) | `cached_params.flags & GATE_FLAG_SL_ENABLED` | `gate_flags` (v5.6.0) | Positions: ✓/✗ SL indicator (v5.6.5) | ⚠ to-add |
| `effective_tp` (SG, includes ratchet) | `FPN_Max(live_tp, ratchet_tp)` | `effective_tp_display` (v5.6.5) | Positions panel | ⚠ to-add (v5.6.5) |
| `effective_sl` (SG, includes ratchet) | `FPN_Max(live_sl, ratchet_sl)` | `effective_sl_display` (v5.6.5) | Positions panel | ⚠ to-add (v5.6.5) |
| Leg B variants | `live_tp_b`, `live_sl_b`, `active_b` | partials snapshot | Positions panel A/B distinction (v5.6.5) | ⚠ to-add |

Status legend: ✅ = exists pre-v5.6; ⚠ = added in v5.6.x as noted.

## Halt-reason taxonomy

When the slow path zero-gates a core (sets `bg_price_threshold = 0`
and/or `flags |= BUY_BLOCKED`), it MUST set a halt-reason byte on
`CoreContext` for GUI display. There are TWO halt-reason channels —
keep them distinct:

### Controller halt_reason (`CoreContext::halt_reason`)

Set by the controller's post-`Strategy_BuildParameters` checks
(spacing, vwap, long-slope, vol-delta, min-stddev, sl-cooldown,
warmup, core-budget, core-kill, book-imbalance). First-reason-wins.

Names array in `GUI/DashboardPanels.hpp` MUST match the codes in
`CoreFrameworks/ControllerEventLoop.hpp:1812-1814`. The bound check
in the renderer MUST equal `sizeof(halt_names) / sizeof(halt_names[0])`.

```cpp
// Codes 0..10 — must match across files
0 = "ok"
1 = "spacing"
2 = "vwap"
3 = "long-slope"
4 = "vol-delta"
5 = "min-stddev"
6 = "sl-cooldown"
7 = "warmup"
8 = "core-budget"
9 = "core-kill"
10 = "imbalance"  // added in v5.6.0; pre-v5.6 was set but invisible
```

Adding a new code: increment in BOTH places. Both files have a
header comment listing the codes — keep them in sync. A test in
`controller_test.cpp` asserts the array sizes match.

### Strategy halt_reason (`CoreContext::strategy_halt_reason`, v5.6.2)

Set by the strategy's `_BuildParameters` BEFORE calling `Gate_Zero`
or setting `BUY_BLOCKED`. Distinguishes strategy-internal vetoes
(no uptrend, no mean-reversion signal, fee-floor margin too tight,
ML below threshold) from controller-level halts.

Codes defined in `Strategies/StrategyInterface.hpp` (`SHALT_*`
prefix). Names array `shalt_names[]` in DashboardPanels — same
sync rule as halt_names.

The two channels are display-priority-ordered:
1. `halt_reason > 0` wins display
2. else `strategy_halt_reason > 0` wins
3. else `gate_flags & BUY_BLOCKED` (no specific reason logged)
4. else "no signal" (strategy zeroed without setting either reason —
   should not happen post-v5.6.2; flag as drift)

## Single-source rule

When a panel displays a numeric pair like `tp_pct=0.13% / floor=0.15%`,
both numbers MUST come from the SAME variable the controller's gate
check reads. Display code MUST NOT recompute thresholds.

Violations of this rule cause display↔execution drift even within
v5.6: the controller checks one formula, the panel renders another,
they diverge over time as the formula changes in one place but not
the other. The fix is harder than it sounds because the controller
often computes thresholds as temp locals that aren't exposed for
display. When a numeric is displayed:

1. If the variable already exists as a slow-path output (in
   `pending_params` / `cached_params` / per-core state): use it
   directly. ✅
2. If it's a temp local in the controller: refactor to expose as a
   slow-path output field. New helper convention:
   `Gate_<Name>Diagnostic` returns a `{passed, actual, threshold}`
   triple; controller uses `passed`, display reads `actual` +
   `threshold`. ✅
3. If display recomputes the formula independently: ❌ rejected,
   that's a drift seed.

## Snapshot truthfulness

`TUISnapshot` is RAM-only, frame-cadence, double-buffered
(`snapshots[2]` at `EngineTUI.hpp:1003`). Slow path writes to
`pending_params` → `ExecutionCore_SetParameters` → seqlock-published
to `cached_params` → hot path reads under `ParameterSlot_Read`.

The TUI snapshot copy MUST read `cached_params` via `ParameterSlot_Read`
(or be safely-equivalent under x86 memory ordering) so the snapshot
sees the same struct the hot path will see on its next tick. Reading
individual fields out of a half-published struct is a torn-read bug —
the snapshot displays a frankenstein state that no real tick observed.

## Health log gate transitions

`cat="gate"` entries in `health.jsonl` MUST be edge-triggered only.
Steady-state per-cycle logging would fill disk during volatile
periods. The slow path keeps a packed `prev_gate_state` byte per
core; emit only when the packed state changes.

Locale: emit numerics under `LC_NUMERIC=C` per the existing
`RunHistory` pattern. Comma-decimal locales corrupt JSON.

## Adding new terms — checklist

When adding a new condition to BG_Evaluate or SG_Evaluate:

1. Add to predicate ↔ display matrix above
2. Add a PerCoreSnap field carrying its source state
3. Add a GUI render of the field
4. Decide priority order if it can co-occur with existing reasons
5. Add `cat="gate"` health log emission on transitions
6. Add a test in `controller_test.cpp` asserting display matches
   hot-path state under that condition
7. Update CHANGELOG with the new term + GUI surface
8. Increment the term-counter assertion in the predicate-parity test

If any step is "deferred to next sprint" — the change is incomplete.
Half-wired predicates are how this bug class entered the codebase.
