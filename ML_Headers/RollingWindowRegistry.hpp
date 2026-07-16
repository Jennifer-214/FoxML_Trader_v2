// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/RollingWindowRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the rolling-window cohort registry (v5.15.5.B.6) — first TEMPLATE-PARAMETERIZED cohort (each row carries a W); field decl + init auto-flow, consumers stay semantic]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_ROLLING_WINDOW]
//======================================================================================================
// X-macro registry for the per-cadence RollingStats cohort on NodeSlowState.
// Closes a small Class-18 mirror: the 4 windows (short/long/medium/baseline)
// each get explicit field declarations + init calls + reset-path entries
// across multiple files. Adding a 5th window (e.g., `rolling_micro` for
// sub-second horizon experimentation) required touching:
//   1. NodeSlowState struct field declaration
//   2. NodeSlowState_Init RollingStats_Init call
//   3. (Possibly) reset paths in EngineSharded.hpp / BacktestSharded.hpp
//   4. Per-window consumer references (semantic — stay manual)
//
// Now it's ONE row here; sites #1-3 auto-flow via X-macro expansion.
// Per-window CONSUMER sites (Regime_ComputeSignals, MeanReversion_BuildParameters,
// etc.) keep explicit references because they semantically depend on a
// specific W parameter (e.g., MeanReversion uses rolling_long for slope
// inference; SimpleDip uses rolling_long for recent_high tracking; the
// dispatch isn't uniform across W).
//
// First TEMPLATE-PARAMETERIZED COHORT registry application — distinct from
// FOREACH_<DOMAIN>_CFG_FLAG (boolean-bitmap variant) and
// FOREACH_SESSION_PHASE (float-cohort variant) in that each entry carries
// a TEMPLATE PARAMETER (the window size W). RollingStats<F, W> instantiates
// distinct types per row; the X-macro walks across template instantiations.
//
// Cross-references:
//   the X-macro registry pattern + structural-fix-preferred gradient
//   DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md
//   ML_Headers/RollingStats.hpp (RollingStats<F, W> with alignas(64) head)
//======================================================================================================
#ifndef ROLLING_WINDOW_REGISTRY_HPP
#define ROLLING_WINDOW_REGISTRY_HPP

namespace tt {

//======================================================================
// [REGISTRY]_[FOREACH_ROLLING_WINDOW]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [FRAMEWORK_DISCIPLINE]]
// [REFERENCE]_[DESIGN_SPEC]_[x-macro-registry-with-presence-dispatch]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[4 windows (short 128 / long 512 / medium 256 / baseline 1024) — row order IS struct layout order]
// [COLUMN]_[name]_[lower_snake suffix -> rolling_<name> field on NodeSlowState]
// [COLUMN]_[window_size]_[compile-time W -> RollingStats<F, W> instantiation]
//======================================================================
// [CODE]
//======================================================================
// Tuple: X(name, window_size)
//   name        — lower_snake_case suffix; produces `rolling_<name>` field
//                 name on NodeSlowState
//   window_size — uint compile-time constant; produces RollingStats<F, W>
//                 template instantiation
//
// Order matters — struct field layout follows this order. Don't reorder
// without checking that no caller's offsetof/persistence assumes a specific
// layout (verified safe for current 4 entries at v5.15.5.B.6 ship via
// safety greps; ShardedSnapshotPersist is field-by-field not memcpy).
//
// Adding a 5th window: append one row. Field decl + init call auto-flow;
// add per-window consumer references manually where the semantic fit is
// clear (e.g., rolling_micro for sub-second metrics → add reference in
// the consumer that needs it).
//------------------------------------------------------------------------------
#define FOREACH_ROLLING_WINDOW(X) \
    X(short,    128)              \
    X(long,     512)              \
    X(medium,   256)              \
    X(baseline, 1024)
//======================================================================
// [END_CODE]
//======================================================================
// [END_REGISTRY]_[FOREACH_ROLLING_WINDOW]
//======================================================================

}  // namespace tt

#endif  // ROLLING_WINDOW_REGISTRY_HPP
