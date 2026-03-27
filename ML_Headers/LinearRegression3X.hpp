// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [LINEAR REGRESSION 3X]
//======================================================================================================
// this is just a basic time and price regression model, im gonna keep iterating on expadning these as i learn more about them
//======================================================================================================
// [EDIT [14-03-26]]
//======================================================================================================
// swapped from FP16 to FP32 for price headroom, Q16.16 overflows on prices above ~200 when
// accumulating sums of squares, Q32.32 handles prices in the thousands with no issues, the
// tradeoff is mul/div go through __int128_t instead of int64_t which is one extra cycle
//======================================================================================================
// [EDIT [14-03-26]]
//======================================================================================================
// templated on FP precision - engine code picks the width, all the math flows through FixedPointN
//======================================================================================================
#ifndef LINEAR_REGRESSION_3X_H
#define LINEAR_REGRESSION_3X_H

#include "../FixedPoint/FixedPointN.hpp"

#ifndef MAX_WINDOW
#define MAX_WINDOW 8
#endif
//======================================================================================================
// [LINEAR REGRESSION STRUCTURES]
//======================================================================================================
// [LinearRegression3XModel]
//======================================================================================================
// this is basically the equation y = mx + b, slope is m, intercept is b, and these numbers give you the slope, we track them in a struct for better organization, and the slope means on average that the price changes by this much per interval
//======================================================================================================
template <unsigned F> struct LinearRegression3XModel {
    FPN<F> slope;
    FPN<F> intercept;
};
//======================================================================================================
// [FEEDER STRUCT]
//======================================================================================================
// this struct just has where there data lives before the regression takes place, apparently this is called a ring buffer, it holds the last 8 price sampels which is from the [MAX_WINDOW], head is where the next write goes, and count tracks which sample your on within the sample window, you can extend this for more intervals, but then the assert size changes, ill probably look into making that confirgurable outside of the file and based on dynamic observations but for now this works as i learn it, this also means that once the buffer gets full, it will start overwriting the oldest data point, which creates like a moving average for the slope, so basically it gives you the most recent output for the 8 prices
//======================================================================================================
template <unsigned F> struct RegressionFeederX {
    FPN<F> price_samples[MAX_WINDOW];
    int head;
    int count;
};
//======================================================================================================
// [RESULT STRUCT]
//======================================================================================================
// this is the fitted line, (slope + intercept), plus the r^2 value, which is just a measure of how well the line fits the data, where 1.0 is perfect, and 0, is no relationship at all, ill probably go more into stuff like overfitting and stuff later, but generally you cant trust extreme values, like most things with stocks will probably fall within like .3 -> .7, and even then, .7 is still unusually high
//======================================================================================================
template <unsigned F> struct LinearRegression3XResult {
    LinearRegression3XModel<F> model;
    FPN<F> r_squared;
};
//======================================================================================================
// [LINEAR REGRESSION FUNCTION PROTOTYPES]
//======================================================================================================
// [FEEDER FUNCTION]
//======================================================================================================
// just creates a clean feeder with everything zerod out, you call this once at startup and basically only then
//======================================================================================================
template <unsigned F> inline RegressionFeederX<F> RegressionFeederX_Init() {
    RegressionFeederX<F> feeder;

    for (int i = 0; i < MAX_WINDOW; i++) {
        feeder.price_samples[i] = FPN_Zero<F>();
    }

    feeder.head  = 0;
    feeder.count = 0;

    return feeder;
}
//======================================================================================================
// [PUSH FUNCTION]
//======================================================================================================
// this writes the new price at the current head position, and it advances with a wrap,so that after it hits the max window length, it starts wrapping around, the count just increments until youve filled the window, and at 8 it just starts overwriting the older value, in short the data ingestion path
//======================================================================================================
template <unsigned F> inline void RegressionFeederX_Push(RegressionFeederX<F> *feeder, FPN<F> price) {
    feeder->price_samples[feeder->head] = price;
    feeder->head                        = (feeder->head + 1) & (MAX_WINDOW - 1); // branchless wrap, power-of-2 bitmask instead of modulo
    feeder->count += (feeder->count < MAX_WINDOW); // branchless saturate, comparison yields 0 or 1, so it stops incrementing at MAX_WINDOW
}
//======================================================================================================
// [COMPUTE FUNCTION]
//======================================================================================================
// this just finds the best fit line throughout the data points, its basically what would be the SMA or a moving average, but for a linear regression, it implements ordinary least squares, which are the the operators in the for loop, it makes one pass through the data, accumulats 5 sums, and the sum_x and sum_y are just totals, and sum_xy is the sum of each x times its corresponding y, it basically caoutes the relationship between x and y, which in this case are like price and time
//
// then we get to the part with n_fp, and this part is just the count converted to a fixed point, numerator is just the covariance between x and y(how much they move together), and the denominator is the variance of x(how spread out the values are), and then these 2 values are shared between the slope and intercept formulas
//
// then the slope part is literally just how much does y change per unit of x, and the magnintude tells you how fast
//
// the result.model_intercept is just the value where y crosses zero, combined with the slope, you can now predict y for any x
//
// ss_total is the total variance in y, which is just how much the prices vary overall, ss_reg is how much of that the variance the regression line explains, and r^2 is just ss_reg / ss_total, which basically means "what fraction of the price movement is explained by the trend", oh thats cool, its literally a ratio of the relationship converted to a percentage, this stuff is neat to finally dig into, its such a shame that college classes feel like funeral processions, this sshit is actually cool when its applied to stuff outside of school, but college is just like "dot your t's and cross your i's and make sure you answer these problems which have no actual meaning", but thats a different tangent, and you arnt reading this for my opinions on how lacking alot of CS education actually is
//======================================================================================================
template <unsigned F> inline LinearRegression3XResult<F> LinearRegression3X_Fit(FPN<F> *x_values, FPN<F> *y_values, int count) {
    using FP = FPN<F>;

    LinearRegression3XResult<F> result;
    result.model.slope     = FPN_Zero<F>();
    result.model.intercept = FPN_Zero<F>();
    result.r_squared       = FPN_Zero<F>();

    // no early return for count < 2, if count is 0 or 1 the denominator ends up 0 and the branchless
    // zero-denom guard below handles it, the sums just naturally produce a degenerate case

    FP sum_x = FPN_Zero<F>(), sum_y = FPN_Zero<F>(), sum_xy = FPN_Zero<F>(), sum_x2 = FPN_Zero<F>(),
       sum_y2 = FPN_Zero<F>();
    for (int i = 0; i < count; i++) {
        sum_x  = FPN_AddSat(sum_x, x_values[i]);
        sum_y  = FPN_AddSat(sum_y, y_values[i]);
        sum_xy = FPN_AddSat(sum_xy, FPN_Mul(x_values[i], y_values[i]));
        sum_x2 = FPN_AddSat(sum_x2, FPN_Mul(x_values[i], x_values[i]));
        sum_y2 = FPN_AddSat(sum_y2, FPN_Mul(y_values[i], y_values[i]));
    }

    FP n_fp        = FPN_FromDouble<F>((double)count);
    FP numerator   = FPN_SubSat(FPN_Mul(n_fp, sum_xy), FPN_Mul(sum_x, sum_y));
    FP denominator = FPN_SubSat(FPN_Mul(n_fp, sum_x2), FPN_Mul(sum_x, sum_x));

    // branchless division guard: if denom is zero, DivNoAssert saturates to max and we zero the
    // result with a mask, so the division always executes but the result is correct either way
    int denom_nonzero = !FPN_IsZero(denominator);
    FP safe_denom     = denom_nonzero ? denominator : FPN_FromDouble<F>(1.0);

    FP raw_slope       = FPN_DivNoAssert(numerator, safe_denom);
    result.model.slope = denom_nonzero ? raw_slope : FPN_Zero<F>();

    FP raw_intercept       = FPN_DivNoAssert(FPN_SubSat(FPN_Mul(sum_y, sum_x2), FPN_Mul(sum_x, sum_xy)), safe_denom);
    result.model.intercept = denom_nonzero ? raw_intercept : FPN_Zero<F>();

    // r^2 = slope * (numerator / ss_total), reuses slope to avoid squaring large values
    // splits the fraction so both operands in the final mul are small, no overflow
    FP ss_total       = FPN_SubSat(FPN_Mul(n_fp, sum_y2), FPN_Mul(sum_y, sum_y));
    int total_nonzero = (!FPN_IsZero(ss_total)) & denom_nonzero;
    FP safe_total     = total_nonzero ? ss_total : FPN_FromDouble<F>(1.0);
    FP raw_r2         = FPN_Mul(result.model.slope, FPN_DivNoAssert(numerator, safe_total));
    result.r_squared  = total_nonzero ? raw_r2 : FPN_Zero<F>();

    return result;
}
//======================================================================================================
// [COMPUTER FUNCTION]
//======================================================================================================
// this is just run the regression on the ring buffer, it doesnt store it in chronological order, so you have to walk from the oldest to the newest and copy them into the linearized in the correct time order, then it just hands the arrays to the _Fit and computes, theres probably a better and faster way to do this but idk, im figuring this out as a go, because FUCK java(I C K Y)
//======================================================================================================
template <unsigned F> inline LinearRegression3XResult<F> RegressionFeederX_Compute(RegressionFeederX<F> *feeder) {
    // no early return needed, _Fit handles count < 2 branchlessly via the zero-denom guard

    FPN<F> linearized[MAX_WINDOW];
    FPN<F> time_index[MAX_WINDOW];

    for (int i = 0; i < feeder->count; i++) {
        int idx       = (feeder->head - feeder->count + i + MAX_WINDOW) & (MAX_WINDOW - 1); // branchless wrap
        linearized[i] = feeder->price_samples[idx];
        time_index[i] = FPN_FromDouble<F>((double)i);
    }

    return LinearRegression3X_Fit(time_index, linearized, feeder->count);
}
//======================================================================================================
// [PREDICTION FUNCTION]
//======================================================================================================
// this is just given the line what is the y at the assocaited x, literally just y = mx + b, an oldie but a goodie
//======================================================================================================
template <unsigned F> inline FPN<F> LinearRegression3X_Predict(LinearRegression3XModel<F> model, FPN<F> x) {
    return FPN_AddSat(FPN_Mul(model.slope, x), model.intercept);
}
//======================================================================================================
// [DATA FLOW]
//======================================================================================================
// [market tick] -> [_Push] -> [ring buffer fills] -> [_Compute] -> [linearize + _Fit] -> [slope/intercept/r^2] -> [_Predict for future price] -> [compare to actual] -> [trading signal]
//======================================================================================================
// damn this stuff is actualyl really cool digging into the lower level stuff, like the overall architecture in FoxML was interesting to build with ai, but actually learning how this stuff works is actually really interesting tbh, like idk, it makes my brain happy, even if it is relatively simple to whats actually out there, and it feels more satisfying than just calling scikit_learn.fit() or whatever the lib call is
//======================================================================================================
#endif // LINEAR_REGRESSION_3X_H
