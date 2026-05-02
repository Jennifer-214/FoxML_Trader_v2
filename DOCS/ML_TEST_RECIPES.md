# ML Test Recipes

**Purpose:** operator-facing end-to-end workflows for the v5.9 ML pipeline.
Each recipe is a step-by-step ritual that produces a known outcome.
Reference these when paper-testing a new model, rolling back a bad
deploy, rotating secrets, or diagnosing a paper-test surprise.

If you're trying to UNDERSTAND what surfaces are parity-bound, read
`DOCS/PARITY_LIFECYCLE.md` first. This doc is HOW-TO; that doc is WHAT.

If you're verifying a change post-implementation, read
`DOCS/PARITY_VERIFICATION_CHECKLIST.md` for the per-surface ML/live/
cross-check pattern.

---

## Recipe 1: Production deploy pre-flight (10-item checklist)

Before promoting a paper-tested model + cfg combination to live trading,
walk this checklist. Every item must be checkable on the actual model
files + engine binary you're about to deploy. Not the "training run
that produced them last week."

### Pre-flight

1. **Model files match cfg paths.** `cfg.core_model_dir[i]` (or
   `cfg.core_model_path[i]` for legacy) points at the actual `.bin`
   file you intend to deploy. Run `ls -la <model_dir>` and confirm.

2. **Stamp present + valid.** `<model>.stamp` exists, parseable by
   `verify_model_stamp`. Spot-check via:
   ```bash
   grep "model_format_version\|gap\|engine_version" <model>.stamp
   ```
   Stamp should show your training run's metrics.

3. **`feature_registry_hash` matches build.** The stamp's hash and
   the build's `FEATURE_REGISTRY_HASH()` MUST agree. Boot the engine
   in paper mode briefly; engine logs:
   ```
   [model] X.bin: trained_engine=5.9.3c registry=fc9119b8ed47bcf9
                  (current=5.9.3c/fc9119b8ed47bcf9) — ok
   ```
   If "registry=" mismatch with "current=" → REFUSE deploy. Recompile
   the engine with the same `FOREACH_FEATURE` set the model trained on.

4. **`engine_version` cross-major OK.** Stamp's engine version major
   matches build's major. Boot log will fire WARN if cross-major;
   refuse unless `acknowledge_cross_binary_version_drift=1` is
   deliberately set + understood.

5. **Scaler sidecar present + bound (post-v5.9.3).** If stamp claims
   `feature_scaler_present=1`:
   - `<model>.scaler` exists at sibling path
   - `sha256sum <model>.scaler` matches stamp's `scaler_sha256` line
   - Boot log emits `[scaler] X.scaler: loaded (registry_hash=...,
     num_features=N)`
   - ML Status panel shows green "scaler: applied"

6. **`held_out_gate_strict=1` in production cfg.** This is the safety
   net. Anything else (=0 warn-mode) lets silent drifts ship to live.
   Production cfgs should always set this.

7. **`held_out_stamp_secret` non-empty + matches training secret.**
   Empty secret means dev-mode (engine accepts any signature). Live
   trading must verify HMAC. The secret used at training time MUST
   equal the secret used at boot time.

8. **Cfg fields stamp-bound match deploy cfg.** The stamp body
   captures (when set at training): `confidence_threshold_scale`,
   `barrier_gate_enabled`, `confidence_hard_block_threshold`,
   `held_out_fraction`, `confidence_freshness_tau`, plus
   `bandit_blend_ratio` + fee rates when the relevant flags were
   on. Mismatched cfg values trigger drift warnings (or refusal in
   strict mode).

9. **Paper-tested ≥24h on the SAME cfg shape.** Run paper engine
   for at least one full day with the EXACT cfg + model bundle.
   Watch for CRITICAL log lines, ML Status panel state changes,
   ml_nan_feature_events / ml_nan_prediction_events counters, and
   any `scaler: WARN — load failed` red-state events.

10. **CHANGELOG entry committed.** Record the deploy: model path,
    cfg path, training date, paper-test soak duration, expected
    behavior. If you have to roll back later, this is your
    breadcrumb.

### Go signal

All 10 items checked → safe to flip live cfg + restart engine.
Any item failing → diagnose first.

