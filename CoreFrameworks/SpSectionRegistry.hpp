// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/SpSectionRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[slow-path latency-profiling section registry — one row = enum index + name + doc string]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_SP_SECTION]
//======================================================================================================
#ifndef SP_SECTION_REGISTRY_HPP
#define SP_SECTION_REGISTRY_HPP

namespace tt {

//======================================================================
// [REGISTRY]_[FOREACH_SP_SECTION]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [MONITORING_PLANE]]
// [REFERENCE]_[DESIGN_SPEC]_[x-macro-registry-with-presence-dispatch]
// [REFERENCE]_[INVARIANT]_[H21]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[slow-path section indices — SP_SECTION_COUNT + name/doc lookups auto-flow from one row]
// [COLUMN]_[NAME]_[UPPER_SNAKE_CASE -> SP_SECTION_<NAME> enum value]
// [COLUMN]_[DOC]_[human description (tooltips + log headers)]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_SP_SECTION(X)                                                                       \
    X(ROLLING,   "EventLoop_UpdateRollingStateOneCore + cadence setup (depth read, swap pickup, mtm_price); dominates cycle (~100-300μs steady-state with W=1024)") \
    X(REBUILD,   "EventLoop_RebuildOneCore: regime classify + strategy dispatch + gate compute (~5-30μs typical)") \
    X(PUSH,      "Seqlock push of pending_params to ExecutionCore (~100-500ns; single FPN_Binary copy + atomic)") \
    X(TIME_EXIT, "EventLoop_TimeExitOneCore (~100-300ns)")                                          \
    X(TRAIL_SL,  "EventLoop_TrailingSLRatchetOneCore (~100-300ns)")                                  \
    X(ML_INFER,  "ML strategy dispatch incl. model inference — NESTED inside REBUILD (attribution sub-bracket, NOT additive with it); cost law ~550ns/boosting-round (3-class) + ~10us fixed per predict, x arms (2026-08-22); the bracket that makes tree-count bloat a visible regression instead of a buried surprise")

//------------------------------------------------------------------------------
// [SECTION]_[Auto-generated enum values + count sentinel.]
//------------------------------------------------------------------------------
enum SpSection {
#define X(NAME, DOC) SP_SECTION_##NAME,
    FOREACH_SP_SECTION(X)
#undef X
    SP_SECTION_COUNT  // sentinel; sized for slow_path_breakdown[SP_SECTION_COUNT]
};

//------------------------------------------------------------------------------
// [SECTION]_[Auto-generated documentation strings (for tooltips, log headers, etc.).]
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
// s5-F13 — WHY THE ASSERT ABOVE DID NOT SAVE US, recorded so the next reader
// does not trust it for more than it says. It is a FLOOR (`>= 5`), so it stayed
// green when ML_INFER made the count 6 while the TUISnapshot publish arrays
// remained hardcoded `[5]`. The 6th section was measured on every node every
// cycle and never published — a Class-51 vacuously-green guard sitting directly
// on top of a Class-58 complement blindness.
//
// The real fix is not a tighter number here: it is that every consumer now
// DERIVES its extent from SP_SECTION_COUNT (see TUISnapshot's
// sp_breakdown_*_ns), so there is no second number left to disagree with this
// one. A count assert can only pin what someone remembered to compare;
// derivation removes the comparison.
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Order matters — sections are sampled IN this order each cycle. Inserting
// in the middle SHIFTS subsequent section indices (slow_path_breakdown[]
// array elements re-map). For backward compat with prior snapshot files
// that may persist per-section breakdown counts, append new sections at
// the END of the registry.
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
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
//======================================================================
// [DERIVED]   (tool-refreshed — ROW_COUNT/CONSUMERS generators land with the drift-gate generalization; empty skeleton is correct, D-327)
//======================================================================
// [END_REGISTRY]_[FOREACH_SP_SECTION]
//======================================================================

}  // namespace tt

#endif  // SP_SECTION_REGISTRY_HPP
