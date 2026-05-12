// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [PER-ARM FLAG REGISTRY — v5.15.5.A]
//======================================================================================================
// FOREACH_PER_ARM_FLAG(X) registry — adding a new per-arm boolean state is 1 row:
//   1. Append X(NAME, field_name, "doc") below
//   2. The field auto-flows into EnsembleModelZoo struct as a uint8_t bitmap
//      where bit N = "this flag is set for arm N" (per CLAUDE.md item 20
//      BITMAP_* API conventions); accessors via BITMAP_IS_SET / BITMAP_SET
//      / BITMAP_ANY using BITMAP_BIT_U8(arm_idx).
//
// STORAGE MODEL: per-flag uint8_t field (one byte per flag kind),
// bit N within = "arm N has this flag." 8 arms × N flags = 8N bits
// total, stored as N bytes (1 byte per flag).
//
//   Alternative (rejected for v5.15.5): per-arm uint8_t field (one byte
//   per arm position), bit M within = "arm has flag M." Would require
//   refactoring the existing v5.14 disabled_horizon_mask which uses the
//   per-flag-field layout. Stay with per-flag-field for boundary stability
//   per CLAUDE.local.md 2026-05-06 boundary-stable rule.
//
// PRECEDENT: `disabled_horizon_mask` (v5.14+) is the canonical per-flag
// uint8_t bitmap of arms. v5.15.5.A adds `arms_with_barriers_mask` as the
// second entry in the registry. Future per-arm boolean state (warmed_up,
// drift_breached, reward_observed) joins as 1 row + auto-flows.
//
// STRUCTURAL FIX (CLAUDE.md item 19): closes the recurrence class
// "operator adds new per-arm boolean state, scatters check sites across
// files." Adding a new per-arm flag = 1 row; check sites auto-discover
// via BITMAP_* accessors typed against the auto-generated field name.
// Compile-time enforcement: forgetting to update a consumer becomes
// impossible (the field generation is registry-driven).
//
// USAGE PATTERN:
//   // Test if arm h has the LOADED_BARRIERS flag:
//   if (BITMAP_IS_SET(ezoo->arms_with_barriers_mask, BITMAP_BIT_U8(h))) {
//       // use ezoo->per_arm_barriers[h]
//   }
//
//   // Mark arm h as having barriers (called at LoadFromCfg post-load):
//   BITMAP_SET(ezoo->arms_with_barriers_mask, BITMAP_BIT_U8(h));
//
//   // Check if ANY arm has flag F set:
//   if (BITMAP_ANY(ezoo-><flag_field>, 0xFF)) { ... }
//
//   // Check "all loaded arms have barriers" combined-mask query:
//   uint8_t loaded = (1u << ezoo->primary_count) - 1u;
//   if (BITMAP_ALL(ezoo->arms_with_barriers_mask, loaded)) { ... }
//
// Pattern documented in DESIGN_SPECS/cache-layout-discipline-for-hot-side-structs.md
// Rule 5 (bit-pack boolean cohorts) + DESIGN_SPECS/bitmap-flag-api.md.
//======================================================================================================
#ifndef PER_ARM_FLAG_REGISTRY_HPP
#define PER_ARM_FLAG_REGISTRY_HPP

#include <stdint.h>  // uint8_t

#include "../MemHeaders/BitmapMacros.hpp"  // BITMAP_BIT_U8 + BITMAP_* accessors

