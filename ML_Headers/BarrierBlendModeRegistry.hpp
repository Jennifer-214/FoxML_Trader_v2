// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BARRIER BLEND MODE REGISTRY — v5.15.5.A]
//======================================================================================================
// FOREACH_BARRIER_BLEND_MODE(X) registry — adding a new barrier blend mode is 1 row:
//   1. Append X(NAME, flags_expr, "doc") below
//   2. The mode_flags column encodes branchless dispatch semantics as bit-packed
//      MODE_F_* bitmap; consumers read MODE_FLAGS[mode] and mask-AND against
//      individual MODE_F_* constants — no nested if/else on the enum value.
//
// Operator selects via cfg.barrier_blend_mode enum (5 values):
//   0 = LEGACY                — cfg-direct (pre-v5.15.5 behavior); per-arm barriers loaded but unused
//   1 = BLEND                 — Σ wᵢ · barrierᵢ from stamp body, weights from bandit/Ridge
//   2 = DOMINANT              — argmax(weights) picks one arm's barriers (exact train-serve match per trade)
//   3 = BOTH_BLEND_DRIVES     — blend drives the trade; dominant logged for shadow-mode A/B compare
//   4 = BOTH_DOMINANT_DRIVES  — dominant drives the trade; blend logged for shadow-mode A/B compare
//
// DESIGN — BIT-PACKED MODE_FLAGS LOOKUP (CLAUDE.md item 28 + Rule 8 Pattern 8b):
//   Each mode's behavior is described by a small bitmap of MODE_F_* flags:
//     MODE_F_BLEND_DRIVES     — blend value writes to params->tp_pct
//     MODE_F_DOMINANT_DRIVES  — dominant arm's barriers write to params->tp_pct
//     MODE_F_SHADOW_ACTIVE    — record the non-driving result into shadow ring telemetry
//     MODE_F_LEGACY           — fall back to cfg.ml_tp_pct / cfg.ml_sl_pct
//
//   Auto-generated MODE_FLAGS[] table is indexed by the mode enum; consumers read:
//     uint8_t flags = MODE_FLAGS[mode];
//     bool blend_drives    = flags & MODE_F_BLEND_DRIVES;     // cmov, no branch
//     bool dominant_drives = flags & MODE_F_DOMINANT_DRIVES;  // cmov, no branch
//     bool shadow_active   = flags & MODE_F_SHADOW_ACTIVE;    // cmov, no branch
//
//   Branchless dispatch via ternary chain (single cmov per output):
//     FPN_Binary<F> tp_pct = blend_drives    ? blend_tp_d
//                   : dominant_drives ? FPN_FromDouble<F>(per_arm_buy_tp_pct[dominant_h])
//                   : config->ml_tp_pct;
//
// FUTURE EXTENSION: adding a 6th mode is 1 row in FOREACH_BARRIER_BLEND_MODE
// + 1 flags expression. MODE_FLAGS[] regenerates automatically; consumer
// dispatch code stays unchanged.
//
// Pattern documented in DESIGN_SPECS/per-horizon-barrier-blending-with-shadow-mode.md
// + DESIGN_SPECS/cache-layout-discipline-for-hot-side-structs.md (Rule 8 branchless dispatch).
// Slow-path-only; hot path UNTOUCHED (consumes params->tp_pct as before).
//======================================================================================================
#ifndef BARRIER_BLEND_MODE_REGISTRY_HPP
#define BARRIER_BLEND_MODE_REGISTRY_HPP

#include <strings.h>     // strcasecmp
#include <stdlib.h>      // atoi
#include <stdint.h>      // uint8_t

#include "../MemHeaders/BitmapMacros.hpp"  // BITMAP_BIT_U8 for MODE_F_* constants (item 20)

// NOTE: deliberately NOT wrapped in `namespace tt` — matches CoreModelZoo.hpp +
// StrategyParameters.hpp consumer convention; MODE_FLAGS[] table + MASK constants
// need to be accessible from template functions without namespace qualification.

