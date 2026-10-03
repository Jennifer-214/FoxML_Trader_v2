// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[Backtest/SuiteStartGates.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the suite's start gates — and the parser of the CSV inputs they read (SuiteCsv_*) — — for each action that starts a suite run, the terms it must pass and the words that say which one failed, as a pure function of plain values and the lease; std-only, so the suite's cells pin every action's gate and a headless verb can refuse with the same policy the panel shows]
// [CONTAINS]
//   - [STRUCT]_[SuiteCsvParse] · [FUNCTION]_[SuiteCsv_NextToken] · [FUNCTION]_[SuiteCsv_HasValue] · [FUNCTION]_[SuiteCsv_CopyToken]
//   - [FUNCTION]_[SuiteCsv_Tiny] · [FUNCTION]_[SuiteCsv_Parse] · [FUNCTION]_[SuiteCsv_RefuseBroadcast]
//   - [FUNCTION]_[SuiteCsv_StopAtRepeat] · [FUNCTION]_[StartGate_FirstBadHorizon]
//   - [FUNCTION]_[SuiteCsv_WriteValue] · [FUNCTION]_[SuiteCsv_ErrorLine]
//   - [FUNCTION]_[StartGate_SelectedFiles]
//   - [FUNCTION]_[StartGate_RekeySelection]
//   - [FUNCTION]_[StartGate_NeedGrid] · [FUNCTION]_[StartGate_GridFits]
//   - [FUNCTION]_[StartGate_RunBacktest]
//   - [FUNCTION]_[StartGate_GridSearch]
//   - [FUNCTION]_[StartGate_CollectFeatures]
//   - [FUNCTION]_[StartGate_CollectMultiHorizon]
//   - [FUNCTION]_[StartGate_TrainModel]
//   - [FUNCTION]_[StartGate_TrainMultiHorizon]
//   - [FUNCTION]_[StartGate_WalkForward]
//   - [FUNCTION]_[StartGate_HyperparamSweep]
//   - [FUNCTION]_[StartGate_FullValidation]
// [REFERENCE]_[DECISION]_[[D-503] [D-507]]
//======================================================================================================
//
// WHY PURE FUNCTIONS OF PLAIN VALUES (D-507; its review's F2): a gate the panel builds inline can be checked only by
// reading the panel, and the panel needs ImGui, so no test lane compiles it. Here each action's terms — which, in what
// order, at what thresholds, with which words and tone — are one call the cells drive with the lease free and held, and
// the panel keeps only the line that reads its fields into the call. Whether this build can train is an input like any
// other, so both answers run in every lane.
//
// THE ORDER, every gate (SuiteGate's rule): what this build can do, what the operator can fix now, whether the suite is
// free, what a run must produce first — so the reason a button shows is the next thing to do.
//
// The reasons are worded for the suite's panel ("(above)", "Set Model Path first"); a headless verb that reuses these
// gates decides its own wording at E.2's consult.

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <charconv>       // E.1.3 MP-6 step 10.5 — the CSV inputs parse locale-free (H5)
#include <type_traits>
#include "SuiteLease.hpp"
#include "../ML_Headers/ModelPathSchema.hpp"   // MODEL_HORIZON_TICKS_MAX — the largest horizon a model path names

// ==== the CSV inputs a start reads (E.1.3 MP-6 step 10.5) ====
// The Training panel's four CSV fields — the horizons, the label kinds, TP and SL — parse through ONE parser that STOPS
// at the first value it cannot keep and says so, where the old loops dropped a value and shifted the rest onto the wrong
// horizon. It lives here, beside the gates its result feeds (StartGate_NeedCsvs), ImGui-free: a headless caller (E.2)
// parses as the panel does.

//======================================================================
// [STRUCT]_[SuiteCsvParse]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[what parsing one CSV field gave — the values it kept and, when it stopped, where, why, the token and the range there; the field's cap travels with it (the red line names it)]
//======================================================================
// [CODE]
//======================================================================
enum : int {
    SUITE_CSV_OK = 0, SUITE_CSV_NOT_A_NUMBER = 1, SUITE_CSV_OUT_OF_RANGE = 2, SUITE_CSV_TOO_MANY = 3,
    SUITE_CSV_UNITS_DIFFER = 4,  // one value for horizons whose units differ (SuiteCsv_RefuseBroadcast)
    SUITE_CSV_DUPLICATE    = 5   // a value repeating an earlier one, where each must be distinct (SuiteCsv_StopAtRepeat)
};
struct SuiteCsvParse {
    int    count;       // values kept, in order
    int    stop;        // SUITE_CSV_OK, or why the field cannot be applied as typed
    int    stop_pos;    // the 1-based position it stopped at (0 = it did not)
    int    cap;         // the most values the field holds — its list's size
    double lo, hi;      // the legal range there (SUITE_CSV_OUT_OF_RANGE)
    char   token[32];   // the text it stopped at — a cut one ends "…", never inside a UTF-8 character
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-03]
//----------------------------------------------------------------------
// [SIZE]_[64B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[SuiteCsvParse]
//======================================================================

