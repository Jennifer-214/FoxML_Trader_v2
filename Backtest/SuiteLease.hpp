// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[Backtest/SuiteLease.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[BACKTEST] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the suite's ONE run lease — the sole authority for "a suite run owns the backtest engine and the shared dataset" — and the ONE launch funnel every suite worker starts through; std + pthread only, so the suite's cells and a headless caller use it without ImGui]
// [CONTAINS]
//   - [STRUCT]_[SuiteLeaseState]
//   - [STRUCT]_[SuiteWorkerSlot]
//   - [FUNCTION]_[SuiteLease_TryAcquire]
//   - [FUNCTION]_[SuiteLease_Release]
//   - [FUNCTION]_[SuiteWorker_LaunchWith]
// [REFERENCE]_[DECISION]_[[D-503]]
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
// [OVERVIEW]_[what the trampoline needs — the worker, its args and its lease; ONE slot serves because the lease admits one worker at a time and the trampoline copies the slot out before anything can reuse it]
//======================================================================
// [CODE]
//======================================================================
struct SuiteWorkerSlot {
    SuiteWorkerFn fn;
    void*         args;
    uint64_t      lease;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-10-01]
//----------------------------------------------------------------------
// [SIZE]_[24B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[SuiteWorkerSlot]
//======================================================================

inline SuiteWorkerSlot g_suite_worker_slot{};

inline void SuiteWorker_ReleaseOnExit(void* lease) {
    SuiteLease_Release(*static_cast<const uint64_t*>(lease));
}

// A cleanup handler, not a call after fn: pthread_exit and a cancellation never come back to this frame.
// The lease goes back however the worker's thread ends.
inline void* SuiteWorker_Trampoline(void* p) {
    SuiteWorkerSlot s = *static_cast<const SuiteWorkerSlot*>(p);
    void* r = nullptr;
    pthread_cleanup_push(SuiteWorker_ReleaseOnExit, &s.lease);
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
// [OVERVIEW]_[start a suite worker through the lease — take it or refuse naming the holder; spawn; a failed spawn gives it back; the trampoline gives it back when the worker's thread ends, however it ends. On a refusal the caller still owns `args`, and `why` says what happened]
//======================================================================
// [CODE]
//======================================================================
inline uint8_t SuiteWorker_LaunchWith(SuiteSpawnFn spawn, const char* name, SuiteWorkerFn fn, void* args,
                                      char* why, size_t why_cap) {
    const uint64_t lease = SuiteLease_TryAcquire(name);
    if (!lease) {
        const char* holder = SuiteLease_Holder();
        if (why && why_cap) snprintf(why, why_cap, "%s: busy — %s is running", name, holder ? holder : "another suite run");
        return SUITE_LAUNCH_REFUSED_BUSY;
    }
    g_suite_worker_slot = SuiteWorkerSlot{fn, args, lease};
    const int rc = spawn(SuiteWorker_Trampoline, &g_suite_worker_slot);
    if (rc != 0) {
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

inline uint8_t SuiteWorker_Launch(const char* name, SuiteWorkerFn fn, void* args, char* why, size_t why_cap) {
    return SuiteWorker_LaunchWith(SuiteWorker_SpawnDetached, name, fn, args, why, why_cap);
}