//======================================================================================================
// [MODE_F_* BIT-PACKED FLAGS]
//======================================================================================================
// Behavior flags that describe each blend mode's semantics. Multiple flags
// can be combined per mode (e.g., BOTH_*_DRIVES = primary flag | SHADOW_ACTIVE).
// 4 bits used today; 4 bits free for future modes.
constexpr uint8_t MODE_F_BLEND_DRIVES    = BITMAP_BIT_U8(0);  // 0x01 — blend writes to params->tp_pct
constexpr uint8_t MODE_F_DOMINANT_DRIVES = BITMAP_BIT_U8(1);  // 0x02 — dominant arm writes to params->tp_pct
constexpr uint8_t MODE_F_SHADOW_ACTIVE   = BITMAP_BIT_U8(2);  // 0x04 — record non-driving result in shadow ring
constexpr uint8_t MODE_F_LEGACY          = BITMAP_BIT_U8(3);  // 0x08 — fall back to cfg.ml_tp_pct / cfg.ml_sl_pct

//======================================================================================================
// [REGISTRY TUPLE]
//======================================================================================================
// Tuple: X(name, mode_flags, doc_string)
//   name        — UPPERCASE token; used for MODE_BARRIER_BLEND_<name> enum
//   mode_flags  — MODE_F_* bitmap composition; drives MODE_FLAGS[] auto-gen
//   doc_string  — engine.cfg.example auto-doc + operator-facing description
//
// APPEND-ONLY discipline. Reordering or removing a row shifts enum values,
// which would invalidate any stamp body that recorded the prior enum value
// (TECH_DEBT-024 / cfg-drift Tier 1 entry once stamped).
#define FOREACH_BARRIER_BLEND_MODE(X) \
    X(LEGACY,               MODE_F_LEGACY,                                  "cfg-direct (pre-v5.15.5 behavior; per-arm barriers loaded but unused)") \
    X(BLEND,                MODE_F_BLEND_DRIVES,                            "Σ wᵢ · barrierᵢ — bandit/Ridge weighted blend from stamp body") \
    X(DOMINANT,             MODE_F_DOMINANT_DRIVES,                         "argmax(weights) picks one arm's barriers (exact train-serve match)") \
    X(BOTH_BLEND_DRIVES,    MODE_F_BLEND_DRIVES | MODE_F_SHADOW_ACTIVE,     "Blend drives trade; dominant logged for shadow-mode A/B compare") \
    X(BOTH_DOMINANT_DRIVES, MODE_F_DOMINANT_DRIVES | MODE_F_SHADOW_ACTIVE,  "Dominant drives trade; blend logged for shadow-mode A/B compare")

//======================================================================================================
// [AUTO-GENERATED ENUM]
//======================================================================================================
// MODE_BARRIER_BLEND_LEGACY=0, BLEND=1, DOMINANT=2, BOTH_BLEND_DRIVES=3, BOTH_DOMINANT_DRIVES=4
// MODE_BARRIER_BLEND_COUNT = 5 (trailing sentinel)
enum {
#define X(id, flags, doc) MODE_BARRIER_BLEND_##id,
    FOREACH_BARRIER_BLEND_MODE(X)
#undef X
    MODE_BARRIER_BLEND_COUNT
};

//======================================================================================================
// [AUTO-GENERATED MODE_FLAGS[] LOOKUP TABLE]
//======================================================================================================
// Indexed by MODE_BARRIER_BLEND_* enum; each entry holds the bit-packed flags
// from the FOREACH tuple's 2nd column. Slow-path dispatch reads this table
// once per cycle (1 byte; same L1 line as adjacent slow-path scratch) and
// derives all branchless predicates from it.
constexpr uint8_t MODE_FLAGS[MODE_BARRIER_BLEND_COUNT] = {
#define X(id, flags, doc) [MODE_BARRIER_BLEND_##id] = (uint8_t)(flags),
    FOREACH_BARRIER_BLEND_MODE(X)
#undef X
};

