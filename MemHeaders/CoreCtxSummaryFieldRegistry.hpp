// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [CORE-CONTEXT SUMMARY FIELD REGISTRY — v5.15.5.C.3 Phase 4]
//======================================================================================================
// FOREACH_CORE_CTX_SUMMARY_FIELD(X) — drives summary.json per_core array
// emission for the paper-reset archive flow (Phase 6) + general operator-
// facing per-core stats reporting.
//
// Tuple: X(field_name, type, json_key)
//   field_name — bare identifier on CoreContext<F>; referenced as ctx.field_name
//   type       — C++ type; dispatched via json_emit_value<T> for per-type
//                 JSON formatting (FPN_Binary<F> → "%.6f" via FPN_ToDouble, uint8/16/32/64 → "%llu",
//                 signed → "%lld", float/double → "%.6f")
//   json_key   — operator-facing JSON key string literal
//
// Adding a new per-core stat = ONE row. Future operator-facing surfaces
// (GUI Account panel, daily PnL report, audit trail) walk the same registry
// for byte-format consistency.
//
// HYBRID PATTERN PRECEDENT (this is the first codebase application of
// type-trait-dispatched JSON emission via templated helpers):
//   - CalibLogColRegistry.hpp uses printf-fmt-string dispatch (CSV row).
//   - This registry uses type-trait dispatch (JSON value; per CLAUDE.md
//     item 23 templated helpers; cleaner because JSON has fewer types).
//
// AGGREGATION (per_strategy section of summary.json):
//   Summary_EmitPerStrategy() hand-codes the aggregation for the 4 summable
//   stats (entries, exits, realized, fees, wins, losses, gross_*). Future
//   refactor: introduce FOREACH_CORE_CTX_SUMMABLE_FIELD sub-registry +
//   aggregation kind column if more aggregation cohorts emerge.
//
// PER_REGIME AGGREGATION (deferred to Phase 5):
//   Requires per-trade regime data (Phase 5 ShardedTradeLog refactor adds
//   regime column to trade log CSV). Phase 4 emits summary.json with
//   per_core + per_strategy sections only; per_regime added at Phase 6
//   archive flow integration.
//
// Cross-references:
//   DESIGN_SPECS/registry-tuple-as-single-source-of-truth.md (3-col tuple shape)
//   DESIGN_SPECS/calibration-log-column-registry.md (sister registry; CSV vs JSON dispatch)
//   DESIGN_SPECS/autopopulate-pattern-for-production-caller-class.md (multi-target dispatch)
//   CLAUDE.md item 13 (X-macro registry for multi-site additions)
//   CLAUDE.md item 23 (templated helpers; type-trait dispatch over branches)
//   v5.14.10.D FOREACH_CALIB_LOG_COL (precedent — CSV row emit registry)
//   v5.15.5.B.7 FOREACH_CORE_CTX_INIT_FIELD (sister registry — per-core init walk)
//======================================================================================================
#ifndef CORE_CTX_SUMMARY_FIELD_REGISTRY_HPP
#define CORE_CTX_SUMMARY_FIELD_REGISTRY_HPP

#include <cstdint>
#include <cstdio>
#include <type_traits>
#include "../FixedPoint/FixedPointN.hpp"

// Forward declaration — CoreContext<F> is defined in
// CoreFrameworks/ControllerEventLoop.hpp (line ~253). Callers of the per-core
// emit function must include that header first.
namespace tt {
    template <unsigned F> struct CoreContext;
}  // namespace tt

