// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/Notify.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[operational alerting primitive — non-blocking queue + dedicated worker thread + pluggable backend; deliberately mutex/condvar (off every trading path)]
// [CONTAINS]
//   - [ENUM]_[NotifyKind]   (NotifyLevel shares the section)
//   - [STRUCT]_[NotifyEvent]
//   - [STRUCT]_[NotifyState]
//   - [FUNCTION]_[Notify_NowMonotonicUs]
//   - [FUNCTION]_[notify_worker_fn]
//   - [FUNCTION]_[NotifyState_Init]
//   - [FUNCTION]_[Notify_Send]
//   - [FUNCTION]_[NotifyState_Shutdown]
//   - [FUNCTION]_[NotifyBackend_Stderr]
//   - [STRUCT]_[NotifyCommandState]
//   - [FUNCTION]_[Notify_ShellEscape]
//   - [FUNCTION]_[Notify_BuildCommand]
//   - [FUNCTION]_[NotifyBackend_Command]
//======================================================================================================
// Routes alertable events (kill switch trips, disconnects, orphan recovery,
// order rejections, etc.) through a queue + dedicated worker thread to a
// pluggable backend. Callers never block on backend I/O.
//
// Phase 8b ships stderr backend only. Slack / Telegram / Discord defer to
// Phase 8b.1 — see the live-readiness master plan errata. The cfg fields +
// backend dispatcher are forward-compatible: 8b.1 lands new backends without
// touching Notify_Send call sites.
//
// Threading model (single mutex, condvar):
//   - Caller (engine, depth, user-data threads) → Notify_Send → enqueue
//   - Worker thread → cond_wait → dequeue → backend → loop
//   - Notify_Send is non-blocking: drops events on full queue (rate-limit
//     failsafe, warns to stderr).
//
// Cooldown gate (per-event-kind):
//   - last_fired_us[kind] tracked in CLOCK_MONOTONIC microseconds (NOT
//     CLOCK_REALTIME; wall clock can NTP-jump backward).
//   - Same kind firing within cooldown_us is dropped silently.
//   - Different kinds are independent. Default cooldown = 60s.
//
// g_notify ownership:
//   - extern declared here, defined in main.cpp (single TU).
//   - Backtest leaves g_notify == nullptr → all callers must
//     `if (g_notify) Notify_Send(...)`.
//======================================================================================================
#ifndef NOTIFY_HPP
#define NOTIFY_HPP

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

//======================================================================
// [ENUM]_[NotifyKind]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[alert severity (NotifyLevel, shares this section) + per-kind cooldown keys — append-only, never renumber; NOTIFY_KINDS_MAX bounds the cooldown array]
//======================================================================
// [CODE]
//======================================================================
enum NotifyLevel {
    NOTIFY_INFO     = 0, // status updates, session start/end, info-level
    NOTIFY_WARN     = 1, // recoverable issues (reconnect, transient errors)
    NOTIFY_ALERT    = 2, // user-attention-required (kill switch trip, orphan)
    NOTIFY_CRITICAL = 3, // engine cannot continue safely
};

// Per-kind cooldown applies to repeated events of the SAME kind.
// Adding a kind: append to enum, do NOT renumber existing entries.
// NOTIFY_KINDS_MAX is the array size for last_fired_us — bump if needed.
enum NotifyKind {
    NK_KILL_TRIGGER         = 0,  // kill switch fired
    NK_KILL_DAILY_LOSS      = 1,
    NK_KILL_DRAWDOWN        = 2,
    NK_ORPHAN_HALT          = 3,  // engine refuses to start with orphans
    NK_ORPHAN_DETECTED      = 4,  // engine detected and recovering
    NK_DISCONNECT_TRADE     = 5,
    NK_DISCONNECT_DEPTH     = 6,
    NK_DISCONNECT_USERDATA  = 7,
    NK_ORDER_REJECTED       = 8,
    NK_SESSION_START        = 9,
    NK_NODE_KILL_TRIP       = 10, // Phase 3 — per-core kill switch fired
    NK_NODE_BUDGET_EXHAUST  = 11, // Phase 2.2 — per-core budget hit
    // append new kinds here; do NOT reorder
};
#define NOTIFY_KINDS_MAX 16
//======================================================================
// [END_CODE]
//======================================================================
// [END_ENUM]_[NotifyKind]
//======================================================================

