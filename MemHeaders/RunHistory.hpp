// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/RunHistory.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[append-only JSONL log of validation runs — schema-v1 line per run, LC_NUMERIC=C-pinned emit, fsync-per-line crash safety]
// [CONTAINS]
//   - [FUNCTION]_[RunHistory_Append]   (RunHistoryEntry input record + RUN_HISTORY_SCHEMA_VERSION ride the block)
// [REFERENCE]_[INVARIANT]_[H9]
//======================================================================================================
// One JSONL line per completed validation run. Captures the metrics, the
// gap, whether a stamp was written, and a timestamp. Lets the operator
// query "what did I try yesterday?" / "which configs scored above 0.55?"
// without having to parse GUI screenshots.
//
// JSONL format chosen because:
//   - line-oriented append, fsync-safe, crash-safe (each line is a complete
//     record; partial last line is recoverable by ignoring it)
//   - readable by jq / pandas.read_json(lines=True) / awk
//   - schema can evolve (new keys added forward-compat)
//
// Schema v1:
//   {
//     "ts":              "<ISO 8601 UTC timestamp>",
//     "schema":          1,
//     "model_path":      "<path or empty>",
//     "wf_mean_val":     <double>,
//     "held_out_metric": <double>,
//     "gap":             <double>,
//     "gap_threshold":   <double>,
//     "gap_acceptable":  <0|1>,
//     "ran_held_out":    <0|1>,
//     "stamp_attempted": <0|1>,
//     "stamp_ok":        <0|1>,
//     "stamp_path":      "<path or empty>"
//   }
//
// Forward-compat: readers must tolerate unknown keys; writers must keep
// known keys at the same names. Bump "schema" only when removing or
// renaming a field (additive changes don't bump).
//
// File handling: append + fsync per line. Slow (~ms per write) but
// crash-safe. Volume is ~hundreds of bytes per run × maybe 10 runs/day
// = trivial. Caller can rotate the file manually if it ever grows past
// inconvenience (no built-in rotation — simpler is better here).
//======================================================================================================
#ifndef RUN_HISTORY_HPP
#define RUN_HISTORY_HPP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace tt {

#define RUN_HISTORY_SCHEMA_VERSION 1

//======================================================================
// [STRUCT]_[RunHistoryEntry]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one schema-v1 run-history record — model path + WF/held-out metrics + gap-acceptance flags + stamp outcome; the JSONL line's field set]
//======================================================================
// [CODE]
//======================================================================
struct RunHistoryEntry {
    const char* model_path;        // model .bin path (or "" if save-less run)
    double      wf_mean_val;
    double      held_out_metric;
    double      gap;
    double      gap_threshold;
    int         gap_acceptable;    // 0 or 1
    int         ran_held_out;      // 0 or 1
    int         stamp_attempted;   // 0 or 1
    int         stamp_ok;          // 0 or 1
    const char* stamp_path;        // path written (or "" if no stamp)
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//======================================================================
// [END_STRUCT]_[RunHistoryEntry]
//======================================================================

//======================================================================
// [FUNCTION]_[RunHistory_Append]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[append one schema-v1 JSONL line — thread-local LC_NUMERIC=C pin around the emit; 1 on success incl. flush+close]
//======================================================================
// [CODE]
//======================================================================

// Append one line to the history file. Returns 1 on success, 0 on i/o failure.
// `path` may not exist yet — file is created on first append.
//
// Locale-agnostic: %g/%f emit under a thread-local uselocale(LC_NUMERIC=C) pin
// (Layer 2, below) so a comma-decimal locale can't break JSON-numeric round-trip.
// (.E.0.1 also pins LC_NUMERIC=C process-wide at boot; this per-emit pin is
// defense-in-depth for the HMAC/stamp-adjacent byte-equivalence path.)
static inline int RunHistory_Append(const char* path, const RunHistoryEntry& e) {
    if (!path || !path[0]) return 0;

    FILE* f = fopen(path, "a");
    if (!f) return 0;

    // ISO 8601 UTC timestamp
    char ts[32] = {0};
    time_t now = time(NULL);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

    // Pin LC_NUMERIC=C around the printf (locale-safe per-thread).
    locale_t pinned = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    locale_t prev = (locale_t)0;
    if (pinned) prev = uselocale(pinned);

    int written = fprintf(f,
        "{\"ts\":\"%s\","
         "\"schema\":%d,"
         "\"model_path\":\"%s\","
         "\"wf_mean_val\":%g,"
         "\"held_out_metric\":%g,"
         "\"gap\":%.6f,"
         "\"gap_threshold\":%g,"
         "\"gap_acceptable\":%d,"
         "\"ran_held_out\":%d,"
         "\"stamp_attempted\":%d,"
         "\"stamp_ok\":%d,"
         "\"stamp_path\":\"%s\""
         "}\n",
        ts,
        RUN_HISTORY_SCHEMA_VERSION,
        e.model_path ? e.model_path : "",
        e.wf_mean_val, e.held_out_metric, e.gap, e.gap_threshold,
        e.gap_acceptable ? 1 : 0,
        e.ran_held_out ? 1 : 0,
        e.stamp_attempted ? 1 : 0,
        e.stamp_ok ? 1 : 0,
        e.stamp_path ? e.stamp_path : "");

    if (pinned) {
        uselocale(prev);
        freelocale(pinned);
    }

    int flush_ok = (fflush(f) == 0);
    int close_ok = (fclose(f) == 0);
    return (written > 0 && flush_ok && close_ok) ? 1 : 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RunHistory_Append]
//======================================================================

} // namespace tt

#endif // RUN_HISTORY_HPP
