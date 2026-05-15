// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
//======================================================================================================
// [TT:: TYPE-TRAIT DISPATCH FOR CFG FIELD I/O]
//======================================================================================================
// v5.15.5.F.4b — 3-barrier structural fix for Class 23 (DOCS/RECURRING_BUG_PATTERNS.md).
// Companion to CoreFrameworks/CfgFieldRegistry.hpp.
//
// Pattern: DESIGN_SPECS/type-trait-dispatch-via-tt-namespace.md (canonical antidote).
// Mirror: ML_Headers/StampBoundModelConstRegistry.hpp:101-124 tt::stamp_parse_field<T>
//   (canonical reference implementation; this file extends the pattern to cfg I/O).
//
// THE 3 BARRIERS:
//   Barrier 1 — API surface: ONLY templated overloads with destination-by-reference exist.
//               NO `void*+offset` variant. NO `*reinterpret_cast<T*>((char*)dst + offset) = v;`.
//   Barrier 2 — X-macro extractor chokepoint: registry consumer macros call
//               `tt::cfg_*_field(cfg.name, ...)`; T deduced from cfg field reference.
//   Barrier 3 — Compile-time type-family static_assert: each tt:: function asserts
//               T is in the recognized family (is_FPN_v / std::is_floating_point_v /
//               std::is_array_v / std::is_integral_v). Adding a cfg field of an
//               unrecognized type FAILS THE BUILD with actionable error message.
//
// CLAUDE.md item 23 (type-trait dispatch via templated helpers) — public statement.
// CLAUDE.md item 19 (structural fix preferred) — meta-pattern motivation.
//======================================================================================================
#pragma once
#include "CfgFieldRegistry.hpp"
#include "../FixedPoint/FixedPointN.hpp"   // FPN<F>, is_FPN_v, FPN_FromDouble, FPN_ToDouble
#include <cstdlib>     // strtoull, atoi, atof
#include <cstring>     // strncpy
#include <cstdio>      // snprintf
#include <type_traits> // std::is_floating_point_v, std::is_integral_v, std::is_array_v, std::is_unsigned_v
#include <algorithm>   // std::clamp
#include <locale.h>    // newlocale, uselocale, freelocale (Layer 2 locale pinning)
#include <strings.h>   // v5.15.5.F.4c — strcasecmp for KIND_INT_ENUM string-token parsing

namespace tt {

    // Forward-declared parse_double_fast (definition lives in
    // ML_Headers/ModelInference.hpp's namespace tt; same precedent as
    // StampBoundModelConstRegistry.hpp:97-99 forward-decl-to-avoid-include-cycle).
    inline double parse_double_fast(const char* s);

