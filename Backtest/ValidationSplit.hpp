// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [VALIDATION SPLIT — PURGED TEMPORAL CV]
//======================================================================================================
// port of FoxML/private purged_time_series_split.py + feature_time_meta.py.
// prevents temporal leakage by enforcing a purge gap between train and test sets
// that accounts for both label horizon AND feature lookback windows.
//
// critical for financial ML — standard K-fold shuffles data randomly, which
// destroys time patterns and allows training on future data to predict past data.
//
// key FoxML design principles preserved:
//   1. lookback-aware purge gap: purge = max(horizon, max_feature_lookback) + buffer
//   2. growing train window (expanding, not sliding): fold 1 = [0..20%], fold 2 = [0..40%]
//   3. skip fold if train set too small after purge
//   4. time contract (t+1): labels never include current tick (enforced in LabelFunctions)
//
// tick-level adaptations (vs FoxML's 5-minute bars):
//   - tick indices ARE the time axis (single symbol, no panel data)
//   - no pd.Timedelta / searchsorted needed
//   - purge gap in ticks, not minutes
//
// source: ~/FoxML/private/TRAINING/ranking/utils/purged_time_series_split.py
// source: ~/FoxML/private/TRAINING/ranking/utils/feature_time_meta.py
// source: ~/FoxML/private/CONFIG/pipeline/training/safety.yaml (temporal config)
//
// FUTURE HOOKS:
//   multi-symbol: add symbol_id param to PurgeGap_Compute (default 0, ignored for now)
//     → see ~/FoxML/private/TRAINING/ranking/utils/cross_sectional_data.py
//   multi-interval embargo: per-feature embargo_minutes
//     → see ~/FoxML/private/TRAINING/ranking/utils/feature_time_meta.py
//======================================================================================================
#ifndef VALIDATION_SPLIT_HPP
#define VALIDATION_SPLIT_HPP

#include "../ML_Headers/ModelInference.hpp"
#include <stdio.h>

//======================================================================================================
// [PURGE GAP COMPUTATION]
//======================================================================================================
// from FoxML purge.py: purge = max(horizon, max_feature_lookback) + buffer
// with safety.yaml: purge_include_feature_lookback = true
//
// the purge gap ensures that the last training sample's features cannot see
// any data that overlaps with the test set's label window. without this,
// a 512-tick feature lookback near the train/test boundary contaminates
// the test set even if the label horizon is only 100 ticks.
//======================================================================================================

// default purge buffer (ticks). from safety.yaml: lookback_buffer_minutes = 5.0
// at ~10 ticks/second this is ~3000 ticks. conservative default, configurable.
#define PURGE_BUFFER_DEFAULT 512

// compute purge gap accounting for both label horizon and feature lookback.
// uses FEATURE_LOOKBACKS table from ModelInference.hpp.
//
// formula (from FoxML purge.py):
//   purge = max(horizon_ticks, max_feature_lookback) + buffer_ticks
//
// this is the minimum gap needed between the last training tick and the
// first test tick to prevent any form of temporal leakage.
static inline int PurgeGap_Compute(int horizon_ticks, int buffer_ticks) {
    int max_lookback = FeatureLookback_Max(); // from ModelInference.hpp
    int base = (horizon_ticks > max_lookback) ? horizon_ticks : max_lookback;
    return base + buffer_ticks;
}

// overload: caller provides explicit max_lookback (for testing or custom feature sets)
static inline int PurgeGap_ComputeExplicit(int horizon_ticks, int max_lookback, int buffer_ticks) {
    int base = (horizon_ticks > max_lookback) ? horizon_ticks : max_lookback;
    return base + buffer_ticks;
}

//======================================================================================================
// [PURGED SPLIT RESULT]
//======================================================================================================
// each fold is a (train, test) pair with the purge gap enforced between them.
// train window grows with each fold (expanding window, not sliding).
//
// layout for fold k of n_splits:
//   train: [0 .. test_start - purge_gap)
//   purge: [test_start - purge_gap .. test_start)    ← discarded, prevents leakage
//   test:  [test_start .. test_end)
//======================================================================================================

#define VALIDATION_MAX_FOLDS 20

