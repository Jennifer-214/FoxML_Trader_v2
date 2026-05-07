// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).

//======================================================================================================
// [tools/compare_scalers.cpp]
//
// v5.11.13 (2026-05-07) — standalone CLI that diffs two FeatureStandardizer
// sidecar binaries (.scaler files) per-feature.
//
// Usage:
//   compare_scalers <a.scaler> <b.scaler> [--threshold=PCT]
//
// Reports per-feature mean/stddev delta, flags features whose
// |stddev_b - stddev_a| / stddev_a exceeds --threshold (default 50%)
// as potential regime shift. Exit code:
//   0 — comparison succeeded, no flags above threshold
//   1 — comparison succeeded, ≥1 feature flagged as regime shift
//   2 — load failure on either sidecar (file missing, magic mismatch,
//       num_features mismatch, SHA tamper)
//   3 — argument error
//
// Notes:
//   - Both sidecars must share registry_hash. Mismatch → exit 2 with a
//     clear error (it would be meaningless to compare features with
//     different registry layouts).
//   - Per v5.9.3a binding, num_features is also identical when registry
//     hashes match, but check explicitly anyway (defense in depth).
//======================================================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "../ML_Headers/FeatureStandardizer.hpp"
#include "../ML_Headers/FeatureRegistry.hpp"

// FeatureStandardizer + FeatureStandardizer_Load live in namespace tt
// (per ML_Headers/FeatureStandardizer.hpp:57). FEATURE_NAMES is global
// (per ML_Headers/FeatureRegistry.hpp:339, before the tt block opens at
// 373). Pull the standardizer in via using-decl so the body below reads
// without prefix noise; the global FEATURE_NAMES needs no qualification.
using tt::FeatureStandardizer;
using tt::FeatureStandardizer_Load;

static int print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s <a.scaler> <b.scaler> [--threshold=PCT]\n"
        "\n"
        "Compares two FeatureStandardizer sidecar binaries per-feature.\n"
        "Reports |Δmean|, |Δstddev|, and percent change. Flags any\n"
        "feature whose |Δstddev| / stddev_a exceeds --threshold (PCT,\n"
        "default 50.0).\n",
        prog);
    return 3;
}

int main(int argc, char** argv) {
    if (argc < 3) return print_usage(argv[0]);

    const char* path_a = argv[1];
    const char* path_b = argv[2];
    double threshold_pct = 50.0;
    for (int i = 3; i < argc; ++i) {
        if (std::strncmp(argv[i], "--threshold=", 12) == 0) {
            threshold_pct = std::atof(argv[i] + 12);
            if (threshold_pct < 0.0 || threshold_pct > 100000.0) {
                std::fprintf(stderr, "compare_scalers: invalid --threshold=%s\n",
                             argv[i] + 12);
                return 3;
            }
        } else {
            std::fprintf(stderr, "compare_scalers: unknown arg '%s'\n", argv[i]);
            return print_usage(argv[0]);
        }
    }

    FeatureStandardizer sc_a;
    FeatureStandardizer sc_b;
    int load_a = FeatureStandardizer_Load(&sc_a, path_a);
    int load_b = FeatureStandardizer_Load(&sc_b, path_b);
    if (load_a <= 0) {
        std::fprintf(stderr, "compare_scalers: failed to load %s (rc=%d)\n",
                     path_a, load_a);
        return 2;
    }
    if (load_b <= 0) {
        std::fprintf(stderr, "compare_scalers: failed to load %s (rc=%d)\n",
                     path_b, load_b);
        return 2;
    }
    if (sc_a.registry_hash != sc_b.registry_hash) {
        std::fprintf(stderr,
            "compare_scalers: registry_hash mismatch\n"
            "  %s: 0x%016llx\n"
            "  %s: 0x%016llx\n"
            "Comparison would be meaningless across different feature registries.\n",
            path_a, (unsigned long long)sc_a.registry_hash,
            path_b, (unsigned long long)sc_b.registry_hash);
        return 2;
    }
    if (sc_a.num_features != sc_b.num_features) {
        std::fprintf(stderr,
            "compare_scalers: num_features mismatch (%u vs %u)\n",
            sc_a.num_features, sc_b.num_features);
        return 2;
    }

    std::printf("compare_scalers — %u features (registry_hash 0x%016llx)\n",
                sc_a.num_features,
                (unsigned long long)sc_a.registry_hash);
    std::printf("  threshold for regime-shift flag: %.1f%% on |Δstddev|/stddev_a\n",
                threshold_pct);
    std::printf("\n%-32s %14s %14s %14s %14s %12s %s\n",
                "feature",
                "mean_a", "mean_b", "stddev_a", "stddev_b",
                "Δstddev%",
                "flag");

    int flagged = 0;
    for (uint32_t i = 0; i < sc_a.num_features; ++i) {
        double ma = sc_a.mean[i];
        double mb = sc_b.mean[i];
        double sa = sc_a.stddev[i];
        double sb = sc_b.stddev[i];

        double abs_a = std::fabs(sa);
        double pct_change_stddev = 0.0;
        if (abs_a > sc_a.stddev_floor) {
            pct_change_stddev = ((sb - sa) / sa) * 100.0;
        } else {
            // sa near floor → percent meaningless. Mark as N/A in
            // the column but use absolute |Δstddev| as a fallback for
            // flagging (>1.0 absolute = significant for floor-region).
            pct_change_stddev = std::nan("");
        }

        bool flag = false;
        if (std::isfinite(pct_change_stddev)) {
            flag = std::fabs(pct_change_stddev) > threshold_pct;
        } else {
            flag = std::fabs(sb - sa) > 1.0;
        }
        if (flag) ++flagged;

        std::printf("%-32s %14.6e %14.6e %14.6e %14.6e ",
                    FEATURE_NAMES[i], ma, mb, sa, sb);
        if (std::isfinite(pct_change_stddev)) {
            std::printf("%+11.2f%%", pct_change_stddev);
        } else {
            std::printf("%12s", "N/A");
        }
        std::printf(" %s\n", flag ? "*FLAG*" : "");
    }

    std::printf("\nSummary: %d / %u features flagged above %.1f%% stddev change.\n",
                flagged, sc_a.num_features, threshold_pct);
    return flagged > 0 ? 1 : 0;
}
