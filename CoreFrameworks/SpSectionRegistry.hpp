// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [SLOW-PATH SECTION REGISTRY — v5.15.5.B.5]
//======================================================================================================
// X-macro registry for slow-path latency-profiling section indices.
// Replaces the v5.1.3-era static constexpr int SP_SECTION_* declarations
// previously nested inside NodeContext<F> (and their back-compat aliases
// SP_SECTION_OTHER / SP_SECTION_PUSH_PARAMS).
//
// Closes a small Class-18 mirror: adding a 6th slow-path section was a
// 4-touchpoint change (constexpr int decl + index in slow_path_breakdown
// array sizing via SP_SECTION_COUNT + per-section Sample_Start/Sample_End
// in EngineSharded.hpp's rdtsc-bracket block + GUI render row). Now it's
// ONE row here; SP_SECTION_COUNT auto-flows; section name is queryable
// via SP_SECTION_DOC[] for tooltips/logs.
//
// Cross-references:
//   CLAUDE.md item 13 (X-macro registry pattern)
//   CLAUDE.md item 19 (structural fix preferred)
//   DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md
//======================================================================================================
#ifndef SP_SECTION_REGISTRY_HPP
#define SP_SECTION_REGISTRY_HPP

namespace tt {

//------------------------------------------------------------------------------
// Tuple: X(NAME, DOC)
//   NAME — UPPER_SNAKE_CASE; produces SP_SECTION_<NAME> enum value
//   DOC  — human description (used in tooltips + log headers)
//
// Order matters — sections are sampled IN this order each cycle. Inserting
// in the middle SHIFTS subsequent section indices (slow_path_breakdown[]
// array elements re-map). For backward compat with prior snapshot files
// that may persist per-section breakdown counts, append new sections at
// the END of the registry.
//------------------------------------------------------------------------------
#define FOREACH_SP_SECTION(X)                                                                       \
    X(ROLLING,   "EventLoop_UpdateRollingStateOneCore + cadence setup (depth read, swap pickup, mtm_price); dominates cycle (~100-300μs steady-state with W=1024)") \
    X(REBUILD,   "EventLoop_RebuildOneCore: regime classify + strategy dispatch + gate compute (~5-30μs typical)") \
    X(PUSH,      "Seqlock push of pending_params to ExecutionCore (~100-500ns; single FPN_Binary copy + atomic)") \
    X(TIME_EXIT, "EventLoop_TimeExitOneCore (~100-300ns)")                                          \
    X(TRAIL_SL,  "EventLoop_TrailingSLRatchetOneCore (~100-300ns)")

//------------------------------------------------------------------------------
// Auto-generated enum values + count sentinel.
//------------------------------------------------------------------------------
enum SpSection {
#define X(NAME, DOC) SP_SECTION_##NAME,
    FOREACH_SP_SECTION(X)
#undef X
    SP_SECTION_COUNT  // sentinel; sized for slow_path_breakdown[SP_SECTION_COUNT]
};

//------------------------------------------------------------------------------
// Auto-generated documentation strings (for tooltips, log headers, etc.).
//------------------------------------------------------------------------------
inline const char* SP_SECTION_NAME(int idx) {
    switch (idx) {
#define X(NAME, DOC) case SP_SECTION_##NAME: return #NAME;
        FOREACH_SP_SECTION(X)
#undef X
        default: return "?";
    }
}
inline const char* SP_SECTION_DOC(int idx) {
    switch (idx) {
#define X(NAME, DOC) case SP_SECTION_##NAME: return DOC;
        FOREACH_SP_SECTION(X)
#undef X
        default: return "";
    }
}

static_assert(SP_SECTION_COUNT >= 5,
              "FOREACH_SP_SECTION must keep at least the v5.1.3 set "
              "(ROLLING, REBUILD, PUSH, TIME_EXIT, TRAIL_SL).");

}  // namespace tt

#endif  // SP_SECTION_REGISTRY_HPP
