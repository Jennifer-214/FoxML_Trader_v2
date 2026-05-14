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
        } else if constexpr (std::is_unsigned_v<T>) {
            // Decimal default; .F.4c may extend with hex base detection if needed.
            int64_t v = static_cast<int64_t>(strtoull(val, nullptr, 10));
            v = std::clamp(v, desc.payload.as_int.clamp_min, desc.payload.as_int.clamp_max);
            dst = static_cast<T>(v);
        } else { // signed integral
            int64_t v = atoi(val);
            v = std::clamp(v, desc.payload.as_int.clamp_min, desc.payload.as_int.clamp_max);
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
        } else if constexpr (std::is_unsigned_v<T>) {
            n = snprintf(buf, cap, "%llu", static_cast<unsigned long long>(src));
        } else { // signed integral
            n = snprintf(buf, cap, "%lld", static_cast<long long>(src));
        }

        if (pinned) {
            uselocale(prev);
            freelocale(pinned);
        }
        return n;
    }

    // Note: cfg_render_field<T> is implemented inline in GUI/SettingsPanel.hpp at T12,
    // since rendering depends on ImGui which isn't included by this header (used by
    // non-GUI code — the parser includes this header but doesn't link ImGui).

}  // namespace tt
