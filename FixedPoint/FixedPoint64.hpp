// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FIXED-POINT ARITHMETIC LIBRARY - 64-BIT FRACTIONAL PART]
//======================================================================================================
// Q64.64 format: sign-magnitude with __uint128_t magnitude, separate sign flag
// integer range: 0 to ~18.4 quintillion (full uint64_t range in the integer part)
// fractional precision: ~19 decimal digits
// mul uses partial products since theres no native 256-bit type
//======================================================================================================
#ifndef FIXED_POINT_64_H
#define FIXED_POINT_64_H

#define FP64_FRAC_BITS 64

#include <stdint.h>
#include <assert.h>
#include <math.h>

//======================================================================================================
// [FIXED-POINT NUMBER REPRESENTATION]
//======================================================================================================
typedef struct {
    __uint128_t magnitude;
    int32_t sign; // 0 = positive/zero, 1 = negative
} FP64;
static_assert(sizeof(FP64) == 32, "FP64 must be 32 bytes");
//======================================================================================================
// [DOUBLE PRECISION FIXED-POINT ARITHMETIC]
//======================================================================================================
static inline FP64 FP64_FromDouble(double input) {
    int32_t neg      = (input < 0.0);
    double abs_input = input * (1.0 - 2.0 * neg);
    FP64 result;

    double int_part  = floor(abs_input);
    double frac_part = abs_input - int_part;
    __uint128_t hi   = (__uint128_t)(uint64_t)int_part << FP64_FRAC_BITS;
    __uint128_t lo   = (__uint128_t)(uint64_t)(frac_part * 18446744073709551616.0);
    result.magnitude = hi + lo;
    result.sign      = neg & (result.magnitude != 0);
    return result;
}

static inline double FP64_ToDouble(FP64 value) {
    uint64_t int_part  = (uint64_t)(value.magnitude >> FP64_FRAC_BITS);
    uint64_t frac_part = (uint64_t)value.magnitude;
    double mag         = (double)int_part + (double)frac_part / 18446744073709551616.0;
    return mag * (1.0 - 2.0 * value.sign);
}
//======================================================================================================
// [GUARDS]
//======================================================================================================
static inline FP64 FP64_Min(FP64 a, FP64 b) {
    int diff_sign = a.sign ^ b.sign;
    int a_lt_same = ((a.magnitude < b.magnitude) ^ a.sign) & (a.magnitude != b.magnitude);
    int a_lt      = (a.sign & diff_sign) | (a_lt_same & !diff_sign);
    a_lt &= !((a.magnitude == 0) & (b.magnitude == 0));
    return a_lt ? a : b;
}

