# cmake/FormatGuard.cmake — a printf-family argument its specifier does not take is a compile ERROR.
#
# The class (2026-09-29; Class 67): a 16-byte Money / FPN_Binary<F> fed to %f / %g is undefined
# behaviour — the struct eats the argument registers, so every value on the line prints garbage and
# nothing crashes to say so. Nothing enabled -Wformat, so it compiled silently. Measured before this went
# on, by a REAL compile of every first-party target (a -fsyntax-only pass misses the optimizer-driven
# format warnings): 6 type-mismatch hits in 2 sites, all real — DataStream/BinanceOrderAPI.hpp's
# "[REST] filters" line (the live exchange-filter report) and the suite's Past Runs stamp view — fixed
# in the same commit, 0 left; format-overflow 0; format-truncation 1, a false positive (below).
#
# A printf-style WRAPPER is visible to the guard only if it declares __attribute__((format(printf, …))):
# Health_Log, Health_LogCriticalRateLimited, LabeledValue, ab_printf, tt::StderrLog (the drift walker's
# log_fn) and BacktestPanels' stats row() lambda do (ImGui::Text does via IM_FMTARGS). A new wrapper must
# carry it too, or calls through it are unchecked — and a variadic TEMPLATE forwarding to printf is
# invisible to the check, so a wrapper takes C varargs.
#
# -Wno-format-truncation: -Werror=format would also make GCC's snprintf-MAY-truncate heuristic an
# error. A bounded snprintf that truncates is memory-safe (the class here is TYPE mismatch), the
# heuristic false-positives on composed paths, and the build never enabled it before — so it stays off.
#
# Three parts, all at configure:
#   FOXML_FORMAT_GUARD            the flags; every first-party EXECUTABLE adds them to its
#                                 target_compile_options (vendor libraries keep their own flags).
#   the teeth                     a wide struct fed to %f must FAIL to compile under the flags, and the
#                                 same program with a double must compile (positive control) — else the
#                                 guard proves nothing on this compiler and configure stops.
#   foxml_check_compile_guards()  call LAST: every executable target must carry the flags, so a new
#                                 target cannot silently skip the guard (the result guard's too — below).
# Sister to -Werror=float-conversion and cmake/NoOpenMPRuntime.cmake (which proves its own matcher too).
#
# THE RESULT GUARD (E.1.3 MP-6 step 10.7, 2026-10-03) — the module's second guard, same three parts. A function whose
# failure is a returned STATUS is [[nodiscard]] — on the function, or on the status TYPE (`enum [[nodiscard]]
# BacktestRunStatus` binds every function returning one) — and discarding that status is a compile ERROR. The class
# (Class 62's value-return form): the label pass's abort (-1) was dropped at every caller, and Class 62's own guard
# (check_partial_output_struct.py) matches a bare `return;` only, so a status-returning function had none. It does
# not see a status stored and never read — Class 62's review rule still owns that. Measured before it went on: no
# first-party call discarded a checked result (0 -Wunused-result warnings across the test and gui lanes), and glibc's
# __wur marks are inert here (no default _FORTIFY_SOURCE), so it binds only what the code marks.
#
# Every first-party executable adds ${FOXML_COMPILE_GUARDS} (both guards' flags); foxml_check_compile_guards()
# refuses one that lacks any of them.

set(FOXML_FORMAT_GUARD -Werror=format -Werror=format-security -Wno-format-truncation)
list(JOIN FOXML_FORMAT_GUARD " " _fg_flags)

set(_fg_dir "${CMAKE_BINARY_DIR}/format_guard_teeth")
file(WRITE "${_fg_dir}/bad.cpp"
    "#include <cstdio>\nstruct Wide { long long a, b; };\n"
    "int main() { Wide w{}; std::printf(\"%f\\n\", w); return 0; }\n")
file(WRITE "${_fg_dir}/good.cpp"
    "#include <cstdio>\nint main() { std::printf(\"%f\\n\", 1.0); return 0; }\n")
try_compile(_fg_good_compiles "${_fg_dir}/good" SOURCES "${_fg_dir}/good.cpp"
            COMPILE_DEFINITIONS ${FOXML_FORMAT_GUARD})
try_compile(_fg_bad_compiles "${_fg_dir}/bad" SOURCES "${_fg_dir}/bad.cpp"
            COMPILE_DEFINITIONS ${FOXML_FORMAT_GUARD})
if(NOT _fg_good_compiles)
    message(FATAL_ERROR "[format-guard] the positive control (a double to %f) did not compile under "
                        "${_fg_flags} — the teeth cannot tell a refusal from a broken harness.")
endif()
if(_fg_bad_compiles)
    message(FATAL_ERROR "[format-guard] VACUOUS: a 16-byte struct fed to %f compiled under "
                        "${_fg_flags} — this compiler does not enforce the guard.")
endif()
message(STATUS "[format-guard] ${_fg_flags}: a wide struct to %f is refused, a double compiles")

set(FOXML_RESULT_GUARD -Werror=unused-result)
list(JOIN FOXML_RESULT_GUARD " " _rg_flags)
set(_rg_dir "${CMAKE_BINARY_DIR}/result_guard_teeth")
file(WRITE "${_rg_dir}/bad.cpp"
    "[[nodiscard]] static int status() { return -1; }\n"
    "int main() { status(); return 0; }\n")
file(WRITE "${_rg_dir}/good.cpp"
    "[[nodiscard]] static int status() { return -1; }\n"
    "int main() { return status() < 0 ? 1 : 0; }\n")
try_compile(_rg_good_compiles "${_rg_dir}/good" SOURCES "${_rg_dir}/good.cpp"
            COMPILE_DEFINITIONS ${FOXML_RESULT_GUARD})
try_compile(_rg_bad_compiles "${_rg_dir}/bad" SOURCES "${_rg_dir}/bad.cpp"
            COMPILE_DEFINITIONS ${FOXML_RESULT_GUARD})
if(NOT _rg_good_compiles)
    message(FATAL_ERROR "[result-guard] the positive control (a [[nodiscard]] status that is read) did not compile "
                        "under ${_rg_flags} — the teeth cannot tell a refusal from a broken harness.")
endif()
if(_rg_bad_compiles)
    message(FATAL_ERROR "[result-guard] VACUOUS: a discarded [[nodiscard]] status compiled under ${_rg_flags} — "
                        "this compiler does not enforce the guard.")
endif()
message(STATUS "[result-guard] ${_rg_flags}: a discarded [[nodiscard]] status is refused, a read one compiles")

set(FOXML_COMPILE_GUARDS ${FOXML_FORMAT_GUARD} ${FOXML_RESULT_GUARD})

function(foxml_check_compile_guards)
    get_property(_targets DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(NOT _type STREQUAL "EXECUTABLE")
            continue()
        endif()
        get_target_property(_opts ${_t} COMPILE_OPTIONS)
        foreach(_flag IN LISTS FOXML_COMPILE_GUARDS)
            if(NOT _flag IN_LIST _opts)
                message(FATAL_ERROR "[compile-guards] executable '${_t}' lacks ${_flag} — add "
                                    "\${FOXML_COMPILE_GUARDS} to its target_compile_options (cmake/FormatGuard.cmake).")
            endif()
        endforeach()
    endforeach()
endfunction()
