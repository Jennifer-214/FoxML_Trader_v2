// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/LifecycleCfgFlagRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [BITMAP_PACKED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[lifecycle-domain (position-exit) boolean cfg flags — the FIRST of the FOREACH_<DOMAIN>_CFG_FLAG family]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_LIFECYCLE_CFG_FLAG]
//======================================================================================================
#ifndef LIFECYCLE_CFG_FLAG_REGISTRY_HPP
#define LIFECYCLE_CFG_FLAG_REGISTRY_HPP

#include <cstdint>
#include "../MemHeaders/BitmapMacros.hpp"

//======================================================================
// [REGISTRY]_[FOREACH_LIFECYCLE_CFG_FLAG]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [BITMAP_PACKED]]
// [REFERENCE]_[DESIGN_SPEC]_[heterogeneous-registry-pattern]
// [REFERENCE]_[DESIGN_SPEC]_[bitmap-flag-api]
// [REFERENCE]_[DESIGN_SPEC]_[autopopulate-pattern-for-production-caller-class]
// [REFERENCE]_[TECH_DEBT]_[[TECH_DEBT-023] [TECH_DEBT-024]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[position-exit-mechanic boolean cfg flags — one row = enum bit + mask + parser + cfg.example + GUI, all auto-flow]
// [COLUMN]_[NAME]_[UPPERCASE token -> MASK_LIFECYCLE_CFG_<NAME> + LIFECYCLE_CFG_<NAME> enum]   (5-col tuple, v5.14.9.F.5+)
// [COLUMN]_[legacy_field]_[pre-migration cfg field name (AUTOPOPULATE source + parser back-compat); NEW flags use the canonical lowercase name as both]
// [COLUMN]_[display_label]_[GUI checkbox label (operator-facing; e.g. "Partial Exits##toggle")]
// [COLUMN]_[section]_[GUI collapsing-header / section name (e.g. "Toggles", "Kill Switch")]
// [COLUMN]_[doc]_[short description for engine.cfg.example + GUI tooltip + audit trails]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_LIFECYCLE_CFG_FLAG(X)                                                                                                                       \
    X(PARTIAL_EXIT_ENABLED, partial_exit_enabled, "Partial Exits##toggle", "Toggles",       "partial-exit dispatcher arm — leg-A and leg-B size split")     \
    X(BREAKEVEN_ON_PARTIAL, breakeven_on_partial, "Breakeven SL",          "Partial Exits", "move SL to entry after TP1 hit (partial-exit ratchet)")        \
    X(BREAKEVEN_ON_PROFIT,  breakeven_on_profit,  "Breakeven on Profit",   "Partial Exits", "ratchet SL to fee-floored breakeven when position crosses net profit (round-trip taker fees threshold)")

//------------------------------------------------------------------------------------------------------
// [SECTION]_[auto-generated enum + count]
//------------------------------------------------------------------------------------------------------
enum LifecycleCfgFlag {
#define X_GEN_LIFECYCLE_CFG_BIT(name, legacy_field, display_label, section, doc) LIFECYCLE_CFG_##name,
    FOREACH_LIFECYCLE_CFG_FLAG(X_GEN_LIFECYCLE_CFG_BIT)
    LIFECYCLE_CFG_COUNT
#undef X_GEN_LIFECYCLE_CFG_BIT
};

static_assert(LIFECYCLE_CFG_COUNT <= 8,
              "FOREACH_LIFECYCLE_CFG_FLAG exhausted uint8_t storage; expand cfg.lifecycle_cfg_flags to uint16_t");

//------------------------------------------------------------------------------------------------------
// [SECTION]_[auto-generated MASK_LIFECYCLE_CFG_<NAME> constants]
//------------------------------------------------------------------------------------------------------
#define X_GEN_LIFECYCLE_CFG_MASK(name, legacy_field, display_label, section, doc) \
    static constexpr uint8_t MASK_LIFECYCLE_CFG_##name = (uint8_t)(1u << LIFECYCLE_CFG_##name);
FOREACH_LIFECYCLE_CFG_FLAG(X_GEN_LIFECYCLE_CFG_MASK)
#undef X_GEN_LIFECYCLE_CFG_MASK

