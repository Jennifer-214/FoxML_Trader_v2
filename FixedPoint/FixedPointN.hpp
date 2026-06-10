// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FIXED-POINT ARITHMETIC LIBRARY — THE 16B BINARY CORE]
//======================================================================================================
// FPN_Binary<64> is the ONLY binary instantiation: 16B two's-complement __int128, value = v / 2^64
// (sign in the top bit; no sign field, no padding; 4 per cache line). The arbitrary-width uint64_t
// w[] generic body was SHED at Ship A (v5.15.5.F.4d.1.E.0.7); the spelling FPN -> FPN_Binary landed
// at A.5 (E.0.8, D-143/D-163). The FRAC template parameter is RETAINED for the D-129 future width work.
//
// usage:
//   FPN_Binary<64> a = FPN_FromDouble<64>(3.14);
//   FPN_Binary<64> b = FPN_FromDouble<64>(2.0);
//   FPN_Binary<64> c = FPN_Add(a, b);
//   double result = FPN_ToDouble(c);   // display-only boundary (H4)
//
// compile: g++ -std=c++17 -O2 ...
//======================================================================================================
#ifndef FIXED_POINT_N_H
#define FIXED_POINT_N_H

#include <stdint.h>
#include <assert.h>
#include <math.h>
#include <type_traits>  // v5.15.5.F.4b — std::false_type / std::true_type for the binary-domain type trait (is_fp_binary<T> since Ship A)

//======================================================================================================
// [FIXED-POINT NUMBER REPRESENTATION]
//======================================================================================================
// v5.15.5.F.4d.1.E Ship A (D-143 intent: FPN_Binary<64> = the 16B binary core). MECHANISM = full-specialization,
// NOT an alias: an alias template `using FPN_Binary = FixedPoint<2,F>` makes FPN_Binary<F> a NON-DEDUCED context, breaking
// `FPN_Op(args)` template-arg deduction across the F-generic engine (red-build caught it at CfgFieldDispatch).
// So FPN_Binary<64> is a concrete 16B specialization (layout-identical to FixedPoint<2,64>: bare __int128 v), and
// is_fp_binary is EXTENDED to cover it below (so the B6 wire-dispatchers see it as binary). The 24B
// sign-magnitude generic body is SHED — only <64> is defined; F=128 was trait-test-only (rewritten this ship).
// The old `.w[]`/`.sign` aggregate-access sites red-build → ported to `.v`.
template <unsigned F> struct FPN_Binary;                         // primary: declaration only (arbitrary width shed)
template <> struct FPN_Binary<64> {
    __int128 v;                                           // value = v / 2^64 (two's-complement; sign in the top bit)
    static constexpr unsigned F = 64;                     // T::F access for templated dispatchers (tt::cfg_*_field<T>)
};
static_assert(sizeof(FPN_Binary<64>) == 16, "Ship A: FPN_Binary<64> is the 16B two's-complement binary core (was 24B sign-mag)");

// v5.15.5.F.4b — type trait for FPN_Binary<F> detection in templated dispatchers.
// Used by tt::cfg_parse_field<T> / tt::cfg_save_field<T> / tt::cfg_render_field<T>
// (and any future templated typed-field dispatcher) to dispatch to FPN_Binary-specific
// branches (FPN_FromDouble + clamp + percent scaling) vs. raw double / int / array.
//
// Critical for closing DOCS/RECURRING_BUG_PATTERNS.md Class 23 (type-erased
// reinterpret_cast through char*+offset dispatch). Without this trait, registry-
// driven cfg dispatch can't safely distinguish FPN_Binary<F> fields from raw double
// fields, leading to silent mantissa corruption when 8-byte double is punned
// through the fixed-point address (24B at the Class-23 origin; 16B since Ship A).
//
// 3-barrier structural fix per DESIGN_SPECS/type-trait-dispatch-via-tt-namespace.md:
//   Barrier 1: API surface (no void*+offset entry; tt:: takes T& destination)
//   Barrier 2: X-macro extractor passes field by reference (T deduced)
//   Barrier 3: type-family static_assert in tt:: dispatch helpers (this trait
//              is one of the recognized family members; failure to extend the
//              family for a new type fails the build with actionable message)
//
// Pattern: parallel to std::is_floating_point_v / std::is_array_v / std::is_unsigned_v
// from <type_traits>. Codebase-specific traits live next to the type they describe.
// The legacy trait spelling is_FPN_v (pre-A.5) was first unified onto is_fp_binary_v at the 16B flip,
// then RETIRED at A.5 (E.0.8): every consumer + the B6 wire-dispatchers gate on is_fp_binary_v
// directly — ONE binary-domain trait, no divergence, no alias.

//======================================================================================================
// [UNIFIED RADIX-PARAMETRIZED FIXED-POINT — v5.15.5.F.4d.1.E Ship A]
//======================================================================================================
// FixedPoint<RADIX,FRAC>: value = stored_integer / RADIX^FRAC. Two concrete instantiations (D-99 —
// NOT speculative generality; two real types, not hypotheticals):
//   <2,64>  binary  — the 16B two's-complement core (Ship A; replaced the 24B sign-magnitude predecessor).
//   <10,8>  decimal — money, scale 10^8 (Ship B).
// Storage is radix-independent (a signed scaled integer); radix appears only in the mul-reduce + div.
// The 16B form keeps the sign in the TOP BIT (no sign field) → 16B, 16-aligned, NO padding (vs
// sign-magnitude's 24B w[2]+sign+_padding). Value-equivalent to the retired 24B core BY CONSTRUCTION
// (same unsigned product, same sign rule) — proven 423/0 at the Ship-A acceptance (op-slice since
// retired at close, workspace 84caea6; see the E.0.7 acceptance record; D-125/D-139).
template <int RADIX, int FRAC> struct FixedPoint;   // generic; exactly TWO specializations: <2,64> binary (Ship A) + <10,8> money (Ship B)

template <> struct FixedPoint<2, 64> {
    __int128 v;                              // value = v / 2^64
    static constexpr int RADIX = 2;
    static constexpr int FRAC  = 64;
};
static_assert(sizeof(FixedPoint<2,64>) == 16,
              "Ship A: the binary core is a bare 16B __int128 two's-complement (no sign field, no padding)");

// Ship B P1 (D-97/D-104/D-125): the DECIMAL MONEY instantiation — value = v / 10^8, signed
// two's-complement, EXACT for every Binance price/qty (<= 8dp). Same 16B bare-__int128 storage
// as binary (radix appears only in the mul-reduce/div, never in storage). Domain: the money
// CLOSURE invariant keeps |v| <= 2^63-1 (every Money_* op saturates results there + sets the
// S-17 sticky flag), which puts every product inside the PROVEN divmul domain by construction.
template <> struct FixedPoint<10, 8> {
    __int128 v;                              // value = v / 10^8 — exact decimal money (D-104)
    static constexpr int RADIX = 10;
    static constexpr int FRAC  = 8;
};
static_assert(sizeof(FixedPoint<10,8>) == 16,
              "Ship B: the decimal money core is a bare 16B __int128 (no sign field, no padding)");
static_assert(std::has_unique_object_representations_v<FixedPoint<10,8>>,
              "Ship B/H12-F-076: the money type must stay padding-free (memcmp/SHA-256/HMAC surfaces)");
using Money = FixedPoint<10, 8>;             // D-176: the money domain alias; the op family is Money_*

// Disjoint domain traits (B6 mechanical guard): binary <2,FRAC> vs decimal <10,FRAC>. Since A.5 the
// binary domain has exactly ONE trait spelling: is_fp_binary_v (legacy is_FPN_v retired). Every tt::
// wire dispatcher will static_assert exhaustively over these (binary || decimal || float || ...) →
// a missing decimal branch becomes a COMPILE ERROR, which is what turns the silent-lossy-emit risk
// into a build break.
template <typename T>  struct is_fp_binary                     : std::false_type {};
template <int F>       struct is_fp_binary<FixedPoint<2, F>>   : std::true_type  {};
template <typename T> inline constexpr bool is_fp_binary_v  = is_fp_binary<T>::value;
template <typename T>  struct is_fp_decimal                    : std::false_type {};
template <int F>       struct is_fp_decimal<FixedPoint<10, F>> : std::true_type  {};
template <typename T> inline constexpr bool is_fp_decimal_v = is_fp_decimal<T>::value;

// FPN_Binary<64> (the concrete 16B binary core, defined above) is is_fp_binary — EXTEND the trait so the
// B6 wire-dispatchers (which gate on is_fp_binary_v) treat it as binary. ONE binary-domain trait
// (D-143/B6; the legacy is_FPN_v alias that briefly lived here was retired at A.5 — D-163).
template <> struct is_fp_binary<FPN_Binary<64>> : std::true_type {};

// P2 EPOCH SWITCH (markers-first, D-174 #14 / S-4): the type the ENGINE'S MONEY FIELDS use.
// Flipping this alias to `Money` IS the value-encoding epoch transition — MONEY_ENCODING_EPOCH
// derives from it, every persisted-surface version floor auto-raises, and the per-surface
// trait-keyed static_asserts red-build until the SAME COMMIT carries: snapshot versions 14/10/7 +
// the OMSEL magic bump + header format_version (rotate-not-append, D-175a) + the stamp v3
// unconditional floor (strict-fork bypassed) + the money-golden regen + retrain checklist.
// The S-4 lesson made structural: the flip is 16B->16B, so NO sizeof/layout guard can see it —
// these ENCODING-keyed guards are the net. DO NOT flip outside the P2 migration commit.
// (Placed AFTER the traits — is_fp_decimal_v must be visible to the constexpr derivation.)
using EngineMoneyT = FPN_Binary<64>;                                  // P2 flip target: -> Money
inline constexpr unsigned MONEY_ENCODING_EPOCH = is_fp_decimal_v<EngineMoneyT> ? 1u : 0u;

//======================================================================================================
// [N-WORD HELPERS]
//======================================================================================================
// all loops are over compile-time N so the compiler fully unrolls them
//======================================================================================================

// is magnitude zero
template <unsigned F> inline int FPN_MagIsZero(FPN_Binary<F> v) {
    uint64_t acc = 0;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < FPN_Binary<F>::N; i++)
        acc |= v.w[i];
    return acc == 0;
}

// a >= b unsigned, branchless, built from LSB up
template <unsigned F> inline int FPN_MagGe(FPN_Binary<F> a, FPN_Binary<F> b) {
    int ge = (a.w[0] >= b.w[0]);
#pragma GCC unroll 65534
    for (unsigned i = 1; i < FPN_Binary<F>::N; i++) {
        int gt = (a.w[i] > b.w[i]);
        int eq = (a.w[i] == b.w[i]);
        ge     = gt | (eq & ge);
    }
    return ge;
}

// a == b
template <unsigned F> inline int FPN_MagEq(FPN_Binary<F> a, FPN_Binary<F> b) {
    uint64_t diff = 0;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < FPN_Binary<F>::N; i++)
        diff |= (a.w[i] ^ b.w[i]);
    return diff == 0;
}

// a > b
template <unsigned F> inline int FPN_MagGt(FPN_Binary<F> a, FPN_Binary<F> b) {
    return FPN_MagGe(a, b) & !FPN_MagEq(a, b);
}

// N-word add with carry chain
template <unsigned F> inline void FPN_MagAddN(const uint64_t *a, const uint64_t *b, uint64_t *r) {
    uint64_t carry = 0;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < FPN_Binary<F>::N; i++) {
        uint64_t t  = a[i] + b[i];
        uint64_t c1 = (t < a[i]);
        r[i]        = t + carry;
        uint64_t c2 = (r[i] < t);
        carry       = c1 | c2;
    }
}

// N-word sub with borrow chain
template <unsigned F> inline void FPN_MagSubN(const uint64_t *a, const uint64_t *b, uint64_t *r) {
    uint64_t borrow = 0;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < FPN_Binary<F>::N; i++) {
        uint64_t t   = a[i] - b[i];
        uint64_t bw1 = (a[i] < b[i]);
        r[i]         = t - borrow;
        uint64_t bw2 = (t < borrow);
        borrow       = bw1 | bw2;
    }
}

template <unsigned F> inline FPN_Binary<F> FPN_Zero() {
    FPN_Binary<F> result;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < FPN_Binary<F>::N; i++)
        result.w[i] = 0;
    result.sign = 0;
    return result;
}

//======================================================================================================
// [DOUBLE PRECISION CONVERSION]
//======================================================================================================
// precision is limited by double (~52 bits) but these are for getting data in and out
//======================================================================================================
template <unsigned F> inline FPN_Binary<F> FPN_FromDouble(double input) {
    constexpr unsigned N  = FPN_Binary<F>::N;
    constexpr unsigned FW = FPN_Binary<F>::FRAC_WORDS;

    int32_t neg      = (input < 0.0);
    double abs_input = input * (1.0 - 2.0 * neg);

    FPN_Binary<F> result;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < N; i++)
        result.w[i] = 0;

    double int_part  = floor(abs_input);
    double frac_part = abs_input - int_part;

    // integer part into the first integer word
    if (FW < N)
        result.w[FW] = (uint64_t)int_part;

    // fractional part: top 2 fractional words (most precision we can get from double)
    double frac_hi = floor(frac_part * 18446744073709551616.0);
    double frac_lo = (frac_part * 18446744073709551616.0 - frac_hi) * 18446744073709551616.0;
    if (FW >= 1)
        result.w[FW - 1] = (uint64_t)frac_hi;
    if (FW >= 2)
        result.w[FW - 2] = (uint64_t)frac_lo;

    result.sign = neg & !FPN_MagIsZero(result);
    return result;
}