struct PurgedSplit {
    int train_start;    // inclusive
    int train_end;      // exclusive (last train tick + 1)
    int test_start;     // inclusive
    int test_end;       // exclusive
    int purge_gap;      // ticks between train_end and test_start
    int train_count;    // train_end - train_start (convenience)
    int test_count;     // test_end - test_start (convenience)
    int valid;          // 1 = usable fold, 0 = skipped (train too small after purge)
};

//======================================================================================================
// [GENERATE FOLDS]
//======================================================================================================
// generates walk-forward folds with lookback-aware purge gap.
// mirrors FoxML PurgedTimeSeriesSplit.split() adapted for tick indices.
//
// key behavior from FoxML:
//   - test folds are equal-sized slices of the data (last fold may be smaller)
//   - train window grows with each fold: fold 1 trains on [0..N/5],
//     fold 2 on [0..2N/5], etc. (expanding window)
//   - purge gap enforced: train_end = test_start - purge_gap
//   - fold skipped if train set would be empty after purge
//
// parameters:
//   folds:          output array (caller allocates, max VALIDATION_MAX_FOLDS)
//   total_samples:  total number of samples in the dataset
//   n_splits:       number of folds (default 5, from FoxML)
//   horizon_ticks:  label forward window (e.g. 1000 ticks)
//   buffer_ticks:   extra safety margin (default PURGE_BUFFER_DEFAULT)
//   min_train:      minimum training samples required (skip fold if fewer)
//
// returns: number of valid folds generated (may be < n_splits if early folds skipped)
//======================================================================================================
static inline int ValidationSplit_Generate(PurgedSplit *folds, int total_samples,
                                            int n_splits, int horizon_ticks,
                                            int buffer_ticks, int min_train) {
    if (n_splits < 2) n_splits = 2;
    if (n_splits > VALIDATION_MAX_FOLDS) n_splits = VALIDATION_MAX_FOLDS;
    if (total_samples < n_splits * 2) return 0; // not enough data for any fold

    int purge_gap = PurgeGap_Compute(horizon_ticks, buffer_ticks);

    // compute fold boundaries (equal-sized test sets, like FoxML)
    // distribute samples evenly: fold_size = total / n_splits
    // remainder goes to earlier folds (same as numpy: fold_sizes[:remainder] += 1)
    int fold_size = total_samples / n_splits;
    int remainder = total_samples % n_splits;

    int valid_count = 0;
    int current = 0; // tracks start of current test fold

    for (int i = 0; i < n_splits; i++) {
        int this_fold_size = fold_size + (i < remainder ? 1 : 0);
        int test_start = current;
        int test_end = current + this_fold_size;
        if (test_end > total_samples) test_end = total_samples;

        // train window: [0 .. test_start - purge_gap)
        // growing window: train always starts at 0 (expanding, not sliding)
        int train_end = test_start - purge_gap;
        int train_start = 0;

        PurgedSplit *f = &folds[i];
        f->purge_gap = purge_gap;

        if (train_end <= train_start || (train_end - train_start) < min_train) {
            // train set too small after purge — skip this fold
            // this is expected behavior for early folds with large purge gaps
            // (same as FoxML: "Fold N: purge too large, skipping")
            f->train_start = 0;
            f->train_end = 0;
            f->test_start = test_start;
            f->test_end = test_end;
            f->train_count = 0;
            f->test_count = test_end - test_start;
            f->valid = 0;

            if (i == 0) {
                fprintf(stderr, "[validation] fold %d/%d: purge_gap=%d > test_start=%d, "
                        "skipping (not enough history). this is normal for early folds.\n",
                        i + 1, n_splits, purge_gap, test_start);
            }
        } else {
            f->train_start = train_start;
            f->train_end = train_end;
            f->test_start = test_start;
            f->test_end = test_end;
            f->train_count = train_end - train_start;
            f->test_count = test_end - test_start;
            f->valid = 1;
            valid_count++;
        }

        current = test_end;
    }

    if (valid_count == 0) {
        fprintf(stderr, "[validation] WARNING: all %d folds skipped — purge_gap=%d "
                "is too large for %d samples. increase data or reduce purge.\n",
                n_splits, purge_gap, total_samples);
    } else {
        fprintf(stderr, "[validation] generated %d/%d valid folds "
                "(purge_gap=%d, max_lookback=%d, horizon=%d, buffer=%d)\n",
                valid_count, n_splits, purge_gap, FeatureLookback_Max(),
                horizon_ticks, buffer_ticks);
    }

    return valid_count;
}

