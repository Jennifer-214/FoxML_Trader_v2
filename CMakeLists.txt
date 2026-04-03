cmake_minimum_required(VERSION 3.14)
project(tick_trader CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#===============================================================================
# engine binary — ANSI TUI (zero deps, diff-based rendering)
#===============================================================================
add_executable(engine main.cpp)
target_compile_options(engine PRIVATE -O3 -march=native -funroll-loops -flto)
target_compile_definitions(engine PRIVATE MULTICORE_TUI USE_ANSI_TUI LICENSE_BYPASS)
target_link_libraries(engine PRIVATE ssl crypto pthread)

#===============================================================================
# build options
#===============================================================================
option(LATENCY_PROFILING "Enable RDTSCP latency profiling in TUI" OFF)
option(LATENCY_LITE "Lite profiling: 2 rdtscp instead of 4" OFF)
option(LATENCY_BENCH "Headless profiling to stderr" OFF)
option(USE_NATIVE_128 "FPN<64> forwards to FP64 __uint128_t" ON)
option(BUSY_POLL "Spin-poll instead of sleeping in poll() (experimental)" OFF)
option(USE_XGBOOST "Link XGBoost C API for ML inference" OFF)
option(USE_LIGHTGBM "Link LightGBM C API for ML inference" OFF)

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
    target_compile_options(engine_gui PRIVATE -O3 -march=native -funroll-loops -flto)
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
    if(USE_NATIVE_128)
        target_compile_definitions(engine_gui PRIVATE USE_NATIVE_128)
    endif()
    if(BUSY_POLL)
        target_compile_definitions(engine_gui PRIVATE BUSY_POLL)
    endif()
endif()

#===============================================================================
# foxml_suite — backtesting + ML training workstation
#===============================================================================
if(USE_IMGUI_GUI)
    add_executable(foxml_suite foxml_suite.cpp)
    target_compile_options(foxml_suite PRIVATE -O3 -march=native -funroll-loops -flto)
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
        pthread
    )
    if(USE_NATIVE_128)
        target_compile_definitions(foxml_suite PRIVATE USE_NATIVE_128)
    endif()
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
# tests
#===============================================================================
add_executable(controller_test tests/controller_test.cpp)
target_compile_options(controller_test PRIVATE -O3 -march=native -funroll-loops -flto)
target_include_directories(controller_test PRIVATE ${CMAKE_SOURCE_DIR}/..)

enable_testing()
add_test(NAME controller_test COMMAND controller_test)
