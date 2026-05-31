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
    //
    // v5.15.5.F.4d.1.B.3 Phase F — `wire_context` param added. KIND_DOUBLE_PCT scaling (operator-facing
    // %-input convention; "15.0" → stored 0.15) applies to CFG-FILE parsing ONLY, NOT wire parsing.
    // Wire format (stamp body) carries raw fractions (sister to cfg_emit_field which doesn't scale).
    // Caller from parse_stamp_cfg_to_derived passes wire_context=true; cfg-file parser passes false
    // (default). Closes round-trip asymmetry exposed by fee_rate_taker test at v5.9.2b — production
    // bug: stamp body parse would scale fee rates 100× smaller on load. Sister discipline: tt::cfg_emit_field
    // is wire-context by definition (no scaling); cfg_parse_field needs the flag for symmetric semantic.
    template <typename T>
    inline void cfg_parse_field(T& dst, const CfgFieldDescriptor& desc, const char* val,
                                 bool wire_context = false) {
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
            // FPN<F>: parse double, apply percent scaling if KIND_DOUBLE_PCT (cfg-file context only),
            // clamp to descriptor range, convert to FPN<T::F>.
            double v = parse_double_fast(val);
            if (!wire_context && desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) {
                v /= 100.0;  // operator types "15.0" → stored as 0.15 (cfg-FILE convention)
            }
            v = std::clamp(v, desc.payload.as_double.clamp_min, desc.payload.as_double.clamp_max);
            dst = FPN_FromDouble<T::F>(v);
        } else if constexpr (std::is_floating_point_v<T>) {
            double v = parse_double_fast(val);
            if (!wire_context && desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) v /= 100.0;
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

    //==================================================================================================
    // [EMIT: typed cfg field → HMAC wire-format kv text]
    //==================================================================================================
    // v5.15.5.F.4d.1.B.1 — fifth in-file sister of the tt:: cfg dispatch family.
    //
    // Writes "<name>=<value>\n" to buf. Used by derived-filter consumer macros
    // (STAMP_CFG_POPULATE_FROM_DERIVED at .B.1+) to emit canonical body bytes for HMAC chain.
    //
    // Distinct from cfg_save_field:
    //   cfg_save_field — operator-readable cfg file save format (%.4f / %.2f for PCT)
    //   cfg_emit_field — HMAC wire-format kv per-line (%.17g lossless; "name=value\n")
    //
    // LAYER 2 LOCALE PINNING per DESIGN_SPECS/wire-format-byte-preservation-discipline.md.
    // MUST honor LC_NUMERIC=C per-thread to prevent "0,55" vs "0.55" drift; I3 invariant
    // in tests/wire_format_invariants.hpp catches violations at test time.
    //
    // FORMAT (per kind):
    //   FPN<F>:        %.17g (lossless double round-trip; sister to MemHeaders/RunHistory.hpp:87-89)
    //   integers:      %lld / %llu (signed/unsigned via is_unsigned_v)
    //   bool:          ternary normalized to {0, 1} (H9 byte-equivalence; see cfg_save_field above)
    //   array<T, N>:   %s (string-style; KIND_FILE_PATH / KIND_STRING)
    //
    // Returns chars written (snprintf semantics; 0 on encoding error).
    template <typename T>
    inline size_t cfg_emit_field(const T& src, const CfgFieldDescriptor& desc, char* buf, size_t cap) {
        static_assert(is_FPN_v<T>
                   || std::is_floating_point_v<T>
                   || std::is_integral_v<T>
                   || std::is_array_v<T>,
                      "cfg field type not in recognized family — "
                      "extend tt::cfg_emit_field<T> with a new branch.");

        // Locale pinning (Layer 2 per wire-format-byte-preservation-discipline.md;
        // per-thread; safe in lock-free + GUI contexts). Sister to cfg_save_field
        // locale-pin precedent.
        locale_t pinned = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
        locale_t prev = (locale_t)0;
        if (pinned) prev = uselocale(pinned);

        int n = 0;
        if constexpr (is_FPN_v<T>) {
            // %.17g — lossless double round-trip.
            n = snprintf(buf, cap, "%s=%.17g\n", desc.cfg_field_name, FPN_ToDouble(src));
        } else if constexpr (std::is_floating_point_v<T>) {
            n = snprintf(buf, cap, "%s=%.17g\n", desc.cfg_field_name, static_cast<double>(src));
        } else if constexpr (std::is_array_v<T>) {
            n = snprintf(buf, cap, "%s=%s\n", desc.cfg_field_name, src);
        } else if constexpr (std::is_integral_v<T>) {
            // KIND_BOOL — ternary-normalized {0, 1} per H9 byte-equivalence (sister to cfg_save_field).
            if (desc.kind == CfgFieldDescriptor::KIND_BOOL) {
                n = snprintf(buf, cap, "%s=%d\n", desc.cfg_field_name, (src != 0) ? 1 : 0);
            } else if constexpr (std::is_unsigned_v<T>) {
                n = snprintf(buf, cap, "%s=%llu\n", desc.cfg_field_name,
                             static_cast<unsigned long long>(src));
            } else {
                n = snprintf(buf, cap, "%s=%lld\n", desc.cfg_field_name,
                             static_cast<long long>(src));
            }
        }

        if (pinned) {
            uselocale(prev);
            freelocale(pinned);
        }
        return (n < 0) ? 0u : static_cast<size_t>(n);
    }

    //==================================================================================================
    // [POPULATE_INF: typed cfg value → inf.<field> + inf.has_<field>]
    //==================================================================================================
    // v5.15.5.F.4d.1.B.1 — sixth in-file sister. Used by INFERENCE_CFG_POPULATE_FROM_DERIVED
    // consumer macro to populate stamp inference cfg inputs from runtime cfg, gate-aware.
    //
    // Distinct from cfg_assign_field (which sets to descriptor default).
    //   cfg_assign_field — descriptor.default → typed dst
    //   cfg_populate_inf_field — runtime cfg src → inf.field + inf.has_field (gate-conditional)
    //
    // Semantics (always-emit canonical Q3.G):
    //   gate true  → inf_dst = cfg_src; inf_has_dst = 1   (cohort field active)
    //   gate false → inf_dst = T{};     inf_has_dst = 0   (cohort gate-off; zero per Q3.G)
    //
    // Branchless for scalar T (cmov compiles `gate ? cfg_src : T{}` to conditional move
    // on x86-64). Array T uses conditional copy (no portable branchless string copy at
    // template level; fallback is acceptable since populate is slow-path/stamp-emit cadence).
    //
    // v5.15.5.F.4d.1.B.2 — extended SrcT/DstT/HasT independent templates per coding-time
    // discovery at Step 1 build verify. Cohort cfg fields are FPN<F> in master registry but
    // inf struct fields are double (per FOREACH_STAMP_BOUND_CFG legacy struct-gen at
    // ModelInference.hpp:1199 `type name;` where type=double for FPN cohort rows + has_ field
    // is uint8_t for StampInferenceCfgInputs OR int for ModelStampResult at :1643). Framework
    // walker needs to convert SrcT→DstT (FPN_ToDouble for FPN→double) + accept any integral
    // HasT. Sister precedent: cfg_emit_field already handles FPN→double via FPN_ToDouble inline.
    template <typename SrcT, typename DstT, typename HasT>
    inline void cfg_populate_inf_field(const SrcT& cfg_src, DstT& inf_dst, HasT& inf_has_dst, bool gate) {
        static_assert(is_FPN_v<SrcT>
                   || std::is_floating_point_v<SrcT>
                   || std::is_integral_v<SrcT>
                   || std::is_array_v<SrcT>,
                      "cfg field src type not in recognized family — "
                      "extend tt::cfg_populate_inf_field<SrcT,DstT> with a new branch.");
        static_assert(std::is_integral_v<HasT>,
                      "inf_has_dst must be integral (uint8_t for StampInferenceCfgInputs; "
                      "int for ModelStampResult).");

        if constexpr (std::is_array_v<SrcT>) {
            // String/array: branched copy + has bit. Slow-path cadence — acceptable.
            if (gate) {
                strncpy(inf_dst, cfg_src, std::extent_v<DstT> - 1);
                inf_dst[std::extent_v<DstT> - 1] = '\0';
                inf_has_dst = static_cast<HasT>(1);
            } else {
                inf_dst[0] = '\0';
                inf_has_dst = static_cast<HasT>(0);
            }
        } else if constexpr (is_FPN_v<SrcT> && std::is_floating_point_v<DstT>) {
            // FPN<F> → double via FPN_ToDouble (cohort field common case; sister to
            // cfg_emit_field FPN→double conversion at line 339).
            inf_dst = gate ? FPN_ToDouble(cfg_src) : DstT{};
            inf_has_dst = gate ? static_cast<HasT>(1) : static_cast<HasT>(0);
        } else if constexpr (is_FPN_v<SrcT> && is_FPN_v<DstT>) {
            // FPN<F> → FPN<F> direct (no conversion).
            inf_dst = gate ? cfg_src : DstT{};
            inf_has_dst = gate ? static_cast<HasT>(1) : static_cast<HasT>(0);
        } else {
            // Integral / floating: implicit conversion via static_cast (branchless cmov).
            inf_dst = gate ? static_cast<DstT>(cfg_src) : DstT{};
            inf_has_dst = gate ? static_cast<HasT>(1) : static_cast<HasT>(0);
        }
    }

    //==================================================================================================
    // [DRIFT_COMPARE: stamp.<field> vs cfg.<field> → bool]
    //==================================================================================================
    // v5.15.5.F.4d.1.B.1 — seventh in-file sister. Used by DRIFT_CHECK_FROM_DERIVED to detect
    // cfg drift between train-time stamp value and serve-time runtime cfg value.
    //
    // Distinct from cfg_diff_field (which compares vs descriptor default).
    //   cfg_diff_field — compare current vs default → bool (used for GUI "modified" badge)
    //   cfg_drift_compare — compare stamp vs runtime cfg → bool (used for drift detection)
    //
    // Returns true if values differ (drift detected). Per H4 — FPN<F> compared via integer
    // equality (NEVER float math on accounting types).
    //
    // v5.15.5.F.4d.1.B.2 — extended StampT/CfgT independent templates per coding-time
    // discovery at Step 1 build verify. Stamp's recorded value is at the inf struct's type
    // (double for FPN cohort rows per ModelInference.hpp:1643 `int has_##name; type name;`
    // where type=double); cfg runtime value is FPN<F>. Compare in DstT space (the stamp's
    // recorded type — what was actually wire-emitted at training time).
    template <typename StampT, typename CfgT>
    inline bool cfg_drift_compare(const StampT& stamp_val, const CfgT& cfg_val) {
        static_assert(is_FPN_v<StampT>
                   || std::is_floating_point_v<StampT>
                   || std::is_integral_v<StampT>
                   || std::is_array_v<StampT>,
                      "stamp field type not in recognized family — "
                      "extend tt::cfg_drift_compare<StampT,CfgT> with a new branch.");

        if constexpr (std::is_floating_point_v<StampT> && is_FPN_v<CfgT>) {
            // stamp is double (legacy struct-gen) vs cfg is FPN<F>. Compare in double space
            // (what was wire-emitted at training time per FPN_ToDouble in cfg_emit_field).
            return stamp_val != FPN_ToDouble(cfg_val);
        } else if constexpr (is_FPN_v<StampT> && is_FPN_v<CfgT>) {
            // FPN equality via byte comparison (H4 — never float math on accounting types).
            // memcmp on POD struct compares the underlying integer limb array bit-exactly;
            // FPN<F> doesn't define operator== directly so we go through the byte layer.
            // .E.0.1 F-076 guard: this raw memcmp is byte-deterministic ONLY if StampT has no
            // padding. FPN<F> is padding-free today (probed); this static_assert FAILS the build
            // if a future layout change adds padding (which would make this memcmp false-drift).
            // Sister to the ControllerConfig zero-init ctor (same H12 byte-equivalence class).
            static_assert(std::has_unique_object_representations_v<StampT>,
                          "F-076/H12: StampT (FPN) gained padding -> raw memcmp would false-drift; "
                          "make it padding-free or compare field-wise.");
            return memcmp(&stamp_val, &cfg_val, sizeof(StampT)) != 0;
        } else if constexpr (std::is_floating_point_v<StampT> && std::is_floating_point_v<CfgT>) {
            return stamp_val != cfg_val;
        } else if constexpr (std::is_array_v<StampT> && std::is_array_v<CfgT>) {
            return strncmp(stamp_val, cfg_val, std::extent_v<StampT>) != 0;
        } else if constexpr (std::is_integral_v<StampT> && std::is_integral_v<CfgT>) {
            return static_cast<int64_t>(stamp_val) != static_cast<int64_t>(cfg_val);
        }
        return false;
    }

    // v5.15.5.F.4d.1.B.3 Step 0.5a — first-drift attribution helper for framework drift walker.
    // Sister to cfg_drift_compare (which detects drift but loses field-attribution info).
    // Used by cfg_derived::drift_check_from_derived to write first-drift attribution into
    // caller-allocated buffer per failure-attribution-buffer-pattern.md § Framework-extension shape
    // (Stage 3 first canonical reference).
    //
    // Idiom: caller-allocated buffer + first-failure-wins (snprintf only if buf[0] == '\0').
    // Per Stage 2 DRAFT spec, the canonical format is "<field> drift: stamp=<stamp_val> cfg=<cfg_val>".
    // Type-family dispatch via if-constexpr sister to cfg_drift_compare; same StampT/CfgT asymmetry.
    //
    // Boot/load-time only (slow path); no hot-path use. snprintf cost is acceptable at this cadence.
    template <typename StampT, typename CfgT>
    inline int cfg_drift_format_reason(char* buf, size_t cap,
                                        const char* field_name,
                                        const StampT& stamp_val,
                                        const CfgT& cfg_val) {
        static_assert(is_FPN_v<StampT>
                   || std::is_floating_point_v<StampT>
                   || std::is_integral_v<StampT>
                   || std::is_array_v<StampT>,
                      "stamp field type not in recognized family — "
                      "extend tt::cfg_drift_format_reason<StampT,CfgT> with a new branch.");

        if (!buf || cap == 0) return 0;

        if constexpr (std::is_floating_point_v<StampT> && is_FPN_v<CfgT>) {
            // Stamp recorded as double (legacy struct-gen); cfg runtime is FPN<F>.
            // Format in double-space (matches wire-emit semantic per cfg_emit_field).
            return snprintf(buf, cap, "%s drift: stamp=%g cfg=%g",
                            field_name, (double)stamp_val, FPN_ToDouble(cfg_val));
        } else if constexpr (is_FPN_v<StampT> && is_FPN_v<CfgT>) {
            return snprintf(buf, cap, "%s drift: stamp=%g cfg=%g",
                            field_name, FPN_ToDouble(stamp_val), FPN_ToDouble(cfg_val));
        } else if constexpr (std::is_floating_point_v<StampT> && std::is_floating_point_v<CfgT>) {
            return snprintf(buf, cap, "%s drift: stamp=%g cfg=%g",
                            field_name, (double)stamp_val, (double)cfg_val);
        } else if constexpr (std::is_array_v<StampT> && std::is_array_v<CfgT>) {
            return snprintf(buf, cap, "%s drift: stamp=\"%s\" cfg=\"%s\"",
                            field_name, stamp_val, cfg_val);
        } else if constexpr (std::is_integral_v<StampT> && std::is_integral_v<CfgT>) {
            return snprintf(buf, cap, "%s drift: stamp=%lld cfg=%lld",
                            field_name, (long long)stamp_val, (long long)cfg_val);
        }
        return 0;
    }

    // Note: cfg_render_field<T> is implemented inline in GUI/SettingsPanel.hpp (Step 0.5
    // landed at .F.4c), since rendering depends on ImGui which isn't included by this header
    // (used by non-GUI code — the parser includes this header but doesn't link ImGui).
    //
    // The tt:: dispatch octet for cfg fields at .F.4d.1.B.3 (in-file 8 + GUI 1 = 9 codebase-wide):
    //   tt::cfg_parse_field<T>            (text → typed; here)
    //   tt::cfg_save_field<T>             (typed → text [operator-readable %.4f]; here)
    //   tt::cfg_assign_field<T>           (default → typed; here)
    //   tt::cfg_diff_field<T>             (typed vs default → bool; here)
    //   tt::cfg_emit_field<T>             (typed → HMAC wire-format kv [%.17g lossless]; here; .B.1+)
    //   tt::cfg_populate_inf_field<T>     (cfg → inf.field + inf.has_field; gate-aware; here; .B.1+)
    //   tt::cfg_drift_compare<T>          (stamp vs cfg → bool; here; .B.1+)
    //   tt::cfg_drift_format_reason<T>    (stamp + cfg → attribution buf; here; .B.3+; first canonical of failure-attribution-buffer-pattern.md)
    //   tt::cfg_render_field<T>           (typed → ImGui widget; GUI/SettingsPanel.hpp)

}  // namespace tt
