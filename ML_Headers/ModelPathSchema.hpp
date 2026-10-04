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
#include <sys/stat.h>   // ModelPath_ExpectedCfgResolve — stat, never a probing fopen

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
// The largest horizon (ticks) a model path names — the matcher below refuses past it, so the Training panel's Horizons
// CSV takes no more (E.1.3 MP-6 step 10.5's review, F8: the two bounds were separate literals).
enum { MODEL_HORIZON_TICKS_MAX = 1000000 };

//======================================================================
// [FUNCTION]_[Model_ParseHorizonSibling]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE horizon-entry matcher — prefix match + all-digits suffix + (0, MODEL_HORIZON_TICKS_MAX] bounds + the leaf-8 canonical round-trip; returns the horizon ticks or -1. RELOCATED here from NodeModelZoo at the nested ship (D-431) so builders and parsers share one grammar header; semantics byte-unchanged, the 3G-ii + L8 test cells pin it]
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
    if (h <= 0 || h > MODEL_HORIZON_TICKS_MAX) return -1;   // sanity bounds
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
// [FUNCTION]_[ModelPath_FamilyDir]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE family-dir builder — "<models_root>/<class_tree>/<family>", D-431's bundle node; false when the path does not fit. The caller resolves the class tree (Training_ResolveClassTree — a training rule, so it stays in Backtest/)]
//======================================================================
// [CODE]
//======================================================================
static inline bool ModelPath_FamilyDir(char* buf, size_t buf_size, const char* models_root, const char* class_tree,
                                       const char* family) {
    const int n = snprintf(buf, buf_size, "%s/%s/%s", models_root, class_tree, family);
    return n > 0 && (size_t)n < buf_size;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ModelPath_FamilyDir]
//======================================================================

//======================================================================
// [FUNCTION]_[ModelPath_FamilyOfHorizonDir]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the family of a horizon dir — its parent, when its last component is a horizon child (the ONE matcher; trailing slashes ignored); false when it is not one (a retired flat run dir, a path with no parent) or the path does not fit]
//======================================================================
// [CODE]
//======================================================================
static inline bool ModelPath_FamilyOfHorizonDir(const char* horizon_dir, char* buf, size_t buf_size) {
    const int n = snprintf(buf, buf_size, "%s", horizon_dir);
    if (n <= 0 || (size_t)n >= buf_size) return false;
    size_t len = (size_t)n;
    while (len > 1 && buf[len - 1] == '/') buf[--len] = '\0';   // ".../horizon_5/" names horizon_5
    char* slash = strrchr(buf, '/');
    if (!slash || slash == buf || ModelPath_ParseHorizonChild(slash + 1) < 0) return false;
    *slash = '\0';
    return true;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ModelPath_FamilyOfHorizonDir]
//======================================================================

//======================================================================
// [FUNCTION]_[ModelPath_RetiredFlatHorizon]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the horizon a name carries in the RETIRED flat form "<family>_horizon_<N>" (the LAST "_horizon_", a non-empty family before it, a canonical N) — >0 when it does, -1 otherwise; the ONE spelling of the rule the picker, the boot WARN and the family-name rule read]
//======================================================================
// Hand-copied twice until MP-6 (3c-1b)'s review (F2 — GUI/ModelBundleScan.hpp and CoreFrameworks/EngineCommon.hpp, the
// copies disagreeing on a name that BEGINS with "_horizon_"): a Class 59-A duplicate on the schema's own surface.
//======================================================================
// [CODE]
//======================================================================
static inline long ModelPath_RetiredFlatHorizon(const char* name) {
    if (!name) return -1;
    const char* last = nullptr;
    for (const char* p = name; (p = strstr(p, "_horizon_")) != nullptr; ++p) last = p;
    if (!last || last == name) return -1;   // no family before it
    return Model_ParseHorizonSibling(name, name, (int)(last - name) + 9 /* strlen("_horizon_") */);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ModelPath_RetiredFlatHorizon]
