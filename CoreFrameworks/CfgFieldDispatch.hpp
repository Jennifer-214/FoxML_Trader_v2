// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
//======================================================================================================
// [FILE]_[CoreFrameworks/CfgFieldDispatch.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CFG_FLOW] [DETERMINISM] [FRAMEWORK_DISCIPLINE]]
// [REFERENCE]_[INVARIANT]_[[H13] [H9] [H4]]
// [REFERENCE]_[DESIGN_SPEC]_[type-trait-dispatch-via-tt-namespace]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the tt:: cfg dispatch octet — 3-barrier Class-23 antidote (typed-by-reference, X-macro chokepoint, type-family static_assert); locale-pinned save/emit]
// [CONTAINS]
//   - [FUNCTION]_[cfg_parse_field]
//   - [FUNCTION]_[cfg_save_field]
//   - [FUNCTION]_[cfg_assign_field]
//   - [FUNCTION]_[cfg_diff_field]
//   - [FUNCTION]_[cfg_emit_field]
//   - [FUNCTION]_[cfg_populate_inf_field]
//   - [FUNCTION]_[cfg_drift_compare]
//   - [FUNCTION]_[cfg_drift_format_reason]
//======================================================================================================
// v5.15.5.F.4b — 3-barrier structural fix for Class 23 (DOCS/RECURRING_BUG_PATTERNS.md).
// Companion to CoreFrameworks/CfgFieldRegistry.hpp.
//
// Pattern: DESIGN_SPECS/type-trait-dispatch-via-tt-namespace.md (canonical antidote).
// Mirror: tt::stamp_parse_field<T> in ML_Headers/StampBoundModelConstRegistry.hpp
//   (canonical reference implementation; this file extends the pattern to cfg I/O).
//
// THE 3 BARRIERS:
//   Barrier 1 — API surface: ONLY templated overloads with destination-by-reference exist.
//               NO `void*+offset` variant. NO `*reinterpret_cast<T*>((char*)dst + offset) = v;`.
//   Barrier 2 — X-macro extractor chokepoint: registry consumer macros call
//               `tt::cfg_*_field(cfg.name, ...)`; T deduced from cfg field reference.
//   Barrier 3 — Compile-time type-family static_assert: each tt:: function asserts
//               T is in the recognized family (is_fp_binary_v / std::is_floating_point_v /
//               std::is_array_v / std::is_integral_v). Adding a cfg field of an
//               unrecognized type FAILS THE BUILD with actionable error message.
//
// H13 (type-trait dispatch via templated helpers) — public statement.
// Structural-fix-preferred gradient — meta-pattern motivation.
//======================================================================================================
#pragma once
#include "CfgFieldRegistry.hpp"
#include "../FixedPoint/FixedPointN.hpp"   // FPN_Binary<F>, is_fp_binary_v, FPN_FromDouble, FPN_ToDouble
#include <cstdlib>     // strtoull, atoi, atof
#include <cstring>     // strncpy
#include <cstdio>      // snprintf
#include <type_traits> // std::is_floating_point_v, std::is_integral_v, std::is_array_v, std::is_unsigned_v
#include <algorithm>   // std::clamp
#include <cmath>       // llround (Ship-B P2a: exact registry-default/clamp -> Money conversion)
#include <locale.h>    // newlocale, uselocale, freelocale (Layer 2 locale pinning)
#include <strings.h>   // v5.15.5.F.4c — strcasecmp for KIND_INT_ENUM string-token parsing

namespace tt {

    // Forward-declared parse_double_fast (definition lives in
    // ML_Headers/ModelInference.hpp's namespace tt; same precedent as
    // StampBoundModelConstRegistry.hpp's forward-decl-to-avoid-include-cycle).
    inline double parse_double_fast(const char* s);
    // ③ C1/C2 (D-261) — checked variant for FEATURE-field malformed-capture (definition in CoreFrameworks/ParseFast.hpp).
    inline double parse_double_fast_checked(const char* s, bool* malformed_out);

    // Dependent-false for exhaustive if-constexpr chains (Ship-B P0, S-5/V5).
    // A dispatcher chain's final `else { static_assert(always_false_v<T>, ...) }` turns an
    // unmatched type COMBINATION into a compile error instead of a silent fall-through —
    // the two-template drift dispatchers below previously compiled `return false`/`return 0`
    // for any pair outside their branch set, silently disabling stamp drift protection.
    template <typename...>
    inline constexpr bool always_false_v = false;

