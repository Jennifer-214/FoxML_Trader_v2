// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/CfgPaths.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the cfg-filename SSoT — every literal that OPENS a cfg file names a constant here; retired names recorded append-only (H21)]
// [CONTAINS]
//   - [SECTION]_[the cfg filenames]
//   - [SECTION]_[retired cfg filenames]
//======================================================================================================

#pragma once

//======================================================================
// [SECTION]_[the cfg filenames]
//----------------------------------------------------------------------
// Relative to the process's working directory. A literal that OPENS a cfg file (fopen / Load / LoadSecrets /
// a panel's default path) names one of these; a message that merely MENTIONS a filename stays a literal.
//======================================================================

// The engine's cfg when its command line names none (main.cpp), and the layout foxml_suite treats as the
// engine's cfg (its first-run copy source + its drift check). A DEFAULT, not the engine's cfg: an engine
// run may name another file, so a comparison against this path compares the default layout only.
inline constexpr const char* CFG_PATH_DEFAULT_ENGINE_CFG = "engine.cfg";

// foxml_suite's own cfg — the default path of its Settings panel, RunControl and Optimizer.
inline constexpr const char* CFG_PATH_BACKTEST_CFG = "backtest.cfg";

// The venue API key + secret, read by LoadSecrets on a live boot only.
inline constexpr const char* CFG_PATH_SECRETS_CFG = "secrets.cfg";

//======================================================================
// [SECTION]_[retired cfg filenames]
//----------------------------------------------------------------------
// H21 — append-only: a retired name is never reused for a different file. No path opens these; they are
// recorded so this header lists every cfg filename the engine has used.
//======================================================================
inline constexpr const char* CFG_PATH_RETIRED_NAMES[] = {
    "engine_sharded.cfg",   // the sharded engine's separate cfg during its 2026-04 bring-up (OMS phases 01-02)
};
