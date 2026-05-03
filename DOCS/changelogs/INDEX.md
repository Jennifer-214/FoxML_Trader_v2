# Changelog Index

Chronological + version-grouped index of all per-sprint detailed
changelogs in this directory. Top-level summary lives in
`DOCS/CHANGELOG.md` (one row per version with elevator pitch); this
file maps each entry to its detailed write-up.

**Reading order recommendation:** if you're new to the codebase,
read `CLAUDE.md` first (always-loaded reference), then skim recent
sprint files here in reverse-chronological order to see how the
architecture evolved.

---

## v5.9.x — ML Hardening (2026-05-02)

Silent-failure surfacing + train-serve parity. 27 V5_9_AUDIT
findings closed across 6 phases / 17+ tags / 1 calendar day.

| Tag(s) | Date | File |
|---|---|---|
| v5.9.0 → v5.9.5j.2 | 2026-05-02 | [`2026-05-02-v5.9-ml-hardening.md`](2026-05-02-v5.9-ml-hardening.md) |

## v5.8.x — Easy Additions (2026-05-01)

X-macro registries everywhere (FOREACH_STRATEGY, FOREACH_FEATURE,
FOREACH_PANEL, etc.). Pure refactor sprint; no operator-visible
behavior changes.

| Tag(s) | Date | File |
|---|---|---|
| v5.8.0 → v5.8.10 | 2026-05-01 | [`2026-05-01-v5.8-easy-additions.md`](2026-05-01-v5.8-easy-additions.md) |

## v5.7.x — Strategy Quality (2026-04-30)

Regime audit + MOM filters + per-trade logging + quality dashboard.
5-phase ship.

| Tag(s) | Date | File |
|---|---|---|
| v5.7.0 → v5.7.6 | 2026-04-30 | [`2026-04-30-v5.7-strategy-quality.md`](2026-04-30-v5.7-strategy-quality.md) |
| audit | 2026-04-30 | [`2026-04-30-regime-classifier-audit.md`](2026-04-30-regime-classifier-audit.md) |

## v5.6.x — Execution / Display Divergence (2026-04-30)

7-phase audit closing the silent-block bug class. New
`DOCS/EXECUTION_DISPLAY_INVARIANTS.md`.

| Tag(s) | Date | File |
|---|---|---|
| v5.6.0 → v5.6.6 | 2026-04-30 | [`2026-04-30-v5.6-execution-display.md`](2026-04-30-v5.6-execution-display.md) |

## v5.5.x — Bug fixes + Class 8 port (2026-04-30)

zero_gate latent bug, regime-transition SL ratchet fee floor cap,
CostModel + VolScaler integration, BeginTable column count fix.

(detail merged into v5.6 / v5.7 changelogs above; no dedicated v5.5
changelog file)

## v5.4.x — Strategy Lifecycle Restoration (2026-04-30)

7-phase ship closing the v4.0 sharding regression: strategy `_Init`,
`_Adapt`, `_BuySignal`, `_ExitAdjust`, `Regime_AdjustPositions`
properly wired across all 5 strategies.

(detail merged into broader v5.x docs; postmortem in
`DOCS/changelogs/v5.4-regression-postmortem.md` if extracted)

## v5.3.x — Held-out + In-process HMAC (2026-04-29)

Phase A held-out training + Phase B in-process EVP signing.
`stamp_write_for_model` for in-process stamping; auto-stamp +
RunHistory JSONL appender.

(detail merged into earlier v5.x docs)

## v5.2.x → v5.0.x — Per-core sharding migration

Sharded engine architecture — branchless ~60ns hot path, per-core
ExecutionCore + per-core PortfolioController + central OMS.

| Topic | Date | File |
|---|---|---|
| Symmetric data-plane decouple | 2026-04-28 | [`2026-04-28-v5.1.2-symmetric-decouple.md`](2026-04-28-v5.1.2-symmetric-decouple.md) |
| Slow-path breakdown profiling | 2026-04-28 | [`2026-04-28-v5.1.1-breakdown-profiling.md`](2026-04-28-v5.1.1-breakdown-profiling.md) |
| Data-plane decouple | 2026-04-28 | [`2026-04-28-v5.1.0-data-plane-decouple.md`](2026-04-28-v5.1.0-data-plane-decouple.md) |
| Sanitizer validation | 2026-04-28 | [`2026-04-28-v5.0.5-sanitizer-validation.md`](2026-04-28-v5.0.5-sanitizer-validation.md) |
| Parity tests | 2026-04-28 | [`2026-04-28-v5.0.4-parity-tests.md`](2026-04-28-v5.0.4-parity-tests.md) |
| Topology advanced | 2026-04-28 | [`2026-04-28-v5.0.3-topology-advanced.md`](2026-04-28-v5.0.3-topology-advanced.md) |
| Pinning + topology | 2026-04-28 | [`2026-04-28-v5.0.2-pinning-and-topology.md`](2026-04-28-v5.0.2-pinning-and-topology.md) |

