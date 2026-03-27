# Future: Auto-Tuning Regime Detector Thresholds

## Status: PLANNED (not started)

## Context

Per-strategy adaptation (Level 1) is already implemented — each strategy's Adapt
function tunes its own parameters (offset, breakout threshold) based on P&L regression.
This handles fast feedback (minutes).

The regime detector thresholds (slope_threshold, r2_threshold, hysteresis) are Level 2 —
meta-optimization of the detector itself. These control WHEN to switch strategies, not
HOW each strategy trades.

## Why defer

- Regime switches are rare (~2-3 per day for BTC)
- Need many switches to evaluate threshold quality
- Feedback loop is days/weeks, not minutes
- High risk of overfitting to recent market conditions
- No paper trading data yet to even evaluate what "good" thresholds look like

## Approach when ready

1. Log every regime switch with timestamp, market conditions, and subsequent P&L
2. After 2+ weeks of paper trading data, analyze which switches were profitable
3. Use offline optimization (grid search or Bayesian) to find better thresholds
4. Deploy as updated config defaults — NOT live auto-tuning

Live auto-tuning of regime thresholds is dangerous because:
- A bad threshold change can cascade (switch too early → lose money → tighten threshold
  → miss real regime changes → lose money differently)
- The sample size per day is too small for reliable online learning
- Manual review of switch decisions is more valuable than automated adjustment

## Dependencies

- Regime detector implemented and running
- 2+ weeks of paper trading logs with regime switch events
- Trade log enhanced with regime/strategy metadata per trade
