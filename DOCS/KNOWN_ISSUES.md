# Known Issues, Active Testing, and Operator Workarounds

**Last updated:** 2026-05-03 (post-v5.9.5j.2)
**Earlier audit:** `DOCS/changelogs/2026-03-27-known-issues-audit.md`
(legacy PortfolioController items; mostly superseded by sharded
architecture v5.0+)

This doc captures items that operators may encounter but which **are
not bugs requiring action**. Distinct from:

- `RECURRING_BUG_PATTERNS.md` — historical bug classes that have been
  fixed but should be remembered to avoid regressions
- `v5.10-design-notes.md` — future work / planned features
- `DOCS/changelogs/` — what shipped when (per-sprint detail)

If you hit something here, the workaround / explanation lives in the
relevant section. If you hit something **not** here, it may be a
genuine bug — open an issue or check the sprint plan in `plans/`.

---

## Active operator testing (Sprint A, v5.9.5h-j just shipped)

**Status:** Sprint A complete (2026-05-02). Branch
`feat/v5.9-ml-hardening` not yet merged to
`experiment/per-core-sharding` pending paper-test validation.

What to validate end-to-end:

1. **Hyperparam ownership (v5.9.5h)** — Train Model panel "Advanced"
   section sliders adjust `xgb_subsample`, `xgb_colsample_bytree`,
   `xgb_min_child_weight`, `xgb_seed`, `xgb_tree_method`. Stamp body
   records all 8 hyperparams. Engine load WARNs on cfg drift.
