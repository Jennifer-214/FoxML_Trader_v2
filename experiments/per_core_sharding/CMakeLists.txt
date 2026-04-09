cmake_minimum_required(VERSION 3.16)
project(per_core_sharding_experiment CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization is critical for measurement validity — we want to measure the real
# cost of operations as the compiler would emit them, not debug-mode bloat.
# -fno-omit-frame-pointer is for perf integration if we add it later.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -fno-omit-frame-pointer -Wall -Wextra")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Phase 12: sanitizer build options. Use a separate build directory for each
# (build_tsan, build_asan) to avoid mixing instrumented and uninstrumented
# objects. The benchmarks should NOT be run under sanitizers — the latency
# numbers will be meaningless. Functional + concurrent tests only.
#
#   cmake -B build_tsan -DTSAN=ON
#   cmake -B build_asan -DASAN=ON
option(TSAN "Build with ThreadSanitizer (-fsanitize=thread)" OFF)
option(ASAN "Build with AddressSanitizer (-fsanitize=address,undefined)" OFF)

if(TSAN)
    message(STATUS "Building with ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=thread)
endif()

if(ASAN)
    message(STATUS "Building with AddressSanitizer + UBSan enabled")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined)
endif()

# Repo root, for reusing FixedPoint/, Limits.hpp, etc. when we need them
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../..)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Phase 0 deliverable: smoke test that calibrates TSC and times an empty loop body.
# If this prints sane numbers (~1-2GHz TSC, ~10-50 cycles/op), the measurement
# foundation works and we can build the real harness on top.
add_executable(test_rdtsc test_rdtsc.cpp)
target_link_libraries(test_rdtsc PRIVATE pthread)

# Phase 01 deliverable: SPSCRing functional smoke test + single-thread latency.
# Validates the lock-free ring template before phase 02 (ExecutionCore) builds on it.
add_executable(test_spsc_ring test_spsc_ring.cpp)
target_link_libraries(test_spsc_ring PRIVATE pthread)

# Phase 02 deliverable: ExecutionCore state machine + per-tick latency measurement.
# Validates the branchless hot path against synthetic input.
add_executable(test_execution_core test_execution_core.cpp)
target_link_libraries(test_execution_core PRIVATE pthread)

# Phase 03 deliverable: side-by-side benchmark of all four hot-path scenarios.
# This is the decision gate — if C doesn't beat A by 10x and stay under 100ns p99,
# phases 04+ don't happen.
add_executable(bench_hot_path bench_hot_path.cpp)
target_link_libraries(bench_hot_path PRIVATE pthread)

# Floor measurement: bracket rdtsc once around N=1M ticks to amortize the
# rdtsc cost to ~0 and report the true per-tick work cost without Heisenberg.
# USE_NATIVE_128 routes FPN<64> comparators through the __uint128_t fast path
# (matches what the production engine binary uses).
add_executable(bench_batch_floor bench_batch_floor.cpp)
target_compile_definitions(bench_batch_floor PRIVATE USE_NATIVE_128)
target_link_libraries(bench_batch_floor PRIVATE pthread)

# Phase 04 deliverable: controller event loop functional tests.
# Validates EventLoopState, OnEvent, DrainEvents, RunController against
# synthetic event sequences across multiple cores.
add_executable(test_controller_event_loop test_controller_event_loop.cpp)
target_link_libraries(test_controller_event_loop PRIVATE pthread)

# Phase 05 deliverable: ParameterSlot triple-buffer functional + stress tests.
# Validates atomic parameter handoff under concurrent producer + consumer.
add_executable(test_parameter_slot test_parameter_slot.cpp)
target_link_libraries(test_parameter_slot PRIVATE pthread)

# Phase 06 deliverable: strategy → parameter pipeline functional tests.
# Validates SimpleDip_BuildParameters, the dispatcher, and the
# RebuildAll → Push → ParameterSlot_Read end-to-end flow.
add_executable(test_strategy_parameters test_strategy_parameters.cpp)
target_link_libraries(test_strategy_parameters PRIVATE pthread)

# Phase 08 deliverable: ShardedTradeLog CSV writer + EventLoop integration.
# Validates v3 row format, header dedup, entry/exit pairs, multi-core interleave.
add_executable(test_trade_log test_trade_log.cpp)
target_link_libraries(test_trade_log PRIVATE pthread)

# Phase 09 deliverable: kill switch refactor — per-core permission revoke.
# Validates trip on balance floor / drawdown, atomic permission clear,
# unpause restoring only assigned-strategy cores, active positions still exit.
add_executable(test_kill_switch test_kill_switch.cpp)
target_link_libraries(test_kill_switch PRIVATE pthread)