template <unsigned F> inline double FPN_ToDouble(FPN_Binary<F> value) {
    constexpr unsigned N  = FPN_Binary<F>::N;
    constexpr unsigned FW = FPN_Binary<F>::FRAC_WORDS;

    // double only has ~52 bits of mantissa, so we only need the most significant
    // non-zero words. processing all words at large N causes scale to hit inf
    // and frac_scale to hit 0, producing NaN from 0*inf

    // integer part: find highest non-zero integer word, convert from there down
    // (at most 2 words contribute meaningful precision to a double)
    double int_part = 0.0;
    int top_int     = -1;
    for (int i = (int)N - 1; i >= (int)FW; i--) {
        if (value.w[i] != 0) {
            top_int = i;
            break;
        }
    }
    if (top_int >= 0) {
        double scale = 1.0;
        for (int i = (int)FW; i < top_int; i++)
            scale *= 18446744073709551616.0;
        // convert top 2 words (more than enough for double precision)
        int start = (top_int > (int)FW) ? (top_int - 1) : top_int;
        double s  = scale;
        for (int i = (int)FW; i < start; i++)
            s *= 18446744073709551616.0;
        // just do it simply: accumulate from FW up to top_int
        double sc = 1.0;
        for (int i = (int)FW; i <= top_int; i++) {
            int_part += (double)value.w[i] * sc;
            sc *= 18446744073709551616.0;
        }
    }

    // fractional part: only top 2 fractional words matter for double precision
    double frac = 0.0;
    if (FW >= 1) {
        frac += (double)value.w[FW - 1] / 18446744073709551616.0;
        if (FW >= 2)
            frac += (double)value.w[FW - 2] / (18446744073709551616.0 * 18446744073709551616.0);
    }

    double mag = int_part + frac;
    return mag * (1.0 - 2.0 * value.sign);
}

//======================================================================================================
// [INTEGER CONVERSION — pure integer, no double round-trip]
//======================================================================================================
// FPN_FromInt avoids the IEEE-754 reorderings that FPN_FromDouble<F>((double)int)
// can introduce across compilers / -O levels. Use this for any FPN_Binary value
// derived from an integer (loop indices, sample counts, precomputed sums like
// n*(n-1)/2). Bytewise-deterministic across builds. v5.10.0b prerequisite.
//======================================================================================================
template <unsigned F> inline FPN_Binary<F> FPN_FromInt(int64_t input) {
    constexpr unsigned N  = FPN_Binary<F>::N;
    constexpr unsigned FW = FPN_Binary<F>::FRAC_WORDS;

    FPN_Binary<F> result = FPN_Zero<F>();
    int neg = (input < 0);
    // careful negation: avoid -INT64_MIN UB by going through unsigned
    uint64_t mag = neg ? (uint64_t)(-(input + 1)) + 1u : (uint64_t)input;

    if (FW < N)
        result.w[FW] = mag;
    result.sign = (uint8_t)neg;
    return result;
}

//======================================================================================================
// [STRING CONVERSION - FULL PRECISION]
//======================================================================================================
// not limited by double's 52-bit mantissa - these preserve every bit
// uses single-word multiply/divmod by 10 across the word array
// decimal_places controls fractional digits (max meaningful ≈ FRAC_BITS * 0.301)
//======================================================================================================

// helper: multiply N-word array in-place by a single uint64_t, return overflow
template <unsigned N_WORDS> inline uint64_t FPN_MulSingle(uint64_t *a, uint64_t b) {
    uint64_t carry = 0;
    for (unsigned i = 0; i < N_WORDS; i++) {
        __uint128_t prod = (__uint128_t)a[i] * b + carry;
        a[i]             = (uint64_t)prod;
        carry            = (uint64_t)(prod >> 64);
    }
    return carry;
}

// helper: divmod N-word array in-place by a single uint64_t, return remainder
template <unsigned N_WORDS> inline uint64_t FPN_DivModSingle(uint64_t *a, uint64_t b) {
    uint64_t rem = 0;
    for (int i = (int)N_WORDS - 1; i >= 0; i--) {
        __uint128_t cur = ((__uint128_t)rem << 64) | a[i];
        a[i]            = (uint64_t)(cur / b);
        rem             = (uint64_t)(cur % b);
    }
    return rem;
}

// helper: is N-word array all zeros
template <unsigned N_WORDS> inline int FPN_ArrayIsZero(const uint64_t *a) {
    uint64_t acc = 0;
    for (unsigned i = 0; i < N_WORDS; i++)
        acc |= a[i];
    return acc == 0;
}

// max meaningful decimal digits for the fractional part
template <unsigned F> inline constexpr unsigned FPN_MaxDecimalDigits() {
    return (unsigned)((uint64_t)F * 301 / 1000) + 1; // ceil(FRAC_BITS * log10(2))
}

// convert to decimal string, returns number of chars written (excluding null terminator)
// buf must be large enough: sign + integer digits + '.' + decimal_places + '\0'
// if decimal_places is 0, uses max meaningful precision
template <unsigned F> inline unsigned FPN_ToString(FPN_Binary<F> value, char *buf, unsigned buf_size, unsigned decimal_places = 0) {
    constexpr unsigned N  = FPN_Binary<F>::N;
    constexpr unsigned FW = FPN_Binary<F>::FRAC_WORDS;
    constexpr unsigned IW = N - FW;

    if (decimal_places == 0)
        decimal_places = FPN_MaxDecimalDigits<F>();

    unsigned pos = 0;

    // sign
    if (value.sign && !FPN_MagIsZero(value)) {
        if (pos < buf_size - 1)
            buf[pos++] = '-';
    }

    // integer part: repeated divmod by 10, collect digits reversed
    uint64_t int_words[IW];
    for (unsigned i = 0; i < IW; i++)
        int_words[i] = value.w[FW + i];

    char int_digits[IW * 20 + 1];
    unsigned n_int_digits = 0;

    if (FPN_ArrayIsZero<IW>(int_words)) {
        int_digits[n_int_digits++] = '0';
    } else {
        while (!FPN_ArrayIsZero<IW>(int_words)) {
            uint64_t rem               = FPN_DivModSingle<IW>(int_words, 10);
            int_digits[n_int_digits++] = '0' + (char)rem;
        }
    }

    // write integer digits (reversed)
    for (int i = (int)n_int_digits - 1; i >= 0 && pos < buf_size - 1; i--)
        buf[pos++] = int_digits[i];

    // fractional part: repeated multiply by 10, overflow digit is the next decimal digit
    if (decimal_places > 0 && pos < buf_size - 1) {
        buf[pos++] = '.';

        uint64_t frac_words[FW];
        for (unsigned i = 0; i < FW; i++)
            frac_words[i] = value.w[i];

        for (unsigned d = 0; d < decimal_places && pos < buf_size - 1; d++) {
            uint64_t digit = FPN_MulSingle<FW>(frac_words, 10);
            buf[pos++]     = '0' + (char)digit;
        }
    }

    buf[pos] = '\0';
    return pos;
}

// parse decimal string to fixed-point
// accepts optional sign, integer digits, optional '.', fractional digits
template <unsigned F> inline FPN_Binary<F> FPN_FromString(const char *str) {
    constexpr unsigned N  = FPN_Binary<F>::N;
    constexpr unsigned FW = FPN_Binary<F>::FRAC_WORDS;
    constexpr unsigned IW = N - FW;

    FPN_Binary<F> result = FPN_Zero<F>();
    unsigned i        = 0;

    // sign
    int neg = 0;
    if (str[i] == '-') {
        neg = 1;
        i++;
    } else if (str[i] == '+') {
        i++;
    }

    // integer part: left to right, result = result * 10 + digit
    uint64_t int_words[IW];
    for (unsigned w = 0; w < IW; w++)
        int_words[w] = 0;

    while (str[i] >= '0' && str[i] <= '9') {
        FPN_MulSingle<IW>(int_words, 10);
        __uint128_t sum = (__uint128_t)int_words[0] + (uint64_t)(str[i] - '0');
        int_words[0]    = (uint64_t)sum;
        uint64_t carry  = (uint64_t)(sum >> 64);
        for (unsigned w = 1; w < IW && carry; w++) {
            sum          = (__uint128_t)int_words[w] + carry;
            int_words[w] = (uint64_t)sum;
            carry        = (uint64_t)(sum >> 64);
        }
        i++;
    }

    for (unsigned w = 0; w < IW; w++)
        result.w[FW + w] = int_words[w];

    // fractional part: collect digits, process right to left
    // for each digit (right to left): place digit in integer word, divmod (FW+1) words by 10
    // this converts 0.12345 by computing (((((0+5)/10)+4)/10)+3)/10...
    if (str[i] == '.') {
        i++;

        const char *frac_start = &str[i];
        unsigned n_frac        = 0;
        while (str[i] >= '0' && str[i] <= '9') {
            n_frac++;
            i++;
        }

        uint64_t frac_words[FW + 1];
        for (unsigned w = 0; w <= FW; w++)
            frac_words[w] = 0;

        for (int d = (int)n_frac - 1; d >= 0; d--) {
            frac_words[FW] = (uint64_t)(frac_start[d] - '0');
            FPN_DivModSingle<FW + 1>(frac_words, 10);
        }

        for (unsigned w = 0; w < FW; w++)
            result.w[w] = frac_words[w];
    }

    result.sign = neg & !FPN_MagIsZero(result);
    return result;
}

//======================================================================================================
// [FP64 CONVERSION]
//======================================================================================================
#ifdef FIXED_POINT_64_H
template <unsigned F> inline FPN_Binary<F> FPN_FromFP64(FP64 value) {
    constexpr unsigned FW = FPN_Binary<F>::FRAC_WORDS;
    // FP64 has 64 frac bits, FPN_Binary has F frac bits
    // FP64 magnitude is __uint128_t = two uint64_t words
    // place them at word (FW-1) and word (FW) to shift left by (F-64) bits
    FPN_Binary<F> result = FPN_Zero<F>();
    if (FW >= 1)
        result.w[FW - 1] = (uint64_t)value.magnitude;
    if (FW < FPN_Binary<F>::N)
        result.w[FW] = (uint64_t)(value.magnitude >> 64);
    result.sign = value.sign;
    return result;
}

template <unsigned F> inline FP64 FPN_ToFP64(FPN_Binary<F> value) {
    constexpr unsigned FW = FPN_Binary<F>::FRAC_WORDS;
    FP64 result;
    uint64_t lo      = (FW >= 1) ? value.w[FW - 1] : 0;
    uint64_t hi      = (FW < FPN_Binary<F>::N) ? value.w[FW] : 0;
    result.magnitude = ((__uint128_t)hi << 64) | (__uint128_t)lo;
    result.sign      = value.sign & (result.magnitude != 0);
    return result;
}
#endif

//======================================================================================================
// [GUARDS]
//======================================================================================================

// Branchless mask-blend selection. Picks if_true when mask = all-1s, if_false when mask = 0.
// Caller forms mask as `-(uint64_t)cond` where cond is 0 or 1 → no branches anywhere.
//
// Audit: plans/2026-05-06-latency-path-discipline.md Rule 8.
// Used by slow-path running-sum maintenance (v5.11.2.C) where the "warmup vs
// full-window" eviction term is data-dependent on count >= W.
//
// FPN_Binary<64> hits this template directly (N=2 limbs); no FP64 specialization needed
// since two AND-OR pairs compile to ~6 instructions, comparable to cmov on the
// __uint128_t magnitude.
template <unsigned F>
inline FPN_Binary<F> FPN_BlendOnMask(FPN_Binary<F> if_true, FPN_Binary<F> if_false, uint64_t mask) {
    FPN_Binary<F> r;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < FPN_Binary<F>::N; ++i) {
        r.w[i] = (if_true.w[i] & mask) | (if_false.w[i] & ~mask);
    }
    // Sign is 0 or 1; sign-extend mask to int32 so the AND preserves that range.
    int32_t m32 = (int32_t)mask;
    r.sign = (if_true.sign & m32) | (if_false.sign & ~m32);
    return r;
}

template <unsigned F> inline FPN_Binary<F> FPN_Min(FPN_Binary<F> a, FPN_Binary<F> b) {
    int diff_sign = a.sign ^ b.sign;
    int a_lt_mag  = FPN_MagGt(b, a);
    int a_lt_same = (a_lt_mag ^ a.sign) & !FPN_MagEq(a, b);
    int both_zero = FPN_MagIsZero(a) & FPN_MagIsZero(b);
    int a_lt      = ((a.sign & diff_sign) | (a_lt_same & (!diff_sign))) & !both_zero;

    uint64_t mask = -(uint64_t)a_lt;
    FPN_Binary<F> result;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < FPN_Binary<F>::N; i++)
        result.w[i] = (a.w[i] & mask) | (b.w[i] & ~mask);
    result.sign = (int32_t)((a.sign & a_lt) | (b.sign & (1 - a_lt)));
    return result;
}

template <unsigned F> inline FPN_Binary<F> FPN_Max(FPN_Binary<F> a, FPN_Binary<F> b) {
    int diff_sign = a.sign ^ b.sign;
    int a_gt_mag  = FPN_MagGt(a, b);
    int a_gt_same = (a_gt_mag ^ a.sign) & !FPN_MagEq(a, b);
    int a_gt      = (((!a.sign) & diff_sign) | (a_gt_same & (!diff_sign)));

    uint64_t mask = -(uint64_t)a_gt;
    FPN_Binary<F> result;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < FPN_Binary<F>::N; i++)
        result.w[i] = (a.w[i] & mask) | (b.w[i] & ~mask);
    result.sign = (int32_t)((a.sign & a_gt) | (b.sign & (1 - a_gt)));
    return result;
}

//======================================================================================================
// [FIXED-POINT ARITHMETIC OPERATIONS]
//======================================================================================================
// branchless sign-magnitude add: compute both paths, mask-select with 64-bit ops
//======================================================================================================
template <unsigned F> inline FPN_Binary<F> FPN_AddSat(FPN_Binary<F> a, FPN_Binary<F> b) {
    constexpr unsigned N = FPN_Binary<F>::N;
    int diff             = a.sign ^ b.sign;

    // same sign: add magnitudes
    uint64_t sum[N];
    FPN_MagAddN<F>(a.w, b.w, sum);

    // different sign: compute both a-b and b-a, mask-select
    uint64_t dab[N], dba[N];
    FPN_MagSubN<F>(a.w, b.w, dab);
    FPN_MagSubN<F>(b.w, a.w, dba);

    int ge           = FPN_MagGe(a, b);
    uint64_t ge_mask = -(uint64_t)ge;
    uint64_t d_mask  = -(uint64_t)diff;

    FPN_Binary<F> result;
    uint64_t or_all = 0;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < N; i++) {
        uint64_t d  = (dab[i] & ge_mask) | (dba[i] & ~ge_mask);
        result.w[i] = (sum[i] & ~d_mask) | (d & d_mask);
        or_all |= result.w[i];
    }

    int diff_sign_result = (a.sign & ge) | (b.sign & (1 - ge));
    int result_sign      = (a.sign & (!diff)) | (diff_sign_result & diff);
    result.sign          = result_sign & (or_all != 0);

    return result;
}

