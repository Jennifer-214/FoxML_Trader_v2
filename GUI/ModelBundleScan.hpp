#pragma once
//======================================================================
// [FILE]_[ModelBundleScan.hpp]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[ImGui-FREE model bundle scanner for the Settings picker (E.1.2.C 3G-ii) — walks models/ (2 levels), groups `_horizon_<N>` siblings into ONE selectable ensemble FAMILY each via the loader's OWN matcher (Model_ParseHorizonSibling — never a parallel rule), lists role-bearing dirs as single-zoo entries, and formats the resolution preview (shape / horizons / roles / exit count / primary / SimpleDip warning). ImGui-free on purpose so controller_test pins the grouping against a disk fixture]
//======================================================================

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
// NodeModelZoo rows need the Money/FPN chain first (same include-order
// contract as SettingsSectionIndex.hpp).
#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/NodeModelZoo.hpp"   // Model_ParseHorizonSibling + ENSEMBLE_HORIZON_MAX

// Role bits (order matches MODEL_BUNDLE_ROLE_NAMES).
enum {
    MB_ROLE_BARRIER    = 1,
    MB_ROLE_BUY_SIGNAL = 2,
    MB_ROLE_REGIME     = 4,
    MB_ROLE_EXIT       = 8,
};
static const char* const MODEL_BUNDLE_ROLE_NAMES[4] =
    { "barrier", "buy_signal", "regime", "exit" };

//======================================================================
// [STRUCT]_[ModelBundleEntry]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one selectable picker row — a `_horizon_` sibling FAMILY (selection writes the BASE path; Shape A) or a single role-bearing dir (Shape B); horizons sorted ascending, per-horizon role bits, exit count]
//======================================================================
// [CODE]
//======================================================================
struct ModelBundleEntry {
    int     is_family;                        // 1 = _horizon_ family; 0 = single-zoo dir
    int     horizon_count;                    // family: sibling count; single: 0
    int     horizons[ENSEMBLE_HORIZON_MAX];   // sorted ascending
    uint8_t roles[ENSEMBLE_HORIZON_MAX];      // MB_ROLE_* bits; single uses [0]
    int     exit_count;                       // horizons carrying MB_ROLE_EXIT
    int     truncated_horizons;               // family had > ENSEMBLE_HORIZON_MAX siblings
    char    label[112];                       // combo display line
    char    cfg_path[256];                    // what selection writes into node_N_model_dir
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-20]
// [SIZE]_[424B]
// [ALIGN]_[4]
// [CACHE_LINES]_[7]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[ModelBundleEntry]
//======================================================================

//======================================================================
// [STRUCT]_[ModelBundleScanState]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the scan result — fixed-capacity entry list (H1: no allocation), label-sorted for a deterministic picker regardless of readdir order; truncated flag is SURFACED, never silent]
//======================================================================
// [CODE]
//======================================================================
struct ModelBundleScanState {
    static constexpr int MAX_ENTRIES = 48;
    int count;
    int truncated;                            // entry cap hit (surfaced in UI)
    ModelBundleEntry entries[MAX_ENTRIES];
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-20]
// [SIZE]_[20360B]
// [ALIGN]_[4]
// [CACHE_LINES]_[319]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[ModelBundleScanState]
//======================================================================

//======================================================================
// [FUNCTION]_[ModelBundle_ScanRoles]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[role-presence bits for one dir — probes <dir>/<role>.{json,xgb,bin} by stat, the same filename set the loaders walk]
//======================================================================
// [CODE]
//======================================================================
static inline uint8_t ModelBundle_ScanRoles(const char* dir_path) {
    static const char* const exts[3] = { "json", "xgb", "bin" };
    uint8_t bits = 0;
    for (int r = 0; r < 4; ++r) {
        for (int e = 0; e < 3; ++e) {
            char full[420];
            snprintf(full, sizeof(full), "%s/%s.%s",
                     dir_path, MODEL_BUNDLE_ROLE_NAMES[r], exts[e]);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
                bits |= (uint8_t)(1u << r);
                break;
            }
        }
    }
    return bits;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ModelBundle_ScanRoles]
//======================================================================

