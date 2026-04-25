# 2026-04-25 (afternoon) — Hardening pass: dust + min_warmup clamp + multiclass weights

Branch: `experiment/phase5-zoo`. Continuing from `8d175b1` (label-type-aware
metrics part 2). Rollback tag `pre-hardening` set before this work began.

Three small but real bugs surfaced during the morning's audit got fixed here.
Each is its own commit so individual rollback is cheap.

---

## c95ef3f — gate-reason `gr[]` OOB read + unwedge REJECT_REASON defines

Two latent dust issues in `PortfolioController.hpp`. Both inert under typical
config but flagged for cleanup in the audit.

### `gr[]` array stuck at 14 entries when `NUM_GATE_REASONS=15`

The local `static const char *gr[]` in the gate-transition log path
(`PortfolioController.hpp:1675`) was added before `GATE_REASON_BARRIER=14`
existed and never extended when that constant was added. Bounds check guarded
against `gate_reason >= 15` but permitted `14`, so a BARRIER transition would
have read past the array end.

`GATE_REASON_BARRIER` doesn't fire under typical configs (requires
`barrier_gate_enabled=1` AND a loaded barrier model), so the bug was
reachable but not currently fired. Latent OOB read regardless.

**Fix:** replaced with `GATE_REASON_TABLE[].name` — single source of truth
already used by `TUIAnsi`, `BacktestEngine` gate breakdown, and
`DashboardPanels`. Future `GATE_REASON_*` additions automatically propagate
without needing to update this site.

### REJECT_REASON_* defines wedged inside GATE_REASON_*

The `c99ec57` centralization commit inserted the new `REJECT_REASON_*`
defines + `REJECT_REASON_NAMES` table *between* `GATE_REASON_COOLDOWN=9`
(line 65) and `GATE_REASON_WIND_DOWN=10` (line 81). Inert (different
namespaces, no symbol collision) but visually confusing — at a glance it
looked like `REJECT_REASON_*` were part of the `GATE_REASON_*` sequence.

**Fix:** moved `REJECT_REASON_*` block + name table below the complete
`GATE_REASON_*` block.

---

## c6aa0cc — clamp `min_warmup_samples` at config load

This is the field that bit us hard Friday night. We spent multiple hours
debugging "why won't Collect Features run" before tracing it to
`min_warmup_samples=512` in the cfg.

### The bug

`min_warmup_samples` semantically means "min rolling stats samples in the
short rolling window before warmup completes." It gates on `rolling.count`,
which is bounded by the rolling window size `W=128`. Setting the config
above 128 means warmup never completes — engine sits in WARMUP forever, no
trades, no features collected, no error logged.

The field name implied "raw ticks" to the user (us), so we kept editing it
thinking it was the primary warmup gate, when `warmup_ticks` is the actual
primary gate.

### Fix

Clamp at `ControllerConfig_Load`. Values above 128 trigger a clear warning
explaining what the field actually does and suggesting `warmup_ticks` for
the "longer raw-tick warmup" use case:

```
[CFG] WARNING: min_warmup_samples=512 exceeds rolling window size 128 and
      would cause warmup to never complete. Clamped to 128.
      If you want a longer total-tick warmup, use warmup_ticks instead
      (counts raw ticks, no upper bound).
```

The struct field comment was also updated to be explicit about the cap.

This is the conservative version of the Phase 5d Wave 2 commit 2.6.1 plan.
The field semantics are preserved (it really IS about rolling window fill);
just the user-hostile silent-fail mode is gone. A proper rename to
`min_rolling_samples` (commit 2.6.2 in the plan) is deferred — would touch
the parser + cfg files + docs; the clamp + warning is the load-bearing fix.

---

## 38ab41d — multiclass per-sample inverse-frequency weights for class balance

`scale_pos_weight` is binary-only. For multiclass softmax (`REGIME`,
`PEAK_VALLEY_STABLE`), the equivalent compensation is per-sample weights
applied via `XGDMatrixSetFloatInfo("weight", ...)`. Without this, multiclass
with skewed class distribution (typical for `PEAK_VALLEY_STABLE` on
tick-scale BTC: ~95% stable / 4% peak / 1% valley) trains a model that
trivially predicts the majority class for high accuracy but zero predictive
value for the minority classes — same failure mode as binary class imbalance
without `scale_pos_weight`.

### Inverse-frequency formula

```
weight[i] = total_samples / (K * count[label[i]])
```

Each class contributes equally to the loss regardless of frequency. Class
with 95% of samples gets weight ~0.21 per sample; class with 1% gets weight
~33.0 per sample.

### Where it's wired

New helper `XGBoost_ComputeMulticlassWeights` in `BacktestEngine.hpp`,
sibling of the existing `XGBoost_ComputeScalePosWeight`. Wired into both:

- **Train Model** path (`BacktestPanels.hpp` ~line 1210): called inside the
  `is_multiclass` branch after `dtrain` is created. Logs per-class counts
  + percentages so user sees the distribution.
- **Walk-Forward** path (`BacktestEngine.hpp` ~line 1170): called per fold.
  Train and test splits can have wildly different class ratios, so weights
  are recomputed per-fold from the actual fold's `train_labels`.

Regression case unchanged (no class concept). Binary case unchanged
(continues to use `scale_pos_weight`, the more efficient single-param
formulation for two classes).

---

## Anti-drift verification

- [x] `ML_Headers/ModelInference.hpp::ModelFeatures_Pack` UNCHANGED
- [x] `ML_Headers/RollingStats.hpp::RollingStats_Push` UNCHANGED
- [x] `CoreFrameworks/ExecutionCore.hpp::ExecutionCore_Tick` UNCHANGED
- [x] `FEAT_*` constants UNCHANGED
- [x] `controller_test` 279/279 passing after each commit
- [x] All 3 targets build clean (engine, engine_gui, foxml_suite) after each commit

## Rollback positions

```bash
git reset --hard pre-hardening      # back to 8d175b1 (before this batch)
git reset --hard pre-label-type-fix # back to 2b27707 (Saturday evening)
git reset --hard pre-zoo            # back to 46b5a25 (before all Phase 5)
```

Each commit in this batch is its own rollback target via `HEAD~1`, `HEAD~2`,
`HEAD~3` — they're individually revertible.

## What's next (deferred to future sessions)

From the Phase 5d plan, still queued:

- **Wave 1** (~30 min, 5 commits): pre-flight data validation, gap detection,
  sharded-mode warning, class distribution surfacing at collection time,
  `backtest.cfg` symlink consolidation
- **Wave 2** (~45 min, 3 commits): `expected.cfg` field expansion,
  full `ControllerConfig_Validate`, cancelled-vs-complete distinction,
  cancel-during-load
- **Wave 3** (~20 min, 2 commits): `static_assert` for FEAT_* order/count,
  settings panel disabled-while-running
- **Wave 4** (~30 min, 3 commits): run name auto-suffix, settings header
  label, comparison panel guard

The hardening batch shipped here addressed the highest-value latent bugs
that were *known to be wrong*. Wave 1-4 are preventive hardening that catch
classes of misuse before they become real bugs.
