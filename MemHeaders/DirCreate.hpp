// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/DirCreate.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[FoxDir_CreateParents — the ONE mkdir -p walker (extracted from PaperResetArchive at E.1.2.D D-a so the model-state savers could share it instead of duplicating the walk); returns 1 on success-or-exists, 0 on hard failure, logging the failing component. Plus the D-483 C directory primitives: FoxDir_SameDir (dev+ino identity), FoxDir_LockExclusive / FoxDir_Unlock (the per-state-dir exclusive lock — one writer per dir, across nodes AND processes)]
// [CONTAINS]
//   - [FUNCTION]_[FoxDir_CreateParents]
//   - [FUNCTION]_[FoxDir_SameDir]
//   - [FUNCTION]_[FoxDir_LockExclusive]
//   - [FUNCTION]_[FoxDir_Unlock]
//======================================================================================================
#ifndef DIR_CREATE_HPP
#define DIR_CREATE_HPP

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>   // D-483 C — flock
#include <fcntl.h>      // D-483 C — open + O_CLOEXEC
#include <unistd.h>     // D-483 C — close
#include <errno.h>

//======================================================================
// [FUNCTION]_[FoxDir_CreateParents]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[mkdir -p semantics in C — create each path component if missing; 1 = success or already-exists, 0 = hard failure (permission, missing device, path too long)]
//======================================================================
// Extracted VERBATIM from PaperResetArchive_CreateDirectories (E.1.2.D
// D-a no-regret batch, 2026-08-22) — both /decision-check halves
// independently prescribed save-side dir provisioning for the four
// bandit/Thompson state writers, and duplicating the walker would have
// been the exact parallel-implementation sin. PaperResetArchive keeps
// its original name as a forwarder. Log prefix generalized
// "[archive]" -> "[mkdir]" (diagnostics only; nothing parses it).
//
// Path must be <= 512 chars. Recurses by null-terminating at each '/'
// and calling mkdir() incrementally.
//======================================================================
// [CODE]
//======================================================================
inline int FoxDir_CreateParents(const char* path) {
    if (!path || !path[0]) return 0;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", path);
    size_t len = std::strlen(buf);
    if (len == 0 || len >= sizeof(buf) - 1) return 0;
    // Walk path; whenever we hit '/', temporarily null-terminate + mkdir up to that point
    for (size_t i = 1; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            int rc = ::mkdir(buf, 0755);
            if (rc != 0 && errno != EEXIST) {
                std::fprintf(stderr, "[mkdir] mkdir(%s) failed: %s\n", buf, std::strerror(errno));
                return 0;
            }
            buf[i] = '/';
        }
    }
    // mkdir the final component (no trailing /)
    int rc = ::mkdir(buf, 0755);
    if (rc != 0 && errno != EEXIST) {
        std::fprintf(stderr, "[mkdir] mkdir(%s) failed: %s\n", buf, std::strerror(errno));
        return 0;
    }
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[FoxDir_CreateParents]
//======================================================================

//======================================================================
// [FUNCTION]_[FoxDir_SameDir]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[D-483 C — directory IDENTITY, not spelling: 1 when both paths resolve to the same inode (dev+ino; symlinks, trailing slashes and ./ prefixes compare equal), else a plain string compare when either side does not exist yet]
// [REFERENCE]_[DECISION]_[D-483]
//======================================================================
// [CODE]
//======================================================================
// Why not strcmp: two nodes on "models/x" and "models/x/" share every state
// file and would compare unequal; a symlinked family dir likewise. Why the
// strcmp fallback: a dir that does not exist yet has no inode — its spelling
// is all there is to compare (both sides absent + equal spelling ⇒ same).
// Cold path only (boot / hot-swap / GUI) — two stat() syscalls.
inline int FoxDir_SameDir(const char* a, const char* b) {
    if (!a || !b || !a[0] || !b[0]) return 0;
    struct stat sa, sb;
    if (::stat(a, &sa) == 0 && ::stat(b, &sb) == 0) {
        return (sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino) ? 1 : 0;
    }
    return (std::strcmp(a, b) == 0) ? 1 : 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[FoxDir_SameDir]
//======================================================================

//======================================================================
// [FUNCTION]_[FoxDir_LockExclusive]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[D-483 C — take the EXCLUSIVE lock on <dir>/<lock_name> (flock LOCK_EX|LOCK_NB on an O_CLOEXEC descriptor); 1 = acquired (fd out), 0 = held elsewhere (another node OR another process), -1 = cannot create/open the lock file. Never blocks (H3)]
// [REFERENCE]_[DECISION]_[D-483]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-331]
//======================================================================
// [CODE]
//======================================================================
// WHY flock and not a pid file: flock dies with the last descriptor of its
// open file description — a crashed owner leaves nothing stale to clean up.
// WHY it also guards two NODES in ONE process: flock is per OPEN FILE
// DESCRIPTION, so a second open() of the same lock file in the same process
// gets EWOULDBLOCK (verified 2026-09-04, kernel 7.2.2). A same-dir handover
// therefore dup()s the holder's descriptor instead of re-acquiring.
// WHY O_CLOEXEC: the engine spawns children (Health_Log rotation `system()`,
// Notify `popen()`, the GUI's Open-Folder fork/exec); without it an exec'd
// child would carry the lock past the engine's own lifetime.
// The WRITER provisions the dir (FoxDir_CreateParents) — the D-a placement
// rule: read paths never mkdir, write paths do. LOCK_NB: a held lock is an
// ANSWER (0), never a wait — this runs on boot / hot-swap paths only.
inline int FoxDir_LockExclusive(const char* dir, const char* lock_name, int* out_fd) {
    if (out_fd) *out_fd = -1;
    if (!out_fd || !dir || !dir[0] || !lock_name || !lock_name[0]) return -1;
    if (!FoxDir_CreateParents(dir)) return -1;
    char path[512];
    int n = std::snprintf(path, sizeof(path), "%s/%s", dir, lock_name);
    if (n <= 0 || n >= (int)sizeof(path)) return -1;
    int fd = ::open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "[dirlock] open(%s) failed: %s\n", path, std::strerror(errno));
        return -1;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;
        ::close(fd);
        if (e == EWOULDBLOCK || e == EAGAIN) return 0;   // held elsewhere — the D-483 refusal
        std::fprintf(stderr, "[dirlock] flock(%s) failed: %s\n", path, std::strerror(e));
        return -1;
    }
    *out_fd = fd;
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[FoxDir_LockExclusive]
//======================================================================

//======================================================================
// [FUNCTION]_[FoxDir_Unlock]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[D-483 C — release a FoxDir_LockExclusive descriptor: close() drops this reference; the lock itself ends when the LAST descriptor of the open file description closes (a dup'd successor keeps it). Idempotent on -1]
// [REFERENCE]_[DECISION]_[D-483]
//======================================================================
// [CODE]
//======================================================================
inline void FoxDir_Unlock(int* fd) {
    if (!fd || *fd < 0) return;
    ::close(*fd);
    *fd = -1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[FoxDir_Unlock]
//======================================================================

#endif // DIR_CREATE_HPP