## v4.7.x — Partial Exits + Cache Optimizations (2026-04-27 → 2026-04-28)

Two-leg partial exits (TP1 + TP2 + breakeven SL on trigger).
Per-core cache optimization. X-macro registries shipped earlier
than v5.8.

| Topic | Date | File |
|---|---|---|
| Stats avg fields | 2026-04-28 | [`2026-04-28-v4.7.25-stats-avg-fields.md`](2026-04-28-v4.7.25-stats-avg-fields.md) |
| Per-core X-macro | 2026-04-28 | [`2026-04-28-v4.7.24-per-core-x-macro.md`](2026-04-28-v4.7.24-per-core-x-macro.md) |
| Strategy-aware tabs | 2026-04-28 | [`2026-04-28-v4.7.23-strategy-aware-tabs.md`](2026-04-28-v4.7.23-strategy-aware-tabs.md) |
| Settings cosmetic | 2026-04-28 | [`2026-04-28-v4.7.22-settings-cosmetic.md`](2026-04-28-v4.7.22-settings-cosmetic.md) |
| W/L pairing + chart fit | 2026-04-28 | [`2026-04-28-v4.7.21-wl-pairing-and-chart-fit.md`](2026-04-28-v4.7.21-wl-pairing-and-chart-fit.md) |
| Counter CSV atomicity | 2026-04-27 | [`2026-04-27-v4.7.19-counter-csv-atomicity.md`](2026-04-27-v4.7.19-counter-csv-atomicity.md) |
| GUI cleanup | 2026-04-27 | [`2026-04-27-v4.7.18-gui-cleanup.md`](2026-04-27-v4.7.18-gui-cleanup.md) |
| Shared slow-path | 2026-04-27 | [`2026-04-27-v4.7.17-shared-slowpath.md`](2026-04-27-v4.7.17-shared-slowpath.md) |
| Parity + chart | 2026-04-27 | [`2026-04-27-v4.7.16-parity-and-chart.md`](2026-04-27-v4.7.16-parity-and-chart.md) |
| Partial exits (initial) | 2026-04-27 | [`2026-04-27-v4.7.0-partial-exits.md`](2026-04-27-v4.7.0-partial-exits.md) |
| v4.7.1 fixes | 2026-04-27 | [`2026-04-27-v4.7.1-fixes.md`](2026-04-27-v4.7.1-fixes.md) |

## v4.6.x — D-wave 2 (2026-04-27)

Spread state + book imbalance + flow features.

| Topic | Date | File |
|---|---|---|
| Wave 2 (D.3 spread) | 2026-04-27 | [`2026-04-27-v4.6.0-wave2.md`](2026-04-27-v4.6.0-wave2.md) |

## v4.5.x — D-wave 1 (2026-04-27)

Wave 1: D.1/D.2/D.4 state. Book imbalance history, flow_state,
large_trade_state.

| Topic | Date | File |
|---|---|---|
| Wave 1 | 2026-04-27 | [`2026-04-27-v4.5.0-wave1.md`](2026-04-27-v4.5.0-wave1.md) |

## v4.4.x — Track E migrations (2026-04-26)

E.4 + E.5 walk-forward / sweep migrations. `Backtest_RunSweep`
calls `Backtest_Run` per-iteration; OptimizerPanel UI.

| Topic | Date | File |
|---|---|---|
| Track E (broad) | 2026-04-26 | [`2026-04-26-v4.4.0-track-e.md`](2026-04-26-v4.4.0-track-e.md) |

## v4.3.x — Past Runs + GUI Wiring (2026-04-08 → 2026-04-09)

Models/{name}/ subdirectory scan; per-run save bundles.

| Topic | Date | File |
|---|---|---|
| GUI wiring | 2026-04-09 | [`2026-04-09-gui-wiring.md`](2026-04-09-gui-wiring.md) |
| Per-core cache opt | 2026-04-08 | [`2026-04-08-percore-cache-opt.md`](2026-04-08-percore-cache-opt.md) |
| OMS phase 01 + 02 | 2026-04-08 | [`2026-04-08-oms-phase-01-02.md`](2026-04-08-oms-phase-01-02.md) |
| Live trading wiring | 2026-04-08 | [`2026-04-08-live-trading-wiring.md`](2026-04-08-live-trading-wiring.md) |

