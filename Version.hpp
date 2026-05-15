#pragma once
// single source of truth for version string
// update HERE only — all renderers include this

#define ENGINE_VERSION_MAJOR 5
#define ENGINE_VERSION_MINOR 15
#define ENGINE_VERSION_PATCH 5
#define ENGINE_VERSION_STRING "5.15.5.F.4c"
// .F.4c (v5.15.5.F.4c) — Bitmap dispatcher framework + SettingsPanel direct-cfg
// refactor (Option 2 mid-session rescope 2026-05-14 evening) + 63 KIND_INT/_BOOL
// cohort migrations to FOREACH_CFG_FIELD + new DESIGN_SPEC
// (universal-registry-bitmap-dispatcher-pattern.md). 3144 tests pass (3118 baseline
// + 26 new sub-tests T7-T18 covering KIND_INT/_BOOL dispatch + bitmap framework
// popcount/iteration + tt::cfg_assign/diff sisters + HIGH-6 tooltip preservation).
// Hot path UNTOUCHED. Per-ship Version.hpp bump discipline restored at .F.4b.
