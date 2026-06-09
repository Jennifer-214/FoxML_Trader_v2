// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [SESSION PHASE REGISTRY — v5.15.5.B.5]
//======================================================================================================
// Closes TECH_DEBT-040: the 4-cfg-field cohort (session_asian/european/us/
// overnight_mult) was an X-macro registry candidate carrying:
//   1. ControllerConfig FPN_Binary<F> field declarations (4 rows)
//   2. ControllerConfig_Default initial-value assignments (4 rows)
//   3. CFG_PARSE_FPN parser entries (4 rows)
//   4. SettingsPanel GUI rows (4 rows)
//   5. Consumer-side 4-way if/else dispatch (3 sites: RebuildOneCore,
//      ShardedSnapshot publisher, legacy PortfolioController)
//   6. cfg.example operator documentation (4 rows)
//
// 6 site categories × 4 entries each = 24 mirror touchpoints per session-
// phase change. Adding a 5th session (e.g., "ROLLOVER" for daily futures
// rollover window) would require synchronizing ALL of those. Now: ONE row
// in this registry; all sites auto-flow.
//
// Float-cohort cfg-registry variant (first reference). Distinct from the
// boolean-bitmap variant of FOREACH_<DOMAIN>_CFG_FLAG (FOREACH_OPS_CFG_FLAG,
// FOREACH_RISK_CFG_FLAG, etc.). Cohorts of FPN_Binary<F> cfg fields with a
// SHARED SEMANTIC ROLE + uniform consumer dispatch get this treatment;
// the storage is direct (no bit-packing) but multi-site sync is registry-
// driven. Subsection in cfg-flag-eligibility-criteria.md captures the
// float-cohort variant promotion criteria.
//
// Branchless consumer dispatch via SESSION_BY_HOUR[24] table-lookup:
// pre-.B.5 consumers had a 4-way data-dependent if/else cascade
// (`if (h < 7) ... else if (h < 13) ... else if (h < 20) ... else ...`).
// Per CLAUDE.md item 28 (latency-vs-cache decision framework): data-
// dependent branches on a per-cycle hour read mispredict ~25% at session
// transitions; SESSION_BY_HOUR[h] table-lookup is 1 load + 0 branches.
//
// Cross-references:
//   CLAUDE.md item 13 (X-macro registry pattern)
//   CLAUDE.md item 19 (structural fix preferred)
//   CLAUDE.md item 28 (latency-vs-cache decision framework)
//   DESIGN_SPECS/cfg-flag-eligibility-criteria.md (float-cohort variant)
//   DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md
//   DOCS/TECH_DEBT.md TECH_DEBT-040 (✅ CLOSED v5.15.5.B.5)
//======================================================================================================
#ifndef SESSION_PHASE_REGISTRY_HPP
#define SESSION_PHASE_REGISTRY_HPP

#include <cstdint>

