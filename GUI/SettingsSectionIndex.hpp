#pragma once
//======================================================================
// [FILE]_[SettingsSectionIndex.hpp]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[ImGui-FREE section-grouping layer for the Settings panel (E.1.2.C 3G-i) — one-time category->field-index layout built over N render sources, curated display order, canonical-name dedupe; kills the duplicate-CollapsingHeader class (FOREACH_CFG_FIELD rows are APPEND-ONLY / wire-load-bearing, so interleaved sections in row order are STRUCTURAL and only an index layer can group them); ImGui-free on purpose so controller_test pins the real builder + the real registry wiring]
//======================================================================

#include <string.h>
#include <stdio.h>
// CfgFieldRegistry rows name Money as a STORAGE_T token, so the registry
// header is include-order-dependent — pull the Money/FPN chain first to
// keep THIS header self-contained for any include site.
#include "../FixedPoint/FixedPointN.hpp"
#include "../CoreFrameworks/CfgFieldRegistry.hpp"

//======================================================================
// [FUNCTION]_[Settings_CanonicalSection]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the display-string vocabulary pass — maps ambiguous section aliases onto ONE canonical display name so near-duplicate headers merge; identity for everything else. Alias table is the SSoT: add a pair here when a new near-duplicate appears in a registry row]
//======================================================================
// [CODE]
//======================================================================
static inline const char* Settings_CanonicalSection(const char* s) {
    // "Time-Based Exit" (per-node registry) vs "Time Exit" (global registry
    // + per-node hand table) — one concept, one header.
    if (strcmp(s, "Time-Based Exit") == 0)        return "Time Exit";
    // "Operational Monitoring" (field_defs) vs "Operational" (global
    // registry) — both operator-ops knobs; merged under the shorter name.
    if (strcmp(s, "Operational Monitoring") == 0) return "Operational";
    return s;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Settings_CanonicalSection]
//======================================================================

// Curated display order for the GLOBAL tab's merged section list. NOT
// load-bearing for correctness: a section missing here (e.g. a future
// registry row's brand-new section) appends AFTER the curated ones in
// first-seen order — the curated list never needs to be complete, it
// only pins the reading order of what the operator sees most.
static const char* const SETTINGS_GLOBAL_SECTION_ORDER[] = {
    "Trading", "Entry Filters", "EMA Gate", "Session Filters",
    "Regime Detection", "Risk Management", "Kill Switch", "Danger Gradient",
    "Gate Recovery", "Time Exit", "Partial Exits",
    "Strategies", "Momentum Tuning",
    "ML", "ML Hyperparams", "ML/Ridge", "ML/Winsor", "ML/Confidence",
    "ML/Thompson", "ML/Bandit", "ML/Exit", "Ensemble", "FoxML",
    "Models", "Barrier", "Training", "Validation", "Drift Acknowledgments",
    "Tick Recording", "Notifications", "Health Logging", "Reconcile",
    "Lifecycle", "Engine Timing", "Performance", "Slow Path",
    "Operational", "Toggles", "Per-Node",
};
static constexpr int SETTINGS_GLOBAL_SECTION_ORDER_COUNT =
    (int)(sizeof(SETTINGS_GLOBAL_SECTION_ORDER) / sizeof(SETTINGS_GLOBAL_SECTION_ORDER[0]));

// Curated order for the per-node tab (pins today's hand-table order so a
// future appended per_node_fields[] row cannot reshuffle the tab).
static const char* const SETTINGS_PER_NODE_SECTION_ORDER[] = {
    "Trading", "Entry Filters", "Strategy-Specific", "Adaptation",
    "Trailing", "Time Exit", "Vol Sizing", "No-Trade Band",
    "Partial Exits", "ML",
};
static constexpr int SETTINGS_PER_NODE_SECTION_ORDER_COUNT =
    (int)(sizeof(SETTINGS_PER_NODE_SECTION_ORDER) / sizeof(SETTINGS_PER_NODE_SECTION_ORDER[0]));

//======================================================================
// [STRUCT]_[SectionLayout]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one-time grouped-render layout: canonical section names in final display order + per-source permutation spans (perm[] holds original row indices grouped by section, original order preserved within a section). Built once at panel init on the GUI thread, read every frame by the GUI thread — cold, single-thread, fixed arrays (H1: no allocation)]
//======================================================================
// [CODE]
//======================================================================
struct SectionLayout {
    static constexpr int MAX_SECTIONS = 64;   // ~40 distinct at HEAD; loud on overflow
    static constexpr int MAX_SOURCES  = 3;    // Global tab: field_defs + 2 registries
    static constexpr int MAX_ROWS     = 160;  // >= max rows of any one source (88 at HEAD)

    int n_sections;
    int built;
    const char* names[MAX_SECTIONS];                 // canonical, final display order
    int span_start[MAX_SOURCES][MAX_SECTIONS];       // into perm[source]
    int span_count[MAX_SOURCES][MAX_SECTIONS];       // 0 = section absent in source
    int perm[MAX_SOURCES][MAX_ROWS];                 // original row indices, grouped
};

// One render source: row_count rows; sec_of(row, ctx) returns the RAW
// section string for a row, or NULL to exclude the row (e.g. render-mask
// cleared). Function pointer + ctx — no std::function (H1).
typedef const char* (*SectionOfFn)(int row, const void* ctx);
struct SectionSource {
    int row_count;
    SectionOfFn sec_of;
    const void* ctx;
};
//======================================================================
// [END_CODE]
//======================================================================
// [END_STRUCT]_[SectionLayout]
//======================================================================