namespace tt {

//======================================================================================================
// [TYPE TRAIT — is_fpn for FPN_Binary<F> detection in templated dispatch]
//======================================================================================================
// FPN_Binary<F> is a template; std::is_same_v<T, FPN_Binary<F>> requires a specific F. To
// detect any FPN_Binary<F> regardless of width, use this custom type trait.
//======================================================================================================
template <typename T> struct is_fpn : std::false_type {};
template <unsigned F> struct is_fpn<FPN_Binary<F>> : std::true_type {};
template <typename T> inline constexpr bool is_fpn_v = is_fpn<T>::value;

//======================================================================================================
// [JSON EMIT HELPERS — type-trait dispatch via templated functions]
//======================================================================================================
// Per CLAUDE.md item 23. Each instantiation discards unused branches via
// if-constexpr; the compiler emits only the per-type code path that matches
// T. Single source of truth for JSON value formatting; adding a new
// supported type = 1 if-constexpr branch.
//
// Format choices:
//   FPN_Binary<F>           → "%.6f" via FPN_ToDouble (6 decimal places; sufficient for
//                       BTC USD prices, fee rates, and P&L)
//   float / double   → "%.6f" (same precision)
//   unsigned integer → "%llu" (works for uint8/16/32/64 via cast)
//   signed integer   → "%lld" (works for int8/16/32/64 via cast)
//   bool / int (logical) → "%d" (0 or 1)
//
// All formats are JSON-compatible (no trailing newlines, no escape sequences
// needed for numeric output). String fields not currently supported — add a
// branch with manual JSON escape if/when string keys emerge.
//======================================================================================================
template <typename T>
inline void json_emit_value(std::FILE* f, const T& value) {
    if (!f) return;
    if constexpr (is_fp_decimal_v<T>) {
        std::fprintf(f, "%.8f", Money_ToDouble(value));  // decimal money — exact 8dp display
    } else if constexpr (is_fpn_v<T>) {
        std::fprintf(f, "%.6f", FPN_ToDouble(value));
    } else if constexpr (std::is_floating_point_v<T>) {
        std::fprintf(f, "%.6f", static_cast<double>(value));
    } else if constexpr (std::is_unsigned_v<T>) {
        std::fprintf(f, "%llu", static_cast<unsigned long long>(value));
    } else if constexpr (std::is_signed_v<T> && std::is_integral_v<T>) {
        std::fprintf(f, "%lld", static_cast<long long>(value));
    } else {
        static_assert(is_fpn_v<T> || is_fp_decimal_v<T> || std::is_arithmetic_v<T>,
                      "json_emit_value: unsupported type — add a constexpr branch above");
    }
}

// Emit "key":value pair into a JSON object body. Caller manages opening { /
// closing } braces + commas between pairs via the first_field bool.
// Returns void; updates first_field by reference (false after first call).
template <typename T>
inline void json_emit_pair(std::FILE* f, const char* key, const T& value, bool& first_field) {
    if (!f || !key) return;
    std::fprintf(f, first_field ? "\"%s\":" : ",\"%s\":", key);
    first_field = false;
    json_emit_value(f, value);
}

}  // namespace tt