template <unsigned F> inline FPN_Binary<F> FPN_SubSat(FPN_Binary<F> a, FPN_Binary<F> b) {
    FPN_Binary<F> neg_b = b;
    neg_b.sign       = b.sign ^ (!FPN_MagIsZero(b));
    return FPN_AddSat(a, neg_b);
}

// also provide non-sat versions (identical for now since 2*FRAC_BITS of headroom is massive)
template <unsigned F> inline FPN_Binary<F> FPN_Add(FPN_Binary<F> a, FPN_Binary<F> b) {
    return FPN_AddSat(a, b);
}
template <unsigned F> inline FPN_Binary<F> FPN_Sub(FPN_Binary<F> a, FPN_Binary<F> b) {
    return FPN_SubSat(a, b);
}

//======================================================================================================
// [MULTIPLY - SCHOOLBOOK PARTIAL PRODUCTS]
//======================================================================================================
// splits each N-word magnitude into N single uint64_t words, does N^2 partial multiplies
// each partial is uint64_t * uint64_t -> __uint128_t (one hardware mul instruction)
// the compiler fully unrolls both loops since N is compile-time, so this is straight-line
// multiply-accumulate with no branches
//
// full product is 2N words, we shift right by FRAC_WORDS to get back to Q(F.F)
//======================================================================================================
template <unsigned F> inline FPN_Binary<F> FPN_Mul(FPN_Binary<F> a, FPN_Binary<F> b) {
    constexpr unsigned N  = FPN_Binary<F>::N;
    constexpr unsigned FW = FPN_Binary<F>::FRAC_WORDS;

    // 2N-word product
    uint64_t p[2 * N] = {0};

    // schoolbook: N^2 partial products, each one mul + add + carry propagation
    // pragma unroll forces GCC to flatten these into straight-line mul/add/carry
#pragma GCC unroll 65534
    for (unsigned i = 0; i < N; i++) {
        uint64_t carry = 0;
#pragma GCC unroll 65534
        for (unsigned j = 0; j < N; j++) {
            __uint128_t prod = (__uint128_t)a.w[i] * b.w[j] + (__uint128_t)p[i + j] + carry;
            p[i + j]         = (uint64_t)prod;
            carry            = (uint64_t)(prod >> 64);
        }
        // propagate remaining carry (fixed-trip, compiler unrolls)
#pragma GCC unroll 65534
        for (unsigned k = i + N; k < 2 * N; k++) {
            __uint128_t s = (__uint128_t)p[k] + carry;
            p[k]          = (uint64_t)s;
            carry         = (uint64_t)(s >> 64);
        }
    }

    // extract result: shift right by FW words
    // overflow: any word above position FW + N - 1 means we overflowed
    uint64_t overflow = 0;
#pragma GCC unroll 65534
    for (unsigned i = FW + N; i < 2 * N; i++)
        overflow |= p[i];
    uint64_t of_mask = -(uint64_t)(overflow != 0);

    FPN_Binary<F> result;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < N; i++) {
        result.w[i] = (p[FW + i] & ~of_mask) | (UINT64_MAX & of_mask);
    }

    result.sign = (a.sign ^ b.sign) & !FPN_MagIsZero(result);
    return result;
}

//======================================================================================================
// [DIVISION - BRANCHLESS LONG DIVISION]
//======================================================================================================
// computes (a_magnitude << FRAC_BITS) / b_magnitude using bit-by-bit long division
// the dividend is 2N words (a shifted left by FRAC_BITS), divisor is N words
// each iteration produces 1 bit of quotient: trial-subtract the divisor from the remainder,
// use branchless mask-select to keep or discard, shift quotient bit in
//
// total iterations: N * 64 (one per bit of quotient)
// each iteration: N-word compare + N-word conditional subtract + 2N-word shift = O(N) work
// all branchless - compare produces a mask, subtract is always computed, mask selects result
//======================================================================================================

// helper: shift a 2N-word value left by 1 bit
template <unsigned F> inline void FPN_ShiftLeft1_2N(uint64_t *v) {
    constexpr unsigned W = 2 * FPN_Binary<F>::N;
#pragma GCC unroll 65534
    for (int i = (int)W - 1; i > 0; i--) {
        v[i] = (v[i] << 1) | (v[i - 1] >> 63);
    }
    v[0] <<= 1;
}

// helper: is N-word a >= N-word b (branchless, from LSB up)
template <unsigned F> inline int FPN_NWordGe(const uint64_t *a, const uint64_t *b) {
    constexpr unsigned N = FPN_Binary<F>::N;
    int ge               = (a[0] >= b[0]);
#pragma GCC unroll 65534
    for (unsigned i = 1; i < N; i++) {
        int gt = (a[i] > b[i]);
        int eq = (a[i] == b[i]);
        ge     = gt | (eq & ge);
    }
    return ge;
}

// helper: N-word conditional subtract: r = ge ? (a - b) : a
// branchless: always computes both, mask-selects
template <unsigned F> inline void FPN_CondSub(uint64_t *a, const uint64_t *b, int ge) {
    constexpr unsigned N = FPN_Binary<F>::N;
    uint64_t mask        = -(uint64_t)ge;

    uint64_t diff[N];
    uint64_t borrow = 0;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < N; i++) {
        uint64_t t   = a[i] - b[i];
        uint64_t bw1 = (a[i] < b[i]);
        diff[i]      = t - borrow;
        uint64_t bw2 = (t < borrow);
        borrow       = bw1 | bw2;
    }

#pragma GCC unroll 65534
    for (unsigned i = 0; i < N; i++) {
        a[i] = (diff[i] & mask) | (a[i] & ~mask);
    }
}

template <unsigned F> inline FPN_Binary<F> FPN_DivNoAssert(FPN_Binary<F> a, FPN_Binary<F> b) {
    constexpr unsigned N  = FPN_Binary<F>::N;
    constexpr unsigned FW = FPN_Binary<F>::FRAC_WORDS;

    // branchless zero-divisor saturation: if b is zero, safe_b = 1, result gets masked to MAX
    int b_zero       = FPN_MagIsZero(b);
    uint64_t bz_mask = -(uint64_t)b_zero;

    // make safe divisor: if b is zero, set LSB to 1 so division executes without UB
    uint64_t divisor[N];
#pragma GCC unroll 65534
    for (unsigned i = 0; i < N; i++)
        divisor[i] = b.w[i];
    divisor[0] |= (uint64_t)b_zero; // 0 becomes 1, nonzero stays the same

    // dividend = a_magnitude << FRAC_BITS
    // in a 2N-word array: lower FW words are 0, upper N words are a.w shifted up by FW
    uint64_t remainder[2 * N] = {0};
#pragma GCC unroll 65534
    for (unsigned i = 0; i < N; i++)
        remainder[FW + i] = a.w[i];

    // quotient accumulates bit by bit
    uint64_t quotient[N] = {0};

    // long division: produce N*64 bits of quotient, one bit per iteration
    // each iteration: shift remainder left 1, compare top N words against divisor,
    // conditionally subtract, shift quotient bit in
    constexpr unsigned TOTAL_QBITS = N * 64;
    // NOTE: do NOT unroll this outer loop - at 512+ bits it's 1024+ iterations and GCC chokes
    // the loop counter branch is fixed-trip (branch predictor hits 100%), not data-dependent
    // all the actual math inside (compare, cond-subtract, quotient bit) stays branchless
    for (unsigned bit = 0; bit < TOTAL_QBITS; bit++) {
        // shift remainder left by 1
        FPN_ShiftLeft1_2N<F>(remainder);

        // compare top N words of remainder against divisor
        int ge = FPN_NWordGe<F>(&remainder[N], divisor);

        // conditionally subtract divisor from top N words
        FPN_CondSub<F>(&remainder[N], divisor, ge);

        // shift quotient left by 1 and OR in the new bit
        unsigned word_idx = N - 1 - (bit / 64);
        unsigned bit_idx  = 63 - (bit % 64);
        quotient[word_idx] |= ((uint64_t)ge << bit_idx);
    }

    // build result: quotient is the magnitude, saturate if b was zero
    FPN_Binary<F> result;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < N; i++) {
        result.w[i] = (quotient[i] & ~bz_mask) | (UINT64_MAX & bz_mask);
    }

    // sign: XOR inputs, but if b was zero use a's sign
    int normal_sign = (a.sign ^ b.sign) & !FPN_MagIsZero(result);
    int zero_sign   = a.sign;
    result.sign     = (normal_sign & (!b_zero)) | (zero_sign & b_zero);

    return result;
}

template <unsigned F> inline FPN_Binary<F> FPN_DivWithAssert(FPN_Binary<F> a, FPN_Binary<F> b) {
    assert(!FPN_MagIsZero(b));
    return FPN_DivNoAssert(a, b);
}

//======================================================================================================
// [FIXED-POINT COMPARISON OPERATIONS]
//======================================================================================================
template <unsigned F> inline int FPN_Equal(FPN_Binary<F> a, FPN_Binary<F> b) {
    int both_zero = FPN_MagIsZero(a) & FPN_MagIsZero(b);
    return both_zero | (FPN_MagEq(a, b) & (a.sign == b.sign));
}

template <unsigned F> inline int FPN_NotEqual(FPN_Binary<F> a, FPN_Binary<F> b) {
    return !FPN_Equal(a, b);
}

template <unsigned F> inline int FPN_LessThan(FPN_Binary<F> a, FPN_Binary<F> b) {
    int both_zero   = FPN_MagIsZero(a) & FPN_MagIsZero(b);
    int diff_sign   = a.sign ^ b.sign;
    int diff_result = a.sign & !both_zero;
    int a_lt_mag    = FPN_MagGt(b, a);
    int same_result = (a_lt_mag ^ a.sign) & !FPN_MagEq(a, b);
    return (diff_result & diff_sign) | (same_result & (!diff_sign));
}

template <unsigned F> inline int FPN_LessThanOrEqual(FPN_Binary<F> a, FPN_Binary<F> b) {
    return FPN_LessThan(a, b) | FPN_Equal(a, b);
}

template <unsigned F> inline int FPN_GreaterThan(FPN_Binary<F> a, FPN_Binary<F> b) {
    return FPN_LessThan(b, a);
}

template <unsigned F> inline int FPN_GreaterThanOrEqual(FPN_Binary<F> a, FPN_Binary<F> b) {
    return !FPN_LessThan(a, b);
}

//======================================================================================================
// [FIXED-POINT COMPARISON OPERATORS — v5.15.5.F.4d.1.B.2 Step 6.5]
//======================================================================================================
// Operator overloads wrapping the existing FPN_Equal / FPN_LessThan / etc. free
// functions. Removes the FPN_ToDouble(a) < FPN_ToDouble(b) workaround pattern
// throughout the codebase — callers can use `a < b` directly.
//
// Coding-time discovery during .B.2 Step 6 Winsor parse-time validation:
// Caramel asked "should we address this?" when the FPN_ToDouble fallback came up.
// Per `feedback_motivated_collaborator_for_caramel` + `feedback_overengineering_boundary_when_future_easier`
// — adding primitive operators NOW removes a recurring workaround forever.
//
// Per H4 — these operators compare in the integer-limb domain (no float math on
// accounting types); same backend semantics as the FPN_* free functions they wrap.

template <unsigned F> inline bool operator==(FPN_Binary<F> a, FPN_Binary<F> b) { return FPN_Equal(a, b) != 0; }
template <unsigned F> inline bool operator!=(FPN_Binary<F> a, FPN_Binary<F> b) { return !FPN_Equal(a, b); }
template <unsigned F> inline bool operator< (FPN_Binary<F> a, FPN_Binary<F> b) { return FPN_LessThan(a, b) != 0; }
template <unsigned F> inline bool operator<=(FPN_Binary<F> a, FPN_Binary<F> b) { return FPN_LessThanOrEqual(a, b) != 0; }
template <unsigned F> inline bool operator> (FPN_Binary<F> a, FPN_Binary<F> b) { return FPN_GreaterThan(a, b) != 0; }
template <unsigned F> inline bool operator>=(FPN_Binary<F> a, FPN_Binary<F> b) { return FPN_GreaterThanOrEqual(a, b) != 0; }

//======================================================================================================
// [FIXED-POINT UTILITY FUNCTIONS]
//======================================================================================================
template <unsigned F> inline FPN_Binary<F> FPN_Negate(FPN_Binary<F> value) {
    FPN_Binary<F> result = value;
    result.sign       = value.sign ^ (!FPN_MagIsZero(value));
    return result;
}

template <unsigned F> inline int FPN_IsZero(FPN_Binary<F> value) {
    return FPN_MagIsZero(value);
}

template <unsigned F> inline FPN_Binary<F> FPN_Abs(FPN_Binary<F> value) {
    FPN_Binary<F> result = value;
    result.sign       = 0;
    return result;
}

// v5.9.0 — FPN_Binary garbage / saturation sanity check.
// FPN_Binary itself can't be NaN/Inf (integer type), but FPN_DivNoAssert(x, 0)
// saturates to FPN_MAX silently (per CLAUDE_INVARIANTS.md), and
// FPN_FromDouble<F>(NaN/Inf) is undefined behavior that can produce
// integer values in any range. Float-side std::isnan/isinf catches
// the cases where FPN_Binary→float conversion produces NaN/Inf, but a
// "wrong but float-finite" FPN_Binary value would slip through.
//
// FPN_IsValidFinite<F>(val) catches that gap with a branchless
// magnitude check. Threshold 1e15 is a global "no legitimate feature
// is this large" sanity bound — well above any realistic price/volume
// /flow value, well below FPN_MAX. Operator-tunable per-feature in
// the future if needed (see DOCS/CLAUDE_ML_INVARIANTS.md).
//
// Returns 1 if |val| < 1e15 (sane), 0 if saturation/garbage suspected.
// Branchless after FPN_Abs + FPN_LessThan compile down to integer cmp.
template <unsigned F> inline int FPN_IsValidFinite(FPN_Binary<F> value) {
    // Threshold computed once per type-instantiation (constexpr-eligible
    // when FPN_FromDouble specializes; otherwise hoisted by the compiler).
    FPN_Binary<F> threshold = FPN_FromDouble<F>(1e15);
    return FPN_LessThan(FPN_Abs(value), threshold);
}

