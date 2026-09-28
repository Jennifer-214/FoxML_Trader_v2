// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/EngineCli.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the engine binary's command-line grammar — option + dispatch rows (X-macro registries) and ONE data-driven resolver over a row ARRAY, so the production table and the suite's fixture table run the same function]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_ENGINE_CLI_REFUSAL]
//   - [REGISTRY]_[FOREACH_ENGINE_CLI_DISPATCH]
//   - [REGISTRY]_[FOREACH_ENGINE_CLI_OPTION]
//   - [FUNCTION]_[EngineCli_ValidateTable]
//   - [FUNCTION]_[EngineCli_Resolve]
//   - [FUNCTION]_[EngineCli_PrintUsage]
//   - [FUNCTION]_[EngineCli_PrintRefusal]
// [REFERENCE]_[DESIGN_SPEC]_[framework-driven-cli-binary-pattern]
//======================================================================================================
//
// THE GRAMMAR (E.1.3 NA CFG-1b; the spec's "planned first engine instance" section):
//   engine [<cfg>]              run the engine (the DEFAULT dispatch — index 0 — when no mode flag is given)
//   engine --help               print the usage to stdout
// Later leaves add ROWS, not code: REPLAY's scoped family (--replay <tape> + --replay-pace max|tape|boundary …),
// PERSIST's maintenance verbs (--protective-state show|clear|init + --latch required under clear), E.2's
// --check-cfg, the roadmap's --dump-stamp-schema (no cfg). The suite's fixture table already exercises every one
// of those grammar classes against THIS resolver.
//
// THE RULES a row cannot bend: one spelling (`--name value`; `--name=value` refuses); `--` ends the options;
// a mode flag (dispatch != NONE) is at most one per invocation, so exclusivity is DERIVED, never listed pairwise;
// an ordinary option is legal only in its scope (a dispatch, optionally one sub-token of that dispatch's ENUM
// selector); a value never starts with '-' (a flag is never swallowed as a value); an empty value or an empty
// cfg path refuses. The flag spelled for a row is "--" + its name with every '_' written as '-'.
//
// Flag names are externally visible identifiers (scripts + units call them): they enroll in the H21 ledger at
// NA C1-ws's bless, and a retired flag is tombstoned, never renamed.

#pragma once

#include "CfgPaths.hpp"    // the run dispatch's default cfg
#include "ParseFast.hpp"   // tt::parse_uint64_checked
#include <cstdint>
#include <cstdio>
#include <cstring>

//======================================================================
// [SECTION]_[the grammar's vocabulary]
//----------------------------------------------------------------------
// Row-array indices, not globals: a dispatch "id" is an index into the dispatch array the resolver is given,
// so a fixture table can define its own dispatches and still run the production resolver.
//======================================================================
enum EngineCliValueKind : uint8_t {
    ENGINE_CLI_VALUE_NONE   = 0,   // a bare flag
    ENGINE_CLI_VALUE_STRING = 1,   // one non-empty argument (a path, an id, a note)
    ENGINE_CLI_VALUE_UINT   = 2,   // one unsigned decimal integer — digits only
    ENGINE_CLI_VALUE_ENUM   = 3,   // exactly one token of the row's value_set
};
enum EngineCliPresence : uint8_t {
    ENGINE_CLI_OPTIONAL = 0,
    ENGINE_CLI_REQUIRED = 1,       // must appear whenever the invocation resolves into the option's scope
};
enum EngineCliPositional : uint8_t {
    ENGINE_CLI_POSITIONAL_FORBIDDEN    = 0,
    ENGINE_CLI_POSITIONAL_OPTIONAL_ONE = 1,
    ENGINE_CLI_POSITIONAL_REQUIRED_ONE = 2,
};
inline constexpr uint8_t ENGINE_CLI_DISPATCH_NONE = 0xFF;   // an option row that selects no mode
inline constexpr uint8_t ENGINE_CLI_SCOPE_ANY     = 0xFE;   // an option legal in every mode
inline constexpr int     ENGINE_CLI_MAX_OPTIONS   = 32;     // per table — the resolver's per-row storage

