# 2026-04-04 — v3.7.2: Walk-Forward Fix + Training Cleanup

## Backtest Suite

### Walk-forward validation fix: non-neutral splitting
- Walk-forward was generating folds over ALL samples (215k), but with barrier labels ~97% are neutral.
- Non-neutral samples clustered in early indices (volatile periods) → later folds' test sets had 0 non-neutral samples → all folds skipped → 0.0% accuracy.
- Fix: pre-compact non-neutral samples before fold generation. Folds now split over non-neutral data only, with purge gap scaled by non-neutral density.
- Added `ValidationSplit_GenerateExplicit()` — takes a pre-computed purge gap directly (bypasses `PurgeGap_Compute` which assumes raw sample space).

### Train Model no longer corrupts feature data
- Train Model was compacting neutrals out of `results->feature_matrix` **in-place**, destroying the original data.
- Walk-forward after training saw corrupted features + stale labels → garbage results or silent failure.
- Fix: training now compacts into a separate `malloc`'d buffer. `results->feature_matrix` and `results->labels` stay intact for walk-forward.

### Stale results cleared on re-run
- Clicking "Collect Features" now clears training status + walk-forward results.
- Clicking "Train Model" clears its own status + walk-forward results.
- No more stale green/red text from previous runs persisting across re-runs.