template <unsigned F> inline FPN_Binary<F> FPN_Sign(FPN_Binary<F> value) {
    // branchless: compute +/-1.0, then mask to zero if input is zero
    int is_nonzero                   = !FPN_MagIsZero(value);
    uint64_t nz_mask                 = -(uint64_t)is_nonzero;
    FPN_Binary<F> result                = FPN_Zero<F>();
    result.w[FPN_Binary<F>::FRAC_WORDS] = 1 & nz_mask;
    result.sign                      = value.sign & is_nonzero;
    return result;
}

//======================================================================================================
// [FIXED-POINT MATH FUNCTIONS]
//======================================================================================================
// all go through double conversion - precision limited but these are convenience functions
//======================================================================================================
// v5.10.0b.2.5.A: FPN_Binary-native Newton-Raphson square root.
// Bytewise-deterministic across compilers — bit-scan seed + integer
// FPN_Binary ops only; no IEEE-754 round-trip. Converges quadratically;
// 12 NR iterations from the bit-scan seed reach FPN_Binary<64> precision
// for any positive input. Returns 0 for zero or negative input
// (matches stub-era assert behavior in release builds).
template <unsigned F> inline FPN_Binary<F> FPN_Sqrt(FPN_Binary<F> value) {
    if (FPN_MagIsZero(value) || value.sign != 0) return FPN_Zero<F>();

    // Find highest set bit position in the integer magnitude.
    // FPN_Binary<F> has F fractional bits; bit position k corresponds to value 2^(k - F).
    // sqrt(2^(k - F)) = 2^((k - F) / 2), which in FPN_Binary bit position is (k + F) / 2.
    int top_bit = -1;
    #pragma GCC unroll 65534
    for (int i = (int)FPN_Binary<F>::N - 1; i >= 0; i--) {
        if (value.w[i] != 0 && top_bit < 0) {
            int hi  = 63 - __builtin_clzll(value.w[i]);
            top_bit = i * 64 + hi;
        }
    }
    int seed_bit = (top_bit + (int)F) / 2;
    FPN_Binary<F> y = FPN_Zero<F>();
    int word_idx = seed_bit / 64;
    int bit_idx  = seed_bit % 64;
    if (word_idx < (int)FPN_Binary<F>::N) y.w[word_idx] = (uint64_t)1 << bit_idx;

    // 12 Newton-Raphson iterations: y_{n+1} = (y_n + x/y_n) / 2.
    // Quadratic convergence; 12 is well past the precision cliff for FPN_Binary<64>.
    FPN_Binary<F> half = FPN_FromDouble<F>(0.5);  // 0.5 is bytewise-exact in IEEE-754
    #pragma GCC unroll 65534
    for (int i = 0; i < 12; i++) {
        FPN_Binary<F> q = FPN_DivNoAssert(value, y);
        y = FPN_Mul(FPN_Add(y, q), half);
    }
    return y;
}

template <unsigned F> inline FPN_Binary<F> FPN_InvSqrt(FPN_Binary<F> value) {
    assert(value.sign == 0 && !FPN_MagIsZero(value));
    return FPN_FromDouble<F>(1.0 / sqrt(FPN_ToDouble(value)));
}

// v5.10.0b.2.5.D: FPN_Binary-native Taylor sin via range reduction to [0, π/2].
// 1. Reduce x to [-π, π] by x = x - n*2π where n = round(x / 2π).
// 2. Use sin oddness to make x ≥ 0; track sign flip for later.
// 3. If x > π/2, use sin(π - x) = sin(x) to bring into [0, π/2].
// 4. Taylor: sin(x) = x - x³/6 + x⁵/120 - ... (8 odd-power terms past x).
// At |x| ≤ π/2 ≈ 1.57, the 17! term hits ~3e-13 — past FPN_Binary<64> noise.
// Bytewise-deterministic across compilers; no IEEE-754 round-trip.
template <unsigned F> inline FPN_Binary<F> FPN_Sin(FPN_Binary<F> value) {
    constexpr unsigned N  = FPN_Binary<F>::N;
    constexpr unsigned FW = FPN_Binary<F>::FRAC_WORDS;

    if (FPN_IsZero(value)) return FPN_Zero<F>();

    // Constants (bytewise-stable IEEE-754 literals)
    FPN_Binary<F> pi         = FPN_FromDouble<F>(3.141592653589793);
    FPN_Binary<F> two_pi     = FPN_FromDouble<F>(6.283185307179586);
    FPN_Binary<F> half_pi    = FPN_FromDouble<F>(1.5707963267948966);
    FPN_Binary<F> inv_two_pi = FPN_FromDouble<F>(0.15915494309189535);
    FPN_Binary<F> half_const = FPN_FromDouble<F>(0.5);

    // Step 1: reduce to [-π, π] via x = value - n*2π, n = round(value/2π)
    FPN_Binary<F> q = FPN_Mul(value, inv_two_pi);
    FPN_Binary<F> q_rounded = q.sign ? FPN_Sub(q, half_const) : FPN_Add(q, half_const);
    int64_t n_int = (FW < N) ? (int64_t)q_rounded.w[FW] : 0;
    if (q_rounded.sign) n_int = -n_int;
    FPN_Binary<F> n_abs    = FPN_FromInt<F>(n_int < 0 ? -n_int : n_int);
    FPN_Binary<F> n_two_pi = FPN_Mul(n_abs, two_pi);
    if (n_int < 0) n_two_pi.sign = 1;
    FPN_Binary<F> x = FPN_Sub(value, n_two_pi);

    // Step 2: sin is odd — make x ≥ 0, remember to flip the result
    int sign_flip = (int)x.sign;
    x.sign = 0;

    // Step 3: if x > π/2, use sin(π - x) = sin(x). Now x ∈ [0, π/2].
    if (FPN_GreaterThanOrEqual(x, half_pi)) x = FPN_Sub(pi, x);

    // Step 4: Taylor — sin(x) = x - x³/6 + x⁵/120 - x⁷/5040 + ...
    // 8 odd-power terms past the x term: stops at x^17/17!.
    FPN_Binary<F> result = x;             // x term (k=0)
    FPN_Binary<F> x_pow  = x;
    FPN_Binary<F> x_sq   = FPN_Mul(x, x);
    static const double inv_fact_odd[8] = {
        -1.0 / 6.0,                   // 3!
         1.0 / 120.0,                 // 5!
        -1.0 / 5040.0,                // 7!
         1.0 / 362880.0,              // 9!
        -1.0 / 39916800.0,            // 11!
         1.0 / 6227020800.0,          // 13!
        -1.0 / 1307674368000.0,       // 15!
         1.0 / 355687428096000.0      // 17!
    };
    #pragma GCC unroll 65534
    for (int k = 0; k < 8; k++) {
        x_pow = FPN_Mul(x_pow, x_sq);
        FPN_Binary<F> term = FPN_Mul(x_pow, FPN_FromDouble<F>(inv_fact_odd[k]));
        result = FPN_Add(result, term);
    }

    if (sign_flip) result.sign = 1 - result.sign;
    return result;
}

// FPN_Cos via identity cos(x) = sin(x + π/2). Trivially deterministic.
template <unsigned F> inline FPN_Binary<F> FPN_Cos(FPN_Binary<F> value) {
    FPN_Binary<F> half_pi = FPN_FromDouble<F>(1.5707963267948966);
    return FPN_Sin(FPN_Add(value, half_pi));
}

template <unsigned F> inline FPN_Binary<F> FPN_Tan(FPN_Binary<F> value) {
    return FPN_FromDouble<F>(tan(FPN_ToDouble(value)));
}

template <unsigned F> inline FPN_Binary<F> FPN_Atan2(FPN_Binary<F> y, FPN_Binary<F> x) {
    return FPN_FromDouble<F>(atan2(FPN_ToDouble(y), FPN_ToDouble(x)));
}

// v5.10.0b.2.5.B: FPN_Binary-native exponential via range reduction + Taylor.
// x = k*ln(2) + r where k = trunc(x / ln(2)), |r| < ln(2).
// exp(x) = 2^k * exp(r); 2^k is a bit-shift, exp(r) Taylor-expands fast.
// Bytewise-deterministic across builds. Designed for EWMA decay range
// [-30, 0]: typical exp(-30) ≈ 9.36e-14 (representable in FPN_Binary<64>'s
// 64 fractional bits as ~2^-43.6).
template <unsigned F> inline FPN_Binary<F> FPN_Exp(FPN_Binary<F> value) {
    constexpr unsigned N  = FPN_Binary<F>::N;
    constexpr unsigned FW = FPN_Binary<F>::FRAC_WORDS;

    // exp(0) = 1
    if (FPN_IsZero(value)) {
        FPN_Binary<F> one = FPN_Zero<F>();
        if (FW < N) one.w[FW] = 1ULL;
        return one;
    }

    // ln(2) ≈ 0.6931471805599453, 1/ln(2) ≈ 1.4426950408889634.
    // Constants via FPN_FromDouble — bytewise-stable for small IEEE-754
    // literals (the round-trip happens once per call at known constants;
    // determinism preserved as long as the literal bytes match across
    // compilers, which they do per IEEE-754).
    FPN_Binary<F> ln2     = FPN_FromDouble<F>(0.6931471805599453);
    FPN_Binary<F> inv_ln2 = FPN_FromDouble<F>(1.4426950408889634);

    // k = round(value / ln(2)) (round-to-nearest, half-away-from-zero).
    // Round-to-nearest bounds |r| ≤ ln(2)/2 ≈ 0.347; Taylor with 9 terms
    // then gives |error| ≈ 0.347^9 / 9! ≈ 1.5e-10 relative — past 1e-9.
    // Truncation (the prior approach) left |r| up to ln(2) ≈ 0.69, which
    // failed the 1e-9 bound for inputs with q ≈ ±0.5 ± k.
    FPN_Binary<F> q = FPN_Mul(value, inv_ln2);
    FPN_Binary<F> half_const = FPN_FromDouble<F>(0.5);
    FPN_Binary<F> q_rounded  = q.sign ? FPN_Sub(q, half_const) : FPN_Add(q, half_const);
    int64_t k = (FW < N) ? (int64_t)q_rounded.w[FW] : 0;
    if (q_rounded.sign) k = -k;

    // r = value - k * ln(2)
    FPN_Binary<F> k_abs = FPN_FromInt<F>(k < 0 ? -k : k);
    FPN_Binary<F> k_ln2 = FPN_Mul(k_abs, ln2);
    if (k < 0) k_ln2.sign = 1;
    FPN_Binary<F> r = FPN_Sub(value, k_ln2);

    // Taylor: exp(r) = sum_{n=0}^{8} r^n / n!
    // 9 terms is past the precision cliff for |r| < ln(2)/2 ≈ 0.347.
    // r^8 / 8! ≈ 0.347^8 / 40320 ≈ 5e-9; r^9 way past FPN_Binary<64> noise floor.
    FPN_Binary<F> result = FPN_Zero<F>();
    if (FW < N) result.w[FW] = 1ULL;       // term n=0: 1
    FPN_Binary<F> r_pow = result;                 // r^0
    static const double inv_fact[9] = {
        1.0, 1.0, 0.5, 1.0/6.0, 1.0/24.0,
        1.0/120.0, 1.0/720.0, 1.0/5040.0, 1.0/40320.0
    };
    #pragma GCC unroll 65534
    for (int n = 1; n < 9; n++) {
        r_pow = FPN_Mul(r_pow, r);
        FPN_Binary<F> term = FPN_Mul(r_pow, FPN_FromDouble<F>(inv_fact[n]));
        result = FPN_Add(result, term);
    }

    // Multiply by 2^k via bit position: 1.0 in FPN_Binary is at bit F, so
    // 2^k value = bit position F + k.
    int seed_bit = (int)F + (int)k;
    if (seed_bit < 0) {
        // 2^k underflows below FPN_Binary precision → result rounds to 0
        return FPN_Zero<F>();
    }
    if (seed_bit >= (int)(N * 64)) {
        // overflow: saturate to current Taylor result (no shift) — caller
        // should not feed in inputs that overflow exp range
        return result;
    }
    FPN_Binary<F> two_k = FPN_Zero<F>();
    two_k.w[seed_bit / 64] = (uint64_t)1 << (seed_bit % 64);
    return FPN_Mul(result, two_k);
}

template <unsigned F> inline FPN_Binary<F> FPN_Log(FPN_Binary<F> value) {
    assert(value.sign == 0 && !FPN_MagIsZero(value));
    return FPN_FromDouble<F>(log(FPN_ToDouble(value)));
}

template <unsigned F> inline FPN_Binary<F> FPN_Pow(FPN_Binary<F> base, FPN_Binary<F> exponent) {
    return FPN_FromDouble<F>(pow(FPN_ToDouble(base), FPN_ToDouble(exponent)));
}

//======================================================================================================
// [FIXED-POINT MISCELLANEOUS FUNCTIONS]
//======================================================================================================
template <unsigned F> inline FPN_Binary<F> FPN_Floor(FPN_Binary<F> value) {
    constexpr unsigned FW = FPN_Binary<F>::FRAC_WORDS;
    // check if any fractional word is nonzero
    uint64_t frac_or = 0;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < FW; i++)
        frac_or |= value.w[i];
    int has_frac = (frac_or != 0);

    FPN_Binary<F> result;
// zero out fractional words
#pragma GCC unroll 65534
    for (unsigned i = 0; i < FW; i++)
        result.w[i] = 0;
// copy integer words
#pragma GCC unroll 65534
    for (unsigned i = FW; i < FPN_Binary<F>::N; i++)
        result.w[i] = value.w[i];
    // if negative and had fraction, bump integer part by 1 with carry chain
    uint64_t bump = (uint64_t)(value.sign & has_frac);
#pragma GCC unroll 65534
    for (unsigned i = FW; i < FPN_Binary<F>::N; i++) {
        uint64_t old = result.w[i];
        result.w[i] += bump;
        bump = (result.w[i] < old);
    }
    result.sign = value.sign;
    return result;
}

template <unsigned F> inline FPN_Binary<F> FPN_Ceil(FPN_Binary<F> value) {
    constexpr unsigned FW = FPN_Binary<F>::FRAC_WORDS;
    uint64_t frac_or      = 0;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < FW; i++)
        frac_or |= value.w[i];
    int has_frac = (frac_or != 0);

    FPN_Binary<F> result;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < FW; i++)
        result.w[i] = 0;
#pragma GCC unroll 65534
    for (unsigned i = FW; i < FPN_Binary<F>::N; i++)
        result.w[i] = value.w[i];
    // if positive and had fraction, bump integer part by 1
    uint64_t bump = (uint64_t)((!value.sign) & has_frac);
