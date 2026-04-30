# Hot-Path Changelog

Append-only record of changes to the hot path (per-tick code in
`ExecutionCore_Tick` and inlined gate evaluators). Each entry pins
WHEN, WHAT, COST estimate, and OPTIMIZATION NOTE so future
optimization passes have a punch list to start from.

The hot path runs once per tick per execution core, target p99 ≤ 500 ns.
Anything added here is paid every tick; the slow path's microsecond
budget does not apply.

## Format

```
### YYYY-MM-DD — vX.Y.Z phase / feature

**File:line:** what changed in one sentence.

**Cost:** estimated ns/tick (leg-A vs leg-B when paired).

**Branchless:** yes/no/conditional. If conditional, what gates it.

**Cache impact:** field offset + cache line. Note any new straddles.

**Optimization note:** what could be cheaper later (e.g. precompute on
slow path, fold into existing operation, hoist to entry).
```

---

### 2026-04-30 — v5.4.0 Phase 3.3 — `ratchet_tp` channel

**Files:**
- `CoreFrameworks/GateParameters.hpp` — added `FPN<F> ratchet_tp` field.
- `CoreFrameworks/ExecutionCore.hpp:~322` — leg-A uses
  `effective_tp = FPN_Max(tp, ratchet_tp)`; leg-B same inside the
  existing `__builtin_expect(active_b, 0)` block.
- `CoreFrameworks/GateParameters.hpp` — standalone `SG_Evaluate` mirrors
  the change for parity (used in tests + non-hot-path callers).

**Cost:** +1 ns leg-A always; +1 ns leg-B when a pair is open. Mirrors
the shape of the existing `ratchet_sl` `FPN_Max` (which itself self-documents
~1 ns).

**Branchless:** yes for leg-A. Leg-B is inside an existing
predicted-not-taken `if (active_b)` — no new branch, two extra FPN ops
inside the taken-case body.

**Cache impact:** `ratchet_tp` at offset 216..239, fully within cache line 3.
No new straddles. `sizeof(GateParameters<64>)` was 256 bytes (4 cache lines,
`alignas(64)`); still 256 after — the 24-byte field absorbed existing
alignment slack.

**Optimization note:** if `ratchet_tp` and `ratchet_sl` are usually both
zero (the steady state for cores without open positions), a flag bit in
`flags` could short-circuit both `FPN_Max` calls in a single test. Not
worth doing yet — slow-path D9 already clears `ratchet_sl` to zero on
slot-inactive cycles, so `FPN_Max(x, 0) = x` is the common case and the
mask-select cost is amortized into the already-present SL ratchet.

---

## Pre-v5.4 entries (retroactive — back-fill as encountered)

The fields that already existed before v5.4.0 (and their costs) are
documented inline in `ExecutionCore.hpp`. This changelog starts tracking
*new* additions from v5.4.0 forward. If an old field gets hot-path
attention during a future optimization pass, back-fill an entry here at
that point.