namespace tt {

//======================================================================================================
// [REGISTRY TUPLE]
//======================================================================================================
// Tuple: X(name, field_name, doc_string)
//   name        — UPPERCASE token; used for PER_ARM_FLAG_<name> enum
//                 (for telemetry / debug; ezoo fields use the field_name)
//   field_name  — exact C identifier for the uint8_t bitmap field that
//                 lives on EnsembleModelZoo. Bit N = arm N has this flag.
//   doc_string  — operator-facing description (engine.cfg.example /
//                 MLStatusPanel tooltip if surfaced).
//
// APPEND-ONLY discipline. Reordering shifts PER_ARM_FLAG_<name> enum
// values which would invalidate any future telemetry that records the
// enum (none today; future-proof discipline). Field NAMES are stable
// — renaming a field would require updating every consumer.
//
// DEFAULT VALUE: all per-arm flag fields default to 0 (no arms have
// the flag). LoadFromCfg / runtime code sets bits as conditions become
// true. Clearing on reload is the responsibility of EnsembleModelZoo_Init
// (which memsets the struct or explicit zero-init).
#define FOREACH_PER_ARM_FLAG(X) \
    X(DISABLED,        disabled_horizon_mask,    "Per-arm operator-disabled (legacy v5.14; cfg.disabled_horizons CSV)") \
    X(LOADED_BARRIERS, arms_with_barriers_mask,  "Per-arm has stamp-recorded TP/SL barriers (v5.15.5+)")

//======================================================================================================
// [AUTO-GENERATED ENUM]
//======================================================================================================
// PER_ARM_FLAG_DISABLED=0, LOADED_BARRIERS=1, ...
// PER_ARM_FLAG_COUNT = N (trailing sentinel)
// Useful for iteration in tests / telemetry; ezoo accesses use the
// field_name directly (e.g., ezoo->disabled_horizon_mask).
enum {
#define X(id, field, doc) PER_ARM_FLAG_##id,
    FOREACH_PER_ARM_FLAG(X)
#undef X
    PER_ARM_FLAG_COUNT
};

//======================================================================================================
// [FIELD DECLARATION MACRO]
//======================================================================================================
// Use inside EnsembleModelZoo struct body to auto-generate the
// uint8_t bitmap fields. Each invocation expands to one
// `uint8_t <field_name>;` line per registry entry.
//
// Usage in CoreModelZoo.hpp:
//   struct EnsembleModelZoo<F> {
//       // ... other fields ...
//       PER_ARM_FLAG_DECLARE_FIELDS()  // expands to uint8_t per-flag bitmap fields
//       // ... more fields ...
//   };
#define PER_ARM_FLAG_DECLARE_FIELDS() \
    /* v5.15.5.A — per-arm flag bitmaps auto-generated from FOREACH_PER_ARM_FLAG. */ \
    /* Bit N in each field = "arm N has this flag." Single-writer slow-path; */ \
    /* read at slow-path dispatch sites via BITMAP_IS_SET. */ \
    FOREACH_PER_ARM_FLAG(PER_ARM_FLAG_DECLARE_FIELD_ONE)

#define PER_ARM_FLAG_DECLARE_FIELD_ONE(id, field, doc) \
    uint8_t field;  /* doc */

//======================================================================================================
// [INIT MACRO — zero all per-arm flag fields]
//======================================================================================================
// Use inside EnsembleModelZoo_Init to reset all per-arm flag bitmaps.
// Expansion: `ezoo->disabled_horizon_mask = 0; ezoo->arms_with_barriers_mask = 0; ...`
#define PER_ARM_FLAG_INIT_FIELDS(ezoo_ptr) \
    FOREACH_PER_ARM_FLAG(PER_ARM_FLAG_INIT_FIELD_ONE_)

#define PER_ARM_FLAG_INIT_FIELD_ONE_(id, field, doc) \
    (ezoo_ptr)->field = 0;

//======================================================================================================
// [TOSTRING — for telemetry / debug]
//======================================================================================================
// Maps PER_ARM_FLAG_<name> enum value to its string token (e.g.,
// "DISABLED", "LOADED_BARRIERS"). Useful in CRITICAL log lines + GUI
// diagnostic panels.
static inline const char* PerArmFlag_ToString(int flag_enum) {
    switch (flag_enum) {
#define X(id, field, doc) case PER_ARM_FLAG_##id: return #id;
        FOREACH_PER_ARM_FLAG(X)
#undef X
        default: return "UNKNOWN";
    }
}

//======================================================================================================
// [COMPILE-TIME SANITY CHECKS]
//======================================================================================================
// Per CLAUDE.md item 13 X-macro discipline + cfg-flag-eligibility-criteria
// "Cohort audit when new field has siblings" rule (2026-05-11): existing
// disabled_horizon_mask must remain the first entry (legacy v5.14
// position; renaming would break consumers). New entries append.
static_assert(PER_ARM_FLAG_DISABLED == 0,
              "DISABLED must be the first per-arm flag entry (legacy v5.14 position)");
static_assert(PER_ARM_FLAG_COUNT >= 2,
              "Must have at least DISABLED + LOADED_BARRIERS entries for v5.15.5");

} // namespace tt

#endif // PER_ARM_FLAG_REGISTRY_HPP