//======================================================================
// [FUNCTION]_[SuiteCsv_NextToken]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the next token of a CSV field — past the separators (commas, spaces, tabs) to the next one; false at the text's end; what a token is, said once]
//======================================================================
// [CODE]
//======================================================================
inline bool SuiteCsv_NextToken(const char** p, const char** tok_end) {
    while (**p == ' ' || **p == '\t' || **p == ',') (*p)++;
    if (!**p) return false;
    const char* e = *p;
    while (*e && *e != ' ' && *e != '\t' && *e != ',') e++;
    *tok_end = e;
    return true;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteCsv_NextToken]
//======================================================================

//======================================================================
// [FUNCTION]_[SuiteCsv_HasValue]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[does a CSV field hold a value — a token, not only separators: "", " ", "," and ", " hold none]
//======================================================================
// A field emptied by deleting its numbers keeps its commas: tested on the first character, ", " looked like a value,
// was never seeded, parsed to nothing, and a start applied the stored fallback unchecked (10.5's second review, N1).
//======================================================================
// [CODE]
//======================================================================
inline bool SuiteCsv_HasValue(const char* csv) {
    const char* p       = csv ? csv : "";
    const char* tok_end = p;
    return SuiteCsv_NextToken(&p, &tok_end);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteCsv_HasValue]
//======================================================================

//======================================================================
// [FUNCTION]_[SuiteCsv_CopyToken]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[copy a token into a fixed buffer, NUL-terminated; one too long is cut at a UTF-8 character's boundary and ends "…", so a cut never splits a character or reads as the whole token]
//======================================================================
// [CODE]
//======================================================================
inline void SuiteCsv_CopyToken(char* dst, size_t cap, const char* tok, const char* tok_end) {
    if (!dst || cap == 0) return;
    const size_t n = (size_t)(tok_end - tok);
    if (n < cap) {
        memcpy(dst, tok, n);
        dst[n] = '\0';
        return;
    }
    static const char ELLIPSIS[] = "\xE2\x80\xA6";   // "…", 3 bytes
    if (cap < sizeof(ELLIPSIS)) {
        dst[0] = '\0';
        return;
    }
    size_t k = cap - sizeof(ELLIPSIS);   // the text kept: room left for "…" and the NUL
    while (k > 0 && ((unsigned char)tok[k] & 0xC0) == 0x80) --k;   // tok[k] starts the first byte dropped: never a continuation
    memcpy(dst, tok, k);
    memcpy(dst + k, ELLIPSIS, sizeof(ELLIPSIS));
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteCsv_CopyToken]
//======================================================================

//======================================================================
// [FUNCTION]_[SuiteCsv_Tiny]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[is a well-formed number that a float cannot hold TINY (|x| < 1 — an underflow) rather than huge — by double, and past double's range by its exponent's sign]
//======================================================================
// Exact for a token under ~300 characters (every CSV field's buffer is 128 bytes at most): past double's range, a tiny
// number needs a negative exponent and a huge one a positive or none — spelling either without one takes 300+ digits.
//======================================================================
// [CODE]
//======================================================================
inline bool SuiteCsv_Tiny(const char* num, const char* end) {
    double d = 0.0;
    const std::from_chars_result fd = std::from_chars(num, end, d);
    if (fd.ec == std::errc() && fd.ptr == end) return d > -1.0 && d < 1.0;
    for (const char* q = num; q + 1 < end; ++q)
        if ((*q == 'e' || *q == 'E') && q[1] == '-') return true;
    return false;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteCsv_Tiny]
//======================================================================