//======================================================================
// [FUNCTION]_[SectionLayout_Build]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[build the grouped layout: pass 1 collects canonical section names first-seen across sources; pass 2 orders them curated-first (curated order restricted to present members, then the rest first-seen); pass 3 fills per-source perms grouped by section with original row order preserved. O(rows x sections) strcmp at cold init, once. Returns n_sections, or -1 on cap overflow (loud stderr; render falls back to whatever fit — never silent)]
//======================================================================
// [CODE]
//======================================================================
static inline int SectionLayout_Build(SectionLayout* L,
                                      const SectionSource* sources, int n_sources,
                                      const char* const* curated, int curated_count) {
    L->n_sections = 0;
    L->built = 0;
    if (n_sources > SectionLayout::MAX_SOURCES) {
        fprintf(stderr, "[settings] SectionLayout_Build: %d sources > cap %d\n",
                n_sources, SectionLayout::MAX_SOURCES);
        return -1;
    }

    // Pass 1 — distinct canonical names, first-seen across sources in order.
    const char* seen[SectionLayout::MAX_SECTIONS];
    int n_seen = 0;
    int overflow = 0;
    for (int s = 0; s < n_sources; ++s) {
        if (sources[s].row_count > SectionLayout::MAX_ROWS) {
            fprintf(stderr, "[settings] SectionLayout_Build: source %d has %d rows > cap %d\n",
                    s, sources[s].row_count, SectionLayout::MAX_ROWS);
            return -1;
        }
        for (int r = 0; r < sources[s].row_count; ++r) {
            const char* raw = sources[s].sec_of(r, sources[s].ctx);
            if (!raw) continue;
            const char* c = Settings_CanonicalSection(raw);
            int found = 0;
            for (int k = 0; k < n_seen; ++k)
                if (strcmp(seen[k], c) == 0) { found = 1; break; }
            if (found) continue;
            if (n_seen >= SectionLayout::MAX_SECTIONS) { overflow = 1; continue; }
            seen[n_seen++] = c;
        }
    }
    if (overflow)
        fprintf(stderr, "[settings] SectionLayout_Build: section cap %d hit — "
                        "extra sections DROPPED from render (raise MAX_SECTIONS)\n",
                SectionLayout::MAX_SECTIONS);

    // Pass 2 — final order: curated members that exist, then the rest.
    int used[SectionLayout::MAX_SECTIONS] = {0};
    for (int c = 0; c < curated_count; ++c) {
        for (int k = 0; k < n_seen; ++k) {
            if (!used[k] && strcmp(seen[k], curated[c]) == 0) {
                L->names[L->n_sections++] = seen[k];
                used[k] = 1;
                break;
            }
        }
    }
    for (int k = 0; k < n_seen; ++k)
        if (!used[k]) L->names[L->n_sections++] = seen[k];

    // Pass 3 — per-source perms grouped by final section order.
    for (int s = 0; s < n_sources; ++s) {
        int cursor = 0;
        for (int k = 0; k < L->n_sections; ++k) {
            L->span_start[s][k] = cursor;
            for (int r = 0; r < sources[s].row_count; ++r) {
                const char* raw = sources[s].sec_of(r, sources[s].ctx);
                if (!raw) continue;
                if (strcmp(Settings_CanonicalSection(raw), L->names[k]) != 0) continue;
                L->perm[s][cursor++] = r;
            }
            L->span_count[s][k] = cursor - L->span_start[s][k];
        }
        // Zero the spans of sources beyond this one's section reach so a
        // renderer can loop [0, n_sections) uniformly for every source.
        for (int k = L->n_sections; k < SectionLayout::MAX_SECTIONS; ++k) {
            L->span_start[s][k] = 0;
            L->span_count[s][k] = 0;
        }
    }
    L->built = 1;
    return overflow ? -1 : L->n_sections;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SectionLayout_Build]
//======================================================================

//======================================================================
// [FUNCTION]_[cfg_field_mask_test]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-bit probe of a CfgMaskArray words[] view — the random-access sibling of CFG_FIELD_FOR_EACH_SET_BIT (which only iterates); used to bake the constexpr render masks into the one-time layout]
//======================================================================
// [CODE]
//======================================================================
static inline int cfg_field_mask_test(const uint64_t* words, int idx) {
    return (int)((words[(size_t)idx / 64] >> ((size_t)idx % 64)) & 1ULL);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[cfg_field_mask_test]
//======================================================================

//======================================================================
// [FUNCTION]_[SettingsSection_GlobalRegistrySectionOf]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[SectionOfFn adapter for the FOREACH_GLOBAL_CFG_FIELD descriptor rows — returns the row's section, or NULL for rows the composed render mask excludes (bakes g_global_cfg_render_mask into the layout; the mask is inline constexpr, so bake-at-init is exact)]
//======================================================================
// [CODE]
//======================================================================
static inline const char* SettingsSection_GlobalRegistrySectionOf(int row, const void*) {
    if (!cfg_field_mask_test(g_global_cfg_render_mask.words, row)) return NULL;
    return g_global_cfg_field_descriptors[row].section;
}

static inline const char* SettingsSection_PerNodeRegistrySectionOf(int row, const void*) {
    if (!cfg_field_mask_test(g_per_node_cfg_render_mask.words, row)) return NULL;
    return g_per_node_cfg_field_descriptors[row].section;
}

static_assert(FIELD_IDX_GLOBAL_END <= SectionLayout::MAX_ROWS,
              "SectionLayout::MAX_ROWS must cover the global cfg registry");
static_assert(FIELD_IDX_PER_NODE_END <= SectionLayout::MAX_ROWS,
              "SectionLayout::MAX_ROWS must cover the per-node cfg registry");
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SettingsSection_GlobalRegistrySectionOf]
//======================================================================
