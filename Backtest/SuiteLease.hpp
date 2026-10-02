// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[Backtest/SuiteLease.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the suite's ONE run lease — the sole authority for "a suite run owns the backtest engine and the shared dataset" — the ONE gate every start button reads it through, the ONE launch funnel every suite worker starts through, and the SuiteJob block through which the funnel owns each worker's display state; std + pthread only, so the suite's cells and a headless caller use it without ImGui]
// [CONTAINS]
//   - [STRUCT]_[SuiteLeaseState]
//   - [STRUCT]_[SuiteGate]
//   - [STRUCT]_[SuiteJob]
//   - [STRUCT]_[SuiteWorkerSlot]
//   - [FUNCTION]_[SuiteLease_TryAcquire]
//   - [FUNCTION]_[SuiteLease_Release]
//   - [FUNCTION]_[SuiteLease_BusyLine]
//   - [FUNCTION]_[SuiteGate_NeedLease]
//   - [FUNCTION]_[SuiteJob_Begin]
//   - [FUNCTION]_[SuiteJob_End]
//   - [FUNCTION]_[SuiteWorker_LaunchWith]
// [REFERENCE]_[DECISION]_[[D-503] [D-506] [D-507]]
//======================================================================================================
//
// WHY ONE LEASE: two resources need one owner at a time — BacktestSharded_Run's function-local statics (any two
// concurrent entries race) and the shared RunControlState::results (reset by Backtest_Run, post-processed by the
// collect / backtest workers after it returns, read by every training worker). Eight workers used to start on their
// own, each with its own `running` flag, and exclusion was a hand-kept OR of some of those flags that had already
// drifted. Now the buttons' can-start gate and Backtest_Run's entry read the SAME word.
//
// WHY ONE FUNNEL: a lock taken by hand at each launch site is a convention a new worker can forget. The funnel takes
// the lease before it spawns, releases it when the spawn fails, and its trampoline releases it however the worker's
// thread ends (a return, pthread_exit, a cancellation) — every exit path of every worker, by construction. A headless
// caller (no thread) takes the lease through SuiteLease_TryAcquire / SuiteLease_Release directly.
//
// One suite run at a time follows: while any worker holds the lease, every other start refuses, naming the holder.
//
// WHY ONE GATE (D-507): a start button used to decide "enabled" with one hand-written expression and "why not" with a
// second, and the two drifted — a Collect greyed out by a running Walk-Forward said "Select data files first". A
// SuiteGate is both at once: enabled iff every term holds, the reason the first term that does not, and the lease is
// one of its terms, worded the way the funnel words a refusal.
//
// WHY ONE JOB BLOCK PER WORKER (D-506): each worker's display state used to be a loose group of fields — running,
// cancel, progress, complete, a thread id — reset at its click site and set back by the worker, rolled back on a failed
// start at two of eight sites, its "result readable" flag in three different forms. Deleting a worker left the group
// behind (the Train Model fields outlived their worker, deleted at f64c324, until MP-6). A SuiteJob is that group as one cache line; the
// funnel resets it under the lease and shows the run, the trampoline clears it however the thread ends, and deleting a
// worker deletes one member.

#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <time.h>

//======================================================================
// [STRUCT]_[SuiteLeaseState]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the lease word (0 = free, else the holder's token), the holder's name for "busy" lines, the last token issued (never reused, so a stale token can never release a later holder), the count of releases refused for a token that does not hold the lease, and when the holder took it (its journal line says how long it held)]
//======================================================================
// [CODE]
//======================================================================
struct alignas(64) SuiteLeaseState {
    volatile uint64_t    token;          // written by acquirers and the holder; read by every button and Backtest_Run
    const char* volatile holder;         // set by the winning acquirer, cleared by its release — display only
    volatile uint64_t    issued;         // the last token handed out (an atomic add)
    volatile uint64_t    bad_releases;   // a release by a token that does not hold the lease: a bug, counted and printed
    volatile uint64_t    acquired_ns;    // the monotonic clock when the holder took it — written by the acquirer, read
                                         // by its release (the same thread, or after the funnel's spawn)
};
static_assert(alignof(SuiteLeaseState) == 64, "the lease is cross-thread state — its own cache line (H6)");
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-01]
//----------------------------------------------------------------------
// [SIZE]_[64B]
// [ALIGN]_[64]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[SuiteLeaseState]
//======================================================================

