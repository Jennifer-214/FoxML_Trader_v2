// Ship-A ② VERTICAL SLICE — prove the 16B two's-complement FixedPoint<2,64>::Mul is
// VALUE-equivalent to the current 24B sign-magnitude FPN<64>::Mul, reusing FP64_Mul's
// certified unsigned-product reduce (the C1 HOIST). Standalone proof — touches NO production
// header. If green over the value-net input vector, the #2 multiply pattern (abs-in /
// shared-unsigned-product / reduce / sign-out / saturate-on-overflow) is sound and the bulk
// op-port can follow. (D-125 representation / D-140 the divmul sister / D-139 value-equivalence.)
#include "FixedPoint/FixedPointN.hpp"
#include <cstdio>
#include <cstdint>
using namespace std;

// The 16B core: a bare __int128 two's-complement, value = v / 2^64 (binary <2,64>).
struct FP2_64 { __int128 v; };

// Decode FPN<64> (sign-magnitude: w[1]:w[0] magnitude + sign flag) to the SAME value as a
// two's-complement int128. Valid for |value| < 2^127 (the R2 bound; feature inputs are tiny).
static FP2_64 from_fpn(FPN<64> x) {
    unsigned __int128 mag = ((unsigned __int128)x.w[1] << 64) | (unsigned __int128)x.w[0];
    __int128 v = (__int128)mag;
    return { x.sign ? -v : v };
}

// #2 shared multiply: [extract sign + abs] -> [unsigned 128x128->256 product, take bits[191:64]
// = the >>64 binary reduce, REUSING FP64_Mul's EXACT partials] -> [reapply sign]. With the
// -2^63 abs note (B1) + saturate-on-overflow (the of_mask preservation, R2).
static FP2_64 fp2_mul(FP2_64 a, FP2_64 b) {
    bool neg = (a.v < 0) ^ (b.v < 0);
    // NOTE (B1): abs(INT128_MIN) is UB — the production guard saturates/flags it; the feature
    // input vector never reaches it, so the slice abs's directly.
    unsigned __int128 amag = a.v < 0 ? (unsigned __int128)(-a.v) : (unsigned __int128)a.v;
    unsigned __int128 bmag = b.v < 0 ? (unsigned __int128)(-b.v) : (unsigned __int128)b.v;

    // ---- FP64_Mul's exact reduce (bits[191:64] of amag*bmag) — the C1 hoist ----
    uint64_t a_lo = (uint64_t)amag, a_hi = (uint64_t)(amag >> 64);
    uint64_t b_lo = (uint64_t)bmag, b_hi = (uint64_t)(bmag >> 64);
    unsigned __int128 ll = (unsigned __int128)a_lo * b_lo;
    unsigned __int128 lh = (unsigned __int128)a_lo * b_hi;
    unsigned __int128 hl = (unsigned __int128)a_hi * b_lo;
    unsigned __int128 hh = (unsigned __int128)a_hi * b_hi;
    unsigned __int128 mid     = lh + hl + (ll >> 64);
    unsigned __int128 shifted = hh + (mid >> 64);
    unsigned __int128 mag     = (shifted << 64) | (uint64_t)mid;
    // BRANCHLESS saturate (R2 — same of_mask discipline as FPN_Mul/FP64_Mul; no data-dependent control flow).
    unsigned __int128 ovf  = (shifted >> 64) | (mag >> 127);     // overflow past the 127-bit magnitude range
    unsigned __int128 nz   = ovf | (~ovf + 1);                   // top bit set iff ovf != 0 (no compare → no branch)
    unsigned __int128 of_m = -(unsigned __int128)(int)(nz >> 127);       // 0 / all-ones
    mag = (mag & ~of_m) | (of_m >> 1);                           // saturate to 2^127-1 = (all-ones >> 1), derived from the mask

    __int128 v = (__int128)mag;
    unsigned __int128 neg_m = -(unsigned __int128)((int)neg & (int)(mag != 0));   // canonicalize -0 -> +0
    return { (__int128)(((unsigned __int128)(-v) & neg_m) | ((unsigned __int128)v & ~neg_m)) };
}