    //==================================================================================================
    // [PARSE: text → typed cfg field]
    //==================================================================================================
    // Caller passes destination BY REFERENCE; T deduced. NEVER takes void*+offset.
    template <typename T>
    inline void cfg_parse_field(T& dst, const CfgFieldDescriptor& desc, const char* val) {
        // BARRIER 3: compile-time type-family guard.
        // Adding a cfg field of an unrecognized type FAILS THE BUILD here, forcing
        // a deliberate decision (extend tt::cfg_parse_field<T> with a new branch)
        // rather than silent truncation via type-erased reinterpret_cast.
        static_assert(is_FPN_v<T>
                   || std::is_floating_point_v<T>
                   || std::is_integral_v<T>
                   || std::is_array_v<T>,
                      "cfg field type not in recognized family — "
                      "extend tt::cfg_parse_field<T> with a new branch before "
                      "using this T as a cfg field. See "
                      "DESIGN_SPECS/type-trait-dispatch-via-tt-namespace.md.");

        if constexpr (is_FPN_v<T>) {
            // FPN<F>: parse double, apply percent scaling if KIND_DOUBLE_PCT,
            // clamp to descriptor range, convert to FPN<T::F>.
            double v = parse_double_fast(val);
            if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) {
                v /= 100.0;  // operator types "15.0" → stored as 0.15
            }
            v = std::clamp(v, desc.payload.as_double.clamp_min, desc.payload.as_double.clamp_max);
            dst = FPN_FromDouble<T::F>(v);
        } else if constexpr (std::is_floating_point_v<T>) {
            double v = parse_double_fast(val);
            if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) v /= 100.0;
            v = std::clamp(v, desc.payload.as_double.clamp_min, desc.payload.as_double.clamp_max);
            dst = static_cast<T>(v);
        } else if constexpr (std::is_array_v<T>) {
            strncpy(dst, val, std::extent_v<T> - 1);
            dst[std::extent_v<T> - 1] = '\0';
        } else if constexpr (std::is_integral_v<T>) {
            // v5.15.5.F.4c — INT_ENUM-aware integer branch. Sub-dispatch on
            // desc.kind handles KIND_INT_ENUM (label[] reverse-lookup + clamp to
            // [0, count) → default_val on OOR) and KIND_INT/KIND_BOOL (clamp to
            // as_int.clamp_min/max). Storage width comes from T via is_unsigned_v
            // + sizeof at compile time per H13/H14 (Kind NEVER drives storage).
            int64_t v;
            if (desc.kind == CfgFieldDescriptor::KIND_INT_ENUM) {
                // Operator-friendly string-token parsing — labels[] case-insensitive
                // lookup matches legacy BanditAlgorithm/BarrierBlendMode/
                // DegradationCurve_FromString strcasecmp convention. Numeric
                // fallback preserves `bandit_algorithm=2` UX. Out-of-range →
                // default_val (audit HIGH-1: reads as_int_enum union member).
                v = -1;
                for (uint8_t i = 0; i < desc.payload.as_int_enum.count; i++) {
                    if (strcasecmp(val, desc.payload.as_int_enum.labels[i]) == 0) {
                        v = i;
                        break;
                    }
                }
                if (v < 0) {
                    v = atoi(val);
                }
                if (v < 0 || v >= desc.payload.as_int_enum.count) {
                    // .F.4c — preserve legacy operator-UX warning emission per BanditAlgorithm/
                    // BarrierBlendMode/DegradationCurve_FromString convention. Walks labels[]
                    // to enumerate valid tokens. Warns unconditionally (no metadata bit gate)
                    // because legacy behavior was unconditional warn-then-default.
                    fprintf(stderr, "[cfg] WARN: %s='%s' invalid; expected one of ",
                            desc.cfg_field_name, val);
                    for (uint8_t i = 0; i < desc.payload.as_int_enum.count; i++) {
                        fprintf(stderr, "%s%s", i > 0 ? "/" : "",
                                desc.payload.as_int_enum.labels[i]);
                    }
                    fprintf(stderr, " or 0-%u. Using default=%d.\n",
                            desc.payload.as_int_enum.count - 1u,
                            desc.payload.as_int_enum.default_val);
                    v = desc.payload.as_int_enum.default_val;
                }
            } else if (desc.kind == CfgFieldDescriptor::KIND_BOOL) {
                // KIND_BOOL — truthy-int normalization (any non-zero → 1; zero → 0).
                // Payload union holds as_bool (NOT as_int); reading as_int would
                // be UB. Bool range is fixed [0, 1] — no descriptor clamp needed.
                const int64_t parsed = atoi(val);
                v = (parsed != 0) ? 1 : 0;
                // WARN_ON_CLAMP: emit when operator wrote something other than 0/1
                // (e.g., `record_ticks=42`); normalization is silent unless tagged.
                if (parsed != 0 && parsed != 1 &&
                    (desc.metadata_flags & CfgFieldDescriptor::WARN_ON_CLAMP)) {
                    fprintf(stderr, "[cfg] WARN: %s='%s' (parsed=%lld) not in {0,1}; "
                            "normalizing to 1.\n",
                            desc.cfg_field_name, val, (long long)parsed);
                }
            } else {
                // KIND_INT — decimal parse + clamp to descriptor range.
                if constexpr (std::is_unsigned_v<T>) {
                    v = static_cast<int64_t>(strtoull(val, nullptr, 10));
                } else {
                    v = atoi(val);
                }
                const int64_t v_before = v;
                v = std::clamp(v, desc.payload.as_int.clamp_min, desc.payload.as_int.clamp_max);
                // .F.4c — WARN_ON_CLAMP metadata bit: emit operator-clarity warning when
                // parse clamps the value. Per-bit gated (not all KIND_INT rows want this;
                // some clamps are defensive boundaries operator never trips).
                if (v != v_before && (desc.metadata_flags & CfgFieldDescriptor::WARN_ON_CLAMP)) {
                    fprintf(stderr, "[cfg] WARN: %s='%s' out of range [%lld, %lld]; clamping to %lld.\n",
                            desc.cfg_field_name, val,
                            (long long)desc.payload.as_int.clamp_min,
                            (long long)desc.payload.as_int.clamp_max,
                            (long long)v);
                }
            }
            dst = static_cast<T>(v);
        }
    }

    //==================================================================================================
    // [SAVE: typed cfg field → text buffer (operator-friendly format)]
    //==================================================================================================
    // Returns chars written (snprintf semantics). Caller routes the buffer
    // through GUI/SettingsPanel.hpp:cfg_write_field(path, key, buf) for
    // comment-preserving save UX.
    //
    // LAYER 2 LOCALE PINNING per DESIGN_SPECS/wire-format-byte-preservation-discipline.md.
    // Status-quo pre-.F.4b SettingsPanel save used raw snprintf which honored
    // LC_NUMERIC; a stamp emitted under de_DE produces "0,55" not "0.55" → HMAC chain
    // breaks. tt:: dispatch pins per-thread to LC_NUMERIC=C; restores before return.
    // Precedent: MemHeaders/RunHistory.hpp:87-89.
    template <typename T>
    inline int cfg_save_field(const T& src, const CfgFieldDescriptor& desc, char* buf, size_t cap) {
        static_assert(is_FPN_v<T>
                   || std::is_floating_point_v<T>
                   || std::is_integral_v<T>
                   || std::is_array_v<T>,
                      "cfg field type not in recognized family — "
                      "extend tt::cfg_save_field<T> with a new branch.");

        // Locale pinning (per-thread; safe in lock-free + GUI contexts).
        locale_t pinned = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
        locale_t prev = (locale_t)0;
        if (pinned) prev = uselocale(pinned);

        int n = 0;
        if constexpr (is_FPN_v<T>) {
            double v = FPN_ToDouble(src);
            if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) v *= 100.0;
            const char* fmt = (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) ? "%.2f" : "%.4f";
            n = snprintf(buf, cap, fmt, v);
        } else if constexpr (std::is_floating_point_v<T>) {
            double v = static_cast<double>(src);
            if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) v *= 100.0;
            const char* fmt = (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) ? "%.2f" : "%.4f";
            n = snprintf(buf, cap, fmt, v);
        } else if constexpr (std::is_array_v<T>) {
            n = snprintf(buf, cap, "%s", src);
        } else if constexpr (std::is_integral_v<T>) {
            // v5.15.5.F.4c — KIND_BOOL sub-dispatch: emit "0" or "1" (normalized) so the
            // saved value round-trips through cfg_parse_field cleanly. Non-bool integral
            // types emit decimal as before.
            if (desc.kind == CfgFieldDescriptor::KIND_BOOL) {
                n = snprintf(buf, cap, "%d", (src != 0) ? 1 : 0);
            } else if constexpr (std::is_unsigned_v<T>) {
                n = snprintf(buf, cap, "%llu", static_cast<unsigned long long>(src));
            } else {
                n = snprintf(buf, cap, "%lld", static_cast<long long>(src));
            }
        }

        if (pinned) {
            uselocale(prev);
            freelocale(pinned);
        }
        return n;
    }

    //==================================================================================================
    // [ASSIGN: descriptor default → typed cfg field]
    //==================================================================================================
    // v5.15.5.F.4c — third sister of cfg_parse_field / cfg_save_field. Sets cfg.name
    // to its declared default value from the descriptor payload. Used by reset-to-
    // defaults GUI button + boot default-fill consumers. Same 3-barrier discipline.
    template <typename T>
    inline void cfg_assign_field(T& dst, const CfgFieldDescriptor& desc) {
        static_assert(is_FPN_v<T>
                   || std::is_floating_point_v<T>
                   || std::is_integral_v<T>
                   || std::is_array_v<T>,
                      "cfg field type not in recognized family — "
                      "extend tt::cfg_assign_field<T> with a new branch.");

        if constexpr (is_FPN_v<T>) {
            // Default is stored as fraction (NOT percent); no PCT scaling needed.
            dst = FPN_FromDouble<T::F>(desc.payload.as_double.default_val);
        } else if constexpr (std::is_floating_point_v<T>) {
            dst = static_cast<T>(desc.payload.as_double.default_val);
        } else if constexpr (std::is_array_v<T>) {
            // KIND_STRING / KIND_FILE_PATH — default is const char*; copy into array.
            if (desc.payload.as_string.default_val) {
                strncpy(dst, desc.payload.as_string.default_val, std::extent_v<T> - 1);
                dst[std::extent_v<T> - 1] = '\0';
            } else {
                dst[0] = '\0';
            }
        } else if constexpr (std::is_integral_v<T>) {
            // Sub-dispatch on Kind for INT_ENUM (as_int_enum) vs INT/BOOL (as_int / as_bool).
            int64_t v;
            if (desc.kind == CfgFieldDescriptor::KIND_INT_ENUM) {
                v = desc.payload.as_int_enum.default_val;
            } else if (desc.kind == CfgFieldDescriptor::KIND_BOOL) {
                v = desc.payload.as_bool.default_val;
            } else {
                v = desc.payload.as_int.default_val;
            }
            dst = static_cast<T>(v);
        }
    }

    //==================================================================================================
    // [DIFF: typed cfg field vs descriptor default]
    //==================================================================================================
    // v5.15.5.F.4c — fourth sister. Returns true if current value differs from declared
    // default. Used by GUI "modified" badge + CLI --list-cfg --changed-only consumer.
    template <typename T>
    inline bool cfg_diff_field(const T& current, const CfgFieldDescriptor& desc) {
        static_assert(is_FPN_v<T>
                   || std::is_floating_point_v<T>
                   || std::is_integral_v<T>
                   || std::is_array_v<T>,
                      "cfg field type not in recognized family — "
                      "extend tt::cfg_diff_field<T> with a new branch.");

        if constexpr (is_FPN_v<T>) {
            // FPN equality via integer comparison (after FromDouble conversion).
            const T default_fpn = FPN_FromDouble<T::F>(desc.payload.as_double.default_val);
            return !(current == default_fpn);
        } else if constexpr (std::is_floating_point_v<T>) {
            return current != static_cast<T>(desc.payload.as_double.default_val);
        } else if constexpr (std::is_array_v<T>) {
            if (!desc.payload.as_string.default_val) {
                return current[0] != '\0';
            }
            return strncmp(current, desc.payload.as_string.default_val, std::extent_v<T>) != 0;
        } else if constexpr (std::is_integral_v<T>) {
            int64_t default_v;
            if (desc.kind == CfgFieldDescriptor::KIND_INT_ENUM) {
                default_v = desc.payload.as_int_enum.default_val;
            } else if (desc.kind == CfgFieldDescriptor::KIND_BOOL) {
                default_v = desc.payload.as_bool.default_val;
            } else {
                default_v = desc.payload.as_int.default_val;
            }
            return static_cast<int64_t>(current) != default_v;
        }
        return false;
    }

    // Note: cfg_render_field<T> is implemented inline in GUI/SettingsPanel.hpp (Step 0.5
    // landed at .F.4c), since rendering depends on ImGui which isn't included by this header
    // (used by non-GUI code — the parser includes this header but doesn't link ImGui).
    //
    // The tt:: dispatch quartet for cfg fields at .F.4c:
    //   tt::cfg_parse_field<T>  (text → typed; here)
    //   tt::cfg_save_field<T>   (typed → text; here)
    //   tt::cfg_assign_field<T> (default → typed; here)
    //   tt::cfg_diff_field<T>   (typed vs default → bool; here)
    //   tt::cfg_render_field<T> (typed → ImGui widget; GUI/SettingsPanel.hpp)

}  // namespace tt