//======================================================================
// [FUNCTION]_[SuiteCsv_Parse]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[parse a CSV field into out[0..cap) — values split by commas, spaces or tabs; range(i, &lo, &hi) gives position i's legal range — STOPPING at the first value it cannot keep: not a number (a malformed token included), outside its range (NaN included), or one past the cap; locale-free (std::from_chars)]
//======================================================================
// A value is never dropped: a drop shifts every later one onto the wrong horizon whenever the counts still agree. An
// integer field takes integers only ("12.5" stops). One leading '+' is taken ("+5" — the old strtol / strtof took it). A
// float too small for its type is zero, as strtof made it, never "out of range"; one too large is out of range.
//======================================================================
// [CODE]
//======================================================================
template <typename T, typename Range>
inline SuiteCsvParse SuiteCsv_Parse(const char* csv, T* out, int cap, Range range) {
    SuiteCsvParse r = {};
    r.cap = cap;
    const char* p       = csv ? csv : "";
    const char* tok_end = p;
    auto stop_at = [&](int why) {
        r.stop     = why;
        r.stop_pos = r.count + 1;
        SuiteCsv_CopyToken(r.token, sizeof(r.token), p, tok_end);
        return r;
    };
    while (SuiteCsv_NextToken(&p, &tok_end)) {
        if (r.count == cap) return stop_at(SUITE_CSV_TOO_MANY);
        const char* num = (*p == '+' && ((p[1] >= '0' && p[1] <= '9') || p[1] == '.')) ? p + 1 : p;
        T v{};
        const std::from_chars_result fc = std::from_chars(num, tok_end, v);
        if (fc.ptr != tok_end || (fc.ec != std::errc() && fc.ec != std::errc::result_out_of_range))
            return stop_at(SUITE_CSV_NOT_A_NUMBER);   // malformed — a number with junk after it included ("1e400x")
        if (fc.ec == std::errc::result_out_of_range) {
            bool tiny = false;
            if constexpr (std::is_floating_point<T>::value) tiny = SuiteCsv_Tiny(num, tok_end);
            if (!tiny) {
                range(r.count, &r.lo, &r.hi);
                return stop_at(SUITE_CSV_OUT_OF_RANGE);
            }
            v = T(0);   // an underflow: zero, as strtof made it
        }
        double lo = 0.0, hi = 0.0;
        range(r.count, &lo, &hi);
        if (!((double)v >= lo && (double)v <= hi)) {   // NaN fails both
            r.lo = lo;
            r.hi = hi;
            return stop_at(SUITE_CSV_OUT_OF_RANGE);
        }
        out[r.count++] = v;
        p = tok_end;
    }
    return r;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteCsv_Parse]
//======================================================================

//======================================================================
// [FUNCTION]_[SuiteCsv_RefuseBroadcast]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[refuse a field's ONE value as a broadcast — the caller found the horizons it would reach read it in different units — so nothing of it is applied and it says so like any stop (SUITE_CSV_UNITS_DIFFER at position 1)]
//======================================================================
// [CODE]
//======================================================================
inline void SuiteCsv_RefuseBroadcast(SuiteCsvParse* r, const char* csv) {
    const char* p       = csv ? csv : "";
    const char* tok_end = p;
    if (SuiteCsv_NextToken(&p, &tok_end)) SuiteCsv_CopyToken(r->token, sizeof(r->token), p, tok_end);
    else r->token[0] = '\0';
    r->count    = 0;
    r->stop     = SUITE_CSV_UNITS_DIFFER;
    r->stop_pos = 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteCsv_RefuseBroadcast]
//======================================================================