// The scope column. Each expands to TWO initializers (the home dispatch, the sub-token), so an X body places
// `scope` directly inside a braced initializer and never forwards it to another macro.
#define ENGINE_CLI_SCOPE_ANY_DISPATCH             ENGINE_CLI_SCOPE_ANY, nullptr
#define ENGINE_CLI_SCOPE_IN(dispatch)             (uint8_t)(dispatch), nullptr
#define ENGINE_CLI_SCOPE_IN_TOKEN(dispatch, tok)  (uint8_t)(dispatch), (tok)

//======================================================================
// [STRUCT]_[EngineCliOption]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one option row — the FOREACH_ENGINE_CLI_OPTION columns, with the scope column expanded into its two fields; boot-time data, read by the resolver + the usage]
//======================================================================
// [CODE]
//======================================================================
struct EngineCliOption {
    const char* name;            // an identifier; the flag is "--" + name with every '_' written as '-'
    uint8_t     value_kind;      // EngineCliValueKind
    const char* value_set;       // ENUM only: "tok|tok|…" — exact, case-sensitive; nullptr otherwise
    uint8_t     dispatch;        // the mode this option SELECTS (an index), or ENGINE_CLI_DISPATCH_NONE
    uint8_t     scope_dispatch;  // an ordinary option's home mode (an index), or ENGINE_CLI_SCOPE_ANY
    const char* scope_token;     // nullptr = any; else the token of its home mode's ENUM selector it needs
    uint8_t     presence;        // EngineCliPresence
    const char* doc;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-09-28]
//----------------------------------------------------------------------
// [SIZE]_[56B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[EngineCliOption]
//======================================================================

//======================================================================
// [STRUCT]_[EngineCliDispatch]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one mode row — the FOREACH_ENGINE_CLI_DISPATCH columns; the mode's cfg-path policy + its default cfg]
//======================================================================
// [CODE]
//======================================================================
struct EngineCliDispatch {
    const char* name;                // for messages and the usage
    uint8_t     positional;          // EngineCliPositional — the cfg-path policy of this mode
    const char* default_positional;  // the cfg path when OPTIONAL and none is named (nullptr = none)
    const char* doc;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-09-28]
//----------------------------------------------------------------------
// [SIZE]_[32B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[EngineCliDispatch]
//======================================================================

//======================================================================
// [REGISTRY]_[FOREACH_ENGINE_CLI_REFUSAL]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[every way the resolver refuses an invocation — the result enum and its message come from one row each]
// [COLUMN]_[id]_[EngineCliResult enumerator]
// [COLUMN]_[message]_[operator-facing reason, printed with the offending token]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_ENGINE_CLI_REFUSAL(X)                                                               \
    X(ENGINE_CLI_REFUSE_UNKNOWN_OPTION,       "unknown option")                                            \
    X(ENGINE_CLI_REFUSE_EQUALS_FORM,          "'--name=value' is not accepted; write '--name value'")      \
    X(ENGINE_CLI_REFUSE_DUPLICATE_OPTION,     "option given twice")                                        \
    X(ENGINE_CLI_REFUSE_MISSING_VALUE,        "option needs a value (a value never starts with '-')")      \
    X(ENGINE_CLI_REFUSE_EMPTY_VALUE,          "empty value")                                               \
    X(ENGINE_CLI_REFUSE_VALUE_NOT_IN_SET,     "value is not one of the option's choices")                  \
    X(ENGINE_CLI_REFUSE_VALUE_NOT_UINT,       "value is not an unsigned integer")                          \
    X(ENGINE_CLI_REFUSE_SECOND_DISPATCH,      "a second mode was selected")                                \
    X(ENGINE_CLI_REFUSE_OUT_OF_SCOPE,         "option not valid in this mode")                             \
    X(ENGINE_CLI_REFUSE_MISSING_REQUIRED,     "required option missing")                                   \
    X(ENGINE_CLI_REFUSE_POSITIONAL_FORBIDDEN, "this mode takes no cfg path")                               \
    X(ENGINE_CLI_REFUSE_EXTRA_POSITIONAL,     "more than one cfg path")                                    \
    X(ENGINE_CLI_REFUSE_EMPTY_POSITIONAL,     "empty cfg path")                                            \
    X(ENGINE_CLI_REFUSE_MISSING_POSITIONAL,   "this mode needs a cfg path")                                \
    X(ENGINE_CLI_REFUSE_TABLE_INVALID,        "the command-line table itself is invalid")
