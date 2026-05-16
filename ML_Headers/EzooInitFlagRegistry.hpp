// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [EZOO INIT FLAG REGISTRY — v5.15.5.A.2.bcd]
//======================================================================================================
// FOREACH_EZOO_INIT_FLAG(X) registry — adding a new ensemble-init flag is 1 row:
//   1. Append X(NAME, "doc") below
//   2. Auto-generates MASK_EZOO_<NAME> = BITMAP_BIT_U8(idx) constant
//   3. Auto-flows into single uint8_t ezoo->init_flags bitmap field
//   4. Consumers read via BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_<NAME>)
//      and write via BITMAP_SET / BITMAP_CLR per CLAUDE.md item 20
//
// STORAGE: single uint8_t bitmap on EnsembleModelZoo. 4 bits used today
// (ACTIVE, BANDITS_READY, EXIT_BANDITS_READY, THOMPSON_READY); 4 bits
// free for future init flags (e.g., RIDGE_INITIALIZED,
// CALIB_LOG_READY, SHADOW_RING_INITIALIZED, etc.).
//
// MIGRATION (v5.15.5.A.2.bcd):
//   - Removes 4 separate `int` fields from EnsembleModelZoo:
//     `active`, `initialized_bandits`, `initialized_exit_bandits`,
//     `initialized_thompson_bandits` (16 bytes total).
//   - Adds single `uint8_t init_flags` field (1 byte).
//   - Net storage: -15 bytes per ezoo (absorbed into .A.2.d Hot/Warm/Cold
//     cluster padding to restore sizeof(EnsembleModelZoo<64>) % 64 == 0).
//
// STRUCTURAL FIX (CLAUDE.md item 19): closes the recurrence class
// "operator adds new init flag, scatters field declarations + check
// sites across files." Adding a new flag = 1 row in FOREACH_EZOO_INIT_FLAG;
// check sites use canonical BITMAP_IS_SET pattern; compile-time enforcement
// via auto-generated MASK constants.
//
// USAGE PATTERN:
//   // Test if bandits are wired:
//   if (BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY)) { ... }
//
//   // Mark bandits as wired (called at end of EnsembleModelZoo_InitBandits):
//   BITMAP_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY);
//
//   // Branchless multi-flag check ("all bandit subsystems wired?"):
//   constexpr uint8_t ALL_BANDITS = MASK_EZOO_BANDITS_READY |
//                                    MASK_EZOO_EXIT_BANDITS_READY |
//                                    MASK_EZOO_BUY_THOMPSON_READY;
//   if (BITMAP_ALL(ezoo->init_flags, ALL_BANDITS)) { ... }
//
// Pattern documented in DESIGN_SPECS/cache-layout-discipline-for-hot-side-structs.md
// Rule 5 (bit-pack boolean cohorts) + DESIGN_SPECS/bitmap-flag-api.md.
//======================================================================================================
#ifndef EZOO_INIT_FLAG_REGISTRY_HPP
#define EZOO_INIT_FLAG_REGISTRY_HPP

#include <stdint.h>  // uint8_t

#include "../MemHeaders/BitmapMacros.hpp"  // BITMAP_BIT_U8 + BITMAP_* accessors

// NOTE: deliberately NOT wrapped in `namespace tt` — matches CoreModelZoo.hpp
// convention (consumer file uses globally-namespaced enums/types). The MASK
// constants need to be accessible from template functions in CoreModelZoo
// without namespace qualification. Sister registries (FailureModeRegistry,
// etc.) use `namespace tt` because their consumers do too.

