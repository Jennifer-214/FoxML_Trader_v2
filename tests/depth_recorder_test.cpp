// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [DEPTH RECORDER TEST SUITE — Phase 8a c7]
//======================================================================================================
// Separate from controller_test because DepthRecorder writes to disk and
// depends on filesystem state. controller_test stays in-memory by design.
//
// Coverage: 17 assertions across 5 groups
//   Group 1: Init + Write + Readback           (5 assertions)
//   Group 2: Daily rotation                    (3 assertions)
//   Group 3: Gap detection (corrected logic)   (4 assertions)
//   Group 4: AutoPrune                         (3 assertions)
//   Group 5: Error handling                    (2 assertions)
//======================================================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include "../DataStream/DepthRecorder.hpp"

using namespace std;

constexpr unsigned FP = 64;

//======================================================================================================
// [HARNESS]
//======================================================================================================
static int tests_passed = 0;
static int tests_failed = 0;

static void check(const char *name, int condition) {
    if (condition) {
        printf("  [PASS] %s\n", name);
        tests_passed++;
    } else {
        printf("  [FAIL] %s\n", name);
        tests_failed++;
    }
}

//======================================================================================================
// [FILESYSTEM HELPERS]
//======================================================================================================
// Recursive rmdir for tempdir cleanup. Matches what mkdtemp + DepthRecorder
// + multiple daily files needs for clean teardown between tests.
static void rmrf(const char *path) {
    DIR *d = opendir(path);
    if (!d) { remove(path); return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            rmrf(child);
        } else {
            remove(child);
        }
    }
    closedir(d);
    rmdir(path);
}

static int count_csv_rows(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    int rows = 0;
    int header_skipped = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;        // skip gap markers
        if (!header_skipped) { header_skipped = 1; continue; } // skip header
        rows++;
    }
    fclose(f);
    return rows;
}

static int count_lines_starting_with(const char *path, const char *prefix) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    int n = 0;
    size_t plen = strlen(prefix);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, prefix, plen) == 0) n++;
    }
    fclose(f);
    return n;
}

static int count_files_in_dir(const char *path) {
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        n++;
    }
    closedir(d);
    return n;
}