//======================================================================

//======================================================================
// [FUNCTION]_[ModelPath_FamilyNameValid]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[can `name` name a model FAMILY (D-431's bundle node)? An ALLOW-list — [A-Za-z0-9_] first, then [A-Za-z0-9_.-], at most MODEL_FAMILY_NAME_MAX bytes, its end within `cap` — and not a schema word (a horizon child "horizon_<N>", a retired flat "<x>_horizon_<N>")]
//======================================================================
// Her call (CC-09, the 2026-09-30 gate): the core validates the run name — one path component, bounded, no control
// characters. Kept as an ALLOW-list (MP-6 (3c-1b)'s review, F1 / F2 / F4 / F6 — hers to veto) because a deny-list leaked
// to every consumer the name meets: the engine's cfg loader (a '#' starts a comment and edge whitespace is stripped —
// "btc #2" would BIND "btc", serving the other family's models), the shells and tools (no spaces), the scanners (no
// leading '.'; no schema word — a family named horizon_100 makes its class tree read as ONE family in the picker, one
// named x_horizon_5 reads as a retired flat arm the migration tool would move), and the served-family name (D-485's
// ensemble_name[32] — the tightest bound). MP-7c will record the name in the stamp and re-apply this rule where it reads it.
//======================================================================
// [CODE]
//======================================================================
static const int MODEL_FAMILY_NAME_MAX = 31;   // D-485's ensemble_name[32]: the tightest consumer of a family's name
static inline bool ModelPath_FamilyNameChar(unsigned char c, bool first) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           (!first && (c == '.' || c == '-'));
}
static inline bool ModelPath_FamilyNameValid(const char* name, size_t cap) {
    if (!name || cap == 0) return false;
    const size_t n = strnlen(name, cap);
    if (n == 0 || n == cap || n > (size_t)MODEL_FAMILY_NAME_MAX) return false;
    for (size_t i = 0; i < n; ++i)
        if (!ModelPath_FamilyNameChar((unsigned char)name[i], i == 0)) return false;
    return ModelPath_ParseHorizonChild(name) < 0 && ModelPath_RetiredFlatHorizon(name) < 0;   // not a schema word
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ModelPath_FamilyNameValid]
//======================================================================

//======================================================================
// [FUNCTION]_[ModelPath_HorizonOfModelFile]
//----------------------------------------------------------------------
// [TAG]_[[ML_INFERENCE] [GUI] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the horizon a model FILE belongs to — its parent directory parsed by the ONE horizon-child matcher (`.../horizon_<N>/<role>.json` → N); -1 when the path names no horizon directory (a flat or legacy path, no directory, an aliased spelling)]
//======================================================================
// Full Validation's gate compares it with the collected labels' horizon (D-507): a model of another horizon would be
// scored on labels it never predicted and re-stamped with a horizon it was never trained on.
//======================================================================
// [CODE]
//======================================================================
static inline long ModelPath_HorizonOfModelFile(const char* path) {
    if (!path) return -1;
    const char* file = strrchr(path, '/');
    if (!file) return -1;                                 // a bare file name: no parent in the path
    const char* dir = file;
    while (dir > path && dir[-1] != '/') --dir;          // the parent's first character
    char name[32];
    const size_t n = (size_t)(file - dir);
    if (n >= sizeof(name)) return -1;                     // longer than any horizon_<N> the matcher accepts
    // (an empty parent — a file at the root, `//` — reaches the matcher as "" and is no horizon: -1)
    memcpy(name, dir, n);
    name[n] = '\0';
    return ModelPath_ParseHorizonChild(name);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ModelPath_HorizonOfModelFile]
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
// D-483 C (2026-09-04, the process dimension) — the FAMILY's exclusive-lock
// file (D-431's bundle node). Held (flock LOCK_EX|LOCK_NB on an O_CLOEXEC
// descriptor) by the family's writers: the ONE process+node whose ezoo is
// bound to it (the four state files above, for as long as it is bound), a
// training run writing its models (MP-6 (3c), D-503 — first write to last;
// Full Validation's re-stamp joins at (3c-2)), and a Past Runs delete for the
// length of its walk. A second taker REFUSES —
// persistence OFF / the run refused / the delete refused, each loud — instead
// of the last-writer-wins clobber TECH_DEBT-331 recorded. A dotfile, so the bundle
// scanner never lists it (ModelBundleScan skips '.'-leading entries). H21
// on-disk identifier: ledgered in tools/identifier_ledger.txt — rename by
// tombstone + new name, never in place.
static const char MODEL_STATE_LOCK_FILE[]           = ".foxml_state.lock";

