cmake_minimum_required(VERSION 3.14)
project(tick_trader CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#===============================================================================
# engine binary — ANSI TUI (zero deps, diff-based rendering)
#===============================================================================
add_executable(engine main.cpp)
target_compile_options(engine PRIVATE -O3 -march=native -funroll-loops -flto)
target_compile_definitions(engine PRIVATE MULTICORE_TUI USE_ANSI_TUI)
target_link_libraries(engine PRIVATE ssl crypto pthread)

#===============================================================================
# build options
#===============================================================================
option(LATENCY_PROFILING "Enable RDTSCP latency profiling in TUI" OFF)
option(LATENCY_LITE "Lite profiling: 2 rdtscp instead of 4" OFF)
option(LATENCY_BENCH "Headless profiling to stderr" OFF)
option(USE_NATIVE_128 "FPN<64> forwards to FP64 __uint128_t (experimental)" OFF)
option(BUSY_POLL "Spin-poll instead of sleeping in poll() (experimental)" OFF)

if(LATENCY_PROFILING)
    target_compile_definitions(engine PRIVATE LATENCY_PROFILING)
endif()
if(LATENCY_LITE)
    target_compile_definitions(engine PRIVATE LATENCY_PROFILING LATENCY_LITE)
endif()
if(LATENCY_BENCH)
    target_compile_definitions(engine PRIVATE LATENCY_PROFILING LATENCY_BENCH)
endif()
if(USE_NATIVE_128)
    target_compile_definitions(engine PRIVATE USE_NATIVE_128)
endif()
if(BUSY_POLL)
    target_compile_definitions(engine PRIVATE BUSY_POLL)
endif()

#===============================================================================
# tests
#===============================================================================
add_executable(controller_test tests/controller_test.cpp)
target_compile_options(controller_test PRIVATE -O3 -march=native -funroll-loops -flto)
target_include_directories(controller_test PRIVATE ${CMAKE_SOURCE_DIR}/..)

enable_testing()
add_test(NAME controller_test COMMAND controller_test)
