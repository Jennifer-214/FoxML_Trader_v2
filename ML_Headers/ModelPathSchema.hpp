// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/ModelPathSchema.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the model-artifact PATH SCHEMA SSoT (E.1.2.D D-f, nested layout D-431) — one header owns the family/horizon-dir grammar, the horizon-entry matcher, and the four state filenames; every builder and parser consumes THIS so the next consumer cannot hand-roll site 12]
//======================================================================================================
// NESTED LAYOUT (operator-decided 2026-08-22, D-431/D-f — supersedes the
// flat `<family>_horizon_<N>` sibling form):
//
//   models/<class>/<family>/                    ← the BUNDLE node (cfg's node_model_dir)
//   models/<class>/<family>/horizon_<N>/<role>.json[.stamp]
//   models/<class>/<family>/{bandit_state, exit_bandit_state,
//                            buy_thompson_state, exit_thompson_state}.json
//
// One filesystem node per logical unit: family delete = one rm -r, backup =
// one cp -r, the family glob cannot over-match prefix-sibling backups, and
// the bandle-scoped state files live inside the node their savers already
// target. The OLD flat form is RETIRED LOUDLY — the walker carries a
// diagnostics-only old-form detector that prints the exact `mv` commands
// (H21 tombstone discipline applied to a path form; see D-f's spec).
//======================================================================================================
#ifndef MODEL_PATH_SCHEMA_HPP
#define MODEL_PATH_SCHEMA_HPP

#include <cstdio>
#include <cstring>
#include <cstdlib>

//======================================================================
// [SECTION]_[horizon-child grammar]
//----------------------------------------------------------------------
// Under nested, a family's horizons are CHILD dirs named `horizon_<N>`
// (constant prefix — the family name no longer appears inside the entry
// name, which is what killed the FIRST-vs-LAST split-rule divergence
// class structurally).
//======================================================================
static const char MODEL_HORIZON_PREFIX[] = "horizon_";
enum { MODEL_HORIZON_PREFIX_LEN = 8 };  // strlen("horizon_")

//======================================================================
// [FUNCTION]_[Model_ParseHorizonSibling]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE horizon-entry matcher — prefix match + all-digits suffix + (0, 1000000] bounds + the leaf-8 canonical round-trip; returns the horizon ticks or -1. RELOCATED here from NodeModelZoo at the nested ship (D-431) so builders and parsers share one grammar header; semantics byte-unchanged, the 3G-ii + L8 test cells pin it]
//======================================================================
// [CODE]
//======================================================================
static inline long Model_ParseHorizonSibling(const char* entry_name,
                                             const char* prefix,
                                             int prefix_len) {
    if (strncmp(entry_name, prefix, (size_t)prefix_len) != 0) return -1;
    const char* suffix = entry_name + prefix_len;
    char* end = nullptr;
    long h = strtol(suffix, &end, 10);
    if (end == suffix || *end != '\0') return -1;  // non-numeric suffix
    if (h <= 0 || h > 1000000) return -1;          // sanity bounds
    // E.1.2.D leaf 8 (S2-F5) — canonical-form round-trip. strtol accepts
    // "07500" / "+7500" / " 7500" / "00000007500" as 7500, and every loader
    // REBUILDS the path FROM the int — so an aliased spelling loaded the ONE
    // canonical dir a SECOND time as a second ensemble arm (measured).
    // Only the spelling the path builders themselves emit is a member.
    char canon[24];
    snprintf(canon, sizeof(canon), "%ld", h);
    if (strcmp(canon, suffix) != 0) return -1;     // aliased spelling — reject
    return h;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Model_ParseHorizonSibling]
//======================================================================

//======================================================================
// [FUNCTION]_[ModelPath_ParseHorizonChild]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[nested-layout entry matcher — `horizon_<N>` child-name to ticks (or -1); the constant-prefix specialization every nested walker uses]
//======================================================================
// [CODE]
//======================================================================
static inline long ModelPath_ParseHorizonChild(const char* entry_name) {
    return Model_ParseHorizonSibling(entry_name, MODEL_HORIZON_PREFIX,
                                     MODEL_HORIZON_PREFIX_LEN);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ModelPath_ParseHorizonChild]
//======================================================================

//======================================================================
// [FUNCTION]_[ModelPath_HorizonDir]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE horizon-dir builder — "<family_dir>/horizon_<N>"; every emit site consumes this instead of an inline snprintf grammar copy]
//======================================================================
// [CODE]
//======================================================================
static inline void ModelPath_HorizonDir(char* buf, size_t buf_size,
                                        const char* family_dir, long horizon) {
    snprintf(buf, buf_size, "%s/%s%ld", family_dir,
             MODEL_HORIZON_PREFIX, horizon);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ModelPath_HorizonDir]
//======================================================================

//======================================================================
// [SECTION]_[old-flat-form detection (transitional, LOUD)]
//----------------------------------------------------------------------
// The RETIRED pre-D-431 form was `<family>_horizon_<N>` as a SIBLING of
// the family base. After the flip an un-migrated flat family is silently
// invisible to the nested walker — the exact silent-failure shape the
// whole plan exists to kill — so walkers keep a diagnostics-only probe
// for the old form and print the fix. Backups/hand-restores keep
// re-introducing flat names, so this is a standing tombstone, not
// first-week scaffolding.
//======================================================================
static inline long ModelPath_ParseOldFlatSibling(const char* entry_name,
                                                 const char* family_basename,
                                                 int family_len) {
    // old grammar: "<family>_horizon_<digits>" (canonical digits only)
    char prefix[300];
    int n = snprintf(prefix, sizeof(prefix), "%.*s_horizon_",
                     family_len, family_basename);
    if (n <= 0 || n >= (int)sizeof(prefix)) return -1;
    return Model_ParseHorizonSibling(entry_name, prefix, n);
}

//======================================================================
// [SECTION]_[bundle-scoped state filenames]
//----------------------------------------------------------------------
// The four persistence files that live AT the family node (their savers
// provision the dir; see the D-a no-regret batch). Named here so a fifth
// state file starts from the schema, not from a new literal.
//======================================================================
static const char MODEL_STATE_FILE_BANDIT[]         = "bandit_state.json";
static const char MODEL_STATE_FILE_EXIT_BANDIT[]    = "exit_bandit_state.json";
static const char MODEL_STATE_FILE_BUY_THOMPSON[]   = "buy_thompson_state.json";
static const char MODEL_STATE_FILE_EXIT_THOMPSON[]  = "exit_thompson_state.json";

#endif // MODEL_PATH_SCHEMA_HPP
