#======================================================================================================
# NoOpenMPRuntime.cmake — no engine binary may load an OpenMP runtime (E.1.3 NA OMP-B, D-494)
#======================================================================================================
# Landmine 1 was a SIGSEGV inside libgomp's parallel-region setup when several training threads ran
# XGBoost at once; D-494 removes OpenMP from the build. This file keeps it removed, at link time, for every
# binary in every lane. One file, two roles, so the rule lives once:
#   include()d by CMakeLists.txt  -> proves its matcher on literal ldd lines (the configure fails if the
#                                    matcher broke), then defines foxml_refuse_openmp_runtime(<target>);
#   run by cmake -P (POST_BUILD)  -> `ldd` over the freshly linked binary; an OpenMP runtime anywhere in its
#                                    load closure fails the build AND removes the binary, so a failed build
#                                    never leaves a runnable one behind and the next build relinks it.
# ldd, not readelf -d: libgomp reached engine_gui, engine and controller_test only THROUGH libxgboost, and
# readelf -d on a binary lists its direct dependencies alone. ldd resolves the whole closure in trace mode —
# nothing is run, main() included.
# The runtimes: libgomp (GCC), libomp (LLVM), libiomp (Intel) — a basename that starts with one of them
# followed by '.', '-' or a digit. tests/controller_test_omp.hpp spells the same three for its in-process scan.
#======================================================================================================

# the matcher: a runtime's name as the LAST path component of a token (a directory named libgomp.d is not one)
function(_foxml_omp_runtime_in text out_var)
    string(REGEX MATCH "(^|[/ \t\n])(libgomp|libomp|libiomp)[.0-9-][^/ \t\n]*([ \t\n]|$)" _hit "${text}")
    string(STRIP "${_hit}" _hit)
    set(${out_var} "${_hit}" PARENT_SCOPE)
endfunction()

# The mode is chosen by the INPUT, not by how the file was reached: handed a binary, it checks it (the
# POST_BUILD always passes one); otherwise it proves its teeth and defines the function. A mode test on
# CMAKE_SCRIPT_MODE_FILE could fall through to the include branch and exit 0 without checking anything.
# FOXML_CHECK_ONLY=ON keeps a refused binary (to demonstrate the refusal on a real one without deleting it).
if(DEFINED FOXML_BIN)
    # check mode: cmake -DFOXML_BIN=<binary> [-DFOXML_CHECK_ONLY=ON] -P NoOpenMPRuntime.cmake
    if(NOT FOXML_BIN OR NOT EXISTS "${FOXML_BIN}")
        message(FATAL_ERROR "[no-openmp] no binary to check: '${FOXML_BIN}'")
    endif()
    find_program(_foxml_ldd ldd)
    if(NOT _foxml_ldd)
        message(FATAL_ERROR "[no-openmp] ldd not found — ${FOXML_BIN}'s load closure cannot be checked, and unchecked is not clean")
    endif()
    execute_process(COMMAND "${_foxml_ldd}" "${FOXML_BIN}"
                    OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "[no-openmp] ldd ${FOXML_BIN} failed (rc ${_rc}): ${_err}")
    endif()
    # positive control: ldd really listed a closure (libc is in every one of ours)
    if(NOT _out MATCHES "libc\\.so")
        message(FATAL_ERROR "[no-openmp] ldd listed no libc for ${FOXML_BIN} — refusing to call its closure clean")
    endif()
    _foxml_omp_runtime_in("${_out}" _hit)
    if(_hit)
        set(_fate "The binary was removed.")
        if(FOXML_CHECK_ONLY)
            set(_fate "The binary was kept (FOXML_CHECK_ONLY).")
        else()
            file(REMOVE "${FOXML_BIN}")
        endif()
        message(FATAL_ERROR "[no-openmp] ${FOXML_BIN} loads an OpenMP runtime (${_hit}) — Landmine 1; D-494 refuses it. "
                            "${_fate} Is the XGBoost it links the pinned one (tools/build_xgboost.sh)?")
    endif()
    message(STATUS "[no-openmp] ${FOXML_BIN}: no OpenMP runtime in its load closure")
    return()
endif()

# include mode — the matcher's teeth, both directions, on the shapes ldd prints
foreach(_line
        "\tlibgomp.so.1 => /usr/lib/libgomp.so.1 (0x00007f0000000000)"
        "\tlibgomp-a34b3233.so.1.0.0 => /site-packages/xgboost.libs/libgomp-a34b3233.so.1.0.0 (0x1)"
        "\tlibomp.so.5 => /usr/lib/libomp.so.5 (0x1)"
        "\tlibiomp5.so => /opt/intel/lib/libiomp5.so (0x1)")
    _foxml_omp_runtime_in("${_line}" _hit)
    if(NOT _hit)
        message(FATAL_ERROR "[no-openmp] matcher tooth failed: it missed the runtime line '${_line}'")
    endif()
endforeach()
foreach(_line
        "\tlibc.so.6 => /usr/lib/libc.so.6 (0x1)"
        "\tlibstdc++.so.6 => /usr/lib/libstdc++.so.6 (0x1)"
        "\tlibompl.so.17 => /usr/lib/libompl.so.17 (0x1)"
        "\tlibfoo.so => /tmp/libgomp.d/libfoo.so (0x1)")
    _foxml_omp_runtime_in("${_line}" _hit)
    if(_hit)
        message(FATAL_ERROR "[no-openmp] matcher tooth failed: it flagged the non-runtime line '${_line}' (${_hit})")
    endif()
endforeach()

function(foxml_refuse_openmp_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DFOXML_BIN=$<TARGET_FILE:${target}> -P ${CMAKE_CURRENT_FUNCTION_LIST_FILE}
        COMMENT "no-OpenMP check: ${target} (D-494)"
        VERBATIM)
endfunction()