// --- radix-agnostic ops in two's-complement (trivial vs sign-magnitude; native +/-/compare).
//     Saturate to the 127-bit magnitude range (R2). For the feature-domain test vector no
//     saturation triggers, so each must match its FPN<64> sibling BY VALUE. ---
static const __int128 FP2_MAX = ((unsigned __int128)1 << 127) - 1;  // +2^127-1
static FP2_64 fp2_neg(FP2_64 a) { __int128 v = -a.v; return { v }; }   // (INT_MIN guard in prod)
static FP2_64 fp2_abs(FP2_64 a) { return { a.v < 0 ? -a.v : a.v }; }
static FP2_64 fp2_addsat(FP2_64 a, FP2_64 b) {
    __int128 s = (__int128)((unsigned __int128)a.v + (unsigned __int128)b.v);     // wrapping add
    // BRANCHLESS: overflow ⇔ a,b same sign AND s differs → (~(a^b) & (a^s)) top bit; saturate by a's sign.
    unsigned __int128 of_m  = -(unsigned __int128)(int)((unsigned __int128)(~(a.v ^ b.v) & (a.v ^ s)) >> 127);
    __int128 sgn = a.v >> 127;                       // arithmetic: 0 (a>=0) / -1 (a<0) — a shift, no compare → no branch
    __int128 sat = (FP2_MAX ^ sgn) - sgn;            // branchless conditional-negate: +MAX / -MAX
    return { (__int128)(((unsigned __int128)s & ~of_m) | ((unsigned __int128)sat & of_m)) };
}
static FP2_64 fp2_sub(FP2_64 a, FP2_64 b) {
    __int128 s = (__int128)((unsigned __int128)a.v - (unsigned __int128)b.v);     // wrapping sub
    // BRANCHLESS: overflow ⇔ a,b differ in sign AND s differs from a → ((a^b) & (a^s)) top bit.
    unsigned __int128 of_m  = -(unsigned __int128)(int)((unsigned __int128)((a.v ^ b.v) & (a.v ^ s)) >> 127);
    __int128 sgn = a.v >> 127;                       // arithmetic: 0 (a>=0) / -1 (a<0) — a shift, no compare → no branch
    __int128 sat = (FP2_MAX ^ sgn) - sgn;            // branchless conditional-negate: +MAX / -MAX
    return { (__int128)(((unsigned __int128)s & ~of_m) | ((unsigned __int128)sat & of_m)) };
}
static FP2_64 fp2_min(FP2_64 a, FP2_64 b) { return { a.v < b.v ? a.v : b.v }; }
static FP2_64 fp2_max(FP2_64 a, FP2_64 b) { return { a.v > b.v ? a.v : b.v }; }

// rebuild a POSITIVE FPN<64> from a 128-bit magnitude (to drive the certified unsigned bodies).
static FPN<64> fp_from_mag(unsigned __int128 m) {
    FPN<64> r{}; r.w[0]=(uint64_t)m; r.w[1]=(uint64_t)(m>>64); r.sign=0; return r;
}
// The COMPLEX binary-specific ops — proven by REUSING the certified unsigned bodies (FPN_DivNoAssert
// long-division / FPN_Sqrt generic-NR) on the abs'd magnitudes + the two's-complement sign-wrap.
// This is the same C1-hoist shape as Mul: the new representation changes only the wrapping, never
// the unsigned core, so value-equivalence reduces to "is the wrap correct" — which these exercise.
// udiv_q64 comes from the included FixedPointN.hpp (the production helper) — the slice tests THAT, not a copy.
static FP2_64 fp2_div(FP2_64 a, FP2_64 b) {
    bool neg = (a.v < 0) ^ (b.v < 0);
    unsigned __int128 am = a.v<0?(unsigned __int128)(-a.v):(unsigned __int128)a.v;
    unsigned __int128 bm = b.v<0?(unsigned __int128)(-b.v):(unsigned __int128)b.v;
    unsigned __int128 qm = udiv_q64(am, bm);                           // certified long-division core (native 16B)
    unsigned __int128 of_m = -(unsigned __int128)(int)(qm >> 127);     // saturate to 2^127-1 (R2), branchless
    qm = (qm & ~of_m) | (of_m >> 1);
    __int128 v = (__int128)qm;
    unsigned __int128 neg_m = -(unsigned __int128)((int)neg & (int)(qm != 0));
    return { (__int128)(((unsigned __int128)(-v) & neg_m) | ((unsigned __int128)v & ~neg_m)) };
}
static FP2_64 fp2_sqrt(FP2_64 a) {                                       // sqrt domain: a > 0 else 0 (matches generic)
    if (a.v <= 0) return { (__int128)0 };
    unsigned __int128 m = (unsigned __int128)a.v;                        // |v| (a > 0 here)
    uint64_t hi = (uint64_t)(m >> 64), lo = (uint64_t)m;                 // top set bit of the magnitude (clz, not w[]-scan)
    int top = hi ? (127 - __builtin_clzll(hi)) : (63 - __builtin_clzll(lo));
    FP2_64 y    { (__int128)((unsigned __int128)1 << ((top + 64) / 2)) };// seed 2^((top_bit+F)/2), F=64
    FP2_64 half { (__int128)((unsigned __int128)1 << 63) };              // 0.5 in Q64.64
    for (int i = 0; i < 12; i++)                                         // 12 Newton-Raphson: y = (y + a/y)/2
        y = fp2_mul(fp2_addsat(y, fp2_div(a, y)), half);
    return y;
}

