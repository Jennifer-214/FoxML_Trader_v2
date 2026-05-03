# v5.9 ML Hardening — Audit Findings

**Date:** 2026-05-01 (closed 2026-05-02)
**Status:** CLOSED — all 27 findings addressed across v5.9.0 → v5.9.5.
See `DOCS/changelogs/2026-05-02-v5.9-ml-hardening.md` for the
sprint postmortem (Phase 6).
**Source:** automated audit (2026-05-01) + manual verification +
post-paper-test findings (2026-05-01 evening + 2026-05-02 morning).
**Closure tag:** `v5.9.5a` (2026-05-02; v5.9.5 + sprint-exit
build-warning hotfix). Sprint statistics: 18 tags, +225 tests
(1030 → 1255), hot path UNTOUCHED every ship, 1 calendar day
end-to-end. v5.9.5a fixed an off-by-8 stack overflow in
`FeatureStandardizer.hpp` Persist + Load paths (caught by
`-Wstringop-overflow` on `gui` + `suite` builds; missed by source-
read `/parity-check` agent run — methodology gap closed by adding
Section K to the skill).

This doc is the **binding registry** of issues v5.9 must address.
Every confirmed finding has a ship in `plans/2026-05-01-v5.9-ml-hardening.md`.
Future plans can reference findings by ID (e.g., "fixes V5_9_AUDIT-#4")
for traceability.

---

## Executive summary

**21 audit findings + 6 post-paper-test findings = 27 items reviewed.**

| Severity | Count | Status |
|---|---|---|
| CRITICAL | 0 | (1 false-positive verified + dropped on manual review) |
| HIGH | 10 | 4 audit + 6 paper-test — all addressed in Phase 1-2 |
| MEDIUM | 9 | Addressed in Phase 1-5 |
| LOW | 4 | Documentation / accept-and-document |
| Cross-cutting | 4 | Closed by single fixes (see Cross-cutting §) |

**Top 3 recommendations:**

1. **HIGH/Foundation:** Fold NaN guards INTO `Features_PackAll`
   (single source of truth, returns -1 sentinel) instead of validating
   at 5 caller sites. Mirrors the v5.8 single-source principle.

2. **HIGH/Visibility:** Surface ALL silent failure modes (model load
   failure, NaN feature, NaN prediction, cfg-source mismatch, all-default
   strategy assignments) via stderr WARN + GUI panel. Operator should
   never face a "why isn't this working?" without a corresponding visible
   signal.

3. **HIGH/Production-readiness:** Document the full production deploy
   ritual (transitioning from paper to `use_real_money=1`) as a binding
   recipe. Today's procedure is tribal knowledge; first production deploy
   = high risk of skipping a step.

---

## Findings by severity

### HIGH (correctness or operator-blind failure)

#### V5_9_AUDIT-#1 — NaN/Inf guards on feature pack output (audit-derived)
- **Symptom:** Division-by-zero in feature compute (e.g. `vwap_dev`
  when stddev=0) → NaN packs into feature array → XGBoost output
  undefined; gate decision `prediction > threshold` evaluates false
  on NaN, silent miss.
- **File:line:** `ML_Headers/FeatureRegistry.hpp:269-280` (Features_PackAll
  loop body); 5 caller sites at MLStrategy.hpp:120,
  StrategyParameters.hpp:665, BacktestSharded.hpp:448,
  PortfolioController.hpp:1634+1792.
- **Fix:** Fold validation INTO `Features_PackAll`; return -1 sentinel
  on NaN/Inf detected. 5 callers each get a `if (n < 0)` branch +
  health-log + counter bump.
- **Ship:** v5.9.0 Phase 1 item #1.
- **Effort:** ~1h (within Phase 1).

#### V5_9_AUDIT-#2 — Silent model-load failure (audit-derived)
- **Symptom:** When `held_out_gate_strict=0` (the default), a
  refused/missing model logs WARN to stderr but the live engine
  silently falls through to SimpleDip. Operator paper-soaks for
  hours not knowing ML never fired.