## Phase 6prep + Confidence loop (2026-04-25)

ConfidenceScorer + IC compute + freshness decay.

| Topic | Date | File |
|---|---|---|
| Confidence loop | 2026-04-25 | [`2026-04-25-phase6prep-confidence-loop.md`](2026-04-25-phase6prep-confidence-loop.md) |

## Walk-forward + Suite + Safety (2026-04-02 → 2026-04-04)

| Topic | Date | File |
|---|---|---|
| Walk-forward | 2026-04-04 | [`2026-04-04-walkforward.md`](2026-04-04-walkforward.md) |
| Suite | 2026-04-03 | [`2026-04-03-suite.md`](2026-04-03-suite.md) |
| Safety | 2026-04-02 | [`2026-04-02-safety.md`](2026-04-02-safety.md) |
| Gate reasons | 2026-04-02 | [`2026-04-02-gate-reasons.md`](2026-04-02-gate-reasons.md) |

## Risk infra + P&L regime + Strategy + Polish (2026-03-29 → 2026-03-30)

Foundational risk infrastructure + regime detection.

| Topic | Date | File |
|---|---|---|
| Risk infra | 2026-03-30 | [`2026-03-30-risk-infra.md`](2026-03-30-risk-infra.md) |
| P&L regime | 2026-03-30 | [`2026-03-30-pnl-regime.md`](2026-03-30-pnl-regime.md) |
| Strategy | 2026-03-30 | [`2026-03-30-strategy.md`](2026-03-30-strategy.md) |
| Polish | 2026-03-29 | [`2026-03-29-polish.md`](2026-03-29-polish.md) |

## Early sprint (2026-03-19 → 2026-03-28)

Initial scaffolding through GUI + audits.

| Topic | Date | File |
|---|---|---|
| GUI changes | 2026-03-28 | [`2026-03-28-gui.md`](2026-03-28-gui.md) |
| Misc B | 2026-03-28 | [`2026-03-28-b.md`](2026-03-28-b.md) |
| Misc | 2026-03-28 | [`2026-03-28.md`](2026-03-28.md) |
| Known-issues audit (legacy) | 2026-03-27 | [`2026-03-27-known-issues-audit.md`](2026-03-27-known-issues-audit.md) |
| Misc | 2026-03-27 | [`2026-03-27.md`](2026-03-27.md) |
| Various 03-24 | 2026-03-24 | [`2026-03-24.md`](2026-03-24.md), [`-b`](2026-03-24-b.md), [`-c`](2026-03-24-c.md), [`-d`](2026-03-24-d.md) |
| Various 03-23 | 2026-03-23 | [`2026-03-23.md`](2026-03-23.md), [`-b`](2026-03-23-b.md) |
| Misc | 2026-03-22 | [`2026-03-22.md`](2026-03-22.md) |
| Initial | 2026-03-21 | [`2026-03-21.md`](2026-03-21.md) |
| Initial | 2026-03-20 | [`2026-03-20.md`](2026-03-20.md) |
| Initial | 2026-03-19 | [`2026-03-19.md`](2026-03-19.md) |

---

## How to add new entries

When you ship a new sprint or hotfix that warrants its own changelog:

1. Create `DOCS/changelogs/YYYY-MM-DD-<theme>.md` with the detailed
   write-up
2. Add a row to this index under the appropriate version family
   header (or create a new family if it's a new minor)
3. Update `DOCS/CHANGELOG.md`'s top-level table with a one-line
   summary linking back to the dated file

Filename conventions:
- Date prefix `YYYY-MM-DD-` for sortability
- Version tag if it's a sprint exit (e.g., `v5.9-ml-hardening`)
- Theme keyword if multiple ships same day (e.g., `-pnl-regime`,
  `-risk-infra`)
- Hotfix suffix `.N` if same theme + same day (e.g., `-b`, `-c`)

## When detail belongs here vs CHANGELOG.md

- **CHANGELOG.md row** = elevator pitch. 1-2 sentences. Operator
  scanning to remember "what was that sprint about."
- **DOCS/changelogs/** = full detail. Phases, file:line citations,
  postmortem lessons, follow-ups. Future-Claude orientation.

If you find yourself writing >200 chars for a CHANGELOG row, split
the detail to a dated file + link.