#pragma GCC unroll 65534
    for (unsigned i = FW; i < FPN_Binary<F>::N; i++) {
        uint64_t old = result.w[i];
        result.w[i] += bump;
        bump = (result.w[i] < old);
    }
    uint64_t int_or = 0;
#pragma GCC unroll 65534
    for (unsigned i = FW; i < FPN_Binary<F>::N; i++)
        int_or |= result.w[i];
    result.sign = value.sign & (int_or != 0);
    return result;
}

template <unsigned F> inline FPN_Binary<F> FPN_Round(FPN_Binary<F> value) {
    constexpr unsigned FW = FPN_Binary<F>::FRAC_WORDS;
    // half = MSB of the top fractional word
    int round_up = (FW >= 1) ? ((value.w[FW - 1] >> 63) & 1) : 0;

    FPN_Binary<F> result;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < FW; i++)
        result.w[i] = 0;
#pragma GCC unroll 65534
    for (unsigned i = FW; i < FPN_Binary<F>::N; i++)
        result.w[i] = value.w[i];
    uint64_t bump = (uint64_t)round_up;
#pragma GCC unroll 65534
    for (unsigned i = FW; i < FPN_Binary<F>::N; i++) {
        uint64_t old = result.w[i];
        result.w[i] += bump;
        bump = (result.w[i] < old);
    }
    uint64_t int_or = 0;
#pragma GCC unroll 65534
    for (unsigned i = FW; i < FPN_Binary<F>::N; i++)
        int_or |= result.w[i];
    result.sign = value.sign & (int_or != 0);
    return result;
}

template <unsigned F> inline FPN_Binary<F> FPN_Mod(FPN_Binary<F> a, FPN_Binary<F> b) {
    assert(!FPN_MagIsZero(b));
    FPN_Binary<F> quotient = FPN_DivNoAssert(a, b);
    // truncate: zero out fractional words
    FPN_Binary<F> truncated = quotient;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < FPN_Binary<F>::FRAC_WORDS; i++)
        truncated.w[i] = 0;
    return FPN_SubSat(a, FPN_Mul(truncated, b));
}

template <unsigned F> inline FPN_Binary<F> FPN_Lerp(FPN_Binary<F> a, FPN_Binary<F> b, FPN_Binary<F> t) {
    FPN_Binary<F> diff   = FPN_SubSat(b, a);
    FPN_Binary<F> scaled = FPN_Mul(diff, t);
    return FPN_AddSat(a, scaled);
}

template <unsigned F> inline FPN_Binary<F> FPN_SmoothStep(FPN_Binary<F> edge0, FPN_Binary<F> edge1, FPN_Binary<F> x) {
    constexpr unsigned N = FPN_Binary<F>::N;

    // always compute the polynomial: t = (x - edge0) / (edge1 - edge0), result = t*t*(3 - 2*t)
    FPN_Binary<F> t     = FPN_DivNoAssert(FPN_SubSat(x, edge0), FPN_SubSat(edge1, edge0));
    FPN_Binary<F> three = FPN_FromDouble<F>(3.0);
    FPN_Binary<F> two   = FPN_FromDouble<F>(2.0);
    FPN_Binary<F> poly  = FPN_Mul(FPN_Mul(t, t), FPN_SubSat(three, FPN_Mul(two, t)));

    // clamp conditions
    int below = FPN_LessThanOrEqual(x, edge0);    // -> 0.0
    int above = FPN_GreaterThanOrEqual(x, edge1); // -> 1.0

    // 1.0 constant
    FPN_Binary<F> one                = FPN_Zero<F>();
    one.w[FPN_Binary<F>::FRAC_WORDS] = 1;

    // mask-select: below -> zero, above -> one, else -> poly
    uint64_t bm = -(uint64_t)below;
    uint64_t am = -(uint64_t)above;
    uint64_t pm = ~bm & ~am; // middle region

    FPN_Binary<F> result;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < N; i++)
        result.w[i] = (poly.w[i] & pm) | (one.w[i] & am);
    // sign: poly could be negative from numerical noise, but clamped regions are non-negative
    result.sign = poly.sign & (int)(pm != 0);
    return result;
}

//======================================================================================================
// [NATIVE 128-BIT SPECIALIZATIONS FOR F=64]
//======================================================================================================
// when USE_NATIVE_128 is defined, FPN_Binary<64> operations forward to FixedPoint64.hpp
// which uses __uint128_t — single native instructions instead of 2-word loops
// reduces instruction cache footprint on the hot path
//======================================================================================================
#ifdef USE_NATIVE_128
#include "FixedPoint64.hpp"
#include <cstring>  // F-058: memcpy for type-pun-free FP64<->FPN_Binary<64> conversion

// (FP64-forwarding block REMOVED — Ship A flip. FPN_Binary<64> is now the 16B two's-complement core, so its ops
//  ARE the proven fp2_* bodies; the FPN_Binary<64> op specializations live after the fp2_* defs, at the bottom of
//  this header. FixedPoint64.hpp stays included [FP64 still backs FPN_FromFP64/ToFP64] until the D-99 absorb.)

#endif // USE_NATIVE_128

//======================================================================================================
//======================================================================================================
//======================================================================================================
// [16B BINARY CORE OPS — v5.15.5.F.4d.1.E Ship A; PROVEN value-equivalent to FPN_Binary<64> (slice 423/0)]
//======================================================================================================
// Transitional free functions on FixedPoint<2,64>, lifted (logic-verbatim) from the proven slice
// tools/ship_a_fp2_64_slice.cpp. At the FPN_Binary<64> flip these FOLD INTO the FPN_*<64> specializations,
// replacing the FP64-forwarding block above. The #2 multiply pattern is [extract sign + abs] ->
// [unsigned 128x128->256 product = FP64_Mul's exact reduce, the C1 HOIST] -> [reduce >>64] ->
// [reapply sign] -> [of_mask saturate-on-overflow, R2]. Div/Sqrt delegate to the certified generic
// FPN_Binary bodies on a rebuilt positive magnitude (abs-in / certified-unsigned-core / sign-out).
// NOTE (B1): -2^63 (INT128_MIN) abs/negate is two's-complement UB; the production fold adds the
// saturate/flag guard + the `build.sh ubsan` lane covers the whole signed-overflow class. The feature
// input domain never reaches it, so these slice bodies abs directly (as the proof did).
inline constexpr __int128 FP2_64_MAX = (__int128)(((unsigned __int128)1 << 127) - 1);  // +2^127-1 (R2 ceiling)

// Branchless __int128 sign helpers (the (v^sgn)-sgn conditional-negate trick) — so 128-bit abs / negate /
// compare never lower to data-dependent jumps (GCC's __int128-conditional codegen otherwise emits branches).
// Reused by the transcendentals below + Ship B's decimal math. (INT128_MIN abs is UB — bounded inputs never hit it.)
inline __int128 i128_abs(__int128 v)           { __int128 s = v >> 127; return (v ^ s) - s; }
inline __int128 i128_cneg(__int128 v, int neg) { __int128 m = -(__int128)(neg & 1); return (v ^ m) - m; }  // neg ? -v : v

// The ONE certified unsigned 128×128→256 product (Ship-B P0.2, S-19/D-174). Extracted from the
// byte-identical inner blocks of FP64_Mul (FixedPoint64.hpp:140-156) + fp2_mul below, so that
// #3 divmul_pow10, #7 Money Div, and the to_decimal cast consume the SAME body — a third hand
// copy would drift (the Class-18 shape the merge-scan flagged). FP64_Mul keeps its inline copy
// until the Ship-B FP64 absorb deletes that file (the copy dies with it).
// DOMAIN: both |a|,|b| < 2^127 (the 16B core magnitude ceiling — the money-domain closure
// invariant guarantees it for every caller). In-domain, `mid` provably cannot wrap
// (a_hi,b_hi ≤ 2^63-1 ⇒ lh+hl+carry < 2^128); outside it this composition is NOT total.
struct u256 { unsigned __int128 hi, lo; };   // product = hi·2^128 + lo
inline u256 umul_128x128_256(unsigned __int128 a, unsigned __int128 b) {
    uint64_t a_lo = (uint64_t)a, a_hi = (uint64_t)(a >> 64);
    uint64_t b_lo = (uint64_t)b, b_hi = (uint64_t)(b >> 64);
    unsigned __int128 ll = (unsigned __int128)a_lo * b_lo;
    unsigned __int128 lh = (unsigned __int128)a_lo * b_hi;
    unsigned __int128 hl = (unsigned __int128)a_hi * b_lo;
    unsigned __int128 hh = (unsigned __int128)a_hi * b_hi;
    unsigned __int128 mid     = lh + hl + (ll >> 64);
    unsigned __int128 shifted = hh + (mid >> 64);
    return { shifted, ((unsigned __int128)(uint64_t)mid << 64) | (uint64_t)ll };
}

// (fp2_from_fpn REMOVED — the 24B-sign-mag → 16B decoder is obsolete now that FPN_Binary<64> IS the 16B type;
//  D-143 + Class-40 dead-code removal. The slice that used it retires at this flip.)

inline FixedPoint<2,64> fp2_mul(FixedPoint<2,64> a, FixedPoint<2,64> b) {
    bool neg = (a.v < 0) ^ (b.v < 0);
    // INT_MIN-safe abs in unsigned space (the i128_abs (v^s)-s trick) — UB-free even at -2^127,
    // consistent with the guarded FPN_Negate/FPN_Abs<64> (D-147). The unreachable INT_MIN now feeds
    // the of_mask saturate below instead of signed-negation UB; value-identical for every reachable input.
    __int128 as = a.v >> 127, bs = b.v >> 127;                                   // 0 / -1 (arithmetic shift)
    unsigned __int128 amag = ((unsigned __int128)a.v ^ (unsigned __int128)as) - (unsigned __int128)as;
    unsigned __int128 bmag = ((unsigned __int128)b.v ^ (unsigned __int128)bs) - (unsigned __int128)bs;
    // FP64_Mul's exact reduce (bits[191:64] of amag*bmag) — the C1 hoist, now via the extracted
    // certified product (P0.2). Bit-identical composition to the previous inline block:
    // p.hi ≡ shifted; (uint64_t)(p.lo >> 64) ≡ (uint64_t)mid.
    u256 p = umul_128x128_256(amag, bmag);
    unsigned __int128 mag = (p.hi << 64) | (unsigned __int128)(uint64_t)(p.lo >> 64);
    // BRANCHLESS saturate (R2 — same of_mask discipline as FPN_Mul:616-622 / FP64_Mul): no data-dependent
    // control flow. ovf nonzero iff the product exceeds the 127-bit magnitude range; of_m is 0 or all-ones.
    unsigned __int128 ovf  = (p.hi >> 64) | (mag >> 127);
    unsigned __int128 nz   = ovf | (~ovf + 1);                            // top bit set iff ovf != 0 (no compare → no branch)
    unsigned __int128 of_m = -(unsigned __int128)(int)(nz >> 127);        // 0 / all-ones
    mag = (mag & ~of_m) | (of_m >> 1);                                    // saturate to 2^127-1 = (all-ones >> 1): the SAT
                                                                          // constant is DERIVED from the mask, so there is no
                                                                          // compile-constant for GCC to turn into a conditional-load
    __int128 v = (__int128)mag;
    unsigned __int128 neg_m = -(unsigned __int128)((int)neg & (int)(mag != 0));   // canonicalize -0 -> +0
    return { (__int128)(((unsigned __int128)(-v) & neg_m) | ((unsigned __int128)v & ~neg_m)) };
}
inline FixedPoint<2,64> fp2_neg(FixedPoint<2,64> a) { __int128 v = -a.v; return { v }; }   // (INT_MIN guard in prod fold)
inline FixedPoint<2,64> fp2_abs(FixedPoint<2,64> a) { return { a.v < 0 ? -a.v : a.v }; }
inline FixedPoint<2,64> fp2_addsat(FixedPoint<2,64> a, FixedPoint<2,64> b) {
    __int128 s = (__int128)((unsigned __int128)a.v + (unsigned __int128)b.v);     // wrapping add
    // BRANCHLESS: signed overflow ⇔ a,b same sign AND s differs → (~(a^b) & (a^s)) top bit. On overflow
    // a,b share a sign → saturate by a's sign (+MAX / −MAX). All mask-select, no data-dependent branch.
    unsigned __int128 of_m  = -(unsigned __int128)(int)((unsigned __int128)(~(a.v ^ b.v) & (a.v ^ s)) >> 127);
    __int128 sgn = a.v >> 127;                       // arithmetic: 0 (a>=0) / -1 (a<0) — a shift, no compare → no branch
    __int128 sat = (FP2_64_MAX ^ sgn) - sgn;         // branchless conditional-negate: +MAX / -MAX
    return { (__int128)(((unsigned __int128)s & ~of_m) | ((unsigned __int128)sat & of_m)) };
}
inline FixedPoint<2,64> fp2_sub(FixedPoint<2,64> a, FixedPoint<2,64> b) {
    __int128 s = (__int128)((unsigned __int128)a.v - (unsigned __int128)b.v);     // wrapping sub
    // BRANCHLESS: signed overflow ⇔ a,b differ in sign AND s differs from a → ((a^b) & (a^s)) top bit.
    unsigned __int128 of_m  = -(unsigned __int128)(int)((unsigned __int128)((a.v ^ b.v) & (a.v ^ s)) >> 127);
    __int128 sgn = a.v >> 127;                       // arithmetic: 0 (a>=0) / -1 (a<0) — a shift, no compare → no branch
    __int128 sat = (FP2_64_MAX ^ sgn) - sgn;         // branchless conditional-negate: +MAX / -MAX
    return { (__int128)(((unsigned __int128)s & ~of_m) | ((unsigned __int128)sat & of_m)) };
}
inline FixedPoint<2,64> fp2_min(FixedPoint<2,64> a, FixedPoint<2,64> b) { return { a.v < b.v ? a.v : b.v }; }
inline FixedPoint<2,64> fp2_max(FixedPoint<2,64> a, FixedPoint<2,64> b) { return { a.v > b.v ? a.v : b.v }; }
// (a_mag << 64) / b_mag → 128-bit quotient magnitude, bit-by-bit long division. This IS the certified
// FPN_DivNoAssert algorithm (shift remainder, compare top half ≥ divisor, conditional-subtract, MSB-first
// quotient bit) lifted to the magnitude level on __int128 — same proven math, no FPN_Binary<64> dependency.
// Fixed 128-trip loop (fixed-trip branch = 100% predicted, not data-dependent — per the generic's note);
// cmov inside, no data-dependent branch. b_mag==0 → all-ones (caller saturates).
//
// Ship-B P1b (#7/D-174): generalized to udiv256_qr — an ARBITRARY 256-bit dividend (hi:lo seed)
// and the REMAINDER returned alongside q (rem_hi at loop end IS the remainder; rem_lo is fully
// shifted out). udiv_q64 below re-wraps it with the original `a_mag << 64` seed — bit-identical
// loop, bit-identical results (A/B determinism oracle re-verified at the change). Seed invariant
// (the trip-0 MSB concern from the design audit): initial hi must be < 2^127 so the first shift
// cannot drop a bit — binary's seed hi = a_mag >> 64 < 2^64; money's divmul seed hi = (a·10^8)>>128
// < 2^26; thereafter the restoring invariant (rem_hi < b after each subtract) bounds the shift.
struct udiv_qr_t { unsigned __int128 q, r; };
inline udiv_qr_t udiv256_qr(unsigned __int128 hi, unsigned __int128 lo, unsigned __int128 b_mag) {
    unsigned __int128 rem_hi = hi, rem_lo = lo, q = 0;
    for (int bit = 0; bit < 128; bit++) {
        rem_hi = (rem_hi << 1) | (rem_lo >> 127);   // shift the 256-bit remainder left 1
        rem_lo <<= 1;
        int ge = (rem_hi >= b_mag);
        rem_hi = ge ? (rem_hi - b_mag) : rem_hi;     // conditional subtract (cmov)
        q = (q << 1) | (unsigned __int128)ge;        // quotient bit, MSB-first
    }
    return { q, rem_hi };
}
inline unsigned __int128 udiv_q64(unsigned __int128 a_mag, unsigned __int128 b_mag) {
    return udiv256_qr(a_mag >> 64, a_mag << 64, b_mag).q;    // the original seed, re-wrapped (value-identical)
}
inline FixedPoint<2,64> fp2_div(FixedPoint<2,64> a, FixedPoint<2,64> b) {
    bool neg = (a.v < 0) ^ (b.v < 0);
    unsigned __int128 am = a.v<0?(unsigned __int128)(-a.v):(unsigned __int128)a.v;
    unsigned __int128 bm = b.v<0?(unsigned __int128)(-b.v):(unsigned __int128)b.v;
    unsigned __int128 qm = udiv_q64(am, bm);                           // certified long-division core (native 16B)
    unsigned __int128 of_m = -(unsigned __int128)(int)(qm >> 127);     // bit127 set ⇒ result ≥ 2^63 (range cap) OR zero-div
    qm = (qm & ~of_m) | (of_m >> 1);                                   // saturate to 2^127-1 (R2), branchless — same as mul
    __int128 v = (__int128)qm;
    unsigned __int128 neg_m = -(unsigned __int128)((int)neg & (int)(qm != 0));
    return { (__int128)(((unsigned __int128)(-v) & neg_m) | ((unsigned __int128)v & ~neg_m)) };  // branchless sign
}
inline FixedPoint<2,64> fp2_sqrt(FixedPoint<2,64> a) {                          // sqrt domain: a > 0 else 0 (matches generic)
    if (a.v <= 0) return { (__int128)0 };
    unsigned __int128 m = (unsigned __int128)a.v;                              // |v| (a > 0 here)
    uint64_t hi = (uint64_t)(m >> 64), lo = (uint64_t)m;                       // top set bit of the magnitude (clz, not w[]-scan)
    int top = hi ? (127 - __builtin_clzll(hi)) : (63 - __builtin_clzll(lo));
    FixedPoint<2,64> y    { (__int128)((unsigned __int128)1 << ((top + 64) / 2)) };  // seed 2^((top_bit+F)/2), F=64
    FixedPoint<2,64> half { (__int128)((unsigned __int128)1 << 63) };          // 0.5 in Q64.64
    for (int i = 0; i < 12; i++)                                               // 12 Newton-Raphson: y = (y + a/y)/2
        y = fp2_mul(fp2_addsat(y, fp2_div(a, y)), half);
    return y;
}