---

## Recipe 2: Model rollback (8-step manual recovery)

Symptom: live engine started behaving badly after a model deploy.
You need to revert to the previous-known-good model + stamp + (post-
v5.9.3) sidecar bundle. Without losing positions or P&L history.

### Steps

1. **Don't panic; halt new entries first.** Set `cfg.kill_switch=1`
   in the live cfg. Engine continues to manage open positions but
   refuses new entries. (This is the v5.4.x kill-switch surface.)

2. **Snapshot current state.** Engine writes
   `data/sharded_snapshot.dat` automatically every N ticks; verify
   the latest snapshot is recent (`ls -la data/sharded_snapshot.dat`).

3. **Identify the last-known-good model bundle.** Look in your
   model directory for the previous deploy's files. They're
   typically retained as `models/<role>/<date>/` or similar. The
   files you need:
   - `<model>.bin` (the model itself)
   - `<model>.stamp` (HMAC-signed metadata)
   - `<model>.scaler` (post-v5.9.3, if applicable)

4. **Verify the rollback bundle's stamp.** Run the verify pass
   manually before swapping:
   ```bash
   # Read the stamp and grep for sanity
   cat <rollback_model>.stamp
   # Confirm the wf_mean_val + held_out_metric were good
   # Confirm feature_registry_hash matches your current build
   ```

5. **Stop the engine cleanly.** Kill via SIGTERM (let it write the
   final snapshot + close any pending order ops cleanly). Do NOT
   SIGKILL — partial state on disk = harder recovery.

6. **Swap the model bundle.** Move/symlink the rollback bundle
   into the cfg's model path. Keep the bad bundle around for
   postmortem (rename `<bad>.bin.failed`, etc.).

7. **Boot in paper mode for 30 minutes.** Even though you trust
   the rollback bundle, verify boot logs are clean:
   - No `[CRITICAL]` lines
   - No `model_load_failed` or `scaler_load_failed`
   - Predictions resume sane values in entry log

8. **Flip live + monitor.** Live with `kill_switch=0` cfg. First
   hour: watch ML Status panel + entry log for any red flags. Open
   positions retained (snapshot loaded successfully); new entries
   fire on the rollback model.

### Anti-pattern

Don't rebuild the engine binary mid-rollback. The rollback's stamp
is bound to a specific `engine_version` major and
`FEATURE_REGISTRY_HASH`. If you recompile, you may flip the hash
and the rollback bundle becomes invalid. Use the binary you've
been running with.

---

## Recipe 3: Retraining cadence (5 trigger conditions)

When to retrain (and when to leave the model alone):

### Triggers requiring retrain

1. **Walk-forward gap ≥ acceptable threshold** (cfg
   `gap_acceptable_threshold`, default 0.05). The model's
   in-sample WF mean differs from the held-out test by more than
   the threshold. v5.8.6+ stamps refuse load when held-out gate
   strict=1; you're getting refused for a reason.

2. **`FOREACH_FEATURE` change.** Adding/removing/reordering ML
   features flips `FEATURE_REGISTRY_HASH`; existing models will
   refuse to load. Mandatory.

3. **`label_table[]` change.** Adding/removing/changing label
   semantics. Old models trained on old labels score nonsense
   against new labels. Mandatory.

4. **Market regime shift.** Periodic check (~weekly): is the
   model's prediction distribution shifting? Use the ML Status
   panel's `ml_confidence_ic` + `ml_confidence_rmse` rolling
   numbers. If IC drops below ~0.02 sustained for >24h,
   retrain.

5. **Feature compute fn body change.** Even without flipping
   `FEATURE_REGISTRY_HASH` (no X-macro change), if you fixed a
   bug in `ML_Compute_X` or `Regime_ComputeSignals`, the model's
   training-time features differ from serve-time features.
   v5.9.2a snapshot tests catch this. Mandatory after the test
   updates.

### Triggers NOT requiring retrain

- Cfg threshold tweaks (e.g. `confidence_threshold_scale` 2.0 →
  2.5). Stamp-bound (post-v5.9.2b) but operator can opt-out via
  `held_out_gate_strict=0`. Document why.
