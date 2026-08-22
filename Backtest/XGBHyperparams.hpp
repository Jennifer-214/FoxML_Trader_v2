#pragma once
//======================================================================================================
// [FILE]_[Backtest/XGBHyperparams.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[XGBoost hyperparameter SSoT — one struct + Defaults factory + Apply helper shared by every training flow; stamp-bound for reproducibility forensics]
// [CONTAINS]
//   - [STRUCT]_[XGBHyperparams]
//   - [FUNCTION]_[XGBHyperparams_Apply]   (XGBHyperparams_Defaults rides)
//======================================================================================================
// XGBoost hyperparameter struct — single source of truth for training-side
// parameters. Used by the Train Model worker, WalkForward folds, HeldOut
// training, and the multi-horizon full-validation eval (4 Apply sites at
// HEAD; StampHelper reads Defaults for stamp emit). Pre-v5.9.5h these were
// hardcoded across the then-3 sites with subtle divergences (Train Model
// used operator-tunable max_depth/lr/n_est while WF/HeldOut hardcoded
// 6/0.1/...; nthread varied). v5.9.5h centralizes the values + extends
// with cfg-tunable fields for the 5 previously-hardcoded params (subsample,
// colsample_bytree, min_child_weight, seed, tree_method).
//
// Stamp binding: v5.9.5h emits all 8 fields into stamp body via
// StampInferenceCfgInputs.has_xgb_hyperparams group (xgb_train_nthread
// joined later — 9 xgb fields + the group flag at HEAD; SSoT =
// StampBoundModelConstRegistry). Engine load-WARN fires when cfg's
// hyperparams differ from stamp's (drift detection: CfgDriftCheckRegistry
// rows -> cfg_cross_binary_drift, SEV_YELLOW Model Health flag).
//
// Train-serve parity: hyperparams don't affect inference; same model
// bytes execute identically regardless of how they were trained.
// Stamp binding is for forensics + reproducibility, not load-time
// refusal (that was v5.9.5i's scope for inference cfg fields — landed;
// the derived-walker DRIFT_CHECK_FROM_DERIVED runs at model load).
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

//======================================================================
// [STRUCT]_[XGBHyperparams]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the 8 training hyperparams, defaults inline per field — stamp-bound as the has_xgb_hyperparams group]
//======================================================================
// [CODE]
//======================================================================
struct XGBHyperparams {
    // Tree shape + learning rate
    int    max_depth         = 6;        // tree depth (1-20 typical; >12 overfits)
    float  learning_rate     = 0.1f;     // shrinkage (0.01-0.3); aka "eta"
    int    n_estimators      = 200;      // boosting rounds; iter cap (WF/HeldOut consume hp.n_estimators too since E.1.2.D NEW-1 — no hardcoded 200 remains)
    // Regularization (subsample fractions; lower = more variance reduction)
    float  subsample         = 0.8f;     // row subsample per tree (0.5-1.0)
    float  colsample_bytree  = 0.8f;     // column subsample per tree (0.5-1.0)
    int    min_child_weight  = 5;        // min sum-of-weights per leaf (1-50)
    // Reproducibility + speed
    int    seed              = 42;       // RNG seed for reproducible runs
    char   tree_method[16]   = "hist";   // hist | exact | approx | auto
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[44B]
// [ALIGN]_[4]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[XGBHyperparams]
//======================================================================

//======================================================================
// [SECTION]_[tree-method name table — the ONE index→string mapping]
//----------------------------------------------------------------------
// E.1.2.D leaf 14 — this table existed as FOUR hand-copies (the panel
// combo, the state adapter, the worker capture, and the deleted dead
// worker). An 8th tree method added to three of four copies fails exactly
// as silently as the entry-point miss did (leaf 4b). One table; the combo
// renders it, the mapper consumes it.
//======================================================================
static const char* const XGB_TREE_METHOD_NAMES[4] = {"hist", "exact", "approx", "auto"};
enum { XGB_TREE_METHOD_COUNT = 4 };

inline XGBHyperparams XGBHyperparams_Defaults();  // defined below (rides Apply's section)

//======================================================================
// [FUNCTION]_[XGBHyperparams_FromRaw]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE panel-values → XGBHyperparams value-mapper (E.1.2.D leaf 14) — Defaults() base, 7 field overwrites, tree-method index clamped through the shared table; pure, memcmp-pinned against both former hand-copies]
//======================================================================
// [CODE]
//======================================================================
inline XGBHyperparams XGBHyperparams_FromRaw(int max_depth, float learning_rate,
                                             int n_estimators, float subsample,
                                             float colsample_bytree,
                                             int min_child_weight, int seed,
                                             int tree_method_idx) {
    XGBHyperparams h = XGBHyperparams_Defaults();
    h.max_depth        = max_depth;
    h.learning_rate    = learning_rate;
    h.n_estimators     = n_estimators;
    h.subsample        = subsample;
    h.colsample_bytree = colsample_bytree;
    h.min_child_weight = min_child_weight;
    h.seed             = seed;
    if (tree_method_idx < 0 || tree_method_idx >= XGB_TREE_METHOD_COUNT)
        tree_method_idx = 0;  // clamp → "hist" (both former copies' rule)
    strncpy(h.tree_method, XGB_TREE_METHOD_NAMES[tree_method_idx],
            sizeof(h.tree_method) - 1);
    h.tree_method[sizeof(h.tree_method) - 1] = '\0';
    return h;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[XGBHyperparams_FromRaw]
//======================================================================

//======================================================================
// [FUNCTION]_[XGBHyperparams_Apply]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[push every hyperparam onto a BoosterHandle in one call, XGBoost-build-gated; the XGBHyperparams_Defaults factory rides]
//======================================================================
// [CODE]
//======================================================================
// Factory: returns the v5.9.5g hardcoded defaults. Use this everywhere
// hyperparams need to be initialized; subsequent operator-cfg overrides
// modify the returned struct in-place.
inline XGBHyperparams XGBHyperparams_Defaults() {
    XGBHyperparams hp;  // member initializers above set defaults
    return hp;
}

#ifdef USE_XGBOOST
// Apply all hyperparams to an XGBoost BoosterHandle. Replaces the
// hand-written XGBoosterSetParam blocks the training sites carried
// pre-v5.9.5h. nthread is passed separately — each flow reads its own
// cfg knob (xgb_train_nthread for Train Model, xgb_eval_nthread for
// WF/HeldOut/full-validation; both default 4, boot-only). Caller chooses.
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[XGBHyperparams_Apply]
//======================================================================

}  // namespace tt