//======================================================================
// [FUNCTION]_[SuiteCsv_StopAtRepeat]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[stop a parsed list at the first value that repeats an earlier one (SUITE_CSV_DUPLICATE) — for a list whose values each name a distinct thing; the values before it are kept, as at any stop, and a repeat before an earlier stop's position takes its place; SuiteCsv_RepeatsEarlier is the ONE "distinct" rule (StartGate_FirstBadHorizon reads it too)]
//======================================================================
// The Horizons list is one: each horizon trains its own model into its own directory, so a repeat sent two jobs at
// one artifact — the same model file, the same stamp's temp path, the same records — and the run counted both as
// trained (the 2026-10-03 run review, M1). TP / SL / kind lists repeat values by design; they do not call this.
//======================================================================
// [CODE]
//======================================================================
template <typename T>
inline bool SuiteCsv_RepeatsEarlier(const T* values, int i) {
    for (int j = 0; j < i; ++j)
        if (values[j] == values[i]) return true;
    return false;
}
template <typename T>
inline void SuiteCsv_StopAtRepeat(SuiteCsvParse* r, const T* values, const char* csv) {
    for (int i = 1; i < r->count; ++i) {
        if (!SuiteCsv_RepeatsEarlier(values, i)) continue;
        const char* p       = csv ? csv : "";
        const char* tok_end = p;
        for (int k = 0; k <= i && SuiteCsv_NextToken(&p, &tok_end); ++k)   // the (i+1)-th token is the repeat's text
            if (k < i) p = tok_end;
        SuiteCsv_CopyToken(r->token, sizeof(r->token), p, tok_end);
        r->count    = i;
        r->stop     = SUITE_CSV_DUPLICATE;
        r->stop_pos = i + 1;
        return;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteCsv_StopAtRepeat]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_FirstBadHorizon]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the horizons a run may train: each 1..MODEL_HORIZON_TICKS_MAX (a model path's grammar — the loader walks no other) and each distinct (its own model directory) — the index of the first that is not, or -1; the producer core refuses on it, whoever built the request]
//======================================================================
// The panel reaches the same verdict field by field — its parse ranges each value to the same bound and stops at a
// repeat through the same rule (SuiteCsv_RepeatsEarlier) — so the core's check is the request's own guarantee, not a
// second opinion (the run review's M1; its fold review's NF-5 / option A: a horizon of 0 wrote horizon_0/, which the
// loader's grammar rejects, and stamped no label parameters at all).
//======================================================================
// [CODE]
//======================================================================
inline int StartGate_FirstBadHorizon(const int* horizons, int n) {
    for (int i = 0; i < n; ++i)
        if (horizons[i] < 1 || horizons[i] > MODEL_HORIZON_TICKS_MAX || SuiteCsv_RepeatsEarlier(horizons, i)) return i;
    return -1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_FirstBadHorizon]
//======================================================================

//======================================================================
// [FUNCTION]_[SuiteCsv_WriteValue]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[write one value as a CSV field's text — the shortest fixed-notation form that parses back to exactly v (std::to_chars; the shortest of any form if fixed cannot fit), locale-free; "" if nothing fits]
//======================================================================
// A field seeded from a stored value shows what a start will apply: "%.3f" showed 0.0005 as "0.001". Fixed notation is
// what an operator types ("0.0005", not "5e-04"); any float's fits a 64-byte field. std::to_chars ignores the locale —
// the "%.3f" seed leaned on the suite's LC_NUMERIC boot pin for its decimal point.
//======================================================================
// [CODE]
//======================================================================
template <typename T>
inline void SuiteCsv_WriteValue(char* csv, size_t cap, T v) {
    if (!csv || cap == 0) return;
    std::to_chars_result w = std::to_chars(csv, csv + cap - 1, v, std::chars_format::fixed);
    if (w.ec != std::errc()) w = std::to_chars(csv, csv + cap - 1, v);
    *(w.ec == std::errc() ? w.ptr : csv) = '\0';
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteCsv_WriteValue]
//======================================================================

//======================================================================
// [FUNCTION]_[SuiteCsv_ErrorLine]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the red line a field that cannot be applied as typed shows at the field — what it stopped at, where, and why; false (and "") when it parsed whole]
//======================================================================
// "Collect and Train refuse": the four collect / train starts read the fields (StartGate_NeedCsvs). Walk-Forward, the HP
// sweep and Full Validation run on the collected labels; of the fields they read only the Horizons parse, for the purge,
// which Label_PurgeCovering widens to the labels' own horizon.
//======================================================================
// [CODE]
//======================================================================
inline bool SuiteCsv_ErrorLine(const char* field, const SuiteCsvParse* r, char* out, size_t out_cap) {
    if (out_cap) out[0] = '\0';
    if (!r || r->stop == SUITE_CSV_OK) return false;
    const char* tail = "Collect and Train refuse until it is fixed";
    if (r->stop == SUITE_CSV_TOO_MANY)
        snprintf(out, out_cap, "⚠ %s: more than %d values — position %d ('%s') and the rest are not applied; %s", field,
                 r->cap, r->stop_pos, r->token, tail);
    else if (r->stop == SUITE_CSV_OUT_OF_RANGE)
        snprintf(out, out_cap, "⚠ %s position %d: '%s' is outside [%.15g, %.15g] — it and the values after it are not "
                 "applied; %s", field, r->stop_pos, r->token, r->lo, r->hi, tail);
    else if (r->stop == SUITE_CSV_DUPLICATE)
        snprintf(out, out_cap, "⚠ %s position %d: '%s' repeats an earlier value — each must be distinct; it and the values "
                 "after it are not applied; %s", field, r->stop_pos, r->token, tail);
    else if (r->stop == SUITE_CSV_UNITS_DIFFER)
        snprintf(out, out_cap, "⚠ %s: one value ('%s') for horizons whose units differ — give one value per horizon; %s",
                 field, r->token, tail);
    else
        snprintf(out, out_cap, "⚠ %s position %d: '%s' is not a number — it and the values after it are not applied; %s",
                 field, r->stop_pos, r->token, tail);
    return true;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteCsv_ErrorLine]
//======================================================================


// Whether this build can train (XGBoost linked). The panel passes it; the cells pass both values.
#ifdef USE_XGBOOST
inline constexpr bool START_GATE_BUILD_TRAINS = true;
#else
inline constexpr bool START_GATE_BUILD_TRAINS = false;
#endif

// The samples each training action needs before its result can mean anything — each reason prints its own floor.
inline constexpr int START_GATE_TRAIN_MIN_SAMPLES = 10;
inline constexpr int START_GATE_WF_MIN_SAMPLES    = 50;
inline constexpr int START_GATE_HP_MIN_SAMPLES    = 100;
inline constexpr int START_GATE_FV_MIN_SAMPLES    = 50;

// Every gate FAILS CLOSED on an input the panel can never produce (a negative count, an unknown verdict): a caller's
// bug — a headless verb's included — refuses, it does not start.

// v5.11.40's broadcast-or-match rule for a per-horizon CSV: one value (or none) broadcasts to every horizon, N values
// map one per horizon; any other count — a negative one included — contradicts the horizons, and only an edit clears it.
inline bool StartGate_BroadcastsOrMatches(int n, int horizons) { return n == 0 || n == 1 || (n > 1 && n == horizons); }

//======================================================================
// [FUNCTION]_[StartGate_SelectedFiles]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the selected files among the files a scan found — the files term's input: only the first file_count flags count, never past capacity]
//======================================================================
// A selection left over from a scan that found more files, or none, cannot count: the Data panel used to keep this as
// a stored count its own recount skipped when a scan found nothing, so the count could open a gate onto zero files
// (D-507 review F7).
//======================================================================
// [CODE]
//======================================================================
inline int StartGate_SelectedFiles(const bool* selected, int file_count, int capacity) {
    if (!selected) return 0;
    const int n = file_count < capacity ? file_count : capacity;
    int count = 0;
    for (int i = 0; i < n; i++)
        count += selected[i] ? 1 : 0;
    return count;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_SelectedFiles]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_RekeySelection]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[a rescan's selection, keyed by the FILE: a file in the new list is selected iff its name was selected before; both lists sorted by name (the scan sorts them), so a merge — never an index; returns how many stay selected]
//======================================================================
// A selection used to outlive a rescan by index: rescanning another directory selected that directory's files at the
// old positions, and a new file sorted into the middle of the same directory shifted every selection after it (D-507
// review F7). A merge on the name can only re-select a file that was selected — out-of-order input loses selections,
// it never invents one.
//======================================================================
// [CODE]
//======================================================================
inline int StartGate_RekeySelection(const char (*kept)[256], int kept_n, const char (*files)[256], int file_count,
                                    int capacity, bool* selected) {
    if (!selected) return 0;
    const int n = file_count < capacity ? file_count : capacity;
    for (int i = 0; i < n; i++)
        selected[i] = false;
    if (!kept || !files) return 0;
    int i = 0, k = 0, count = 0;
    while (i < n && k < kept_n) {
        const int c = strncmp(files[i], kept[k], 256);
        if (c == 0)     { selected[i] = true; count++; i++; k++; }
        else if (c < 0) { i++; }
        else            { k++; }
    }
    return count;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_RekeySelection]
//======================================================================

// The terms more than one gate shares — each worded once.
inline void StartGate_NeedBuildTrains(SuiteGate* g, bool build_trains) {
    SuiteGate_Need(g, build_trains, "Build with -DUSE_XGBOOST=ON");
}
inline void StartGate_NeedFiles(SuiteGate* g, int selected_files) {
    SuiteGate_Need(g, selected_files > 0, "Select data files first");
}
// E.1.3 MP-6 step 10.3 (F2 of its review) — the Horizons CSV is the panel's ONE source of label horizons: a start with
// none typed refuses, where single mode used to label at the last value typed (shown nowhere) and multi mode named a
// cfg.horizon_list fallback that no longer exists.
inline void StartGate_NeedHorizons(SuiteGate* g, int horizons) {
    SuiteGate_Need(g, horizons > 0, "Type a horizon in Horizons (CSV)");   // no "above": it sits below the Collect buttons
}
// E.1.3 MP-6 step 10.5 — a CSV field that cannot be applied as typed (the panel's TrainingPanel_CsvError: the first such
// field the start reads, nullptr when none — a stopped parse, or one value for horizons whose units differ) refuses the
// start and names the field; its red line says where and why. Before the horizons term, so a typo ("0") is named as one
// rather than as "no horizon". A FIX: an input to correct. (A stopped TP CSV that kept one value used to broadcast it to
// every horizon — the gates opened on it.)
inline void StartGate_NeedCsvs(SuiteGate* g, const char* bad_csv) {
    SuiteGate_NeedFix(g, bad_csv == nullptr, "(%s: fix the value in red)", bad_csv ? bad_csv : "");
}
// side_gate is Training_SideLabelGate's verdict over the label set (0 refuse, 1 warn, 2 ok); the panel says why above.
// Anything but a warn or an ok refuses.
inline void StartGate_NeedSideAccepts(SuiteGate* g, int side_gate) {
    SuiteGate_Need(g, side_gate > 0, "label refused for this training side (above)");
}
inline void StartGate_NeedTrainSamples(SuiteGate* g, int samples) {
    SuiteGate_Need(g, samples >= START_GATE_TRAIN_MIN_SAMPLES, "Collect features first (need %d+ samples)",
                   START_GATE_TRAIN_MIN_SAMPLES);
}
inline void StartGate_NeedAlignedTpSl(SuiteGate* g, int horizons, int tp_n, int sl_n) {
    SuiteGate_NeedFix(g, StartGate_BroadcastsOrMatches(tp_n, horizons) && StartGate_BroadcastsOrMatches(sl_n, horizons),
                      "(misaligned: TP=%d, SL=%d, horizons=%d — need 1 or %d each)", tp_n, sl_n, horizons, horizons);
}
// v5.13.1.B — the Label Kind CSV follows the same broadcast-or-match rule as TP / SL; both multi-horizon starts read it
// (E.1.3 MP-6 step 10.5: the collect labelled with a misaligned list — resolved silently — that the train then refused).
inline void StartGate_NeedAlignedKinds(SuiteGate* g, int horizons, int lk_n) {
    SuiteGate_NeedFix(g, StartGate_BroadcastsOrMatches(lk_n, horizons),
                      "(misaligned: Label Kind=%d, horizons=%d — need 1 or %d)", lk_n, horizons, horizons);
}

//======================================================================
// [FUNCTION]_[StartGate_NeedGrid]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE grid rule both sweeps' results can hold, as gate terms in order: 1..max_axes swept parameters (the ranges and rows the sweeps index), every axis has a step, no axis has more than max_steps (the parameter-value rows), the cells within max_cells (the per-cell arrays)]
//======================================================================
// Each axis is checked on its own: two empty (inverted) ranges multiply to a positive count. The cells multiply in 64
// bits — two ints cannot overflow it. Each bound guards its own array — with today's constants (50 and 50 x 50) the
// cell term cannot fail once the axis terms pass; it holds the per-cell arrays if their cap ever drops below that. The
// sweeps refuse on the same rule (StartGate_GridFits — OptimizerGrid_Fits): a caller that skips the gate gets the same
// answer (D-507 review F2; the axis count joined at the second review's R2 — the panel's slider took a typed 3).
// steps_b is 1 for a one-parameter grid.
//======================================================================
// [CODE]
//======================================================================
inline void StartGate_NeedGrid(SuiteGate* g, int axes, int max_axes, int steps_a, int steps_b, int max_steps,
                               int max_cells) {
    SuiteGate_Need(g, axes >= 1 && axes <= max_axes, "Sweep 1 to %d parameters", max_axes);
    SuiteGate_Need(g, steps_a >= 1 && steps_b >= 1, "An axis has no steps — check its Min / Max / Step");
    SuiteGate_Need(g, steps_a <= max_steps && steps_b <= max_steps, "An axis has more than %d steps — widen its Step",
                   max_steps);
    SuiteGate_Need(g, (int64_t)steps_a * steps_b <= max_cells, "Too many combos (max %d)", max_cells);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_NeedGrid]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_GridFits]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the grid rule as a yes / no — the terms StartGate_NeedGrid states, never a second copy of them]