- **File:line:** `ML_Headers/CoreModelZoo.hpp:113-137`.
- **Fix:** Per-core `model_load_failed` flag on EventLoopState +
  TUISnapshot field + new "ML Status" dashboard panel +
  CRITICAL health log on each ML→SimpleDip fall-through cycle
  (rate-limited per-core to once-per-minute).
- **Ship:** v5.9.0 Phase 1 item #2.
- **Effort:** ~2h.

#### V5_9_AUDIT-#3 — ML entry health logs missing prediction + threshold (audit-derived)
- **Symptom:** Entry events log `strat=ml`, `slot=`, `price=` but
  not the model decision context (prediction value, threshold,
  confidence, registry hash). Post-mortem analysis blind.
- **File:line:** `CoreFrameworks/PortfolioController.hpp` entry log site
  (currently at ~1013).
- **Fix:** Plumb `MLBuildContext.out_prediction` + `out_confidence`
  into entry event. Add `prediction=`, `threshold=`, `confidence=`,
  `effective_threshold=`, `feature_registry_hash=` fields.
- **Ship:** v5.9.0 Phase 1 item #3.
- **Effort:** ~1h.

#### V5_9_AUDIT-#4 — Cfg-source confusion (post-paper-test 2026-05-01)
- **Symptom:** foxml_suite reads `backtest.cfg`, live engine reads
  `engine.cfg`. Operator edited engine.cfg (`core_N_strategy=mr`) but
  the suite ignored that file → cfg defaults applied (all DIP) → no
  ML inference fired in backtest. **The bug that surfaced this whole
  audit pass.**
- **File:line:** `Backtest/BacktestEngine.hpp` (cfg load), foxml_suite.cpp
  (boot path).
- **Fix:** (a) Engine header panel displays loaded cfg path. (b) Suite
  warns on legacy cfg (no per-core fields). (c) Cfg comparison panel
  diffs `backtest.cfg` vs `engine.cfg`.
- **Ship:** v5.9.0 Phase 1 item #4.
- **Effort:** ~1h.

#### V5_9_AUDIT-#5 — Default-vs-deliberate cfg (post-paper-test 2026-05-01)
- **Symptom:** `ControllerConfig_Default` sets `core_strategies[i]=2`
  (SIMPLE_DIP) for all 16 slots. If cfg has zero `core_N_strategy=`
  lines, defaults apply silently. The "0!"/"1!"/"2!" indicator in
  Per-Core P&L can't distinguish "deliberately hardcoded DIP" from
  "cfg field missing".
- **File:line:** `CoreFrameworks/ControllerConfig.hpp:847`.
- **Fix:** Track explicit-set bitmap per cfg field. TUI distinguishes
  "0!" (deliberate) vs "0?" (defaulted) vs "0" (auto-regime). Boot
  WARN on all-default `core_strategies[]`.
- **Ship:** v5.9.0 Phase 1 item #5.
- **Effort:** ~1.5h.

#### V5_9_AUDIT-#6 — Prediction NaN guard (post-paper-test 2026-05-01)
- **Symptom:** XGBoost can return NaN on poorly-trained models or
  pathological inputs. `prediction > threshold` evaluates false on
  NaN → no entry → silent miss.
- **File:line:** `Strategies/MLStrategy.hpp:120` and
  `Strategies/StrategyParameters.hpp:705` (call sites of
  Model_Predict / Model_PredictMulti).
- **Fix:** Same fold idiom as feature NaN. Validate prediction
  output, bump per-core counter, emit rate-limited critical log,
  fall through.
- **Ship:** v5.9.0 Phase 1 item #6.
- **Effort:** ~30min.

#### V5_9_AUDIT-#7 — Train Model GUI freeze (post-paper-test 2026-05-01)
- **Symptom:** "Train Model" runs synchronously; GUI freezes 5-30s.
  Comment in suite literally documents this. Bad UX.