//======================================================================
// [END_CODE]
//======================================================================
// [END_REGISTRY]_[FOREACH_ENGINE_CLI_REFUSAL]
//======================================================================

enum EngineCliResult : uint8_t {
    ENGINE_CLI_OK = 0,
#define X(id, msg) id,
    FOREACH_ENGINE_CLI_REFUSAL(X)
#undef X
    ENGINE_CLI_RESULT_COUNT
};

static inline const char* EngineCli_ResultMessage(uint8_t result) {
    static const char* const k_messages[] = {
        "ok",
#define X(id, msg) msg,
        FOREACH_ENGINE_CLI_REFUSAL(X)
#undef X
    };
    static_assert(sizeof(k_messages) / sizeof(k_messages[0]) == ENGINE_CLI_RESULT_COUNT,
                  "one message per EngineCliResult");
    return result < ENGINE_CLI_RESULT_COUNT ? k_messages[result] : "unknown result";
}

//======================================================================
// [REGISTRY]_[FOREACH_ENGINE_CLI_DISPATCH]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the engine binary's modes — row 0 is the default (no mode flag); every other row is selected by exactly one option]
// [COLUMN]_[id]_[EngineCliDispatchId enumerator = the row's index]
// [COLUMN]_[name]_[the mode's name in messages + the usage]
// [COLUMN]_[positional]_[EngineCliPositional — the mode's cfg-path policy]
// [COLUMN]_[default_positional]_[the cfg path an OPTIONAL positional falls back to, or nullptr]
// [COLUMN]_[doc]_[one-line usage description]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_ENGINE_CLI_DISPATCH(X)                                                                        \
    X(ENGINE_CLI_DISPATCH_RUN,  "run",  ENGINE_CLI_POSITIONAL_OPTIONAL_ONE, CFG_PATH_DEFAULT_ENGINE_CFG,        \
      "run the engine on the cfg file named")                                                                  \
    X(ENGINE_CLI_DISPATCH_HELP, "help", ENGINE_CLI_POSITIONAL_FORBIDDEN,    nullptr,                           \
      "print this usage to stdout and exit")
//======================================================================
// [END_CODE]
//======================================================================
// [END_REGISTRY]_[FOREACH_ENGINE_CLI_DISPATCH]
//======================================================================

enum EngineCliDispatchId : uint8_t {
#define X(id, name, positional, default_positional, doc) id,
    FOREACH_ENGINE_CLI_DISPATCH(X)
#undef X
    ENGINE_CLI_DISPATCH_COUNT
};

inline constexpr EngineCliDispatch g_engine_cli_dispatches[] = {
#define X(id, name, positional, default_positional, doc) { name, positional, default_positional, doc },
    FOREACH_ENGINE_CLI_DISPATCH(X)
#undef X
};

//======================================================================
// [REGISTRY]_[FOREACH_ENGINE_CLI_OPTION]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the engine binary's options — add an option = 1 row; the resolver, the usage and the fixture-proven rules follow]
// [COLUMN]_[name]_[identifier; the flag is "--" + name with '_' written as '-'; ENGINE_CLI_OPT_<name> is its row index]
// [COLUMN]_[value_kind]_[EngineCliValueKind]_[[ENGINE_CLI_VALUE_NONE] [ENGINE_CLI_VALUE_STRING] [ENGINE_CLI_VALUE_UINT] [ENGINE_CLI_VALUE_ENUM]]
// [COLUMN]_[value_set]_[ENUM choices "tok|tok", or nullptr]
// [COLUMN]_[dispatch]_[the mode it selects (EngineCliDispatchId), or ENGINE_CLI_DISPATCH_NONE]
// [COLUMN]_[scope]_[ENGINE_CLI_SCOPE_ANY_DISPATCH / ENGINE_CLI_SCOPE_IN(d) / ENGINE_CLI_SCOPE_IN_TOKEN(d, tok) — two initializers]
// [COLUMN]_[presence]_[EngineCliPresence within its scope]
// [COLUMN]_[doc]_[one-line usage description]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_ENGINE_CLI_OPTION(X)                                                                          \
    X(help, ENGINE_CLI_VALUE_NONE, nullptr, ENGINE_CLI_DISPATCH_HELP, ENGINE_CLI_SCOPE_ANY_DISPATCH,           \
      ENGINE_CLI_OPTIONAL, "print this usage to stdout and exit")