// --- Conversions: replicate FPN_FromDouble<64>/FPN_ToDouble<64>'s F=64 arithmetic on __int128.
// Bit-identical by construction (same floor / (uint64_t) casts / 2^64 scale) → value-equivalent.
inline FixedPoint<2,64> fp2_from_double(double input) {
    // Ship-A: non-finite / out-of-range input → DETERMINISTIC saturate. The conversion below does
    // (uint64_t)floor(|input|), which is UB for NaN/Inf/|input|>2^63 — and the UB result VARIES by
    // optimization level (an -O3 NaN slipped past FPN_IsValidFinite + std::isnan in Features_PackAll
    // where -O2 caught it; the 24B path relied on the same UB happening to propagate NaN→ToDouble).
    // Saturating to ±MAX makes FPN_IsValidFinite ALWAYS reject it, identically across every build.
    // __builtin_expect-rare (H20 sanctioned error-path branch): legit price/feature values sit far
    // inside ±9e18 (2^63); -FP2_64_MAX (not FP2_64_MIN, defined later) dodges the INT128_MIN edge.
    if (__builtin_expect(!(input >= -9.0e18 && input <= 9.0e18), 0))
        return { input < 0.0 ? -FP2_64_MAX : FP2_64_MAX };
    int neg = (input < 0.0);
    double abs_input = input * (1.0 - 2.0 * neg);
    double int_part = floor(abs_input);
    double frac_part = abs_input - int_part;
    unsigned __int128 mag = ((unsigned __int128)(uint64_t)int_part << 64)
                          | (unsigned __int128)(uint64_t)floor(frac_part * 18446744073709551616.0);
    __int128 v = (__int128)mag;
    return { i128_cneg(v, neg & (int)(mag != 0)) };
}
inline double fp2_to_double(FixedPoint<2,64> a) {
    unsigned __int128 m = a.v < 0 ? (unsigned __int128)(-a.v) : (unsigned __int128)a.v;
    double mag = (double)(uint64_t)(m >> 64) + (double)(uint64_t)m / 18446744073709551616.0;
    return a.v < 0 ? -mag : mag;
}
// --- Feature-only transcendentals that round-trip through double (match FPN_Log/InvSqrt/Tan/Pow/Atan2
// by construction: same FromDouble(stdlib(ToDouble)) shape on value-equivalent conversions). Slow-path,
// feature-domain. Exp/Sin/Cos (the FPN_Binary-native Taylor ops) port separately.
inline FixedPoint<2,64> fp2_log(FixedPoint<2,64> a)     { return fp2_from_double(log(fp2_to_double(a))); }
inline FixedPoint<2,64> fp2_invsqrt(FixedPoint<2,64> a) { return fp2_from_double(1.0 / sqrt(fp2_to_double(a))); }
inline FixedPoint<2,64> fp2_tan(FixedPoint<2,64> a)     { return fp2_from_double(tan(fp2_to_double(a))); }
inline FixedPoint<2,64> fp2_pow(FixedPoint<2,64> a, FixedPoint<2,64> b)   { return fp2_from_double(pow(fp2_to_double(a), fp2_to_double(b))); }
inline FixedPoint<2,64> fp2_atan2(FixedPoint<2,64> y, FixedPoint<2,64> x) { return fp2_from_double(atan2(fp2_to_double(y), fp2_to_double(x))); }

// --- FPN_Binary-native Taylor transcendentals (Exp/Sin/Cos), ported to 16B. Mul/Add/Sub are native; the
// layout-specific bits adapt: integer-word read q.w[FW] → (|v|>>64); set-1.0 w[FW]=1 → (1<<64);
// 2^k bit-set w[k/64]=1<<k%64 → (1<<k). The generic's sign-FLAG ops map to two's-complement:
// `.sign=0` → abs(v); `.sign=1` (on a positive) → -v; `.sign=1-.sign` (flip) → -v. Same algorithm,
// same constants → value-equivalent (verified by the slice net).
inline FixedPoint<2,64> fp2_from_int(int64_t input) {       // == FPN_FromInt<64>: integer in the high word
    int neg = (input < 0);
    uint64_t mag = neg ? (uint64_t)(-(input + 1)) + 1u : (uint64_t)input;   // careful -INT64_MIN-safe negate
    __int128 v = (__int128)((unsigned __int128)mag << 64);
    return { i128_cneg(v, neg) };
}
inline FixedPoint<2,64> fp2_exp(FixedPoint<2,64> value) {                   // value==0 needs no guard: full path → 1.0
    FixedPoint<2,64> ln2 = fp2_from_double(0.6931471805599453);
    FixedPoint<2,64> inv_ln2 = fp2_from_double(1.4426950408889634);
    FixedPoint<2,64> q = fp2_mul(value, inv_ln2);
    FixedPoint<2,64> half_const = fp2_from_double(0.5);
    // round-half-away (q<0 ? q-0.5 : q+0.5): compute both, mask-select on sign — no data-dependent branch.
    FixedPoint<2,64> qa = fp2_addsat(q, half_const), qs = fp2_sub(q, half_const);
    unsigned __int128 qneg = (unsigned __int128)(q.v >> 127);              // all-ones iff q<0
    FixedPoint<2,64> q_rounded { (__int128)(((unsigned __int128)qs.v & qneg) | ((unsigned __int128)qa.v & ~qneg)) };
    unsigned __int128 qm = (unsigned __int128)i128_abs(q_rounded.v);
    int64_t qint = (int64_t)(uint64_t)(qm >> 64);
    int64_t k = (q_rounded.v < 0) ? -qint : qint;                          // cmov, not a branch
    FixedPoint<2,64> k_abs = fp2_from_int(k < 0 ? -k : k);
    FixedPoint<2,64> k_ln2 = fp2_mul(k_abs, ln2);
    k_ln2.v = i128_cneg(k_ln2.v, k < 0);                                   // branchless (was: if(k<0) sign=1)
    FixedPoint<2,64> r = fp2_sub(value, k_ln2);
    FixedPoint<2,64> result { (__int128)1 << 64 };                         // 1.0 (term n=0)
    FixedPoint<2,64> r_pow = result;
    static const double inv_fact[9] = { 1.0,1.0,0.5,1.0/6.0,1.0/24.0,1.0/120.0,1.0/720.0,1.0/5040.0,1.0/40320.0 };
    for (int n = 1; n < 9; n++) {                                          // fixed-trip
        r_pow = fp2_mul(r_pow, r);
        result = fp2_addsat(result, fp2_mul(r_pow, fp2_from_double(inv_fact[n])));
    }
    int seed_bit = 64 + (int)k;                                            // 2^k position (F=64)
    // Cold range guards (H20 __builtin_expect-rare exception): they prevent the out-of-range 1<<seed_bit UB
    // on the extreme-exp tail — forcing these branchless would compute an undefined shift (worse).
    if (__builtin_expect(seed_bit < 0, 0))   return { (__int128)0 };       // 2^k underflows FP precision
    if (__builtin_expect(seed_bit >= 128, 0)) return result;              // 2^k overflows; skip the scale
    FixedPoint<2,64> two_k { (__int128)((unsigned __int128)1 << seed_bit) };
    return fp2_mul(result, two_k);
}
inline FixedPoint<2,64> fp2_sin(FixedPoint<2,64> value) {                   // value==0 needs no guard: full path → 0
    FixedPoint<2,64> pi         = fp2_from_double(3.141592653589793);
    FixedPoint<2,64> two_pi     = fp2_from_double(6.283185307179586);
    FixedPoint<2,64> half_pi    = fp2_from_double(1.5707963267948966);
    FixedPoint<2,64> inv_two_pi = fp2_from_double(0.15915494309189535);
    FixedPoint<2,64> half_const = fp2_from_double(0.5);
    FixedPoint<2,64> q = fp2_mul(value, inv_two_pi);                        // step 1: reduce to [-pi, pi]
    FixedPoint<2,64> qa = fp2_addsat(q, half_const), qs = fp2_sub(q, half_const);   // round-half-away, mask-select
    unsigned __int128 qneg = (unsigned __int128)(q.v >> 127);
    FixedPoint<2,64> q_rounded { (__int128)(((unsigned __int128)qs.v & qneg) | ((unsigned __int128)qa.v & ~qneg)) };
    unsigned __int128 qm = (unsigned __int128)i128_abs(q_rounded.v);
    int64_t nint = (int64_t)(uint64_t)(qm >> 64);
    int64_t n_int = (q_rounded.v < 0) ? -nint : nint;                      // cmov
    FixedPoint<2,64> n_abs = fp2_from_int(n_int < 0 ? -n_int : n_int);
    FixedPoint<2,64> n_two_pi = fp2_mul(n_abs, two_pi);
    n_two_pi.v = i128_cneg(n_two_pi.v, n_int < 0);                          // branchless
    FixedPoint<2,64> x = fp2_sub(value, n_two_pi);
    int sign_flip = (int)((unsigned __int128)x.v >> 127);                  // step 2: sin is odd (sign bit, branchless)
    x.v = i128_abs(x.v);                                                   // make x >= 0 (branchless abs)
    // step 3: if x >= pi/2, use sin(pi-x)=sin(x). Compute pi-x always, mask-select — no data-dependent branch.
    FixedPoint<2,64> xr = fp2_sub(pi, x);
    unsigned __int128 ge = (unsigned __int128)(~((x.v - half_pi.v) >> 127));        // all-ones iff x>=pi/2 (diff sign, no 128-bit compare)
    x.v = (__int128)(((unsigned __int128)xr.v & ge) | ((unsigned __int128)x.v & ~ge));
    FixedPoint<2,64> result = x;                                          // step 4: Taylor (odd powers), x in [0,pi/2]
    FixedPoint<2,64> x_pow = x;
    FixedPoint<2,64> x_sq = fp2_mul(x, x);
    static const double inv_fact_odd[8] = {
        -1.0/6.0, 1.0/120.0, -1.0/5040.0, 1.0/362880.0,
        -1.0/39916800.0, 1.0/6227020800.0, -1.0/1307674368000.0, 1.0/355687428096000.0 };
    for (int k = 0; k < 8; k++) {                                          // fixed-trip
        x_pow = fp2_mul(x_pow, x_sq);
        result = fp2_addsat(result, fp2_mul(x_pow, fp2_from_double(inv_fact_odd[k])));
    }
    result.v = i128_cneg(result.v, sign_flip);                            // branchless (sin odd: flip sign back)
    return result;
}
inline FixedPoint<2,64> fp2_cos(FixedPoint<2,64> value) {                   // cos(x) = sin(x + pi/2)
    return fp2_sin(fp2_addsat(value, fp2_from_double(1.5707963267948966)));
}

