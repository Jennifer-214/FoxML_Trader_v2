#=======================================================================
# GenBuildCommit.cmake — emit the BUILD-TIME commit identity header.
#
# Run in script mode (`cmake -P`) from a custom target that is part of
# ALL, so it re-evaluates on EVERY build rather than once at configure.
#
# WHY build-time and not configure-time: a configure-time SHA is baked
# when the build dir is first created and then silently keeps asserting
# that commit across every later rebuild. A tag whose whole purpose is
# to answer "which commit is this binary?" must never be able to give a
# confidently wrong answer — that is the vacuously-green / false-
# assertion shape (Class 51). Re-deriving per build costs one git call.
#
# WHY the dirty marker: a binary built from a modified tree is NOT the
# commit it names. Landmine 24 (a stale editor buffer silently reverted
# committed work while every gate stayed green) is exactly the incident
# where "clean SHA, dirty tree" would have read as reassurance. The
# marker keeps the tag honest instead of merely precise.
#
# WHY copy_if_different: the header is included via Version.hpp, so a
# byte change rebuilds the tree. The SHA only moves at a commit and the
# dirty flag only flips at the first edit after one, so rebuilds stay
# rare — but an unconditional write would rebuild the world every build.
#
# Inputs:  SRC_DIR (repo root to interrogate), OUT_FILE (header path)
# Output:  OUT_FILE defining FOXML_BUILD_COMMIT / _SHA / _DIRTY
#=======================================================================

set(_sha "unknown")
set(_dirty 0)

find_package(Git QUIET)

if(GIT_FOUND AND EXISTS "${SRC_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE _sha_out
        ERROR_QUIET
        RESULT_VARIABLE _sha_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_sha_rc EQUAL 0 AND NOT _sha_out STREQUAL "")
        set(_sha "${_sha_out}")
    endif()

    # --porcelain covers tracked modifications + staged changes. Untracked
    # files are deliberately EXCLUDED (-uno): a scratch file in the tree does
    # not change what was compiled, and counting it would leave the tag
    # permanently dirty for every operator who keeps notes in the repo
    # (feedback_keep_operator_scratch_files_as_history).
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain -uno
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE _status_out
        ERROR_QUIET
        RESULT_VARIABLE _status_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_status_rc EQUAL 0 AND NOT _status_out STREQUAL "")
        set(_dirty 1)
    endif()
endif()

if(_dirty)
    set(_display "${_sha}+dirty")
else()
    set(_display "${_sha}")
endif()

set(_content
"#pragma once
//======================================================================
// GENERATED AT BUILD TIME by cmake/GenBuildCommit.cmake — DO NOT EDIT.
// DO NOT COMMIT: this header lives in the build dir, not the source tree.
//
// DISPLAY-ONLY build identity. NEVER wire-written: not part of
// BUILD_FLAGS_CANONICAL (ML_Headers/BuildFlags.hpp — its order-locked
// allowlist is what keeps build_flags_hash stable across builds), and
// never emitted into a stamp / fingerprint / HMAC body (H9). Same rule
// as RELEASE_VERSION_STRING: re-tagging must not perturb determinism.
//======================================================================
#define FOXML_BUILD_COMMIT       \"${_display}\"
#define FOXML_BUILD_COMMIT_SHA   \"${_sha}\"
#define FOXML_BUILD_COMMIT_DIRTY ${_dirty}
")

set(_tmp "${OUT_FILE}.tmp")
file(WRITE "${_tmp}" "${_content}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_tmp}" "${OUT_FILE}")
file(REMOVE "${_tmp}")