static inline FP64 FP64_Max(FP64 a, FP64 b) {
    int diff_sign = a.sign ^ b.sign;
    int a_gt_same = ((a.magnitude > b.magnitude) ^ a.sign) & (a.magnitude != b.magnitude);
    int a_gt      = ((!a.sign) & diff_sign) | (a_gt_same & (!diff_sign));
    return a_gt ? a : b;
}
//======================================================================================================
// [FIXED-POINT ARITHMETIC OPERATIONS]
//======================================================================================================
static inline FP64 FP64_AddSat(FP64 a, FP64 b) {
    int diff = a.sign ^ b.sign;

    // split into 64-bit halves so GCC can use cmov/sbb instead of branching on __uint128_t
    uint64_t a_lo = (uint64_t)a.magnitude, a_hi = (uint64_t)(a.magnitude >> 64);
    uint64_t b_lo = (uint64_t)b.magnitude, b_hi = (uint64_t)(b.magnitude >> 64);

    // same sign path: add magnitudes
    uint64_t sum_lo       = a_lo + b_lo;
    uint64_t carry        = (sum_lo < a_lo);
    uint64_t sum_hi       = a_hi + b_hi + carry;
    uint64_t sum_overflow = (sum_hi < a_hi) | ((sum_hi == a_hi) & carry & (b_hi != 0));
    // simpler overflow: the 128-bit add overflowed if the high part wrapped
    sum_overflow = (sum_hi < a_hi + carry) | ((a_hi + carry < a_hi) & (sum_hi <= b_hi));
    // just check carry out of the full 128-bit add
    __uint128_t sum_full = a.magnitude + b.magnitude;
    sum_overflow         = (sum_full < a.magnitude);
    uint64_t sat_mask    = -(uint64_t)sum_overflow;
    uint64_t sat_lo      = sum_lo | sat_mask;
    uint64_t sat_hi      = sum_hi | sat_mask;

    // different sign path: both subtractions computed, mask selects
    int a_ge         = (a_hi > b_hi) | ((a_hi == b_hi) & (a_lo >= b_lo));
    uint64_t ge_mask = -(uint64_t)a_ge;

    uint64_t borrow_ab = (a_lo < b_lo);
    uint64_t dab_lo    = a_lo - b_lo;
    uint64_t dab_hi    = a_hi - b_hi - borrow_ab;

    uint64_t borrow_ba = (b_lo < a_lo);
    uint64_t dba_lo    = b_lo - a_lo;
    uint64_t dba_hi    = b_hi - a_hi - borrow_ba;

    uint64_t diff_lo = (dab_lo & ge_mask) | (dba_lo & ~ge_mask);
    uint64_t diff_hi = (dab_hi & ge_mask) | (dba_hi & ~ge_mask);

    // select between same-sign (sat_sum) and diff-sign (diff) paths
    uint64_t d_mask = -(uint64_t)diff;
    uint64_t r_lo   = (sat_lo & ~d_mask) | (diff_lo & d_mask);
    uint64_t r_hi   = (sat_hi & ~d_mask) | (diff_hi & d_mask);

    // select sign
    int diff_sign_result = (a.sign & a_ge) | (b.sign & (1 - a_ge));
    int result_sign      = (a.sign & (!diff)) | (diff_sign_result & diff);
    result_sign &= ((r_lo | r_hi) != 0);

    __uint128_t result_mag = ((__uint128_t)r_hi << 64) | r_lo;
    return (FP64){.magnitude = result_mag, .sign = result_sign};
}

static inline FP64 FP64_SubSat(FP64 a, FP64 b) {
    FP64 neg_b = {.magnitude = b.magnitude, .sign = b.sign ^ (b.magnitude != 0)};
    return FP64_AddSat(a, neg_b);
}
//======================================================================================================
// [MULTIPLY - PARTIAL PRODUCT APPROACH]
//======================================================================================================
// split each __uint128_t into two uint64 halves and do 4 partial multiplies
// then shift right by 64 frac bits - we need bits [191:64] of the full 256-bit product
// sign is just XOR of the two sign flags
//======================================================================================================
static inline FP64 FP64_Mul(FP64 a, FP64 b) {
    FP64 result;

    uint64_t a_lo = (uint64_t)a.magnitude;
    uint64_t a_hi = (uint64_t)(a.magnitude >> 64);
    uint64_t b_lo = (uint64_t)b.magnitude;
    uint64_t b_hi = (uint64_t)(b.magnitude >> 64);

    __uint128_t ll = (__uint128_t)a_lo * b_lo;
    __uint128_t lh = (__uint128_t)a_lo * b_hi;
    __uint128_t hl = (__uint128_t)a_hi * b_lo;
    __uint128_t hh = (__uint128_t)a_hi * b_hi;

    // we want bits [191:64] of the 256-bit result
    __uint128_t mid     = lh + hl + (ll >> 64);
    __uint128_t shifted = hh + (mid >> 64);

    __uint128_t result_mag = (shifted << 64) | ((uint64_t)mid);

    // overflow: if shifted has bits above 127, saturate
    int overflow        = (shifted >> 64) != 0;
    __uint128_t of_mask = -(__uint128_t)overflow;
    result.magnitude    = (result_mag & ~of_mask) | ((__uint128_t)(-1) & of_mask);

    result.sign = (a.sign ^ b.sign) & (result.magnitude != 0);
    return result;
}
//======================================================================================================
// [DIVISION]
//======================================================================================================
// goes through long double since theres no 256-bit / 128-bit hardware divide
// long double on x86-64 is 80-bit extended precision (~18-19 significant digits)
// sign is just XOR of sign flags
//======================================================================================================
static inline FP64 FP64_DivNoAssert(FP64 a, FP64 b) {
    FP64 result;
    if (b.magnitude == 0) {
        result.magnitude = (__uint128_t)(-1);
        result.sign      = a.sign;
        return result;
    }
    long double fa = (long double)FP64_ToDouble(a);
    long double fb = (long double)FP64_ToDouble(b);
    // compute absolute quotient through doubles, apply sign separately
    if (fa < 0)
        fa = -fa;
    if (fb < 0)
        fb = -fb;
    long double fq = fa / fb;
    result         = FP64_FromDouble((double)fq);
    result.sign    = (a.sign ^ b.sign) & (result.magnitude != 0);
    return result;
}

