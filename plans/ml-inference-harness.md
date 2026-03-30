# ML Inference Harness — XGBoost + LightGBM

## Context

Jennifer wants a model inference layer so trained tree models (XGBoost, LightGBM) can drive buy signals and/or regime classification. Models are trained offline in Python; the engine only does inference. LSTM and heavier models are future work — this plan covers the lightweight tree-based path only.

The codebase already has a stub for this: `RegimeSignals.model_score` (RegimeDetector.hpp:80, commented out). The strategy pattern (4 functions + state struct) and config macro system make this straightforward.

---

## Architecture: Two Integration Modes

### Mode A — Regime Signal Enrichment (simplest, no new strategy)
- Load a trained model at startup
- On each slow-path cycle, pack RegimeSignals fields into a feature vector, run inference
- Write the prediction into `RegimeSignals.model_score`
- Factor `model_score` into `Regime_Classify` scoring (e.g., +2 trending if model says trend)
- **No new strategy needed** — existing MR/Momentum/SimpleDip benefit from better regime calls

### Mode B — Full ML Strategy (STRATEGY_ML = 3)
- New strategy with its own Init/Adapt/BuySignal/ExitAdjust
- BuySignal runs model inference → returns buy conditions based on predicted probability
- Can coexist with Mode A (regime model + separate buy-signal model)
- Follows the same 4-function pattern as MR/Momentum/SimpleDip

**Recommendation: Implement both.** Mode A is ~30 lines of real logic. Mode B is the full harness. They're independent — Mode A enriches regime detection for all strategies, Mode B adds a new strategy option.

---

## Implementation Plan

### Step 1: ML Inference Abstraction (`ML_Headers/ModelInference.hpp`)

A thin C-style wrapper that compiles to a no-op when neither backend is enabled:

```
Template struct: ModelHandle<F>
  - void *handle (opaque: BoosterHandle for XGB, BoosterHandle for LGBM)
  - int backend (0=none, 1=xgboost, 2=lightgbm)
  - int num_features
  - float feature_buf[32] (scratch space, float because both C APIs want float)

Functions:
  Model_Load(ModelHandle *m, const char *path, int backend)
    #ifdef USE_XGBOOST: XGBoosterCreate / XGBoosterLoadModel
    #ifdef USE_LIGHTGBM: LGBM_BoosterCreateFromModelfile
    fallback: m->backend = 0 (no-op)

  Model_Predict(ModelHandle *m, float *features, int n) -> float
    #ifdef USE_XGBOOST: XGBoosterPredict (single row)
    #ifdef USE_LIGHTGBM: LGBM_BoosterPredictForMatSingleRow
    fallback: return 0.0f

  Model_Free(ModelHandle *m)
    cleanup handles

  Model_IsLoaded(ModelHandle *m) -> int
    return m->backend != 0
```

Both XGBoost and LightGBM C APIs take `float*` row vectors. Single-row inference is ~1-5us for typical tree models (well within slow-path budget).

### Step 2: Feature Packing (`ML_Headers/ModelInference.hpp`)

```
ModelFeatures_Pack(float *buf, const RegimeSignals<F> *sig, const RollingStats<F> *r, const RollingStats<F,512> *r_long)
  buf[0] = FPN_ToDouble(sig->short_slope)
  buf[1] = FPN_ToDouble(sig->short_r2)
  buf[2] = FPN_ToDouble(sig->short_variance)
  buf[3] = FPN_ToDouble(sig->long_slope)
  buf[4] = FPN_ToDouble(sig->long_r2)
  buf[5] = FPN_ToDouble(sig->long_variance)
  buf[6] = FPN_ToDouble(sig->vol_ratio)
  buf[7] = FPN_ToDouble(sig->ror_slope)
  buf[8] = FPN_ToDouble(sig->volume_slope)
  buf[9] = FPN_ToDouble(sig->ema_sma_spread)  // if exists
  buf[10] = FPN_ToDouble(r->vwap_deviation)
  buf[11] = FPN_ToDouble(r->price_stddev)
  ... (map to whatever features the trained model expects)
  return num_features
```