inline SuiteLeaseState g_suite_lease{};   // C++17 inline: ONE lease per process, whichever translation units include this

inline bool SuiteLease_HeldBy(uint64_t token) {
    return token != 0 && __atomic_load_n(&g_suite_lease.token, __ATOMIC_ACQUIRE) == token;
}

inline bool SuiteLease_Busy() {
    return __atomic_load_n(&g_suite_lease.token, __ATOMIC_ACQUIRE) != 0;
}

// The holder's name while held; nullptr when free (and for the instant before a winner publishes it).
inline const char* SuiteLease_Holder() {
    return __atomic_load_n(&g_suite_lease.holder, __ATOMIC_ACQUIRE);
}

//======================================================================
// [FUNCTION]_[SuiteLease_BusyLine]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE wording of "a suite run holds the lease" — the funnel's refusal and every start button the lease greys out write it, so the two never say different things; the holder's name is null for the instant around an acquire or a release, and the line then names no one]
//======================================================================
// [CODE]
//======================================================================
inline void SuiteLease_BusyLine(char* buf, size_t cap) {
    if (!buf || !cap) return;
    const char* holder = SuiteLease_Holder();
    snprintf(buf, cap, "busy — %s is running", holder ? holder : "another suite run");
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteLease_BusyLine]
//======================================================================

// A name for a journal line: callers pass string literals, but a line never prints a null.
inline const char* SuiteLease_NameOr(const char* name) { return name ? name : "(unnamed)"; }

// The monotonic clock, in nanoseconds — how long a holder held the lease, immune to a wall-clock step. Journald stamps
// each line with the wall clock itself.
inline uint64_t SuiteLease_NowNs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