//======================================================================
// [CODE]
//======================================================================
inline bool StartGate_GridFits(int axes, int max_axes, int steps_a, int steps_b, int max_steps, int max_cells) {
    SuiteGate g;
    StartGate_NeedGrid(&g, axes, max_axes, steps_a, steps_b, max_steps, max_cells);
    return !g.closed;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_GridFits]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_RunBacktest]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Run Backtest's gate — the files to replay, then the suite free]
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_RunBacktest(int selected_files) {
    SuiteGate g;
    StartGate_NeedFiles(&g, selected_files);
    SuiteGate_NeedLease(&g);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_RunBacktest]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_GridSearch]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Grid Search's gate — the files, the grid rule (StartGate_NeedGrid: 1..max_axes parameters, every axis 1..max_steps, the cells within max_cells), then the suite free]
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_GridSearch(int selected_files, int axes, int max_axes, int steps_a, int steps_b,
                                      int max_steps, int max_cells) {
    SuiteGate g;
    StartGate_NeedFiles(&g, selected_files);
    StartGate_NeedGrid(&g, axes, max_axes, steps_a, steps_b, max_steps, max_cells);
    SuiteGate_NeedLease(&g);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_GridSearch]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_CollectFeatures]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Collect Features' gate — the files, the training side's verdict on the label (E.1.2.C F3), its CSV fields parsed whole, a horizon typed (the labels' horizon), then the suite free]
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_CollectFeatures(int selected_files, int side_gate, int horizons, const char* bad_csv) {
    SuiteGate g;
    StartGate_NeedFiles(&g, selected_files);
    StartGate_NeedSideAccepts(&g, side_gate);
    StartGate_NeedCsvs(&g, bad_csv);
    StartGate_NeedHorizons(&g, horizons);
    SuiteGate_NeedLease(&g);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_CollectFeatures]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_CollectMultiHorizon]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Collect Multi-Horizon's gate — the files, its CSV fields parsed whole, the panel's horizons, TP / SL and Label Kind counts that agree with them, the training side's verdict on the label, then the suite free]
