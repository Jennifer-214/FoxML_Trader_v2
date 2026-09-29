#======================================================================================================
# XGBoostPin.cmake — the ONE place the engine finds and links XGBoost (E.1.3 NA OMP-B, D-494)
#======================================================================================================
# Landmine 1 was a SIGSEGV inside libgomp's parallel-region setup when several training threads ran
# XGBoost at once. The fix is removal, not taming: XGBoost is built WITHOUT OpenMP, from a pinned
# revision, into a project-local prefix (tools/build_xgboost.sh reads the pin below and builds exactly
# that), and this module refuses any other XGBoost. The library is SHARED and found through the binaries'
# RUNPATH, which the loader searches before the system directories — so a later install of a stock
# (OpenMP) XGBoost under /usr/local cannot reach a built binary; only a deliberate LD_LIBRARY_PATH could.
# (A static link was built and measured first — D-494 sub-choice 1, amended: it put all of XGBoost's
# code in every binary and grew the gui lane's asm sidecars from 231 MB to 3.6 GB of code no source
# maps to, +~80 s per build.)
#
# Included by CMakeLists.txt only when USE_XGBOOST is ON. Every XGBoost-linking target calls
# foxml_link_xgboost(<target>) — one recipe, never a hand-written link line per target.
#======================================================================================================

# THE PIN — what the prefix must hold; tools/build_xgboost.sh reads these two lines.
set(FOXML_XGBOOST_SHA      "3284a0fff5f321beb29938a65e534bb487a8432e")   # XGBoost 3.3.0 (master snapshot, 2026-05-05)
set(FOXML_XGBOOST_DMLC_SHA "4baa84e627849e675a3f99c92990ef9c39e4269e")   # its dmlc-core submodule

# The prefix: FOXML_XGBOOST_PREFIX in the environment, else the per-user default the recipe installs
# into. Derived on every configure and never cached, so a pin bump can't leave a stale path behind.
string(SUBSTRING "${FOXML_XGBOOST_SHA}" 0 12 _foxml_xgb_short)
if(DEFINED ENV{FOXML_XGBOOST_PREFIX} AND NOT "$ENV{FOXML_XGBOOST_PREFIX}" STREQUAL "")
    set(FOXML_XGBOOST_PREFIX "$ENV{FOXML_XGBOOST_PREFIX}")
else()
    set(FOXML_XGBOOST_PREFIX "$ENV{HOME}/.local/opt/xgboost-${_foxml_xgb_short}-noomp")
endif()

set(_foxml_xgb_config_dir "${FOXML_XGBOOST_PREFIX}/lib/cmake/xgboost")
if(NOT EXISTS "${_foxml_xgb_config_dir}/xgboost-config.cmake")
    message(FATAL_ERROR
        "[xgboost] no pinned XGBoost at ${FOXML_XGBOOST_PREFIX} — build it once with tools/build_xgboost.sh "
        "(D-494; FOXML_XGBOOST_PREFIX in the environment overrides the location)")
endif()

# The prefix records the revision it was built from; a prefix holding anything else is refused.
set(_foxml_xgb_stamp_file "${FOXML_XGBOOST_PREFIX}/FOXML_XGBOOST_BUILD")
set(_foxml_xgb_stamp "")
if(EXISTS "${_foxml_xgb_stamp_file}")
    file(STRINGS "${_foxml_xgb_stamp_file}" _foxml_xgb_stamp REGEX "^xgboost_sha=")
endif()
if(NOT _foxml_xgb_stamp STREQUAL "xgboost_sha=${FOXML_XGBOOST_SHA}")
    message(FATAL_ERROR
        "[xgboost] ${FOXML_XGBOOST_PREFIX} does not hold the pinned revision (stamp: '${_foxml_xgb_stamp}', "
        "pin: ${FOXML_XGBOOST_SHA}) — rebuild it with tools/build_xgboost.sh")
endif()

# find_package trusts a cached xgboost_DIR and skips its search — a build dir configured before OMP-B
# carries /usr/local/lib/cmake/xgboost (the OpenMP build), so the cache is overwritten, never trusted.
set(xgboost_DIR "${_foxml_xgb_config_dir}" CACHE PATH "pinned by cmake/XGBoostPin.cmake (D-494)" FORCE)
find_package(xgboost REQUIRED CONFIG NO_DEFAULT_PATH PATHS "${FOXML_XGBOOST_PREFIX}")

# The refusal. The found package's config file records how the library was built, in this scope:
# USE_OPENMP must be OFF, and the build the pinned shared one. An undefined flag is refused too — a config
# that stopped recording it can't be checked, and unchecked is not clean.
if(NOT DEFINED USE_OPENMP OR USE_OPENMP)
    message(FATAL_ERROR
        "[xgboost] ${xgboost_DIR} was built WITH OpenMP (USE_OPENMP='${USE_OPENMP}') — Landmine 1; D-494 "
        "refuses it. Rebuild the pinned library with tools/build_xgboost.sh")
endif()
if(NOT DEFINED XGBOOST_BUILD_STATIC_LIB OR XGBOOST_BUILD_STATIC_LIB)
    message(FATAL_ERROR
        "[xgboost] ${xgboost_DIR} is not the pinned shared build (XGBOOST_BUILD_STATIC_LIB='${XGBOOST_BUILD_STATIC_LIB}') "
        "— a static link puts all of XGBoost in every binary (the gui lane's asm sidecars went 231 MB -> 3.6 GB; D-494). "
        "Rebuild with tools/build_xgboost.sh")
endif()
message(STATUS "[xgboost] pinned ${_foxml_xgb_short}, shared, no OpenMP: ${FOXML_XGBOOST_PREFIX}")

function(foxml_link_xgboost target)
    target_compile_definitions(${target} PRIVATE USE_XGBOOST)
    target_link_libraries(${target} PRIVATE xgboost::xgboost)
endfunction()
