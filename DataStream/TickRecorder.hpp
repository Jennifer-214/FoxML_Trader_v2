// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[DataStream/TickRecorder.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[raw tick CSV capture for datasets — aggTrades-compatible rows via locale-immune std::to_chars (F-054/PARITY-036), daily rotation into _staging/, auto-prune]
// [CONTAINS]
//   - [STRUCT]_[TickRecorder]
//   - [FUNCTION]_[TickRecorder_Push]   (+ MkdirP / DateInt / OpenFile / PruneOld / Init / Close family)
//======================================================================================================
// records raw tick data to CSV for building historical datasets for backtesting and ML training.
// outputs Binance aggTrades-compatible format: timestamp_us,price,quantity,is_buyer_maker
//
// features:
//   - daily file rotation: data/{symbol}/YYYY-MM-DD.csv
//   - auto-prune: deletes CSVs older than max_days (prevents disk fill)
//   - disabled by default (record_ticks=0 in engine.cfg)
//   - zero overhead when disabled (all calls are gated on enabled flag)
//
// disk estimate: ~30-70MB/day for BTCUSDT, ~1-2GB/month
// at max_days=30, caps at ~2GB
//======================================================================================================
#ifndef TICK_RECORDER_HPP
#define TICK_RECORDER_HPP

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <charconv>  // F-054/PARITY-036: std::to_chars locale-immune lossless emit

//======================================================================
// [STRUCT]_[TickRecorder]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[FILE* + rotation day + symbol/dir + max_days retention — zero overhead when disabled (every call gates on the flag)]
//======================================================================
// [CODE]
//======================================================================
struct TickRecorder {
    FILE *file;
    uint64_t count;
    int enabled;
    int current_day;        // YYYYMMDD of currently open file (triggers rotation)
    char symbol[16];
    char data_dir[256];     // "data/{symbol}/"
    uint32_t max_days;      // auto-delete CSVs older than this
    // D-472 — COVERAGE. Which time ranges this process was actually recording.
    // NOT which trades it captured: a reconciler does not need per-trade identity,
    // it needs to know what it was AWAKE for. A gap in TRADES is ambiguous (quiet
    // market, or not running?); a gap in SESSIONS is not. That distinction is why
    // no aggTrade id is carried here, and why Tick / the WS parser / the hot ingest
    // path stay untouched.
    int64_t  session_open_us;   // data timestamp of this session's first recorded tick (0 = none yet)
    int64_t  session_last_us;   // data timestamp of the most recent recorded tick
    char     coverage_path[512];// sidecar beside the CSV; "" when disabled
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-09-01]
// [SIZE]_[832B]
// [ALIGN]_[8]
// [CACHE_LINES]_[13]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[TickRecorder]
//======================================================================