- **File:line:** `Backtest/BacktestPanels.hpp` (Train Model button).
- **Fix:** Worker-thread pattern mirroring v5.8.7's
  `fullvalidation_worker_fn`.
- **Ship:** v5.9.0 Phase 1 item #7.
- **Effort:** ~1h.

#### V5_9_AUDIT-#8 — WF purge gap not validated post-generation (audit-derived)
- **Symptom:** `ValidationSplit_Generate` populates folds but no
  post-check that `train_end + purge_gap <= test_start`. Future bug
  could silently overlap train/val.
- **File:line:** `Backtest/ValidationSplit.hpp:126-200`.
- **Fix:** Post-gen assertion loop. Mark invalid folds, exclude
  from training.
- **Ship:** v5.9.0 Phase 1 item #8.
- **Effort:** ~30min.

#### V5_9_AUDIT-#9 — Class-weight handling for imbalanced labels (audit-derived)
- **Symptom:** User's 3-class barrier model: c0=4.2%, c1=48.3%, c2=47.5%.
  Without `sample_weight`, XGBoost overfits to majority. Predictions
  rarely say "stable" → BarrierGate fires too often.
- **File:line:** External training script (Python or wherever XGBoost
  is invoked). NOT in this codebase directly.
- **Fix:** Document in `DOCS/ML_TRAINING.md` (NEW, Phase 2). Operator
  must pass `sample_weight` array at training. v5.9 doesn't fix the
  training script (out of repo) but documents the requirement.
- **Ship:** v5.9.1 Phase 2 — docs only.
- **Effort:** ~1h docs.

#### V5_9_AUDIT-#10 — Confidence hard-floor missing (audit-derived)
- **Symptom:** ConfidenceScore DAMPENS the threshold but doesn't
  BLOCK at very-low confidence. Noisy predictions fire trades.
- **File:line:** `Strategies/StrategyParameters.hpp:731-737`.
- **Fix:** Hard floor in dispatcher: `if (conf < CONFIDENCE_HARD_BLOCK)
  Gate_Zero(out); strategy_halt_reason = SHALT_LOW_CONFIDENCE;`
  Cfg-driven floor (`confidence_hard_block_threshold`, default 0.05).
- **Ship:** v5.9.1 Phase 2 item #5.
- **Effort:** ~30min.

---

### MEDIUM (silent failure / observability gap)

#### V5_9_AUDIT-#11 — RollingStats warmup observability (audit-derived)
- **Symptom:** During warmup, features silently return zero. ML core
  silently falls through to SimpleDip until rolling.count >=
  min_warmup_samples. Operator doesn't know why ML isn't firing.
- **Fix:** Per-core `warmup_progress_pct` field on TUISnapshot. Boot-
  time stderr line per core when warmup completes.
- **Ship:** v5.9.1 Phase 2 item #1.
- **Effort:** ~1h.

#### V5_9_AUDIT-#12 — Label NaN per-type (audit-derived, refined post-readiness)
- **Symptom:** `Label_ForwardPnl` and others can produce NaN on
  degenerate inputs. Currently silent.
- **Fix:** Per-type neutral default — binary→0.5, regression→0.0,
  multiclass→skip-sample-via-NaN-sentinel. Counter on BacktestStats.
- **Ship:** v5.9.1 Phase 2 item #2.
- **Effort:** ~30min.

#### V5_9_AUDIT-#13 — ConfidenceScore tau=0 silent default (audit-derived)
- **Fix:** WARN log when tau<=0 + use default. Reject at cfg parse
  time (validate cfg fields).
- **Ship:** v5.9.1 Phase 2 item #3.
- **Effort:** ~30min.

#### V5_9_AUDIT-#14 — BarrierGate hardcoded constants (audit-derived)
- **Fix:** Move BARRIER_G_MIN, BARRIER_GAMMA, BARRIER_DELTA,
  BARRIER_HARD_BLOCK to ControllerConfig.hpp.