Feature order must match training pipeline exactly. A `model_features.cfg` or hardcoded enum keeps this in sync. Start with hardcoded, add config later if needed.

### Step 3: Mode A — Regime Signal Enrichment

**RegimeDetector.hpp:**
- Uncomment `model_score` in RegimeSignals struct (line 80)
- In `Regime_ComputeSignals`: if model loaded, pack features → predict → write `model_score`
- In `Regime_Classify`: add `model_score` as a signal in trending/volatile scoring (e.g., `if model_score > threshold: trending_score += 2`)

**PortfolioController struct:**
- Add `ModelHandle<F> regime_model` field
- Load in init from config path (`regime_model_path` config field)

### Step 4: Mode B — ML Strategy (`Strategies/MLStrategy.hpp`)

```
STRATEGY_ML = 3  (StrategyInterface.hpp)

MLStrategyState<F>:
  ModelHandle<F> buy_model      // buy signal model (separate from regime model)
  float feature_buf[32]
  FPN<F> buy_threshold          // predict > threshold → buy signal
  FPN<F> last_prediction
  BuySideGateConditions<F> buy_conds_initial

MLStrategy_Init(state, rolling, buy_conds):
  - Model already loaded by controller init
  - Set buy_conds_initial from rolling stats (same as other strategies)

MLStrategy_Adapt(state, price, portfolio_delta, active_bitmap, buy_conds, cfg):
  - No-op for now (model is static)
  - Future: online learning / feature drift detection

MLStrategy_BuySignal(state, rolling, rolling_long, cfg):
  - Pack features from rolling stats
  - Run inference: prediction = Model_Predict(&state->buy_model, features, n)
  - If prediction > buy_threshold:
    - Set buy price = rolling->price_avg (or model-suggested level)
    - Set buy volume = rolling->volume_avg * cfg->volume_mult
    - gate_direction = 0 (buy below, like MR) or model-driven
  - Else: return zero-gate (no buy)

MLStrategy_ExitAdjust(portfolio, price, rolling, state, cfg):
  - Option 1: Use fixed TP/SL from config (simplest)
  - Option 2: Run exit model for per-position adjustment (future)
  - Start with option 1, same pattern as SimpleDip
```

### Step 5: Controller Wiring (`PortfolioController.hpp`)

- Add `MLStrategyState<F> ml_strategy` to PortfolioController struct
- Add `ModelHandle<F> regime_model` to PortfolioController struct
- In init: `Model_Load(&ctrl->ml_strategy.buy_model, cfg.ml_model_path, cfg.ml_backend)`
- In init: `Model_Load(&ctrl->regime_model, cfg.regime_model_path, cfg.regime_model_backend)`
- Add `case STRATEGY_ML:` to both dispatch switches (StrategyDispatch + StrategyBuySignal)
- Add `MLStrategy_Init` call alongside other strategy inits

### Step 6: Config Fields (`ControllerConfig.hpp` + `engine.cfg`)

```
# ML inference
ml_backend = 0              # 0=disabled, 1=xgboost, 2=lightgbm
ml_model_path =             # path to buy-signal model file
ml_buy_threshold = 0.6      # prediction threshold for buy signal
ml_tp_pct = 1.5             # take profit % for ML positions
ml_sl_pct = 0.8             # stop loss % for ML positions

regime_model_backend = 0    # 0=disabled, 1=xgboost, 2=lightgbm
regime_model_path =         # path to regime enrichment model
regime_model_weight = 2     # score weight in Regime_Classify
```

### Step 7: Build System (`CMakeLists.txt`)

