#pragma once
//======================================================================================================
// XGBoost hyperparameter struct — single source of truth for training-side
// parameters. Used by Train Model worker, WalkForward folds, and HeldOut
// training (3 sites total). Pre-v5.9.5h these were hardcoded across the
// 3 sites with subtle divergences (Train Model used operator-tunable
// max_depth/lr/n_est while WF/HeldOut hardcoded 6/0.1/...; nthread varied).
// v5.9.5h centralizes the values + extends with cfg-tunable fields for
// the 5 previously-hardcoded params (subsample, colsample_bytree,
// min_child_weight, seed, tree_method).
//
// Stamp binding: v5.9.5h emits all 8 fields into stamp body via
// StampInferenceCfgInputs.has_xgb_hyperparams group. Engine load-WARN
// fires when cfg's hyperparams differ from stamp's (drift detection).
//
// Train-serve parity: hyperparams don't affect inference; same model
// bytes execute identically regardless of how they were trained.
// Stamp binding is for forensics + reproducibility, not load-time
// refusal (that's v5.9.5i for inference cfg fields).
//
// Defaults match the v5.9.5g hardcoded values exactly so non-tuning
// operators get bytewise-identical training output post-v5.9.5h.
//======================================================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef USE_XGBOOST
#include <xgboost/c_api.h>
#endif

namespace tt {

struct XGBHyperparams {
    // Tree shape + learning rate
    int    max_depth         = 6;        // tree depth (1-20 typical; >12 overfits)
    float  learning_rate     = 0.1f;     // shrinkage (0.01-0.3); aka "eta"
    int    n_estimators      = 200;      // boosting rounds; iter cap (matches v5.9.5g WF/HeldOut hardcoded n_rounds)
    // Regularization (subsample fractions; lower = more variance reduction)
    float  subsample         = 0.8f;     // row subsample per tree (0.5-1.0)
    float  colsample_bytree  = 0.8f;     // column subsample per tree (0.5-1.0)
    int    min_child_weight  = 5;        // min sum-of-weights per leaf (1-50)
    // Reproducibility + speed
    int    seed              = 42;       // RNG seed for reproducible runs
    char   tree_method[16]   = "hist";   // hist | exact | approx | auto
};

// Factory: returns the v5.9.5g hardcoded defaults. Use this everywhere
// hyperparams need to be initialized; subsequent operator-cfg overrides
// modify the returned struct in-place.
inline XGBHyperparams XGBHyperparams_Defaults() {
    XGBHyperparams hp;  // member initializers above set defaults
    return hp;
}

#ifdef USE_XGBOOST
// Apply all hyperparams to an XGBoost BoosterHandle. Replaces hand-written
// XGBoosterSetParam blocks at 3 training sites. nthread is passed
// separately because Train Model uses 4 (faster GUI iter) while WF/HeldOut
// use 1 (deterministic per-fold output). Caller chooses.
inline void XGBHyperparams_Apply(BoosterHandle booster,
                                  const XGBHyperparams& hp,
                                  int nthread) {
    char buf[24];

    snprintf(buf, sizeof(buf), "%d", hp.max_depth);
    XGBoosterSetParam(booster, "max_depth", buf);

    snprintf(buf, sizeof(buf), "%f", hp.learning_rate);
    XGBoosterSetParam(booster, "eta", buf);

    snprintf(buf, sizeof(buf), "%f", hp.subsample);
    XGBoosterSetParam(booster, "subsample", buf);

    snprintf(buf, sizeof(buf), "%f", hp.colsample_bytree);
    XGBoosterSetParam(booster, "colsample_bytree", buf);

    snprintf(buf, sizeof(buf), "%d", hp.min_child_weight);
    XGBoosterSetParam(booster, "min_child_weight", buf);

    snprintf(buf, sizeof(buf), "%d", nthread);
    XGBoosterSetParam(booster, "nthread", buf);

    snprintf(buf, sizeof(buf), "%d", hp.seed);
    XGBoosterSetParam(booster, "seed", buf);

    XGBoosterSetParam(booster, "tree_method", hp.tree_method);
    XGBoosterSetParam(booster, "verbosity", "0");
    // Note: n_estimators is NOT a booster param — it's the iteration loop
    // count in XGBoosterUpdateOneIter calls. Caller reads hp.n_estimators
    // directly for the loop bound.
}
#endif

}  // namespace tt
