# Parity Verification Checklist

**Purpose:** generic operational checklist for "I just changed X — how
do I verify ML-side and live-side stay 1:1?" Reused across v5.9.X
subships and any future ML-touching change.

If you're trying to figure out WHAT to do when changing X, read
`DOCS/PARITY_LIFECYCLE.md` first. This doc is what to verify AFTER.

Each section below covers a specific parity surface. Pick the
sections that match what you changed; ignore the rest.

## Generic verification pattern

For every change, run all three:

1. **ML-side check** — exercise the change via foxml_suite or a
   backtest, observe the artifact / output / log.
2. **Live-side check** — exercise the change via paper engine
   (`./bin/engine_gui` against live ticks or replay), observe the
   equivalent surface.
3. **Cross-check (1:1)** — same inputs through both paths produce
   matching outputs, within documented tolerance.

If any of the three fails, the change has a parity gap. Fix or
document before merging.

## Per-surface checklists

### Surface A: Feature compute fn (`ML_Compute_*`) body changed

**ML-side:**
- [ ] `./build.sh test` passes — the v5.9.2a Sub-area 1a snapshot
      test catches output changes
- [ ] If snapshot failed, decision made: bytewise-equivalent
      refactor OR intentional shift + version bump
- [ ] If version bumped, `FOREACH_FEATURE` row's version field
      reflects the bump
- [ ] CHANGELOG entry written

**Live-side (paper engine, ≥4h soak):**
- [ ] Boot engine with existing model — engine refuses to load
      (strict mode) OR warns + loads with current build's hash mismatch
- [ ] If loaded, predictions reflect new feature semantics
- [ ] No CRITICAL log lines from unexpected code paths

**Cross-check:**
- [ ] Backtest run with NEW model + NEW build → predictions match
      live engine on same tick stream
- [ ] Backtest with OLD model on NEW build → strict refuses; non-
      strict warns + uses identity (intentional)

### Surface B: Label `Label_*` body changed

**ML-side:**
- [ ] `./build.sh test` passes — Sub-area 3 catches output changes
- [ ] If failed, decision made + CHANGELOG entry

**Live-side:**
- [ ] N/A — labels are training-time only; live engine never sees
      labels

**Cross-check:**
- [ ] Backtest comparison: run on same data with old vs new label
      semantics → metrics differ as expected
- [ ] Document: "training runs before/after this change are not
      directly comparable"

### Surface C: Strategy `_BuildParameters` body changed

**ML-side:**
- [ ] `./build.sh test` passes — Sub-area 2b catches the change for
      the modified strategy (currently SimpleDip; MR/Momentum/EmaCross
      deferred to v5.9.4)
- [ ] If failed, decision made

**Live-side (paper engine, ≥4h soak):**
- [ ] Boot engine with the strategy active — gate decisions reflect
      new behavior
- [ ] PerCoreSnap shows expected `bg_price_threshold`,
      `bg_volume_threshold`, etc.
- [ ] No unexpected `strategy_halt_reason` codes

**Cross-check:**
- [ ] Same tick stream through backtest + live → identical entry
      decisions at the same tick boundaries

### Surface D: `Confidence_Compute` formula changed

**ML-side:**
- [ ] `./build.sh test` — Sub-area 2a catches output changes
- [ ] CHANGELOG entry: "confidence threshold damping behavior
      changed"

**Live-side (paper engine, ≥4h soak):**
- [ ] ML Status panel shows new `ml_confidence_ic` / `ml_confidence_rmse`
      / `ml_last_effective_threshold` values
- [ ] Threshold damping curve reflects new formula

**Cross-check:**
- [ ] Backtest's `effective_threshold` log column matches live
      engine's `last_ml_effective_threshold` for matching IC + RMSE +
      data_age inputs

### Surface E: Cfg field added (inference-affecting)

**ML-side:**
- [ ] Stamp body emits the new field (v5.9.2b+)
- [ ] Verifier accepts on match, refuses (strict) / warns (non-strict)
      on mismatch

**Live-side (paper engine, ≥4h soak):**
- [ ] Boot with cfg matching stamp → load OK, no WARN
- [ ] Boot with cfg DIFFERENT from stamp → strict-mode REFUSE OR
      non-strict WARN with `inference_cfg_drift_count` surfaced

**Cross-check:**
- [ ] Stamp body has the new field
- [ ] CHANGELOG: "v5.X.Y stamp-binds `<cfg_field>`; trained models
      are pinned to their training-time value of this cfg"

### Surface F: Scaler sidecar (`<model>.scaler`) introduced (v5.9.3+)