//======================================================================
// [FUNCTION]_[ModelBundle_ScanParent]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[scan ONE parent dir: entries whose name splits at the LAST `_horizon_` and validates through the loader's Model_ParseHorizonSibling fold into families; role-bearing dirs list as singles; role-less depth-0 dirs (models/classification etc.) recurse ONE level. Families then get per-horizon role probes + sorted horizons + a summary label]
//======================================================================
// [CODE]
//======================================================================
static inline void ModelBundle_ScanParent(ModelBundleScanState* st,
                                          const char* parent, int depth) {
    DIR* dir = opendir(parent);
    if (!dir) return;

    static constexpr int MAX_FAM = 32;
    char fam_base[MAX_FAM][96];
    int  fam_h[MAX_FAM][ENSEMBLE_HORIZON_MAX];
    int  fam_hcount[MAX_FAM];
    int  fam_trunc[MAX_FAM];
    int  nfam = 0;

    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        char dir_path[360];
        snprintf(dir_path, sizeof(dir_path), "%s/%s", parent, de->d_name);
        struct stat stt;
        if (stat(dir_path, &stt) != 0 || !S_ISDIR(stt.st_mode)) continue;

        // Sibling test — split at the LAST `_horizon_` occurrence, then
        // validate the suffix through the LOADER'S matcher so picker
        // grouping and boot auto-detect can never disagree on a name.
        const char* last = nullptr;
        for (const char* p = de->d_name; (p = strstr(p, "_horizon_")) != nullptr; ++p)
            last = p;
        long h = -1;
        if (last && last != de->d_name) {
            const int prefix_len = (int)(last - de->d_name) + 9; // + "_horizon_"
            h = Model_ParseHorizonSibling(de->d_name, de->d_name, prefix_len);
        }
        if (h > 0) {
            const int base_len = (int)(last - de->d_name);
            int f = -1;
            for (int i = 0; i < nfam; ++i)
                if ((int)strlen(fam_base[i]) == base_len &&
                    strncmp(fam_base[i], de->d_name, (size_t)base_len) == 0) { f = i; break; }
            if (f < 0) {
                if (nfam >= MAX_FAM) { st->truncated = 1; continue; }
                f = nfam++;
                int n = base_len < (int)sizeof(fam_base[0]) - 1
                      ? base_len : (int)sizeof(fam_base[0]) - 1;
                memcpy(fam_base[f], de->d_name, (size_t)n);
                fam_base[f][n] = '\0';
                fam_hcount[f] = 0;
                fam_trunc[f] = 0;
            }
            if (fam_hcount[f] >= ENSEMBLE_HORIZON_MAX) { fam_trunc[f] = 1; continue; }
            fam_h[f][fam_hcount[f]++] = (int)h;
            continue;
        }

        // Not a sibling: role-bearing dir = single-zoo entry.
        uint8_t roles = ModelBundle_ScanRoles(dir_path);
        if (roles) {
            if (st->count >= ModelBundleScanState::MAX_ENTRIES) { st->truncated = 1; continue; }
            ModelBundleEntry* e = &st->entries[st->count++];
            memset(e, 0, sizeof(*e));
            e->is_family = 0;
            e->roles[0] = roles;
            e->exit_count = (roles & MB_ROLE_EXIT) ? 1 : 0;
            snprintf(e->cfg_path, sizeof(e->cfg_path), "%s", dir_path);
            const char* rel = (strncmp(dir_path, "models/", 7) == 0)
                            ? dir_path + 7 : dir_path;
            snprintf(e->label, sizeof(e->label), "%s  [single%s]",
                     rel, (roles & MB_ROLE_EXIT) ? " · exit" : "");
            continue;
        }

        // Role-less dir at depth 0 (models/classification, models/regression,
        // any operator grouping) — recurse ONE level.
        if (depth == 0) ModelBundle_ScanParent(st, dir_path, 1);
    }
    closedir(dir);

    // Materialize family entries: sorted horizons + per-horizon role probes.
    for (int f = 0; f < nfam; ++f) {
        if (st->count >= ModelBundleScanState::MAX_ENTRIES) { st->truncated = 1; break; }
        ModelBundleEntry* e = &st->entries[st->count++];
        memset(e, 0, sizeof(*e));
        e->is_family = 1;
        e->truncated_horizons = fam_trunc[f];
        // insertion-sort ascending (deterministic dispatch order, like the loader)
        for (int i = 0; i < fam_hcount[f]; ++i) {
            int v = fam_h[f][i], j = i;
            while (j > 0 && fam_h[f][j - 1] > v) { fam_h[f][j] = fam_h[f][j - 1]; --j; }
            fam_h[f][j] = v;
        }
        e->horizon_count = fam_hcount[f];
        int any_buy = 0;
        for (int i = 0; i < fam_hcount[f]; ++i) {
            e->horizons[i] = fam_h[f][i];
            char hdir[420];
            snprintf(hdir, sizeof(hdir), "%s/%s_horizon_%d",
                     parent, fam_base[f], fam_h[f][i]);
            e->roles[i] = ModelBundle_ScanRoles(hdir);
            if (e->roles[i] & (MB_ROLE_BARRIER | MB_ROLE_BUY_SIGNAL | MB_ROLE_REGIME))
                any_buy = 1;
            if (e->roles[i] & MB_ROLE_EXIT) e->exit_count++;
        }
        snprintf(e->cfg_path, sizeof(e->cfg_path), "%s/%s", parent, fam_base[f]);
        const char* rel = (strncmp(e->cfg_path, "models/", 7) == 0)
                        ? e->cfg_path + 7 : e->cfg_path;
        const char* rsum = (any_buy && e->exit_count) ? "buy+exit"
                         : (any_buy ? "buy" : (e->exit_count ? "exit" : "none"));
        snprintf(e->label, sizeof(e->label), "%s  [ensemble · %dh · %s]",
                 rel, e->horizon_count, rsum);
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ModelBundle_ScanParent]
//======================================================================