//======================================================================
// [FUNCTION]_[SuiteLease_TryAcquire]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[take the lease for `holder` (a string with static storage — the lease keeps the pointer while held) — a fresh token, or 0 when another run holds it; never waits; either outcome is one journal line]
//======================================================================
// THE JOURNAL LINES (D-507 — her journalctl monitoring, D-493): the lease logs its own transitions, one line each on
// stderr, so every caller — the suite's funnel, a headless verb — leaves the same record: "acquired by <name> (token
// N)", "refused <name>: busy — <holder> is running", and Release's "released by <name> (token N) after <S> s". Only a
// start attempt logs: the buttons' gates read SuiteLease_Busy, which never does.
//======================================================================
// [CODE]
//======================================================================
inline uint64_t SuiteLease_TryAcquire(const char* holder) {
    const uint64_t mine     = __atomic_add_fetch(&g_suite_lease.issued, 1, __ATOMIC_RELAXED);
    uint64_t       expected = 0;
    if (!__atomic_compare_exchange_n(&g_suite_lease.token, &expected, mine, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        char busy[128];
        SuiteLease_BusyLine(busy, sizeof(busy));
        fprintf(stderr, "[suite-lease] refused %s: %s\n", SuiteLease_NameOr(holder), busy);
        return 0;
    }
    __atomic_store_n(&g_suite_lease.acquired_ns, SuiteLease_NowNs(), __ATOMIC_RELAXED);
    __atomic_store_n(&g_suite_lease.holder, holder, __ATOMIC_RELEASE);
    fprintf(stderr, "[suite-lease] acquired by %s (token %llu)\n", SuiteLease_NameOr(holder), (unsigned long long)mine);
    return mine;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteLease_TryAcquire]
//======================================================================

//======================================================================
// [FUNCTION]_[SuiteLease_Release]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[give the lease back — only its holder's token can; any other token is a bug, refused, counted and printed, the lease untouched]
//======================================================================
// [CODE]
//======================================================================
inline bool SuiteLease_Release(uint64_t token) {
    if (!SuiteLease_HeldBy(token)) {
        __atomic_add_fetch(&g_suite_lease.bad_releases, 1, __ATOMIC_RELAXED);
        fprintf(stderr, "[suite-lease] CRITICAL: a release by token %llu, which does not hold the lease — refused\n",
                (unsigned long long)token);
        return false;
    }
    const char*    name    = SuiteLease_Holder();
    const uint64_t held_ns = SuiteLease_NowNs() - __atomic_load_n(&g_suite_lease.acquired_ns, __ATOMIC_RELAXED);
    // the name is cleared first: once the word reads 0, a new holder may publish its own
    __atomic_store_n(&g_suite_lease.holder, (const char*)nullptr, __ATOMIC_RELAXED);
    uint64_t expected = token;
    const bool released = __atomic_compare_exchange_n(&g_suite_lease.token, &expected, (uint64_t)0, false,
                                                      __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    if (released)
        fprintf(stderr, "[suite-lease] released by %s (token %llu) after %.3f s\n", SuiteLease_NameOr(name),
                (unsigned long long)token, (double)held_ns / 1e9);   // display only — a log line (H4)
    return released;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteLease_Release]
//======================================================================

//======================================================================
// [STRUCT]_[SuiteGate]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[a start button's gate — open iff every term holds; closed, it carries the FIRST term that failed as the button's reason, in that term's tone. The lease is a term (SuiteGate_NeedLease), so every button reads the one authority. Built on the GUI thread's stack, every frame]
//======================================================================
// Terms go in the order the operator can act on them — what this build can do, what she can fix now, whether the suite
// is free, what a run must produce first — so the reason a button shows is the next thing to do. The enablement and the
// reason are one value: neither can say something the other does not (Class 2).
//======================================================================
// [CODE]
//======================================================================
enum SuiteGateTone : uint8_t {
    SUITE_GATE_WAIT = 0,   // clears with time or the next step — shown greyed
    SUITE_GATE_FIX  = 1,   // an input that contradicts itself — shown as a warning until she corrects it
};

struct SuiteGate {
    bool    closed  = false;             // a term failed; a new gate starts with nothing against it
    uint8_t tone    = SUITE_GATE_WAIT;   // the failed term's SuiteGateTone
    char    why[62] = {};                // the failed term's reason, as the button shows it
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-02]
//----------------------------------------------------------------------
// [SIZE]_[64B]
// [ALIGN]_[1]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[SuiteGate]
//======================================================================

inline bool SuiteGate_Open(const SuiteGate* g) { return !g->closed; }

// A term that does not hold closes the gate with its reason (printf-style, so a reason can carry its numbers) — unless
// an earlier term already did: the first failure is the one a button shows.
__attribute__((format(printf, 4, 0)))
inline void SuiteGate_Term(SuiteGate* g, bool holds, uint8_t tone, const char* fmt, va_list ap) {
    if (holds || g->closed) return;
    g->closed = true;
    g->tone   = tone;
    vsnprintf(g->why, sizeof(g->why), fmt, ap);
}

// A term that clears with time or the next step.
__attribute__((format(printf, 3, 4)))
inline void SuiteGate_Need(SuiteGate* g, bool holds, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SuiteGate_Term(g, holds, SUITE_GATE_WAIT, fmt, ap);
    va_end(ap);
}

// A term only the operator's correction clears — an input that contradicts itself.
__attribute__((format(printf, 3, 4)))
inline void SuiteGate_NeedFix(SuiteGate* g, bool holds, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SuiteGate_Term(g, holds, SUITE_GATE_FIX, fmt, ap);
    va_end(ap);
}

//======================================================================
// [FUNCTION]_[SuiteGate_NeedLease]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the lease as a start button's term — while a suite run holds it the gate closes naming the holder, in the funnel's own wording (SuiteLease_BusyLine)]
//======================================================================
// The gate is read on the GUI thread in the frame of its click, and in the suite the GUI thread is the lease's only
// acquirer — so a button that read the lease free cannot be refused BUSY when clicked. The funnel still decides; this
// term is what makes the button say so before the click instead of after it.
//======================================================================
// [CODE]
//======================================================================
inline void SuiteGate_NeedLease(SuiteGate* g) {
    if (g->closed || !SuiteLease_Busy()) return;
    g->closed = true;
    g->tone   = SUITE_GATE_WAIT;
    SuiteLease_BusyLine(g->why, sizeof(g->why));
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteGate_NeedLease]
//======================================================================

//======================================================================
// [STRUCT]_[SuiteJob]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [THREAD]_[[FUNNEL_WRITER] [SUITE_WORKER_WRITER] [GUI_READER]]
// [SYNC]_[ATOMIC]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one suite worker's display + control state — the funnel owns its start (resets it under the lease, then shows the run) and its end (the trampoline clears the run it shows); the worker writes its progress and publishes complete; the panel reads it through the accessors. DISPLAY only — every gate reads the lease]
//======================================================================
// [CODE]
//======================================================================
struct alignas(64) SuiteJob {
    volatile uint64_t running;    // the lease token of the run it shows, 0 = none — written ONLY by SuiteJob_Begin / _End
    volatile int      cancel;     // set by the panel's Cancel, polled by the worker
    volatile int      progress;   // the worker's — a percent when total == 0, else a count out of total
    volatile int      total;
    volatile int      complete;   // the worker publishes "this run's result is readable" LAST (release); read with acquire
};
static_assert(sizeof(SuiteJob) == 64 && alignof(SuiteJob) == 64, "one suite job = one cache line (H6)");
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-01]
//----------------------------------------------------------------------
// [SIZE]_[64B]
// [ALIGN]_[64]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[SuiteJob]
//======================================================================

// Display only: a gate reads SuiteLease_Busy(), never this — two readers of "is a run on" is the split authority D-503
// closed.
inline bool SuiteJob_Running(const SuiteJob* j) { return __atomic_load_n(&j->running, __ATOMIC_ACQUIRE) != 0; }

// The run's result is readable — pairs with the worker's SuiteJob_Publish.
inline bool SuiteJob_Done(const SuiteJob* j) { return __atomic_load_n(&j->complete, __ATOMIC_ACQUIRE) != 0; }

// The worker, after its last write to the result.
inline void SuiteJob_Publish(SuiteJob* j) { __atomic_store_n(&j->complete, 1, __ATOMIC_RELEASE); }

// The panel, when the data a finished result describes is gone (a new collect) — the job is not running then.
inline void SuiteJob_Forget(SuiteJob* j) { __atomic_store_n(&j->complete, 0, __ATOMIC_RELAXED); }

inline void SuiteJob_Cancel(SuiteJob* j) { __atomic_store_n(&j->cancel, 1, __ATOMIC_RELAXED); }

//======================================================================
// [FUNCTION]_[SuiteJob_Begin]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the funnel's start of a job — under the lease, before the spawn: forget the last run, then show this one (running carries the run's lease token)]
//======================================================================
// [CODE]
//======================================================================
inline void SuiteJob_Begin(SuiteJob* j, uint64_t lease) {
    __atomic_store_n(&j->cancel, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&j->progress, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&j->total, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&j->complete, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&j->running, lease, __ATOMIC_RELEASE);   // last: whoever sees the run sees it reset
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteJob_Begin]
//======================================================================

//======================================================================
// [FUNCTION]_[SuiteJob_End]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the end of a job's run — clear running only while it still shows THIS run (a compare-and-swap on its lease token), so a stale exit can never clear a later run's display; false = it shows another run, left alone]
//======================================================================
// [CODE]
//======================================================================
inline bool SuiteJob_End(SuiteJob* j, uint64_t lease) {
    uint64_t expected = lease;
    return __atomic_compare_exchange_n(&j->running, &expected, (uint64_t)0, false,
                                       __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteJob_End]
//======================================================================

//======================================================================
// [SECTION]_[the launch funnel]
//----------------------------------------------------------------------
// A suite worker runs holding `lease` and passes it to whatever needs the proof (Backtest_Run). The spawn primitive
// is a parameter so the cells can make a thread start fail; production spawns a detached pthread.
//======================================================================
typedef void* (*SuiteWorkerFn)(void* args, uint64_t lease);
typedef int   (*SuiteSpawnFn)(void* (*entry)(void*), void* arg);   // 0, or an errno

enum SuiteLaunch : uint8_t {
    SUITE_LAUNCH_REFUSED_BUSY = 0,   // the zero value refuses, so an unset result never reads as started
    SUITE_LAUNCH_SPAWN_FAILED = 1,
    SUITE_LAUNCH_STARTED      = 2,
};

//======================================================================
// [STRUCT]_[SuiteWorkerSlot]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[what the trampoline needs — the worker, its args, its lease and its job; ONE slot serves because the lease admits one worker at a time and the trampoline copies the slot out before anything can reuse it]
//======================================================================
// [CODE]
//======================================================================
struct SuiteWorkerSlot {
    SuiteWorkerFn fn;
    void*         args;
    uint64_t      lease;
    SuiteJob*     job;        // nullptr = a worker with no display
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-01]
//----------------------------------------------------------------------
// [SIZE]_[32B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[SuiteWorkerSlot]
//======================================================================

inline SuiteWorkerSlot g_suite_worker_slot{};

// The job's display first (only while it still shows this run), then the lease: once the lease is free a new run may
// start and show itself.
inline void SuiteWorker_ReleaseOnExit(void* slot) {
    const SuiteWorkerSlot* s = static_cast<const SuiteWorkerSlot*>(slot);
    if (s->job) SuiteJob_End(s->job, s->lease);
    SuiteLease_Release(s->lease);
}

// A cleanup handler, not a call after fn: pthread_exit and a cancellation never come back to this frame.
// The job ends and the lease goes back however the worker's thread ends.
inline void* SuiteWorker_Trampoline(void* p) {
    SuiteWorkerSlot s = *static_cast<const SuiteWorkerSlot*>(p);
    void* r = nullptr;
    pthread_cleanup_push(SuiteWorker_ReleaseOnExit, &s);
    r = s.fn(s.args, s.lease);
    pthread_cleanup_pop(1);           // 1 = run it now: the worker returned
    return r;
}

inline int SuiteWorker_SpawnDetached(void* (*entry)(void*), void* arg) {
    pthread_t tid;
    const int rc = pthread_create(&tid, NULL, entry, arg);
    if (rc == 0) pthread_detach(tid);
    return rc;
}

//======================================================================
// [FUNCTION]_[SuiteWorker_LaunchWith]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[start a suite worker through the lease — take it or refuse naming the holder (the job untouched, so its last result stays shown); begin the job; spawn; a failed spawn ends the job and gives the lease back; the trampoline does both when the worker's thread ends, however it ends. On a refusal the caller still owns `args`, and `why` says what happened]
//======================================================================
// [CODE]
//======================================================================
inline uint8_t SuiteWorker_LaunchWith(SuiteSpawnFn spawn, const char* name, SuiteJob* job, SuiteWorkerFn fn,
                                      void* args, char* why, size_t why_cap) {
    const uint64_t lease = SuiteLease_TryAcquire(name);
    if (!lease) {
        char busy[128];   // not the gate's line size: a refusal names a long holder in full
        SuiteLease_BusyLine(busy, sizeof(busy));
        if (why && why_cap) snprintf(why, why_cap, "%s: %s", name, busy);
        return SUITE_LAUNCH_REFUSED_BUSY;
    }
    if (job) SuiteJob_Begin(job, lease);
    g_suite_worker_slot = SuiteWorkerSlot{fn, args, lease, job};
    const int rc = spawn(SuiteWorker_Trampoline, &g_suite_worker_slot);
    if (rc != 0) {
        // the journal reads acquired → did not start → released, in that order
        char line[160];
        snprintf(line, sizeof(line), "%s: the worker did not start (%s)", SuiteLease_NameOr(name), strerror(rc));
        fprintf(stderr, "[suite-lease] %s\n", line);
        if (job) SuiteJob_End(job, lease);
        SuiteLease_Release(lease);
        if (why && why_cap) snprintf(why, why_cap, "%s", line);
        return SUITE_LAUNCH_SPAWN_FAILED;
    }
    return SUITE_LAUNCH_STARTED;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteWorker_LaunchWith]
//======================================================================

inline uint8_t SuiteWorker_Launch(const char* name, SuiteJob* job, SuiteWorkerFn fn, void* args, char* why,
                                  size_t why_cap) {
    return SuiteWorker_LaunchWith(SuiteWorker_SpawnDetached, name, job, fn, args, why, why_cap);
}
