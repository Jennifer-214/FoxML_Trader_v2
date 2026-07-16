// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/SystemInit.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[tiny main()-shared init helpers — every engine binary calls these at entry]
// [CONTAINS]
//   - [FUNCTION]_[engine_set_mxcsr_ftz_daz]
//======================================================================================================
// Keep this header tiny — it ships into engine, controller_test, parity_harness,
// and foxml_suite. No transitive deps allowed beyond system headers.
//
// Audit refs:
//   - DOCS/LATENCY_OPTIMIZATION_AUDIT.md Part 12.3 (FTZ/DAZ)
//   - DOCS/STRATEGY_AND_CODING_RULES.md §9 (Advanced System & Compiler Optimizations)
//======================================================================================================
#pragma once
#include <xmmintrin.h>  // _MM_SET_FLUSH_ZERO_MODE
#include <pmmintrin.h>  // _MM_SET_DENORMALS_ZERO_MODE

namespace tt {

//======================================================================
// [FUNCTION]_[engine_set_mxcsr_ftz_daz]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [DETERMINISM] [CRITICAL]]
// [REFERENCE]_[AUDIT]_[latency-optimization-part-12.3]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[flip MXCSR FTZ+DAZ at main() entry — kills 100x subnormal stalls + pins train<->serve subnormal parity]
//======================================================================
// [CODE]
//======================================================================
static inline void engine_set_mxcsr_ftz_daz() {
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.11.0.A — FTZ/DAZ MXCSR INIT.
//
// Configure CPU MXCSR to flush subnormal floating-point numbers to zero:
//   - FTZ (Flush-To-Zero): output operands that would be subnormal → 0
//   - DAZ (Denormals-Are-Zero): input operands that are subnormal treated as 0
//
// Subnormal stalls cost up to 100x FPU throughput (microcode trap). Triggered
// in any path that touches FP math: ML inference, EMA decay, FlowFeatures,
// RegimeSignals, scoring, etc.
//
// Required for sub-microsecond determinism. Must be called from EVERY binary's
// main() — engine, controller_test, parity_harness, foxml_suite — or
// train↔serve parity drifts in subnormal territory (snapshot tests in
// controller_test would silently diverge from engine runtime when feature
// values land below ~1e-308).
//
// Linux-only invariant: pthread_create inherits the parent thread's MXCSR,
// so calling once at main() entry covers all spawned slow-path threads.
// macOS / Windows have different inheritance — out of scope.
//======================================================================
// [END_FUNCTION]_[engine_set_mxcsr_ftz_daz]
//======================================================================

} // namespace tt