2. **Build flags fingerprint (v5.9.5h #10)** — stamp records
   `build_flags_hash` (USE_NATIVE_128 / USE_XGBOOST / __OPTIMIZE__).
   Cross-build deploy → `[build_flags] WARN` at boot.
3. **Past Runs stamp_ok column (v5.9.5h #19)** — column shows
   `—` (no stamp) / `?` (unverified) / `✓` (OK) / `✗` (FAIL).
4. **Inference cfg load-WARN (v5.9.5i #12)** — Tier 1 (freshness_tau,
   confidence_threshold_scale, barrier_gate_enabled) REFUSE in strict
   mode / WARN otherwise. Tier 2 (hard_block, bandit, fees) WARN.
5. **ML Status drift row (v5.9.5i #16)** — engine_gui shows per-core
   "cfg drift: N Tier 1 (...), M Tier 2".
6. **Past Runs stamp filter (v5.9.5i #14 slim)** — filter combo
   isolates runs by stamp status.
7. **Train Model auto-stamp (v5.9.5j #6)** — when
   `cfg.auto_stamp_on_held_out=1` + `cfg.held_out_stamp_secret` set,
   worker auto-emits training-only stamp post-Persist.

**Validation gate before merge:** at minimum, exercise Train Model →
Verify Stamp → engine_gui load → confirm WARNs fire on intentional
cfg mismatch + don't fire on matching cfg.

---

## Hardware constraints + customization gaps

**Operator-flagged 2026-05-03:** several engine + ML pipeline
parameters are tuned for the dev machine and don't scale automatically
when operator moves to a bigger box (Xeon, more RAM, more cores).
Hardware-aware cfg fields are tracked as v5.10 candidate — see
`v5.10-design-notes.md` Idea #14 ("hardware-aware tunables").

What's currently dev-machine-tuned:

| Parameter | Current value | Where set | Should be cfg-tunable |
|---|---|---|---|
| Train Model XGBoost `nthread` | 4 (hardcoded) | `BacktestPanels.hpp:1898` (post-v5.9.5h XGBHyperparams_Apply) | Yes — `xgb_train_nthread` cfg |
| WF / HeldOut XGBoost `nthread` | 1 (hardcoded for determinism) | `BacktestEngine.hpp:1308`, `:1604` | Yes — but with WARN that >1 breaks bytewise reproducibility |
| Tick / label buffer initial allocation | `BACKTEST_SAMPLES_INIT` | `BacktestEngine.hpp` | Already grows dynamically; cap is RAM-driven |
| `MAX_PORTFOLIO_POSITIONS` | 16 | `Limits.hpp` | NO — bitmap is `uint16_t`; bumping means struct redesign |
| `MAX_EXECUTION_CORES` | 16 | `Limits.hpp` | Same — covered by `num_execution_cores` cfg already |
| CSV-load thread count (when v5.10 ships) | 1 (serial) | `BacktestData_Load` | Yes — `csv_load_workers` cfg |

**Until v5.10 ships hardware-aware cfg:** if you move to a Xeon with
more RAM/cores, you'll need to manually adjust hardcoded values OR
just live with the dev-machine defaults (slow but functional).

**Workaround for RAM-constrained dev machine:** reduce dataset days,
bump `poll_interval` (fewer feature collection runs), reduce
`label_forward_ticks` (smaller label compute per sample).

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

**Tracking:** `v5.10-design-notes.md` — multi-symbol candidate.

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

### Backtest hot-loop perf is dev-machine-tuned

89 minutes for 895M ticks (~167K ticks/sec) on a 4-core dev box. ML
pipeline itself is <1 minute of that — cost is dominated by CSV
parsing + 4-core hot-path simulation + slow-path feature collection.
Optimization candidates (parallel CSV load, sparse label buffer, SIMD
RegimeSignals) tracked as `v5.10-design-notes.md` Idea #15+.

For now: reduce dataset days for iteration loops; full 365-day
training is a one-shot that you tolerate.

### Label buffer OOMs on 2+ year datasets (HIGH priority for v5.10)

**Observed 2026-05-03:** operator attempted 2-year feature collection
on 30.9 GiB RAM box → OOM crash during feature collection.

**Math:**
- 1 year (~895M ticks) → label buffer 28.6 GB
- 2 years (~1.8B ticks) → label buffer ~57 GB → exceeds RAM
- Allocation pattern: per-tick label slot (32 bytes), but only
  sample-point labels (every poll_interval ticks) are actually used

**Root cause:** label buffer sized at full tick granularity. With
`poll_interval=100`, 99 of every 100 buffer slots are unused. Should
be sized by `sample_count` not `tick_count` (100x reduction →
57GB shrinks to 570MB).

**Workarounds today:**
- Reduce dataset to ≤ 1 year for iteration loops
- Bump `poll_interval` to 200+ (halves buffer + halves compute)
- Wait for v5.10 sparse-buffer fix

**Tracking:** `v5.10-design-notes.md` Idea #15 (sparse label buffer).
HIGH priority — real crash, not theoretical.

---

## Deferred features (planned, not shipped)

These were explicitly scoped out of Sprint A and tracked for v5.10+.
If you wanted these and they're not there, that's why:

- **Per-class accuracy display** (#8) — multiclass WF results would
  show per-class TP/FP. Display polish; v5.10 candidate.
- **ConfidenceScorer extended snapshot tests** (#9) — formula
  regression protection. Internal; existing 4 tests cover the formula.
- **Dedicated Stamps Inspection panel** (#14 full) — v5.9.5i shipped
  the **filter inside Past Runs** instead of a new panel. Dedicated
  panel is v5.10 polish if operator demand surfaces.
- **Operator-tunable XGBoost `nthread` via cfg** — currently
  hardcoded (4 for Train Model, 1 for WF/HeldOut). Hardware-aware
  cfg is v5.10 Idea #14.
- **Multi-symbol** — see "BTC-only" above.
- **FPN-end-to-end** — see "Cross-build" above.
- **Hot model swap** — engine reloads on cfg-change without restart.
  v5.10.0c.
- **ML pipeline performance optimizations** — parallel CSV load,
  sparse label buffer (28GB → 286MB), SIMD RegimeSignals. v5.10
  Idea #15+.

Full list: `DOCS/v5.10-design-notes.md` +
`plans/2026-05-08-v5.11-deferred-items.md`.

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
