# Parity Lifecycle

**Purpose:** operator-facing reference for "I'm modifying X — what do
I need to do, and what do I need to retrain?" Answers the questions
that surface mid-edit when changing anything in the ML pipeline.

If you're trying to ADD a new thing (feature, label, strategy, etc.),
read `DOCS/EASY_ADDITIONS_INVARIANTS.md` and the relevant interface
doc (`FEATURE_INTERFACE.md`, `TARGET_INTERFACE.md`, etc.).

If you're trying to verify your change post-implementation, read
`DOCS/PARITY_VERIFICATION_CHECKLIST.md` for the generic ML/live
verification pattern.

This doc is the WHAT-TO-DO map across all parity-relevant surfaces.

## The parity surface map

The codebase has these surfaces where train-time and serve-time
artifacts must agree. Each row is a different thing you might
modify; columns capture what changes flip what / requires retrain /
requires snapshot update.

| Surface | What changes flip the contract | Hash protection | Snapshot test (v5.9.2a) | Retrain required? |
|---|---|---|---|---|
| **`FOREACH_FEATURE` row** (add/remove/reorder, version bump, enabled flag flip) | `FEATURE_REGISTRY_HASH` flips | YES (v5.8.6+) | Auto via Sub-area 1a | YES (existing models refuse to load) |
| **`ML_Compute_*` body** (formula change, fix sign error) | Output bytes change | NO (hash stable) | Sub-area 1a catches | YES if intentional shift; bump row's version field + retrain |
| **`Regime_ComputeSignals` body** (populate field differently) | Feature outputs change | NO | Sub-area 1a catches via dependency tree | YES if outputs change |
| **`RollingStats_Push` math** (windowing, EMA, SMA) | Feature outputs change | NO | Sub-area 1a catches via dependency tree | YES if outputs change |
| **`label_table[]` row** (add/remove/reorder) | No hash today (deferred to v5.10+ `FOREACH_TARGET`) | NO | Manual via 8 Label_* tests | Backtest comparisons not comparable; live unaffected |
| **`Label_*` body** (formula change, lookahead shift) | Training data changes; live unaffected | NO | Sub-area 3 catches | Research-integrity only; live unaffected |
| **`MODEL_FORMAT_VERSION` bump** (currently 5; would be 6+ for a future wire-format change) | Engine refuses old format unless backward-compat field handling | YES (stamp body version field) | None — version bumps are deliberate | YES if hash flips OR backward-compat field added |
| **Stamp body field add** (e.g. `feature_scaler_present`) | New field parsed; absent → backward-compat default | Forward-compat parser tolerates unknown keys | v5.8.8 round-trip catches | NO — new field optional; existing models load with default |
| **Scaler sidecar `.scaler` content** (mean/stddev change) | SHA-256 in stamp differs | YES (`scaler_sha256` in stamp + sidecar's own registry hash) | None today (v5.9.3 will add) | Implicit — sidecar pinned to model |
| **`SCALER_STDDEV_FLOOR`** (constexpr default) | Apply-time math changes | YES (Q32 floor embedded in sidecar per v5.9.3 design) | None | NO — persisted floor pins per-model |
| **Strategy `_BuildParameters` body** | Gate output changes | NO | Sub-area 2b (currently SimpleDip only) | Live behavior changes; document in CHANGELOG |
| **`Confidence_Compute` formula** | Threshold damping changes | NO | Sub-area 2a catches | Behavior change; document in CHANGELOG |
| **Cfg field add (inference-affecting)** | Behavior changes if operator sets the new field | v5.9.2b stamp-binds (planned) | None | Depends on default; if default matches existing behavior, no |
| **Cfg `confidence_freshness_tau`** | Confidence damping shifts | v5.9.1 + v5.9.2b: tau range-clamped + stamp-bound | None | NO if within range; YES if intentional shift |
| **Cfg `held_out_fraction`** | Held-out test set boundary changes | v5.9.2b will stamp-bind | None | YES — different held-out → different metric |
| **Cfg `csv_sort_check_mode`** | Backtest input filtering only | v5.9.2c: validation mode | None — operational | NO — runtime-only, training-time only |
| **`SHARDED_SNAPSHOT_VERSION` bump** | Engine refuses old snapshots | YES (snapshot version field) | None | Operator clears snapshot dir on upgrade |
| **Build flag** (e.g. `-DUSE_NATIVE_128`) | FPN bit-representation changes | NO (not in stamp today) | None | YES if flag changes; document; future stamp may bind |

## Common scenarios

### Scenario: I'm fixing a bug in `ML_Compute_VwapDev`

The bug fix changes the formula. Output bytes will differ for
non-trivial inputs.

1. Make the fix.
2. Run `./build.sh test`. The v5.9.2a snapshot test for FEATURE_VWAP_DEV
   will fail.
3. Update the recorded snapshot value in `controller_test.cpp` to the
   new correct output.
4. Bump `FOREACH_FEATURE(...)` row's `version` field for `VWAP_DEV`
   from `1` to `2`. This flips `FEATURE_REGISTRY_HASH`.
5. Existing models trained against version-1 vwap_dev will refuse to
   load (strict mode) or warn-load (non-strict). Operator must retrain.
6. CHANGELOG: "v5.X.Y fixed VwapDev formula; FEATURE_VWAP_DEV version
   bumped 1→2; retrain required."

### Scenario: I'm changing `Regime_ComputeSignals` to compute `ema_sma_spread` differently

Same thing — output bytes change, snapshot fails, version bump
required for any feature that reads from `signals->ema_sma_spread`.
Look at FOREACH_FEATURE rows that pass the value through (FEATURE_EMA_SMA_SPREAD
in this case) and bump that version.

If multiple features depend on the changed signals field, bump each
of their versions individually. Each version field affects the
registry hash; bumping all dependent features documents the
relationship.

### Scenario: I'm adding a new feature

See `DOCS/FEATURE_INTERFACE.md` recipe. Adding a new feature flips
the registry hash automatically (X-macro adds a row).

### Scenario: I'm changing a label semantics

See `DOCS/TARGET_INTERFACE.md` snapshot-test discipline section.
Lower urgency — labels are training-time only.

### Scenario: I'm changing a cfg default

Default flips are BREAKING-CHANGE candidates. Plan must justify the
flip; CHANGELOG must mark it. v5.9.2b will stamp-bind
inference-affecting cfg fields so operators can detect drift between
trained-model cfg and serve-time cfg.

For non-inference cfg (operational settings, GUI prefs, log paths),
default flips are normal — no parity concern.

### Scenario: I'm bumping `MODEL_FORMAT_VERSION`

`MODEL_FORMAT_VERSION` versions the model FILE shape. Most stamp body
schema changes do NOT need a bump — they can be added as optional
forward-compat fields with `has_*` flags (the v5.8.6
`feature_registry_hash` pattern, repeated in v5.9.2b for
`inference_cfg_*` and v5.9.3a for `feature_scaler_present` +
`scaler_sha256`). Only bump when the model file's serialization
itself changes.

If a bump is genuinely needed:

1. Bump constant.
2. Verifier (`verify_model_stamp`) must handle BOTH old and new
   formats. Old: assume new fields absent → defaults. New: parse +
   verify.
3. Trainer (`stamp_write_for_model` + `tools/stamp_model.sh`)
   updates to emit new fields when format >= bumped version.
4. Add v5.8.8-style round-trip test for new fields.
5. CHANGELOG: list the new fields + backward-compat behavior.

**v5.9.3 specifically:** scaler sidecar landed WITHOUT a format bump.
Stamp body gains optional fields (`feature_scaler_present`,
`scaler_sha256`) parsed forward-compat by older verifiers; legacy
v5.x stamps load with `has_scaler_fields=0` + identity scaler. The
sidecar binary itself has its OWN magic + registry hash + body SHA;
binding to the model is via stamp's `scaler_sha256` field.

## What NOT to change without strong justification

- `FEATURE_REGISTRY_HASH` constant or computation method —
  fundamental contract; changing it forces all models to retrain.
- `SHARDED_SNAPSHOT_VERSION` — affects operator's persisted state.
- Hot-path function signatures (`BG_Evaluate`, `SG_Evaluate`) — see
  `DOCS/CLAUDE_INVARIANTS.md` for the discipline.

## Cross-references

- `DOCS/CLAUDE_ML_INVARIANTS.md` — load-bearing rules for ML
  pipeline; this doc is the operator-facing reference for those rules
- `DOCS/CLAUDE_INVARIANTS.md` — codebase-wide invariants; train-serve
  handoff verification rule applies to scaler + future artifacts
- `DOCS/PARITY_VERIFICATION_CHECKLIST.md` — generic per-surface
  ML-side / live-side / cross-check pattern for verifying changes
  post-implementation
- `DOCS/FEATURE_INTERFACE.md` / `DOCS/TARGET_INTERFACE.md` — surface-
  specific recipes
- `DOCS/EASY_ADDITIONS_INVARIANTS.md` — X-macro pattern + when to use
- `tests/controller_test.cpp` v5.9.2a EXTENSIBILITY block — the
  snapshot tests this doc references

## Updates

This doc is updated as new parity surfaces are added (e.g. v5.9.3
adds the scaler row; v5.10 may add `FOREACH_TARGET`). Each addition
documents: what changes flip the contract, hash protection
available, snapshot test coverage, retrain requirement.