```cmake
option(USE_XGBOOST "Link XGBoost C API for ML inference" OFF)
option(USE_LIGHTGBM "Link LightGBM C API for ML inference" OFF)

# Apply to both engine and engine_gui targets
foreach(tgt IN ITEMS engine engine_gui)
  if(TARGET ${tgt})
    if(USE_XGBOOST)
      find_package(xgboost REQUIRED)
      target_compile_definitions(${tgt} PRIVATE USE_XGBOOST)
      target_link_libraries(${tgt} PRIVATE xgboost::xgboost)
    endif()
    if(USE_LIGHTGBM)
      find_library(LIGHTGBM_LIB _lightgbm)
      target_compile_definitions(${tgt} PRIVATE USE_LIGHTGBM)
      target_link_libraries(${tgt} PRIVATE ${LIGHTGBM_LIB})
    endif()
  endif()
endforeach()
```

Build: `cmake -B build -DUSE_XGBOOST=ON && cmake --build build`

### Step 8: Regime Wiring (`RegimeDetector.hpp`)

- Add `case REGIME_*: return STRATEGY_ML` mapping (or keep ML as explicit `default_strategy=3` only)
- Add position adjustment case in `Regime_AdjustPositions` for ML positions
- Follow Safety Invariants: SL floor, TP floor, stddev guard

### Step 9: Tests (`tests/controller_test.cpp`)

- Test Model_Load with missing file (graceful no-op)
- Test ModelFeatures_Pack produces correct feature count
- Test MLStrategy_BuySignal with mock prediction (inject threshold crossing)
- Test regime enrichment: model_score shifts regime classification
- Test TP/SL invariants hold for ML-entered positions

---

## File Summary

| File | Action |
|------|--------|
| `ML_Headers/ModelInference.hpp` | **NEW** — inference abstraction + feature packing |
| `Strategies/MLStrategy.hpp` | **NEW** — 4-function ML strategy |
| `Strategies/StrategyInterface.hpp` | Add `STRATEGY_ML 3` |
| `Strategies/RegimeDetector.hpp` | Uncomment `model_score`, use in classify |
| `CoreFrameworks/PortfolioController.hpp` | Add ML state, dispatch case, model loading |
| `CoreFrameworks/ControllerConfig.hpp` | Add ML config fields |
| `CMakeLists.txt` | Add USE_XGBOOST / USE_LIGHTGBM options |
| `engine.cfg` | Add ML config section |
| `tests/controller_test.cpp` | ML strategy + regime enrichment tests |

## Verification

1. Build without ML flags — everything compiles, no-op paths, zero overhead
2. Build with `-DUSE_XGBOOST=ON` — links xgboost, model loads from path
3. `./build/controller_test` — all existing tests pass + new ML tests
4. Run with `ml_backend=0` — engine behaves identically to before
5. Run with `ml_backend=1, ml_model_path=model.xgb` — predictions flow into regime + buy signals
6. Hot-reload: change `ml_buy_threshold` in engine.cfg — picks up without restart

## Ideas from FoxML LIVE_TRADING (5m interval system)

Transferable patterns from ~/FoxML/private/LIVE_TRADING/, adapted for tick-level:

### Tier 1 — High ROI, build alongside harness

**1. Confidence Scoring (FoxML: confidence.py)**
- `confidence = IC × freshness × capacity × stability`
- Freshness decay: `exp(-age_days / 30)` — auto-downweight stale models
- Rolling IC (information coefficient): correlation of past predictions vs actual returns
- Gate predictions by confidence: `calibrated_score = raw_prediction × confidence`
- **C++ impl:** Ring buffer of (prediction, actual_return) pairs, compute IC every N ticks

**2. Barrier Gate — Peak/Valley Classifiers (FoxML: barrier_gate.py)**
- Two lightweight binary XGBoost models: `P(will_peak)` and `P(will_valley)`
- Hard block: if `p_peak > threshold`, don't enter regardless of alpha signal
- Soft modulation: `gate_value = max(g_min, (1 - p_peak)^γ × (0.5 + 0.5 × p_valley)^δ)`
- Position sizing: `weight = alpha × gate_value`
- **C++ impl:** Two extra small models loaded alongside main model, inference on slow path
- **Why tick-level:** Sub-second precision on detecting local highs before entry

