// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [HEALTH LOG — append-only structured diagnostic log, JSONL]
//======================================================================================================
// One line of JSON per event. Categories are caller-defined free-text
// (e.g. "regime", "sl_emission", "sg_eval", "cooldown", "latency"); levels
// are info/debug/trace. Always-on infrastructure for ongoing operational
// visibility — replaces ad-hoc env-gated fprintfs that come and go.
//
// Why this exists: ad-hoc stderr diagnostics get added during a debug
// session and removed when the bug is fixed, leaving zero residual
// observability. The next regression starts from "I have no idea what's
// happening internally." Health log is the durable surface.
//
// Output format (JSONL):
//   {"ts":"2026-04-29T22:14:53.123Z","level":"info","cat":"regime",
//    "core":0,"msg":"ema_sma_spread=0.000034 trending=0 detected=0"}
//
// Schema is intentionally loose:
//   - "ts": ISO 8601 UTC with millisecond precision
//   - "level": "info" | "debug" | "trace"
//   - "cat": short category tag
//   - "core": optional, -1 if not applicable
//   - "msg": free-text key=val payload (caller formats whatever it wants)
//
// Levels filter at the call site:
//   - info  = always logged (when cfg.health_log_path != "")
//   - debug = logged when cfg.health_log_level >= 1
//   - trace = logged when cfg.health_log_level >= 2
//
// Performance: single fopen("a") + one fprintf + fclose per event.
// ~10-30µs per event including disk flush. Don't call from hot path.
// Slow-path / per-cycle / per-event use is fine.
//
// Locale-pinned LC_NUMERIC=C around the printf so JSON-numeric parsers
// don't break on comma-decimal locales.
//======================================================================================================
#ifndef HEALTH_LOG_HPP
#define HEALTH_LOG_HPP

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <locale.h>

namespace tt {

// Level enum — keep small ints so the cfg int comparison is cheap.
enum HealthLogLevel {
    HEALTH_INFO  = 0,
    HEALTH_DEBUG = 1,
    HEALTH_TRACE = 2,
};

// Module-static config holder. Set once at engine boot, read on every
// log call. Single-writer (engine init), multi-reader (any thread).
// Path == empty → all log calls become no-ops; cheap branch on hot
// callers' steady state.
struct HealthLogState {
    char path[512];     // "" = disabled
    int  min_level;     // events with level > this are dropped
};

// Process-singleton. Engine init writes; all callers read.
inline HealthLogState* HealthLog_Singleton() {
    static HealthLogState s = { {0}, HEALTH_INFO };
    return &s;
}

// Configure once at engine init. After this, Health_Log() emits to
// `path` filtered by level. path==NULL or empty disables.
inline void Health_LogConfigure(const char* path, int min_level) {
    HealthLogState* s = HealthLog_Singleton();
    if (!path || !path[0]) {
        s->path[0] = '\0';
        s->min_level = HEALTH_INFO;
        return;
    }
    size_t n = strlen(path);
    if (n >= sizeof(s->path)) n = sizeof(s->path) - 1;
    memcpy(s->path, path, n);
    s->path[n] = '\0';
    s->min_level = min_level;
}

// Returns 1 if logging is enabled at or above the given level. Cheap
// short-circuit so callers can wrap expensive payload formatting.
inline int Health_LogEnabled(int level) {
    HealthLogState* s = HealthLog_Singleton();
    if (!s->path[0]) return 0;
    return level >= s->min_level ? 1 : 0;
}

// Core append. Caller passes category, optional core_id (-1 = global),
// and a printf-style payload string. Output is one JSONL line.
//
// Returns 1 on success, 0 on i/o failure (ignored by most callers).
inline int Health_Log(int level, const char* category, int core_id,
                      const char* fmt, ...) {
    HealthLogState* s = HealthLog_Singleton();
    if (!s->path[0]) return 0;
    if (level < s->min_level) return 0;

    // ISO 8601 UTC with ms precision
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_buf;
    gmtime_r(&tv.tv_sec, &tm_buf);
    char ts[40] = {0};
    int n = strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    if (n > 0 && (size_t)n + 5 < sizeof(ts)) {
        snprintf(ts + n, sizeof(ts) - n, ".%03ldZ", (long)(tv.tv_usec / 1000));
    }

    const char* level_str =
        (level == HEALTH_TRACE) ? "trace" :
        (level == HEALTH_DEBUG) ? "debug" :
        "info";

    // Format payload first (caller's printf). Buffer large enough for
    // typical key=val payloads; truncate gracefully if longer.
    char payload[1024];
    va_list ap;
    va_start(ap, fmt);
    int p_len = vsnprintf(payload, sizeof(payload), fmt, ap);
    va_end(ap);
    if (p_len < 0) payload[0] = '\0';

    // JSON-escape the payload's quotes + backslashes inline. Cheap
    // single-pass, output buffer is roughly 2× input worst case.
    char escaped[2200];
    int e = 0;
    for (int i = 0; payload[i] && e + 2 < (int)sizeof(escaped); ++i) {
        char c = payload[i];
        if (c == '"' || c == '\\') {
            escaped[e++] = '\\';
            escaped[e++] = c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            escaped[e++] = ' ';
        } else if ((unsigned char)c >= 0x20) {
            escaped[e++] = c;
        }
    }
    escaped[e] = '\0';

    // Pin LC_NUMERIC=C around the printf — rare but the caller might
    // have included %g/%f in their payload via vsnprintf above. Re-pin
    // here as belt-and-suspenders for the JSON output itself.
    locale_t pinned = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    locale_t prev = (locale_t)0;
    if (pinned) prev = uselocale(pinned);

    FILE* f = fopen(s->path, "a");
    if (!f) {
        if (pinned) { uselocale(prev); freelocale(pinned); }
        return 0;
    }
    int written = fprintf(f,
        "{\"ts\":\"%s\",\"level\":\"%s\",\"cat\":\"%s\","
         "\"core\":%d,\"msg\":\"%s\"}\n",
        ts, level_str, category ? category : "?", core_id, escaped);
    int flush_ok = (fflush(f) == 0);
    int close_ok = (fclose(f) == 0);

    if (pinned) { uselocale(prev); freelocale(pinned); }

    return (written > 0 && flush_ok && close_ok) ? 1 : 0;
}

} // namespace tt

#endif // HEALTH_LOG_HPP
