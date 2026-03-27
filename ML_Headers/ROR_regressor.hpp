// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ROR REGRESSOR]
//======================================================================================================
// regression on regression - takes the slope outputs from LinearRegression3X and runs a second
// regression on them, so instead of y=price x=time, its y=slope x=time, which tells you if the
// trend itself is accelerating, decelerating, or reversing, the slope-of-slopes is basically
// the second derivative of price, same way velocity vs acceleration works in physics
//======================================================================================================
#ifndef ROR_REGRESSOR_H
#define ROR_REGRESSOR_H

#include "LinearRegression3X.hpp"
//======================================================================================================
// [ROR REGRESSOR STRUCTURE]
//======================================================================================================
// same ring buffer pattern as RegressionFeederX, but instead of storing raw prices it stores the
// slope values that come out of the inner regression, each time you run _Compute on the inner
// regression you push the resulting slope into this buffer, once it fills up you can run regression
// on the slopes themselves to get the trend of the trend
//======================================================================================================
template<unsigned F>
struct RORRegressor {
    FPN<F> slope_samples[MAX_WINDOW];
    FPN<F> r_squared_samples[MAX_WINDOW];
    int head;
    int count;
};
//======================================================================================================
// [INIT FUNCTION]
//======================================================================================================
// zeroes everything out, call once at startup
//======================================================================================================
template<unsigned F>
inline RORRegressor<F> RORRegressor_Init() {
    RORRegressor<F> reg;

    for (int i = 0; i < MAX_WINDOW; i++) {
        reg.slope_samples[i]     = FPN_Zero<F>();
        reg.r_squared_samples[i] = FPN_Zero<F>();
    }

    reg.head  = 0;
    reg.count = 0;

    return reg;
}
//======================================================================================================
// [PUSH FUNCTION]
//======================================================================================================
// takes a LinearRegression3XResult from the inner regression and stores the slope into the ring
// buffer, also stores the r_squared so you could filter out bad fits later if you wanted to,
// like if the inner regression had an r^2 of 0.05 that slope is basically noise and maybe you
// dont want it polluting the outer regression
//======================================================================================================
template<unsigned F>
inline void RORRegressor_Push(RORRegressor<F> *reg, LinearRegression3XResult<F> inner_result) {
    reg->slope_samples[reg->head]     = inner_result.model.slope;
    reg->r_squared_samples[reg->head] = inner_result.r_squared;
    reg->head                         = (reg->head + 1) & (MAX_WINDOW - 1);
    reg->count += (reg->count < MAX_WINDOW);
}
//======================================================================================================
// [COMPUTE FUNCTION]
//======================================================================================================
// linearizes the slope ring buffer oldest-to-newest and runs LinearRegression3X_Fit on it, same
// approach as RegressionFeederX_Compute but the y values are slopes instead of prices, the output
// slope tells you how fast the trend is changing - positive means the trend is getting steeper
// (accelerating), negative means its flattening or reversing
//======================================================================================================
template<unsigned F>
inline LinearRegression3XResult<F> RORRegressor_Compute(RORRegressor<F> *reg) {
    FPN<F> linearized[MAX_WINDOW];
    FPN<F> time_index[MAX_WINDOW];

    for (int i = 0; i < reg->count; i++) {
        int idx       = (reg->head - reg->count + i + MAX_WINDOW) & (MAX_WINDOW - 1);
        linearized[i] = reg->slope_samples[idx];
        time_index[i] = FPN_FromDouble<F>((double)i);
    }

    return LinearRegression3X_Fit(time_index, linearized, reg->count);
}
//======================================================================================================
// [DATA FLOW]
//======================================================================================================
// [price ticks] -> [RegressionFeederX_Push] -> [RegressionFeederX_Compute] -> [slope/r^2]
//                                                                                  |
//                                                                           [RORRegressor_Push]
//                                                                                  |
//                                                                           [RORRegressor_Compute]
//                                                                                  |
//                                                                           [slope-of-slopes]
//                                                                                  |
//                                                          positive = trend accelerating (momentum)
//                                                          negative = trend decelerating (reversal?)
//                                                          near zero = trend is steady
//======================================================================================================
#endif // ROR_REGRESSOR_H
