// Ship-A value-equivalence net — prove the PRODUCTION header ops on the 16B two's-complement
// FixedPoint<2,64> are VALUE-equivalent to the 24B sign-magnitude FPN<64>, over a feature-domain input
// vector. Tests the REAL functions in FixedPoint/FixedPointN.hpp (fp2_*), not local copies — so it can't
// silently drift from production. Pre-flip proof (valid while 24B FPN<64> + 16B FixedPoint<2,64> coexist;
// at the flip FPN<64> BECOMES FixedPoint<2,64> and this collapses to the byte-determinism golden). D-125/D-139.
//
// build+run (note: /tmp is noexec on this box — emit the binary into the repo dir):
//   g++ -std=c++20 -O3 -march=native -DUSE_NATIVE_128 -I. tools/ship_a_fp2_64_slice.cpp -o ./_x && ./_x; rm -f ./_x
#include "FixedPoint/FixedPointN.hpp"
#include <cstdio>
#include <cstdint>
using namespace std;

using FP2 = FixedPoint<2,64>;   // the production 16B type under test

// Extract (sign, 128-bit magnitude) from each representation for an exact VALUE comparison.
static void val_fpn(FPN<64> r, int& sign, unsigned __int128& mag) {
    mag = ((unsigned __int128)r.w[1] << 64) | (unsigned __int128)r.w[0];
    sign = (r.sign && mag != 0) ? 1 : 0;
}
static void val_fp2(FP2 r, int& sign, unsigned __int128& mag) {
    mag = r.v < 0 ? (unsigned __int128)(-r.v) : (unsigned __int128)r.v;
    sign = (r.v < 0 && mag != 0) ? 1 : 0;
}
static int check(const char* tag, FPN<64> ref, FP2 got) {
    int rs, gs; unsigned __int128 rm, gm;
    val_fpn(ref, rs, rm); val_fp2(got, gs, gm);
    if (rs != gs || rm != gm) {
        printf("  MISMATCH %-12s FPN %c%016llx%016llx  FP2 %c%016llx%016llx\n", tag,
               rs?'-':'+',(unsigned long long)(rm>>64),(unsigned long long)rm,
               gs?'-':'+',(unsigned long long)(gm>>64),(unsigned long long)gm);
        return 1;
    }
    return 0;
}

