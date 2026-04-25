// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [NOTIFY — operational alerting primitive (Phase 8b)]
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

//======================================================================================================
// [LEVELS + KINDS]
//======================================================================================================
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
    // append new kinds here; do NOT reorder
};
#define NOTIFY_KINDS_MAX 16

//======================================================================================================
// [EVENT + STATE]
//======================================================================================================
// Heap-free per event — queue holds POD by value.
struct NotifyEvent {
    int level;             // NotifyLevel
    int event_kind;        // NotifyKind (used as cooldown key)
    uint64_t timestamp_us; // CLOCK_MONOTONIC at enqueue time
    char subject[128];
    char body[512];
};

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

//======================================================================================================
// [INTERNAL HELPERS]
//======================================================================================================
static inline uint64_t Notify_NowMonotonicUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

//======================================================================================================
// [WORKER THREAD]
//======================================================================================================
// Drains the queue and invokes the backend. Backend is called WITHOUT the
// state lock held (so a slow backend doesn't block enqueue). On shutdown,
// drains remaining events before exiting (per test sidecar Group 5).
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

//======================================================================================================
// [API]
//======================================================================================================
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

// Non-blocking enqueue. Drops events on full queue (rate-limit failsafe).
// Cooldown gate uses CLOCK_MONOTONIC (NTP-jump-safe).
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

//======================================================================================================
// [STDERR BACKEND] — default, always available, ships in Phase 8b
//======================================================================================================
static inline int NotifyBackend_Stderr(const NotifyEvent *evt, void *state) {
    (void)state;
    static const char *level_str[] = {"INFO", "WARN", "ALERT", "CRITICAL"};
    int li = (evt->level >= 0 && evt->level < 4) ? evt->level : 0;
    fprintf(stderr, "[NOTIFY %s] %s — %s\n",
            level_str[li], evt->subject, evt->body);
    fflush(stderr); // make alerts visible immediately under tail -f
    return 0;
}

#endif // NOTIFY_HPP