//======================================================================
// [STRUCT]_[NotifyEvent]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[heap-free POD alert — level + cooldown kind + monotonic timestamp + fixed subject/body buffers; queued by value]
//======================================================================
// [CODE]
//======================================================================
// Heap-free per event — queue holds POD by value.
struct NotifyEvent {
    int level;             // NotifyLevel
    int event_kind;        // NotifyKind (used as cooldown key)
    uint64_t timestamp_us; // CLOCK_MONOTONIC at enqueue time
    char subject[128];
    char body[512];
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[656B]
// [ALIGN]_[8]
// [CACHE_LINES]_[11]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[NotifyEvent]
//======================================================================

//======================================================================
// [STRUCT]_[NotifyState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [LIVE_TRADING]]
// [THREAD]_[[ANY_THREAD_WRITER] [WORKER_READER]]
// [STRADDLE_EXEMPT]_[cond]_[mutex-guarded cold MPSC — lock+cond intentionally adjacent, all access serialized by the lock; off every trading path per OVERVIEW — D-414 leaf-3 2026-08-10]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[mutex+condvar MPSC ring (any thread enqueues, one worker drains) + per-kind cooldown + backend hook; g_notify inline global — null in backtest/tests]
//======================================================================
// [CODE]
//======================================================================
// Backend interface: takes one event + opaque state, returns 0 on success.
typedef int (*NotifyBackendFn)(const NotifyEvent *evt, void *backend_state);

#define NOTIFY_QUEUE_CAP 64

struct NotifyState {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    NotifyEvent     queue[NOTIFY_QUEUE_CAP]; // ring buffer
    int             head;
    int             tail;
    int             count;
    int             shutdown;

    // throttle state — per-kind, CLOCK_MONOTONIC microseconds
    uint64_t        last_fired_us[NOTIFY_KINDS_MAX];
    uint64_t        cooldown_us;             // default 60_000_000 = 60 sec

    // backend
    NotifyBackendFn backend;
    void           *backend_state;

    // worker thread
    pthread_t       worker_tid;
    int             worker_started;
};

// Global notifier pointer. C++17 inline variable — single definition across
// translation units (avoids needing a Notify.cpp). Initialized to nullptr.
// Live engine assigns &g_notify_state to it after NotifyState_Init when
// cfg.notify_enabled=1. Backtest, controller_test, foxml_suite all leave it
// null → callers must guard `if (g_notify) Notify_Send(...)`.
inline NotifyState *g_notify = nullptr;
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-10]
// [SIZE]_[42256B]
// [ALIGN]_[8]
// [CACHE_LINES]_[661]
// [STRADDLE]_[cond@40]
// [UPSTREAM]_[[NotifyBackendFn] [NotifyEvent]]
// [CONSUMERS]_[[notify_worker_fn] [NotifyState_Init] [Notify_Send] [NotifyState_Shutdown] [EngineSharded_Run] [EngineSharded_OrphanRecovery] [EngineSharded_ForceCloseOnShutdown]]
//======================================================================
// [END_STRUCT]_[NotifyState]
//======================================================================

//======================================================================
// [FUNCTION]_[Notify_NowMonotonicUs]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[CLOCK_MONOTONIC microseconds — cooldown clock (never CLOCK_REALTIME; wall clock can NTP-jump backward)]
//======================================================================
// [CODE]
//======================================================================
static inline uint64_t Notify_NowMonotonicUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Notify_NowMonotonicUs]
//======================================================================