//======================================================================
// E.1.3 MP-6 step 10.5 — the Label Kind term is Train Multi-Horizon's: a collect that labels with a list the train
// would refuse summarises labels no model is trained on (the E.1.2.G rule — collect and train read the kinds alike).
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_CollectMultiHorizon(int selected_files, int horizons, int tp_n, int sl_n, int lk_n,
                                               int side_gate, const char* bad_csv) {
    SuiteGate g;
    StartGate_NeedFiles(&g, selected_files);
    StartGate_NeedCsvs(&g, bad_csv);
    StartGate_NeedHorizons(&g, horizons);
    StartGate_NeedAlignedTpSl(&g, horizons, tp_n, sl_n);
    StartGate_NeedAlignedKinds(&g, horizons, lk_n);
    StartGate_NeedSideAccepts(&g, side_gate);
    SuiteGate_NeedLease(&g);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_CollectMultiHorizon]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_TrainModel]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Train Model's gate — a build that trains, the training side's verdict on the label, its CSV fields parsed whole, a horizon typed (the one it trains), the suite free, then the samples a collect must produce]
//======================================================================
// E.1.2.C — the side term is the half F3 was missing: the verdict used to reach only the two COLLECT gates, so a
// REFUSE-tier label could still be TRAINED from samples an earlier collect had left behind (collect at side=Buy, flip
// to Exit, pick any label, Train) — the tier rendered red and stopped nothing.
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_TrainModel(bool build_trains, int side_gate, int horizons, int samples, const char* bad_csv) {
    SuiteGate g;
    StartGate_NeedBuildTrains(&g, build_trains);
    StartGate_NeedSideAccepts(&g, side_gate);
    StartGate_NeedCsvs(&g, bad_csv);
    StartGate_NeedHorizons(&g, horizons);
    SuiteGate_NeedLease(&g);
    StartGate_NeedTrainSamples(&g, samples);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_TrainModel]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_TrainMultiHorizon]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Train Multi-Horizon's gate — Train Model's (its CSV fields parsed whole among them), with the horizons and their TP / SL and Label Kind counts between the side's verdict and the suite]
