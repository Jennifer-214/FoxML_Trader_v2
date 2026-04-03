// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [OVERFIT DETECTION]
//======================================================================================================
// port of FoxML/private overfitting_detection.py.
// 4 sequential threshold checks to detect when a model has memorized training data
// instead of learning generalizable patterns.
//
// thresholds from FoxML safety.yaml (battle-tested over 6 months):
//   - train_acc >= 0.99           → memorization (model learned noise)
//   - train_acc - cv_acc >= 0.20  → train/CV gap (in-sample vs cross-validation)
//   - train_acc - val_acc >= 0.20 → train/val gap (in-sample vs held-out)
//   - n_features >= cap           → feature count cap (optional, prevents curse of dimensionality)
//
// source: ~/FoxML/private/TRAINING/ranking/utils/overfitting_detection.py
// source: ~/FoxML/private/CONFIG/pipeline/training/safety.yaml:165-169
//======================================================================================================
#ifndef OVERFIT_DETECTION_HPP
#define OVERFIT_DETECTION_HPP

#include <stdio.h>
#include <string.h>

//======================================================================================================
// [THRESHOLDS — from FoxML safety.yaml]
//======================================================================================================
// these were tuned over 6 months of experiments across 20+ model families.
// 0.99 accuracy is the point where even good financial models start memorizing.
// 0.20 gap is the point where generalization has clearly failed.
//======================================================================================================
#define OVERFIT_TRAIN_ACC_THRESHOLD  0.99f   // train acc >= this = memorization
#define OVERFIT_TRAIN_VAL_GAP        0.20f   // train - val >= this = overfitting
#define OVERFIT_FEATURE_COUNT_CAP    250     // optional: skip if n_features >= this

//======================================================================================================
// [REPORT]
//======================================================================================================
struct OverfitReport {
    float train_accuracy;
    float cv_accuracy;      // cross-validation accuracy (from walk-forward folds, -1 if unavailable)
    float val_accuracy;     // held-out validation accuracy (-1 if unavailable)
    float train_cv_gap;     // train - cv (-1 if cv unavailable)
    float train_val_gap;    // train - val (-1 if val unavailable)
    int n_features;         // number of features used
    int is_overfit;         // 1 = overfit detected, 0 = clean
    int check_failed;       // which check triggered (1-4), 0 if none
    char reason[128];       // human-readable reason
};

//======================================================================================================
// [CHECK]
//======================================================================================================
// runs the 4 sequential overfit checks from FoxML.
// returns an OverfitReport with the result.
//
// parameters:
//   train_acc:    training set accuracy (0.0 - 1.0)
//   cv_acc:       cross-validation mean accuracy (-1.0 if not available)
//   val_acc:      held-out validation accuracy (-1.0 if not available)
//   n_features:   number of features (0 to skip feature count check)
//   acc_thresh:   memorization threshold (default 0.99)
//   gap_thresh:   gap threshold (default 0.20)
//   feat_cap:     feature count cap (0 to disable)
//======================================================================================================
static inline OverfitReport OverfitDetection_Check(float train_acc, float cv_acc, float val_acc,
                                                     int n_features,
                                                     float acc_thresh, float gap_thresh,
                                                     int feat_cap) {
    OverfitReport r;
    memset(&r, 0, sizeof(r));
    r.train_accuracy = train_acc;
    r.cv_accuracy = cv_acc;
    r.val_accuracy = val_acc;
    r.n_features = n_features;
    r.train_cv_gap = (cv_acc >= 0.0f) ? (train_acc - cv_acc) : -1.0f;
    r.train_val_gap = (val_acc >= 0.0f) ? (train_acc - val_acc) : -1.0f;

    // check 1: train accuracy threshold (memorization)
    if (train_acc >= acc_thresh) {
        r.is_overfit = 1;
        r.check_failed = 1;
        snprintf(r.reason, sizeof(r.reason),
                 "memorization: train_acc %.4f >= %.2f", train_acc, acc_thresh);
        return r;
    }

    // check 2: train/CV gap
    if (cv_acc >= 0.0f) {
        float gap = train_acc - cv_acc;
        if (gap >= gap_thresh) {
            r.is_overfit = 1;
            r.check_failed = 2;
            snprintf(r.reason, sizeof(r.reason),
                     "train/CV gap: %.4f >= %.2f (train=%.4f, cv=%.4f)",
                     gap, gap_thresh, train_acc, cv_acc);
            return r;
        }
    }

    // check 3: train/val gap
    if (val_acc >= 0.0f) {
        float gap = train_acc - val_acc;
        if (gap >= gap_thresh) {
            r.is_overfit = 1;
            r.check_failed = 3;
            snprintf(r.reason, sizeof(r.reason),
                     "train/val gap: %.4f >= %.2f (train=%.4f, val=%.4f)",
                     gap, gap_thresh, train_acc, val_acc);
            return r;
        }
    }

    // check 4: feature count cap (optional)
    if (feat_cap > 0 && n_features >= feat_cap) {
        r.is_overfit = 1;
        r.check_failed = 4;
        snprintf(r.reason, sizeof(r.reason),
                 "feature count: %d >= %d", n_features, feat_cap);
        return r;
    }

    // all checks passed
    r.is_overfit = 0;
    r.check_failed = 0;
    snprintf(r.reason, sizeof(r.reason), "clean");
    return r;
}

// convenience: check with default FoxML thresholds
static inline OverfitReport OverfitDetection_CheckDefaults(float train_acc, float cv_acc,
                                                             float val_acc, int n_features) {
    return OverfitDetection_Check(train_acc, cv_acc, val_acc, n_features,
                                   OVERFIT_TRAIN_ACC_THRESHOLD,
                                   OVERFIT_TRAIN_VAL_GAP,
                                   OVERFIT_FEATURE_COUNT_CAP);
}

// aggregate overfit status across multiple folds
// returns: number of folds flagged as overfit
static inline int OverfitDetection_CountOverfit(const OverfitReport *reports, int n_folds) {
    int count = 0;
    for (int i = 0; i < n_folds; i++) {
        if (reports[i].is_overfit) count++;
    }
    return count;
}

// print report (for logging / debugging)
static inline void OverfitDetection_Print(const OverfitReport *r, int fold_idx) {
    if (r->is_overfit) {
        fprintf(stderr, "[overfit] fold %d: OVERFIT — %s\n", fold_idx + 1, r->reason);
    } else {
        fprintf(stderr, "[overfit] fold %d: clean (train=%.4f",
                fold_idx + 1, r->train_accuracy);
        if (r->cv_accuracy >= 0.0f)
            fprintf(stderr, ", cv=%.4f, gap=%.4f", r->cv_accuracy, r->train_cv_gap);
        if (r->val_accuracy >= 0.0f)
            fprintf(stderr, ", val=%.4f, gap=%.4f", r->val_accuracy, r->train_val_gap);
        fprintf(stderr, ")\n");
    }
}

#endif // OVERFIT_DETECTION_HPP