//======================================================================
// [END_CODE]
//======================================================================
// [END_REGISTRY]_[FOREACH_ENGINE_CLI_OPTION]
//======================================================================

enum EngineCliOptionId : uint8_t {
#define X(name, value_kind, value_set, dispatch, scope, presence, doc) ENGINE_CLI_OPT_##name,
    FOREACH_ENGINE_CLI_OPTION(X)
#undef X
    ENGINE_CLI_OPTION_COUNT
};

inline constexpr EngineCliOption g_engine_cli_options[] = {
#define X(name, value_kind, value_set, dispatch, scope, presence, doc) \
    { #name, value_kind, value_set, dispatch, scope, presence, doc },
    FOREACH_ENGINE_CLI_OPTION(X)
#undef X
};
static_assert(ENGINE_CLI_OPTION_COUNT <= ENGINE_CLI_MAX_OPTIONS, "raise ENGINE_CLI_MAX_OPTIONS");

//======================================================================
// [STRUCT]_[EngineCliArgs]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the resolved invocation — the mode, the cfg path, per-row values; on a refusal the offending token or row. Per-row arrays are indexed by the table's row index (ENGINE_CLI_OPT_<name> for the production table)]
//======================================================================
// [CODE]
//======================================================================
struct EngineCliArgs {
    uint8_t     result;                                // ENGINE_CLI_OK or a refusal
    uint8_t     dispatch;                              // the resolved mode (an index; 0 = the default)
    uint8_t     positional_given;                      // 1 = named on the command line; 0 = the default or none
    int8_t      offending_row;                         // the row a refusal names when no argv token does, or -1
    const char* dispatch_token;                        // the selecting ENUM token (a sub-mode), or nullptr
    const char* positional;                            // as named, else the mode's default, else nullptr
    const char* offending;                             // the argv token behind a refusal, or nullptr
    const char* value[ENGINE_CLI_MAX_OPTIONS];         // per row: its value ("" for a present bare flag); nullptr = absent
    const char* token[ENGINE_CLI_MAX_OPTIONS];         // per row: the argv token that named it
    int         token_index[ENGINE_CLI_MAX_OPTIONS];   // per row: that token's argv index (-1 = absent)
    uint64_t    uvalue[ENGINE_CLI_MAX_OPTIONS];        // per row: the parsed UINT value
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-09-28]
//----------------------------------------------------------------------
// [SIZE]_[928B]
// [ALIGN]_[8]
// [CACHE_LINES]_[15]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[EngineCliArgs]
//======================================================================

//======================================================================
// [SECTION]_[spelling + value helpers]
//======================================================================
static inline bool EngineCli_FlagMatches(const char* row_name, const char* argv_name) {
    for (; *row_name && *argv_name; ++row_name, ++argv_name) {
        if (*argv_name != (*row_name == '_' ? '-' : *row_name)) return false;
    }
    return *row_name == '\0' && *argv_name == '\0';
}

static inline void EngineCli_PrintFlag(FILE* out, const char* row_name) {
    fputs("--", out);
    for (const char* p = row_name; *p; ++p) fputc(*p == '_' ? '-' : *p, out);
}

// ENUM membership: `set` is "tok|tok|…"; the value must equal one token exactly.
static inline bool EngineCli_TokenInSet(const char* set, const char* value) {
    if (!set || !value) return false;
    const size_t vn = strlen(value);
    for (const char* p = set;;) {
        const char*  bar = strchr(p, '|');
        const size_t tn  = bar ? (size_t)(bar - p) : strlen(p);
        if (tn == vn && strncmp(p, value, vn) == 0) return true;
        if (!bar) return false;
        p = bar + 1;
    }
}

static inline bool EngineCli_InScope(const EngineCliOption& row, const EngineCliArgs* a) {
    const bool mode_ok  = row.scope_dispatch == ENGINE_CLI_SCOPE_ANY || row.scope_dispatch == a->dispatch;
    const bool token_ok = !row.scope_token ||
                          (a->dispatch_token && strcmp(row.scope_token, a->dispatch_token) == 0);
    return mode_ok && token_ok;
}

//======================================================================
// [FUNCTION]_[EngineCli_ValidateTable]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[table integrity — nullptr when the option + dispatch tables are well-formed, else the first violation; the resolver refuses an invalid table, and the suite pins both the production and the fixture tables]
//======================================================================
// [CODE]
//======================================================================
static inline const char* EngineCli_ValidateTable(const EngineCliOption* rows, int n_rows,
                                                  const EngineCliDispatch* disp, int n_disp) {
    if (!rows || n_rows < 0 || n_rows > ENGINE_CLI_MAX_OPTIONS) return "option count outside 0..ENGINE_CLI_MAX_OPTIONS";
    if (!disp || n_disp < 1 || n_disp >= ENGINE_CLI_SCOPE_ANY) return "dispatch count outside 1..253";
    for (int d = 0; d < n_disp; ++d) {
        if (!disp[d].name || !disp[d].name[0]) return "a mode has no name";
        if (disp[d].positional > ENGINE_CLI_POSITIONAL_REQUIRED_ONE) return "a mode has an unknown cfg-path policy";
        if (disp[d].default_positional && disp[d].positional != ENGINE_CLI_POSITIONAL_OPTIONAL_ONE)
            return "a default cfg path needs an OPTIONAL cfg-path policy";
        int selectors = 0;
        for (int r = 0; r < n_rows; ++r) selectors += (rows[r].dispatch == d);
        if (d == 0 && selectors != 0) return "the default mode (row 0) cannot be selected by a flag";
        if (d != 0 && selectors != 1) return "every other mode needs exactly one selecting option";
    }
    for (int r = 0; r < n_rows; ++r) {
        const EngineCliOption& o = rows[r];
        if (!o.name || !o.name[0]) return "an option has no name";
        for (const char* p = o.name; *p; ++p) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
                return "an option name is not lower_snake";
        }
        for (int q = 0; q < r; ++q) {
            if (strcmp(rows[q].name, o.name) == 0) return "two options share a name";
        }
        if (o.value_kind > ENGINE_CLI_VALUE_ENUM) return "an option has an unknown value kind";
        if ((o.value_kind == ENGINE_CLI_VALUE_ENUM) != (o.value_set != nullptr))
            return "a value set belongs to ENUM options only, and every ENUM option needs one";
        if (o.value_set) {
            const size_t n = strlen(o.value_set);
            if (n == 0 || o.value_set[0] == '|' || o.value_set[n - 1] == '|' || strstr(o.value_set, "||"))
                return "an ENUM value set has an empty choice";
        }
        if (o.presence > ENGINE_CLI_REQUIRED) return "an option has an unknown presence";
        if (o.dispatch != ENGINE_CLI_DISPATCH_NONE) {
            if (o.dispatch >= n_disp) return "an option selects a mode that does not exist";
            if (o.scope_dispatch != ENGINE_CLI_SCOPE_ANY || o.scope_token) return "a mode-selecting option cannot be scoped";
            if (o.presence != ENGINE_CLI_OPTIONAL) return "a mode-selecting option cannot be REQUIRED";
            continue;
        }
        if (o.scope_dispatch != ENGINE_CLI_SCOPE_ANY && o.scope_dispatch >= n_disp)
            return "an option is scoped to a mode that does not exist";
        if (o.scope_token) {
            if (o.scope_dispatch == ENGINE_CLI_SCOPE_ANY) return "a sub-mode scope needs a mode";
            const EngineCliOption* selector = nullptr;
            for (int q = 0; q < n_rows; ++q) {
                if (rows[q].dispatch == o.scope_dispatch) selector = &rows[q];
            }
            if (!selector || selector->value_kind != ENGINE_CLI_VALUE_ENUM ||
                !EngineCli_TokenInSet(selector->value_set, o.scope_token))
                return "a sub-mode scope names a choice its mode's selector does not offer";
        }
    }
    return nullptr;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineCli_ValidateTable]
