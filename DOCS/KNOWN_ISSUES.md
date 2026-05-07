# Known Issues, Active Testing, and Operator Workarounds

**Last updated:** 2026-05-06 (post-v5.10.0a.next.2)
**Earlier audit:** `DOCS/changelogs/2026-03-27-known-issues-audit.md`
(legacy PortfolioController items; mostly superseded by sharded
architecture v5.0+)

This doc captures items that operators may encounter but which **are
not bugs requiring action**. Distinct from:

- `RECURRING_BUG_PATTERNS.md` — historical bug classes that have been
  fixed but should be remembered to avoid regressions
- `DOCS/changelogs/` — what shipped when (per-sprint detail)

If you hit something here, the workaround / explanation lives in the
relevant section. If you hit something **not** here, it may be a
genuine bug — open an issue or check the sprint plan in `plans/`.

---

## Active operator testing (Sprint B, v5.10 in flight)

**Status:** Sprint A merged 2026-05-03 (commit `5ea002c`,
`v5.9.5j-final`). Sprint B 2/6 shipped on `feat/v5.10-foundation`:
v5.10.0 (foundation) + v5.10.0a-final (multi-horizon ensemble).

What to validate end-to-end on v5.10 ships:

1. **Per-phase timers (v5.10.0A)** — Backtest panel shows feature
   collect / train / WF / held-out / stamp emit timings. Operator
   confirms expected order-of-magnitude per phase before any further
   perf claims.
2. **Streaming label compute (v5.10.0B)** — 2-year feature collect
   runs without OOM (closes 2026-05-03 incident). Per-sample
   allocation, not per-tick.
3. **Hardware-aware cfg (v5.10.0D)** — `xgb_train_nthread`,
   `csv_load_workers`, `feature_collect_max_gb`, `wf_split_max_gb`,
   `held_out_max_gb`, `xgb_eval_nthread` all parse + reach
   compute paths. Defaults match pre-v5.10 bytewise.
4. **Save Run race (v5.10.0E)** — `auto_stamp_path` doesn't race
   with stale `wf_results`; Save Run emits a coherent bundle.
5. **Multi-horizon training (v5.10.0a.G.1)** — Train Multi-Horizon
   button trains N models, one per horizon in `horizon_list` cfg.
   Per-horizon dirs land at `<base>_horizon_<H>/`.
6. **Ensemble inference (v5.10.0a.G.5–G.7)** — engine boot
   auto-detects N horizons from disk; per-regime BanditState blends
   predictions. ML Status panel ensemble heatmap (G.10) shows
   regimes × horizons weight grid.
7. **Bandit reward + persistence (v5.10.0a.G.8 / G.9)** —
   slow-path lookback + trade-close rewards stream into bandit
   weights; `bandit_state.json` survives restart bytewise.
8. **Bandit replay-determinism (v5.10.0a.next.2)** — same input
   sequence + same prior weights → bytewise-identical bandit
   trajectory in backtest replay.

**Validation gate before merging Sprint B back to
`experiment/per-core-sharding`:** all six v5.10 ships exercised
end-to-end on operator hardware; ML Status panel ensemble heatmap
shows live updates; `bandit_state.json` round-trips across restart.

---

## Hardware constraints + customization gaps (closed in v5.10.0D)

**Closed 2026-05-03 in v5.10.0D ("hardware-aware cfg").** The
parameters below are now cfg-driven; defaults preserve pre-v5.10
behavior bytewise so existing operators see no change unless they
opt in.

| Parameter | Cfg field (v5.10.0D) | Default | Notes |
|---|---|---|---|
| Train Model XGBoost `nthread` | `xgb_train_nthread` | 1 | matches pre-v5.10 hardcoded; opt-in >1 |
| WF / HeldOut XGBoost `nthread` | `xgb_eval_nthread` | 1 | >1 breaks bytewise reproducibility — WARN at apply |
| CSV-load thread count | `csv_load_workers` | 1 | parallel CSV ingest in v5.10.0 foundation |
| Feature collect RAM cap | `feature_collect_max_gb` | 12 | abort with clear msg if budget exceeded |
| WF split RAM cap | `wf_split_max_gb` | 8 | same abort behavior |
| Held-out RAM cap | `held_out_max_gb` | 4 | same |

**Still hardware-fixed (struct redesign required to change):**
- `MAX_PORTFOLIO_POSITIONS` = 16 (`Limits.hpp`) — `uint16_t` bitmap
- `MAX_EXECUTION_CORES` = 16 (`Limits.hpp`) — covered by
  `num_execution_cores` cfg

