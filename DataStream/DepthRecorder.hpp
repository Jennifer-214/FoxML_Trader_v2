// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[DataStream/DepthRecorder.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[orderbook snapshot CSV capture for replay + audit (TickRecorder sibling) — top-of-book rows via locale-immune std::to_chars (F-055/PARITY-036) + the "# GAP" marker audit contract]
// [CONTAINS]
//   - [STRUCT]_[DepthRecorder]
//   - [FUNCTION]_[DepthRecorder_Write]   (+ MkdirP / DateInt / OpenFile / PruneOld / Init / LogGap / Close family)
//======================================================================================================
// records orderbook snapshots to CSV for replay + audit. sibling of TickRecorder.
//
// CSV format: timestamp_us,last_update_id,bid_price,bid_qty,ask_price,ask_qty
//   one row per parsed @depth5@100ms snapshot (top-of-book only initially).
//
// gap markers: a comment line "# GAP at_us=X reason=Y" inserted on:
//   - last_update_id going BACKWARD between consecutive snapshots
//     (real signal of reconnect-to-stale-snapshot)
//   - wallclock between consecutive snapshots > 2 seconds
//     (real signal of WS silence — connection dead but socket still open)
//   - explicit DepthRecorder_LogGap call from disconnect site
//
// what is NOT a gap: lastUpdateId jumping by 50-500 between consecutive
// snapshots. That's normal — book updates much faster than the 10Hz feed.
// Filtering on this would spam markers every message.
//
// features:
//   - daily file rotation: data/{symbol}/depth/YYYY-MM-DD.csv
//   - auto-prune: deletes CSVs older than max_days
//   - disabled by default (record_depth=0 in engine.cfg)
//   - zero overhead when disabled (all calls are gated on enabled flag)
//   - allocation-free per-snapshot path (Write is called ~10 Hz)
//
// disk estimate: ~50 MB/day for BTCUSDT @depth5@100ms.
// at max_days=30, caps at ~1.5 GB.
//======================================================================================================
#ifndef DEPTH_RECORDER_HPP
#define DEPTH_RECORDER_HPP

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <charconv>  // F-055/PARITY-036: std::to_chars locale-immune lossless emit
#include "BinanceDepth.hpp" // BookSnapshot<F>

//======================================================================
// [STRUCT]_[DepthRecorder]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[FILE* + rotation day + retention + the recorder-owned gap-detection state (last_seen_id/wallclock; 0 = no prior snapshot this run)]
//======================================================================
// [CODE]
//======================================================================
struct DepthRecorder {
    FILE *file;
    uint64_t count;
    int enabled;
    int current_day;        // YYYYMMDD of currently open file (triggers rotation)
    char symbol[16];
    char data_dir[256];     // "{base_dir}/{SYMBOL}/depth/"
    uint32_t max_days;      // auto-delete CSVs older than this
    // Gap-detection state — recorder owns this, not DepthSharedState.
    // last_seen_id == 0 means "no prior snapshot in this run" — skip gap check
    // on first _Write so a fresh-startup write doesn't false-positive.
    // _LogGap resets last_seen_id to 0 to suppress immediate re-flag.
    uint64_t last_seen_id;
    uint64_t last_seen_wallclock_us;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — layout emitter cannot probe this block yet; quartet lands when the emitter covers it, D-327)
//======================================================================
// [END_STRUCT]_[DepthRecorder]
//======================================================================