//======================================================================

static inline uint8_t EngineCli_Refuse(EngineCliArgs* a, uint8_t result, const char* token, int row) {
    a->result        = result;
    a->offending     = token;
    a->offending_row = (int8_t)row;
    return result;
}

//======================================================================
// [FUNCTION]_[EngineCli_Resolve]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[resolve argv against a row ARRAY — pass 1 walks argv (unknown / spelling / duplicate / value / second-mode refusals, the first offender in argv order); pass 2 checks scope (first offender in argv order), required options (row order) and the mode's cfg-path policy]
//======================================================================
// [CODE]
//======================================================================
static inline uint8_t EngineCli_Resolve(const EngineCliOption* rows, int n_rows,
                                        const EngineCliDispatch* disp, int n_disp,
                                        int argc, const char* const* argv, EngineCliArgs* out) {
    *out = EngineCliArgs{};
    out->offending_row = -1;
    for (int r = 0; r < ENGINE_CLI_MAX_OPTIONS; ++r) out->token_index[r] = -1;
    if (EngineCli_ValidateTable(rows, n_rows, disp, n_disp)) return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_TABLE_INVALID, nullptr, -1);

    int  selector      = -1;
    bool options_ended = false;
    for (int i = 1; i < argc && argv[i]; ++i) {
        const char* a = argv[i];
        if (!options_ended && a[0] == '-') {
            if (strcmp(a, "--") == 0) { options_ended = true; continue; }
            if (a[1] != '-') return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_UNKNOWN_OPTION, a, -1);
            const char* name = a + 2;
            if (strchr(name, '=')) return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_EQUALS_FORM, a, -1);
            int r = -1;
            for (int q = 0; q < n_rows; ++q) {
                if (EngineCli_FlagMatches(rows[q].name, name)) { r = q; break; }
            }
            if (r < 0) return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_UNKNOWN_OPTION, a, -1);
            if (out->token[r]) return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_DUPLICATE_OPTION, a, r);
            out->token[r]       = a;
            out->token_index[r] = i;
            if (rows[r].value_kind == ENGINE_CLI_VALUE_NONE) {
                out->value[r] = "";
            } else {
                if (i + 1 >= argc || !argv[i + 1]) return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_MISSING_VALUE, a, r);
                const char* v = argv[++i];
                if (v[0] == '\0') return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_EMPTY_VALUE, a, r);
                if (v[0] == '-')  return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_MISSING_VALUE, a, r);
                if (rows[r].value_kind == ENGINE_CLI_VALUE_UINT && !tt::parse_uint64_checked(v, &out->uvalue[r]))
                    return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_VALUE_NOT_UINT, v, r);
                if (rows[r].value_kind == ENGINE_CLI_VALUE_ENUM && !EngineCli_TokenInSet(rows[r].value_set, v))
                    return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_VALUE_NOT_IN_SET, v, r);
                out->value[r] = v;
            }
            if (rows[r].dispatch != ENGINE_CLI_DISPATCH_NONE) {
                if (selector >= 0) return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_SECOND_DISPATCH, a, r);
                selector = r;
            }
            continue;
        }
        if (a[0] == '\0')          return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_EMPTY_POSITIONAL, a, -1);
        if (out->positional_given) return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_EXTRA_POSITIONAL, a, -1);
        out->positional       = a;
        out->positional_given = 1;
    }

    if (selector >= 0) {
        out->dispatch = rows[selector].dispatch;
        if (rows[selector].value_kind == ENGINE_CLI_VALUE_ENUM) out->dispatch_token = out->value[selector];
    }
    int stray = -1;
    for (int r = 0; r < n_rows; ++r) {
        if (!out->token[r] || rows[r].dispatch != ENGINE_CLI_DISPATCH_NONE) continue;
        if (EngineCli_InScope(rows[r], out)) continue;
        if (stray < 0 || out->token_index[r] < out->token_index[stray]) stray = r;
    }
    if (stray >= 0) return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_OUT_OF_SCOPE, out->token[stray], stray);
    for (int r = 0; r < n_rows; ++r) {
        if (rows[r].dispatch != ENGINE_CLI_DISPATCH_NONE || rows[r].presence != ENGINE_CLI_REQUIRED || out->token[r]) continue;
        if (EngineCli_InScope(rows[r], out)) return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_MISSING_REQUIRED, nullptr, r);
    }
    const EngineCliDispatch& mode = disp[out->dispatch];
    if (mode.positional == ENGINE_CLI_POSITIONAL_FORBIDDEN && out->positional_given)
        return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_POSITIONAL_FORBIDDEN, out->positional, -1);
    if (mode.positional == ENGINE_CLI_POSITIONAL_REQUIRED_ONE && !out->positional_given)
        return EngineCli_Refuse(out, ENGINE_CLI_REFUSE_MISSING_POSITIONAL, nullptr, -1);
    if (!out->positional_given && mode.default_positional) out->positional = mode.default_positional;
    return out->result = ENGINE_CLI_OK;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineCli_Resolve]