namespace tt {

//------------------------------------------------------------------------------
// Tuple: X(NAME_UPPER, name_lower, START, END, DEFAULT_MULT, DOC)
//   NAME_UPPER    — UPPER_SNAKE_CASE; produces SESSION_PHASE_<NAME_UPPER> enum
//   name_lower    — lower_snake_case; produces session_<name_lower>_mult cfg field
//   START         — inclusive start hour (UTC, 0-23)
//   END           — exclusive end hour (UTC, 1-24)
//   DEFAULT_MULT  — default gate multiplier for this session (cfg default value)
//   DOC           — human description (operator docs + GUI tooltips)
//
// Coverage: START/END ranges MUST collectively cover [0, 24) with no gaps
// or overlaps. Validated via static_asserts below.
//
// Adding a 5th session: append one row. ControllerConfig field + default
// + parser + GUI + consumer dispatch + cfg.example all auto-flow via the
// FOREACH_SESSION_PHASE expansions at each site.
//------------------------------------------------------------------------------
#define FOREACH_SESSION_PHASE(X)                                                                    \
    X(ASIAN,     asian,      0,  7, 1.5, "00:00-06:59 UTC; thin liquidity, wider gates")            \
    X(EUROPEAN,  european,   7, 13, 1.0, "07:00-12:59 UTC; London open, normal liquidity")          \
    X(US,        us,        13, 20, 0.8, "13:00-19:59 UTC; NY open, best liquidity, tighter gates") \
    X(OVERNIGHT, overnight, 20, 24, 1.3, "20:00-23:59 UTC; declining volume, wider gates")

//------------------------------------------------------------------------------
// Auto-generated enum values.
//------------------------------------------------------------------------------
enum SessionPhase {
#define X(NAME_U, name_l, START, END, MULT, DOC) SESSION_PHASE_##NAME_U,
    FOREACH_SESSION_PHASE(X)
#undef X
    SESSION_PHASE_COUNT
};

//------------------------------------------------------------------------------
// Auto-derived hour → session-phase resolver. Used to build the
// SESSION_BY_HOUR[24] constexpr table below; can also be called directly
// for ad-hoc lookups.
//------------------------------------------------------------------------------
constexpr uint8_t session_phase_for_hour(int h) {
#define X(NAME_U, name_l, START, END, MULT, DOC) \
    if (h >= (START) && h < (END)) return (uint8_t)SESSION_PHASE_##NAME_U;
    FOREACH_SESSION_PHASE(X)
#undef X
    return (uint8_t)SESSION_PHASE_ASIAN;  // fallback for malformed input (shouldn't happen given coverage asserts below)
}

//------------------------------------------------------------------------------
// Range-validity asserts: each entry has [START, END) ⊆ [0, 24) with START < END.
// Coverage asserts (no gaps, no overlaps) follow via the SESSION_BY_HOUR table.
//------------------------------------------------------------------------------
#define X(NAME_U, name_l, START, END, MULT, DOC)                                                   \
    static_assert((START) >= 0 && (START) < 24,                                                    \
                  "FOREACH_SESSION_PHASE " #NAME_U " START must be in [0, 24)");                    \
    static_assert((END) > 0 && (END) <= 24,                                                        \
                  "FOREACH_SESSION_PHASE " #NAME_U " END must be in (0, 24]");                      \
    static_assert((START) < (END),                                                                  \
                  "FOREACH_SESSION_PHASE " #NAME_U " START must be < END");
FOREACH_SESSION_PHASE(X)
#undef X

//------------------------------------------------------------------------------
// SESSION_BY_HOUR[24] lookup table — derived from the registry's start/end
// ranges via session_phase_for_hour(). Single load + index = branchless
// hour-of-day dispatch; replaces the 4-way data-dependent if/else cascade
// that occupied lines 2310-2314 of ControllerEventLoop.hpp pre-.B.5.
//------------------------------------------------------------------------------
constexpr uint8_t SESSION_BY_HOUR[24] = {
    session_phase_for_hour(0),  session_phase_for_hour(1),  session_phase_for_hour(2),
    session_phase_for_hour(3),  session_phase_for_hour(4),  session_phase_for_hour(5),
    session_phase_for_hour(6),  session_phase_for_hour(7),  session_phase_for_hour(8),
    session_phase_for_hour(9),  session_phase_for_hour(10), session_phase_for_hour(11),
    session_phase_for_hour(12), session_phase_for_hour(13), session_phase_for_hour(14),
    session_phase_for_hour(15), session_phase_for_hour(16), session_phase_for_hour(17),
    session_phase_for_hour(18), session_phase_for_hour(19), session_phase_for_hour(20),
    session_phase_for_hour(21), session_phase_for_hour(22), session_phase_for_hour(23),
};

//------------------------------------------------------------------------------
// Coverage assert: every hour 0..23 must resolve to a valid session phase
// (< SESSION_PHASE_COUNT). Validates registry coverage at compile time.
//------------------------------------------------------------------------------
static_assert(SESSION_BY_HOUR[ 0] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[ 0] out of range");
static_assert(SESSION_BY_HOUR[ 1] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[ 1] out of range");
static_assert(SESSION_BY_HOUR[ 2] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[ 2] out of range");
static_assert(SESSION_BY_HOUR[ 3] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[ 3] out of range");
static_assert(SESSION_BY_HOUR[ 4] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[ 4] out of range");
static_assert(SESSION_BY_HOUR[ 5] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[ 5] out of range");
static_assert(SESSION_BY_HOUR[ 6] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[ 6] out of range");
static_assert(SESSION_BY_HOUR[ 7] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[ 7] out of range");
static_assert(SESSION_BY_HOUR[ 8] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[ 8] out of range");
static_assert(SESSION_BY_HOUR[ 9] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[ 9] out of range");
static_assert(SESSION_BY_HOUR[10] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[10] out of range");
static_assert(SESSION_BY_HOUR[11] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[11] out of range");
static_assert(SESSION_BY_HOUR[12] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[12] out of range");
static_assert(SESSION_BY_HOUR[13] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[13] out of range");
static_assert(SESSION_BY_HOUR[14] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[14] out of range");
static_assert(SESSION_BY_HOUR[15] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[15] out of range");
static_assert(SESSION_BY_HOUR[16] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[16] out of range");
static_assert(SESSION_BY_HOUR[17] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[17] out of range");
static_assert(SESSION_BY_HOUR[18] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[18] out of range");
static_assert(SESSION_BY_HOUR[19] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[19] out of range");
static_assert(SESSION_BY_HOUR[20] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[20] out of range");
static_assert(SESSION_BY_HOUR[21] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[21] out of range");
static_assert(SESSION_BY_HOUR[22] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[22] out of range");
static_assert(SESSION_BY_HOUR[23] < SESSION_PHASE_COUNT, "SESSION_BY_HOUR[23] out of range");

}  // namespace tt

#endif  // SESSION_PHASE_REGISTRY_HPP
