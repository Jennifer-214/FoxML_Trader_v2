// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/PaperResetArchive.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[paper-reset session archiving — timestamped dir + snapshot + trades copy + summary.json before the OMS wipe]
// [CONTAINS]
//   - [FUNCTION]_[PaperResetArchive_FormatDirname]
//   - [FUNCTION]_[PaperResetArchive_CreateDirectories]
//   - [FUNCTION]_[Summary_WriteJson]
//======================================================================================================
// Operator-initiated paper-reset captures the prior session's state into a
// timestamped archive directory BEFORE wiping the OMS for the next session.
// Enables date-range session review + per-strategy / per-regime aggregation
// on completed sessions.
//
// Archive directory layout:
//
//   data/paper_resets/{start_iso}_to_{end_iso}.paper/
//     ├── snapshot.dat    — ShardedSnapshot_Save output (full OMS + per-core state)
//     ├── trades.csv      — copy of logging/SYMBOL_order_history.csv at reset time
//     │                      (per-core split deferred to Phase 5.B; aggregate file
//     │                       is preserved for backward compat with TradeReader/GUI)
//     └── summary.json    — { session, global, per_node[], per_strategy[], per_regime[] }
//
// {start_iso} = ISO 8601 date from `paper_session_start_us` (e.g., 2026-05-13-091523)
// {end_iso}   = ISO 8601 date from current wall clock at reset time
//
// Reset flow integration (EngineSharded.hpp paper-reset handler):
//
//   1. Capture session_end_us = now_us()
//   2. PaperResetArchive_FormatDirname(start_us, end_us, dirname_buf, sizeof(dirname_buf))
//   3. PaperResetArchive_CreateDirectories(dirname_buf)
//   4. ShardedSnapshot_Save(&state, "<dirname>/snapshot.dat", partial_on)
//   5. rename(logging/SYMBOL_order_history.csv, <dirname>/trades.csv)
//   6. Summary_WriteJson("<dirname>/summary.json", state, cfg, num_nodes, session_end_us)
//   7. OMS_RESET_AUTOPOPULATE(state.oms, cfg.starting_balance)  — existing
//   8. NODE_CTX_RESET_AUTOPOPULATE loop                          — existing
//   9. ShardedTradeLog_Init (reopens fresh CSV; header written on empty file)
//   10. OrderEventLog_Reset(&state.oms->event_log)               — existing
//
// Per_regime aggregation DEFERRED: requires per-trade regime data which lives
// in the trades.csv (Phase 5.A added regime + regime_name columns). Aggregation
// requires parsing the CSV back; phase 6 emits empty `per_regime: []` placeholder.
// Phase 5.B (per-core CSV split) or a focused per-regime aggregator can fill
// this in via trade-log parse + group-by-regime sum.
//
// Operator-visible feature — adds FEATURE_LOOKUP.md entry at Phase 10 ship close.
//
// Cross-references:
//   DESIGN_SPECS/shadow-load-state-transition-pattern.md (capture-validate-publish)
//   bundled-plan Phase 6 section
//   CLAUDE.local.md "auto-write contract" for FEATURE_LOOKUP (operator-visible
//     features get a FEATURE_LOOKUP.md entry at sprint close)
//   CLAUDE.md item 13 (X-macro registry for per-core summary fields)
//======================================================================================================
#ifndef PAPER_RESET_ARCHIVE_HPP
#define PAPER_RESET_ARCHIVE_HPP

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include "../FixedPoint/FixedPointN.hpp"
#include "../MemHeaders/NodeCtxSummaryFieldRegistry.hpp"  // Summary_EmitPerCoreEntry + Summary_EmitPerStrategy + json_emit_*
#include "../MemHeaders/OmsStateFlagRegistry.hpp"         // MASK_OMS_STATE_KILL_SWITCH_TRIPPED
#include "../MemHeaders/BitmapMacros.hpp"                 // BITMAP_IS_SET
#include "../MemHeaders/DirCreate.hpp"                    // FoxDir_CreateParents (the mkdir -p SSoT; extracted from here at E.1.2.D D-a)

