cmake_minimum_required(VERSION 3.14)
project(tick_trader CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Every build dir emits its REAL compile_commands.json at configure time (TD-257: the 1:1 flag
# source — tools/compile_command.py + the plugin's asm/layout surfaces resolve invocations from
# the db of the build actually being shipped, never from editor-supplied or hand-assembled flags).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#===============================================================================
# engine binary — ANSI TUI (zero deps, diff-based rendering)
#===============================================================================
add_executable(engine main.cpp)
# -g: debug info ONLY — zero codegen change (GCC guarantee; identical instructions). Feeds the
# 1:1 asm sidecars' line interleave (objdump -l reads DWARF from the SHIPPED binary — the
# source↔shipped-asm sync + inline attribution + per-line cost the plugin renders). Strippable
# for deploy; never affects -O3/-flto output.
target_compile_options(engine PRIVATE -O3 -march=native -funroll-loops -flto -g -Werror=float-conversion)  # E.1.2.C leg 0 — PARTIAL guard for the fan-shift class (type-crossing positional re-maps; arity de-defaulting is the full closure). Measured 0 pre-existing hits.
target_compile_definitions(engine PRIVATE MULTICORE_TUI USE_ANSI_TUI LICENSE_BYPASS)
target_link_libraries(engine PRIVATE ssl crypto pthread)

#===============================================================================
# build options
#===============================================================================
option(LATENCY_PROFILING "Enable RDTSCP latency profiling in TUI" OFF)
option(LATENCY_LITE "Lite profiling: 2 rdtscp instead of 4" OFF)
option(LATENCY_BENCH "Headless profiling to stderr" OFF)
option(BUSY_POLL "Spin-poll instead of sleeping in poll() (experimental)" OFF)
option(USE_XGBOOST "Link XGBoost C API for ML inference" OFF)
option(USE_LIGHTGBM "Link LightGBM C API for ML inference" OFF)

# v5.11.0.D — Profile-Guided Optimization (PGO).
# GCC-only orchestration. Use via `./build.sh pgo`:
#   1. Build with USE_PGO_GENERATE=ON  → instrumented binary
#   2. Run a representative workload   → emits .gcda profile data
#   3. Build with USE_PGO_USE=ON       → consumes profile, optimizes hot paths
# 2-8% latency improvement typical on the engine's hot path.
option(USE_PGO_GENERATE "Build with -fprofile-generate (instrumented run)" OFF)
option(USE_PGO_USE "Build with -fprofile-use (consumes .gcda profile)" OFF)
set(PGO_PROFILE_DIR "${CMAKE_BINARY_DIR}/pgo_profile" CACHE PATH "Where .gcda files live")

function(apply_pgo_flags target)
    # PGO orchestration is GCC-specific (-fprofile-generate / -fprofile-use
    # syntax). Clang LTO PGO uses -fprofile-instr-generate / -fprofile-instr-use
    # with different filename conventions. Pin to GCC for now; revisit if
    # operator switches toolchains. Silently no-op for non-GCC compilers.
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        return()
    endif()
    if(USE_PGO_GENERATE)
        target_compile_options(${target} PRIVATE -fprofile-generate=${PGO_PROFILE_DIR})
        target_link_options(${target} PRIVATE -fprofile-generate=${PGO_PROFILE_DIR})
    elseif(USE_PGO_USE)
        # Note: NOT using -fprofile-correction. We want fail-fast on stale
        # profile data — operator should always train on the same SHA. If
        # a future workflow legitimately needs to ignore mismatches (e.g.
        # cross-version PGO reuse), document the rationale before enabling.
        target_compile_options(${target} PRIVATE -fprofile-use=${PGO_PROFILE_DIR})
        target_link_options(${target} PRIVATE -fprofile-use=${PGO_PROFILE_DIR})
    endif()
endfunction()

if(LATENCY_PROFILING)
    target_compile_definitions(engine PRIVATE LATENCY_PROFILING)
endif()
if(LATENCY_LITE)
    target_compile_definitions(engine PRIVATE LATENCY_PROFILING LATENCY_LITE)
endif()
if(LATENCY_BENCH)
    target_compile_definitions(engine PRIVATE LATENCY_PROFILING LATENCY_BENCH)
endif()
if(BUSY_POLL)
    target_compile_definitions(engine PRIVATE BUSY_POLL)
endif()

# ML inference backends (applied to engine, engine_gui added below if built)
if(USE_XGBOOST)
    find_package(xgboost REQUIRED)
    target_compile_definitions(engine PRIVATE USE_XGBOOST)
    target_link_libraries(engine PRIVATE xgboost::xgboost)
endif()
if(USE_LIGHTGBM)
    find_library(LIGHTGBM_LIB _lightgbm REQUIRED)
    target_compile_definitions(engine PRIVATE USE_LIGHTGBM)
    target_link_libraries(engine PRIVATE ${LIGHTGBM_LIB})
endif()

# v5.11.0.D — Apply PGO flags to engine (no-op when neither GENERATE
# nor USE option is on, no-op for non-GCC compilers).
apply_pgo_flags(engine)

#===============================================================================
# engine_gui binary — Dear ImGui + implot (SDL2 + OpenGL3)
#===============================================================================
option(USE_IMGUI_GUI "Build engine_gui with Dear ImGui + implot (requires SDL2, OpenGL)" ON)

if(USE_IMGUI_GUI)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    find_package(OpenGL REQUIRED)

    # imgui core sources
    set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)
    set(IMGUI_SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )

    # implot sources
    set(IMPLOT_DIR ${CMAKE_SOURCE_DIR}/vendor/implot)
    set(IMPLOT_SOURCES
        ${IMPLOT_DIR}/implot.cpp
        ${IMPLOT_DIR}/implot_items.cpp
    )

    # compile vendored libs as a static lib without LTO (fast compile)
    add_library(imgui_lib STATIC ${IMGUI_SOURCES} ${IMPLOT_SOURCES})
    target_compile_options(imgui_lib PRIVATE -O2)
    target_include_directories(imgui_lib PUBLIC
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${IMPLOT_DIR}
        ${SDL2_INCLUDE_DIRS}
    )

    add_executable(engine_gui main.cpp)
    target_compile_options(engine_gui PRIVATE -O3 -march=native -funroll-loops -flto -g)
    target_compile_definitions(engine_gui PRIVATE
        MULTICORE_TUI USE_IMGUI_GUI LICENSE_BYPASS
    )
    target_include_directories(engine_gui PRIVATE
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${IMPLOT_DIR}
        ${SDL2_INCLUDE_DIRS}
    )
    target_link_libraries(engine_gui PRIVATE
        imgui_lib
        ${SDL2_LIBRARIES}
        OpenGL::GL
        ssl crypto pthread
    )

    # propagate profiling options to GUI build
    if(LATENCY_PROFILING)
        target_compile_definitions(engine_gui PRIVATE LATENCY_PROFILING)
    endif()
    if(LATENCY_LITE)
        target_compile_definitions(engine_gui PRIVATE LATENCY_PROFILING LATENCY_LITE)
    endif()
    if(BUSY_POLL)
        target_compile_definitions(engine_gui PRIVATE BUSY_POLL)
    endif()
    if(USE_XGBOOST)
        target_compile_definitions(engine_gui PRIVATE USE_XGBOOST)
        target_link_libraries(engine_gui PRIVATE xgboost::xgboost)
    endif()
    if(USE_LIGHTGBM)
        target_compile_definitions(engine_gui PRIVATE USE_LIGHTGBM)
        target_link_libraries(engine_gui PRIVATE ${LIGHTGBM_LIB})
    endif()

    # v5.11.0.D — Apply PGO to engine_gui (operator's typical
    # profile-collection target since it has the full panel surface).
    apply_pgo_flags(engine_gui)
endif()

#===============================================================================
# foxml_suite — backtesting + ML training workstation
#===============================================================================
if(USE_IMGUI_GUI)
    add_executable(foxml_suite foxml_suite.cpp)
    target_compile_options(foxml_suite PRIVATE -O3 -march=native -funroll-loops -flto -g)
    target_compile_definitions(foxml_suite PRIVATE
        FOXML_SUITE LICENSE_BYPASS MULTICORE_TUI
    )
    target_include_directories(foxml_suite PRIVATE
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${IMPLOT_DIR}
        ${SDL2_INCLUDE_DIRS}
    )
    target_link_libraries(foxml_suite PRIVATE
        imgui_lib
        ${SDL2_LIBRARIES}
        OpenGL::GL
        ssl crypto pthread
        gomp  # v5.11.44 hotfix — omp_set_num_threads + omp_set_dynamic in
              # mh_per_horizon_parallel_worker (caps libgomp thread pool
              # per-pthread to prevent OpenMP team collisions across
              # parallel multi-horizon workers running XGBoost)
    )
    if(USE_XGBOOST)
        target_compile_definitions(foxml_suite PRIVATE USE_XGBOOST)
        target_link_libraries(foxml_suite PRIVATE xgboost::xgboost)
    endif()
    if(USE_LIGHTGBM)
        target_compile_definitions(foxml_suite PRIVATE USE_LIGHTGBM)
        target_link_libraries(foxml_suite PRIVATE ${LIGHTGBM_LIB})
    endif()
endif()

#===============================================================================
# tests  (private: tests/ is gitignored for the code-only public surface)
#===============================================================================
# Skip the dev/test targets on a public clone where tests/ is absent; build them
# locally (the dir is present, symlinked from the private workspace).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests)
# LANDMINE (symlink include resolution): tests/ + tools/ are symlinks into the private
# workspace. A test/tool file's "../X" engine include resolves ".." against the symlink's
# CANONICAL dir (the workspace), not the engine, so e.g. "../DataStream/MockGenerator.hpp"
# misses. Anchor quote-include search on a REAL engine subdir: "../X" then resolves as
# CoreFrameworks/../X == <engine>/X. Engine/gui/suite targets above are unaffected (defined
# before this line; they use no "../" engine includes). See DOCS/LANDMINES.md.
include_directories(${CMAKE_SOURCE_DIR}/CoreFrameworks)
add_executable(controller_test tests/controller_test.cpp)
target_compile_options(controller_test PRIVATE -O3 -march=native -funroll-loops -flto -g -Werror=float-conversion)  # E.1.2.C leg 0 — same class guard; measured 0 hits
target_include_directories(controller_test PRIVATE ${CMAKE_SOURCE_DIR}/..)
# Phase 5d regression tests pull in Backtest/BacktestEngine.hpp for BacktestResults
# helpers, XGBoost weight helpers, and LabelType_* helpers. BacktestEngine.hpp
# unconditionally references TUISnapshot, which is only defined under
# MULTICORE_TUI. Match the production engine target.
target_compile_definitions(controller_test PRIVATE MULTICORE_TUI)
# (native-FP pin REMOVED at Ship-B P5 — USE_NATIVE_128 absorbed; one FP path only.)
find_package(Threads REQUIRED)
target_link_libraries(controller_test PRIVATE Threads::Threads ssl crypto)

# Phase 8a c7 — DepthRecorder test binary. Separate from controller_test
# because the recorder writes to disk and tests use tempdir fixtures. Pulls
# in DataStream/DepthRecorder.hpp which transitively includes BinanceDepth.hpp;
# no MULTICORE_TUI needed (no TUISnapshot reference path).
add_executable(depth_recorder_test tests/depth_recorder_test.cpp)
target_compile_options(depth_recorder_test PRIVATE -O2 -march=native)
target_include_directories(depth_recorder_test PRIVATE ${CMAKE_SOURCE_DIR}/..)
target_link_libraries(depth_recorder_test PRIVATE ssl crypto pthread)
# (F-057 native-FP-path pin REMOVED at Ship-B P5 — USE_NATIVE_128 absorbed; one FP path only.)

endif()  # tests/ present

# v5.11.14 — standalone CLI that diffs two FeatureStandardizer .scaler
# binaries per-feature. Reports |Δmean|, |Δstddev|, and percent change.
# Linked against ssl + crypto for the SHA-256 verifier the loader runs
# on each sidecar. No MULTICORE_TUI / no test framework — pure CLI tool.
# compare_scalers is a private dev tool (tools/ gitignored); skip on a code-only public clone
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/compare_scalers.cpp)
add_executable(compare_scalers tools/compare_scalers.cpp)
target_compile_options(compare_scalers PRIVATE -O2 -march=native)
target_include_directories(compare_scalers PRIVATE ${CMAKE_SOURCE_DIR}/..)
target_link_libraries(compare_scalers PRIVATE ssl crypto)
endif()

# v5.15.5.F.4d.1.B.3 Phase L REVERTED — stamp_model_cli target REMOVED. Operator workflow shifts to
# foxml_suite GUI auto-stamp via Backtest_RunFullValidation → Stamp_AssembleAndEmit (in-process).
# DESIGN_SPEC `framework-driven-cli-binary-pattern.md` retained as Stage 2 DRAFT; first canonical
# queued for v5.16+ cmdline-invocable training per `plans/_future/2026-05-12-decoupling-endgoal-roadmap.md`.

enable_testing()
add_test(NAME controller_test COMMAND controller_test)
add_test(NAME depth_recorder_test COMMAND depth_recorder_test)
