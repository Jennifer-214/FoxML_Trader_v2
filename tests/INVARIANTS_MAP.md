# INVARIANTS_MAP.md

Maps each Safety Invariant from `DOCS/CLAUDE_INVARIANTS.md` to the
test(s) that would catch a regression.

**Use case**: "I'm modifying X, what invariants does it touch, what tests
verify them?" Read this map, find affected invariants, verify tests
still pass after change.

**Generated 2026-04-29** by hand-mapping (no auto-regen tool yet — invariants
change rarely enough that manual updates work).

## Coverage verdicts

- **COVERED** — explicit test(s) exist that would fail if invariant breaks
- **PARTIAL** — some aspect tested but not exhaustive
- **DISCIPLINE** — process/structural invariant, not directly unit-testable
- **GAP** — no test found, invariant relies on human discipline alone

## The 22 invariants

| # | Invariant | Tests covering it | Coverage |
|---|---|---|---|
| 1 | **Position Exit Invariants** (TP > entry > SL, 2:1 reward/risk, fee-floor TP) | Position Exit Gate (line 224), Regime Adjust: TRENDING_DOWN TP/SL (1805), FEE FLOOR AFTER REGIME TIGHTENING (2860) | COVERED |
| 2 | **FPN Division Guards** (`FPN_DivNoAssert` requires `IsZero` guard) | Volume Spike Detection (1451), DANGER GRADIENT (2080), Wave 2 D.3 (5638) | COVERED |
| 3 | **Fill-Counter Atomicity** (v4.7.19 — bump only in DrainPostFill) | v4.7.16 — backtest/live parity (6198) via DrainPostFill mask walk; v4.7.19 — counter/CSV atomicity (6403) | COVERED |
| 4 | **Config Field Conventions** (`_pct` decimal vs `_mult` direct) | Config Parser (72), Phase 5d: Config validation (3126) | COVERED |
| 5 | **Cross-Mode Init Placement** (legacy + sharded both initialize globals) | Phase 4: sharded snapshot (4300), v5.0.4 — Parity (6807) | DISCIPLINE — structural rule, not directly testable |
| 6 | **FPN-Only Accounting** (no double in decision logic) | Full Pipeline Integration (723), BALANCE DRIFT (2529), Phase 2.1 (3883) | COVERED |
| 7 | **FPN Comparison Completeness** (no partial-word ops) | FPN EXIT GATE COMPARISON (2464), Wave 2 D.3 spread_bps (5638) | COVERED — but `Portfolio.hpp:226-229` has documented partial-word bug accepted as known issue |
| 8 | **Halt Flag Invariant** (`buying_halted=1` AND zero `gate_offset`) | CENTRALIZED HALT FLAG (2220), GATE OFFSET TRACKING (2169) | COVERED |
| 9 | **Confidence Loop Invariant** (single update, slow-path-only compute, threshold formula) | Phase 6prep: ConfidenceScorer composition (3427); regime mapping with threshold (1971) | PARTIAL — formula tested; "single update site" relies on grep discipline |
| 10 | **Train-Serve Feature Parity** (BOTH paths feed `Regime_ComputeSignals` identically) | v4.7.16 — backtest/live parity (6198), Wave 2 D.3 RegimeSignals (5638), v5.0.4 — OneCore identity (6807); + `parity_harness` returns 0 drift | COVERED |
| 11 | **Maker/Taker Fee Accuracy** (Fee_Compute, is_maker source, sanity) | Phase 8: Fee_Compute helper (3652), Maker/taker accounting (3777), executionReport parser (3729) | COVERED |
| 12 | **Held-Out Validation Discipline** (token-locked, single eval, gap < threshold, **v5.2.0 stamp gate**) | Phase 7prep: HeldOutSplit math (3501), Lock-token discipline (3529), RunFullValidation framework (3558), v5.2.0 verify_model_stamp (8 tests) | COVERED + ENFORCED (v5.2.0 added crypto stamp check at model load) |
| 13 | **Operational Alerting** (Notify_Send levels, slow-path-only, kind cooldown) | Phase 8b: Notify lifecycle (3212), Send + dispatch (3225), Cooldown gate (3246) | COVERED |
| 14 | **Regime Adjustment Checklist** (FPN_Max/Min direction for tighten/widen) | Regime Adjust: TRENDING_DOWN TP/SL (1805), stddev=0 guard (1862), SL floor transitions (2010) | COVERED |
| 15 | **Label-type-aware metric invariant** (binary/regression/multiclass dispatch) | Phase 5d: Label-type-aware metric dispatch (3055), Class-balance helpers (3080) | COVERED |
| 16 | **Snapshot Re-Activation Invariant** (load reactivates ExecutionCore mirrors) | Phase 4: sharded snapshot (line 4501 re-activation test) | COVERED |
| 17 | **Snapshot Tick-Counter Drift** (`entry_t > now_tick` underflow guard) | Tick Counter (695), Phase 4: sharded snapshot tick-counter (4476) | COVERED |
| 18 | **Per-Core Data-Plane Single-Writer** (v5.1.0+ slow_state ownership) | v5.0.4 — Single-core update isolation (6896), Phase 2.1 core_open_notional (3883) | DISCIPLINE — single-writer rule is enforced by thread topology, threading-tests are TSan/ASan + parity_harness, not unit assertions |
| 19 | **Lifecycle Bitmap Single-Writer** (v5.0.3 paused_engines_mask) | v5.0.4 — Engine Topology (6998) | DISCIPLINE — GUI-thread-write / engine-thread-read is structural |
| 20 | **Per-Section Latency Stats Single-Writer** (v5.1.1 breakdown) | v5.0.4 — Topology field stability (6998) | DISCIPLINE — same as #19 |
| 21 | **Partial Exits — Two-Position-per-Core** (slot mapping, leg-A only counters) | P.1: Sharded_LegSlot mapping (5681), P.1: Validation (5711), P.2: dual-leg SG (5763) | COVERED |
| 22 | **W/L Pair Classification under Partials** (v4.7.21 partner_pending_pnl combined sign) | v4.7.21 — W/L pairing under partials (6522) via partner_pending_pnl tests | COVERED |

## Summary

- **17 COVERED** (78%) — direct test failure on regression
- **4 DISCIPLINE** (#5, #18, #19, #20) — structural invariants enforced by code
  layout / thread topology, verified via parity_harness + sanitizers
- **1 PARTIAL** (#9) — formula tested, "single update site" relies on grep
- **0 GAP** — every invariant has at least structural enforcement

## When to update this map

- After adding a new invariant to `DOCS/CLAUDE_INVARIANTS.md` — add a row here
- After adding a new test that covers an existing DISCIPLINE invariant — promote it to COVERED
- After fixing a known bug (e.g. `Portfolio.hpp:226-229` partial-word) — update verdict cell
- Periodically (~quarterly) — verify line numbers haven't drifted; re-grep test groups

## Companion docs

- `DOCS/CLAUDE_INVARIANTS.md` — the invariants themselves
- `DOCS/CLAUDE_REVIEW.md` — the 10-item Plan Review Checklist that gates new invariant additions
- `DOCS/CODE_MAP.md` — function lookup index
- `tests/controller_test.cpp` — actual test file (line numbers above point here)