//======================================================================
// [FUNCTION]_[TickRecorder_Push]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the recorder family (MkdirP / DateInt / OpenFile / PruneOld / Init / Close ride) — per-tick to_chars row + daily rotation + rotation-time prune; flush every 1024 ticks]
// [REFERENCE]_[PARITY]_[PARITY-36]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-160]
//======================================================================
// [CODE]
//======================================================================
static inline void TickRecorder_MkdirP(const char *path) {
    char tmp[256];
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
static inline int TickRecorder_DateInt(int64_t timestamp_us) {
    // BUG-FIX: parameter is MICROSECONDS (per name + caller contract), so we
    // need /1e6 to get seconds. Pre-fix used /1000 which treated input as
    // milliseconds — produced years like 58286 instead of 2026 because the
    // resulting "time_t" was 1000× too large. Burned through one new file
    // per tick because each tick's "day" computed differently.
    time_t t = (time_t)(timestamp_us / 1000000LL);
    struct tm tm;
    gmtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

//======================================================================================================
static inline void TickRecorder_OpenFile(TickRecorder *rec, int date_int) {
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
        // Class 62 — this early return used to strand current_day / coverage_path /
        // the session window, leaving the PREVIOUS day's coverage_path live while no
        // file is open. A later seal would then append this session's window to the
        // WRONG day's sidecar: coverage claiming a day the engine never recorded,
        // which is precisely the over-claim the manifest exists to prevent.
        // Clearing coverage_path makes CoverageSeal a no-op by its own guard, so the
        // failure degrades to NOT-COVERED — the conservative reading (re-fetched)
        // rather than the silent one. current_day is deliberately left UNSET so the
        // next push retries the rotation (pre-existing behaviour, preserved).
        rec->coverage_path[0] = '\0';
        rec->session_open_us  = 0;
        rec->session_last_us  = 0;
        fprintf(stderr, "[tick-recorder] failed to open %s — this day will read as "
                        "NOT-COVERED\n", path);
        return;
    }

    if (is_new) {
        fprintf(rec->file, "timestamp_us,price,quantity,is_buyer_maker\n");
        fflush(rec->file);
    }

    // D-472 — the coverage sidecar, opened in the SAME function as the CSV so the
    // two cannot drift apart under a future edit. It lives BESIDE the tape rather
    // than inside it: the CSV's other producer is a curl|unzip of Binance's daily
    // dump, and putting engine-specific marker rows in it would stop the two being
    // byte-interchangeable — which is the entire property that lets a downloaded
    // day substitute for a recorded one.
    snprintf(rec->coverage_path, sizeof(rec->coverage_path),
             "%s%04d-%02d-%02d.coverage", rec->data_dir, year, month, day);
    rec->session_open_us = 0;   // set by the first push of this file
    rec->session_last_us = 0;

    rec->current_day = date_int;
    fprintf(stderr, "[tick-recorder] recording to %s\n", path);
}

//======================================================================================================
// [FUNCTION]_[TickRecorder_CoverageSeal]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[append one closed coverage window to the day's sidecar — the record of what this process was awake for (D-472)]
// [REFERENCE]_[DECISION]_[D-472]
//======================================================================================================
// [CODE]
//======================================================================================================
// Appended, never rewritten: one line per (session x day), so a day recorded across
// several runs accumulates several windows and the union is the coverage.
//
// AN UNTERMINATED SESSION IS THE INCOMPLETENESS SIGNAL, and that is by design. A
// clean shutdown (or a day rotation) seals its window here; a crash does not. So a
// day whose sidecar is missing, or whose windows do not span it, is exactly the
// "this segment is incomplete, prefer the authoritative dump" case the reconciler
// must act on — the format ENCODES the failure instead of needing a heuristic to
// infer it. A reconciler must therefore treat ABSENT coverage as NOT-COVERED, never
// as "probably fine": the conservative reading over-downloads, and over-downloading
// is free while trusting a partial tape is not.
//
// Timestamps are DATA timestamps (the tick's own), never wall clock — the same
// discipline the warm-start replay depends on (D-472; live stamps now(), backtest
// threads tick.timestamp, and only the latter reconstructs real elapsed time).
static inline void TickRecorder_CoverageSeal(TickRecorder *rec) {
    if (!rec->enabled || !rec->coverage_path[0]) return;
    if (rec->session_open_us == 0) return;    // nothing recorded into this file
    FILE *cf = fopen(rec->coverage_path, "a");
    if (!cf) {
        fprintf(stderr, "[tick-recorder] WARN: cannot write coverage %s — this day will read as "
                        "NOT-COVERED and be re-fetched\n", rec->coverage_path);
        return;
    }
    fprintf(cf, "%lld,%lld,%lu\n", (long long)rec->session_open_us,
            (long long)rec->session_last_us, (unsigned long)rec->count);
    fclose(cf);
    rec->session_open_us = 0;
}
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [END_FUNCTION]_[TickRecorder_CoverageSeal]
//======================================================================================================

//======================================================================================================
// ⚠️ D-472 REPLAY HAZARD, named before stage 3 makes it live. This prunes by the
// DATE ENCODED IN THE FILENAME against wall-clock `now`, and it runs on every day
// ROTATION — which a push with a historical timestamp triggers. Feeding 72 h-old
// ticks through TickRecorder_Push (exactly what a warm-start replay does) therefore
// OPENS the historical day's file and then DELETES it, and the deletion is silent
// apart from one stderr line. Unreachable in production today (live ticks are always
// `today`), reachable the moment a replay writes through a recorder.
// The stage-3 fix is to not route replay through the recorder at all — a replay
// re-reads the tape, it does not re-record it — but if that ever changes, this
// function needs a replay guard, not a wider max_days.
static inline void TickRecorder_PruneOld(TickRecorder *rec) {
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
            // D-472 — NEVER leave the sidecar behind. Coverage that outlives its tape
            // claims a range whose data no longer exists, so a reconciler would read
            // the day as covered and decline to re-fetch it — the same over-claim the
            // manifest exists to prevent, arriving from the other direction. Deleted
            // FIRST: if the process dies between the two unlinks, an orphaned CSV
            // reads as NOT-COVERED (conservative, re-fetched) whereas an orphaned
            // coverage file reads as covered-but-empty (silent hole).
            char cov[512];
            snprintf(cov, sizeof(cov), "%s%.*s.coverage", rec->data_dir,
                     (int)(strlen(entry->d_name) - 4), entry->d_name);
            remove(cov);   // remove(), not unlink() — same call the CSV line below uses; no new include
            fprintf(stderr, "[tick-recorder] pruning old file: %s\n", path);
            remove(path);
        }
    }
    closedir(dir);
}