//======================================================================
// v5.13.1.B — the Label Kind term (StartGate_NeedAlignedKinds): its misalignment used to grey the button with no reason
// shown.
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_TrainMultiHorizon(bool build_trains, int side_gate, int horizons, int tp_n, int sl_n,
                                             int lk_n, int samples, const char* bad_csv) {
    SuiteGate g;
    StartGate_NeedBuildTrains(&g, build_trains);
    StartGate_NeedSideAccepts(&g, side_gate);
    StartGate_NeedCsvs(&g, bad_csv);
    StartGate_NeedHorizons(&g, horizons);
    StartGate_NeedAlignedTpSl(&g, horizons, tp_n, sl_n);
    StartGate_NeedAlignedKinds(&g, horizons, lk_n);
    SuiteGate_NeedLease(&g);
    StartGate_NeedTrainSamples(&g, samples);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_TrainMultiHorizon]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_WalkForward]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Run Walk-Forward's gate — a build that trains, the suite free, then the samples]
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_WalkForward(bool build_trains, int samples) {
    SuiteGate g;
    StartGate_NeedBuildTrains(&g, build_trains);
    SuiteGate_NeedLease(&g);
    SuiteGate_Need(&g, samples >= START_GATE_WF_MIN_SAMPLES, "Need %d+ samples", START_GATE_WF_MIN_SAMPLES);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_WalkForward]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_HyperparamSweep]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Run Hyperparam Sweep's gate — a build that trains, the grid rule (StartGate_NeedGrid — the same one Grid Search's gate and both sweeps use), the suite free, then the samples]