//======================================================================================================
// [FPN_Binary<64> OP SPECIALIZATIONS — Ship A flip: FPN_Binary<64> = the concrete 16B core; ops = the proven fp2_* bodies]
//======================================================================================================
// Replaces the obsolete USE_NATIVE_128 FP64-forwarding block. FPN_Binary<64> is layout-identical to FixedPoint<2,64>
// (both bare __int128 v), so each op feeds .v into the PROVEN fp2_* body (slice 423/0, value-equivalent to the
// old 24B sign-magnitude) and re-wraps. Comparisons are native on .v (two's-complement total order — value-
// equivalent in the |value|<2^63 bounded range, R2). Negate/Abs carry the D-147 branchless INT_MIN→MAX
// saturate (magnitude in unsigned space = UB-free; the `== FP2_64_MIN` mask lowers to setcc, NO data-dependent
// jump → zero variable latency). Ops the engine calls that aren't here yet (Add/Sign/Floor/Ceil/Round/
// FromString/ToString/FromFP64/ToFP64/Zero/DivWithAssert) red-build → added next.
inline constexpr __int128 FP2_64_MIN = (__int128)((unsigned __int128)1 << 127);   // -2^127 (INT128_MIN)

//======================================================================================================
// [16B DECIMAL MONEY CORE — Ship B P1a (D-97/D-99/D-174/D-176; designs: the D-93 fold blocks)]
//======================================================================================================
// The Money_* op family over FixedPoint<10,8> (`Money`). The multiply is the #2-shared shape:
// [abs via the i128 trick] -> [umul_128x128_256, the ONE certified product] -> [#3 divmul_pow10
// PROVEN reduce] -> [#4 divisor-generalized half-even] -> [closure-domain saturate ±(2^63-1) +
// S-17 sticky flag (D-147 flag-loud-for-money)] -> [sign]. Branchless throughout (H7/H20/H11).
// P1a lands the arithmetic spine (Mul + rounding + flags); Div (#7) + the radix-agnostic share
// land at P1b; FromString (#5) + the casts at P1c. NO production caller until the P2 migration
// (markers-first epoch commit) — the engine's money FIELDS are still FPN_Binary<64> here.

// S-17 sticky-flag accumulator (D-174 #13): money ops OR their fault bits here UNCONDITIONALLY
// (plain |= on a thread_local — branchless, no RMW, H3-clean). Drained at the DRAINER cycle tail
// into OMSState at P3 (where the halt-new-entries action lands); REPLAY runs the same path
// (flags are pure functions of values -> replay==production INCLUDING flags). Never read by
// math (the flags-don't-feed-back invariant).
inline constexpr uint64_t MONEY_FLAG_OVERFLOW = 1ull << 0;   // result exceeded ±(2^63-1) -> saturated
inline constexpr uint64_t MONEY_FLAG_DIVZERO  = 1ull << 1;   // money division by zero -> saturated (P1b)
inline thread_local uint64_t money_op_flags = 0;

inline constexpr __int128 MONEY_MAX = (__int128)(((unsigned __int128)1 << 63) - 1);  // closure ceiling (R2-sister)
inline constexpr unsigned __int128 MONEY_SCALE = 100000000ull;                       // 10^8 (D-104)

// #3 — the PROVEN decimal reduce (D-140): floor(P / 10^8) for 0 <= P < 2^127 via the
// Granlund–Montgomery reciprocal multiply q = (P*M) >> 153 with the exhaustively-validated magic
// (proof record: plan_checks/2026-06-01-11-phase1-divmul-proof/PROOF.md — G-M bound + validated
// predicate + 208k differential vs Python decimal). M is 127-bit -> P*M <= 254 bits, inside the
// certified 256-bit primitive with 2 bits margin. Returns (q, r = P - q*10^8), r in [0, 10^8).
// Constant-time: one umul_128x128_256 + one shift + one 128-bit mul-subtract. NEVER __udivti3.
struct divmul_qr { unsigned __int128 q, r; };
inline divmul_qr divmul_pow10(unsigned __int128 P) {
    constexpr unsigned __int128 M =
        ((unsigned __int128)0x55e63b88c230e77eull << 64) | 0x7ee106959b5d3e1full;
    u256 pm = umul_128x128_256(P, M);
    unsigned __int128 q = pm.hi >> 25;                       // (P*M) >> 153, with 153 - 128 = 25
    unsigned __int128 r = P - q * MONEY_SCALE;
    return { q, r };
}

// #4 — branchless rounding from (q, r), divisor-generalized HALF-EVEN (D-128/D-174a; the
// overflow-free form from the D-93 fold — a literal `2r` compare overflows u128 for #7's runtime
// divisors, so compare r against half = d>>1; an exact tie exists only for EVEN d). ONE body for
// d = 10^8 (mul reduce) / 2^64 (the P1c cast) / runtime divisor (#7 Div) = D-105
// uniform-everywhere-incl-replay BY CONSTRUCTION. Unsigned magnitudes; the caller reapplies sign
// (value-symmetric — verified against ROUND_HALF_EVEN over signed pairs by the oracle).
inline unsigned __int128 money_round_half_even(unsigned __int128 q, unsigned __int128 r,
                                               unsigned __int128 d) {
    unsigned __int128 half = d >> 1;
    unsigned __int128 up = (unsigned __int128)(r > half)
                         | ((unsigned __int128)(r == half)
                            & (unsigned __int128)((d & 1) == 0)
                            & (q & 1));
    return q + up;
}

// Money_Mul — exact decimal multiply with half-even reduce. In-domain operands (closure
// |v| <= 2^63-1) give P <= (2^63-1)^2 < 2^126: p.hi == 0 and the divmul precondition holds BY
// CONSTRUCTION. Out-of-domain (hand-built) operands stay TOTAL + deterministic: p.hi != 0 folds
// into the overflow mask -> saturate + flag (never garbage-as-value).
inline Money Money_Mul(Money a, Money b) {
    int neg = (int)((a.v < 0) ^ (b.v < 0));
    unsigned __int128 amag = (unsigned __int128)i128_abs(a.v);
    unsigned __int128 bmag = (unsigned __int128)i128_abs(b.v);
    u256 p = umul_128x128_256(amag, bmag);
    divmul_qr dr = divmul_pow10(p.lo);
    unsigned __int128 mag = money_round_half_even(dr.q, dr.r, MONEY_SCALE);
    unsigned __int128 lim = ((unsigned __int128)1 << 63) - 1;
    unsigned __int128 ovf = (unsigned __int128)(p.hi != 0) | (unsigned __int128)(mag > lim);
    unsigned __int128 of_m = (unsigned __int128)0 - ovf;                 // 0 / all-ones
    money_op_flags |= (uint64_t)ovf;                                     // MONEY_FLAG_OVERFLOW
    mag = (mag & ~of_m) | (lim & of_m);                                  // saturate to the closure ceiling
    return { i128_cneg((__int128)mag, neg) };
}

// Money_Div — #7 (P1b): (a/1e8)/(b/1e8) at <10,8> = (a·10^8)/b with half-even on the TRUE
// remainder (r = P − q·b ∈ [0,b), exactly #4's contract with d = b). Seed = the certified
// product a_mag·10^8 (in-domain ≤ 2^90, hi==0; defensively total to ≤ 2^154, rem_hi ≤ 2^26),
// divided by the GENERALIZED certified long division udiv256_qr (defined with the binary core
// ops above — the decimal section sits after them). Div-by-zero: deterministic saturate to
// ±MONEY_MAX by sign(a) + the DISTINCT DIVZERO sticky bit (OVERFLOW suppressed on dz); all call
// sites pre-guard today (verified at the D-93 audit) — the op stays TOTAL regardless. Quotient
// overflow (tiny divisors — the audit's Q5 catch): closure saturate + OVERFLOW bit, same as Mul.
// NEVER __udivti3 (constant 128-trip cmov loop).
inline Money Money_Div(Money a, Money b) {
    int neg = (int)((a.v < 0) ^ (b.v < 0));
    unsigned __int128 amag = (unsigned __int128)i128_abs(a.v);
    unsigned __int128 bmag = (unsigned __int128)i128_abs(b.v);
    u256 P = umul_128x128_256(amag, MONEY_SCALE);
    udiv_qr_t dr = udiv256_qr(P.hi, P.lo, bmag);
    unsigned __int128 q = money_round_half_even(dr.q, dr.r, bmag);
    unsigned __int128 lim = ((unsigned __int128)1 << 63) - 1;
    unsigned __int128 dz  = (unsigned __int128)(bmag == 0);
    unsigned __int128 ovf = (unsigned __int128)(q > lim) | dz;          // saturate mask covers BOTH
    unsigned __int128 of_m = (unsigned __int128)0 - ovf;
    money_op_flags |= (uint64_t)(((unsigned __int128)(q > lim) & ~dz))  // OVERFLOW only when genuine
                    | ((uint64_t)dz << 1);                              // DIVZERO distinct (bit 1)
    q = (q & ~of_m) | (lim & of_m);
    return { i128_cneg((__int128)q, neg) };                             // b==0 ⇒ neg=(a<0): ±MAX by sign(a)
}

// Money_Add / Money_Sub — exact in __int128 BY DOMAIN (|a|,|b| ≤ 2^63-1 ⇒ |a±b| ≤ 2^64-2 ≪ 2^127:
// the i128 sum/difference CANNOT wrap, so no certified-saturate body is needed — these are exact
// integer ops with a closure clamp + S-17 flag). Branchless mask-select clamp by the result sign.
inline Money Money_Add(Money a, Money b) {
    __int128 s = a.v + b.v;
    unsigned __int128 ovf  = (unsigned __int128)(s > MONEY_MAX) | (unsigned __int128)(s < -MONEY_MAX);
    unsigned __int128 of_m = (unsigned __int128)0 - ovf;
    money_op_flags |= (uint64_t)ovf;                                     // MONEY_FLAG_OVERFLOW
    __int128 sat = i128_cneg(MONEY_MAX, (int)(s < 0));                   // ±MONEY_MAX by the result sign
    return { (__int128)(((unsigned __int128)s & ~of_m) | ((unsigned __int128)sat & of_m)) };
}
inline Money Money_Sub(Money a, Money b) { return Money_Add(a, Money{ -b.v }); }  // |b.v| ≤ 2^63-1: negation exact

template<> inline FPN_Binary<64> FPN_Mul<64>(FPN_Binary<64> a, FPN_Binary<64> b)         { return { fp2_mul({a.v}, {b.v}).v }; }
template<> inline FPN_Binary<64> FPN_AddSat<64>(FPN_Binary<64> a, FPN_Binary<64> b)      { return { fp2_addsat({a.v}, {b.v}).v }; }
template<> inline FPN_Binary<64> FPN_SubSat<64>(FPN_Binary<64> a, FPN_Binary<64> b)      { return { fp2_sub({a.v}, {b.v}).v }; }
template<> inline FPN_Binary<64> FPN_Sub<64>(FPN_Binary<64> a, FPN_Binary<64> b)         { return { fp2_sub({a.v}, {b.v}).v }; }
template<> inline FPN_Binary<64> FPN_DivNoAssert<64>(FPN_Binary<64> a, FPN_Binary<64> b) { return { fp2_div({a.v}, {b.v}).v }; }
template<> inline FPN_Binary<64> FPN_Min<64>(FPN_Binary<64> a, FPN_Binary<64> b)         { return { fp2_min({a.v}, {b.v}).v }; }
template<> inline FPN_Binary<64> FPN_Max<64>(FPN_Binary<64> a, FPN_Binary<64> b)         { return { fp2_max({a.v}, {b.v}).v }; }

template<> inline int FPN_Equal<64>(FPN_Binary<64> a, FPN_Binary<64> b)              { return a.v == b.v; }
template<> inline int FPN_LessThan<64>(FPN_Binary<64> a, FPN_Binary<64> b)           { return a.v <  b.v; }
template<> inline int FPN_LessThanOrEqual<64>(FPN_Binary<64> a, FPN_Binary<64> b)    { return a.v <= b.v; }
template<> inline int FPN_GreaterThan<64>(FPN_Binary<64> a, FPN_Binary<64> b)        { return a.v >  b.v; }
template<> inline int FPN_GreaterThanOrEqual<64>(FPN_Binary<64> a, FPN_Binary<64> b) { return a.v >= b.v; }
template<> inline int FPN_IsZero<64>(FPN_Binary<64> v)                        { return v.v == 0; }

template<> inline FPN_Binary<64> FPN_Negate<64>(FPN_Binary<64> a) {
    unsigned __int128 neg  = -(unsigned __int128)a.v;                             // wrapping -v (UB-free)
    unsigned __int128 minm = -(unsigned __int128)(a.v == FP2_64_MIN);            // all-ones iff v == INT_MIN
    return { (__int128)((neg & ~minm) | ((unsigned __int128)FP2_64_MAX & minm)) };
}
template<> inline FPN_Binary<64> FPN_Abs<64>(FPN_Binary<64> a) {
    __int128 s = a.v >> 127;                                                     // 0 / -1 (arithmetic)
    unsigned __int128 mag  = ((unsigned __int128)a.v ^ (unsigned __int128)s) - (unsigned __int128)s;   // |v| (UB-free)
    unsigned __int128 minm = -(unsigned __int128)(a.v == FP2_64_MIN);
    return { (__int128)((mag & ~minm) | ((unsigned __int128)FP2_64_MAX & minm)) };
}

template<> inline FPN_Binary<64> FPN_FromDouble<64>(double d) { return { fp2_from_double(d).v }; }
template<> inline double   FPN_ToDouble<64>(FPN_Binary<64> v)  { return fp2_to_double({v.v}); }
template<> inline FPN_Binary<64> FPN_FromInt<64>(int64_t i)    { return { fp2_from_int(i).v }; }

