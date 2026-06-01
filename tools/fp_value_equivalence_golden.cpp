// Ship-A VALUE-EQUIVALENCE NET — the value-canonical sibling of fp_determinism_golden.cpp.
//
// fp_determinism_golden.cpp emits the RAW STRUCT BYTES of the FPN<64> op-vector (layout-
// DEPENDENT, 24B). Under the #11 24B-sign-magnitude → 16B-two's-complement compaction the
// bytes change BY DESIGN, so that byte-golden cannot survive (D-139 supersedes its
// "FixedPoint<2,64> MUST reproduce byte-for-byte" comment).
//
// This harness emits the same op-vector as a LAYOUT-INDEPENDENT value: `sign + 128-bit
// magnitude` (w[1]:w[0]). The 24B sign-magnitude FPN<64> and the future 16B two's-complement
// FixedPoint<2,64> produce IDENTICAL output here IFF they represent the same VALUES — that is
// the D-139 value-equivalence criterion (NOT byte-identity). Freeze this NOW under the current
// FPN<64> as tools/fp_value_equivalence_golden.txt (the value-baseline); Ship A's 16B core must
// reproduce it. That reproduction is the P1 STOP-before-money gate (D-130/D-139).
//
// Scope (R2): value-equivalence holds for |value| < 2^63 (the 16B two's-complement integer
// range); the input vector below stays well inside it (feature-domain magnitudes). A value
// using bit 127 would have no 16B image — by design, not a regression.
#include "FixedPoint/FixedPointN.hpp"
#include <cstdio>
#include <cstddef>
using namespace std;

// LAYOUT-INDEPENDENT value emit: sign char, then the 128-bit magnitude (high word : low word).
// FPN<64> stores w[0]=low(frac), w[1]=high(int) + a separate sign flag; the future
// FixedPoint<2,64> decodes its __int128 to the SAME sign+magnitude form. Equal line == equal value.
static void emit(const char* op, FPN<64> r) {
    printf("%-16s%c %016llx%016llx\n", op,
           r.sign ? '-' : '+',
           (unsigned long long)r.w[1], (unsigned long long)r.w[0]);
}

int main() {
    // SAME fixed input vector as fp_determinism_golden.cpp (the two goldens share an identity;
    // editing it is a deliberate D-100 epoch transition, never silent).
    static const double IN[] = {
        2.0, 3.0, 1.5, 0.25, 100.0, 12345.678, 2.0000001, 0.0001, 9999999.0,
        0.5, 1.0, 7.0, 0.1, 0.3, 1000000.0, 0.000001, 42.42, 3.14159265358979
    };
    const int N = (int)(sizeof(IN) / sizeof(IN[0]));
    char tag[24];

    for (int i = 0; i < N; ++i) {
        FPN<64> x = FPN_FromDouble<64>(IN[i]);
        snprintf(tag, sizeof tag, "FromDouble[%d]", i); emit(tag, x);
        snprintf(tag, sizeof tag, "Sqrt[%d]", i);       emit(tag, FPN_Sqrt<64>(x));
        snprintf(tag, sizeof tag, "Abs[%d]", i);        emit(tag, FPN_Abs<64>(x));
        snprintf(tag, sizeof tag, "Negate[%d]", i);     emit(tag, FPN_Negate<64>(x));
    }
    for (int i = 0; i + 1 < N; ++i) {
        FPN<64> a = FPN_FromDouble<64>(IN[i]);
        FPN<64> b = FPN_FromDouble<64>(IN[i + 1]);
        snprintf(tag, sizeof tag, "AddSat[%d]", i); emit(tag, FPN_AddSat<64>(a, b));
        snprintf(tag, sizeof tag, "SubSat[%d]", i); emit(tag, FPN_SubSat<64>(a, b));
        snprintf(tag, sizeof tag, "Sub[%d]", i);    emit(tag, FPN_Sub<64>(a, b));
        snprintf(tag, sizeof tag, "Mul[%d]", i);    emit(tag, FPN_Mul<64>(a, b));
        snprintf(tag, sizeof tag, "Div[%d]", i);    emit(tag, FPN_DivNoAssert<64>(a, b));
        snprintf(tag, sizeof tag, "Min[%d]", i);    emit(tag, FPN_Min<64>(a, b));
        snprintf(tag, sizeof tag, "Max[%d]", i);    emit(tag, FPN_Max<64>(a, b));
    }
    return 0;
}
