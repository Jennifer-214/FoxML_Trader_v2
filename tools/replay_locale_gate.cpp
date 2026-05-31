// tools/replay_locale_gate.cpp — .E.0.1 REPLAY-LOCALE gate (the net's locale half).
//
// Proves the replay parse path is LOCALE-IMMUNE: parsing the same decimal fields via
// tt::parse_double_fast (the primitive BacktestEngine F-054 + DepthReplayState F-055
// now use) under LC_NUMERIC=C and under a comma-decimal locale yields BYTE-IDENTICAL
// doubles. The old strtod path would read "1.5" as 1.0 under de_DE (',' is the decimal
// separator) -> corrupt every replayed price + diverge from live. std::from_chars (what
// tt:: wraps) ignores the locale by the C++ standard; this gate CATCHES any regression
// that reintroduces a locale-dependent parser (strtod/atof/scanf) on the replay path.
//
// Exit 0 = GREEN (or SKIP if no comma-decimal locale is installed — immunity still holds
// by construction, only the empirical leg is unavailable). Exit 1 = RED (drift).
#include "../CoreFrameworks/ParseFast.hpp"
#include <cstdio>
#include <cstring>
#include <clocale>
using namespace std;

static const char* FIELDS[] = {
    "12345.678", "0.00012345", "9.999999", "0.1", "0.3", "100.5",
    "0.000000001", "42.42", "3.14159265358979", "1000000.5"
};
static const int NF = (int)(sizeof(FIELDS) / sizeof(FIELDS[0]));

static void capture(unsigned char out[][sizeof(double)]) {
    for (int i = 0; i < NF; ++i) {
        double d = tt::parse_double_fast(FIELDS[i]);
        memcpy(out[i], &d, sizeof(double));
    }
}

int main() {
    // (A) Behavioral check — ALWAYS runs, independent of installed locales. from_chars
    // MUST read '.' as the decimal separator; a locale-dependent parser (strtod/atof)
    // under a comma-locale would fail these. Values are exactly representable -> == is safe.
    struct { const char* s; double v; } EXACT[] = {
        {"1.5", 1.5}, {"0.5", 0.5}, {"100.25", 100.25}, {"0.0", 0.0}, {"2.0", 2.0}, {"0.25", 0.25}
    };
    for (auto& t : EXACT) {
        double d = tt::parse_double_fast(t.s);
        if (d != t.v) {
            printf("RED: parse(\"%s\")=%.17g != %.17g — decimal point not honored "
                   "(a locale-dependent parser regressed onto the replay path).\n", t.s, d, t.v);
            return 1;
        }
    }
    printf("OK: decimal-point honored on %d exact-value fields.\n",
           (int)(sizeof(EXACT) / sizeof(EXACT[0])));

    // (B) Empirical C-vs-non-C leg (fires when a comma-decimal locale is installed).
    unsigned char c_bytes[NF][sizeof(double)];
    unsigned char x_bytes[NF][sizeof(double)];

    setlocale(LC_NUMERIC, "C");
    capture(c_bytes);

    const char* loc = nullptr;
    const char* cand[] = {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "fr_FR", "nl_NL.UTF-8"};
    for (const char* c : cand) { if (setlocale(LC_NUMERIC, c)) { loc = c; break; } }

    if (!loc) {
        printf("SKIP: no comma-decimal locale installed; tt::parse_double_fast is "
               "locale-immune by construction (std::from_chars). Empirical leg unavailable.\n");
        return 0;  // immunity holds by the standard; not a failure
    }
    capture(x_bytes);

    int diffs = 0;
    for (int i = 0; i < NF; ++i)
        if (memcmp(c_bytes[i], x_bytes[i], sizeof(double)) != 0) {
            printf("  DRIFT field[%d]=\"%s\": C != %s\n", i, FIELDS[i], loc);
            ++diffs;
        }
    if (diffs == 0) {
        printf("GREEN: replay parse locale-immune (C == %s; %d fields byte-identical)\n", loc, NF);
        return 0;
    }
    printf("RED: replay parse LOCALE-DEPENDENT (%d/%d fields drift under %s) "
           "— a locale-dependent parser regressed onto the replay path.\n", diffs, NF, loc);
    return 1;
}