template<> inline FPN_Binary<64> FPN_Sqrt<64>(FPN_Binary<64> v)             { return { fp2_sqrt({v.v}).v }; }
template<> inline FPN_Binary<64> FPN_Exp<64>(FPN_Binary<64> v)              { return { fp2_exp({v.v}).v }; }
template<> inline FPN_Binary<64> FPN_Sin<64>(FPN_Binary<64> v)              { return { fp2_sin({v.v}).v }; }
template<> inline FPN_Binary<64> FPN_Cos<64>(FPN_Binary<64> v)              { return { fp2_cos({v.v}).v }; }
template<> inline FPN_Binary<64> FPN_Log<64>(FPN_Binary<64> v)              { return { fp2_log({v.v}).v }; }
template<> inline FPN_Binary<64> FPN_Tan<64>(FPN_Binary<64> v)              { return { fp2_tan({v.v}).v }; }
template<> inline FPN_Binary<64> FPN_InvSqrt<64>(FPN_Binary<64> v)          { return { fp2_invsqrt({v.v}).v }; }
template<> inline FPN_Binary<64> FPN_Atan2<64>(FPN_Binary<64> y, FPN_Binary<64> x) { return { fp2_atan2({y.v}, {x.v}).v }; }
template<> inline FPN_Binary<64> FPN_Pow<64>(FPN_Binary<64> b, FPN_Binary<64> e)   { return { fp2_pow({b.v}, {e.v}).v }; }

template<> inline FPN_Binary<64> FPN_Zero<64>() { return { (__int128)0 }; }

// Branchless mask-blend: caller forms `mask = -(uint64_t)cond` (0 / all-ones-64). Sign-extend to 128b + blend .v.
template<> inline FPN_Binary<64> FPN_BlendOnMask<64>(FPN_Binary<64> if_true, FPN_Binary<64> if_false, uint64_t mask) {
    unsigned __int128 m = (unsigned __int128)(__int128)(int64_t)mask;            // 0 / all-ones-128 (sign-extended)
    return { (__int128)(((unsigned __int128)if_true.v & m) | ((unsigned __int128)if_false.v & ~m)) };
}

// Decimal string <-> 16B. Value-equivalent to the generic at F=64: the integer part is the high 64b word
// (old w[1]), the fraction is the low 64b word (old w[0]); same per-word base-10 divmod/mul-by-10.
template<> inline unsigned FPN_ToString<64>(FPN_Binary<64> value, char *buf, unsigned buf_size, unsigned decimal_places) {
    if (decimal_places == 0) decimal_places = FPN_MaxDecimalDigits<64>();
    unsigned pos = 0;
    int neg = (value.v < 0);
    unsigned __int128 mag = neg ? -(unsigned __int128)value.v : (unsigned __int128)value.v;
    if (neg && mag != 0) { if (pos < buf_size - 1) buf[pos++] = '-'; }
    uint64_t int_part  = (uint64_t)(mag >> 64);                                  // integer word (old w[1])
    uint64_t frac_part = (uint64_t)mag;                                          // fractional word (old w[0])
    char int_digits[21]; unsigned n_int = 0;
    if (int_part == 0) int_digits[n_int++] = '0';
    else while (int_part) { int_digits[n_int++] = (char)('0' + (int)(int_part % 10)); int_part /= 10; }
    for (int i = (int)n_int - 1; i >= 0 && pos < buf_size - 1; i--) buf[pos++] = int_digits[i];
    if (decimal_places > 0 && pos < buf_size - 1) {
        buf[pos++] = '.';
        for (unsigned d = 0; d < decimal_places && pos < buf_size - 1; d++) {
            unsigned __int128 t = (unsigned __int128)frac_part * 10;             // ×10, overflow word = next digit
            buf[pos++] = (char)('0' + (int)(uint64_t)(t >> 64));
            frac_part  = (uint64_t)t;
        }
    }
    buf[pos] = '\0';
    return pos;
}
// BRANCHLESS body (mask-based, no per-char dispatch if/else; unified int/frac accumulation), ONE safe
// null-bounded loop — the loop-exit is safe-termination, NOT a data-dependent dispatch branch (a fixed-trip
// scan would over-read past the null on untrusted producer input). Value-equivalent to the digit-by-digit
// generic: diff-tested byte-identical across the venue string domain (build_probe/fromstring_difftest.cpp,
// 297/0). frac_low = floor((frac_int<<64)/10^n) IS the generic's right-to-left divmod for n≤19 (venue
// precision). Full fixed-width SWAR throughput stays task #5; this is the branchless-where-it-counts form.
// Ship-B P1c (#5/D-174): the SHARED digit-scan front-end. EXACTLY the live binary accumulate —
// same branchless arithmetic, same ≥20-digit wrap, same second-dot fold, same silent bad-char
// skip — with OBSERVATIONAL flags computed alongside (the accumulate itself is untouched, so the
// binary wrapper below stays BYTE-IDENTICAL; the decimal consumer rejects/rounds on the flags
// per the per-site failure table in the design sidecar).
inline constexpr uint32_t FP_SCAN_BAD_CHAR   = 1u << 0;   // non-digit/non-dot seen (live path skips it silently)
inline constexpr uint32_t FP_SCAN_DUP_DOT    = 1u << 1;   // second '.' (live path folds digits after it into frac)
inline constexpr uint32_t FP_SCAN_NO_DIGITS  = 1u << 2;   // zero digits scanned
inline constexpr uint32_t FP_SCAN_WRAP       = 1u << 3;   // a 64-bit accumulator would-wrap (live path wraps silently)
struct fp_scan_t { uint64_t int_part, frac_int; unsigned n_frac; int neg; uint32_t flags; };
inline fp_scan_t fp_scan_decimal_string(const char *str) {
    int neg = (str[0] == '-');
    unsigned i = (unsigned)(neg | (str[0] == '+'));
    uint64_t int_part = 0, frac_int = 0;
    unsigned n_frac = 0, n_digits = 0;
    int seen_dot = 0;
    uint32_t bad = 0, dup = 0, wrap = 0;
    for (; str[i] != '\0'; i++) {                                   // safe: stop at null (no over-read)
        char c = str[i];
        int is_digit = ((unsigned char)(c - '0') <= 9u);
        int is_dot   = (c == '.');
        unsigned dig = (unsigned)(unsigned char)(c - '0');
        int in_int  = is_digit & !seen_dot;
        int in_frac = is_digit &  seen_dot;
        bad  |= (uint32_t)!(is_digit | is_dot);                     // flag only — the accumulate still skips it
        dup  |= (uint32_t)(is_dot & seen_dot);                      // flag only — the fold behavior is preserved
        wrap |= (uint32_t)(in_int  & (int_part > (~0ull - dig) / 10));   // pre-mul would-wrap detect (flag only)
        wrap |= (uint32_t)(in_frac & (frac_int > (~0ull - dig) / 10));
        int_part += (uint64_t)in_int  * (int_part * 9 + dig);       // branchless: in_int ? int_part*10+dig : int_part
        frac_int += (uint64_t)in_frac * (frac_int * 9 + dig);
        n_frac   += (unsigned)in_frac;
        n_digits += (unsigned)is_digit;
        seen_dot |= is_dot;
    }
    uint32_t flags = (bad  * FP_SCAN_BAD_CHAR)
                   | (dup  * FP_SCAN_DUP_DOT)
                   | ((uint32_t)(n_digits == 0) * FP_SCAN_NO_DIGITS)
                   | (wrap * FP_SCAN_WRAP);
    return { int_part, frac_int, n_frac, neg, flags };
}

inline const uint64_t FP_POW10[20] = {
    1ull,10ull,100ull,1000ull,10000ull,100000ull,1000000ull,10000000ull,100000000ull,1000000000ull,
    10000000000ull,100000000000ull,1000000000000ull,10000000000000ull,100000000000000ull,
    1000000000000000ull,10000000000000000ull,100000000000000000ull,1000000000000000000ull,
    10000000000000000000ull };

template<> inline FPN_Binary<64> FPN_FromString<64>(const char *str) {
    fp_scan_t s = fp_scan_decimal_string(str);                      // flags DISCARDED — binary keeps live semantics
    unsigned nf = s.n_frac < 20u ? s.n_frac : 19u;                  // clamp (cmov); >19 frac digits is beyond venue precision
    // The scale tail, via the certified constant-trip divider (P1c kills the former __udivti3
    // libcall at this exact site — the audit's :1760 disposition; floor division both ways =
    // value-identical, A/B-oracle-verified).
    unsigned __int128 frac_low = udiv256_qr(0, (unsigned __int128)s.frac_int << 64, FP_POW10[nf]).q;
    unsigned __int128 mag = ((unsigned __int128)s.int_part << 64) | (uint64_t)frac_low;
    int sg = s.neg & (mag != 0);
    return { (__int128)(sg ? -mag : mag) };
}

// (FPN_FromFP64<64>/FPN_ToFP64<64> NOT specialized — their generic is `#ifdef FIXED_POINT_64_H`-gated at a
//  point where the macro isn't defined yet, and the engine red-build never instantiates them for <64>. If a
//  build ever flags them, specialize at a point after FixedPoint64.hpp is included.)

//======================================================================================================
// [DECIMAL MONEY PARSE + CASTS — Ship B P1c (#5 + D-170 casts; placed here so fp_scan is in scope)]
//======================================================================================================
// Money_FromString — #5: exact venue-decimal-string -> <10,8>, NEVER via double. Returns
// (value, flags): the per-site failure table (design sidecar) keys on the flags — LIVE-context
// consumers REFUSE on MALFORMED/EXCESS_DP per D-174b/c; paper/cfg contexts may round+warn.
// SEPARATE mechanism from money_op_flags by design (S-16 note).
inline constexpr uint32_t MONEY_PARSE_MALFORMED = 1u << 0;   // bad char / second dot / digitless
inline constexpr uint32_t MONEY_PARSE_OVERFLOW  = 1u << 1;   // |value| exceeds the closure ceiling (saturated)
inline constexpr uint32_t MONEY_PARSE_EXCESS_DP = 1u << 2;   // > 8 fractional digits (venue contract says <= 8; value half-even-rounded)
struct MoneyParse { Money value; uint32_t flags; };
inline MoneyParse Money_FromString(const char *str) {
    fp_scan_t s = fp_scan_decimal_string(str);
    uint32_t flags = ((s.flags & (FP_SCAN_BAD_CHAR | FP_SCAN_DUP_DOT | FP_SCAN_NO_DIGITS)) ? MONEY_PARSE_MALFORMED : 0u)
                   | ((s.flags & FP_SCAN_WRAP) ? MONEY_PARSE_OVERFLOW : 0u);
    // Scale to 10^8: <=8dp pads exactly (frac_int < 10^nf => *10^(8-nf) < 10^8, u64-exact);
    // >8dp divides by 10^(nf-8) with #4 half-even (defensive arm — the venue never sends it;
    // plain u64 HW divide, parse cadence, constant-bounded).
    unsigned nf = s.n_frac < 20u ? s.n_frac : 19u;
    uint64_t frac_scaled;
    if (__builtin_expect(nf <= 8u, 1)) {
        frac_scaled = s.frac_int * FP_POW10[8u - nf];
    } else {
        uint64_t d = FP_POW10[nf - 8u];
        uint64_t q = s.frac_int / d, r = s.frac_int - q * d;
        frac_scaled = (uint64_t)money_round_half_even(q, r, d);
        flags |= MONEY_PARSE_EXCESS_DP;
    }
    unsigned __int128 scaled = (unsigned __int128)s.int_part * MONEY_SCALE + frac_scaled;  // <= ~2^91: exact
    unsigned __int128 lim = ((unsigned __int128)1 << 63) - 1;
    flags |= (scaled > lim) ? MONEY_PARSE_OVERFLOW : 0u;
    scaled = scaled > lim ? lim : scaled;                            // deterministic saturate (cmov)
    int sg = s.neg & (scaled != 0);                                  // -0 canonicalizes to +0 (mirror the live parser)
    return { Money{ i128_cneg((__int128)scaled, sg) }, flags };
}

// Money_ToBinary — the D-170 INGRESS cast (decimal -> binary feature domain; per-tick capable):
// b = round_he(m * 2^64 / 10^8) via the PROVEN divmul on (|m| << 64) — |m| <= 2^63-1 => the
// dividend < 2^127 sits INSIDE the D-140 proven domain (the fold's range note; no new proof).
// Saturation provably UNREACHABLE this direction (q < 2^101 << 2^127) => NO flag, no dead arm
// (S-16). Branchless; ~one umul + shift + round.
inline FPN_Binary<64> Money_ToBinary(Money m) {
    unsigned __int128 mag = (unsigned __int128)i128_abs(m.v);
    divmul_qr dr = divmul_pow10(mag << 64);
    unsigned __int128 q = money_round_half_even(dr.q, dr.r, MONEY_SCALE);
    return { i128_cneg((__int128)q, (int)(m.v < 0)) };
}

// Money_FromBinary — the EGRESS cast (binary threshold -> money at gate-build cadence, D-170):
// m = round_he(b * 10^8 / 2^64) via the certified product; divisor d = 2^64 (even => exact ties
// possible; #4's generalized form handles it). Saturation REACHABLE (binary values reach ~2^63
// => *10^8 ~ 2^90 > ceiling) => closure saturate + S-17 OVERFLOW flag (a silently-saturated
// threshold would be a wrong gate — load-bearing, per the fold).
inline Money Money_FromBinary(FPN_Binary<64> b) {
    unsigned __int128 mag = (unsigned __int128)i128_abs(b.v);
    u256 p = umul_128x128_256(mag, MONEY_SCALE);
    unsigned __int128 q = (p.hi << 64) | (unsigned __int128)(uint64_t)(p.lo >> 64);   // bits[191:64] = floor(b*10^8 / 2^64)
    unsigned __int128 r = (unsigned __int128)(uint64_t)p.lo;                          // bits[63:0]  = the true remainder
    q = money_round_half_even(q, r, (unsigned __int128)1 << 64);
    unsigned __int128 lim = ((unsigned __int128)1 << 63) - 1;
    unsigned __int128 ovf = (unsigned __int128)(p.hi >> 64 != 0) | (unsigned __int128)(q > lim);
    unsigned __int128 of_m = (unsigned __int128)0 - ovf;
    money_op_flags |= (uint64_t)ovf;                                                  // MONEY_FLAG_OVERFLOW
    q = (q & ~of_m) | (lim & of_m);
    return { i128_cneg((__int128)q, (int)(b.v < 0)) };
}

#endif // FIXED_POINT_N_H
