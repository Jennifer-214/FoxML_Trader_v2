// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).

#ifndef DEBUG_LOG_HPP
#define DEBUG_LOG_HPP

//======================================================================================================
// [DEBUG LOG MACROS — v5.11.32]
//
// Compile-time-gated diagnostic logging for engine hot-path / slow-path
// code. The discipline:
//
//   1. Hot path (BG_Evaluate / SG_Evaluate / ExecutionCore_Tick) and
//      engine slow path (EventLoop_RebuildOneCore + per-poll cadence)
//      are LATENCY-BUDGETED. Hot path runs at 40-400ns p99; slow path
//      at ~150µs p99. A 5-10µs Health_Log call is 3-7% of the slow-path
//      budget — unacceptable EVEN when operator opts in to logging.
//
//   2. Therefore: NEW diagnostic logging on engine paths goes through
//      LOG_DEBUG_ENGINE / LOG_DEBUG_HOT macros that EVAPORATE at compile
//      time when FOXML_DEBUG_LOGS is undefined. Zero bytes in the
//      release binary. No branch, no string formatting, no register
//      pressure.
//
//   3. Existing Health_Log call sites in engine slow-path code
//      (ControllerEventLoop.hpp regime/drain/entry/exit/gate logs) are
//      OPERATOR-FEATURES — they're operator-gated by health_log_path
//      and surface trade events the operator wants visibility into.
//      Those stay as-is (Health_LogEnabled fast-path is ~1ns when off).
//
//   4. Suite-side code (Backtest_Run, WF, Train Model worker, GUI panel
//      handlers) is operator-initiated batch work with seconds-to-minutes
//      runtime. Log there with direct Health_Log(WARN/INFO/DEBUG, ...).
//      No compile-time macro needed; latency is not budgeted.
//
//   5. Engine boot / shutdown / model load is one-time work. Direct
//      Health_Log is fine.
//
// Build flags:
//
//   Default (release):    no FOXML_DEBUG_LOGS → engine debug logs are
//                         compile-time eliminated. This is the production
//                         build path. ./build.sh test|gui|suite|engine.
//
//   ./build.sh debug:     -DFOXML_DEBUG_LOGS=ON enables engine debug
//                         logs at runtime. Use this when reproducing a
//                         bug that needs hot/slow path tracing.
//
// Macro usage:
//
//   LOG_DEBUG_HOT("cat", core_id, "fmt %d %g", x, y)
//     -> Health_Log(HEALTH_DEBUG, "cat", core_id, "fmt %d %g", x, y) when
//        FOXML_DEBUG_LOGS defined; ((void)0) otherwise.
//
//   LOG_DEBUG_ENGINE alias for LOG_DEBUG_HOT — semantically the engine
//   slow path is the same latency budget for compile-time stripping
//   (the macro doesn't distinguish; the discipline is in the call site).
//
// Caller pattern (MUST evaluate args even when stripped — common bug):
//   When the macro is `((void)0)`, args are NOT evaluated. Don't put
//   side-effect-bearing expressions in macro arguments. Use plain
//   reads + literal format strings. Same discipline as `assert()`.
//======================================================================================================

#include "HealthLog.hpp"   // Health_Log + HEALTH_* level constants

#ifdef FOXML_DEBUG_LOGS

#define LOG_DEBUG_HOT(cat, core_id, ...)    \
    tt::Health_Log(tt::HEALTH_DEBUG, (cat), (core_id), __VA_ARGS__)

#define LOG_DEBUG_ENGINE(cat, core_id, ...) \
    tt::Health_Log(tt::HEALTH_DEBUG, (cat), (core_id), __VA_ARGS__)

#else

// Stripped at compile time. Args NOT evaluated — caller must keep
// side-effect-bearing expressions out of arguments (same discipline
// as `assert()` under NDEBUG).
#define LOG_DEBUG_HOT(cat, core_id, ...)    ((void)0)
#define LOG_DEBUG_ENGINE(cat, core_id, ...) ((void)0)

#endif

#endif // DEBUG_LOG_HPP
