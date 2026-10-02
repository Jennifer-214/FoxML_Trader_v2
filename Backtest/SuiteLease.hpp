// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[Backtest/SuiteLease.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the suite's ONE run lease — the sole authority for "a suite run owns the backtest engine and the shared dataset" — the ONE launch funnel every suite worker starts through, and the SuiteJob block through which the funnel owns each worker's display state; std + pthread only, so the suite's cells and a headless caller use it without ImGui]
// [CONTAINS]
//   - [STRUCT]_[SuiteLeaseState]
//   - [STRUCT]_[SuiteJob]
//   - [STRUCT]_[SuiteWorkerSlot]
//   - [FUNCTION]_[SuiteLease_TryAcquire]
//   - [FUNCTION]_[SuiteLease_Release]
//   - [FUNCTION]_[SuiteJob_Begin]
//   - [FUNCTION]_[SuiteJob_End]
//   - [FUNCTION]_[SuiteWorker_LaunchWith]
// [REFERENCE]_[DECISION]_[[D-503] [D-506]]
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
// WHY ONE JOB BLOCK PER WORKER (D-506): each worker's display state used to be a loose group of fields — running,
// cancel, progress, complete, a thread id — reset at its click site and set back by the worker, rolled back on a failed
// start at two of eight sites, its "result readable" flag in three different forms. Deleting a worker left the group
// behind (the Train Model fields outlived their worker, deleted at f64c324, until MP-6). A SuiteJob is that group as one cache line; the
// funnel resets it under the lease and shows the run, the trampoline clears it however the thread ends, and deleting a
// worker deletes one member.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>

//======================================================================
// [STRUCT]_[SuiteLeaseState]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the lease word (0 = free, else the holder's token), the holder's name for "busy" lines, the last token issued (never reused, so a stale token can never release a later holder) and the count of releases refused for a token that does not hold the lease]
//======================================================================
// [CODE]
//======================================================================
struct alignas(64) SuiteLeaseState {
    volatile uint64_t    token;          // written by acquirers and the holder; read by every button and Backtest_Run
    const char* volatile holder;         // set by the winning acquirer, cleared by its release — display only
    volatile uint64_t    issued;         // the last token handed out (an atomic add)
    volatile uint64_t    bad_releases;   // a release by a token that does not hold the lease: a bug, counted and printed
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

//======================================================================
// [FUNCTION]_[SuiteLease_TryAcquire]
//----------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[take the lease for `holder` (a string with static storage — the lease keeps the pointer while held) — a fresh token, or 0 when another run holds it; never waits]
//======================================================================
// [CODE]
//======================================================================
inline uint64_t SuiteLease_TryAcquire(const char* holder) {
    const uint64_t mine     = __atomic_add_fetch(&g_suite_lease.issued, 1, __ATOMIC_RELAXED);
    uint64_t       expected = 0;
    if (!__atomic_compare_exchange_n(&g_suite_lease.token, &expected, mine, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    __atomic_store_n(&g_suite_lease.holder, holder, __ATOMIC_RELEASE);
    return mine;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteLease_TryAcquire]
//======================================================================

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
    // the name is cleared first: once the word reads 0, a new holder may publish its own
    __atomic_store_n(&g_suite_lease.holder, (const char*)nullptr, __ATOMIC_RELAXED);
    uint64_t expected = token;
    return __atomic_compare_exchange_n(&g_suite_lease.token, &expected, (uint64_t)0, false,
                                       __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SuiteLease_Release]
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
        const char* holder = SuiteLease_Holder();
        if (why && why_cap) snprintf(why, why_cap, "%s: busy — %s is running", name, holder ? holder : "another suite run");
        return SUITE_LAUNCH_REFUSED_BUSY;
    }
    if (job) SuiteJob_Begin(job, lease);
    g_suite_worker_slot = SuiteWorkerSlot{fn, args, lease, job};
    const int rc = spawn(SuiteWorker_Trampoline, &g_suite_worker_slot);
    if (rc != 0) {
        if (job) SuiteJob_End(job, lease);
        SuiteLease_Release(lease);
        if (why && why_cap) snprintf(why, why_cap, "%s: the worker did not start (%s)", name, strerror(rc));
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