**Migrating to bigger hardware:** bump `xgb_train_nthread` first
(largest wall-clock win on multi-core boxes). Leave eval threads at
1 unless you accept non-bytewise WF/HeldOut output. Bump
`csv_load_workers` to ~nproc/2 for diminishing returns past that.

---

## Known data quality issues

### TickRecorder write-truncation (workaround shipped in v5.9.5j.2)

Daily Binance CSVs occasionally contain truncated rows where the
recording process was interrupted mid-write (SIGTERM, crash, etc.).
Symptoms:

```
Bogus row in 2024-04-29.csv:
  2990172343,63008.69000000,0.00018000,3578270992,3578270992,17144

Normal row (8 fields):
  2989519398,63118.62000000,0.00070000,3577384221,3577384221,1714348800000,True,True
```

The truncated row has 6 fields (last 2 dropped) and the timestamp
column ends with a partial value (`17144` instead of `1714348800000`).

**Workaround (active):** v5.9.5j.2 filter at parse time drops any
tick with `ts < 1.5e12 ms` (= 2017-07-14). This catches all observed
corruption patterns:

- `ts = 17144` (timestamp truncated to 5 chars)
- `ts = 0` (zero / null parse)
- `ts = 1719336055` (ms→sec truncation, last 3 digits cut)

**Impact:** ~30 corrupt rows in ~400M total = statistically invisible.
Filter eliminates them at load; no manual data cleaning needed.

### CSV ordering at file boundaries (workaround shipped in v5.9.2c)

Binance recording occasionally emits ticks slightly out-of-order at
day boundaries (clock skew). Symptoms: `[WARN] data file N has K tick
ordering violations` per affected file.

**Workaround:** set `csv_sort_check_mode=2` in `backtest.cfg` →
auto-sort on load. ~2-3s prep cost; eliminates warnings + guarantees
monotonic stream → cleaner features near boundary indices.

---

## Pre-existing build warnings (false positives)

The following warnings appear during `./build.sh` and are **not
bugs**:

| Warning | Site | Cause |
|---|---|---|
| `-Wstringop-overflow` "writing 1 byte into a region of size 0" at offset 1248576 | `tests/controller_test.cpp:5104` (writing through `EventLoopState`) | GCC constprop pass making bogus offset computations during inlining; tests pass at runtime |
| Same pattern | `FauxFIX.hpp:286`, `SPSCRing.hpp:128`, `ControllerEventLoop.hpp:816` | Same constprop class; pre-existing since pre-v5.9 |
| `-Waggressive-loop-optimizations` "iteration 5 invokes undefined behavior" | `TUIAnsi.hpp:824` | Pre-existing; loop bound analysis edge case |
| Lambda capture warning at `EngineSharded.hpp:2085` | Capture of `cores` with non-automatic storage | Pre-existing; static-storage capture pattern |

These are **separate from** the v5.9.5a real overflow that was fixed
(FeatureStandardizer Persist/Load — bug closed). The list above is
known-noise, not real overflows.

---

## Limitations (not bugs; future-work tracked elsewhere)

### BTC-only training + inference

The engine is BTCUSDT-focused. Stamps don't bind to symbol. If you
train on BTCUSDT and deploy on ETHUSDT (same data shape), engine
doesn't notice. Stamp body needs `trained_symbol` field for
symbol-aware verification.

**Tracking:** internal roadmap — multi-symbol candidate.

### Cross-build determinism: detection-only

Cross-build deploy (different `-O` level / `-march` / `USE_NATIVE_128`)
fires `[build_flags] WARN` at boot (v5.9.5h) so operator notices the
drift. But the model can still be loaded — predictions may silently
shift due to IEEE-754 reorderings.

**For full safety:** v5.10.0b ships FPN-end-to-end refactor (~500 LOC
double → FPN). Until then, deploy with the same build config used at
training time.

### v5.9.5i strict-mode REFUSE is observability-grade

When stamp's `inference_cfg_*` differs from runtime cfg in strict
mode, engine logs `[inference_cfg] FATAL: ... N Tier 1 mismatch(es)`
+ counters but **continues to run** with the model loaded. True
load-time refuse (free handle, return-from-boot to abort) is v5.10.

**Workaround:** treat the FATAL log as "abort engine + retrain or fix
cfg" yourself. Don't deploy a model whose stamp's cfg differs from
runtime cfg.

### Train Model auto-stamp uses sentinel held-out

v5.9.5j #6 auto-stamps with `held_out=0.0` + `gap_threshold=0.0`
sentinels (Option A: WF-only). Engine treats as info-grade
("training-only stamp"); won't refuse on missing held-out.