- **Ship:** v5.9.1 Phase 2 item #6 (optional in scope).
- **Effort:** ~1h.

#### V5_9_AUDIT-#15 — Stamp signature format-version (audit-derived)
- **Symptom:** Signature covers 7 fields. Adding a future field
  doesn't invalidate signature if attacker has stamp-write access.
- **Fix:** Add `stamp_format_version=1` field; bump on schema change;
  verifier rejects unknown version.
- **Ship:** v5.9.0 Phase 1 (defensive) — ~30min.

#### V5_9_AUDIT-#16 — Production deploy runbook missing (post-paper-test 2026-05-01)
- **Symptom:** Transitioning from paper to live with real money is
  tribal knowledge. Multi-step ritual not documented.
- **Fix:** New section in `DOCS/ML_TEST_RECIPES.md` covering the full
  pre-flight checklist (held_out_gate_strict=1, secret rotation, paper
  soak duration, kill switch verification, etc).
- **Ship:** v5.9.4 Phase 5 Output A.
- **Effort:** ~1h.

#### V5_9_AUDIT-#17 — Model rollback procedure missing (post-paper-test 2026-05-01)
- **Symptom:** If model goes bad in live, operator has no documented
  fast-rollback. Today: edit cfg + restart.
- **Fix:** Document the manual rollback procedure. Future enhancement
  (deferred to v5.10): hot-swap via `rollback_model_dir=` cfg.
- **Ship:** v5.9.4 Phase 5 Output A.
- **Effort:** ~30min.

#### V5_9_AUDIT-#18 — Retraining cadence not documented (post-paper-test 2026-05-01)
- **Symptom:** Markets drift. Stale models lose edge. No guidance on
  when to retrain.
- **Fix:** Recipe documenting triggers (gap drift, win-rate drop,
  regime shift, calendar quarterly).
- **Ship:** v5.9.4 Phase 5 Output A.
- **Effort:** ~30min.

#### V5_9_AUDIT-#19 — Health log rotation policy missing (post-paper-test 2026-05-01)
- **Symptom:** `logging/health.jsonl` grows unboundedly. Long-running
  engines fill disk.
- **Fix:** In-process rotation (cfg: `health_log_max_bytes=100MB`,
  `health_log_keep_count=7`). Atomic-rename on threshold.
- **Ship:** v5.9.4 Phase 5 Output C.
- **Effort:** ~1h.

---

### LOW (polish / accept-and-document)

#### V5_9_AUDIT-#20 — Held-out lock token entropy (audit-derived)
- Friction-grade by design; documented as "not cryptographic" in
  HeldOutSplit.hpp. Accept; no ship needed.

#### V5_9_AUDIT-#21 — Locale pinning thread-safety (audit-derived)
- Linux uselocale() is thread-local-safe. Engine is Linux-only per
  CLAUDE.md. Accept; no ship.

#### V5_9_AUDIT-#22 — Model_IsLoaded torn-read (audit-derived)
- No hot-swap supported today; race doesn't fire. Document; no ship.

#### V5_9_AUDIT-#23 — XGBoost random_state pinning (audit-derived)
- Training-script concern (Python), not C++ code. Document in ML_TRAINING.md.
- **Ship:** v5.9.1 Phase 2 docs.

#### V5_9_AUDIT-#24 — Stamp body forward-compat (audit-derived)
- Existing parser handles unknown keys forward-compat. Plus v5.9.3
  bumps MODEL_FORMAT_VERSION 5→6 with WARN-fallback. Adequate.

#### V5_9_AUDIT-#25 — Cross-binary version mismatch (post-paper-test 2026-05-01)
- engine_gui and foxml_suite versions could drift. Today silent.
- **Fix:** Live engine warns on stamp's `engine_version` major.minor
  drift from current `ENGINE_VERSION_STRING`. Patch differences OK.
- **Ship:** v5.9.4 Phase 5 Output D.
- **Effort:** ~1h.