// Build the CSV path for a recorder + a UTC timestamp.
static void recorder_csv_path(char *out, size_t outlen,
                               const DepthRecorder *rec, uint64_t ts_us) {
    time_t t = (time_t)(ts_us / 1000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, outlen, "%s%04d-%02d-%02d.csv",
             rec->data_dir,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

//======================================================================================================
// [GROUP 1: Init + Write + Readback — 5 assertions]
//======================================================================================================
static void test_init_write_readback() {
    printf("\n--- Group 1: Init + Write + Readback ---\n");

    char tempdir[] = "/tmp/depth_rec_g1_XXXXXX";
    if (!mkdtemp(tempdir)) { check("mkdtemp", 0); return; }

    DepthRecorder rec;
    // max_days large because tests use synthetic past timestamps for the
    // CSV filename (e.g. 2023-11-14). _PruneOld parses the filename date,
    // not mtime — at production max_days=30, a file named 2023-11-14.csv
    // would be immediately pruned. Use a value larger than any test span.
    DepthRecorder_Init(&rec, "TEST", tempdir, /*max_days=*/999999, /*enabled=*/1);

    // Use a fixed UTC base so all 10 snapshots fall on the same day.
    uint64_t base_us = 1700000000ULL * 1000000ULL; // 2023-11-14T22:13:20Z
    BookSnapshot<FP> snap = BookSnapshot_Init<FP>();

    for (int i = 0; i < 10; i++) {
        snap.bids[0].price = FPN_FromDouble<FP>(60000.0 - i);
        snap.bids[0].qty   = FPN_FromDouble<FP>(1.0 + i * 0.1);
        snap.asks[0].price = FPN_FromDouble<FP>(60001.0 + i);
        snap.asks[0].qty   = FPN_FromDouble<FP>(0.5 + i * 0.05);
        // realistic Binance jump (50-500 between snapshots), NOT +1
        snap.last_update_id = 1000 + (uint64_t)i * 50;
        // 100ms apart, well under the 2s gap threshold
        snap.timestamp_us   = base_us + (uint64_t)i * 100000;
        DepthRecorder_Write(&rec, &snap);
    }
    DepthRecorder_Close(&rec);

    char path[1024];
    recorder_csv_path(path, sizeof(path), &rec, base_us);

    // file exists
    struct stat st;
    check("CSV file created at expected path",
          stat(path, &st) == 0 && st.st_size > 0);

    // header line is present + has expected fields
    FILE *f = fopen(path, "r");
    char line[1024];
    int got_header_ts = 0, got_header_id = 0;
    if (f && fgets(line, sizeof(line), f)) {
        got_header_ts = (strstr(line, "timestamp_us") != NULL);
        got_header_id = (strstr(line, "last_update_id") != NULL);
    }
    if (f) fclose(f);
    check("CSV header has timestamp_us + last_update_id columns",
          got_header_ts && got_header_id);

    // exactly 10 data rows (no gap markers expected — normal +50 jumps)
    int rows = count_csv_rows(path);
    check("10 data rows written (header + gap markers excluded)", rows == 10);

    int gaps = count_lines_starting_with(path, "# GAP");
    check("no gap markers on normal +50 lastUpdateId jumps", gaps == 0);

    // last row has last_update_id = 1000 + 9*50 = 1450
    f = fopen(path, "r");
    char last[1024] = "";
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (line[0] != '#' && strchr(line, ',')) {
                strncpy(last, line, sizeof(last) - 1);
                last[sizeof(last) - 1] = '\0';
            }
        }
        fclose(f);
    }
    // CSV columns: ts,last_update_id,bid_price,bid_qty,ask_price,ask_qty
    // Find the second field (last_update_id).
    char *p = strchr(last, ','); long long id_val = 0;
    if (p) id_val = strtoll(p + 1, NULL, 10);
    check("last row's last_update_id == 1450 (1000 + 9*50)", id_val == 1450);

    rmrf(tempdir);
}

//======================================================================================================
// [GROUP 2: Daily rotation — 3 assertions]
//======================================================================================================
static void test_daily_rotation() {
    printf("\n--- Group 2: Daily rotation ---\n");

    char tempdir[] = "/tmp/depth_rec_g2_XXXXXX";
    if (!mkdtemp(tempdir)) { check("mkdtemp", 0); return; }

    DepthRecorder rec;
    // max_days large because tests use synthetic past timestamps for the
    // CSV filename (e.g. 2023-11-14). _PruneOld parses the filename date,
    // not mtime — at production max_days=30, a file named 2023-11-14.csv
    // would be immediately pruned. Use a value larger than any test span.
    DepthRecorder_Init(&rec, "TEST", tempdir, /*max_days=*/999999, /*enabled=*/1);

    // Day 1: 23:59:30 UTC on a fixed date.
    // 1700006370 = 2023-11-14T23:59:30Z; +60s = day 2.
    uint64_t day1_us = 1700006370ULL * 1000000ULL;
    uint64_t day2_us = day1_us + 60ULL * 1000000ULL;

    BookSnapshot<FP> snap = BookSnapshot_Init<FP>();
    snap.bids[0].price = FPN_FromDouble<FP>(60000.0);
    snap.bids[0].qty   = FPN_FromDouble<FP>(1.0);
    snap.asks[0].price = FPN_FromDouble<FP>(60001.0);
    snap.asks[0].qty   = FPN_FromDouble<FP>(1.0);

    snap.last_update_id = 100;
    snap.timestamp_us   = day1_us;
    DepthRecorder_Write(&rec, &snap);

    snap.last_update_id = 200;
    snap.timestamp_us   = day2_us;
    DepthRecorder_Write(&rec, &snap);

    DepthRecorder_Close(&rec);

    char path1[1024], path2[1024];
    recorder_csv_path(path1, sizeof(path1), &rec, day1_us);
    recorder_csv_path(path2, sizeof(path2), &rec, day2_us);

    struct stat st;
    check("day-1 file exists after rotation",
          stat(path1, &st) == 0 && st.st_size > 0);
    check("day-2 file exists after rotation",
          stat(path2, &st) == 0 && st.st_size > 0);

    int rows1 = count_csv_rows(path1);
    int rows2 = count_csv_rows(path2);
    check("each day file has exactly 1 row (no leakage across day boundary)",
          rows1 == 1 && rows2 == 1);

    rmrf(tempdir);
}

