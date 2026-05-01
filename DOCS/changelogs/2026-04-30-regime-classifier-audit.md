# Regime Classifier Audit — 2026-04-30

**Trigger:** v5.7 Phase 0 audit per
`plans/2026-04-30-v5.7-strategy-quality.md`. Determines whether the
"MOM enters bad trades in RANGING regimes" symptom traces to:
- (a) hardcoded strategy bypassing AUTO,
- (b) regime hysteresis flicker,
- (c) classifier mis-thresholding,
- (d) regime → strategy map mis-tuning.

## Method

Replayed `logging/health.jsonl`'s existing `cat="regime"` entries
captured during v5.4 — v5.5 paper runs (cumulative ~18.3K cycles).
The log already records, per slow-path cycle, per AUTO core:

```json
{"old": <regime>, "new": <regime>, "resolved_strat": <id>,
 "ema_sma_spread": ..., "short_r2": ..., "ror_slope": ...,
 "hyst": "<count>/<threshold>", "short_count": ...}
```

Source: `ControllerEventLoop.hpp:1626` (`Health_Log("regime", ...)`).

## Findings

### Cycle distribution by resolved strategy (AUTO Core 2)

| Resolved | Cycles | % |
|---|---|---|
| MR (0) | 15,340 | 83.7% |
| MOM (1) | 2,071 | 11.3% |
| DIP (2) | 17 | 0.1% |
| EMA (4) | 898 | 4.9% |
| **Total** | **18,326** | 100% |

### Transition matrix (old → new regime)

| From / To | RANGING | TRENDING | VOLATILE | TR_DOWN | MILD_TR |
|---|---|---|---|---|---|
| RANGING | 13,441 | 24 | 2 | 44 | 22 |
| TRENDING | 19 | 2,019 | 0 | 33 | (n/a) |
| VOLATILE | 2 | 0 | 15 | 0 | 0 |

(Other transitions present but counts low.)

**Sustained TRENDING:** 2,019 cycles in `TRENDING:TRENDING` self-loop
vs ~85 cycles transitioning INTO TRENDING. Average ~24 cycles per
TRENDING period — that's ~24 × poll_interval ticks of sustained
classification, not flickering.

### Strategy transition counts (resolved → resolved)

| Transition | Count |
|---|---|
| EMA → MR | 47 |
| MR → EMA | 42 |
| MOM → EMA | 33 |
| EMA → MOM | 28 |
| MR → MOM | 24 |
| MOM → MR | 19 |
| DIP → MR | 2 |
| MR → DIP | 2 |
| **Total** | **197** |

197 transitions over 18,326 cycles ≈ one transition per 93 cycles.
Reasonable cadence — not pathological flicker.

## Verdict: PASS

The regime classifier is operating as designed:
- TRENDING is sustained (avg ~24 cycles when active)
- Distribution matches market behavior (most BTC time is ranging)
- Strategy transitions are bounded (~93 cycles between flips)
- Hysteresis is doing its job (`hyst=N/3` averaging suggests
  proposed-regime stability is required for switching)

**The "MOM bought at $76695 in supposedly RANGING" symptom traces to
`core_0_strategy=momentum`** — Core 0 is hardcoded MOM, runs
regardless of regime. Core 2 (the AUTO core) was correctly on MR
most of the time, including during the observed MOM bad trades on
Core 0. The screenshot pairing of "regime: RANGING" + "AUTO(MOM)"
was at a transient moment when Core 2 had briefly resolved to MOM
(11.3% of cycles per the table above). The MOM losing trades on
Core 0 are independent of that AUTO state.

## Action

- **v5.7 Phase 2 path applies** — cfg-side rerouting + boot warning
  for hardcoded strategies in live/paper mode. This is the clean fix
  for the observed symptom.
- **v5.7 Phase 3 (hysteresis tuning) — SKIP.** Existing hysteresis
  (default threshold=3) is sufficient.
- **v5.7 Phase 4 (classifier rethresholding) — SKIP.** Hardcoded
  `>= 2` thresholds are working empirically.
- **v5.7 Phase 5 MOM filters — STILL USEFUL** as defensive depth.
  Filters apply to ALL MOM cores (hardcoded + AUTO-resolved), so they
  protect Core 0 even after the cfg rerouting in Phase 2. The
  fee-floor case from v5.6 already proves the BUY_BLOCKED route works
  for marginal entries.

## Effort impact on v5.7 plan

- Phase 0 → 0.5h (vs 2-3h estimate) — the existing health log already
  captured the data we needed; just had to query it.
- Phase 3 / Phase 4 → 0h each (skipped per audit).
- Phase 2 → 1.5h (cfg + boot warning, scoped per plan).
- Total estimate revised: 22-26h → **17-20h**.