// overload: caller provides an explicit purge gap (already computed, no PurgeGap_Compute call)
// used by walk-forward when splitting in non-neutral sample space where raw lookback doesn't apply
static inline int ValidationSplit_GenerateExplicit(PurgedSplit *folds, int total_samples,
                                                    int n_splits, int explicit_purge_gap,
                                                    int min_train) {
    if (n_splits < 2) n_splits = 2;
    if (n_splits > VALIDATION_MAX_FOLDS) n_splits = VALIDATION_MAX_FOLDS;
    if (total_samples < n_splits * 2) return 0;

    int purge_gap = explicit_purge_gap;
    int fold_size = total_samples / n_splits;
    int remainder = total_samples % n_splits;

    int valid_count = 0;
    int current = 0;

    for (int i = 0; i < n_splits; i++) {
        int this_fold_size = fold_size + (i < remainder ? 1 : 0);
        int test_start = current;
        int test_end = current + this_fold_size;
        if (test_end > total_samples) test_end = total_samples;

        int train_end = test_start - purge_gap;
        int train_start = 0;

        PurgedSplit *f = &folds[i];
        f->purge_gap = purge_gap;

        if (train_end <= train_start || (train_end - train_start) < min_train) {
            f->train_start = 0;
            f->train_end = 0;
            f->test_start = test_start;
            f->test_end = test_end;
            f->train_count = 0;
            f->test_count = test_end - test_start;
            f->valid = 0;
        } else {
            f->train_start = train_start;
            f->train_end = train_end;
            f->test_start = test_start;
            f->test_end = test_end;
            f->train_count = train_end - train_start;
            f->test_count = test_end - test_start;
            f->valid = 1;
            valid_count++;
        }

        current = test_end;
    }

    if (valid_count == 0) {
        fprintf(stderr, "[validation] WARNING: all %d folds skipped — purge_gap=%d "
                "is too large for %d samples.\n", n_splits, purge_gap, total_samples);
    } else {
        fprintf(stderr, "[validation] generated %d/%d valid folds "
                "(explicit purge_gap=%d, total=%d)\n",
                valid_count, n_splits, purge_gap, total_samples);
    }

    return valid_count;
}

//======================================================================================================
// [VALIDATION HELPERS]
//======================================================================================================

// verify no overlap between train and test (debug assertion)
// returns 1 if all folds are clean, 0 if leakage detected
static inline int ValidationSplit_Verify(const PurgedSplit *folds, int n_splits) {
    for (int i = 0; i < n_splits; i++) {
        if (!folds[i].valid) continue;
        // train must end before test starts (with purge gap)
        if (folds[i].train_end > folds[i].test_start) {
            fprintf(stderr, "[validation] LEAKAGE: fold %d train_end=%d > test_start=%d\n",
                    i + 1, folds[i].train_end, folds[i].test_start);
            return 0;
        }
        // purge gap must be respected
        int actual_gap = folds[i].test_start - folds[i].train_end;
        if (actual_gap < folds[i].purge_gap) {
            fprintf(stderr, "[validation] LEAKAGE: fold %d actual_gap=%d < purge_gap=%d\n",
                    i + 1, actual_gap, folds[i].purge_gap);
            return 0;
        }
    }
    return 1;
}

// print fold summary (for logging / debugging)
static inline void ValidationSplit_Print(const PurgedSplit *folds, int n_splits) {
    fprintf(stderr, "[validation] fold summary:\n");
    for (int i = 0; i < n_splits; i++) {
        const PurgedSplit *f = &folds[i];
        if (f->valid) {
            fprintf(stderr, "  fold %d/%d: train=[%d..%d) (%d samples), "
                    "test=[%d..%d) (%d samples), purge=%d\n",
                    i + 1, n_splits, f->train_start, f->train_end, f->train_count,
                    f->test_start, f->test_end, f->test_count, f->purge_gap);
        } else {
            fprintf(stderr, "  fold %d/%d: SKIPPED (train too small after purge)\n",
                    i + 1, n_splits);
        }
    }
}

#endif // VALIDATION_SPLIT_HPP