//======================================================================================================
// [CANONICAL REGISTRY — per-core summary fields]
//======================================================================================================
// Fields ordered by semantic grouping (identity → counters → P&L → kill switch).
// Order is operator-facing (appears in summary.json + future per-core reports).
// Append new entries at the end to preserve operator-side parser stability if/when
// downstream tooling indexes by position (currently keyed by json_key, but
// keeping append-only discipline simplifies migrations).
//======================================================================================================
#define FOREACH_CORE_CTX_SUMMARY_FIELD(X)                                                       \
    /* Identity + strategy */                                                                   \
    X(strategy_id,           uint8_t,  "strategy_id")                                           \
    X(resolved_strategy_id,  uint8_t,  "resolved_strategy_id")                                  \
    X(halt_reason,           uint8_t,  "halt_reason")                                           \
    X(strategy_halt_reason,  uint8_t,  "strategy_halt_reason")                                  \
    /* Capital allocation */                                                                    \
    X(allocated_balance,     Money,           "allocated_balance")                                     \
    /* Event counters */                                                                        \
    X(entries_processed,     uint64_t, "entries")                                               \
    X(exits_processed,       uint64_t, "exits")                                                 \
    X(sl_cooldown_remaining, uint32_t, "sl_cooldown_remaining")                                 \
    X(idle_cycles,           uint32_t, "idle_cycles")                                           \
    /* P&L (net) */                                                                             \
    X(core_realized,         Money,           "realized")                                              \
    X(core_fees,             Money,           "fees")                                                  \
    X(core_wins,             uint32_t, "wins")                                                  \
    X(core_losses,           uint32_t, "losses")                                                \
    /* P&L (gross — per-side accumulators for avg_win, avg_loss, profit_factor, expectancy) */  \
    X(core_gross_wins,       Money,           "gross_wins")                                            \
    X(core_gross_losses,     Money,           "gross_losses")                                          \
    X(core_open_notional,    Money,           "open_notional")                                         \
    /* Kill switch / drawdown */                                                                \
    X(core_peak_balance,     Money,           "peak_balance")                                          \
    X(core_dd_pct,           Money,           "dd_pct")                                                \
    X(core_ks_trips_total,   uint32_t, "ks_trips_total")                                        \
    /* ML observability */                                                                      \
    X(last_confidence,       double,   "last_confidence")

//======================================================================================================
// [COMPILE-TIME COUNT SENTINEL]
//======================================================================================================
#define _CCSUM_COUNT_ONE(name, type, key) +1
constexpr int FOREACH_CORE_CTX_SUMMARY_FIELD_COUNT =
    0 FOREACH_CORE_CTX_SUMMARY_FIELD(_CCSUM_COUNT_ONE);
#undef _CCSUM_COUNT_ONE

static_assert(FOREACH_CORE_CTX_SUMMARY_FIELD_COUNT >= 18,
              "FOREACH_CORE_CTX_SUMMARY_FIELD must keep the v5.15.5.C.3 Phase 4 "
              "minimum set (~20 per-core trading-relevant stats). Removing "
              "entries requires explicit justification + operator tooling audit.");