// Extract (sign, 128-bit magnitude) from each rep for an exact VALUE comparison.
static void val_fpn(FPN<64> r, int& sign, unsigned __int128& mag) {
    mag = ((unsigned __int128)r.w[1] << 64) | (unsigned __int128)r.w[0];
    sign = (r.sign && mag != 0) ? 1 : 0;
}
static void val_fp2(FP2_64 r, int& sign, unsigned __int128& mag) {
    mag = r.v < 0 ? (unsigned __int128)(-r.v) : (unsigned __int128)r.v;
    sign = (r.v < 0 && mag != 0) ? 1 : 0;
}

static int check(const char* tag, FPN<64> ref, FP2_64 got) {
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

    for (int i = 0; i < N; ++i) {                       // unary
        FPN<64> x = FPN_FromDouble<64>(IN[i]); FP2_64 fx = from_fpn(x);
        snprintf(t,sizeof t,"Abs[%d]",i);    miss += check(t, FPN_Abs<64>(x),    fp2_abs(fx)); tested++;
        snprintf(t,sizeof t,"Negate[%d]",i); miss += check(t, FPN_Negate<64>(x), fp2_neg(fx)); tested++;
        snprintf(t,sizeof t,"Sqrt[%d]",i);   miss += check(t, FPN_Sqrt<64>(x),   fp2_sqrt(fx)); tested++;
    }
    for (int i = 0; i + 1 < N; ++i) {                   // binary
        FPN<64> a = FPN_FromDouble<64>(IN[i]), b = FPN_FromDouble<64>(IN[i+1]);
        FP2_64 fa = from_fpn(a), fb = from_fpn(b);
        snprintf(t,sizeof t,"Mul[%d]",i);    miss += check(t, FPN_Mul<64>(a,b),    fp2_mul(fa,fb));    tested++;  // C1 hoist
        snprintf(t,sizeof t,"AddSat[%d]",i); miss += check(t, FPN_AddSat<64>(a,b), fp2_addsat(fa,fb)); tested++;
        snprintf(t,sizeof t,"SubSat[%d]",i); miss += check(t, FPN_SubSat<64>(a,b), fp2_sub(fa,fb));    tested++;
        snprintf(t,sizeof t,"Sub[%d]",i);    miss += check(t, FPN_Sub<64>(a,b),    fp2_sub(fa,fb));    tested++;
        snprintf(t,sizeof t,"Min[%d]",i);    miss += check(t, FPN_Min<64>(a,b),    fp2_min(fa,fb));    tested++;
        snprintf(t,sizeof t,"Max[%d]",i);    miss += check(t, FPN_Max<64>(a,b),    fp2_max(fa,fb));    tested++;
        snprintf(t,sizeof t,"Div[%d]",i);    miss += check(t, FPN_DivNoAssert<64>(a,b), fp2_div(fa,fb)); tested++;
    }
    // SIGN-XOR coverage: the all-positive input vector never exercised Mul/Div sign handling —
    // negate operands so the two's-complement sign-wrap (sign = a<0 ^ b<0) is actually tested.
    for (int i = 0; i + 1 < N; ++i) {
        FPN<64> a = FPN_FromDouble<64>(IN[i]), b = FPN_FromDouble<64>(IN[i+1]);
        FPN<64> na = FPN_Negate<64>(a), nb = FPN_Negate<64>(b);
        FP2_64 fa=from_fpn(a), fb=from_fpn(b), fna=fp2_neg(from_fpn(a)), fnb=fp2_neg(from_fpn(b));
        snprintf(t,sizeof t,"Mul-+[%d]",i); miss += check(t, FPN_Mul<64>(na,b),  fp2_mul(fna,fb)); tested++;
        snprintf(t,sizeof t,"Mul+-[%d]",i); miss += check(t, FPN_Mul<64>(a,nb),  fp2_mul(fa,fnb)); tested++;
        snprintf(t,sizeof t,"Mul--[%d]",i); miss += check(t, FPN_Mul<64>(na,nb), fp2_mul(fna,fnb));tested++;
        snprintf(t,sizeof t,"Div-+[%d]",i); miss += check(t, FPN_DivNoAssert<64>(na,b), fp2_div(fna,fb)); tested++;
        snprintf(t,sizeof t,"Div+-[%d]",i); miss += check(t, FPN_DivNoAssert<64>(a,nb), fp2_div(fa,fnb)); tested++;
    }
    printf("\nop VALUE-equivalence (16B two's-comp vs 24B sign-mag):\n");
    printf("  %d checks, %d mismatches -> %s\n", tested, miss, miss==0 ? "PASS" : "FAIL");
    printf("  ops proven: Mul[C1-hoist] Abs Negate AddSat SubSat Sub Min Max Div Sqrt + sign-XOR(Mul/Div)\n");
    printf("  remaining (feature-only transcendentals, integration-time per-op port): Exp Log InvSqrt Sin Cos Pow\n");
    return miss == 0 ? 0 : 1;
}