static inline FP64 FP64_DivWithAssert(FP64 a, FP64 b) {
    assert(b.magnitude != 0);
    return FP64_DivNoAssert(a, b);
}

//======================================================================================================
// [FIXED-POINT COMPARISON OPERATIONS]
//======================================================================================================
static inline int FP64_Equal(FP64 a, FP64 b) {
    int both_zero = (a.magnitude == 0) & (b.magnitude == 0);
    return both_zero | ((a.magnitude == b.magnitude) & (a.sign == b.sign));
}

static inline int FP64_NotEqual(FP64 a, FP64 b) {
    return !FP64_Equal(a, b);
}

static inline int FP64_LessThan(FP64 a, FP64 b) {
    int both_zero   = (a.magnitude == 0) & (b.magnitude == 0);
    int diff_sign   = a.sign ^ b.sign;
    int diff_result = a.sign & !both_zero;
    int same_result = ((a.magnitude < b.magnitude) ^ a.sign) & (a.magnitude != b.magnitude);
    return (diff_result & diff_sign) | (same_result & !diff_sign);
}

static inline int FP64_LessThanOrEqual(FP64 a, FP64 b) {
    return FP64_LessThan(a, b) | FP64_Equal(a, b);
}

static inline int FP64_GreaterThan(FP64 a, FP64 b) {
    return FP64_LessThan(b, a);
}

static inline int FP64_GreaterThanOrEqual(FP64 a, FP64 b) {
    return !FP64_LessThan(a, b);
}
//======================================================================================================
// [FIXED-POINT UTILITY FUNCTIONS]
//======================================================================================================
static inline FP64 FP64_Negate(FP64 value) {
    return (FP64){.magnitude = value.magnitude, .sign = value.sign ^ (value.magnitude != 0)};
}

static inline int FP64_IsZero(FP64 value) {
    return value.magnitude == 0;
}

static inline FP64 FP64_Abs(FP64 value) {
    return (FP64){.magnitude = value.magnitude, .sign = 0};
}

static inline FP64 FP64_Sign(FP64 value) {
    __uint128_t one = (__uint128_t)1 << FP64_FRAC_BITS;
    int is_nonzero  = (value.magnitude != 0);
    __uint128_t mag = is_nonzero ? one : 0;
    return (FP64){.magnitude = mag, .sign = value.sign & is_nonzero};
}
//======================================================================================================
// [FIXED-POINT MATH FUNCTIONS]
//======================================================================================================
static inline FP64 FP64_Sqrt(FP64 value) {
    assert(value.sign == 0 || value.magnitude == 0);
    return FP64_FromDouble(sqrt(FP64_ToDouble(value)));
}

static inline FP64 FP64_InvSqrt(FP64 value) {
    assert(value.sign == 0 && value.magnitude != 0);
    double x = FP64_ToDouble(value);
    return FP64_FromDouble(1.0 / sqrt(x));
}

static inline FP64 FP64_Sin(FP64 value) {
    return FP64_FromDouble(sin(FP64_ToDouble(value)));
}

static inline FP64 FP64_Cos(FP64 value) {
    return FP64_FromDouble(cos(FP64_ToDouble(value)));
}

static inline FP64 FP64_Tan(FP64 value) {
    return FP64_FromDouble(tan(FP64_ToDouble(value)));
}

static inline FP64 FP64_Atan2(FP64 y, FP64 x) {
    return FP64_FromDouble(atan2(FP64_ToDouble(y), FP64_ToDouble(x)));
}

