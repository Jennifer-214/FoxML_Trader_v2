#pragma once
// single source of truth for version string
// update HERE only — all renderers include this

#define ENGINE_VERSION_MAJOR 5
#define ENGINE_VERSION_MINOR 15
#define ENGINE_VERSION_PATCH 5
#define ENGINE_VERSION_STRING "5.15.5.F.4b"
// Note: bumping 5.15.5.E → 5.15.5.F.4b skips ships F.1/F.2/F.3/F.6 which all
// missed the per-ship Version.hpp bump per feedback_bump_version_per_ship rule.
// .F.4b backfills the bump; future ships restore per-ship discipline.