**For deploy-grade stamps:** use Run Full Validation panel which runs
held-out training + emits a stamp with real metrics.

### Backtest hot-loop perf (partial close in v5.10.0)

89 minutes for 895M ticks (~167K ticks/sec) on a 4-core dev box. ML
pipeline itself is <1 minute of that — cost is dominated by CSV
parsing + 4-core hot-path simulation + slow-path feature collection.

**Closed in v5.10.0 foundation:** parallel CSV ingest
(`csv_load_workers`) + sparse label buffer (Idea #15, see below).

**Still pending:** SIMD RegimeSignals deferred behind v5.10.0b
(FPN-end-to-end refactor, target MODEL_FORMAT_VERSION 6) since FPN
refactor will move RegimeSignals math out of float anyway.

For now: reduce dataset days for iteration loops; full 365-day
training is a one-shot that you tolerate.

### Label buffer OOMs on 2+ year datasets (closed in v5.10.0B)

**Observed 2026-05-03:** operator attempted 2-year feature collection
on 30.9 GiB RAM box → OOM crash during feature collection.

**Math (pre-fix):**
- 1 year (~895M ticks) → label buffer 28.6 GB
- 2 years (~1.8B ticks) → label buffer ~57 GB → exceeds RAM
- Allocation pattern: per-tick label slot (32 bytes), but only
  sample-point labels (every poll_interval ticks) actually used

**Closed 2026-05-03 in v5.10.0B ("streaming label compute").**
Per-sample allocation, not per-tick. With `poll_interval=100`, the
buffer is now 100x smaller (~286 MB for 1 year, ~570 MB for 2
years). Bytewise-equivalent label output to the prior dense path —
verified against a small-dataset round-trip test before merge.

**Confirm a 2-year run works** on RAM-constrained boxes after pull;
this was the highest-impact v5.10 fix for operators with multi-year
historical data.

---

## Deferred features (planned, not shipped)

Tracking what shipped in v5.10 and what's still pending. If you
wanted something and it's not here, that's why:

**Closed in Sprint A (v5.9):**
- ✅ Per-class accuracy display (#8) — v5.9.5j
- ✅ ConfidenceScorer extended snapshot tests (#9) — v5.9.5j

**Closed in Sprint B (v5.10) so far:**
- ✅ Operator-tunable XGBoost `nthread` via cfg — v5.10.0D
- ✅ Parallel CSV load — v5.10.0 foundation
- ✅ Sparse label buffer (28GB → 286MB) — v5.10.0B
- ✅ Multi-horizon ensemble + per-regime bandit blend —
  v5.10.0a-final + .next.1 + .next.2

**Still pending in v5.10:**
- **FPN-end-to-end** — v5.10.0b (next ship). Bumps
  MODEL_FORMAT_VERSION 5→6; closes "Cross-build determinism" above.
- **Hot model swap** — v5.10.0c. Engine reloads on cfg-change
  without restart. Depends on v5.10.0b for format-6 swap targets.
- **FOREACH_TARGET label registry** — v5.10.0d. X-macro retrofit
  for labels (mirrors v5.8.x FOREACH_FEATURE pattern).
- **Drift detection / model retirement** — v5.10.0e. Runtime IC
  monitoring + auto-retire on threshold breach.
- **SIMD RegimeSignals** — folded into v5.10.0b (FPN refactor will
  move regime math out of float anyway).

**Deferred to v5.11 (Sprint C):**
- Multi-symbol (see "BTC-only" above)
- Per-core feature mask cfg
- Scaler comparison tool
- RFV scaler binding
- Operator quickstart docs gap
- Test file split (controller_test.cpp at ~12k lines)

Full list lives in internal roadmap (gitignored — operator-private).

---

## Operator workflow gotchas

### Build staleness across binaries

Engine_gui and foxml_suite share `Version.hpp` and the same code, but
each rebuilds separately. After pulling code or changing Version.hpp:

```bash
./build.sh test gui suite
```

Otherwise one binary may be at v5.9.5j while the other shows v5.9.5i.
Side-by-side comparison gets confusing.

### Past Runs panel TableSetupColumn class

Adding a column to the Past Runs table without bumping `BeginTable`'s
expected count argument crashes the panel on first render with
`TableSetupColumn(): called too many times!`. Last hit in v5.9.5h
Phase 11; fixed in v5.9.5j.1. **If you add a column later: bump the
count too.**

Same pattern bit Hold column in v5.5.3.

### foxml_suite spawn shows different version than engine_gui

If you launched both before rebuilding, they show different versions.
Restart whichever is older.

### `backtest.cfg` is gitignored (post-v5.9.5j.2)

Live cfgs (`engine.cfg`, `backtest.cfg`, `secrets.cfg`,
`controller.cfg`) are private. Template (`engine.cfg.example`) is
public. Operator-tuned values + secrets stay local.

For off-machine backup: cfgs are mirrored to the workspace repo at
`tick-trader-percore-workspace/configs/` (private GitHub remote).

### `is_buyer_maker` dropped between SPSC ring and slow-path RollingStats (since v5.1.2; v5.10.3.C documented)

**Symptom:** `FEAT_VOLUME_DELTA` values cluster tightly near +1.0 instead
of being distributed [-1, +1]. Models trained on this feature can't learn
from it productively.

**Root cause:** The v5.1.2 sharded slow-path scalar bus doesn't carry
`is_buyer_maker`; `EventLoop_UpdateRollingStateOneCore` calls
`RollingStats_Push` with `is_buyer_maker=0` hardcoded
(`CoreFrameworks/EngineSharded.hpp:2663`). `CumDelta` and
`FlowState_Push` get the correct flag (line 1641, 1650-1652);
RollingStats does NOT.

**Parity status:** Train-serve parity PRESERVED. `BacktestSharded`
mirrors the live behavior via `SharedBacktest_FromHistorical` zeroing
the field (`Backtest/BacktestSharded.hpp:78-86`). Both training and
serving see the same degraded feature → no drift, no silent
miscalibration. Models simply can't learn from this signal.

**Mitigation:** None needed for parity. Feature is zero-information;
models that don't depend on it are unaffected. The `volume_delta`
docstring at `Strategies/RegimeDetector.hpp:67` overpromises ("net
buy/sell pressure ... -1.0 to +1.0") relative to actual behavior.

**Full closure (~4h, v5.10.X or v5.11+):**
- Plumb `is_buyer_maker` through the scalar bus
  (`g_last_buyer_maker.store(...)` per producer tick)
- Read it in `EventLoop_UpdateRollingStateOneCore`
- Pass to `RollingStats_Push`
- Patch `SharedBacktest_FromHistorical` to copy `h->is_buyer_maker`
- Re-run `v5.9.2` replay-determinism test to verify bytewise unchanged
  (it'll diverge — VOLUME_DELTA values will redistribute — so this
  requires retraining downstream models)

See `plans/plan_checks/parity-2026-05-06-full.md` Finding #5 for
the full audit context.

### `drift_history` not persisted across snapshot save/restore (v5.10.0e + v5.10.3.C documented)

**Symptom:** After engine restart from a snapshot, drift detection
re-warms from empty. Up to `confidence_ic_floor_window` seconds
(default 86400 = 24h) of IC samples must accumulate before the drift
detector can re-arm. Operator running `auto_kill_on_drift=1` loses 24h
of drift coverage on each restart.

**Root cause:** `SHARDED_SNAPSHOT_VERSION=6` doesn't serialize
`CoreContext.drift_history` (struct introduced at v5.10.0e;
`ML_Headers/ConfidenceScore.hpp:265-273`). The 256-slot ring buffer of
`(ic, ts_us)` samples + `breached` / `kill_tripped` flags rebuilds from
zero post-restart.

**Mitigation:** Lower `confidence_ic_floor_window` (e.g. to 3600 = 1h)
if you restart frequently and want faster re-arming. Trade-off:
more false-positive breaches from short-window noise.

**Full closure (~2.5h, v5.10.X or v5.11+):**
- Bump `SHARDED_SNAPSHOT_VERSION` to 7
- Serialize `ic_samples[]` + `ts_us[]` + `count` + `head` + `breached`
  + `breach_first_us` + `kill_tripped`
- Add back-compat read for v6 stamps (zero-fill drift_history on
  upgrade)
- See `plans/plan_checks/parity-2026-05-06-full.md` Finding #11.

---

## How to update this doc

When you ship a hotfix or close a known issue:
1. Update the relevant section here
2. Mark the issue as `(workaround shipped in vX.Y.Z)` or `(closed in vX.Y.Z)`
3. Don't delete closed entries for at least 2 sprints — they help
   future-you remember what bit you

When you discover a new known issue:
1. Add to the appropriate section (data quality / limitations /
   workflow gotchas)
2. Cite the symptom + workaround
3. Reference the v5.10/v5.11 plan if there's a fix in the queue

This doc is operator + future-Claude orientation. Keep it scannable
(< 1 screen per section). Detailed sprint history lives in
`DOCS/changelogs/`.