    //======================================================================
    // [FUNCTION]_[cfg_parse_field]
    //----------------------------------------------------------------------
    // [TAG]_[[ENGINE] [CFG_FLOW] [DETERMINISM]]
    // [REFERENCE]_[INVARIANT]_[[H13] [H14]]
    // [SCHEMA]_[v1.0]
    // [OVERVIEW]_[text -> typed cfg field; exact decimal Money parse (never via double), PCT scaling cfg-file-only (wire_context flag), malformed-money/feature fault capture, INT_ENUM label lookup]
    // [REFERENCE]_[DECISION]_[[D-102] [D-254] [D-256] [D-259] [D-261]]
    //======================================================================
    // [CODE]
    //======================================================================
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
                                 bool wire_context = false, uint32_t* fault_out = nullptr) {
        // BARRIER 3: compile-time type-family guard.
        // Adding a cfg field of an unrecognized type FAILS THE BUILD here, forcing
        // a deliberate decision (extend tt::cfg_parse_field<T> with a new branch)
        // rather than silent truncation via type-erased reinterpret_cast.
        static_assert(is_fp_binary_v<T>
                   || is_fp_decimal_v<T>
                   || std::is_floating_point_v<T>
                   || std::is_integral_v<T>
                   || std::is_array_v<T>,
                      "cfg field type not in recognized family — "
                      "extend tt::cfg_parse_field<T> with a new branch before "
                      "using this T as a cfg field. See "
                      "DESIGN_SPECS/type-trait-dispatch-via-tt-namespace.md.");

        if constexpr (is_fp_decimal_v<T>) {
            // Ship-B P2a (B3/S-15): EXACT decimal money parse — never via double (the D-102
            // class this ship exists to kill). PCT file convention: parse exact, then ÷100
            // decimally (ONE rounding total, S-18). Parse flags surface via stderr here; the
            // LIVE refuse-boot policy (D-174c) plumbs at the mode-aware parser layer in P2b.
            MoneyParse p = Money_FromString(val);
            if (!wire_context && desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT)
                p.value = money_scale_down_pow10(p.value, 2);
            if (p.flags) {
                fprintf(stderr, "[cfg] WARN: %s='%s' decimal parse flags=0x%x "
                        "(1=malformed 2=overflow 4=excess-dp); LIVE boot refuses these (D-174c).\n",
                        desc.cfg_field_name, val, p.flags);
            }
            // ③ D-256 / B1 (was D-254, bit-gated) — UNIT-AGNOSTIC malformed-refuse: ANY decimal-Money
            // cfg field that's MALFORMED/OVERFLOW is a HARD fault. It parsed to 0, indistinguishable
            // from a legit sentinel post-parse, so the post-resolve value-sweep can't catch it — catch
            // it HERE at the flag (the founding silent-disabled bug). DECOUPLED from CAPITAL_BOUND_*
            // (a malformed money value is always wrong, regardless of cap-bit) so the migrated capital
            // globals (meta=0) are covered too. EXCESS_DP (>8dp, half-even-rounded-but-VALID) stays a
            // WARN above, NOT a fault. wire_context stays corrupt-only (never faults on reload — B2).
            if (!wire_context && fault_out
                && (p.flags & (MONEY_PARSE_MALFORMED | MONEY_PARSE_OVERFLOW))) {
                *fault_out |= CFG_FAULT_CAPITAL_MALFORMED;
                fprintf(stderr, "[cfg] FATAL: money cfg field %s='%s' is %s -> boot REFUSED (D-256/B1). "
                        "Likely: locale comma (1,5->1.5), a trailing unit (1.5%%->1.5), a "
                        "letter-for-digit (0.o3), or an empty/placeholder value. A money field "
                        "cannot silently default.\n",
                        desc.cfg_field_name, val,
                        (p.flags & MONEY_PARSE_MALFORMED) ? "MALFORMED (not a number)"
                                                          : "OVERFLOW (too large; saturated)");
            }
            // Clamp parity with the binary branch (same numbers, same vacuousness for
            // percent-form rows) — converted ONCE to .v space; the VALUE never round-trips
            // through double. TOTAL conversion (saturating — a raw llround is UB past 2^63
            // and inverted a wide clamp to INT64_MIN during P2a testing).
            const __int128 clo = money_from_double_payload(desc.payload.as_double.clamp_min);
            const __int128 chi = money_from_double_payload(desc.payload.as_double.clamp_max);
            if (p.value.v < clo) p.value.v = clo;
            if (p.value.v > chi) p.value.v = chi;
            dst = p.value;
        } else if constexpr (is_fp_binary_v<T>) {
            // FPN_Binary<F>: parse double, apply percent scaling if KIND_DOUBLE_PCT (cfg-file context only),
            // clamp to descriptor range, convert to FPN_Binary<T::F>.
            // ③ C1/C2 (D-261) — FEATURE fields are determinism-bearing: a malformed value silently coercing
            // to 0 is a reproducibility hole. Route through the CHECKED parser → CFG_FAULT_FEATURE_MALFORMED
            // (DISTINCT bit per D-259, NEVER the capital bit — no Class-49). Refuse-don't-coerce (lean-refuse,
            // D-261). EMPTY = clean inherit/default; wire_context never faults (stamp-body reload, B2).
            bool malformed = false;
            double v = parse_double_fast_checked(val, &malformed);
            if (!wire_context && malformed && val[0] != '\0' && fault_out) *fault_out |= CFG_FAULT_FEATURE_MALFORMED;
            if (!wire_context && desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) {
                v /= 100.0;  // operator types "15.0" → stored as 0.15 (cfg-FILE convention)
            }
            v = std::clamp(v, desc.payload.as_double.clamp_min, desc.payload.as_double.clamp_max);
            dst = FPN_FromDouble<T::F>(v);
        } else if constexpr (std::is_floating_point_v<T>) {
            // ③ C1/C2 (D-261) — same FEATURE-malformed capture for the legacy float branch.
            bool malformed = false;
            double v = parse_double_fast_checked(val, &malformed);
            if (!wire_context && malformed && val[0] != '\0' && fault_out) *fault_out |= CFG_FAULT_FEATURE_MALFORMED;
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
    //======================================================================
    // [END_CODE]
    //======================================================================
    // [END_FUNCTION]_[cfg_parse_field]
    //======================================================================

    //======================================================================
    // [FUNCTION]_[cfg_save_field]
    //----------------------------------------------------------------------
    // [TAG]_[[ENGINE] [CFG_FLOW]]
    // [REFERENCE]_[INVARIANT]_[[H13] [H9]]
    // [SCHEMA]_[v1.0]
    // [OVERVIEW]_[typed -> operator-readable cfg-file text — exact decimal Money (never %.Nf), PCT x100 form, LC_NUMERIC=C pinned]
    //======================================================================
    // [CODE]
    //======================================================================
    // Returns chars written (snprintf semantics). Caller routes the buffer
    // through GUI/SettingsPanel.hpp:cfg_write_field(path, key, buf) for
    // comment-preserving save UX.
    //
    // LAYER 2 LOCALE PINNING per DESIGN_SPECS/wire-format-byte-preservation-discipline.md.
    // Status-quo pre-.F.4b SettingsPanel save used raw snprintf which honored
    // LC_NUMERIC; a stamp emitted under de_DE produces "0,55" not "0.55" → HMAC chain
    // breaks. tt:: dispatch pins per-thread to LC_NUMERIC=C; restores before return.
    // Precedent: MemHeaders/RunHistory.hpp's locale-pin block.
    template <typename T>
    inline int cfg_save_field(const T& src, const CfgFieldDescriptor& desc, char* buf, size_t cap) {
        static_assert(is_fp_binary_v<T>
                   || is_fp_decimal_v<T>
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
        if constexpr (is_fp_decimal_v<T>) {
            // Ship-B P2a (S-15): EXACT decimal save — never %.4f/%.2f (value-mutating for money
            // rates: 0.00075 -> "0.07" -> reload 0.0007) and never %.17g (doesn't round-trip
            // 10^8-scaled ints past 2^53). PCT saves the percent form ×100 EXACTLY in .v
            // (|v|·100 < 2^70 — no overflow possible from the closure domain).
            Money out = src;
            if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) out.v *= 100;
            char tmp[40];
            Money_ToCString(out, tmp, (int)sizeof(tmp));
            // Cfg-file presentation trim: drop trailing zeros past 2 decimals ("2.34000000"
            // -> "2.34", "3.00000000" -> "3.00"). EXACTNESS preserved (only zero digits
            // removed; reload parses identically). The WIRE emit branch stays full-raw.
            char* dot = strchr(tmp, '.');
            if (dot) {
                char* end = tmp + strlen(tmp) - 1;
                while (end > dot + 2 && *end == '0') *end-- = '\0';
            }
            n = snprintf(buf, cap, "%s", tmp);
        } else if constexpr (is_fp_binary_v<T>) {
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
    //======================================================================
    // [END_CODE]
    //======================================================================
    // [END_FUNCTION]_[cfg_save_field]
    //======================================================================

    //======================================================================
    // [FUNCTION]_[cfg_assign_field]
    //----------------------------------------------------------------------
    // [TAG]_[[ENGINE] [CFG_FLOW]]
    // [REFERENCE]_[INVARIANT]_[H13]
    // [SCHEMA]_[v1.0]
    // [OVERVIEW]_[descriptor default -> typed cfg field (reset-to-defaults + boot default-fill); PCT payload is PERCENT-space, scaled exactly like the file parser]
    // [REFERENCE]_[PARITY]_[PARITY-37]
    //======================================================================
    // [CODE]
    //======================================================================
    // v5.15.5.F.4c — third sister of cfg_parse_field / cfg_save_field. Sets cfg.name
    // to its declared default value from the descriptor payload. Used by reset-to-
    // defaults GUI button + boot default-fill consumers. Same 3-barrier discipline.
    template <typename T>
    inline void cfg_assign_field(T& dst, const CfgFieldDescriptor& desc) {
        static_assert(is_fp_binary_v<T>
                   || is_fp_decimal_v<T>
                   || std::is_floating_point_v<T>
                   || std::is_integral_v<T>
                   || std::is_array_v<T>,
                      "cfg field type not in recognized family — "
                      "extend tt::cfg_assign_field<T> with a new branch.");

        if constexpr (is_fp_decimal_v<T>) {
            // Ship-B P2a: PERCENT-space payload (P0.3 convention) -> exact fraction. Registry
            // default literals are <=8dp => llround(d*1e8) is exact; no double VALUE round-trip.
            double d = desc.payload.as_double.default_val;
            if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) d /= 100.0;
            dst = Money{ money_from_double_payload(d) };
        } else if constexpr (is_fp_binary_v<T>) {
            // Ship-B P0.3 (PARITY-037/V12 inverted): the DBL payload column is PERCENT-space for
            // KIND_DOUBLE_PCT rows — matching the GUI ×100 display, the cfg-FILE convention
            // (cfg_parse_field ÷100), and the 0-100 clamp ranges. Scale exactly as the file
            // parser does. (The previous "default is stored as fraction" comment was wrong vs
            // the row data; it silently made reset-to-default / boot-fill 100× too large for
            // every PCT field once its manual init is swept.)
            double v = desc.payload.as_double.default_val;
            if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) v /= 100.0;
            dst = FPN_FromDouble<T::F>(v);
        } else if constexpr (std::is_floating_point_v<T>) {
            double v = desc.payload.as_double.default_val;
            if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) v /= 100.0;
            dst = static_cast<T>(v);
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
    //======================================================================
    // [END_CODE]
    //======================================================================
    // [END_FUNCTION]_[cfg_assign_field]
    //======================================================================

    //======================================================================
    // [FUNCTION]_[cfg_diff_field]
    //----------------------------------------------------------------------
    // [TAG]_[[ENGINE] [CFG_FLOW]]
    // [REFERENCE]_[INVARIANT]_[H13]
    // [SCHEMA]_[v1.0]
    // [OVERVIEW]_[current vs descriptor default -> bool (GUI modified badge + --changed-only); exact .v compare for Money, PERCENT-space scaled like assign]
    //======================================================================
    // [CODE]
    //======================================================================
    // v5.15.5.F.4c — fourth sister. Returns true if current value differs from declared
    // default. Used by GUI "modified" badge + CLI --list-cfg --changed-only consumer.
    template <typename T>
    inline bool cfg_diff_field(const T& current, const CfgFieldDescriptor& desc) {
        static_assert(is_fp_binary_v<T>
                   || is_fp_decimal_v<T>
                   || std::is_floating_point_v<T>
                   || std::is_integral_v<T>
                   || std::is_array_v<T>,
                      "cfg field type not in recognized family — "
                      "extend tt::cfg_diff_field<T> with a new branch.");

        if constexpr (is_fp_decimal_v<T>) {
            // Ship-B P2a: exact .v compare against the percent-converted default (sister to
            // cfg_assign_field's conversion — same llround path, byte-deterministic).
            double d = desc.payload.as_double.default_val;
            if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) d /= 100.0;
            return current.v != money_from_double_payload(d);
        } else if constexpr (is_fp_binary_v<T>) {
            // FPN_Binary equality via integer comparison (after FromDouble conversion).
            // PCT payload is PERCENT-space — scale before compare (Ship-B P0.3, sister to
            // cfg_assign_field; without it the GUI "modified" badge is wrong for every PCT field).
            double dv = desc.payload.as_double.default_val;
            if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) dv /= 100.0;
            const T default_fpn = FPN_FromDouble<T::F>(dv);
            return !(current == default_fpn);
        } else if constexpr (std::is_floating_point_v<T>) {
            double dv = desc.payload.as_double.default_val;
            if (desc.kind == CfgFieldDescriptor::KIND_DOUBLE_PCT) dv /= 100.0;
            return current != static_cast<T>(dv);
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
    //======================================================================
    // [END_CODE]
    //======================================================================
    // [END_FUNCTION]_[cfg_diff_field]
    //======================================================================

    //======================================================================
    // [FUNCTION]_[cfg_emit_field]
    //----------------------------------------------------------------------
    // [TAG]_[[ENGINE] [CFG_FLOW] [PERSISTENCE] [DETERMINISM]]
    // [REFERENCE]_[INVARIANT]_[[H13] [H9]]
    // [SCHEMA]_[v1.0]
    // [OVERVIEW]_[typed -> HMAC wire-format "name=value\n" — exact decimal Money string on the wire, %.17g lossless binary, LC_NUMERIC=C pinned]
    //======================================================================
    // [CODE]
    //======================================================================
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
    //   FPN_Binary<F>:        %.17g (lossless double round-trip; sister to MemHeaders/RunHistory.hpp:87-89)
    //   integers:      %lld / %llu (signed/unsigned via is_unsigned_v)
    //   bool:          ternary normalized to {0, 1} (H9 byte-equivalence; see cfg_save_field above)
    //   array<T, N>:   %s (string-style; KIND_FILE_PATH / KIND_STRING)
    //
    // Returns chars written (snprintf semantics; 0 on encoding error).
    template <typename T>
    inline size_t cfg_emit_field(const T& src, const CfgFieldDescriptor& desc, char* buf, size_t cap) {
        static_assert(is_fp_binary_v<T>
                   || is_fp_decimal_v<T>
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
        if constexpr (is_fp_decimal_v<T>) {
            // Ship-B P2a (B3 — THE wire branch this ship exists for): EXACT decimal string on
            // the HMAC body. Raw fraction (wire carries fractions; sister to the binary branch's
            // no-PCT-scale rule); pure digits => locale-immune on top of the pin.
            char tmp[40];
            Money_ToCString(src, tmp, (int)sizeof(tmp));
            n = snprintf(buf, cap, "%s=%s\n", desc.cfg_field_name, tmp);
        } else if constexpr (is_fp_binary_v<T>) {
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
    //======================================================================
    // [END_CODE]
    //======================================================================
    // [END_FUNCTION]_[cfg_emit_field]
    //======================================================================

    //======================================================================
    // [FUNCTION]_[cfg_populate_inf_field]
    //----------------------------------------------------------------------
    // [TAG]_[[ENGINE] [CFG_FLOW] [ML_INFERENCE]]
    // [REFERENCE]_[INVARIANT]_[[H13] [H20]]
    // [SCHEMA]_[v1.0]
    // [OVERVIEW]_[runtime cfg -> inf.field + inf.has_field, gate-aware (always-emit Q3.G: gate-off writes T{}+0); branchless cmov for scalars; SrcT/DstT/HasT independent]
    //======================================================================
    // [CODE]
    //======================================================================
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
    // discovery at Step 1 build verify. Cohort cfg fields are FPN_Binary<F> in master registry but
    // inf struct fields are double (per the FOREACH_STAMP_BOUND_CFG legacy struct-gen
    // `type name;` emission in ML_Headers/ModelInference.hpp, where type=double for FPN_Binary
    // cohort rows + has_ field is uint8_t for StampInferenceCfgInputs OR int for
    // ModelStampResult). Framework
    // walker needs to convert SrcT→DstT (FPN_ToDouble for FPN_Binary→double) + accept any integral
    // HasT. Sister precedent: cfg_emit_field already handles FPN_Binary→double via FPN_ToDouble inline.
    template <typename SrcT, typename DstT, typename HasT>
    inline void cfg_populate_inf_field(const SrcT& cfg_src, DstT& inf_dst, HasT& inf_has_dst, bool gate) {
        static_assert(is_fp_binary_v<SrcT>
                   || is_fp_decimal_v<SrcT>
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
        } else if constexpr (is_fp_binary_v<SrcT> && std::is_floating_point_v<DstT>) {
            // FPN_Binary<F> → double via FPN_ToDouble (cohort field common case; sister to
            // cfg_emit_field's FPN_Binary→double %.17g branch above).
            inf_dst = gate ? FPN_ToDouble(cfg_src) : DstT{};
            inf_has_dst = gate ? static_cast<HasT>(1) : static_cast<HasT>(0);
        } else if constexpr (is_fp_binary_v<SrcT> && is_fp_binary_v<DstT>) {
            // FPN_Binary<F> → FPN_Binary<F> direct (no conversion).
            inf_dst = gate ? cfg_src : DstT{};
            inf_has_dst = gate ? static_cast<HasT>(1) : static_cast<HasT>(0);
        } else if constexpr (is_fp_decimal_v<SrcT> && std::is_floating_point_v<DstT>) {
            // Ship-B P2a BRIDGE: Money → double inf field (the v2-era stamp records doubles).
            // Display/compare semantics only — the WIRE body emits the exact string via
            // cfg_emit_field's decimal branch; the exact-typed inf field lands with stamp v3
            // (P2b). Sound to 16 significant digits (money magnitudes sit far inside).
            inf_dst = gate ? Money_ToDouble(cfg_src) : DstT{};
            inf_has_dst = gate ? static_cast<HasT>(1) : static_cast<HasT>(0);
        } else if constexpr (is_fp_decimal_v<SrcT> && is_fp_decimal_v<DstT>) {
            inf_dst = gate ? cfg_src : DstT{};
            inf_has_dst = gate ? static_cast<HasT>(1) : static_cast<HasT>(0);
        } else {
            // Integral / floating: implicit conversion via static_cast (branchless cmov).
            inf_dst = gate ? static_cast<DstT>(cfg_src) : DstT{};
            inf_has_dst = gate ? static_cast<HasT>(1) : static_cast<HasT>(0);
        }
    }
    //======================================================================
    // [END_CODE]
    //======================================================================
    // [END_FUNCTION]_[cfg_populate_inf_field]
    //======================================================================

    //======================================================================
    // [FUNCTION]_[cfg_drift_compare]
    //----------------------------------------------------------------------
    // [TAG]_[[ENGINE] [CFG_FLOW] [ML_INFERENCE] [DETERMINISM]]
    // [REFERENCE]_[INVARIANT]_[[H13] [H4] [H12]]
    // [SCHEMA]_[v1.0]
    // [OVERVIEW]_[train-time stamp value vs serve-time cfg -> drift bool; exact .v / byte compares (never float math on money); exhaustive-else kills silent no-drift fall-through]
    //======================================================================
    // [CODE]
    //======================================================================
    // v5.15.5.F.4d.1.B.1 — seventh in-file sister. Used by DRIFT_CHECK_FROM_DERIVED to detect
    // cfg drift between train-time stamp value and serve-time runtime cfg value.
    //
    // Distinct from cfg_diff_field (which compares vs descriptor default).
    //   cfg_diff_field — compare current vs default → bool (used for GUI "modified" badge)
    //   cfg_drift_compare — compare stamp vs runtime cfg → bool (used for drift detection)
    //
    // Returns true if values differ (drift detected). Per H4 — FPN_Binary<F> compared via integer
    // equality (NEVER float math on accounting types).
    //
    // v5.15.5.F.4d.1.B.2 — extended StampT/CfgT independent templates per coding-time
    // discovery at Step 1 build verify. Stamp's recorded value is at the inf struct's type
    // (double for FPN_Binary cohort rows per ModelInference.hpp's ModelStampResult struct-gen
    // `int has_##name; type name;` where type=double); cfg runtime value is FPN_Binary<F>.
    // Compare in DstT space (the stamp's
    // recorded type — what was actually wire-emitted at training time).
    template <typename StampT, typename CfgT>
    inline bool cfg_drift_compare(const StampT& stamp_val, const CfgT& cfg_val) {
        static_assert(is_fp_binary_v<StampT>
                   || is_fp_decimal_v<StampT>
                   || std::is_floating_point_v<StampT>
                   || std::is_integral_v<StampT>
                   || std::is_array_v<StampT>,
                      "stamp field type not in recognized family — "
                      "extend tt::cfg_drift_compare<StampT,CfgT> with a new branch.");
        // Ship-B P0 (S-5/V5): CfgT was previously UNASSERTED — cfg_drift_compare<double, Money>
        // compiled and fell through to `return false` (silent no-drift on the HMAC-bound stamp
        // surface). Both params now family-asserted; the chain below is exhaustive.
        static_assert(is_fp_binary_v<CfgT>
                   || is_fp_decimal_v<CfgT>
                   || std::is_floating_point_v<CfgT>
                   || std::is_integral_v<CfgT>
                   || std::is_array_v<CfgT>,
                      "cfg field type not in recognized family — "
                      "extend tt::cfg_drift_compare<StampT,CfgT> with a new branch.");

        if constexpr (std::is_floating_point_v<StampT> && is_fp_decimal_v<CfgT>) {
            // Ship-B P2a BRIDGE (v2-era stamps record doubles): compare in double space via the
            // SAME conversion the populate path uses — equal values round-trip identically, so
            // the compare is sound to 16 significant digits. The exact-typed compare lands with
            // stamp v3 (P2b), which retires this branch.
            return stamp_val != Money_ToDouble(cfg_val);
        } else if constexpr (is_fp_decimal_v<StampT> && is_fp_decimal_v<CfgT>) {
            return stamp_val.v != cfg_val.v;                 // exact scaled-int equality
        } else if constexpr (std::is_floating_point_v<StampT> && is_fp_binary_v<CfgT>) {
            // stamp is double (legacy struct-gen) vs cfg is FPN_Binary<F>. Compare in double space
            // (what was wire-emitted at training time per FPN_ToDouble in cfg_emit_field).
            return stamp_val != FPN_ToDouble(cfg_val);
        } else if constexpr (is_fp_binary_v<StampT> && is_fp_binary_v<CfgT>) {
            // FPN_Binary equality via byte comparison (H4 — never float math on accounting types).
            // memcmp on POD struct compares the underlying integer limb array bit-exactly;
            // FPN_Binary<F> doesn't define operator== directly so we go through the byte layer.
            // .E.0.1 F-076 guard: this raw memcmp is byte-deterministic ONLY if StampT has no
            // padding. FPN_Binary<F> is padding-free today (probed); this static_assert FAILS the build
            // if a future layout change adds padding (which would make this memcmp false-drift).
            // Sister to the ControllerConfig zero-init ctor (same H12 byte-equivalence class).
            static_assert(std::has_unique_object_representations_v<StampT>,
                          "F-076/H12: StampT (FPN_Binary) gained padding -> raw memcmp would false-drift; "
                          "make it padding-free or compare field-wise.");
            return memcmp(&stamp_val, &cfg_val, sizeof(StampT)) != 0;
        } else if constexpr (std::is_floating_point_v<StampT> && std::is_floating_point_v<CfgT>) {
            return stamp_val != cfg_val;
        } else if constexpr (std::is_array_v<StampT> && std::is_array_v<CfgT>) {
            return strncmp(stamp_val, cfg_val, std::extent_v<StampT>) != 0;
        } else if constexpr (std::is_integral_v<StampT> && std::is_integral_v<CfgT>) {
            return static_cast<int64_t>(stamp_val) != static_cast<int64_t>(cfg_val);
        } else {
            // Exhaustive-else (S-5/V5): an in-family but UNHANDLED (StampT, CfgT) pairing is a
            // compile error, never a silent "no drift". Decimal money gets its branch at P2.
            static_assert(always_false_v<StampT, CfgT>,
                          "cfg_drift_compare: unhandled (StampT, CfgT) combination — "
                          "add an explicit branch (silent fall-through = disabled drift protection).");
        }
    }
    //======================================================================
    // [END_CODE]
    //======================================================================
    // [END_FUNCTION]_[cfg_drift_compare]
    //======================================================================

    //======================================================================
    // [FUNCTION]_[cfg_drift_format_reason]
    //----------------------------------------------------------------------
    // [TAG]_[[ENGINE] [CFG_FLOW] [ML_INFERENCE]]
    // [REFERENCE]_[INVARIANT]_[H13]
    // [REFERENCE]_[DESIGN_SPEC]_[failure-attribution-buffer-pattern]
    // [SCHEMA]_[v1.0]
    // [OVERVIEW]_[first-drift attribution — "<field> drift: stamp=X cfg=Y" into a caller-allocated first-failure-wins buffer; boot/load-time only]
    //======================================================================
    // [CODE]
    //======================================================================
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
        static_assert(is_fp_binary_v<StampT>
                   || is_fp_decimal_v<StampT>
                   || std::is_floating_point_v<StampT>
                   || std::is_integral_v<StampT>
                   || std::is_array_v<StampT>,
                      "stamp field type not in recognized family — "
                      "extend tt::cfg_drift_format_reason<StampT,CfgT> with a new branch.");
        // Ship-B P0 (S-5/V5): CfgT family-asserted + exhaustive chain, sister to cfg_drift_compare.
        static_assert(is_fp_binary_v<CfgT>
                   || is_fp_decimal_v<CfgT>
                   || std::is_floating_point_v<CfgT>
                   || std::is_integral_v<CfgT>
                   || std::is_array_v<CfgT>,
                      "cfg field type not in recognized family — "
                      "extend tt::cfg_drift_format_reason<StampT,CfgT> with a new branch.");

        if (!buf || cap == 0) return 0;

        if constexpr (std::is_floating_point_v<StampT> && is_fp_decimal_v<CfgT>) {
            // Ship-B P2a bridge attribution (sister to cfg_drift_compare's decimal bridge).
            return snprintf(buf, cap, "%s drift: stamp=%g cfg=%g",
                            field_name, (double)stamp_val, Money_ToDouble(cfg_val));
        } else if constexpr (is_fp_decimal_v<StampT> && is_fp_decimal_v<CfgT>) {
            char st[40], ct[40];
            Money_ToCString(stamp_val, st, (int)sizeof(st));
            Money_ToCString(cfg_val, ct, (int)sizeof(ct));
            return snprintf(buf, cap, "%s drift: stamp=%s cfg=%s", field_name, st, ct);
        } else if constexpr (std::is_floating_point_v<StampT> && is_fp_binary_v<CfgT>) {
            // Stamp recorded as double (legacy struct-gen); cfg runtime is FPN_Binary<F>.
            // Format in double-space (matches wire-emit semantic per cfg_emit_field).
            return snprintf(buf, cap, "%s drift: stamp=%g cfg=%g",
                            field_name, (double)stamp_val, FPN_ToDouble(cfg_val));
        } else if constexpr (is_fp_binary_v<StampT> && is_fp_binary_v<CfgT>) {
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
        } else {
            // Exhaustive-else (S-5/V5): unhandled pairing = compile error, never a silent
            // zero-length attribution. Decimal money gets its branch at P2.
            static_assert(always_false_v<StampT, CfgT>,
                          "cfg_drift_format_reason: unhandled (StampT, CfgT) combination — "
                          "add an explicit branch (sister to cfg_drift_compare).");
        }
    }
    //======================================================================
    // [END_CODE]
    //======================================================================
    // [END_FUNCTION]_[cfg_drift_format_reason]
    //======================================================================

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