//======================================================================================================
// [GROUP 3: Gap detection (corrected logic) — 4 assertions]
//======================================================================================================
// Validates the recorder's internal gap detection against the corrected
// algorithm: backward jump OR wallclock gap >2s (NOT "+1" check).
static void test_gap_detection() {
    printf("\n--- Group 3: Gap detection (corrected) ---\n");

    // 3a. Normal +50 jump → NO gap marker
    {
        char tempdir[] = "/tmp/depth_rec_g3a_XXXXXX";
        if (!mkdtemp(tempdir)) { check("g3a mkdtemp", 0); return; }
        DepthRecorder rec;
        DepthRecorder_Init(&rec, "TEST", tempdir, /*max_days=*/999999, /*enabled=*/1);

        uint64_t base_us = 1700000000ULL * 1000000ULL;
        BookSnapshot<FP> snap = BookSnapshot_Init<FP>();
        snap.bids[0].price = FPN_FromDouble<FP>(60000.0);
        snap.bids[0].qty   = FPN_FromDouble<FP>(1.0);
        snap.asks[0].price = FPN_FromDouble<FP>(60001.0);
        snap.asks[0].qty   = FPN_FromDouble<FP>(1.0);

        snap.timestamp_us = base_us;
        snap.last_update_id = 1000;
        DepthRecorder_Write(&rec, &snap);

        snap.timestamp_us = base_us + 100000; // 100ms later
        snap.last_update_id = 1050; // +50 — NORMAL
        DepthRecorder_Write(&rec, &snap);

        DepthRecorder_Close(&rec);

        char path[1024];
        recorder_csv_path(path, sizeof(path), &rec, base_us);
        int gaps = count_lines_starting_with(path, "# GAP");
        check("normal +50 jump produces NO gap marker (no false-positive)", gaps == 0);
        rmrf(tempdir);
    }

    // 3b. Backward jump (reconnect to stale snapshot) → gap marker
    {
        char tempdir[] = "/tmp/depth_rec_g3b_XXXXXX";
        if (!mkdtemp(tempdir)) { check("g3b mkdtemp", 0); return; }
        DepthRecorder rec;
        DepthRecorder_Init(&rec, "TEST", tempdir, /*max_days=*/999999, /*enabled=*/1);

        uint64_t base_us = 1700000000ULL * 1000000ULL;
        BookSnapshot<FP> snap = BookSnapshot_Init<FP>();
        snap.bids[0].price = FPN_FromDouble<FP>(60000.0);
        snap.bids[0].qty   = FPN_FromDouble<FP>(1.0);
        snap.asks[0].price = FPN_FromDouble<FP>(60001.0);
        snap.asks[0].qty   = FPN_FromDouble<FP>(1.0);

        snap.timestamp_us = base_us;
        snap.last_update_id = 5000;
        DepthRecorder_Write(&rec, &snap);

        snap.timestamp_us = base_us + 100000;
        snap.last_update_id = 4500; // BACKWARD
        DepthRecorder_Write(&rec, &snap);

        DepthRecorder_Close(&rec);

        char path[1024];
        recorder_csv_path(path, sizeof(path), &rec, base_us);
        int gaps = count_lines_starting_with(path, "# GAP");
        check("backward last_update_id triggers gap marker", gaps >= 1);
        rmrf(tempdir);
    }

    // 3c. Wallclock gap >2s → gap marker
    {
        char tempdir[] = "/tmp/depth_rec_g3c_XXXXXX";
        if (!mkdtemp(tempdir)) { check("g3c mkdtemp", 0); return; }
        DepthRecorder rec;
        DepthRecorder_Init(&rec, "TEST", tempdir, /*max_days=*/999999, /*enabled=*/1);

        uint64_t base_us = 1700000000ULL * 1000000ULL;
        BookSnapshot<FP> snap = BookSnapshot_Init<FP>();
        snap.bids[0].price = FPN_FromDouble<FP>(60000.0);
        snap.bids[0].qty   = FPN_FromDouble<FP>(1.0);
        snap.asks[0].price = FPN_FromDouble<FP>(60001.0);
        snap.asks[0].qty   = FPN_FromDouble<FP>(1.0);

        snap.timestamp_us = base_us;
        snap.last_update_id = 1000;
        DepthRecorder_Write(&rec, &snap);

        snap.timestamp_us = base_us + 5ULL * 1000000ULL; // 5s later
        snap.last_update_id = 1050;
        DepthRecorder_Write(&rec, &snap);

        DepthRecorder_Close(&rec);

        char path[1024];
        recorder_csv_path(path, sizeof(path), &rec, base_us);
        int gaps = count_lines_starting_with(path, "# GAP");
        check("wallclock gap >2s triggers gap marker", gaps >= 1);
        rmrf(tempdir);
    }

    // 3d. Explicit _LogGap call from disconnect site
    {
        char tempdir[] = "/tmp/depth_rec_g3d_XXXXXX";
        if (!mkdtemp(tempdir)) { check("g3d mkdtemp", 0); return; }
        DepthRecorder rec;
        DepthRecorder_Init(&rec, "TEST", tempdir, /*max_days=*/999999, /*enabled=*/1);

        uint64_t base_us = 1700000000ULL * 1000000ULL;
        DepthRecorder_LogGap(&rec, base_us, "disconnect");
        DepthRecorder_Close(&rec);

        char path[1024];
        recorder_csv_path(path, sizeof(path), &rec, base_us);
        int gaps = count_lines_starting_with(path, "# GAP");
        check("explicit _LogGap writes exactly one gap marker", gaps == 1);
        rmrf(tempdir);
    }
}