//======================================================================================================
// [REGISTRY TUPLE]
//======================================================================================================
// Tuple: X(name, doc_string)
//   name        — UPPERCASE token; bit position determined by registry order;
//                 generates MASK_EZOO_<name> constant + EZOO_INIT_FLAG_<name>
//                 enum value (for diagnostic / iteration use).
//   doc_string  — operator-facing description (for engine.cfg.example auto-doc
//                 + MLStatusPanel tooltips if surfaced).
//
// APPEND-ONLY discipline. Reordering shifts MASK_EZOO_<name> bit positions,
// which would invalidate any stamp body that recorded the bitmap value
// (none today; future-proof discipline if init_flags is ever stamped).
//
// LEGACY POSITIONAL STABILITY: ACTIVE is the first entry (bit 0) — matches
// the conceptual "is the ensemble active at all" semantic that's most-tested.
#define FOREACH_EZOO_INIT_FLAG(X) \
    X(ACTIVE,                  "Ensemble path active (1 = use ensemble, 0 = single-zoo fallback)") \
    X(BANDITS_READY,           "Exp3 buy-side bandits wired (post-LoadFromCfg + _InitBandits)") \
    X(EXIT_BANDITS_READY,      "Exit-side bandits wired (post-exit_predictor load + _InitExitBandits)") \
    /* v5.15.5.F.4d TECH_DEBT-084 — renamed THOMPSON_READY → BUY_THOMPSON_READY for symmetric naming with EXIT_THOMPSON_READY (FOREACH_BANDIT_SIDE first canonical) */ \
    X(BUY_THOMPSON_READY,      "Thompson posterior bandits wired (buy-side; post-LoadFromCfg + _InitBuyThompsonBandits)") \
    /* v5.15.5.F.4d — exit-side Thompson mirror per FOREACH_BANDIT_SIDE auto-mirror (§ G of merged plan body) */ \
    X(EXIT_THOMPSON_READY,     "Exit-side Thompson posterior bandits wired (post-LoadFromCfg + _InitExitThompsonBandits)")

//======================================================================================================
// [AUTO-GENERATED ENUM]
//======================================================================================================
// EZOO_INIT_FLAG_ACTIVE=0, BANDITS_READY=1, EXIT_BANDITS_READY=2, THOMPSON_READY=3
// EZOO_INIT_FLAG_COUNT = N (trailing sentinel)
// Useful for iteration in tests / telemetry; ezoo accesses use the MASK_*
// constants below.
enum {
#define X(id, doc) EZOO_INIT_FLAG_##id,
    FOREACH_EZOO_INIT_FLAG(X)
#undef X
    EZOO_INIT_FLAG_COUNT
};

//======================================================================================================
// [AUTO-GENERATED MASK CONSTANTS]
//======================================================================================================
// MASK_EZOO_ACTIVE              = BITMAP_BIT_U8(0) = 0x01
// MASK_EZOO_BANDITS_READY       = BITMAP_BIT_U8(1) = 0x02
// MASK_EZOO_EXIT_BANDITS_READY  = BITMAP_BIT_U8(2) = 0x04
// MASK_EZOO_BUY_THOMPSON_READY      = BITMAP_BIT_U8(3) = 0x08
// 4 bits used; 4 bits free for future init flags (RIDGE_INITIALIZED,
// CALIB_LOG_READY, SHADOW_RING_INITIALIZED, etc.).
#define X(id, doc) constexpr uint8_t MASK_EZOO_##id = BITMAP_BIT_U8(EZOO_INIT_FLAG_##id);
FOREACH_EZOO_INIT_FLAG(X)
#undef X

//======================================================================================================
// [TOSTRING — for telemetry / debug]
//======================================================================================================
// Maps EZOO_INIT_FLAG_<name> enum value to its string token. Useful in
// CRITICAL log lines + diagnostic panels.
static inline const char* EzooInitFlag_ToString(int flag_enum) {
    switch (flag_enum) {
#define X(id, doc) case EZOO_INIT_FLAG_##id: return #id;
        FOREACH_EZOO_INIT_FLAG(X)
#undef X
        default: return "UNKNOWN";
    }
}

//======================================================================================================
// [COMPILE-TIME SANITY CHECKS]
//======================================================================================================
// Asserts the registry stays within uint8_t storage (8 bits max).
// At 4 entries today + future growth, will fit comfortably; if a 9th
// flag is ever proposed, widen to uint16_t and update this assertion.
static_assert(EZOO_INIT_FLAG_COUNT <= 8,
              "FOREACH_EZOO_INIT_FLAG count exceeds uint8_t storage (8 bits); "
              "widen ezoo->init_flags to uint16_t and update this assertion");

// Per CLAUDE.local.md cohort-audit rule + legacy positional stability:
// ACTIVE must remain bit 0 (most-frequently-checked flag; tested by every
// strategy dispatch call to determine ensemble vs single-zoo path).
static_assert(EZOO_INIT_FLAG_ACTIVE == 0,
              "ACTIVE must remain the first per-ezoo init flag (bit 0)");

#endif // EZOO_INIT_FLAG_REGISTRY_HPP
