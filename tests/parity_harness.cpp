// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [PARITY HARNESS — Track E.6]
//======================================================================================================
// Runs Backtest_Run twice on the same tick file (engine_mode=single_core
// then engine_mode=sharded) and diffs the resulting feature_matrix +
// per-row metadata. Final gate before E.7 deletes the legacy backtest
// body — if this harness reports parity, the legacy path is safe to
// remove because sharded reproduces the same training data.
//
// Comparison rules:
//   - sample_count: exact match (warmup-edge tolerance ±1 row)
//   - sample_tick_indices[i] / sample_prices[i] / sample_regimes[i]:
//     index + price exact, regime ignored on sharded (sharded has no
//     central regime field, defaults to 0; legacy uses regime.current_regime)
//   - feature_matrix[i*N + j]: relative error ≤ tolerance (default 1e-6)
//
// On drift detection: dumps first differing row, per-feature breakdown
// (which FEAT_* constant differs and by how much). Returns exit code 1.
// Clean PASS returns 0.
//
// Usage:
//   parity_harness <tick_file.csv> [config_file] [--tolerance 1e-6]
//                                                [--pay-fees-in-bnb 0|1]
//
// Single tick file at a time — call from a shell script for the multi-
// file sweep the plan calls for ("3 representative tick files").
//
// --pay-fees-in-bnb override (v5.15.5.F.4d.1.B.4 WIP-15 Phase C.6 — PARITY-030
// regression cohort): forces cfg.pay_fees_in_bnb on both runs. ApplyBnbDiscount
// (EngineCommon helper; called once at boot in both LIVE + BACKTEST per WIP-13
// migration) mutates per-core fee_rate_maker/taker to 0.75 × baseline. Train-
// serve parity verified by cross-path total_fees BPS equality below — if BNB
// discount applied asymmetrically across paths, total_fees would diverge.
//======================================================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../CoreFrameworks/SystemInit.hpp"  // v5.11.0.A — engine_set_mxcsr_ftz_daz
#include <cmath>
#include "../Backtest/BacktestEngine.hpp"
#include "../Backtest/BacktestSharded.hpp"  // pulls the sharded body for linking
#include "../ML_Headers/ModelInference.hpp"

static const char* kFeatNames[MODEL_NUM_FEATURES] = {
    "short_slope",   "short_r2",       "short_variance",
    "long_slope",    "long_r2",        "long_variance",
    "vol_ratio",     "ror_slope",      "volume_slope",
    "volume_delta",  "ema_sma_spread", "vwap_dev",
    "price_stddev",  "price_avg",      "volume_avg",
    "ema_above_sma", "mid_slope",      "mid_r2",
    "cumdelta",      "hour_sin",       "hour_cos",
    "vol_regime_rat","tick_rate_z",    "dist_to_high",
    "dist_to_low",
};

struct DiffStats {
    int rows_compared;
    int rows_with_any_diff;
    int per_feat_diff_count[MODEL_NUM_FEATURES];
    double per_feat_max_abs_err[MODEL_NUM_FEATURES];
    double per_feat_max_rel_err[MODEL_NUM_FEATURES];
    int first_diff_row;
};

static void compute_diff_stats(const BacktestResults* a,
                                const BacktestResults* b,
                                double tolerance,
                                DiffStats* out) {
    memset(out, 0, sizeof(*out));
    out->first_diff_row = -1;
    int n = a->sample_count < b->sample_count ? a->sample_count : b->sample_count;
    out->rows_compared = n;
    for (int i = 0; i < n; i++) {
        int row_diff = 0;
        for (int j = 0; j < MODEL_NUM_FEATURES; j++) {
            double av = a->feature_matrix[i * MODEL_NUM_FEATURES + j];
            double bv = b->feature_matrix[i * MODEL_NUM_FEATURES + j];
            double abs_err = fabs(av - bv);
            double mag = fmax(fabs(av), fabs(bv));
            double rel_err = (mag > 1e-12) ? (abs_err / mag) : abs_err;
            if (rel_err > tolerance) {
                out->per_feat_diff_count[j]++;
                if (abs_err > out->per_feat_max_abs_err[j])
                    out->per_feat_max_abs_err[j] = abs_err;
                if (rel_err > out->per_feat_max_rel_err[j])
                    out->per_feat_max_rel_err[j] = rel_err;
                row_diff = 1;
            }
        }
        if (row_diff) {
            out->rows_with_any_diff++;
            if (out->first_diff_row < 0) out->first_diff_row = i;
        }
    }
}