namespace tt {

//======================================================================================================
// [Summary_EmitPerCoreEntry — emit one JSON object for a single CoreContext]
//======================================================================================================
// Emits a complete JSON object (with opening { / closing }) for one core's
// summary fields. Caller manages the surrounding array brackets [...] +
// inter-object commas.
//
// Usage from caller (per_core array writer):
//   std::fprintf(f, "[");
//   for (int c = 0; c < num_cores; ++c) {
//       if (c > 0) std::fprintf(f, ",");
//       Summary_EmitPerCoreEntry(f, state.cores[c], c);
//   }
//   std::fprintf(f, "]");
//
// JSON shape per core:
//   {"core_id":0,"strategy_id":3,"resolved_strategy_id":3,"halt_reason":0,
//    "strategy_halt_reason":0,"allocated_balance":1000.000000,"entries":42,...}
//======================================================================================================
template <unsigned F>
inline void Summary_EmitPerCoreEntry(std::FILE* f, const CoreContext<F>& ctx, int core_id) {
    if (!f) return;
    std::fprintf(f, "{");
    bool first_field = true;
    json_emit_pair(f, "core_id", static_cast<uint64_t>(core_id), first_field);
#define _CCSUM_JSON_EMIT_ONE(NAME, TYPE, KEY) \
    json_emit_pair<TYPE>(f, KEY, ctx.NAME, first_field);
    FOREACH_CORE_CTX_SUMMARY_FIELD(_CCSUM_JSON_EMIT_ONE)
#undef _CCSUM_JSON_EMIT_ONE
    std::fprintf(f, "}");
}

//======================================================================================================
// [Summary_EmitPerStrategy — aggregate cores by strategy_id, emit per_strategy array]
//======================================================================================================
// Groups cores by strategy_id; for each unique strategy present, sums the
// SUMMABLE fields (entries, exits, realized, fees, wins, losses, gross_*,
// open_notional) across cores. Emits JSON array of per-strategy objects.
//
// Iterates 256 possible strategy_id values (full uint8_t range); skips
// strategies with no cores (present == 0). Today's strategies fit within
// NUM_STRATEGIES (6: MR, MOM, DIP, ML, EMA, AUTO); the 256 capacity gives
// headroom for future expansion without API change.
//
// STRATEGY_NONE (0xFF) is skipped — "no strategy" cores have no trades to aggregate.
//
// Hand-coded aggregation (not registry-driven) because:
//   1. Aggregation arithmetic differs per type (FPN_Add vs += vs no-op).
//   2. The summable subset is stable today (4-7 fields); a 4th tuple column
//      for aggregation kind would add complexity for marginal benefit.
//   3. Future refactor possible: FOREACH_CORE_CTX_SUMMABLE_FIELD sub-registry
//      with FPN_AGG / INT_AGG / MAX_AGG dispatch when ≥3 aggregation
//      patterns emerge.
//======================================================================================================
template <unsigned F>
inline void Summary_EmitPerStrategy(std::FILE* f, const CoreContext<F>* cores, int num_cores) {
    if (!f || !cores) return;
    constexpr int MAX_STRAT = 256;  // full uint8_t range
    struct StratAgg {
        int      present;
        uint64_t entries;
        uint64_t exits;
        Money    realized;
        Money    fees;
        uint32_t wins;
        uint32_t losses;
        Money    gross_wins;
        Money    gross_losses;
        Money    open_notional;
    };
    StratAgg agg[MAX_STRAT] = {};
    for (int c = 0; c < num_cores; ++c) {
        const uint8_t sid = cores[c].strategy_id;
        if (sid == 0xFF) continue;  // STRATEGY_NONE — skip
        StratAgg& a = agg[sid];
        a.present = 1;
        a.entries      += cores[c].entries_processed;
        a.exits        += cores[c].exits_processed;
        a.realized      = Money_Add(a.realized,      cores[c].core_realized);
        a.fees          = Money_Add(a.fees,          cores[c].core_fees);
        a.wins         += cores[c].core_wins;
        a.losses       += cores[c].core_losses;
        a.gross_wins    = Money_Add(a.gross_wins,    cores[c].core_gross_wins);
        a.gross_losses  = Money_Add(a.gross_losses,  cores[c].core_gross_losses);
        a.open_notional = Money_Add(a.open_notional, cores[c].core_open_notional);
    }
    std::fprintf(f, "[");
    bool first_strat = true;
    for (int sid = 0; sid < MAX_STRAT; ++sid) {
        if (!agg[sid].present) continue;
        if (!first_strat) std::fprintf(f, ",");
        first_strat = false;
        std::fprintf(f, "{");
        bool first_field = true;
        json_emit_pair(f, "strategy_id",   static_cast<uint64_t>(sid), first_field);
        json_emit_pair(f, "entries",       agg[sid].entries,           first_field);
        json_emit_pair(f, "exits",         agg[sid].exits,             first_field);
        json_emit_pair<Money>(f, "realized",      agg[sid].realized,      first_field);
        json_emit_pair<Money>(f, "fees",          agg[sid].fees,          first_field);
        json_emit_pair(f, "wins",          static_cast<uint64_t>(agg[sid].wins),   first_field);
        json_emit_pair(f, "losses",        static_cast<uint64_t>(agg[sid].losses), first_field);
        json_emit_pair<Money>(f, "gross_wins",    agg[sid].gross_wins,    first_field);
        json_emit_pair<Money>(f, "gross_losses",  agg[sid].gross_losses,  first_field);
        json_emit_pair<Money>(f, "open_notional", agg[sid].open_notional, first_field);
        std::fprintf(f, "}");
    }
    std::fprintf(f, "]");
}

}  // namespace tt

#endif  // CORE_CTX_SUMMARY_FIELD_REGISTRY_HPP