//======================================================================
// [SECTION]_[side-addressed sidecar records at the horizon dir]
//----------------------------------------------------------------------
// Discipline 6 of model-artifact-path-schema-discipline.md: a record shared
// across roles is a COLLISION class. summary_{entry,exit}.txt closed it for
// the summaries (E.1.2.D D-e); expected.cfg kept the shared name and the
// exit run overwrote the entry record of every horizon of the operator's
// best family on 2026-09-02 (what-to-do-next.md, "writer clobber, one file
// two roles"). The writer now addresses the file by side; readers prefer the
// side file and fall back to the legacy shared name ONCE (an old bundle
// carries one record, of whichever role wrote last — the legacy file's own
// expected_role says which).
//
// data_files_{entry,exit}.txt is the corpus list the same run trained on —
// the selection the summary only hashes + brackets (first / last / count).
//======================================================================
static const char MODEL_SIDECAR_EXPECTED_ENTRY[]    = "expected_entry.cfg";
static const char MODEL_SIDECAR_EXPECTED_EXIT[]     = "expected_exit.cfg";
static const char MODEL_SIDECAR_EXPECTED_LEGACY[]   = "expected.cfg";   // pre-2026-09-03 shared name (read-only)
static const char MODEL_SIDECAR_DATA_FILES_ENTRY[]  = "data_files_entry.txt";
static const char MODEL_SIDECAR_DATA_FILES_EXIT[]   = "data_files_exit.txt";

// The side-addressed expected-record NAME for a training side (0 = entry,
// 1 = exit). The writer's ONE spelling; the readers resolve through the
// helper below so the fallback rule lives in exactly one place.
static inline const char* ModelPath_ExpectedCfgName(int training_side) {
    return training_side == 1 ? MODEL_SIDECAR_EXPECTED_EXIT
                              : MODEL_SIDECAR_EXPECTED_ENTRY;
}

// Resolve the expected-record PATH a reader should open for `training_side`
// under `dir`. Returns 1 = the side file exists (path filled), 2 = only the
// legacy shared file exists (path filled — the caller must treat its
// expected_role as the record's own side, not the requested one), 0 = no
// record (path emptied; readers silent-pass — an old bundle without one).
// `stat`, not `fopen`: a resolve must not open a handle the caller then
// re-opens (two opens of one file is the shape that leaks under early return).
static inline int ModelPath_ExpectedCfgResolve(const char* dir, int training_side,
                                               char* buf, size_t buf_size) {
    if (!dir || dir[0] == '\0' || !buf || buf_size == 0) { if (buf && buf_size) buf[0] = '\0'; return 0; }
    struct stat st;
    snprintf(buf, buf_size, "%s/%s", dir, ModelPath_ExpectedCfgName(training_side));
    if (stat(buf, &st) == 0) return 1;
    snprintf(buf, buf_size, "%s/%s", dir, MODEL_SIDECAR_EXPECTED_LEGACY);
    if (stat(buf, &st) == 0) return 2;
    buf[0] = '\0';
    return 0;
}

#endif // MODEL_PATH_SCHEMA_HPP