//======================================================================
// [FUNCTION]_[ModelBundleScan_Run]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[full scan under models/ (depth 0 + one recurse level) then label-sort the entries — readdir order is filesystem-dependent and a picker must be deterministic]
//======================================================================
// [CODE]
//======================================================================
static inline void ModelBundleScan_Run(ModelBundleScanState* st,
                                       const char* root = "models") {
    st->count = 0;
    st->truncated = 0;
    ModelBundle_ScanParent(st, root, 0);
    // insertion-sort by label (N <= 48; entries are POD, memcpy-swappable)
    for (int i = 1; i < st->count; ++i) {
        ModelBundleEntry tmp;
        memcpy(&tmp, &st->entries[i], sizeof(tmp));
        int j = i;
        while (j > 0 && strcmp(st->entries[j - 1].label, tmp.label) > 0) {
            memcpy(&st->entries[j], &st->entries[j - 1], sizeof(tmp));
            --j;
        }
        memcpy(&st->entries[j], &tmp, sizeof(tmp));
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ModelBundleScan_Run]
//======================================================================

//======================================================================
// [FUNCTION]_[ModelBundle_FormatPreview]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the resolution preview — what the LOADER would resolve for this entry: shape, per-horizon roles, exit-predictor count, primary role (the EnsurePrimary priority: buy_signal > barrier > regime; exit never primary), and the operator warnings (nothing-loadable SimpleDip fallback / buy-side-empty / partial exit coverage / horizon-cap truncation / Shape-B skips-HMAC note)]
//======================================================================
// [CODE]
//======================================================================
static inline void ModelBundle_FormatPreview(const ModelBundleEntry* e,
                                             char* out, size_t out_sz) {
    size_t n = 0;
    #define MBP(...) do { if (n < out_sz) \
        n += (size_t)snprintf(out + n, out_sz - n, __VA_ARGS__); } while (0)

    uint8_t all = 0;
    const int nh = e->is_family ? e->horizon_count : 1;
    for (int i = 0; i < nh; ++i) all |= e->roles[i];
    // EnsurePrimary priority mirror: buy_signal > barrier > regime.
    const char* primary = (all & MB_ROLE_BUY_SIGNAL) ? "buy_signal"
                        : (all & MB_ROLE_BARRIER)    ? "barrier"
                        : (all & MB_ROLE_REGIME)     ? "regime" : nullptr;

    if (e->is_family) {
        MBP("ensemble (Shape A) · %d horizon%s · %d exit predictor%s",
            e->horizon_count, e->horizon_count == 1 ? "" : "s",
            e->exit_count, e->exit_count == 1 ? "" : "s");
        if (primary) MBP(" · primary=%s", primary);
        MBP("\n");
        for (int i = 0; i < e->horizon_count; ++i) {
            MBP("  %d:", e->horizons[i]);
            int first = 1;
            for (int r = 0; r < 4; ++r)
                if (e->roles[i] & (1u << r)) {
                    MBP("%s%s", first ? " " : " + ", MODEL_BUNDLE_ROLE_NAMES[r]);
                    first = 0;
                }
            if (first) MBP(" (no role files)");
            MBP("\n");
        }
        if (e->truncated_horizons)
            MBP("WARN: more than %d sibling dirs — the loader caps at %d\n",
                ENSEMBLE_HORIZON_MAX, ENSEMBLE_HORIZON_MAX);
    } else {
        MBP("single-zoo (Shape B) · roles:");
        int first = 1;
        for (int r = 0; r < 4; ++r)
            if (e->roles[0] & (1u << r)) {
                MBP("%s%s", first ? " " : " + ", MODEL_BUNDLE_ROLE_NAMES[r]);
                first = 0;
            }
        MBP("\n");
        MBP("note: no cross-horizon ensemble/bandit on this shape; boot-time\n"
            "single-zoo loads skip HMAC verify (known #7). A base dir of\n"
            "_horizon_ siblings gives Shape A.\n");
    }

    if (!all) {
        MBP("WARN: NOTHING LOADABLE — an ML node on this dir falls back to SimpleDip.\n");
    } else if (!(all & (MB_ROLE_BARRIER | MB_ROLE_BUY_SIGNAL | MB_ROLE_REGIME))) {
        MBP("WARN: no buy-side roles — entry side has no model; exit-only deployments\n"
            "still need a buy primary to trade.\n");
    }
    if (e->is_family && e->exit_count > 0 && e->exit_count < e->horizon_count)
        MBP("note: exit on %d/%d horizons (bandit arms = %d).\n",
            e->exit_count, e->horizon_count, e->exit_count);
    #undef MBP
    if (out_sz) out[out_sz - 1] = '\0';
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ModelBundle_FormatPreview]
//======================================================================