int main() {
    static const double IN[] = { 2.0,3.0,1.5,0.25,100.0,12345.678,2.0000001,0.0001,9999999.0,
        0.5,1.0,7.0,0.1,0.3,1000000.0,0.000001,42.42,3.14159265358979 };
    const int N = (int)(sizeof(IN) / sizeof(IN[0]));
    char t[24]; int tested = 0, miss = 0;

    for (int i = 0; i < N; ++i) {                       // unary + conversions + double-round-trip transcendentals
        FPN<64> x = FPN_FromDouble<64>(IN[i]); FP2 fx = fp2_from_fpn(x);
        snprintf(t,sizeof t,"Abs[%d]",i);    miss += check(t, FPN_Abs<64>(x),    fp2_abs(fx)); tested++;
        snprintf(t,sizeof t,"Negate[%d]",i); miss += check(t, FPN_Negate<64>(x), fp2_neg(fx)); tested++;
        snprintf(t,sizeof t,"Sqrt[%d]",i);   miss += check(t, FPN_Sqrt<64>(x),   fp2_sqrt(fx)); tested++;
        snprintf(t,sizeof t,"FromDbl[%d]",i);miss += check(t, FPN_FromDouble<64>(IN[i]), fp2_from_double(IN[i])); tested++;
        snprintf(t,sizeof t,"Log[%d]",i);    miss += check(t, FPN_Log<64>(x),     fp2_log(fx));     tested++;
        snprintf(t,sizeof t,"InvSqrt[%d]",i);miss += check(t, FPN_InvSqrt<64>(x), fp2_invsqrt(fx)); tested++;
        snprintf(t,sizeof t,"Tan[%d]",i);    miss += check(t, FPN_Tan<64>(x),     fp2_tan(fx));     tested++;
        if (FPN_ToDouble<64>(x) != fp2_to_double(fx)) { printf("  MISMATCH ToDbl[%d] %.17g vs %.17g\n", i, FPN_ToDouble<64>(x), fp2_to_double(fx)); miss++; } tested++;
    }
    for (int i = 0; i + 1 < N; ++i) {                   // binary
        FPN<64> a = FPN_FromDouble<64>(IN[i]), b = FPN_FromDouble<64>(IN[i+1]);
        FP2 fa = fp2_from_fpn(a), fb = fp2_from_fpn(b);
        snprintf(t,sizeof t,"Mul[%d]",i);    miss += check(t, FPN_Mul<64>(a,b),    fp2_mul(fa,fb));    tested++;
        snprintf(t,sizeof t,"AddSat[%d]",i); miss += check(t, FPN_AddSat<64>(a,b), fp2_addsat(fa,fb)); tested++;
        snprintf(t,sizeof t,"SubSat[%d]",i); miss += check(t, FPN_SubSat<64>(a,b), fp2_sub(fa,fb));    tested++;
        snprintf(t,sizeof t,"Sub[%d]",i);    miss += check(t, FPN_Sub<64>(a,b),    fp2_sub(fa,fb));    tested++;
        snprintf(t,sizeof t,"Min[%d]",i);    miss += check(t, FPN_Min<64>(a,b),    fp2_min(fa,fb));    tested++;
        snprintf(t,sizeof t,"Max[%d]",i);    miss += check(t, FPN_Max<64>(a,b),    fp2_max(fa,fb));    tested++;
        snprintf(t,sizeof t,"Div[%d]",i);    miss += check(t, FPN_DivNoAssert<64>(a,b), fp2_div(fa,fb)); tested++;
    }
    for (int i = 0; i + 1 < N; ++i) {                   // sign-XOR coverage (Mul/Div with negative operands)
        FPN<64> a = FPN_FromDouble<64>(IN[i]), b = FPN_FromDouble<64>(IN[i+1]);
        FPN<64> na = FPN_Negate<64>(a), nb = FPN_Negate<64>(b);
        FP2 fa=fp2_from_fpn(a), fb=fp2_from_fpn(b), fna=fp2_neg(fp2_from_fpn(a)), fnb=fp2_neg(fp2_from_fpn(b));
        snprintf(t,sizeof t,"Mul-+[%d]",i); miss += check(t, FPN_Mul<64>(na,b),  fp2_mul(fna,fb)); tested++;
        snprintf(t,sizeof t,"Mul+-[%d]",i); miss += check(t, FPN_Mul<64>(a,nb),  fp2_mul(fa,fnb)); tested++;
        snprintf(t,sizeof t,"Mul--[%d]",i); miss += check(t, FPN_Mul<64>(na,nb), fp2_mul(fna,fnb));tested++;
        snprintf(t,sizeof t,"Div-+[%d]",i); miss += check(t, FPN_DivNoAssert<64>(na,b), fp2_div(fna,fb)); tested++;
        snprintf(t,sizeof t,"Div+-[%d]",i); miss += check(t, FPN_DivNoAssert<64>(a,nb), fp2_div(fa,fnb)); tested++;
    }
    // Pow/Atan2 on controlled finite inputs (large base^exp → inf, not a meaningful equivalence probe).
    static const double PB[][2] = {{2.0,3.0},{1.5,2.0},{3.0,0.5},{9.0,0.5},{1.0,1.0},{0.25,2.0}};
    for (int i = 0; i < (int)(sizeof(PB)/sizeof(PB[0])); ++i) {
        FPN<64> a=FPN_FromDouble<64>(PB[i][0]), b=FPN_FromDouble<64>(PB[i][1]);
        FP2 fa=fp2_from_fpn(a), fb=fp2_from_fpn(b);
        snprintf(t,sizeof t,"Pow[%d]",i);   miss += check(t, FPN_Pow<64>(a,b),   fp2_pow(fa,fb));   tested++;
        snprintf(t,sizeof t,"Atan2[%d]",i); miss += check(t, FPN_Atan2<64>(a,b), fp2_atan2(fa,fb)); tested++;
    }
    printf("\nop VALUE-equivalence (16B two's-comp vs 24B sign-mag) — PRODUCTION header fns:\n");
    printf("  %d checks, %d mismatches -> %s\n", tested, miss, miss==0 ? "PASS" : "FAIL");
    printf("  ops proven: Mul Abs Negate AddSat SubSat Sub Min Max Div Sqrt + From/ToDouble Log InvSqrt Tan Pow Atan2 + sign-XOR\n");
    printf("  remaining (FPN-native Taylor transcendentals, per-op port next): Exp Sin Cos\n");
    return miss == 0 ? 0 : 1;
}