//------------------------------------------------------------------------------------------------------
// [SECTION]_[autopopulate companion — read legacy cfg fields -> write bitmap]
//------------------------------------------------------------------------------------------------------
// Branchless OR-reduction; compiler emits cmov per row.
// Used by tests + transitional callers (e.g., v5.14.9.F.4 parser refactor will use a generalized
// CFG_FLAG_AUTOPOPULATE_PARSE companion that walks all 5 domain registries).
//
// In v5.14.9.F itself, the parser writes directly to `cfg.lifecycle_cfg_flags` via inline
// strcmp branches (3 entries; manual is fine pre-.F.4 generalization).

#define LIFECYCLE_CFG_FLAG_AUTOPOPULATE_FROM_TRIPLE(target_flags, legacy_partial, legacy_breakeven_partial, legacy_breakeven_profit) \
    do {                                                                                                                              \
        uint8_t _new_flags = 0;                                                                                                       \
        _new_flags |= ((legacy_partial)            ? MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED : (uint8_t)0u);                          \
        _new_flags |= ((legacy_breakeven_partial)  ? MASK_LIFECYCLE_CFG_BREAKEVEN_ON_PARTIAL : (uint8_t)0u);                          \
        _new_flags |= ((legacy_breakeven_profit)   ? MASK_LIFECYCLE_CFG_BREAKEVEN_ON_PROFIT  : (uint8_t)0u);                          \
        (target_flags) = _new_flags;                                                                                                  \
    } while (0)
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]_[family origin + eligibility criteria]
//----------------------------------------------------------------------
// First domain registry in the FOREACH_<DOMAIN>_CFG_FLAG family (v5.14.9.F-.F.3).
// Position-exit-mechanic boolean cfg flags. uint8_t bitmap on ControllerConfig.
//
// Pattern: heterogeneous-registry-pattern.md DOMAIN SPLIT form.
// Closes: TECH_DEBT-013 candidate (5).
// Cross-ref:
//   - DESIGN_SPECS/heterogeneous-registry-pattern.md (decision framework + Y3 dispatch canon)
//   - DESIGN_SPECS/bitmap-flag-api.md (BITMAP_* primitives)
//   - DESIGN_SPECS/autopopulate-pattern-for-production-caller-class.md (AUTOPOPULATE shape)
//   - DOCS/TECH_DEBT.md TECH_DEBT-023 (lat_enabled NOT cfg-flag-eligible — rejected from this domain)
//   - DOCS/TECH_DEBT.md TECH_DEBT-024 (breakeven_on_profit currently dormant — see entry)
//
// CFG-FLAG ELIGIBILITY (per TECH_DEBT-023; codified in heterogeneous-registry-pattern.md):
//   1. Boot-frozen (loaded at startup; not mutated at runtime)
//   2. Engine-wide (not per-core runtime atomic — those use ParameterSlot)
//   3. Hot-path-tolerant (~1-2ns BITMAP_IS_SET acceptable at every read site)
//   4. No compile-time elision benefit (not a template<bool> candidate)
//   5. Cfg-domain-coherent (semantically belongs to LIFECYCLE — position-exit mechanics)
//
// All 3 entries below pass all 5 criteria.
//
// Adding a new entry: append 1 row → enum bit, MASK constant, AUTOPOPULATE bit-set, parser branch,
// engine.cfg.example doc, AND GUI checkbox+section+tooltip all auto-flow / mechanically extend.
// Registry is the SINGLE SOURCE OF TRUTH for all cfg + GUI metadata (v5.14.9.F.5 Option D).
// uint8_t fits 8 entries; expand to uint16_t if exceeded (static_assert above).
//
// Domain identity: position-exit mechanics. Add here if the flag governs HOW positions exit
// (partial-fill geometry, breakeven ratchet logic, exit-related dispatcher behavior). Use
// other domains (GATE/RISK/ML/OPS — v5.14.9.F.1-.F.3) for non-lifecycle flags.
//======================================================================
// [DERIVED]   (tool-refreshed — ROW_COUNT/CONSUMERS generators land with the drift-gate generalization; empty skeleton is correct, D-327)
//======================================================================
// [END_REGISTRY]_[FOREACH_LIFECYCLE_CFG_FLAG]
//======================================================================

#endif // LIFECYCLE_CFG_FLAG_REGISTRY_HPP