**ML-side:**
- [ ] Train Model produces `.scaler` sidecar
- [ ] Sidecar SHA-256 matches stamp's `scaler_sha256`
- [ ] Sidecar's embedded `feature_registry_hash` matches build's
      `FEATURE_REGISTRY_HASH()`
- [ ] Stamp body has `feature_scaler_present=1`
- [ ] Stddev floor in sidecar reflects `SCALER_STDDEV_FLOOR`

**Live-side (paper engine, ≥24h MANDATORY soak):**
- [ ] Boot engine with new model + sidecar — ML Status panel shows
      "scaler: applied (registry_hash=...)"
- [ ] Entry log shows post-scaler features (mean ~0, stddev ~1)
- [ ] Deliberately remove sidecar + boot strict-mode → REFUSE,
      CRITICAL log
- [ ] Deliberately remove sidecar + boot non-strict → WARN, identity
      applied, ML Status panel shows red "scaler: WARN — sidecar missing"

**Cross-check:**
- [ ] Same model + same tick replay → backtest's post-scaler
      `feature_matrix[i][j]` BYTEWISE MATCHES live engine's
      standardized features at the equivalent tick boundary
- [ ] Predictions match
- [ ] Decisions match

### Surface G: `MODEL_FORMAT_VERSION` bumped

**ML-side:**
- [ ] Verifier handles old and new format stamps
- [ ] Trainer emits new format (in-process + bash script in parity)
- [ ] v5.8.8-style round-trip test passes for new fields

**Live-side:**
- [ ] Boot with old-format model → backward-compat path (warn-load
      with new field defaults)
- [ ] Boot with new-format model → all new fields verified

**Cross-check:**
- [ ] Stamp body new fields present + signed correctly
- [ ] HMAC signature still validates after new fields added

### Surface H: Build flag changed (`-DUSE_NATIVE_128`, etc.)

**ML-side:**
- [ ] Same source compiled with old flag → snapshot tests pass
- [ ] Recompile with new flag → snapshot tests pass

**Live-side:**
- [ ] Bin compiled with new flag set boots clean
- [ ] No FPN-related test failures from precision differences

**Cross-check:**
- [ ] Operator discipline: training and deployment binaries built
      with IDENTICAL flag set
- [ ] CI must produce one authoritative binary tree used for both
      backtest + live engine

## Generic exit criteria for any ship

Before tagging a v5.9.X subship that touches the ML pipeline:

- [ ] All applicable surface checklists above ticked
- [ ] `./build.sh test gui suite` clean
- [ ] `./tools/calls_graph_diff.sh` CLEAN
- [ ] Hot path UNTOUCHED (verified via `git diff --stat` excludes
      `CoreFrameworks/ExecutionCore.hpp`, `Strategies/StrategyParameters.hpp`
      BG/SG inner block, `MemHeaders/PoolAllocator.hpp`)
- [ ] Per-subship paper soak completed at the duration the plan
      specifies (typically ≥4h for low-risk, ≥12h for medium, ≥24h
      for high-risk like scaler activation)
- [ ] Pre-tag (`pre-vX.Y.Z`) created for rollback
- [ ] CHANGELOG entry written
- [ ] DOCS/CHANGELOG.md table updated (top-level summary)

## When cross-check fails

If ML-side and live-side diverge unexpectedly:

1. **Capture the divergence point.** Identify the smallest input
   where outputs differ.
2. **Bisect the dependency tree.** Walk: cfg → tick consumption →
   rolling stats → Regime_ComputeSignals → Features_PackAll →
   (scaler, v5.9.3+) → Model_Predict → strategy gate.
3. **Each step is parity-checkable.** Use the v5.9.2 determinism
   test + v5.9.2a snapshot tests + the `parity_harness.cpp` binary
   as primitives.
4. **Documented architectural divergence is OK.** Live's threading
   observation timing vs backtest's serial processing produces
   sub-tick differences for some features (see audit Tier 5).
   Document but don't fix.
5. **Silent unexplained divergence is NOT OK.** Stop, isolate, root-
   cause. Don't ship until understood.

## Cross-references

- `DOCS/PARITY_LIFECYCLE.md` — what each change requires (read first)
- `DOCS/CLAUDE_ML_INVARIANTS.md` — load-bearing ML rules
- `DOCS/CLAUDE_INVARIANTS.md` — codebase-wide invariants
- `tests/parity_harness.cpp` — offline binary for legacy ↔ sharded
  feature_matrix diff
- `tests/controller_test.cpp` v5.9.2/.2a/.2c blocks — snapshot +
  determinism tests

## Update discipline

This doc updates as new parity surfaces are added. Each addition
adds a Surface section above with concrete ML-side / live-side /
cross-check items. Generic enough to reuse across v5.10+ sprints
without rewrite.
