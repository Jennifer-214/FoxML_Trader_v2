# Future Strategy: Volatile Regime Handler

## Status: PLANNED (not started)

## Context

The regime detector classifies REGIME_VOLATILE when stddev is high but R² is low
(big moves, no direction). v1 handles this by pausing buying — no new entries until
the detector sees a clean ranging or trending signal. Existing positions keep their
TP/SL and may exit naturally during the volatility.

## When to implement

After the regime detector + momentum strategy are paper-traded and proven. The volatile
strategy is the third regime handler — it replaces the "pause buying" fallback with
an active approach.

## Design considerations

### Option A: Wide straddle
- Enter two positions simultaneously: one with TP above, one with TP below
- Captures the move regardless of direction
- Risk: both positions stop out if price chops in a tight range
- Requires: new position type (paired entries), modified ExitGate logic

### Option B: Reduced exposure
- Continue mean reversion but with:
  - Smaller position size (risk_pct * 0.25)
  - Wider TP/SL (stddev * 3x instead of 1.5x)
  - Stricter volume filter (require 5x avg instead of 2x)
- Trades less, survives the chop, catches the big move if one develops
- Requires: per-regime config overrides (no new structs)

### Option C: Exit-only mode
- No new entries. Actively manage existing positions:
  - Tighten SL to lock in gains (prevent winners from reversing)
  - Widen TP slightly (if the volatility resolves in your favor, capture it)
- Simplest to implement — just a modified ExitAdjust function
- Requires: regime-aware ExitAdjust dispatch

### Recommendation

Start with Option C (exit-only management) as the v1 volatile handler. It's the
lowest risk and uses existing infrastructure. Option B is the natural v2 — same
strategy logic with different parameters, no new concepts. Option A is a fundamentally
different trading approach and should be its own strategy design.

## Dependencies

- Regime detector (REGIME_VOLATILE classification working)
- Strategy dispatch function (so volatile handler can be swapped in)
- Per-position strategy tracking (so positions entered before volatility are managed correctly)