- Engine version patch bump (5.9.0a → 5.9.0b). Same major.minor
  always compatible.
- Cfg additions for non-inference fields (operational logging,
  GUI settings).

### Retraining workflow

See `DOCS/ML_TRAINING.md` "Scaler-aware training (v5.9.3+)"
section for the concrete steps.

---

## Recipe 4: Stamp secret rotation (5-step ceremony)

`held_out_stamp_secret` is the HMAC-SHA256 key bound to every stamp
written by your training pipeline. Rotate periodically (~yearly
in low-stakes shops, ~quarterly in regulated environments) or
immediately if you suspect compromise.

### Steps

1. **Generate the new secret.** `openssl rand -hex 32` produces a
   256-bit secret. Save it to your secret store (env var, vault,
   sealed file — wherever your operator practice keeps the old
   one).

2. **Re-stamp ALL deployed models with the new secret.** For each
   model in production:
   ```bash
   ./tools/stamp_model.sh \
       --model <model>.bin \
       --secret "$NEW_SECRET" \
       --wf-mean-val <from old stamp> \
       --held-out-metric <from old stamp> \
       --gap-threshold 0.05 \
       --feature-registry-hash <from old stamp> \
       --engine-version <current build's version> \
       --feature-scaler-present 1 \
       --scaler-sha256 <from sidecar's sha256sum>
   ```
   Each stamp file gets rewritten with the same metrics +
   different signature. The model file itself doesn't change.

3. **Verify new stamps in dev mode.** Set
   `held_out_stamp_secret=$NEW_SECRET` in a paper cfg. Boot
   engine; verify all models load with no signature failures.

4. **Atomic swap in production.** Update the production cfg's
   `held_out_stamp_secret` to the new value. Restart engine
   cleanly. Boot logs must show all models loading with the new
   signature verified.

5. **Destroy the old secret.** Once you've confirmed all models
   are running on the new secret, securely delete the old one
   from your secret store (and any operator notebooks that
   captured it).

### Anti-pattern

Don't run "during the ceremony" with the old engine still up
using the old secret. Stop the engine first; do the swap
atomically; restart. Otherwise an attacker who learned the old
secret could re-sign a malicious stamp during the rotation
window.

---

## Failure mode diagnosis

When something goes wrong at boot, check these surfaces in order:

| Symptom | Where to look | Likely cause |
|---|---|---|
| Engine refuses to boot | stderr (boot log) | `[held-out gate] REFUSING ...` → strict-mode caught a stamp issue. Read the `reason` field. |
| `model: LOAD FAILED` red in ML Status | `ml_model_load_failed` PerCoreSnap field; entry log | Model file missing / unreadable / strict-mode refused. Check stamp validity. |
| `scaler: WARN — load failed` red in ML Status | `ml_scaler_load_failed` PerCoreSnap field; CRITICAL log | Stamp claims scaler present, sidecar missing/corrupt. Either restore sidecar or rebind stamp without scaler. |
| `scaler: NONE (legacy v5 model)` sand | (no warn) | Model is pre-v5.9.3 or stamp deliberately omits scaler. Identity applied. Expected for legacy models. |
| ML predictions drift over time | `ml_confidence_ic` / `ml_confidence_rmse` counters | Market regime shift OR feature distribution shift. Retrain. |
| Cfg drift WARN at boot | stderr `[stamp] WARN ...` lines | A stamp-bound cfg field differs from current cfg. Either align cfg OR retrain with new cfg. |

---

## See also

- `DOCS/CLAUDE_ML_INVARIANTS.md` — load-bearing rules for the ML pipeline
- `DOCS/PARITY_LIFECYCLE.md` — operator-facing change matrix (what to do when modifying X)
- `DOCS/PARITY_VERIFICATION_CHECKLIST.md` — generic per-surface
  ML/live/cross-check pattern
- `DOCS/ML_TRAINING.md` — class-weight tuning + scaler-aware training
- `DOCS/V5_9_ML_HARDENING_AUDIT.md` — comprehensive audit findings
  (post-v5.8 paper testing)
- `tools/stamp_model.sh` — bash wrapper for in-process
  `stamp_write_for_model`