//======================================================================

static inline void EngineCli_PrintValueHint(FILE* out, const EngineCliOption& row) {
    switch (row.value_kind) {
        case ENGINE_CLI_VALUE_STRING: fputs(" <value>", out); break;
        case ENGINE_CLI_VALUE_UINT:   fputs(" <n>", out); break;
        case ENGINE_CLI_VALUE_ENUM:   fprintf(out, " %s", row.value_set); break;
        default: break;
    }
}

//======================================================================
// [FUNCTION]_[EngineCli_PrintUsage]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the usage, generated from the rows — one line per mode (its selecting flag + cfg-path policy), then every ordinary option with its scope]
//======================================================================
// [CODE]
//======================================================================
static inline void EngineCli_PrintUsage(const EngineCliOption* rows, int n_rows,
                                        const EngineCliDispatch* disp, int n_disp,
                                        const char* prog, FILE* out) {
    fputs("usage:\n", out);
    for (int d = 0; d < n_disp; ++d) {
        fprintf(out, "  %s", prog);
        for (int r = 0; r < n_rows; ++r) {
            if (rows[r].dispatch != d) continue;
            fputc(' ', out);
            EngineCli_PrintFlag(out, rows[r].name);
            EngineCli_PrintValueHint(out, rows[r]);
        }
        if (disp[d].positional == ENGINE_CLI_POSITIONAL_OPTIONAL_ONE) fputs(" [<cfg>]", out);
        if (disp[d].positional == ENGINE_CLI_POSITIONAL_REQUIRED_ONE) fputs(" <cfg>", out);
        fprintf(out, "\n      %s", disp[d].doc);
        if (disp[d].default_positional) fprintf(out, " (no <cfg> named: %s)", disp[d].default_positional);
        fputc('\n', out);
    }
    bool header = false;
    for (int r = 0; r < n_rows; ++r) {
        const EngineCliOption& o = rows[r];
        if (o.dispatch != ENGINE_CLI_DISPATCH_NONE) continue;
        if (!header) { fputs("options:\n", out); header = true; }
        fputs("  ", out);
        EngineCli_PrintFlag(out, o.name);
        EngineCli_PrintValueHint(out, o);
        if (o.scope_dispatch == ENGINE_CLI_SCOPE_ANY) fputs("  [every mode", out);
        else fprintf(out, "  [mode '%s'%s%s", disp[o.scope_dispatch].name, o.scope_token ? " " : "",
                     o.scope_token ? o.scope_token : "");
        fprintf(out, "%s]\n      %s\n", o.presence == ENGINE_CLI_REQUIRED ? ", required" : "", o.doc);
    }
    fputs("  --  ends the options; what follows is the cfg path\n", out);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineCli_PrintUsage]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineCli_PrintRefusal]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one line naming the refusal and the offending token (or the missing option's flag)]
//======================================================================
// [CODE]
//======================================================================
static inline void EngineCli_PrintRefusal(const EngineCliArgs* a, const EngineCliOption* rows, FILE* out) {
    fprintf(out, "[engine] command line refused: %s", EngineCli_ResultMessage(a->result));
    if (a->offending) {
        fprintf(out, ": '%s'", a->offending);
    } else if (a->offending_row >= 0) {
        fputs(": ", out);
        EngineCli_PrintFlag(out, rows[a->offending_row].name);
    }
    fputc('\n', out);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineCli_PrintRefusal]
//======================================================================