//======================================================================================================
// [BRANCHLESS DISPATCH PREDICATE ACCESSORS]
//======================================================================================================
// Single-cycle mask-AND from the bit-packed flag table. Pattern is reused at
// the ML_BuildParameters dispatch site + at the shadow-ring write site.
// inline avoids per-call-site duplication; constexpr-eligible at -O2+ when
// mode is compile-time-known.
static inline bool BarrierBlendMode_BlendDrives(int mode) {
    if (mode < 0 || mode >= MODE_BARRIER_BLEND_COUNT) return false;
    return (MODE_FLAGS[mode] & MODE_F_BLEND_DRIVES) != 0;
}
static inline bool BarrierBlendMode_DominantDrives(int mode) {
    if (mode < 0 || mode >= MODE_BARRIER_BLEND_COUNT) return false;
    return (MODE_FLAGS[mode] & MODE_F_DOMINANT_DRIVES) != 0;
}
static inline bool BarrierBlendMode_ShadowActive(int mode) {
    if (mode < 0 || mode >= MODE_BARRIER_BLEND_COUNT) return false;
    return (MODE_FLAGS[mode] & MODE_F_SHADOW_ACTIVE) != 0;
}
static inline bool BarrierBlendMode_IsLegacy(int mode) {
    if (mode < 0 || mode >= MODE_BARRIER_BLEND_COUNT) return true;  // out-of-range → safe legacy fallback
    return (MODE_FLAGS[mode] & MODE_F_LEGACY) != 0;
}

//======================================================================================================
// [TOSTRING / FROMSTRING]
//======================================================================================================
// Operator-facing serialization for cfg-file parse, engine.cfg.example
// auto-doc, GUI dropdown labels (via the SettingsPanel mode selector),
// and stamp body cfg-drift comparison.
static inline const char* BarrierBlendMode_ToString(int mode) {
    switch (mode) {
#define X(id, flags, doc) case MODE_BARRIER_BLEND_##id: return #id;
        FOREACH_BARRIER_BLEND_MODE(X)
#undef X
        default: return "UNKNOWN";
    }
}

static inline int BarrierBlendMode_FromString(const char* s) {
    if (!s || !*s) return MODE_BARRIER_BLEND_LEGACY;
#define X(id, flags, doc) if (strcasecmp(s, #id) == 0) return MODE_BARRIER_BLEND_##id;
    FOREACH_BARRIER_BLEND_MODE(X)
#undef X
    // Numeric fallback ("0" / "1" / ...) — operator might write the raw enum value.
    long n = atol(s);
    if (n >= 0 && n < MODE_BARRIER_BLEND_COUNT) return (int)n;
    return MODE_BARRIER_BLEND_LEGACY;
}

static inline const char* BarrierBlendMode_Doc(int mode) {
    switch (mode) {
#define X(id, flags, doc) case MODE_BARRIER_BLEND_##id: return doc;
        FOREACH_BARRIER_BLEND_MODE(X)
#undef X
        default: return "unknown barrier blend mode";
    }
}

//======================================================================================================
// [COMPILE-TIME SANITY CHECKS]
//======================================================================================================
// Asserts the registry hasn't been silently corrupted (entries removed without
// updating MODE_BARRIER_BLEND_COUNT, or flags expression dropped).
static_assert(MODE_BARRIER_BLEND_COUNT == 5,
              "FOREACH_BARRIER_BLEND_MODE must have exactly 5 entries for v5.15.5; "
              "if extending, update this assertion + any downstream consumers");
static_assert(MODE_FLAGS[MODE_BARRIER_BLEND_LEGACY] == MODE_F_LEGACY,
              "LEGACY mode must map to MODE_F_LEGACY exactly (no other flags)");
static_assert(MODE_FLAGS[MODE_BARRIER_BLEND_BLEND] == MODE_F_BLEND_DRIVES,
              "BLEND mode must map to MODE_F_BLEND_DRIVES exactly");
static_assert(MODE_FLAGS[MODE_BARRIER_BLEND_DOMINANT] == MODE_F_DOMINANT_DRIVES,
              "DOMINANT mode must map to MODE_F_DOMINANT_DRIVES exactly");
static_assert((MODE_FLAGS[MODE_BARRIER_BLEND_BOTH_BLEND_DRIVES]
                & MODE_F_SHADOW_ACTIVE) != 0,
              "BOTH_BLEND_DRIVES must have shadow active for telemetry");
static_assert((MODE_FLAGS[MODE_BARRIER_BLEND_BOTH_DOMINANT_DRIVES]
                & MODE_F_SHADOW_ACTIVE) != 0,
              "BOTH_DOMINANT_DRIVES must have shadow active for telemetry");

#endif // BARRIER_BLEND_MODE_REGISTRY_HPP