static int run_one_path(BacktestResults* out_results,
                         const char* tick_file,
                         const char* config_path,
                         int engine_mode_value,
                         int pay_fees_in_bnb_override) {
    BacktestResults_Init(out_results);

    BacktestRunConfig run;
    memset(&run, 0, sizeof(run));
    snprintf(run.data_paths[0], sizeof(run.data_paths[0]), "%s", tick_file);
    run.num_data_files = 1;
    snprintf(run.config_path, sizeof(run.config_path), "%s", config_path);
    run.use_config_override = 1;
    run.config_override = ControllerConfig_Load<BACKTEST_FP>(config_path);
    run.config_override.engine_mode = engine_mode_value;
    // PARITY-030 regression cohort (Phase C.6): override pay_fees_in_bnb when
    // operator passes --pay-fees-in-bnb. Value < 0 = inherit cfg setting.
    if (pay_fees_in_bnb_override >= 0) {
        run.config_override.pay_fees_in_bnb = (pay_fees_in_bnb_override != 0) ? 1 : 0;
    }
    run.collect_features = 1;
    run.label_type = LABEL_WIN_LOSS;
    run.label_tp_pct = 1.5;
    run.label_sl_pct = 1.0;
    run.label_forward_ticks = 1000;

    int progress = 0;
    int cancel = 0;
    Backtest_Run(out_results, &run, &progress, &cancel, NULL);
    return out_results->sample_count;
}