#### V5_9_AUDIT-#26 — Stamp secret rotation (post-paper-test 2026-05-01)
- No documented procedure for rotating compromised
  `held_out_stamp_secret`.
- **Fix:** Recipe in ML_TEST_RECIPES.md.
- **Ship:** v5.9.4 Phase 5 Output A.
- **Effort:** ~30min docs.

#### V5_9_AUDIT-#27 — Stamp canonical body rigidity (audit-derived)
- Adding fields requires MODEL_FORMAT_VERSION bump. Pattern works
  but creates churn. Architectural observation only; accept.

---

## Cross-cutting concerns (single fixes that close multiple findings)

### Cross-cutting #1 — Train-serve parity overhaul

Closes: V5_9_AUDIT-#1 (NaN guard) + V5_9_AUDIT-#6 (prediction NaN).

The NaN-fold-into-Features_PackAll fix is single-source-of-truth.
Parallel design at the prediction layer (validate Model_Predict
output once at the same shape). Both prevent silent passthrough
to gate decision.

### Cross-cutting #2 — ML observability dashboard

Closes: V5_9_AUDIT-#2 (load failure) + #3 (entry log) + #11 (warmup)
+ #5 (default vs deliberate) + #4 (cfg source) + #25 (cross-binary).

A single new "ML Status" dashboard panel + extended Engine Header
panel (showing loaded cfg path) closes 6 separate visibility gaps.
All data flows through TUISnapshot — single read path, single render
path.

### Cross-cutting #3 — Production deploy ritual + recipes

Closes: V5_9_AUDIT-#16 (deploy runbook) + #17 (rollback) + #18
(retraining) + #26 (secret rotation).

A single ML_TEST_RECIPES.md doc covers all 4 operator transitions
that today are tribal knowledge.

### Cross-cutting #4 — Stamp body integrity

Closes: V5_9_AUDIT-#15 (signature scope) + #24 (forward-compat) +
#27 (canonical-body rigidity, accept).

`stamp_format_version=1` field at v5.9.0 (Phase 1) makes future
schema changes deliberate + version-rejected. Forward-compat
parsing already adequate. Rigidity is accepted (one bump per
schema change is correct behavior).

---

## Live-vs-ML parity gaps (specific concern Jenny flagged)

This is the audit's primary lens: find places where the live engine
and the suite diverge silently.

### Confirmed parity gaps:

| Gap | Root cause | Fix in v5.9 | Ship |
|---|---|---|---|
| **cfg path divergence** (live reads engine.cfg, suite reads backtest.cfg) | Two binaries with two cfg files; operators expected one | V5_9_AUDIT-#4: cfg-path display + diff panel | v5.9.0 Phase 1 |
| **default cfg fields** silently apply different strategies | `ControllerConfig_Default` sets DIP; missing per-core fields = DIP | V5_9_AUDIT-#5: explicit-set bitmap + tri-state TUI | v5.9.0 Phase 1 |
| **`auto_stamp_on_held_out` cfg field** parsed but ignored pre-v5.8.10 | Suite UI used its own toggle | Closed v5.8.10 (suite reads cfg now) | DONE |
| **`feature_registry_hash` registry check** dormant pre-v5.8.6 | CoreModelZoo passed default 0 = skip | Closed v5.8.6 | DONE |
| **`tools/stamp_model.sh` field set** lagged in-process pre-v5.8.8 | Two sign paths drifted | Closed v5.8.8 (regression test) | DONE |
| **`stamp_format_version`** field absent (no schema-change protection) | Stamp body has no version-of-itself | V5_9_AUDIT-#15: add field + verifier check | v5.9.0 Phase 1 |
| **expected.cfg vs engine.cfg** mismatch policy is warn-only | `model_verify_strict` defaults to 0 | Document in production runbook (v5.9.4) — operator must flip strict | v5.9.4 Phase 5 |
| **`MLBuildContext` populator** in live vs ShardedBacktestDriver in suite | Both populate `Regime_ComputeSignals` args; could fall out of sync if either omits a state field | V5_9_AUDIT-#1 + Phase 3 parity regression catches this | v5.9.0 + v5.9.2 |
| **engine_version stamp vs current build** could differ silently | engine_gui + foxml_suite versions could drift | V5_9_AUDIT-#25: cross-binary handshake | v5.9.4 Phase 5 |
| **NaN feature** vs **NaN prediction** — different layers, both silent | Two separate validation gaps | V5_9_AUDIT-#1 + #6 | v5.9.0 Phase 1 |