//======================================================================================================
static inline void TickRecorder_Init(TickRecorder *rec, const char *symbol,
                                      int enabled, uint32_t max_days) {
    memset(rec, 0, sizeof(*rec));
    rec->enabled = enabled;
    rec->max_days = max_days;
    if (!enabled) return;

    strncpy(rec->symbol, symbol, sizeof(rec->symbol) - 1);

    // build data directory path: data/{SYMBOL}/
    char upper_sym[16];
    for (int i = 0; symbol[i] && i < 15; i++)
        upper_sym[i] = (symbol[i] >= 'a' && symbol[i] <= 'z') ? symbol[i] - 32 : symbol[i];
    upper_sym[strlen(symbol) < 15 ? strlen(symbol) : 15] = '\0';

    // v4.2.2: ticks land in data/{SYMBOL}/_staging/ first. A separate
    // verification step (scripts/verify_ticks.sh) checks the file is a
    // full day and promotes it into data/{SYMBOL}/ for training use.
    // This stops partial-day or corrupt recordings (e.g. the year-58286
    // TickRecorder bug from before v4.0.2) from polluting the dataset
    // folder. Symlinking via build.sh keeps relative paths working.
    snprintf(rec->data_dir, sizeof(rec->data_dir), "data/%s/_staging/", upper_sym);
    TickRecorder_MkdirP(rec->data_dir);

    // prune old files on startup
    TickRecorder_PruneOld(rec);

    fprintf(stderr, "[tick-recorder] initialized: symbol=%s, max_days=%u, dir=%s\n",
            upper_sym, max_days, rec->data_dir);
}

//======================================================================================================
static inline void TickRecorder_Push(TickRecorder *rec, double price, double qty,
                                      int64_t timestamp_us, int is_buyer_maker) {
    if (!rec->enabled) return;

    // daily rotation
    int today = TickRecorder_DateInt(timestamp_us);
    if (today != rec->current_day) {
        // D-472 — seal the OUTGOING day before the path is overwritten. Sealing
        // after OpenFile would append the finished window to the NEW day's sidecar,
        // recording coverage the engine never had.
        TickRecorder_CoverageSeal(rec);
        TickRecorder_OpenFile(rec, today);
        // prune on rotation (once per day)
        TickRecorder_PruneOld(rec);
    }

    if (!rec->file) return;

    // F-054/PARITY-036: locale-immune + lossless emit (std::to_chars shortest round-trip).
    // %.8f is LOSSY for a double AND LC_NUMERIC-fragile; to_chars completes the
    // write∧read replay loop (read side = tt::parse_double_fast_advance).
    // rend-1 per to_chars: the separator byte is PROVABLY in-bounds even on the value-too-large
    // path (ptr==last). Worst-case row is ~73B of 96 — unreachable arithmetically; provable >
    // unreachable (TECH_DEBT-160 sister-cohort fix with DepthRecorder's identical emit shape).
    char row[96]; char* o = row; char* const rend = row + sizeof(row);
    o = std::to_chars(o, rend - 1, (long long)timestamp_us).ptr; *o++ = ',';
    o = std::to_chars(o, rend - 1, price).ptr;                   *o++ = ',';
    o = std::to_chars(o, rend - 1, qty).ptr;                     *o++ = ',';
    o = std::to_chars(o, rend - 1, is_buyer_maker).ptr;          *o++ = '\n';
    fwrite(row, 1, (size_t)(o - row), rec->file);
    rec->count++;
    // D-472 — the window is bounded by ticks actually WRITTEN, so a session that
    // opened a file and wrote nothing seals no window (see the guard in Seal).
    if (rec->session_open_us == 0) rec->session_open_us = timestamp_us;
    rec->session_last_us = timestamp_us;

    // flush every 1000 ticks (balance I/O and data safety)
    if ((rec->count & 0x3FF) == 0)
        fflush(rec->file);
}

//======================================================================================================
static inline void TickRecorder_Close(TickRecorder *rec) {
    TickRecorder_CoverageSeal(rec);   // D-472 — a clean shutdown is what makes a window TERMINATED
    if (rec->file) {
        fflush(rec->file);
        fclose(rec->file);
        rec->file = NULL;
    }
    if (rec->enabled)
        fprintf(stderr, "[tick-recorder] closed after %lu ticks\n", (unsigned long)rec->count);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TickRecorder_Push]
//======================================================================

#endif // TICK_RECORDER_HPP
