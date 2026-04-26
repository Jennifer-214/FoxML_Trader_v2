// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [TICK RECORDER]
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

struct TickRecorder {
    FILE *file;
    uint64_t count;
    int enabled;
    int current_day;        // YYYYMMDD of currently open file (triggers rotation)
    char symbol[16];
    char data_dir[256];     // "data/{symbol}/"
    uint32_t max_days;      // auto-delete CSVs older than this
};

//======================================================================================================
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
        fprintf(stderr, "[tick-recorder] failed to open %s\n", path);
        return;
    }

    if (is_new) {
        fprintf(rec->file, "timestamp_us,price,quantity,is_buyer_maker\n");
        fflush(rec->file);
    }

    rec->current_day = date_int;
    fprintf(stderr, "[tick-recorder] recording to %s\n", path);
}

//======================================================================================================
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

    snprintf(rec->data_dir, sizeof(rec->data_dir), "data/%s/", upper_sym);
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
        TickRecorder_OpenFile(rec, today);
        // prune on rotation (once per day)
        TickRecorder_PruneOld(rec);
    }

    if (!rec->file) return;

    fprintf(rec->file, "%lld,%.8f,%.8f,%d\n",
            (long long)timestamp_us, price, qty, is_buyer_maker);
    rec->count++;

    // flush every 1000 ticks (balance I/O and data safety)
    if ((rec->count & 0x3FF) == 0)
        fflush(rec->file);
}

//======================================================================================================
static inline void TickRecorder_Close(TickRecorder *rec) {
    if (rec->file) {
        fflush(rec->file);
        fclose(rec->file);
        rec->file = NULL;
    }
    if (rec->enabled)
        fprintf(stderr, "[tick-recorder] closed after %lu ticks\n", (unsigned long)rec->count);
}

#endif // TICK_RECORDER_HPP