### Verified NOT a parity gap:

| Investigated | Conclusion | Why |
|---|---|---|
| FeatureComputeCtx aux fields (medium_rolling, ema_price, etc.) being null in some path | NOT a gap | All 34 features read from `signals` or `short_rolling`. Aux fields are forward-compat dead code (removed in Phase 0). |
| MLBuildContext field population | NOT a gap | Verified at ControllerEventLoop.hpp:1818-1840 — fully populated. Same shape as ShardedBacktestDriver's args to Regime_ComputeSignals. |
| Feature registry hash check | NOT a gap (post-v5.8.6) | Wired through CoreModelZoo + verified in tests. |
| Stamp signature canonical body | NOT a gap (post-v5.8.8) | Bash + in-process produce bytewise identical bodies; regression-tested. |

---

## Suggested ship sequence

### v5.9.0a — Pre-implementation docs (Phase 0)
- This audit doc finalized
- DOCS/CLAUDE_ML_INVARIANTS.md NEW
- Remove unused FeatureComputeCtx aux fields
- ~3h

### v5.9.0 — Visibility + immediate guards (Phase 1)
- V5_9_AUDIT-#1 (NaN fold), #2 (load viz), #3 (entry log), #4 (cfg
  source), #5 (default-vs-deliberate), #6 (prediction NaN), #7
  (Train Model worker), #8 (WF purge), #9 (ctx cleanup, already in
  Phase 0), #15 (stamp_format_version)
- ~11-13h

### v5.9.1 — Edge cases + tuning (Phase 2)
- V5_9_AUDIT-#10 (confidence hard-floor), #11 (warmup viz), #12
  (label NaN), #13 (ConfidenceScore tau), #14 (BarrierGate cfg),
  #9 (class-weight docs), #23 (random_state docs)
- ~4.5-5.5h

### v5.9.2 — Train-serve parity regression test (Phase 3)
- Cross-cutting parity test that catches future drift structurally
- ~4-7h (includes 2h scaffolding spike)

### v5.9.3 — Feature standardization (Phase 4)
- Per-feature mean+stddev, sidecar file, MODEL_FORMAT_VERSION 5→6
- ~6-8h

### v5.9.4 — Recipes + readiness extensions (Phase 5)
- V5_9_AUDIT-#16 (deploy runbook), #17 (rollback), #18 (retraining),
  #26 (secret rotation), #19 (health log rotation), #25 (cross-
  binary handshake), readiness checks 15-17
- ~6-7h

### v5.9.5 — Postmortem (Phase 6)
- ~2h

**Total: ~36-45h.**

---

## Findings NOT a bug (false positives + verified-OK)

The following were initially flagged but verified safe on inspection:

- **CRITICAL #1 from automated audit (FeatureComputeCtx incomplete):**
  False positive. Auditor mistook unused forward-compat fields for
  required inputs. All 34 features read from `signals` or
  `short_rolling`, both of which ARE populated. Dead aux fields
  removed in Phase 0 to prevent future confusion.

- **Atomic write of stamps:** Uses rename(2), POSIX-atomic.
- **Held-out lock entropy:** Friction-grade by design.
- **Locale pinning:** Linux thread-local-safe.
- **Hot-swap race:** Not supported; no race today.
- **Feature scaling absence:** Tree-based models scale-invariant
  for splits; defer to v5.9.3 work.
- **OOM during predict:** Vanishingly small allocation.