// Forward declarations — PaperResetArchive.hpp is included by EngineSharded.hpp
// AFTER EventLoopState<F> + ControllerConfig<F> + NodeContext<F> are defined.
// Templated helpers below take pointers / refs to incomplete types; template
// body access deferred to instantiation site.
//
// IMPORTANT — namespace placement: EventLoopState + NodeContext live in
// namespace tt (per ControllerEventLoop.hpp:77). ControllerConfig lives at
// GLOBAL scope (per ControllerConfig.hpp:280 — no enclosing namespace tt).
// Forward-declaring `tt::ControllerConfig<F>` here would create a DIFFERENT
// type from `::ControllerConfig<F>`, breaking template deduction at consumer
// sites. ControllerConfig is intentionally NOT forward-declared here —
// rely on the consumer (EngineSharded.hpp) to have ControllerConfig.hpp
// included BEFORE this header.
template <unsigned F> struct ControllerConfig;  // global scope (NOT in namespace tt)

namespace tt {

template <unsigned F> struct EventLoopState;

//------------------------------------------------------------------------------------------------------
// [SECTION]_[timestamp + dirname formatters]
//------------------------------------------------------------------------------------------------------
// PaperResetArchive_FormatTimestamp — wall-clock us → ISO 8601-flavored string.
// Format: "YYYY-MM-DD-HHMMSS" (filesystem-friendly; no colons; no T separator).
// Uses localtime_r for thread safety. out_size should be >= 20.
inline void PaperResetArchive_FormatTimestamp(uint64_t us, char* out, size_t out_size) {
    if (!out || out_size < 20) return;
    time_t t = (time_t)(us / 1000000ULL);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    strftime(out, out_size, "%Y-%m-%d-%H%M%S", &tm_buf);
}

// PaperResetArchive_FormatDirname — compose archive directory path.
// Output: "data/paper_resets/{start_iso}_to_{end_iso}.paper"
// out_size should be >= 128 (typical ISO string + path overhead ~80 chars).
inline void PaperResetArchive_FormatDirname(uint64_t start_us, uint64_t end_us,
                                             char* out, size_t out_size) {
    if (!out || out_size < 64) return;
    char start_iso[32], end_iso[32];
    PaperResetArchive_FormatTimestamp(start_us, start_iso, sizeof(start_iso));
    PaperResetArchive_FormatTimestamp(end_us,   end_iso,   sizeof(end_iso));
    std::snprintf(out, out_size, "data/paper_resets/%s_to_%s.paper", start_iso, end_iso);
}

//------------------------------------------------------------------------------------------------------
// [SECTION]_[directory creation — mkdir -p semantics in C]
//------------------------------------------------------------------------------------------------------
// Creates each path component if it doesn't exist. Returns 1 on success
// (or "exists already"), 0 on hard failure (permission denied, no parent
// dir, etc.).
//
// Path must be <= 512 chars. Recurses by null-terminating at each '/' and
// calling mkdir() incrementally.
inline int PaperResetArchive_CreateDirectories(const char* path) {
    // E.1.2.D D-a — body extracted VERBATIM to MemHeaders/DirCreate.hpp
    // (FoxDir_CreateParents) so the model-state savers share the ONE
    // mkdir -p walker; this name survives as the forwarder for its
    // existing callers. Log prefix moved "[archive]" -> "[mkdir]".
    return FoxDir_CreateParents(path);
}

//======================================================================
// [FUNCTION]_[Summary_WriteJson]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[session summary.json — session + global + per_node + per_strategy blocks; per_regime placeholder pending the trade-log aggregator]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int Summary_WriteJson(const char* output_path,
                              const EventLoopState<F>& state,
                              const ControllerConfig<F>& cfg,
                              int num_nodes,
                              uint64_t session_end_us) {
    if (!output_path) return 0;
    FILE* f = std::fopen(output_path, "w");
    if (!f) {
        std::fprintf(stderr, "[archive] Summary_WriteJson fopen(%s) failed: %s\n",
                     output_path, std::strerror(errno));
        return 0;
    }
    // ---- Session block ----
    uint64_t start_us = state.oms->paper_session_start_us;
    uint64_t end_us   = session_end_us;
    char start_iso[32], end_iso[32];
    PaperResetArchive_FormatTimestamp(start_us, start_iso, sizeof(start_iso));
    PaperResetArchive_FormatTimestamp(end_us,   end_iso,   sizeof(end_iso));
    uint64_t duration_us = (end_us > start_us) ? (end_us - start_us) : 0;

    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"session\":{");
    {
        bool first_field = true;
        json_emit_pair(f, "start_us",    start_us,    first_field);
        json_emit_pair(f, "end_us",      end_us,      first_field);
        // ISO strings emitted manually (json_emit_value doesn't handle const char*)
        std::fprintf(f, ",\"start_iso\":\"%s\"", start_iso);
        std::fprintf(f, ",\"end_iso\":\"%s\"",   end_iso);
        json_emit_pair(f, "duration_us", duration_us, first_field);
    }
    std::fprintf(f, "},\n");

    // ---- Global block ----
    std::fprintf(f, "  \"global\":{");
    {
        bool first_field = true;
        json_emit_pair<Money>(f, "balance_start",     cfg.starting_balance,           first_field);
        json_emit_pair<Money>(f, "balance_end",       state.oms->balance,             first_field);
        json_emit_pair<Money>(f, "realized_pnl",      state.oms->realized_pnl,        first_field);
        json_emit_pair<Money>(f, "total_fees",        state.oms->total_fees,          first_field);
        json_emit_pair<Money>(f, "total_maker_fees",  state.oms->total_maker_fees,    first_field);
        json_emit_pair<Money>(f, "total_taker_fees",  state.oms->total_taker_fees,    first_field);
        json_emit_pair(f, "maker_fills_count",         (uint64_t)state.oms->maker_fills_count, first_field);
        json_emit_pair(f, "taker_fills_count",         (uint64_t)state.oms->taker_fills_count, first_field);
        json_emit_pair(f, "total_entries",             (uint64_t)state.total_entries,  first_field);
        json_emit_pair(f, "total_exits",               (uint64_t)state.total_exits,    first_field);
        const uint64_t kst = BITMAP_IS_SET(state.oms->oms_state_flags,
                                            tt::MASK_OMS_STATE_KILL_SWITCH_TRIPPED) ? 1ULL : 0ULL;
        json_emit_pair(f, "kill_switch_tripped",       kst, first_field);
    }
    std::fprintf(f, "},\n");

    // ---- per_node array ----
    std::fprintf(f, "  \"per_node\":[");
    for (int c = 0; c < num_nodes; ++c) {
        if (c > 0) std::fprintf(f, ",");
        Summary_EmitPerCoreEntry(f, state.nodes[tt::NodeIdx{(int16_t)c}], c);
    }
    std::fprintf(f, "],\n");

    // ---- per_strategy array ----
    std::fprintf(f, "  \"per_strategy\":");
    Summary_EmitPerStrategy<F>(f, state.nodes, num_nodes);
    std::fprintf(f, ",\n");

    // ---- per_regime placeholder ----
    // v5.15.5.C.3 Phase 6 emits empty array; per-regime aggregation requires
    // trade-log parse + group-by-regime sum. Trade log gained regime + regime_name
    // columns in Phase 5.A. Phase 5.B (per-core CSV split) or focused
    // per-regime aggregator (parses trades.csv from archive dir post-rotation)
    // populates this section in a follow-up.
    std::fprintf(f, "  \"per_regime\":[]\n");

    std::fprintf(f, "}\n");
    std::fflush(f);
    std::fclose(f);
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Composes the full summary.json structure:
//
//   {
//     "session":     { start_us, end_us, start_iso, end_iso, duration_us },
//     "global":      { balance_start, balance_end, realized_pnl, total_fees,
//                       total_maker_fees, total_taker_fees, maker_fills_count,
//                       taker_fills_count, total_entries, total_exits,
//                       kill_switch_tripped },
//     "per_node":    [ { node_id, strategy_id, ..., last_confidence } × num_nodes ],
//     "per_strategy":[ { strategy_id, entries, exits, realized, fees, wins, losses,
//                         gross_wins, gross_losses, open_notional } × N strategies ],
//     "per_regime":  []   ← Phase 6 placeholder; populated by trade-log parser
//                          in Phase 5.B follow-up or focused per-regime aggregator.
//   }
//
// Returns 1 on success, 0 on file-open failure or null path.
//======================================================================
// [END_FUNCTION]_[Summary_WriteJson]
//======================================================================

}  // namespace tt

#endif  // PAPER_RESET_ARCHIVE_HPP