static inline FP64 FP64_Exp(FP64 value) {
    return FP64_FromDouble(exp(FP64_ToDouble(value)));
}

static inline FP64 FP64_Log(FP64 value) {
    assert(value.sign == 0 && value.magnitude != 0);
    return FP64_FromDouble(log(FP64_ToDouble(value)));
}

static inline FP64 FP64_Pow(FP64 base, FP64 exponent) {
    return FP64_FromDouble(pow(FP64_ToDouble(base), FP64_ToDouble(exponent)));
}
//======================================================================================================
// [FIXED-POINT MISCELLANEOUS FUNCTIONS]
//======================================================================================================
static inline FP64 FP64_Floor(FP64 value) {
    __uint128_t frac_mask = ((__uint128_t)1 << FP64_FRAC_BITS) - 1;
    int has_frac          = (value.magnitude & frac_mask) != 0;
    __uint128_t trunc     = value.magnitude & ~frac_mask;
    __uint128_t bump      = (__uint128_t)(value.sign & has_frac) << FP64_FRAC_BITS;
    return (FP64){.magnitude = trunc + bump, .sign = value.sign};
}

static inline FP64 FP64_Ceil(FP64 value) {
    __uint128_t frac_mask  = ((__uint128_t)1 << FP64_FRAC_BITS) - 1;
    int has_frac           = (value.magnitude & frac_mask) != 0;
    __uint128_t trunc      = value.magnitude & ~frac_mask;
    __uint128_t bump       = (__uint128_t)((!value.sign) & has_frac) << FP64_FRAC_BITS;
    __uint128_t result_mag = trunc + bump;
    int result_sign        = value.sign & (result_mag != 0);
    return (FP64){.magnitude = result_mag, .sign = result_sign};
}

static inline FP64 FP64_Round(FP64 value) {
    __uint128_t frac_mask  = ((__uint128_t)1 << FP64_FRAC_BITS) - 1;
    __uint128_t half       = (__uint128_t)1 << (FP64_FRAC_BITS - 1);
    __uint128_t frac       = value.magnitude & frac_mask;
    __uint128_t trunc      = value.magnitude & ~frac_mask;
    int do_bump            = (frac >= half);
    __uint128_t bump       = (__uint128_t)do_bump << FP64_FRAC_BITS;
    __uint128_t result_mag = trunc + bump;
    int result_sign        = value.sign & (result_mag != 0);
    return (FP64){.magnitude = result_mag, .sign = result_sign};
}

static inline FP64 FP64_Mod(FP64 a, FP64 b) {
    assert(b.magnitude != 0);
    FP64 quotient     = FP64_DivNoAssert(a, b);
    __uint128_t frac_mask = ((__uint128_t)1 << FP64_FRAC_BITS) - 1;
    FP64 truncated    = {.magnitude = quotient.magnitude & ~frac_mask, .sign = quotient.sign};
    return FP64_SubSat(a, FP64_Mul(truncated, b));
}

static inline FP64 FP64_Lerp(FP64 a, FP64 b, FP64 t) {
    FP64 diff   = FP64_SubSat(b, a);
    FP64 scaled = FP64_Mul(diff, t);
    return FP64_AddSat(a, scaled);
}

static inline FP64 FP64_SmoothStep(FP64 edge0, FP64 edge1, FP64 x) {
    if (FP64_LessThanOrEqual(x, edge0))
        return (FP64){.magnitude = 0, .sign = 0};
    if (FP64_GreaterThanOrEqual(x, edge1))
        return (FP64){.magnitude = (__uint128_t)1 << FP64_FRAC_BITS, .sign = 0};

    FP64 t = FP64_DivNoAssert(FP64_SubSat(x, edge0), FP64_SubSat(edge1, edge0));

    FP64 three = {.magnitude = (__uint128_t)3 << FP64_FRAC_BITS, .sign = 0};
    FP64 two   = {.magnitude = (__uint128_t)2 << FP64_FRAC_BITS, .sign = 0};
    return FP64_Mul(FP64_Mul(t, t), FP64_SubSat(three, FP64_Mul(two, t)));
}
//======================================================================================================
//======================================================================================================

//======================================================================================================
#endif // FIXED_POINT_64_H
