// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/DirCreate.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[FoxDir_CreateParents — the ONE mkdir -p walker (extracted from PaperResetArchive at E.1.2.D D-a so the model-state savers could share it instead of duplicating the walk); returns 1 on success-or-exists, 0 on hard failure, logging the failing component]
//======================================================================================================
#ifndef DIR_CREATE_HPP
#define DIR_CREATE_HPP

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
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

#endif // DIR_CREATE_HPP