int main(int argc, char** argv) {
    // v5.11.0.A — Match engine's MXCSR state so parity harness compares
    // feature output under identical FP regime (no subnormal-region divergence).
    tt::engine_set_mxcsr_ftz_daz();

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <tick_file.csv> [config_file] [--tolerance 1e-6]\n"
                "                                       [--pay-fees-in-bnb 0|1]\n"
                "\n"
                "Runs the same tick file through both engine_mode=single_core\n"
                "(legacy) and engine_mode=sharded paths, then diffs the\n"
                "feature_matrix output + cross-path total_fees equality.\n"
                "Exit 0 = parity. Exit 1 = drift.\n",
                argv[0]);
        return 2;
    }
    const char* tick_file   = argv[1];
    const char* config_path = (argc >= 3 && argv[2][0] != '-') ? argv[2] : "backtest.cfg";
    double tolerance = 1e-6;
    int    pay_fees_in_bnb_override = -1;  // -1 = inherit cfg; 0/1 = override
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--tolerance") == 0) {
            tolerance = atof(argv[i + 1]);
        } else if (strcmp(argv[i], "--pay-fees-in-bnb") == 0) {
            pay_fees_in_bnb_override = atoi(argv[i + 1]);
        }
    }

    fprintf(stderr, "[parity-harness] tick_file        = %s\n", tick_file);
    fprintf(stderr, "[parity-harness] config           = %s\n", config_path);
    fprintf(stderr, "[parity-harness] tolerance        = %.1e\n", tolerance);
    if (pay_fees_in_bnb_override >= 0) {
        fprintf(stderr, "[parity-harness] pay_fees_in_bnb  = %d (override)\n",
                pay_fees_in_bnb_override);
    }

    fprintf(stderr, "[parity-harness] running LEGACY (engine_mode=single_core)...\n");
    BacktestResults legacy;
    int n_legacy = run_one_path(&legacy, tick_file, config_path, ENGINE_MODE_SINGLE_CORE,
                                 pay_fees_in_bnb_override);
    fprintf(stderr, "[parity-harness] legacy: %d samples\n", n_legacy);

    fprintf(stderr, "[parity-harness] running SHARDED (engine_mode=sharded)...\n");
    BacktestResults sharded;
    int n_sharded = run_one_path(&sharded, tick_file, config_path, ENGINE_MODE_SHARDED,
                                  pay_fees_in_bnb_override);
    fprintf(stderr, "[parity-harness] sharded: %d samples\n", n_sharded);

    int passed = 1;
    int sample_diff = abs(n_legacy - n_sharded);
    if (sample_diff > 1) {
        fprintf(stderr, "[parity-harness] SAMPLE COUNT MISMATCH: legacy=%d sharded=%d (diff=%d > 1)\n",
                n_legacy, n_sharded, sample_diff);
        passed = 0;
    } else if (sample_diff == 1) {
        fprintf(stderr, "[parity-harness] sample count diff = 1 (warmup-edge tolerance, OK)\n");
    } else {
        fprintf(stderr, "[parity-harness] sample counts match: %d\n", n_legacy);
    }

    int n = n_legacy < n_sharded ? n_legacy : n_sharded;
    int idx_mismatches = 0;
    int price_mismatches = 0;
    for (int i = 0; i < n; i++) {
        if (legacy.sample_tick_indices[i] != sharded.sample_tick_indices[i])
            idx_mismatches++;
        double dp = fabs(legacy.sample_prices[i] - sharded.sample_prices[i]);
        if (dp > 1e-9) price_mismatches++;
    }
    if (idx_mismatches > 0) {
        fprintf(stderr, "[parity-harness] tick-index mismatches: %d / %d rows\n",
                idx_mismatches, n);
        passed = 0;
    }
    if (price_mismatches > 0) {
        fprintf(stderr, "[parity-harness] price mismatches: %d / %d rows\n",
                price_mismatches, n);
        passed = 0;
    }

    DiffStats diff;
    compute_diff_stats(&legacy, &sharded, tolerance, &diff);
    fprintf(stderr, "\n[parity-harness] feature_matrix diff:\n");
    fprintf(stderr, "  rows compared:           %d\n", diff.rows_compared);
    fprintf(stderr, "  rows with any drift:     %d (%.2f%%)\n",
            diff.rows_with_any_diff,
            diff.rows_compared > 0
                ? 100.0 * diff.rows_with_any_diff / diff.rows_compared : 0.0);
    if (diff.first_diff_row >= 0) {
        fprintf(stderr, "  first differing row:     %d\n", diff.first_diff_row);
    }
    fprintf(stderr, "\n  per-feature breakdown (only features with drift shown):\n");
    int any_feature_drift = 0;
    for (int j = 0; j < MODEL_NUM_FEATURES; j++) {
        if (diff.per_feat_diff_count[j] > 0) {
            any_feature_drift = 1;
            fprintf(stderr, "    %-20s rows=%d  max_abs=%.3e  max_rel=%.3e\n",
                    kFeatNames[j], diff.per_feat_diff_count[j],
                    diff.per_feat_max_abs_err[j],
                    diff.per_feat_max_rel_err[j]);
        }
    }
    if (!any_feature_drift)
        fprintf(stderr, "    (no per-feature drift detected)\n");

    if (any_feature_drift) passed = 0;

    if (diff.first_diff_row >= 0 && diff.first_diff_row < n) {
        int i = diff.first_diff_row;
        fprintf(stderr, "\n[parity-harness] first-diff-row dump (row %d):\n", i);
        fprintf(stderr, "  tick_index legacy=%d sharded=%d\n",
                legacy.sample_tick_indices[i], sharded.sample_tick_indices[i]);
        fprintf(stderr, "  price      legacy=%.4f sharded=%.4f\n",
                legacy.sample_prices[i], sharded.sample_prices[i]);
        for (int j = 0; j < MODEL_NUM_FEATURES; j++) {
            double a = legacy.feature_matrix[i * MODEL_NUM_FEATURES + j];
            double b = sharded.feature_matrix[i * MODEL_NUM_FEATURES + j];
            if (fabs(a - b) > 1e-12) {
                fprintf(stderr, "    %-20s legacy=%+.6e sharded=%+.6e diff=%+.3e\n",
                        kFeatNames[j], a, b, b - a);
            }
        }
    }

    // PARITY-030 cross-path total_fees equality check (Phase C.6 PARTIAL
    // closure; per-fill o->pre_resolved.fee_rate assertion deferred to
    // .F.5.C — requires on_fill hook in ShardedBacktestDriver + Order
    // pre_resolved exposure that exceed parity_harness scope). Cross-path
    // total_fees BPS equality is the aggregate verification: if BNB
    // discount applied asymmetrically across legacy/sharded paths, totals
    // would diverge. BPS tolerance = 1e-4 (0.01 BPS = sub-cent on $100
    // total_fees aggregate).
    fprintf(stderr, "\n[parity-harness] total_fees cross-path equality check:\n");
    fprintf(stderr, "  legacy.stats.total_fees  = %.4f\n", legacy.stats.total_fees);
    fprintf(stderr, "  sharded.stats.total_fees = %.4f\n", sharded.stats.total_fees);
    double fee_abs_diff = fabs(legacy.stats.total_fees - sharded.stats.total_fees);
    double fee_mag = fmax(fabs(legacy.stats.total_fees), fabs(sharded.stats.total_fees));
    double fee_rel_diff = (fee_mag > 1e-12) ? (fee_abs_diff / fee_mag) : fee_abs_diff;
    const double FEE_BPS_TOL = 1e-4;
    if (fee_rel_diff > FEE_BPS_TOL) {
        fprintf(stderr,
                "  FAIL — total_fees diverged (abs=%.6e rel=%.6e > %.0e)\n",
                fee_abs_diff, fee_rel_diff, FEE_BPS_TOL);
        passed = 0;
    } else if (fee_mag > 1e-12) {
        fprintf(stderr, "  PASS — total_fees BPS equality (rel=%.3e)\n", fee_rel_diff);
    } else {
        fprintf(stderr, "  (both zero — no fills under this cfg/tick combo)\n");
    }

    BacktestResults_Free(&legacy);
    BacktestResults_Free(&sharded);

    if (passed) {
        fprintf(stderr, "\n[parity-harness] PASS — features + total_fees match within tolerance\n");
        return 0;
    } else {
        fprintf(stderr, "\n[parity-harness] FAIL — drift detected (see above)\n");
        return 1;
    }
}