//======================================================================
// [FUNCTION]_[DepthRecorder_Write]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the recorder family (MkdirP / DateInt / OpenFile / PruneOld / Init / LogGap / Close ride) — ~10Hz allocation-free row emit + internal gap detection (id backward / >2s silence)]
//======================================================================
// [CODE]
//======================================================================
static inline void DepthRecorder_MkdirP(const char *path) {
    char tmp[300];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

//======================================================================================================
static inline int DepthRecorder_DateInt(uint64_t timestamp_us) {
    time_t t = (time_t)(timestamp_us / 1000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

//======================================================================================================
static inline void DepthRecorder_OpenFile(DepthRecorder *rec, int date_int) {
    if (rec->file) {
        fclose(rec->file);
        rec->file = NULL;
    }

    int year  = date_int / 10000;
    int month = (date_int / 100) % 100;
    int day   = date_int % 100;

    char path[512];
    snprintf(path, sizeof(path), "%s%04d-%02d-%02d.csv", rec->data_dir, year, month, day);

    // append mode — survives restarts within the same day
    int is_new = 0;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size == 0)
        is_new = 1;

    rec->file = fopen(path, "a");
    if (!rec->file) {
        // log once, leave file=NULL; subsequent writes gated on file pointer
        fprintf(stderr, "[depth-recorder] failed to open %s — recording disabled for this day\n", path);
        return;
    }

    if (is_new) {
        fprintf(rec->file, "timestamp_us,last_update_id,bid_price,bid_qty,ask_price,ask_qty\n");
        fflush(rec->file);
    }

    rec->current_day = date_int;
    fprintf(stderr, "[depth-recorder] recording to %s\n", path);
}

//======================================================================================================
static inline void DepthRecorder_PruneOld(DepthRecorder *rec) {
    if (rec->max_days == 0) return;

    time_t now = time(NULL);
    time_t cutoff = now - (time_t)rec->max_days * 86400;

    DIR *dir = opendir(rec->data_dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // match YYYY-MM-DD.csv pattern (length 14)
        if (strlen(entry->d_name) != 14) continue;
        if (strcmp(entry->d_name + 10, ".csv") != 0) continue;

        int year, month, day;
        if (sscanf(entry->d_name, "%d-%d-%d.csv", &year, &month, &day) != 3) continue;

        struct tm tm = {};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        time_t file_time = timegm(&tm);

        if (file_time < cutoff) {
            char path[512];
            snprintf(path, sizeof(path), "%s%s", rec->data_dir, entry->d_name);
            fprintf(stderr, "[depth-recorder] pruning old file: %s\n", path);
            remove(path);
        }
    }
    closedir(dir);
}

//======================================================================================================
static inline void DepthRecorder_Init(DepthRecorder *rec, const char *symbol,
                                       const char *base_dir, uint32_t max_days,
                                       int enabled) {
    memset(rec, 0, sizeof(*rec));
    rec->enabled = enabled;
    rec->max_days = max_days;
    if (!enabled) return;

    strncpy(rec->symbol, symbol, sizeof(rec->symbol) - 1);

    // build data directory path: {base_dir}/{SYMBOL}/depth/
    char upper_sym[16];
    int sym_len = (int)strlen(symbol);
    if (sym_len > 15) sym_len = 15;
    for (int i = 0; i < sym_len; i++)
        upper_sym[i] = (symbol[i] >= 'a' && symbol[i] <= 'z') ? symbol[i] - 32 : symbol[i];
    upper_sym[sym_len] = '\0';

    // ensure base_dir ends with '/' for clean concatenation
    int base_len = (int)strlen(base_dir);
    int needs_slash = (base_len > 0 && base_dir[base_len - 1] != '/');
    snprintf(rec->data_dir, sizeof(rec->data_dir), "%s%s%s/depth/",
             base_dir, needs_slash ? "/" : "", upper_sym);

    DepthRecorder_MkdirP(rec->data_dir);
    DepthRecorder_PruneOld(rec);

    fprintf(stderr, "[depth-recorder] initialized: symbol=%s, max_days=%u, dir=%s\n",
            upper_sym, max_days, rec->data_dir);
}

//======================================================================================================
// Append a "# GAP" comment line to the current file. Resets last_seen_id so
// the next _Write doesn't double-flag the same gap from its internal check.
// Caller passes the wallclock_us to record on the marker (typically the
// disconnect time, or the current snapshot's timestamp_us).
static inline void DepthRecorder_LogGap(DepthRecorder *rec, uint64_t at_us, const char *reason) {
    if (!rec->enabled) return;

    // Make sure a file is open (rotation may not have happened yet on session start).
    int today = DepthRecorder_DateInt(at_us);
    if (today != rec->current_day) {
        DepthRecorder_OpenFile(rec, today);
    }

    if (rec->file) {
        fprintf(rec->file, "# GAP at_us=%llu reason=%s last_seen_id=%llu\n",
                (unsigned long long)at_us,
                reason ? reason : "unknown",
                (unsigned long long)rec->last_seen_id);
        fflush(rec->file);
    }

    // Reset gap state so the next _Write skips its internal check (avoids
    // double-marking the same gap).
    rec->last_seen_id = 0;
    rec->last_seen_wallclock_us = 0;
}

//======================================================================================================
// Write one snapshot's top-of-book to the CSV. Does its own gap detection
// against last_seen_id / last_seen_wallclock_us — backward id jump or
// wallclock silence > 2s gets a "# GAP" line via DepthRecorder_LogGap.
template <unsigned F>
static inline void DepthRecorder_Write(DepthRecorder *rec, const BookSnapshot<F> *snap) {
    if (!rec->enabled) return;

    uint64_t cur_id = snap->last_update_id;
    uint64_t cur_us = snap->timestamp_us;

    // daily rotation
    int today = DepthRecorder_DateInt(cur_us);
    if (today != rec->current_day) {
        DepthRecorder_OpenFile(rec, today);
        DepthRecorder_PruneOld(rec); // once per day boundary
    }

    if (!rec->file) return;

    // Internal gap detection (amendment #2): backward id jump OR wallclock >2s.
    // last_seen_id == 0 means "first write this run" — skip the check.
    if (rec->last_seen_id != 0) {
        int backward = (cur_id < rec->last_seen_id);
        int wall_gap = (cur_us > rec->last_seen_wallclock_us &&
                        cur_us - rec->last_seen_wallclock_us > 2000000ULL);
        if (backward || wall_gap) {
            DepthRecorder_LogGap(rec, cur_us, backward ? "id_backward" : "wallclock_gap");
            // _LogGap zeroed last_seen_id — re-establish below from current snapshot
        }
    }

    // CSV row: timestamp_us,last_update_id,bid_price,bid_qty,ask_price,ask_qty
    // F-055/PARITY-036: locale-immune + lossless emit (std::to_chars shortest round-trip)
    // — replaces lossy/LC_NUMERIC-fragile %.8f; completes the write∧read replay loop.
    // Each to_chars gets rend-1 so the trailing separator byte is PROVABLY in-bounds even on
    // to_chars' value-too-large path (ptr==last): worst-case row is 142B of 160 so the limit is
    // arithmetically unreachable, but provable > unreachable-by-arithmetic (TECH_DEBT-160; the
    // unchecked ptr + *o++ shape was a real 1-byte-overflow latent under the type contract).
    char row[160]; char* o = row; char* const rend = row + sizeof(row);
    o = std::to_chars(o, rend - 1, (unsigned long long)cur_us).ptr;          *o++ = ',';
    o = std::to_chars(o, rend - 1, (unsigned long long)cur_id).ptr;          *o++ = ',';
    o = std::to_chars(o, rend - 1, FPN_ToDouble(snap->bids[0].price)).ptr;   *o++ = ',';
    o = std::to_chars(o, rend - 1, FPN_ToDouble(snap->bids[0].qty)).ptr;     *o++ = ',';
    o = std::to_chars(o, rend - 1, FPN_ToDouble(snap->asks[0].price)).ptr;   *o++ = ',';
    o = std::to_chars(o, rend - 1, FPN_ToDouble(snap->asks[0].qty)).ptr;     *o++ = '\n';
    fwrite(row, 1, (size_t)(o - row), rec->file);
    rec->count++;

    // flush every 256 snapshots (~26 sec at 10Hz, balance I/O and durability)
    if ((rec->count & 0xFF) == 0)
        fflush(rec->file);

    rec->last_seen_id = cur_id;
    rec->last_seen_wallclock_us = cur_us;
}

//======================================================================================================
static inline void DepthRecorder_Close(DepthRecorder *rec) {
    if (rec->file) {
        fflush(rec->file);
        fclose(rec->file);
        rec->file = NULL;
    }
    if (rec->enabled)
        fprintf(stderr, "[depth-recorder] closed after %llu snapshots\n",
                (unsigned long long)rec->count);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[DepthRecorder_Write]
//======================================================================

#endif // DEPTH_RECORDER_HPP