//======================================================================
// Each axis is checked on its own and the cells multiply in 64 bits, for the reasons StartGate_GridSearch gives.
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_HyperparamSweep(bool build_trains, int axes, int max_axes, int steps_a, int steps_b,
                                           int max_steps, int max_cells, int samples) {
    SuiteGate g;
    StartGate_NeedBuildTrains(&g, build_trains);
    StartGate_NeedGrid(&g, axes, max_axes, steps_a, steps_b, max_steps, max_cells);
    SuiteGate_NeedLease(&g);
    SuiteGate_Need(&g, samples >= START_GATE_HP_MIN_SAMPLES, "Need %d+ samples (Collect Features first)",
                   START_GATE_HP_MIN_SAMPLES);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_HyperparamSweep]
//======================================================================

//======================================================================
// [FUNCTION]_[StartGate_FullValidation]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI] [ML]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Run Full Validation's gate — a build that trains, the model to validate, a model of the collected labels' horizon, the suite free, then the samples]
//======================================================================
// Full Validation validates the model against the collected labels and re-stamps it with their identity, so a model
// of another horizon would be scored on labels it never predicted and stamped with a horizon it was never trained on
// (D-507 second review, R1 — after a multi-horizon collect the labels are the LAST horizon's). model_horizon is the
// horizon_<N> directory the model file sits in (ModelPath_HorizonOfModelFile), <= 0 when its path names none;
// labels_horizon is the horizon the collected labels were made with, <= 0 when there are none (nothing collected, or a
// collect that made no samples — the samples term says so). Once there ARE labels the model must be checkable: a path
// naming no horizon directory is refused, not passed unchecked (the third review, A1 — the Model Path field's default
// is a flat path). The labels' other params (kind, barriers) are MP-3a's: there the stamp takes them from the model's
// own training record, and a flat model with such a record can validate again.
//======================================================================
// [CODE]
//======================================================================
inline SuiteGate StartGate_FullValidation(bool build_trains, bool has_model_path, long model_horizon, int labels_horizon,
                                          int samples) {
    SuiteGate g;
    StartGate_NeedBuildTrains(&g, build_trains);
    SuiteGate_Need(&g, has_model_path, "Set Model Path first");
    SuiteGate_NeedFix(&g, labels_horizon <= 0 || model_horizon > 0, "(Model Path is not in a horizon_<N> dir)");
    SuiteGate_NeedFix(&g, labels_horizon <= 0 || model_horizon == labels_horizon,
                      "(model horizon %ld, labels horizon %d — collect %ld)", model_horizon, labels_horizon,
                      model_horizon);
    SuiteGate_NeedLease(&g);
    SuiteGate_Need(&g, samples >= START_GATE_FV_MIN_SAMPLES, "Need %d+ samples", START_GATE_FV_MIN_SAMPLES);
    return g;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[StartGate_FullValidation]
//======================================================================