# Phase 10 deliverable: EventLoopAggregates adapter — flat money view that
# the existing TUI/GUI panels consume after phase 13 migration. Computes
# balance, equity, unrealized P&L, drawdown from per-core EventLoopState.
add_executable(test_event_loop_aggregates test_event_loop_aggregates.cpp)
target_link_libraries(test_event_loop_aggregates PRIVATE pthread)

# Phase 11 deliverable: ShardedBacktestDriver — single-threaded replay loop
# that drives EventLoopState + N execution cores against a tick stream.
# Validates determinism, multi-core fan-out, slow-path cadence, kill switch
# mid-run handling. Phase 13 plugs this into the production BacktestEngine.
add_executable(test_sharded_backtest test_sharded_backtest.cpp)
target_link_libraries(test_sharded_backtest PRIVATE pthread)

# Phase 12 deliverables: concurrent stress tests for the SPSC ring and the
# full execution-core pipeline. These are the multi-threaded tests deferred
# from phases 01 and 02 — meant to be run under TSan to validate the
# memory ordering claims, and under ASan as a memory-safety smoke check.
add_executable(test_spsc_concurrent test_spsc_concurrent.cpp)
target_link_libraries(test_spsc_concurrent PRIVATE pthread)

add_executable(test_execution_core_concurrent test_execution_core_concurrent.cpp)
target_link_libraries(test_execution_core_concurrent PRIVATE pthread)

# Phase 13 deliverable: head-to-head comparison between the LegacyReferenceDriver
# (single-thread, direct state) and the ShardedBacktestDriver (per-core, SPSC,
# seqlock, events). Validates that the sharded path produces structurally
# identical trade decisions to a reference implementation of the same gates.
# This is the architectural validation gate for the production migration.
add_executable(test_migration_head_to_head test_migration_head_to_head.cpp)
target_link_libraries(test_migration_head_to_head PRIVATE pthread)

# Production dispatch verification: drives the real Backtest_Run + the new
# BacktestSharded_Run on a real tick file in both modes and compares results.
# This is what we use to verify the dispatch wiring against actual data.
#
# MULTICORE_TUI + LICENSE_BYPASS match the production engine target so that
# TUISnapshot and the rest of the production headers compile. Without
# MULTICORE_TUI the TUISnapshot struct in EngineTUI.hpp is ifdef'd out.
add_executable(run_backtest_dispatch run_backtest_dispatch.cpp)
target_compile_definitions(run_backtest_dispatch PRIVATE MULTICORE_TUI LICENSE_BYPASS USE_ANSI_TUI)
target_link_libraries(run_backtest_dispatch PRIVATE pthread)

# Real apples-to-apples latency benchmark of the legacy production hot path
# vs the new per-core sharded hot path. This is the measurement that was
# blocked in phase 03 because of dependency complexity. With the production
# headers accessible from this worktree, we can finally instantiate
# PortfolioController and time it head to head against ExecutionCore_Tick.
add_executable(bench_production_vs_sharded bench_production_vs_sharded.cpp)
target_compile_definitions(bench_production_vs_sharded PRIVATE MULTICORE_TUI LICENSE_BYPASS USE_ANSI_TUI)
target_link_libraries(bench_production_vs_sharded PRIVATE pthread)

# OMS phase 01+02 functional tests. Validates the OrderManager state
# machine, the paper-mode short-circuit, the adapter callback flow, and
# the table-full / rejection paths via a synchronous MockAdapter that
# fires the OMS callback inline. No real threading — concurrent stress
# testing for the OMS lives in test_oms_concurrent.cpp under TSan.
add_executable(test_oms test_oms.cpp)
target_compile_definitions(test_oms PRIVATE USE_NATIVE_128)
target_link_libraries(test_oms PRIVATE pthread)

# OMS phase 02 concurrent stress test. Spawns a worker thread that mimics
# the BinanceAdapter's submission queue → callback flow, then a drainer-
# simulating thread that does Submit + Tick in a loop. Validates the SPSC
# contracts on both submission_queue (drainer→worker) and result_queue
# (worker→drainer-via-Tick) under realistic concurrency. Run under TSan
# to catch any unsynchronized access.
add_executable(test_oms_concurrent test_oms_concurrent.cpp)
target_compile_definitions(test_oms_concurrent PRIVATE USE_NATIVE_128)
target_link_libraries(test_oms_concurrent PRIVATE pthread)
