// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ROLLING WINDOW REGISTRY — v5.15.5.B.6]
//======================================================================================================
// X-macro registry for the per-cadence RollingStats cohort on CoreSlowState.
// Closes a small Class-18 mirror: the 4 windows (short/long/medium/baseline)
// each get explicit field declarations + init calls + reset-path entries
// across multiple files. Adding a 5th window (e.g., `rolling_micro` for
// sub-second horizon experimentation) required touching:
//   1. CoreSlowState struct field declaration
//   2. CoreSlowState_Init RollingStats_Init call
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
//   CLAUDE.md item 13 (X-macro registry pattern)
//   CLAUDE.md item 19 (structural fix preferred)
//   DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md
//   ML_Headers/RollingStats.hpp (RollingStats<F, W> with alignas(64) head)
//======================================================================================================
#ifndef ROLLING_WINDOW_REGISTRY_HPP
#define ROLLING_WINDOW_REGISTRY_HPP

namespace tt {

//------------------------------------------------------------------------------
// Tuple: X(name, window_size)
//   name        — lower_snake_case suffix; produces `rolling_<name>` field
//                 name on CoreSlowState
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

}  // namespace tt

#endif  // ROLLING_WINDOW_REGISTRY_HPP