//======================================================================
// [FUNCTION]_[notify_worker_fn]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[worker loop — cond_wait, dequeue, backend WITHOUT the lock held; drains the residue on shutdown]
//======================================================================
// [CODE]
//======================================================================
static inline void *notify_worker_fn(void *arg) {
    NotifyState *ns = (NotifyState *)arg;

    pthread_mutex_lock(&ns->lock);
    while (1) {
        // Wake on: new event OR shutdown signal.
        while (ns->count == 0 && !ns->shutdown) {
            pthread_cond_wait(&ns->cond, &ns->lock);
        }

        // Drain whatever's queued, even on shutdown.
        if (ns->count > 0) {
            NotifyEvent ev = ns->queue[ns->head];
            ns->head = (ns->head + 1) % NOTIFY_QUEUE_CAP;
            ns->count--;
            pthread_mutex_unlock(&ns->lock);

            if (ns->backend) {
                ns->backend(&ev, ns->backend_state);
            }

            pthread_mutex_lock(&ns->lock);
            continue;
        }

        // count == 0 and shutdown → exit.
        if (ns->shutdown) break;
    }
    pthread_mutex_unlock(&ns->lock);
    return NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Drains the queue and invokes the backend. Backend is called WITHOUT the
// state lock held (so a slow backend doesn't block enqueue). On shutdown,
// drains remaining events before exiting (per test sidecar Group 5).
//======================================================================
// [END_FUNCTION]_[notify_worker_fn]
//======================================================================

//======================================================================
// [FUNCTION]_[NotifyState_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[zero state, init mutex/cond, bind backend + cooldown, spawn worker; create-failure disables alerts, never fatal]
//======================================================================
// [CODE]
//======================================================================
static inline void NotifyState_Init(NotifyState *ns,
                                     NotifyBackendFn backend, void *backend_state,
                                     uint64_t cooldown_us) {
    memset(ns, 0, sizeof(*ns));
    pthread_mutex_init(&ns->lock, NULL);
    pthread_cond_init(&ns->cond, NULL);
    ns->backend = backend;
    ns->backend_state = backend_state;
    ns->cooldown_us = cooldown_us;
    if (pthread_create(&ns->worker_tid, NULL, notify_worker_fn, ns) == 0) {
        ns->worker_started = 1;
    } else {
        fprintf(stderr, "[NOTIFY] worker thread create failed — alerts disabled\n");
        ns->worker_started = 0;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[NotifyState_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[Notify_Send]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[non-blocking enqueue — per-kind monotonic cooldown gate, drop+warn on full queue; caller never waits on backend I/O]
//======================================================================
// [CODE]
//======================================================================
static inline void Notify_Send(NotifyState *ns, int level, int kind,
                                const char *subject, const char *body) {
    if (!ns || !ns->worker_started) return;

    uint64_t now = Notify_NowMonotonicUs();

    pthread_mutex_lock(&ns->lock);

    // Per-kind cooldown
    if (kind >= 0 && kind < NOTIFY_KINDS_MAX) {
        if (ns->last_fired_us[kind] != 0 &&
            now - ns->last_fired_us[kind] < ns->cooldown_us) {
            pthread_mutex_unlock(&ns->lock);
            return; // silent drop within cooldown window
        }
        ns->last_fired_us[kind] = now;
    }

    // Queue full → drop + warn (don't hold lock for the warn fprintf)
    if (ns->count >= NOTIFY_QUEUE_CAP) {
        pthread_mutex_unlock(&ns->lock);
        fprintf(stderr, "[NOTIFY] queue full — dropping event '%s'\n",
                subject ? subject : "(null)");
        return;
    }

    NotifyEvent *ev = &ns->queue[ns->tail];
    ev->level = level;
    ev->event_kind = kind;
    ev->timestamp_us = now;
    if (subject) {
        strncpy(ev->subject, subject, sizeof(ev->subject) - 1);
        ev->subject[sizeof(ev->subject) - 1] = '\0';
    } else {
        ev->subject[0] = '\0';
    }
    if (body) {
        strncpy(ev->body, body, sizeof(ev->body) - 1);
        ev->body[sizeof(ev->body) - 1] = '\0';
    } else {
        ev->body[0] = '\0';
    }
    ns->tail = (ns->tail + 1) % NOTIFY_QUEUE_CAP;
    ns->count++;

    pthread_cond_signal(&ns->cond);
    pthread_mutex_unlock(&ns->lock);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Non-blocking enqueue. Drops events on full queue (rate-limit failsafe).
// Cooldown gate uses CLOCK_MONOTONIC (NTP-jump-safe).
//======================================================================
// [END_FUNCTION]_[Notify_Send]
//======================================================================

//======================================================================
// [FUNCTION]_[NotifyState_Shutdown]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[signal shutdown, join worker (drains residue first), destroy pthread resources]
//======================================================================
// [CODE]
//======================================================================
// Drain remaining events + join worker thread + free pthread resources.
static inline void NotifyState_Shutdown(NotifyState *ns) {
    if (!ns->worker_started) return;
    pthread_mutex_lock(&ns->lock);
    ns->shutdown = 1;
    pthread_cond_signal(&ns->cond);
    pthread_mutex_unlock(&ns->lock);
    pthread_join(ns->worker_tid, NULL);
    pthread_mutex_destroy(&ns->lock);
    pthread_cond_destroy(&ns->cond);
    ns->worker_started = 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[NotifyState_Shutdown]
//======================================================================

//======================================================================
// [FUNCTION]_[NotifyBackend_Stderr]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[default backend — level-prefixed stderr line, flushed immediately for tail -f visibility (ships in Phase 8b)]
//======================================================================
// [CODE]
//======================================================================
static inline int NotifyBackend_Stderr(const NotifyEvent *evt, void *state) {
    (void)state;
    static const char *level_str[] = {"INFO", "WARN", "ALERT", "CRITICAL"};
    int li = (evt->level >= 0 && evt->level < 4) ? evt->level : 0;
    fprintf(stderr, "[NOTIFY %s] %s — %s\n",
            level_str[li], evt->subject, evt->body);
    fflush(stderr); // make alerts visible immediately under tail -f
    return 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[NotifyBackend_Stderr]
//======================================================================

//======================================================================
// [STRUCT]_[NotifyCommandState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[command-backend state — the cfg-supplied shell template (dunst/Discord/Slack/Telegram/ntfy/email examples in the section prose)]
//======================================================================
// [CODE]
//======================================================================
struct NotifyCommandState {
    char template_str[512]; // copied from cfg at Init; not modified
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Runs a configurable shell command for each event. Decouples the engine
// from any specific notification service. Templates MUST wrap %s in single
// quotes — Notify_ShellEscape replaces internal ' with '\'' but does NOT
// add the surrounding quotes (caller's responsibility).
//
//   - dunst:    notify-send 'Engine: %s' '%s'
//   - Discord:  curl -s -X POST -H 'Content-Type: application/json' \
//                    -d '{"content":"%s\n%s"}' YOUR_DISCORD_WEBHOOK
//   - Slack:    curl -s -X POST -H 'Content-Type: application/json' \
//                    -d '{"text":"%s: %s"}' YOUR_SLACK_WEBHOOK
//   - Telegram: curl -s 'https://api.telegram.org/bot<TOK>/sendMessage' \
//                    -d 'chat_id=<CHAT>&text=%s: %s'
//   - ntfy.sh:  curl -s -d '%s: %s' https://ntfy.sh/your-topic
//   - email:    printf 'Subject: %s\n\n%s' '%s' '%s' | sendmail you@example
//
// Template syntax: up to TWO %s placeholders. First gets the SUBJECT, second
// gets the BODY. Anything else passes through literally — no printf parsing
// (avoids format-string injection from user-supplied templates).
//
// Substitutions are shell-escaped (internal ' → '\'') but NOT JSON-escaped.
// For Discord/Slack JSON payloads, " and \ in the message body will break
// the JSON layer. Engine-generated alert text is plain; if you want bullet-
// proof JSON, wrap your curl invocation in a helper script that handles
// JSON escaping (e.g., via `jq -Rs` or `python -c 'import json,sys;...'`).
//
// Threading: popen forks /bin/sh; pclose reaps. Worker thread blocks until
// the command exits — a hung curl will back up the queue. Recommend prepending
// `timeout 10 ` to the cfg default. Worker thread is dedicated so this won't
// affect the engine hot path.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[512B]
// [ALIGN]_[1]
// [CACHE_LINES]_[8]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[NotifyCommandState]
//======================================================================

//======================================================================
// [FUNCTION]_[Notify_ShellEscape]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[single-quote shell escaping (' -> '\'' close-escape-reopen) — caller's template supplies the enclosing quotes]
//======================================================================
// [CODE]
//======================================================================
static inline void Notify_ShellEscape(char *out, size_t out_cap, const char *in) {
    if (out_cap == 0) return;
    size_t pos = 0;
    while (*in && pos + 4 < out_cap) {
        if (*in == '\'') {
            // close-escape-reopen: '\''
            out[pos++] = '\'';
            out[pos++] = '\\';
            out[pos++] = '\'';
            out[pos++] = '\'';
        } else {
            out[pos++] = *in;
        }
        in++;
    }
    out[pos] = '\0';
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Shell-quote `in` into `out`. Replaces internal ' with '\'' (close-escape-
// reopen). Does NOT add enclosing quotes — the USER TEMPLATE provides them
// (e.g., `notify-send 'Engine: %s' '%s'`). Worst case ~4x input + 1.
//
// Why not enclose: the user's template typically already has '%s' (single-
// quoted placeholder) so it can sit inside JSON strings, dunst args, curl -d
// payloads, etc. Doubly-quoting (template's quote + our enclosure) produces
// ''-pairs that bash parses as empty strings, breaking the surrounding
// quoted region. Standard shell-escape contract: caller wraps with quotes.
//
// Limitations: assumes user's surrounding context is SINGLE quotes. For
// JSON payloads (curl -d '{"content":"%s"}'), values with " or \ also need
// JSON escaping — out of scope here. Plain-text alerts (the engine's
// default format) work everywhere; messages with ", \, or other JSON-
// special chars may break the JSON layer when sent to Discord/Slack.
// Document this for users; provide a wrapper script if needed.
//======================================================================
// [END_FUNCTION]_[Notify_ShellEscape]
//======================================================================

//======================================================================
// [FUNCTION]_[Notify_BuildCommand]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[manual two-%s template substitution (subject, body) — never printf, so a cfg template with stray %d/%n can't inject]
//======================================================================
// [CODE]
//======================================================================
static inline int Notify_BuildCommand(char *out, size_t out_cap,
                                       const char *tmpl,
                                       const char *subj_esc,
                                       const char *body_esc) {
    size_t pos = 0;
    int n_subs = 0;
    const char *p = tmpl;
    while (*p && pos + 1 < out_cap) {
        if (p[0] == '%' && p[1] == 's' && n_subs < 2) {
            const char *sub = (n_subs == 0) ? subj_esc : body_esc;
            size_t sub_len = strlen(sub);
            if (pos + sub_len + 1 >= out_cap) {
                out[pos] = '\0';
                return 0; // truncated
            }
            memcpy(out + pos, sub, sub_len);
            pos += sub_len;
            p += 2;
            n_subs++;
        } else {
            out[pos++] = *p++;
        }
    }
    out[pos] = '\0';
    return *p == '\0';
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Manual %s substitution — NOT printf-based. Prevents format-string injection
// from user-supplied templates (template comes from cfg, may have stray %d/%n).
// Substitutes up to two %s with subject + body in order. Other characters
// pass through literally. Returns 1 on success, 0 if the command buffer
// overflowed before completion (still tries to run what fit).
//======================================================================
// [END_FUNCTION]_[Notify_BuildCommand]
//======================================================================

//======================================================================
// [FUNCTION]_[NotifyBackend_Command]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[escape subject/body, build the command, popen + drain + pclose; fire-and-forget on the dedicated worker]
//======================================================================
// [CODE]
//======================================================================
static inline int NotifyBackend_Command(const NotifyEvent *evt, void *state) {
    NotifyCommandState *cs = (NotifyCommandState *)state;
    if (!cs || cs->template_str[0] == '\0') return -1;

    // Shell-escape both fields (just internal ' replacement; no enclosure).
    // Buffers sized so even all-quote input stays bounded: 4x growth + null.
    char esc_subj[1024];   // subject is 128 → worst case 4*128 + 1 = 513
    char esc_body[3072];   // body is 512 → worst case 4*512 + 1 = 2049
    Notify_ShellEscape(esc_subj, sizeof(esc_subj), evt->subject);
    Notify_ShellEscape(esc_body, sizeof(esc_body), evt->body);

    char cmd[8192];
    Notify_BuildCommand(cmd, sizeof(cmd), cs->template_str, esc_subj, esc_body);

    FILE *f = popen(cmd, "r");
    if (!f) {
        fprintf(stderr, "[NOTIFY] popen failed for command backend (cmd truncated to %.80s...)\n", cmd);
        return -1;
    }
    // Drain stdout so the child doesn't block on a full pipe. We don't care
    // about the output — the alert is fire-and-forget.
    char drain[256];
    while (fread(drain, 1, sizeof(drain), f) > 0) {}
    pclose(f); // reap child to avoid zombies
    return 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[NotifyBackend_Command]
//======================================================================

#endif // NOTIFY_HPP
