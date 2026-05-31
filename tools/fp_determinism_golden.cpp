// .E.0.1 FP-DETERMINISM GOLDEN harness (promoted from the F-056 seed diagnostic).
//
// Exercises the FPN<64> op set over a FIXED input vector and emits a byte-exact hex
// dump of every result. Built under the SHIPPED config (USE_NATIVE_128, -O3,
// -march=native), its stdout is frozen as tools/fp_determinism_golden.txt — the
// locked BINARY-EPOCH golden (D-100 / D-101: "locks the golden = THE NET").
//
// The gate (tools/check_fp_determinism.sh) re-runs this three ways and diffs each
// against the frozen golden:
//   1. native -O3      → cross-run drift (and future code drift, e.g. #11's
//                        FixedPoint<2,64> binary instantiation MUST reproduce it)
//   2. native -O0      → cross-binary / opt-level determinism (H10 cross-binary)
//   3. generic (no native) → the sqrt-scoped ±USE_NATIVE_128 diagnostic that
//                        revealed F-056; post-F-056 at F=64 native==generic for the
//                        WHOLE set (R1 refuted: FromDouble/ToDouble do not diverge at
//                        F=64), so this must now MATCH the golden (the RED→GREEN flip).
// Any non-match = red build. The input vector is part of the golden's identity:
// changing it is a DELIBERATE epoch transition (regenerate the golden), never silent.
#include "FixedPoint/FixedPointN.hpp"
#include <cstdio>
#include <cstring>
#include <cstddef>
using namespace std;

static void emit(const char* op, FPN<64> r) {
    unsigned char b[sizeof(FPN<64>)];
    memcpy(b, &r, sizeof(r));
    printf("%-16s", op);
    for (size_t i = 0; i < sizeof(FPN<64>); ++i) printf("%02x", b[i]);
    printf("\n");
}

int main() {
    // Fixed input vector — representative magnitudes + fractions; all non-zero so
    // DivNoAssert over adjacent pairs is well-defined. DO NOT edit without
    // regenerating the golden (a deliberate D-100 epoch transition).
    static const double IN[] = {
        2.0, 3.0, 1.5, 0.25, 100.0, 12345.678, 2.0000001, 0.0001, 9999999.0,
        0.5, 1.0, 7.0, 0.1, 0.3, 1000000.0, 0.000001, 42.42, 3.14159265358979
    };
    const int N = (int)(sizeof(IN) / sizeof(IN[0]));
    char tag[24];

    // unary ops over each input (FromDouble seeds; Sqrt is now the generic NR)
    for (int i = 0; i < N; ++i) {
        FPN<64> x = FPN_FromDouble<64>(IN[i]);
        snprintf(tag, sizeof tag, "FromDouble[%d]", i); emit(tag, x);
        snprintf(tag, sizeof tag, "Sqrt[%d]", i);       emit(tag, FPN_Sqrt<64>(x));
        snprintf(tag, sizeof tag, "Abs[%d]", i);        emit(tag, FPN_Abs<64>(x));
        snprintf(tag, sizeof tag, "Negate[%d]", i);     emit(tag, FPN_Negate<64>(x));
    }
    // binary ops over adjacent pairs (the native-specialized accounting ops)
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