**3. Ridge Ensemble Weighting (FoxML: ridge_weights.py)**
- `w ∝ (Σ + λI)^-1 × μ` where Σ = correlation matrix of model predictions, μ = IC vector
- Auto-balances correlated models (two similar XGB models get 50/50, not double weight)
- Ridge λ=0.15, recompute every 50-100 predictions
- **C++ impl:** Small matrix inverse (Eigen or hand-rolled for 2-4 models), circular prediction buffer

**4. Volatility-Scaled Sizing (FoxML: position_sizer.py)**
- `position_weight ∝ alpha / rolling_volatility`
- High vol → smaller position, low vol → larger (consistent risk per trade)
- No-trade band: skip if |Δweight| < threshold (prevents churn)
- **C++ impl:** Already have price_stddev in RollingStats — use as vol scaler on quantity

**5. Sticky Kill Switch (FoxML: guardrails.py)**
- Once breached, stays active until EOD (no "recovery mode")
- Triggers: daily loss > X%, drawdown > Y% from intra-day peak, gross exposure > Z%
- **C++ impl:** Atomic bool, checked on hot path, persisted to snapshot for crash recovery

### Tier 2 — Advanced, build after harness is proven

**6. Exp3-IX Bandit for Online Model Weighting (FoxML: bandit.py)**
- Each (model × horizon) pair is a "arm"
- Reward = realized net P&L in bps after fees/slippage
- Adaptive learning rate: `eta = min(eta_max, √(ln(K) / (K×T)))`
- Blends with static weights via ramp: `final = (1-blend) × static + blend × bandit`
- Ramp-up: bandit influence grows after 100-500 trades (prevents early instability)
- **C++ impl:** Track pending trades (entry_price, arm_id, timestamp), attribute P&L on exit

**7. Cost-Aware Horizon Arbitration (FoxML: cost_model.py)**
- `cost = k1×spread + k2×σ×√(h/5) + k3×impact(q)`
- `score_h = (alpha_h - cost_h) / √(h/5)` — pick horizon with best risk-adjusted return
- **Tick-level adaptation:** Estimate spread from bid-ask, vol from RollingStats, impact from order book imbalance

**8. Temperature Compression (FoxML: temperature.py)**
- `w_final ∝ w^(1/T)` — short horizons get flattened weights (more conservative)
- T=0.75 for 5m, could use T=0.5 for tick-level (less information → less concentration)

**9. Reward Attribution Tracker (FoxML: reward_tracker.py)**
- Separate entry recording from exit recording
- Map realized P&L back to originating model/arm
- `net_pnl_bps = (exit - entry) / entry × 10000 - fee_bps - slippage_bps`
- Essential for bandit feedback loop

### Tier 3 — Polish

**10. Prediction Standardization**
- Welford's online algorithm for rolling mean/std per model
- Z-score normalize before blending: `z = (raw - μ) / σ`
- Prevents scale mismatch between XGBoost and LightGBM outputs

**11. Model Checksum Verification**
- SHA256 of model file stored in metadata, verified before loading
- Prevents silent corruption (especially after crashes)

**12. Sequential Buffer for LSTM (future)**
- Ring buffer of shape (T, num_features) for sequential models
- `push(bar)`, `is_ready()` when filled_count >= lookback window
- Needed when LSTM/transformer models are added later

**13. Alert Rate Limiting**
- Severity levels + 1s dedup window
- Tick engines generate 1000s events/min — prevents log spam

---

## Future (not this PR)

- LSTM / transformer inference (needs ONNX Runtime or TensorFlow Lite — heavier dep)
- Online learning (update model weights from live P&L)
- Feature drift detection (alert when live distribution diverges from training)
- Multi-model ensemble (average XGB + LGBM predictions)
- Python training pipeline scaffold (feature extraction matching engine's packing order)