//======================================================================================================
// [GROUP 4: AutoPrune — 3 assertions]
//======================================================================================================
static void test_autoprune() {
    printf("\n--- Group 4: AutoPrune ---\n");

    char tempdir[] = "/tmp/depth_rec_g4_XXXXXX";
    if (!mkdtemp(tempdir)) { check("mkdtemp", 0); return; }

    // Build the depth dir manually so we can pre-create files BEFORE Init runs prune.
    char depth_dir[512];
    snprintf(depth_dir, sizeof(depth_dir), "%s/TEST/depth", tempdir);
    {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s/TEST", tempdir);
        mkdir(tmp, 0755);
    }
    mkdir(depth_dir, 0755);

    // Create 5 fake daily files with mtimes spanning 60 days (1, 15, 25, 35, 60 days old).
    int days_old[] = {1, 15, 25, 35, 60};
    char names[5][32];
    char paths[5][512];
    time_t now = time(NULL);
    for (int i = 0; i < 5; i++) {
        time_t t = now - (time_t)days_old[i] * 86400;
        struct tm tm; gmtime_r(&t, &tm);
        snprintf(names[i], sizeof(names[i]), "%04d-%02d-%02d.csv",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        snprintf(paths[i], sizeof(paths[i]), "%s/%s", depth_dir, names[i]);
        FILE *f = fopen(paths[i], "w");
        if (f) { fputs("dummy\n", f); fclose(f); }
        struct utimbuf ut; ut.actime = t; ut.modtime = t;
        utime(paths[i], &ut);
    }

    int before = count_files_in_dir(depth_dir);
    check("5 fake daily files exist before prune", before == 5);

    // Init with max_days=30 → should prune the 35-day and 60-day files.
    // (This is the test that DELIBERATELY uses production-default max_days
    // because we're verifying the prune logic itself; the file *names* are
    // computed from now() above so they're recent — not the synthetic
    // 2023-11-14 timestamps used by the other groups.)
    DepthRecorder rec;
    DepthRecorder_Init(&rec, "TEST", tempdir, /*max_days=*/30, /*enabled=*/1);

    int after = count_files_in_dir(depth_dir);
    check("after prune (max_days=30): 3 files remain (1, 15, 25 days old)",
          after == 3);

    // Specifically: 35-day-old file is gone, 1-day-old is kept.
    struct stat st;
    int day35_gone = (stat(paths[3], &st) != 0);
    int day1_kept  = (stat(paths[0], &st) == 0);
    check("35-day-old pruned + 1-day-old kept (specific files)",
          day35_gone && day1_kept);

    DepthRecorder_Close(&rec);
    rmrf(tempdir);
}

//======================================================================================================
// [GROUP 5: Error handling — 2 assertions]
//======================================================================================================
static void test_error_handling() {
    printf("\n--- Group 5: Error handling ---\n");

    // 5a. fopen failure (non-creatable dir): recorder should not crash.
    // Use a path under /proc which is read-only on Linux — mkdir fails,
    // first write attempt fopens fails, recorder continues with file=NULL.
    {
        DepthRecorder rec;
        DepthRecorder_Init(&rec, "TEST", "/proc/this/cannot/exist/", /*max_days=*/30,
                           /*enabled=*/1);

        BookSnapshot<FP> snap = BookSnapshot_Init<FP>();
        snap.bids[0].price = FPN_FromDouble<FP>(60000.0);
        snap.bids[0].qty   = FPN_FromDouble<FP>(1.0);
        snap.asks[0].price = FPN_FromDouble<FP>(60001.0);
        snap.asks[0].qty   = FPN_FromDouble<FP>(1.0);
        snap.last_update_id = 1;
        snap.timestamp_us = 1700000000ULL * 1000000ULL;

        // Should not crash. file stays NULL → count stays 0.
        DepthRecorder_Write(&rec, &snap);

        check("write to unwritable dir does not crash, count stays 0",
              rec.count == 0 && rec.file == NULL);

        DepthRecorder_Close(&rec);
    }

    // 5b. enabled=0 → all calls are no-ops, no files created.
    {
        char tempdir[] = "/tmp/depth_rec_g5b_XXXXXX";
        if (!mkdtemp(tempdir)) { check("g5b mkdtemp", 0); return; }

        DepthRecorder rec;
        DepthRecorder_Init(&rec, "TEST", tempdir, /*max_days=*/30, /*enabled=*/0);

        BookSnapshot<FP> snap = BookSnapshot_Init<FP>();
        DepthRecorder_Write(&rec, &snap);
        DepthRecorder_LogGap(&rec, 1700000000ULL * 1000000ULL, "test");
        DepthRecorder_Close(&rec);

        // No data dir even created (Init returns early when enabled=0)
        int n = count_files_in_dir(tempdir);
        check("disabled recorder creates no files / does not crash",
              n == 0 && rec.count == 0);

        rmrf(tempdir);
    }
}

//======================================================================================================
// [MAIN]
//======================================================================================================
int main() {
    printf("======================================\n");
    printf("  DEPTH RECORDER TEST SUITE (Phase 8a)\n");
    printf("======================================\n");

    test_init_write_readback();
    test_daily_rotation();
    test_gap_detection();
    test_autoprune();
    test_error_handling();

    printf("\n======================================\n");
    printf("  RESULTS: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("======================================\n");

    return tests_failed > 0 ? 1 : 0;
}
