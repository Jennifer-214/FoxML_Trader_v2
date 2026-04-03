# ML Features Usage Guide

Phase 6-7 added ML infrastructure ported from FoxML. All features default OFF — enable them in `engine.cfg` as needed.

## Quick Reference

| Feature | Config | Needs Model? | What It Does |
|---------|--------|-------------|--------------|
| Cost Gate | `cost_gate_enabled=1` | No | Blocks entries when estimated trade cost exceeds TP target |
| Vol Scaling | `foxml_vol_scaling_enabled=1` | No | Scales position size inversely with volatility |
| Bandit | `bandit_enabled=1` | No | Learns which strategies profit over time (Exp3-IX) |
| Confidence | `confidence_enabled=1` | No | Dynamic ML threshold from prediction quality |
| ML Strategy | `ml_backend=1` + model path | Yes | XGBoost/LightGBM buy signal model |
| Barrier Gate | `barrier_gate_enabled=1` + models | Yes | Blocks entries before predicted price peaks |
| Prediction Normalize | `prediction_normalize=1` | Yes | Z-score normalize model predictions |

## Features That Work Without Models

These features use the engine's own rolling stats — no model training required.

### Cost Gate
Estimates trade cost from spread + volatility timing + market impact. Suppresses entries when the cost exceeds the TP target.

```
cost_gate_enabled=1
```

### Vol-Scaled Sizing
Scales position size inversely with volatility — smaller positions in volatile markets, larger in calm ones. Targets consistent risk per trade.

```
foxml_vol_scaling_enabled=1
foxml_vol_scaling_z_max=3.0     # z-score cap (default 3.0)
```

### Bandit Strategy Selection
Blends regime-based strategy selection with learned bandit weights. Tracks which strategies actually profit and gradually shifts allocation toward winners.

```
bandit_enabled=1
bandit_blend_ratio=0.30         # max bandit influence (30%)
```

The bandit ramps from 0% to `blend_ratio` over the first 200 trades. State persists across restarts (snapshot v11).

### Confidence Scoring
Dynamically adjusts the ML buy threshold based on prediction quality metrics (IC, RMSE, data freshness). Raises the bar when predictions are unreliable.

```
confidence_enabled=1
```

## Training Models in FoxML Suite

### 1. Record Tick Data

```
# engine.cfg
record_ticks=1
record_max_days=30
```

Run the engine — tick data saves to `data/{SYMBOL}/YYYY-MM-DD.csv` (~30-70MB/day for BTCUSDT).

### 2. Build and Run the Suite

```bash
cmake -B build_suite -DUSE_IMGUI_GUI=ON -DUSE_XGBOOST=ON
cmake --build build_suite --target foxml_suite
cd build_suite && ./foxml_suite
```

### 3. Run a Backtest

1. **Data tab**: Browse `data/` directory, select one or more CSV files
2. **Run Control tab**: Click "Start" to run backtest
3. **Results tab**: Check trade statistics

### 4. Train a Buy Signal Model

1. **Run Control tab**: Check "Collect Features", run backtest again
2. **Training tab**:
   - Set Label Type (Win/Loss is simplest, Vol Barrier is best for MR)
   - Set Max Depth (3-6), Learning Rate (0.05-0.3), Estimators (50-200)
   - Set Model Path (e.g. `models/buy_signal.xgb`)
   - Click "Train Model"
3. Check stderr for training accuracy and fingerprint

### 5. Walk-Forward Validation (Recommended)

Before trusting a model, validate it out-of-sample:

1. **Training tab**: Set N Splits (5), Horizon (1000 ticks)
2. Click "Walk-Forward" — trains on historical folds, tests on future
3. Check per-fold accuracy — look for consistency, not just average

### 6. Load Model in Engine

```
# engine.cfg
ml_backend=1                    # 1 = xgboost, 2 = lightgbm
ml_model_path=models/buy_signal.xgb
ml_buy_threshold=0.50           # prediction threshold (0.5 = 50% confidence)
default_strategy=3              # 3 = ML strategy
```

On startup, stderr shows:
```
[ML] model checksum: <hex> (models/buy_signal.xgb)
[ML] XGBoost model loaded: models/buy_signal.xgb (16 features, format v1, fingerprint: <hex>)
```

## Training Barrier Gate Models

The barrier gate uses two separate models — one for peaks, one for valleys.

### Train Peak Model
1. In foxml_suite Training tab, set Label Type to "will_peak"
2. Set Model Path to `models/peak.xgb`
3. Train

### Train Valley Model
1. Set Label Type to "will_valley"
2. Set Model Path to `models/valley.xgb`
3. Train

### Enable Barrier Gate
```
barrier_gate_enabled=1
peak_model_path=models/peak.xgb
valley_model_path=models/valley.xgb
```

Gate formula: `g = max(0.2, (1-p_peak)^1.0 * (0.5+0.5*p_valley)^0.5)`
Hard block when p_peak > 0.6.

## GUI: ML Intelligence Panel

When any ML feature is enabled, the "ML Intelligence" panel appears in the bottom-right tab group (engine GUI and suite). It has 4 collapsible sections:

- **Bandit Arms**: per-strategy pull count, average reward (bps), weight, probability
- **Confidence**: IC (rank correlation), RMSE, stability, combined progress bar
- **Cost Model**: spread/timing/impact decomposition, total cost, breakeven threshold
- **Models**: loaded model paths, last prediction value

## GUI: Prediction Chart Overlay

When a model is loaded, the price chart shows an "ML" checkbox. Enable it to see:
- Prediction probability line (green = bullish, red = bearish) on secondary Y-axis (0-1)
- Confidence shading band
- Threshold line at 0.5

## Reward Attribution

Every trade automatically logs to `logging/reward_attribution.csv` with columns:
```
timestamp,strategy,reward_bps,entry_price,exit_price,hold_ticks,exit_reason
```

Use this to analyze which strategies perform best and validate bandit decisions.

## Snapshot Persistence

Bandit state and confidence scorer persist across 24h session restarts (snapshot v11). On load:
```
[SNAPSHOT] restored bandit state (N steps) + confidence scorer
```

No configuration needed — happens automatically when `bandit_enabled=1` or `confidence_enabled=1`.

## Prediction Normalization

Optional z-score normalization for model predictions. Useful when model outputs drift over time.

```
prediction_normalize=1          # default OFF
```

After 100 predictions, raw model output is z-scored using Welford online statistics, then sigmoid-mapped to [0,1] for threshold comparison. The normalizer adapts continuously.
