// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/LinearRegressionSimple.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[single-feature fixed-point least-squares — the original learning implementation; Fit + Predict]
// [CONTAINS]
//   - [FUNCTION]_[LinearRegression_Fit]   (+ Predict shares the file)
//======================================================================================================
// oh yeah its gonna be fixed point boiiiiiii, this is just a simple implementation, with only a single feature, im learning, for the more advanced one it will probably use a struct to just pack all the features in idk yet, im learning and trying to imagine ways to use this, so w/e, if you see me IRL it would make my day if you told me you love me, and remember class, java is bad
//======================================================================================================
#ifndef LINEAR_REGRESSION_H
#define LINEAR_REGRESSION_H

#include "../FixedPoint/FixedPointN.hpp"
//======================================================================
// [FUNCTION]_[LinearRegression_Fit]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[least-squares slope/intercept over FPN arrays (branchless zero-denominator guard); LinearRegressionModel + Predict share the section]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F> struct LinearRegressionModel {
    FPN_Binary<F> slope;
    FPN_Binary<F> intercept;
};
//------------------------------------------------------------------------------
// [SECTION]_[LINEAR REGRESSION FUNCTION PROTOTYPES]
//------------------------------------------------------------------------------
template <unsigned F> inline LinearRegressionModel<F> LinearRegression_Fit(FPN_Binary<F> *x_values, FPN_Binary<F> *y_values, int count) {
    using FP = FPN_Binary<F>;
    LinearRegressionModel<F> model;

    FP sum_x = FPN_Zero<F>(), sum_y = FPN_Zero<F>();
    for (int i = 0; i < count; i++) {
        sum_x = FPN_Add(sum_x, x_values[i]);
        sum_y = FPN_Add(sum_y, y_values[i]);
    }
    FP n_fp   = FPN_FromDouble<F>((double)count);
    FP mean_x = FPN_DivNoAssert(sum_x, n_fp);
    FP mean_y = FPN_DivNoAssert(sum_y, n_fp);

    FP numerator = FPN_Zero<F>(), denominator = FPN_Zero<F>();
    for (int i = 0; i < count; i++) {
        FP x_diff   = FPN_Sub(x_values[i], mean_x);
        FP y_diff   = FPN_Sub(y_values[i], mean_y);
        numerator   = FPN_Add(numerator, FPN_Mul(x_diff, y_diff));
        denominator = FPN_Add(denominator, FPN_Mul(x_diff, x_diff));
    }

    int denom_nonzero = !FPN_IsZero(denominator);
    FP safe_denom     = denom_nonzero ? denominator : FPN_FromDouble<F>(1.0);
    FP raw_slope      = FPN_DivNoAssert(numerator, safe_denom);
    model.slope       = denom_nonzero ? raw_slope : FPN_Zero<F>();

    model.intercept = FPN_Sub(mean_y, FPN_Mul(model.slope, mean_x));

    return model;
}

template <unsigned F> inline FPN_Binary<F> LinearRegression_Predict(LinearRegressionModel<F> model, FPN_Binary<F> x) {
    return FPN_Add(FPN_Mul(model.slope, x), model.intercept);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[LinearRegression_Fit]
//======================================================================
#endif
